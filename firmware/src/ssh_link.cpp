#include "ssh_link.h"

#include "board.h"

const char* GATEWAY_HOST = "127.0.0.1";
const int GATEWAY_PORT = 24543;

namespace {
const long CONNECT_TIMEOUT_S = 10;
}

ssh_key loadOrCreateDeviceKey() {
  ssh_key key = nullptr;
  String b64 = settings::deviceKey();
  if (b64.length() && ssh_pki_import_privkey_base64(b64.c_str(), nullptr, nullptr, nullptr, &key) == SSH_OK)
    return key;

  Serial.println("Generating ed25519 device key");
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) return nullptr;
  char* priv = nullptr;
  if (ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &priv) != SSH_OK) {
    ssh_key_free(key);
    return nullptr;
  }
  settings::saveDeviceKey(priv);
  ssh_string_free_char(priv);
  return key;
}

String keyFingerprint(ssh_key key) {
  unsigned char* hash = nullptr;
  size_t len = 0;
  if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &len) != SSH_OK) return String();
  char* fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, len);
  ssh_clean_pubkey_hash(&hash);
  String out = fp ? fp : "";
  ssh_string_free_char(fp);
  return out;
}

String authorizedKeysLine(ssh_key key) {
  char* pub = nullptr;
  if (ssh_pki_export_pubkey_base64(key, &pub) != SSH_OK) return String();
  char line[256];
  snprintf(line, sizeof(line), "restrict,port-forwarding,permitopen=\"%s:%d\",command=\"false\" ssh-ed25519 %s rhino-" BOARD_SLUG,
           GATEWAY_HOST, GATEWAY_PORT, pub);
  ssh_string_free_char(pub);
  return line;
}

bool SshLink::readFingerprint() {
  ssh_key srv = nullptr;
  if (ssh_get_server_publickey(session_, &srv) != SSH_OK) return false;
  unsigned char* hash = nullptr;
  size_t len = 0;
  int rc = ssh_get_publickey_hash(srv, SSH_PUBLICKEY_HASH_SHA256, &hash, &len);
  ssh_key_free(srv);
  if (rc != SSH_OK) return false;
  char* fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, len);
  ssh_clean_pubkey_hash(&hash);
  fingerprint_ = fp;
  ssh_string_free_char(fp);
  return true;
}

SshResult SshLink::connect(const HostConfig& host, ssh_key key) {
  close();
  error_ = "";
  fingerprint_ = "";
  session_ = ssh_new();
  if (!session_) {
    error_ = "out of memory";
    return SshResult::Unreachable;
  }
  unsigned int port = host.port;
  long timeout = CONNECT_TIMEOUT_S;
  ssh_options_set(session_, SSH_OPTIONS_HOST, host.address.c_str());
  ssh_options_set(session_, SSH_OPTIONS_PORT, &port);
  ssh_options_set(session_, SSH_OPTIONS_USER, host.user.c_str());
  ssh_options_set(session_, SSH_OPTIONS_HOSTKEYS, "ssh-ed25519");
  ssh_options_set(session_, SSH_OPTIONS_TIMEOUT, &timeout);

  if (ssh_connect(session_) != SSH_OK) {
    error_ = ssh_get_error(session_);
    close();
    return SshResult::Unreachable;
  }
  if (!readFingerprint()) {
    error_ = "could not read the host key";
    close();
    return SshResult::Unreachable;
  }
  String pinned = settings::pinnedHostKey();
  if (pinned.isEmpty()) {
    close();
    return SshResult::HostKeyUnknown;
  }
  if (pinned != fingerprint_) {
    close();
    return SshResult::HostKeyChanged;
  }
  if (ssh_userauth_publickey(session_, nullptr, key) != SSH_AUTH_SUCCESS) {
    error_ = ssh_get_error(session_);
    close();
    return SshResult::AuthRefused;
  }
  return SshResult::Ok;
}

void SshLink::close() {
  if (!session_) return;
  ssh_disconnect(session_);
  ssh_free(session_);
  session_ = nullptr;
}

ssh_channel SshLink::openGateway() {
  if (!session_) {
    error_ = "not connected";
    return nullptr;
  }
  ssh_channel ch = ssh_channel_new(session_);
  if (!ch) {
    error_ = "out of memory";
    return nullptr;
  }
  if (ssh_channel_open_forward(ch, GATEWAY_HOST, GATEWAY_PORT, "127.0.0.1", 0) != SSH_OK) {
    error_ = ssh_get_error(session_);
    ssh_channel_free(ch);
    return nullptr;
  }
  return ch;
}

void closeChannel(ssh_channel ch) {
  if (!ch) return;
  if (ssh_channel_is_open(ch)) {
    ssh_channel_send_eof(ch);
    ssh_channel_close(ch);
  }
  ssh_channel_free(ch);
}

int gatewayRequest(SshLink& link, const char* method, const char* path, const String& body, String& response,
                   size_t maxResponse) {
  response = "";
  ssh_channel ch = link.openGateway();
  if (!ch) {
    Serial.printf("[ssh] open forward failed: %s\n", link.error().c_str());
    return -1;
  }

  String req = String(method) + " " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n";
  if (body.length()) req += "Content-Type: application/json\r\nContent-Length: " + String(body.length()) + "\r\n";
  req += "\r\n";
  // The body (a plan can be many KB) goes out as is, never copied into req.
  if (ssh_channel_write(ch, req.c_str(), req.length()) != (int)req.length() ||
      (body.length() && ssh_channel_write(ch, body.c_str(), body.length()) != (int)body.length())) {
    closeChannel(ch);
    return -1;
  }

  String raw;
  char buf[512];
  int n;
  while ((n = ssh_channel_read_timeout(ch, buf, sizeof(buf), 0, 10000)) > 0) {
    if (raw.length() + n > maxResponse + 1024) break;  // headers + capped body
    raw.concat(buf, n);
  }
  if (n < 0 || raw.isEmpty())
    Serial.printf("[ssh] read ended with %d after %u bytes, eof=%d\n", n, (unsigned)raw.length(),
                  ssh_channel_is_eof(ch));
  closeChannel(ch);

  // "HTTP/1.1 200 OK"
  int sp = raw.indexOf(' ');
  if (!raw.startsWith("HTTP/") || sp < 0) return -1;
  int status = raw.substring(sp + 1, sp + 4).toInt();
  int bodyAt = raw.indexOf("\r\n\r\n");
  if (bodyAt >= 0) response = raw.substring(bodyAt + 4);
  return status;
}

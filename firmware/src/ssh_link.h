#pragma once

#include <Arduino.h>
#include <libssh/libssh.h>

#include "settings.h"

// moshi-hook's gateway, as seen from the host: loopback only.
extern const char* GATEWAY_HOST;
extern const int GATEWAY_PORT;

/** The X4's ed25519 key, loaded from NVS or generated on first boot. */
ssh_key loadOrCreateDeviceKey();

/** A key's SHA256 fingerprint, "SHA256:...". */
String keyFingerprint(ssh_key key);

/** The restricted authorized_keys line the host needs for `key`. */
String authorizedKeysLine(ssh_key key);

enum class SshResult {
  Ok,
  Unreachable,     // TCP or SSH handshake failed
  HostKeyUnknown,  // nothing pinned yet: fingerprint() needs confirming
  HostKeyChanged,  // pinned key differs: refuse
  AuthRefused,     // the host does not accept the X4's key
};

/** One SSH session to one host, carrying direct-tcpip channels to the gateway. */
class SshLink {
 public:
  SshResult connect(const HostConfig& host, ssh_key key);
  void close();
  bool isOpen() const { return session_ != nullptr; }

  // A new channel to the gateway, or nullptr (see error()).
  ssh_channel openGateway();

  const String& error() const { return error_; }
  const String& fingerprint() const { return fingerprint_; }  // host key, "SHA256:..."

 private:
  bool readFingerprint();

  ssh_session session_ = nullptr;
  String error_;
  String fingerprint_;
};

/** Closes and frees a channel; tolerates nullptr. */
void closeChannel(ssh_channel ch);

/**
 * One HTTP/1.0 request over a fresh gateway channel. Returns the status code,
 * or -1 if the channel or exchange failed. `body` is sent as JSON when not
 * empty. The response body is capped at `maxResponse` bytes.
 */
int gatewayRequest(SshLink& link, const char* method, const char* path, const String& body, String& response,
                   size_t maxResponse = 8192);

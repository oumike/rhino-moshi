// SSH feasibility spike for the X4.
//
// Answers one question: can the ESP32-C3 hold an SSH session, tunnel to
// moshi-hook's loopback gateway, and parse /events frames while the e-paper
// frame buffer is also resident? Logs free heap at every step over serial.
//
// First boot generates an ed25519 key and starts setup mode: an open access
// point (no captive redirect) serving the settings page
// at http://192.168.4.1. Afterwards the page is at http://rhino-x4.local. Hold
// Back while booting to return to setup mode. The host key is trusted on
// first use and pinned in NVS.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <WiFi.h>

#include "libssh_esp32.h"
#include <libssh/libssh.h>

#include "board.h"
#include "buttons.h"
#include "config_server.h"
#include "settings.h"

// libssh needs far more than the Arduino loop task's default 8 KB of stack;
// this matches LibSSH-ESP32's examples. The high-water mark is logged below so
// it can be trimmed.
SET_LOOP_TASK_STACK_SIZE(50 * 1024);

using EpdDisplay = GxEPD2_BW<GxEPD2_426_GDEQ0426T82, GxEPD2_426_GDEQ0426T82::HEIGHT>;
EpdDisplay display(GxEPD2_426_GDEQ0426T82(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

static const char* GATEWAY_HOST = "127.0.0.1";
static const int GATEWAY_PORT = 24543;

static void heap(const char* step) {
  Serial.printf("[heap] %-28s free=%6u  min=%6u  largest=%6u  stack_left=%5u\n", step, ESP.getFreeHeap(),
                ESP.getMinFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                uxTaskGetStackHighWaterMark(nullptr));
}

static void show(const String& text) {
  display.setRotation(settings::rotation());
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(2);
    display.setCursor(20, 30);
    display.print(text);
  } while (display.nextPage());
}

static void fail(const char* what) {
  Serial.printf("FAIL: %s\n", what);
  heap("at failure");
  show(String("Spike failed:\n ") + what + "\n\n Settings:\n http://" + config_server::HOSTNAME + ".local");
  while (true) {
    config_server::handle();
    delay(2);
  }
}

static ssh_key loadOrCreateKey() {
  ssh_key key = nullptr;
  String b64 = settings::deviceKey();
  if (b64.length() && ssh_pki_import_privkey_base64(b64.c_str(), nullptr, nullptr, nullptr, &key) == SSH_OK)
    return key;

  Serial.println("Generating ed25519 key...");
  if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) fail("keygen");
  char* priv = nullptr;
  if (ssh_pki_export_privkey_base64(key, nullptr, nullptr, nullptr, &priv) != SSH_OK) fail("export key");
  settings::saveDeviceKey(priv);
  ssh_string_free_char(priv);
  return key;
}

static String authorizedKeysLine(ssh_key key) {
  char* pub = nullptr;
  if (ssh_pki_export_pubkey_base64(key, &pub) != SSH_OK) fail("export pubkey");
  char line[256];
  snprintf(line, sizeof(line),
           "restrict,port-forwarding,permitopen=\"%s:%d\",command=\"false\" ssh-ed25519 %s rhino-x4", GATEWAY_HOST,
           GATEWAY_PORT, pub);
  ssh_string_free_char(pub);
  return line;
}

static void onSetupMode(const char* ssid, const char* url) {
  show(String("Setup\n\n Join Wi-Fi:\n ") + ssid + "\n\n Then open:\n " + url);
}

static void onConnecting(const char* ssid) { show(String("Joining\n ") + ssid + "..."); }

// Trust on first use: pin the SHA256 fingerprint, refuse if it ever changes.
static void verifyHostKey(ssh_session s) {
  ssh_key srv = nullptr;
  if (ssh_get_server_publickey(s, &srv) != SSH_OK) fail("server key");
  unsigned char* hash = nullptr;
  size_t hlen = 0;
  if (ssh_get_publickey_hash(srv, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) != SSH_OK) fail("hash");
  char* fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
  ssh_clean_pubkey_hash(&hash);
  ssh_key_free(srv);

  String pinned = settings::pinnedHostKey();
  Serial.printf("Host key %s\n", fp);
  if (pinned.isEmpty()) {
    Serial.println("  (first use: pinned)");
    settings::pinHostKey(fp);
  } else if (pinned != fp) {
    Serial.printf("  pinned was %s\n", pinned.c_str());
    ssh_string_free_char(fp);
    fail("HOST KEY CHANGED");
  }
  ssh_string_free_char(fp);
}

static ssh_channel openTunnel(ssh_session s) {
  ssh_channel ch = ssh_channel_new(s);
  if (!ch) fail("channel new");
  if (ssh_channel_open_forward(ch, GATEWAY_HOST, GATEWAY_PORT, "127.0.0.1", 0) != SSH_OK) {
    Serial.println(ssh_get_error(s));
    fail("open direct-tcpip");
  }
  return ch;
}

static void writeAll(ssh_channel ch, const char* data, size_t len) {
  if (ssh_channel_write(ch, data, len) != (int)len) fail("channel write");
}

// Reads one HTTP/1.0 response to EOF and prints the body.
static void getVersion(ssh_session s) {
  ssh_channel ch = openTunnel(s);
  heap("tunnel open (version)");
  const char req[] = "GET /v1/version HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
  writeAll(ch, req, sizeof(req) - 1);
  String resp;
  char buf[256];
  int n;
  while ((n = ssh_channel_read_timeout(ch, buf, sizeof(buf), 0, 5000)) > 0) resp.concat(buf, n);
  ssh_channel_send_eof(ch);
  ssh_channel_close(ch);
  ssh_channel_free(ch);
  int body = resp.indexOf("\r\n\r\n");
  Serial.printf("/v1/version -> %s\n", body >= 0 ? resp.c_str() + body + 4 : resp.c_str());
}

// Minimal WebSocket client over the channel: upgrade, send one masked text
// frame, then read unfragmented frames and parse them with a filter.
static bool readExact(ssh_channel ch, uint8_t* p, size_t len) {
  while (len) {
    int n = ssh_channel_read_timeout(ch, p, len, 0, 15000);
    if (n <= 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

static void sendText(ssh_channel ch, const String& text) {
  uint8_t hdr[8] = {0x81};
  size_t h = 2;
  size_t len = text.length();
  if (len < 126) {
    hdr[1] = 0x80 | len;
  } else {
    hdr[1] = 0x80 | 126;
    hdr[2] = len >> 8;
    hdr[3] = len & 0xff;
    h = 4;
  }
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  memcpy(hdr + h, mask, 4);
  writeAll(ch, (const char*)hdr, h + 4);
  String masked = text;
  for (size_t i = 0; i < len; i++) masked[i] = text[i] ^ mask[i % 4];
  writeAll(ch, masked.c_str(), len);
}

static void watchEvents(ssh_session s) {
  ssh_channel ch = openTunnel(s);
  heap("tunnel open (events)");
  const char req[] =
      "GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
  writeAll(ch, req, sizeof(req) - 1);

  String headers;
  char c;
  while (!headers.endsWith("\r\n\r\n")) {
    if (ssh_channel_read_timeout(ch, &c, 1, 0, 5000) != 1) fail("ws handshake");
    headers += c;
  }
  if (headers.indexOf(" 101 ") < 0) {
    Serial.println(headers);
    fail("ws upgrade refused");
  }
  sendText(ch, "{\"watch\":{\"workspaces\":true,\"usage\":true}}");

  JsonDocument filter;
  filter["workspaces"]["groups"][0]["label"] = true;
  JsonObject fc = filter["workspaces"]["groups"][0]["children"][0].to<JsonObject>();
  for (const char* k : {"id", "label", "agent", "agentStatus", "title", "sessionId", "statusChangedAt"}) fc[k] = true;
  filter["gateway"]["version"] = true;
  filter["watching"] = true;

  size_t maxFrame = 0;
  for (int frames = 0; frames < 8; frames++) {
    uint8_t h[2];
    if (!readExact(ch, h, 2)) break;
    uint64_t len = h[1] & 0x7f;
    if (len == 126) {
      uint8_t e[2];
      readExact(ch, e, 2);
      len = (e[0] << 8) | e[1];
    } else if (len == 127) {
      uint8_t e[8];
      readExact(ch, e, 8);
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
    }
    if (len > 32768) fail("frame too large for spike");
    char* payload = (char*)malloc(len + 1);
    if (!payload) fail("frame alloc");
    readExact(ch, (uint8_t*)payload, len);
    payload[len] = 0;
    maxFrame = max(maxFrame, (size_t)len);

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload, len, DeserializationOption::Filter(filter));
    Serial.printf("frame %d: opcode=%d %u bytes, parsed=%s, kept=%u bytes\n", frames, h[0] & 0x0f, (unsigned)len,
                  err.c_str(), (unsigned)measureJson(doc));
    heap("after frame parse");
    if (doc["workspaces"].is<JsonObject>()) {
      for (JsonObject g : doc["workspaces"]["groups"].as<JsonArray>())
        for (JsonObject t : g["children"].as<JsonArray>())
          Serial.printf("  %-16s %-8s %-8s %s\n", (const char*)(g["label"] | ""), (const char*)(t["agent"] | "-"),
                        (const char*)(t["agentStatus"] | ""), (const char*)(t["title"] | ""));
    }
    free(payload);
  }
  Serial.printf("largest frame: %u bytes\n", (unsigned)maxFrame);
  ssh_channel_close(ch);
  ssh_channel_free(ch);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n== Rhino X4 SSH spike ==");
  heap("boot");

  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  display.init(0);
  heap("display init (48KB buffer)");

  Buttons buttons;
  buttons.begin();
  bool forceSetup = false;
  for (int i = 0; i < 10; i++) {  // ~0.5 s window to catch Back held at boot
    buttons.update();
    forceSetup |= buttons.isPressed(BTN_BACK);
    delay(50);
  }

  libssh_begin();
  uint32_t t0 = millis();
  ssh_key key = loadOrCreateKey();
  Serial.printf("key ready in %u ms\n", millis() - t0);
  String akLine = authorizedKeysLine(key);
  Serial.printf("\nauthorized_keys line for the host:\n%s\n\n", akLine.c_str());
  heap("key loaded");

  config_server::Callbacks cb;
  cb.onSetupMode = onSetupMode;
  cb.onConnecting = onConnecting;
  config_server::begin(akLine, forceSetup, cb);
  Serial.printf("WiFi %s\n", WiFi.localIP().toString().c_str());
  heap("wifi up");

  HostConfig host = settings::loadHost();
  if (!host.isComplete()) fail("no host configured");
  Serial.printf("Host %s -> %s@%s:%u\n", host.name.c_str(), host.user.c_str(), host.address.c_str(), host.port);
  show("Connecting to " + host.name + "...");

  ssh_session s = ssh_new();
  ssh_options_set(s, SSH_OPTIONS_HOST, host.address.c_str());
  unsigned int port = host.port;
  ssh_options_set(s, SSH_OPTIONS_PORT, &port);
  ssh_options_set(s, SSH_OPTIONS_USER, host.user.c_str());
  ssh_options_set(s, SSH_OPTIONS_HOSTKEYS, "ssh-ed25519");
  t0 = millis();
  if (ssh_connect(s) != SSH_OK) {
    Serial.println(ssh_get_error(s));
    fail("connect");
  }
  Serial.printf("handshake in %u ms\n", millis() - t0);
  heap("ssh connected");
  verifyHostKey(s);

  t0 = millis();
  if (ssh_userauth_publickey(s, nullptr, key) != SSH_AUTH_SUCCESS) {
    Serial.println("Auth refused: add the authorized_keys line above to the host, then reset.");
    fail("auth");
  }
  Serial.printf("auth in %u ms\n", millis() - t0);
  heap("authenticated");

  getVersion(s);
  heap("after /v1/version");
  watchEvents(s);
  heap("after /events");

  ssh_disconnect(s);
  ssh_free(s);
  ssh_key_free(key);
  heap("disconnected");
  Serial.println("== spike done ==");
  show("Spike done: SSH tunnel to " + host.name + " works.\n\n See serial log for heap numbers.");
}

void loop() {
  config_server::handle();
  delay(2);
}

#include "settings.h"

#include <Preferences.h>

#include "board.h"

namespace {

const char* NS = "rhino";

// Preferences::getString logs an error for a missing key; absent is normal here.
String str(Preferences& p, const char* key) { return p.isKey(key) ? p.getString(key, "") : String(); }

}  // namespace

namespace settings {

WifiConfig loadWifi() {
  Preferences p;
  p.begin(NS, true);
  WifiConfig w;
  w.ssid = str(p, "w_ssid");
  w.password = str(p, "w_pass");
  p.end();
  return w;
}

void saveWifi(const WifiConfig& wifi) {
  Preferences p;
  p.begin(NS, false);
  p.putString("w_ssid", wifi.ssid);
  p.putString("w_pass", wifi.password);
  p.end();
}

uint8_t rotation() {
  Preferences p;
  p.begin(NS, true);
  uint8_t r = p.getUChar("rotation", DEFAULT_ROTATION);
  p.end();
  return r & 0x03;
}

void saveRotation(uint8_t rotation) {
  Preferences p;
  p.begin(NS, false);
  p.putUChar("rotation", rotation & 0x03);
  p.end();
}

String webPassword() {
  Preferences p;
  p.begin(NS, true);
  String pw = str(p, "web_pw");
  p.end();
  return pw;
}

void saveWebPassword(const String& password) {
  Preferences p;
  p.begin(NS, false);
  p.putString("web_pw", password);
  p.end();
}

HostConfig loadHost() {
  Preferences p;
  p.begin(NS, true);
  HostConfig h;
  h.name = str(p, "h_name");
  h.address = str(p, "h_addr");
  h.port = p.getUShort("h_port", 22);
  h.user = str(p, "h_user");
  p.end();
  return h;
}

void saveHost(const HostConfig& host) {
  HostConfig old = loadHost();
  Preferences p;
  p.begin(NS, false);
  p.putString("h_name", host.name);
  p.putString("h_addr", host.address);
  p.putUShort("h_port", host.port);
  p.putString("h_user", host.user);
  // A different machine has a different host key; keeping the old pin would
  // make the new host look like a man-in-the-middle.
  if (old.address != host.address || old.port != host.port) p.remove("h_fp");
  p.end();
}

String pinnedHostKey() {
  Preferences p;
  p.begin(NS, true);
  String fp = str(p, "h_fp");
  p.end();
  return fp;
}

void pinHostKey(const String& fingerprint) {
  Preferences p;
  p.begin(NS, false);
  p.putString("h_fp", fingerprint);
  p.end();
}

void forgetHostKey() {
  Preferences p;
  p.begin(NS, false);
  p.remove("h_fp");
  p.end();
}

const char* DEFAULT_PROMPTS =
    "Continue\n"
    "Yes\n"
    "No\n"
    "Run the tests and fix any failures\n"
    "Summarize what you've done so far\n"
    "Commit your changes";

String promptsText() {
  Preferences p;
  p.begin(NS, true);
  String s = p.isKey("prompts") ? p.getString("prompts", "") : String(DEFAULT_PROMPTS);
  p.end();
  return s;
}

std::vector<String> prompts() {
  std::vector<String> out;
  String s = promptsText() + "\n";
  int start = 0;
  while (start < (int)s.length() && out.size() < MAX_PROMPTS) {
    int nl = s.indexOf('\n', start);
    String line = s.substring(start, nl);
    line.trim();
    if (line.length()) out.push_back(line.substring(0, MAX_PROMPT_LEN));
    start = nl + 1;
  }
  return out;
}

void savePrompts(const String& lines) {
  String clean;
  String s = lines + "\n";
  int start = 0, n = 0;
  while (start < (int)s.length() && n < (int)MAX_PROMPTS) {
    int nl = s.indexOf('\n', start);
    String line = s.substring(start, nl);
    line.trim();  // also drops the \r of browser line endings
    if (line.length()) {
      if (n++) clean += '\n';
      clean += line.substring(0, MAX_PROMPT_LEN);
    }
    start = nl + 1;
  }
  Preferences p;
  p.begin(NS, false);
  p.putString("prompts", clean);
  p.end();
}

String deviceKey() {
  Preferences p;
  p.begin(NS, true);
  String k = str(p, "dev_key");
  p.end();
  return k;
}

void saveDeviceKey(const String& base64) {
  Preferences p;
  p.begin(NS, false);
  p.putString("dev_key", base64);
  p.end();
}

}  // namespace settings

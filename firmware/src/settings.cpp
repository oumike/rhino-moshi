#include "settings.h"

#include <Preferences.h>

#include "board.h"

namespace {

const char* NS = "rhino-moshi";

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

uint16_t screenTimeout() {
  Preferences p;
  p.begin(NS, true);
  uint16_t s = p.getUShort("screen_off", DEFAULT_SCREEN_TIMEOUT);
  p.end();
  return s;
}

String screenTimeoutName(uint16_t seconds) {
  if (!seconds) return "Never";
  if (seconds < 60) return String(seconds) + " seconds";
  return String(seconds / 60) + (seconds < 120 ? " minute" : " minutes");
}

void saveScreenTimeout(uint16_t seconds) {
  Preferences p;
  p.begin(NS, false);
  p.putUShort("screen_off", seconds);
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

// Hosts live in slots h0_* .. h3_*, counted by h_count. Firmware before that
// kept one host in h_name/h_addr/h_port/h_user/h_fp: read as slot 0 until the
// first save converts it.
std::vector<HostConfig> hosts() {
  Preferences p;
  p.begin(NS, true);
  std::vector<HostConfig> out;
  if (!p.isKey("h_count")) {
    HostConfig h;
    h.name = str(p, "h_name");
    h.address = str(p, "h_addr");
    h.port = p.getUShort("h_port", 22);
    h.user = str(p, "h_user");
    h.hostKey = str(p, "h_fp");
    if (h.address.length()) out.push_back(h);
  } else {
    int n = min((int)p.getUChar("h_count", 0), (int)MAX_HOSTS);
    for (int i = 0; i < n; i++) {
      String k = "h" + String(i) + "_";
      HostConfig h;
      h.name = str(p, (k + "name").c_str());
      h.address = str(p, (k + "addr").c_str());
      h.port = p.getUShort((k + "port").c_str(), 22);
      h.user = str(p, (k + "user").c_str());
      h.hostKey = str(p, (k + "fp").c_str());
      if (h.address.length()) out.push_back(h);
    }
  }
  p.end();
  return out;
}

void saveHosts(const std::vector<HostConfig>& list) {
  std::vector<HostConfig> keep;
  for (const HostConfig& h : list)
    if (h.address.length() && keep.size() < MAX_HOSTS) keep.push_back(h);
  Preferences p;
  p.begin(NS, false);
  for (int i = 0; i < (int)MAX_HOSTS; i++) {
    String k = "h" + String(i) + "_";
    if (i < (int)keep.size()) {
      const HostConfig& h = keep[i];
      p.putString((k + "name").c_str(), h.name);
      p.putString((k + "addr").c_str(), h.address);
      p.putUShort((k + "port").c_str(), h.port);
      p.putString((k + "user").c_str(), h.user);
      p.putString((k + "fp").c_str(), h.hostKey);
    } else {
      for (const char* f : {"name", "addr", "port", "user", "fp"})
        if (p.isKey((k + f).c_str())) p.remove((k + f).c_str());
    }
  }
  // Only once the slots are written, so a reset midway still finds the old host.
  p.putUChar("h_count", keep.size());
  for (const char* legacy : {"h_name", "h_addr", "h_port", "h_user", "h_fp"})
    if (p.isKey(legacy)) p.remove(legacy);
  if (p.getUChar("h_active", 0) >= keep.size()) p.putUChar("h_active", 0);
  p.end();
}

int activeHost() {
  Preferences p;
  p.begin(NS, true);
  int i = p.getUChar("h_active", 0);
  p.end();
  int n = hosts().size();
  return i < n ? i : 0;
}

void saveActiveHost(int index) {
  Preferences p;
  p.begin(NS, false);
  p.putUChar("h_active", max(0, index));
  p.end();
}

HostConfig loadHost() {
  std::vector<HostConfig> list = hosts();
  int i = activeHost();
  return i < (int)list.size() ? list[i] : HostConfig();
}

String pinnedHostKey() { return loadHost().hostKey; }

void pinHostKey(const String& fingerprint) {
  std::vector<HostConfig> list = hosts();
  int i = activeHost();
  if (i >= (int)list.size()) return;
  list[i].hostKey = fingerprint;
  saveHosts(list);
}

void forgetHostKey() { pinHostKey(""); }

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

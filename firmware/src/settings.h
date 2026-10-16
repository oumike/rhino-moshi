#pragma once

#include <Arduino.h>

#include <vector>

// One SSH host the X4 can connect to. Stored in NVS; edited on the settings page.
struct HostConfig {
  String name;
  String address;
  uint16_t port = 22;
  String user;
  String hostKey;  // pinned SHA256 fingerprint ("SHA256:..."), empty until first use

  bool isComplete() const { return address.length() && user.length() && port; }
};

struct WifiConfig {
  String ssid;
  String password;

  bool isComplete() const { return ssid.length() > 0; }
};

namespace settings {

WifiConfig loadWifi();
void saveWifi(const WifiConfig& wifi);

// Seconds without a key press before the backlight turns off; 0 is never.
const uint16_t DEFAULT_SCREEN_TIMEOUT = 30;
uint16_t screenTimeout();
// The choices offered, in order, and how each reads.
const uint16_t SCREEN_TIMEOUTS[] = {15, 30, 60, 120, 300, 600, 0};
String screenTimeoutName(uint16_t seconds);
void saveScreenTimeout(uint16_t seconds);

// Password for the settings web page. Empty means the page is open.
String webPassword();
void saveWebPassword(const String& password);

// The hosts, in order (those with an address), and which one to connect to.
const size_t MAX_HOSTS = 4;
std::vector<HostConfig> hosts();
// Stores the list as given, pins included.
void saveHosts(const std::vector<HostConfig>& hosts);
int activeHost();  // index into hosts(); 0 when there are none
void saveActiveHost(int index);

// The active host; empty if none.
HostConfig loadHost();

// The active host's pinned key.
String pinnedHostKey();
void pinHostKey(const String& fingerprint);
void forgetHostKey();

// Canned prompts for the Prompt menu, in order. Defaults until saved.
const size_t MAX_PROMPTS = 20;
const size_t MAX_PROMPT_LEN = 300;
std::vector<String> prompts();
// One prompt per line; blank lines are dropped and the rest capped to the limits above.
void savePrompts(const String& lines);
String promptsText();  // as saved: one per line

// The X4's own SSH private key (OpenSSH base64), empty until generated.
String deviceKey();
void saveDeviceKey(const String& base64);

}  // namespace settings

#pragma once

#include <Arduino.h>

// One SSH host the X4 can connect to. Stored in NVS; edited in the setup portal.
struct HostConfig {
  String name;
  String address;
  uint16_t port = 22;
  String user;

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

// Display rotation (0-3); 1 and 3 are the two portrait orientations.
uint8_t rotation();
void saveRotation(uint8_t rotation);

// Password for the settings web page. Empty means the page is open.
String webPassword();
void saveWebPassword(const String& password);

HostConfig loadHost();
void saveHost(const HostConfig& host);

// Pinned SHA256 host-key fingerprint ("SHA256:..."), empty until first use.
String pinnedHostKey();
void pinHostKey(const String& fingerprint);
void forgetHostKey();

// The X4's own SSH private key (OpenSSH base64), empty until generated.
String deviceKey();
void saveDeviceKey(const String& base64);

}  // namespace settings

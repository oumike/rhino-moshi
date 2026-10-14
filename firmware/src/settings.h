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

namespace settings {

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

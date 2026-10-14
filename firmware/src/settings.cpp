#include "settings.h"

#include <Preferences.h>

namespace {

const char* NS = "rhino";

}  // namespace

namespace settings {

HostConfig loadHost() {
  Preferences p;
  p.begin(NS, true);
  HostConfig h;
  h.name = p.getString("h_name", "");
  h.address = p.getString("h_addr", "");
  h.port = p.getUShort("h_port", 22);
  h.user = p.getString("h_user", "");
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
  String fp = p.getString("h_fp", "");
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

String deviceKey() {
  Preferences p;
  p.begin(NS, true);
  String k = p.getString("dev_key", "");
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

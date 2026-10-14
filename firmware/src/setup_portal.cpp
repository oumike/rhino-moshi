#include "setup_portal.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include "settings.h"

namespace setup_portal {

const char* AP_NAME = "Rhino-X4-Setup";

namespace {

// Closes the portal and reboots if nobody finishes setup, so a device left
// in setup mode does not keep an open access point forever.
const int PORTAL_TIMEOUT_S = 600;

}  // namespace

bool connect(const String& authorizedKeysLine, bool force, void (*onPortalOpen)()) {
  HostConfig host = settings::loadHost();

  WiFiManager wm;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setConnectTimeout(20);
  wm.setTitle("Rhino X4");
  wm.setAPCallback([onPortalOpen](WiFiManager*) {
    if (onPortalOpen) onPortalOpen();
  });

  // Base64 and quotes only, so it needs no HTML escaping.
  String keyHtml =
      "<h3>Device key</h3><p>Append this line to <code>~/.ssh/authorized_keys</code> on each host. It only "
      "allows a tunnel to moshi-hook's gateway: no shell, no commands, no other ports.</p>"
      "<textarea readonly rows='5' style='width:100%;font-family:monospace;font-size:12px' "
      "onclick='this.select()'>" +
      authorizedKeysLine + "</textarea><h3>Host</h3>";
  WiFiManagerParameter keyBlock(keyHtml.c_str());

  char portBuf[6];
  snprintf(portBuf, sizeof(portBuf), "%u", host.port);
  WiFiManagerParameter pName("h_name", "Name (shown on the X4)", host.name.c_str(), 24);
  WiFiManagerParameter pAddr("h_addr", "Address (IP or hostname)", host.address.c_str(), 64);
  WiFiManagerParameter pPort("h_port", "SSH port", portBuf, 5, "type='number' min='1' max='65535'");
  WiFiManagerParameter pUser("h_user", "SSH user", host.user.c_str(), 32);
  wm.addParameter(&keyBlock);
  wm.addParameter(&pName);
  wm.addParameter(&pAddr);
  wm.addParameter(&pPort);
  wm.addParameter(&pUser);

  wm.setSaveParamsCallback([&]() {
    HostConfig h;
    h.name = pName.getValue();
    h.address = pAddr.getValue();
    h.address.trim();
    long port = atol(pPort.getValue());
    h.port = (port > 0 && port <= 65535) ? port : 22;
    h.user = pUser.getValue();
    h.user.trim();
    if (h.name.isEmpty()) h.name = h.address;
    settings::saveHost(h);
  });

  bool ok;
  if (force || !host.isComplete()) {
    ok = wm.startConfigPortal(AP_NAME);
  } else {
    ok = wm.autoConnect(AP_NAME);
  }
  return ok && WiFi.status() == WL_CONNECTED;
}

}  // namespace setup_portal

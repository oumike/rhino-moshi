#pragma once

#include <Arduino.h>

namespace setup_portal {

// Name of the access point the setup portal runs on.
extern const char* AP_NAME;

/**
 * Brings Wi-Fi up, opening the setup portal when it is needed.
 *
 * The portal runs on the X4's own access point and is the only place Wi-Fi
 * and host settings are entered. It opens when there are no working Wi-Fi
 * credentials, when no host is configured yet, or when `force` is set (Back
 * held at boot). It shows `authorizedKeysLine` so it can be copied into the
 * host's ~/.ssh/authorized_keys.
 *
 * `onPortalOpen` runs once just before the portal starts, so the caller can
 * tell the user where to go. Returns true once connected to Wi-Fi.
 */
bool connect(const String& authorizedKeysLine, bool force, void (*onPortalOpen)());

}  // namespace setup_portal

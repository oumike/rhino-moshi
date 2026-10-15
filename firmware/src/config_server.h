#pragma once

#include <Arduino.h>

/**
 * Wi-Fi bring-up plus the settings web page.
 *
 * Normal operation: join the saved network and serve the settings page at
 * http://rhino-moshi-x4.local (and the device IP). The page is open unless a page
 * password has been set on it, after which it requires HTTP Basic auth
 * (user "admin").
 *
 * Setup mode: when no network is saved, or `forceSetup` is set (Back held at
 * boot), the X4 runs its own access point instead -- no captive redirect, just
 * the same page at http://192.168.4.1. The access point is open (no
 * password); it only runs until settings are saved.
 */
namespace config_server {

extern const char* HOSTNAME;  // mDNS name, without ".local"
extern const char* AP_SSID;

struct Callbacks {
  // Setup mode started; show the user how to join.
  void (*onSetupMode)(const char* ssid, const char* url) = nullptr;
  // Trying to join the saved network (called once per attempt).
  void (*onConnecting)(const char* ssid) = nullptr;
};

/**
 * Joins Wi-Fi, or runs setup mode. Setup mode never returns: saving restarts
 * the device. Joining retries until it succeeds. Then starts the web server
 * and returns.
 */
void begin(const String& authorizedKeysLine, bool forceSetup, const Callbacks& cb);

// Serve pending HTTP requests. Call from loop() and from any long wait.
void handle();

}  // namespace config_server

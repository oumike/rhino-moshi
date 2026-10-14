// Rhino Companion for the Xteink X4: a client for moshi-hook's gateway,
// reached over an SSH tunnel. See README.md.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "libssh_esp32.h"

#include "agents.h"
#include "buttons.h"
#include "config_server.h"
#include "gateway.h"
#include "settings.h"
#include "ssh_link.h"
#include "ui.h"

// libssh needs far more than the loop task's default 8 KB of stack.
SET_LOOP_TASK_STACK_SIZE(50 * 1024);

Buttons buttons;
Agents agents;
Gateway gateway(agents);

static void onSetupMode(const char* ssid, const char* url) {
  ui::message("Setup", String("Join the Wi-Fi network\n") + ssid + "\n\nthen open\n" + url +
                           "\n\nin a browser to set up Wi-Fi and the host.");
}

static void onConnecting(const char* ssid) {
  ui::message("Joining Wi-Fi", String(ssid) + "\n\nHold Back while restarting to change Wi-Fi.");
}

static void halt(const char* why) {
  Serial.printf("FATAL: %s\n", why);
  ui::message("Can't start", why);
  while (true) delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n== Rhino X4 ==");

  ui::begin();
  buttons.begin();
  bool forceSetup = false;
  for (int i = 0; i < 10; i++) {  // ~0.5 s to catch Back held at boot
    buttons.update();
    forceSetup |= buttons.isPressed(BTN_BACK);
    delay(50);
  }

  libssh_begin();
  ssh_key key = loadOrCreateDeviceKey();
  if (!key) halt("Could not create the device key.");
  String akLine = authorizedKeysLine(key);
  Serial.printf("authorized_keys line:\n%s\n", akLine.c_str());

  config_server::Callbacks cb;
  cb.onSetupMode = onSetupMode;
  cb.onConnecting = onConnecting;
  config_server::begin(akLine, forceSetup, cb);

  // Ages ("finished 5m ago") need wall-clock time; until it arrives they're hidden.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  gateway.begin(key);
  buttons.resync();
}

void loop() {
  buttons.update();
  config_server::handle();
  bool changed = gateway.poll();
  ui::update(gateway, agents, buttons, changed);
  delay(5);
}

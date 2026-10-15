// Rhino Companion for the Xteink X4 and ThinkNode M9: a client for moshi-hook's gateway,
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
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

Buttons buttons;
Agents agents;
Gateway gateway(agents);

static void onSetupMode(const char* ssid, const char* url) {
  ui::message("Setup", String("Join the Wi-Fi network\n") + ssid + "\n\nthen open\n" + url +
                           "\n\nin a browser to set up Wi-Fi and the host.");
}

static void onConnecting(const char* ssid) {
  ui::message("Joining Wi-Fi", String(ssid) + "\n\nHold " + (HAS_KEYBOARD ? "a key" : "Back") +
                                  " while restarting to change Wi-Fi.");
}

static void halt(const char* why) {
  Serial.printf("FATAL: %s\n", why);
  ui::message("Can't start", why);
  while (true) delay(1000);
}

// Serial debugging: b c l r u d press Back, Confirm, Left, Right, Up, Down;
// h holds Back, m is Home; s dumps the screen; text between double quotes is
// typed. Read in a task of its own, so presses land mid-refresh just as real
// ones do.
static volatile bool shotRequested = false;
static void uiTask(void*);

static void serialTask(void*) {
  bool quoted = false;
  for (;;) {
    while (!buttons.injecting() && Serial.available()) {
      int c = Serial.read();
      if (c == '"') quoted = !quoted;
      else if (quoted) buttons.injectChar((char)c);
      else if (c == 'b') buttons.inject(BTN_BACK);
      else if (c == 'c') buttons.inject(BTN_CONFIRM);
      else if (c == 'l') buttons.inject(BTN_LEFT);
      else if (c == 'r') buttons.inject(BTN_RIGHT);
      else if (c == 'u') buttons.inject(BTN_UP);
      else if (c == 'd') buttons.inject(BTN_DOWN);
      else if (c == 'h') buttons.inject(BTN_BACK, 1000);
      else if (c == 'm') buttons.inject(BTN_HOME);
      else if (c == 's') shotRequested = true;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n== Rhino " BOARD_NAME " ==");

  ui::begin();
  buttons.begin();
  bool forceSetup = false;
  for (int i = 0; i < 10; i++) {  // ~0.5 s to catch Back held at boot
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
  buttons.clearQueue();  // presses made while starting up
  xTaskCreate(serialTask, "serial", 3072, nullptr, 1, nullptr);
  xTaskCreate(uiTask, "ui", 8 * 1024, nullptr, 2, nullptr);
}

// The UI runs in a task of its own, above the network: a press is drawn
// while the network task sits in a blocking SSH call, which can take seconds
// when the link drops.
static void uiTask(void*) {
  for (;;) {
    if (shotRequested) {
      shotRequested = false;
      ui::dumpScreen();
    }
    buttons.update();
    gateway.lock();
    ui::update(gateway, agents, buttons, gateway.takeChanged());
    gateway.unlock();
    delay(5);
  }
}

// The Arduino loop task, with its big stack, is the network task.
void loop() {
  config_server::handle();
  gateway.poll();
  delay(5);
}

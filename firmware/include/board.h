/**
 * The board this build is for: pins, panel and keys. Pick one with
 * -DBOARD_X4 (the default) or -DBOARD_M9.
 *
 * Every board reports the same logical buttons, so the UI is written once.
 * They are bit positions in the input bitmask.
 */
#pragma once

#include <Arduino.h>

enum ButtonIndex : uint8_t {
  BTN_BACK = 0,
  BTN_CONFIRM = 1,
  BTN_LEFT = 2,
  BTN_RIGHT = 3,
  BTN_UP = 4,
  BTN_DOWN = 5,
  BTN_POWER = 6,
  BTN_HOME = 7,     // M9 only: the dashboard of every session
  BTN_KEYS = 8,     // M9 only: the button under Back, opens Keys
  BTN_PROMPT = 9,   // M9 only: the button under Home, opens Prompt
  BTN_MESSAGES = 10,  // M9 only: the far-left key, opens the session
  BTN_COUNT = 11,
};

#if defined(BOARD_M9)
#include "boards/m9.h"
#else
#include "boards/x4.h"
#endif

#ifndef DISPLAY_EPAPER
#define DISPLAY_EPAPER 0
#endif
// A backlit screen, turned off after the screen timeout (e-paper needs none).
#ifndef HAS_BACKLIGHT
#define HAS_BACKLIGHT 0
#endif
// A keyboard to type prompts on, rather than the on-screen one.
#ifndef HAS_KEYBOARD
#define HAS_KEYBOARD 0
#endif

// Keyboard builds can compose free-text prompts and send raw keys. Read-only
// builds (no keyboard) only view agents, answer cards, and send canned prompts.
#ifndef CAN_COMPOSE
#define CAN_COMPOSE HAS_KEYBOARD
#endif

// The agent page is the watched agent's scrollable conversation transcript,
// and the device boots to a host dashboard. With a keyboard (M9) prompts are
// composed inline under it; the X4 shows it read-only.
#ifndef HAS_TRANSCRIPT
#define HAS_TRANSCRIPT HAS_KEYBOARD
#endif

// The Home dashboard shows the host's CPU and memory, read by running top
// over SSH. Its key's forced command is that top instead of "false", so
// it still can't run anything else.
#ifndef HAS_HOST_STATS
#define HAS_HOST_STATS HAS_TRANSCRIPT
#endif

// The M9's small landscape layout. PREVIEW_COMPACT draws it on an X4, in the
// top-left 320x240 of the panel turned landscape, to check it by screenshot.
#if defined(BOARD_M9) || defined(PREVIEW_COMPACT)
#define LAYOUT_COMPACT 1
#define LAYOUT_W 320
#define LAYOUT_H 240
#else
#define LAYOUT_COMPACT 0
#endif

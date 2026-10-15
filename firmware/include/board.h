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
  BTN_HOME = 7,  // M9 only: back to the agent list
  BTN_COUNT = 8,
};

#if defined(BOARD_M9)
#include "boards/m9.h"
#else
#include "boards/x4.h"
#endif

#ifndef DISPLAY_EPAPER
#define DISPLAY_EPAPER 0
#endif
// A keyboard to type prompts on, rather than the on-screen one.
#ifndef HAS_KEYBOARD
#define HAS_KEYBOARD 0
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

#pragma once

#include <Adafruit_GFX.h>
#include <Arduino.h>

#include "board.h"

/**
 * The screen, behind one interface: the X4's e-paper panel or the M9's TFT.
 * The UI draws through gfx() between startFrame() and the last nextPage().
 */
namespace panel {

void begin(uint8_t rotation);

Adafruit_GFX& gfx();

// Starts a frame. `full` matters on e-paper only: a full refresh (slow, with
// a flash) clears the ghosting partial ones leave.
void startFrame(bool full, uint8_t rotation);
// Sends what was drawn to the screen. True means draw the frame again for the
// next page (the X4 has one page, so it never is). Takes as long as the panel
// does: up to ~2 s for a full e-paper refresh, ~20 ms on the TFT.
bool nextPage();

// Writes the last frame to serial for tools/x4log.py: base64 between a header
// line and "END". The X4 sends "SHOT <w> <h> <rotation>" and its 1-bit RAM in
// native orientation; the M9 "SHOT565 <w> <h>" and RGB565 as drawn.
void dump(uint8_t rotation);
// The base64 body of a dump, 80 characters a line, then "END".
void dumpBase64(const uint8_t* data, size_t n);

}  // namespace panel

// Colour roles. E-paper has two colours; GxEPD2_BW draws anything that isn't
// white as black.
namespace color {
#if DISPLAY_EPAPER
constexpr uint16_t BG = 0xFFFF, FG = 0x0000, MUTED = FG, RULE = FG, SEL_BG = FG, SEL_FG = BG, NOTICE_BG = FG,
                   NOTICE_FG = BG, BAR = FG, KEY_BG = FG, KEY_FG = BG;
constexpr uint16_t BLOCKED = FG, WORKING = FG, DONE = FG, IDLE = FG;
#else
constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
}
constexpr uint16_t BG = rgb(0x12, 0x14, 0x1A), FG = rgb(0xE8, 0xEA, 0xEE), MUTED = rgb(0x8A, 0x90, 0x9C),
                   RULE = rgb(0x3A, 0x3F, 0x4A), SEL_BG = rgb(0x2B, 0x4C, 0x7E), SEL_FG = rgb(0xFF, 0xFF, 0xFF),
                   NOTICE_BG = rgb(0xF2, 0xB1, 0x34), NOTICE_FG = rgb(0x12, 0x14, 0x1A), BAR = rgb(0x5B, 0x9B, 0xF0),
                   KEY_BG = rgb(0x3A, 0x3F, 0x4A), KEY_FG = rgb(0xE8, 0xEA, 0xEE);
constexpr uint16_t BLOCKED = rgb(0xF2, 0xB1, 0x34), WORKING = rgb(0x5B, 0x9B, 0xF0), DONE = rgb(0x4C, 0xC3, 0x6A),
                   IDLE = rgb(0x8A, 0x90, 0x9C);
#endif
}  // namespace color

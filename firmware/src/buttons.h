#pragma once

#include <Arduino.h>

#include "board.h"

/**
 * Debounced reader for the X4's seven buttons.
 *
 * Six of them share two resistor ladders, so at most one button per ladder can
 * be distinguished at a time -- chords within a ladder read as a third,
 * meaningless voltage. Treat this as "one button at a time" input.
 */
class Buttons {
 public:
  void begin();

  // Poll and debounce. Call once per loop iteration.
  void update();

  /**
   * Re-baseline to whatever is being read now, discarding pending events, and
   * ignore transitions for the next `settleMs`.
   *
   * Call after anything that blocks the poll loop for a long stretch -- above
   * all a panel refresh, which starves this of samples for seconds and
   * disturbs the rail the ladders divide. Without it the first sample
   * afterwards looks like a fresh transition and registers as a press nobody
   * made, which on a decision card silently answers it.
   *
   * Discarding a press made *during* the refresh is correct, not a loss: the
   * card was not on screen yet, so nobody could have been answering it.
   */
  void resync(unsigned long settleMs = SETTLE_MS);

  bool isPressed(uint8_t button) const { return current_ & (1 << button); }
  bool wasPressed(uint8_t button) const { return pressedEvents_ & (1 << button); }
  bool wasReleased(uint8_t button) const { return releasedEvents_ & (1 << button); }
  bool wasAnyPressed() const { return pressedEvents_ != 0; }

  // How long the current (or most recent) press has lasted, in ms.
  unsigned long heldTime() const;

  // Raw ladder readings in 16-bit space, for `calibrate`.
  uint16_t rawAdc1() const { return rawAdc1_; }
  uint16_t rawAdc2() const { return rawAdc2_; }

  static const char* name(uint8_t button);

 private:
  uint16_t readState();

  uint8_t current_ = 0;
  uint8_t last_ = 0;
  uint8_t pressedEvents_ = 0;
  uint8_t releasedEvents_ = 0;
  unsigned long lastDebounceAt_ = 0;
  unsigned long pressStartedAt_ = 0;
  unsigned long pressFinishedAt_ = 0;
  unsigned long settleStartedAt_ = 0;
  unsigned long settleMs_ = 0;
  uint16_t rawAdc1_ = 0;
  uint16_t rawAdc2_ = 0;

  static const unsigned long DEBOUNCE_MS = 50;

  // How long the ladders are treated as untrustworthy after a panel refresh.
  // Comfortably longer than DEBOUNCE_MS, so a reading disturbed by the refresh
  // cannot survive long enough to debounce into a press.
  static const unsigned long SETTLE_MS = 250;
};

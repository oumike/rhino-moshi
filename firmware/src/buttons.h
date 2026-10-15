#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "board.h"

/**
 * Debounced, buffered reader for the device's keys.
 *
 * On the X4, seven buttons. A background task samples the ladders every few
 * milliseconds, whatever the
 * main loop is doing (a panel refresh, an SSH round trip, parsing a frame),
 * and queues each press, release and hold in order. The loop takes them one
 * at a time, so nothing pressed is lost and nothing is reported twice.
 *
 * Six of the buttons share two resistor ladders, so at most one button per
 * ladder can be distinguished at a time -- chords within a ladder read as a
 * third, meaningless voltage. Treat this as "one button at a time" input.
 *
 * On the M9, a keyboard: its controller reports key presses (no releases)
 * over I2C. Del, Enter and the arrows act as Back, Confirm and the four
 * directions; printable keys come through as typed(). Holding Del is a hold
 * of Back.
 */
class Buttons {
 public:
  // A press lasting this long also queues a hold event, while still held.
  static const unsigned long HOLD_MS = 700;

  // Starts the sampling task.
  void begin();

  // Takes the next queued event into wasPressed()/wasReleased()/wasHeld(), or
  // clears them if there is none. Call once per loop iteration.
  void update() { next(); }
  // The same, returning false when nothing was queued: for draining the queue.
  bool next();
  bool queued() const { return uxQueueMessagesWaiting(queue_) > 0; }
  void clearQueue() { xQueueReset(queue_); }

  // Live, debounced state.
  bool isPressed(uint8_t button) const { return stable_ & (1 << button); }

  // The current event.
  bool wasPressed(uint8_t button) const { return event_.pressed & (1 << button); }
  bool wasReleased(uint8_t button) const { return event_.released & (1 << button); }
  bool wasHeld(uint8_t button) const { return event_.held & (1 << button); }
  // A printable character typed on a keyboard, or 0.
  char typed() const { return event_.ch; }
  bool hasEvent() const { return event_.pressed || event_.released || event_.held || event_.ch; }
  // When it happened (millis()), 0 with no event.
  unsigned long eventAt() const { return event_.at; }
  // For a release or hold: how long the button had been down.
  unsigned long heldTime() const { return event_.heldMs; }

  // A press of `heldMs` as if made on the keys -- it goes through the same
  // sampling, debouncing and queue. For driving the UI over serial.
  void inject(uint8_t button, unsigned long heldMs = 100);
  // An injected press is still down, or its release not yet debounced.
  bool injecting() const { return injectMask_ || (long)(injectFreeAt_ - millis()) > 0; }
  // A character as if typed on a keyboard.
  void injectChar(char c);

  // Upside down, the side keys trade places: report each as the other so Up
  // stays the upper one.
  void setSwapUpDown(bool swap) { swapUpDown_ = swap; }

  static const char* name(uint8_t button);

 private:
  struct Event {
    uint8_t pressed, released, held;
    unsigned long at, heldMs;
    char ch;
  };

  static void task(void* self);
  void sample();
  uint8_t readState();
  void push(const Event& e);

  QueueHandle_t queue_ = nullptr;
  Event event_ = {};

  // Sampling task state.
  volatile uint8_t stable_ = 0;
  uint8_t last_ = 0;
  unsigned long changedAt_ = 0, pressStartedAt_ = 0;
  bool holdSent_ = false;
  volatile bool swapUpDown_ = false;
  volatile uint8_t injectMask_ = 0;
  volatile unsigned long injectUntil_ = 0, injectFreeAt_ = 0;

#if defined(BOARD_M9)
  void readKeyboard();
  void tap(uint8_t button);
  uint8_t kbAddr_ = 0;
  unsigned long lastPoll_ = 0;
#endif

  static const int QUEUE_SIZE = 32;
  static const unsigned long SAMPLE_MS = 5;
  static const unsigned long DEBOUNCE_MS = 50;
};

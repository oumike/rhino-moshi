#include "buttons.h"

namespace {

// The ESP32 Arduino core reads 12 bits; the documented ladder thresholds are in
// 16-bit space. Shifting left by 4 puts them in the same units. It is a scale
// change, not added precision.
uint16_t readAdc16(uint8_t pin) {
  return (uint16_t)(analogRead(pin) << 4);
}

// A reading belongs to button i when ranges[i + 1] < raw <= ranges[i].
// Anything above ranges[0] is the idle rail: no button held.
int8_t decodeLadder(int raw, const int* ranges, uint8_t count) {
  for (uint8_t i = 0; i < count; i++) {
    if (raw > ranges[i + 1] && raw <= ranges[i]) return (int8_t)i;
  }
  return -1;
}

}  // namespace

void Buttons::begin() {
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // full ~0-3.3V swing, matching the ladder
  pinMode(PIN_BUTTON_POWER, INPUT_PULLUP);
}

uint16_t Buttons::readState() {
  uint16_t state = 0;

  rawAdc1_ = readAdc16(PIN_BUTTON_ADC_1);
  int8_t b1 = decodeLadder(rawAdc1_, BUTTON_ADC_1_RANGES, BUTTON_ADC_1_COUNT);
  if (b1 >= 0) state |= (1 << b1);

  rawAdc2_ = readAdc16(PIN_BUTTON_ADC_2);
  int8_t b2 = decodeLadder(rawAdc2_, BUTTON_ADC_2_RANGES, BUTTON_ADC_2_COUNT);
  if (b2 >= 0) state |= (1 << (b2 + 4));  // ladder 2 starts at BTN_UP

  if (digitalRead(PIN_BUTTON_POWER) == LOW) state |= (1 << BTN_POWER);

  return state;
}

void Buttons::resync(unsigned long settleMs) {
  uint8_t state = (uint8_t)readState();
  current_ = state;
  last_ = state;
  pressedEvents_ = 0;
  releasedEvents_ = 0;
  queueLen_ = 0;
  replaying_ = false;
  lastDebounceAt_ = millis();
  settleStartedAt_ = lastDebounceAt_;
  settleMs_ = settleMs;
}

void Buttons::beginCapture() {
  capturing_ = true;
}

void Buttons::endCapture(bool keep) {
  capturing_ = false;
  if (!keep) resync();
}

void Buttons::push() {
  if (queueLen_ >= QUEUE_SIZE) return;
  queue_[(queueHead_ + queueLen_) % QUEUE_SIZE] = {pressedEvents_, releasedEvents_, releasedEvents_ ? heldTime() : 0};
  queueLen_++;
}

bool Buttons::replay() {
  if (!queueLen_) return false;
  const Event& e = queue_[queueHead_];
  queueHead_ = (queueHead_ + 1) % QUEUE_SIZE;
  queueLen_--;
  pressedEvents_ = e.pressed;
  releasedEvents_ = e.released;
  replaying_ = e.released != 0;
  replayHeld_ = e.held;
  return true;
}

void Buttons::update() {
  unsigned long now = millis();
  uint8_t state = (uint8_t)readState();

  pressedEvents_ = 0;
  releasedEvents_ = 0;
  replaying_ = false;

  // Inside the post-refresh settling window, keep the baseline following the
  // reading but refuse to emit events. A rail still recovering from a refresh
  // can sit in a button's voltage band for longer than DEBOUNCE_MS, which is
  // otherwise indistinguishable from someone holding that button down.
  if (settleMs_ > 0) {
    if (now - settleStartedAt_ < settleMs_) {
      last_ = state;
      current_ = state;
      lastDebounceAt_ = now;
      return;
    }
    settleMs_ = 0;
  }

  if (state != last_) {
    lastDebounceAt_ = now;
    last_ = state;
    return;
  }

  if (now - lastDebounceAt_ <= DEBOUNCE_MS) return;
  if (state == current_) return;

  pressedEvents_ = state & ~current_;
  releasedEvents_ = current_ & ~state;

  if (pressedEvents_ && current_ == 0) pressStartedAt_ = now;
  if (releasedEvents_ && state == 0) pressFinishedAt_ = now;

  current_ = state;

  // Keep order: while anything is queued, new events join the back of it.
  if (capturing_ || queueLen_) {
    push();
    pressedEvents_ = 0;
    releasedEvents_ = 0;
  }
}

unsigned long Buttons::heldTime() const {
  if (replaying_) return replayHeld_;
  if (current_ != 0) return millis() - pressStartedAt_;
  return pressFinishedAt_ - pressStartedAt_;
}

const char* Buttons::name(uint8_t button) {
  switch (button) {
    case BTN_BACK: return "back";
    case BTN_CONFIRM: return "confirm";
    case BTN_LEFT: return "left";
    case BTN_RIGHT: return "right";
    case BTN_UP: return "up";
    case BTN_DOWN: return "down";
    case BTN_POWER: return "power";
    default: return "unknown";
  }
}

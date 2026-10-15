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
  queue_ = xQueueCreate(QUEUE_SIZE, sizeof(Event));
  last_ = stable_ = readState();
  // Above the loop task, so a busy loop can't starve it; it runs for
  // microseconds every SAMPLE_MS.
  xTaskCreate(task, "buttons", 3072, this, 3, nullptr);
}

void Buttons::task(void* self) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    static_cast<Buttons*>(self)->sample();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(SAMPLE_MS));
  }
}

uint8_t Buttons::readState() {
  uint8_t state = 0;

  int8_t b1 = decodeLadder(readAdc16(PIN_BUTTON_ADC_1), BUTTON_ADC_1_RANGES, BUTTON_ADC_1_COUNT);
  if (b1 >= 0) state |= (1 << b1);

  int8_t b2 = decodeLadder(readAdc16(PIN_BUTTON_ADC_2), BUTTON_ADC_2_RANGES, BUTTON_ADC_2_COUNT);
  if (b2 >= 0 && swapUpDown_) b2 = 1 - b2;
  if (b2 >= 0) state |= (1 << (b2 + 4));  // ladder 2 starts at BTN_UP

  if (digitalRead(PIN_BUTTON_POWER) == LOW) state |= (1 << BTN_POWER);

  if (injectMask_) {
    if ((long)(injectUntil_ - millis()) > 0) {
      state |= injectMask_;
    } else {
      injectMask_ = 0;
      injectFreeAt_ = millis() + 2 * DEBOUNCE_MS;
    }
  }
  return state;
}

void Buttons::inject(uint8_t button, unsigned long heldMs) {
  injectUntil_ = millis() + heldMs;
  injectFreeAt_ = injectUntil_ + 2 * DEBOUNCE_MS;
  injectMask_ = 1 << button;
}

void Buttons::push(const Event& e) {
  xQueueSend(queue_, &e, 0);  // full: drop the newest rather than block sampling
}

void Buttons::sample() {
  unsigned long now = millis();
  uint8_t state = readState();
  if (state != last_) {
    last_ = state;
    changedAt_ = now;
  } else if (state != stable_ && now - changedAt_ >= DEBOUNCE_MS) {
    Event e = {};
    e.pressed = state & ~stable_;
    e.released = stable_ & ~state;
    e.at = now;
    if (e.pressed && !stable_) {
      pressStartedAt_ = now;
      holdSent_ = false;
    }
    if (e.released) e.heldMs = now - pressStartedAt_;
    stable_ = state;
    push(e);
  }
  if (stable_ && !holdSent_ && now - pressStartedAt_ >= HOLD_MS) {
    holdSent_ = true;
    push({0, 0, stable_, now, now - pressStartedAt_});
  }
}

bool Buttons::next() {
  if (xQueueReceive(queue_, &event_, 0) == pdTRUE) return true;
  event_ = {};
  return false;
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

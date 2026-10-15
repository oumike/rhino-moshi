#include "buttons.h"

#if defined(BOARD_M9)
#include <Wire.h>
#endif

void Buttons::task(void* self) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    static_cast<Buttons*>(self)->sample();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(SAMPLE_MS));
  }
}

void Buttons::push(const Event& e) {
  xQueueSend(queue_, &e, 0);  // full: drop the newest rather than block sampling
}

void Buttons::injectChar(char c) {
  Event e = {};
  e.ch = c;
  e.at = millis();
  push(e);
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
    case BTN_HOME: return "home";
    default: return "unknown";
  }
}

#if defined(BOARD_M9)

void Buttons::begin() {
  queue_ = xQueueCreate(QUEUE_SIZE, sizeof(Event));
  pinMode(KB_INT, INPUT_PULLDOWN);
  pinMode(KB_LED, OUTPUT);
  digitalWrite(KB_LED, LOW);
  Wire.begin(KB_SDA, KB_SCL, 100000);
  for (uint8_t a : KB_ADDRS) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      kbAddr_ = a;
      break;
    }
  }
  if (kbAddr_) {
    Wire.beginTransmission(kbAddr_);
    Wire.write(KB_REG_LONG_PRESS);
    Wire.write((uint8_t)(HOLD_MS >> 8));
    Wire.write((uint8_t)(HOLD_MS & 0xFF));
    Wire.endTransmission();
    Serial.printf("[kb] keyboard at 0x%02X\n", kbAddr_);
  } else {
    Serial.println("[kb] no keyboard found");
  }
  stable_ = digitalRead(KB_INT) == HIGH ? (1 << BTN_BACK) : 0;
  xTaskCreate(task, "buttons", 3072, this, 3, nullptr);
}

void Buttons::inject(uint8_t button, unsigned long heldMs) {
  if (heldMs < HOLD_MS) {
    tap(button);
    return;
  }
  uint8_t bit = 1 << button;
  unsigned long now = millis();
  push({0, 0, bit, now, heldMs, 0});
  push({0, bit, 0, now, heldMs, 0});
}

// The controller reports presses only; each is its own release.
void Buttons::tap(uint8_t button) {
  uint8_t bit = 1 << button;
  push({bit, bit, 0, millis(), 0, 0});
}

void Buttons::sample() {
  // Any key down reads as Back held: what the boot-time check for setup mode
  // looks at.
  bool down = digitalRead(KB_INT) == HIGH;
  stable_ = down ? (1 << BTN_BACK) : 0;
  unsigned long now = millis();
  // The interrupt line says a key is waiting; poll now and then as well, in
  // case a pulse was too short to see.
  if (!kbAddr_ || (!down && now - lastPoll_ < 100)) return;
  lastPoll_ = now;
  readKeyboard();
}

void Buttons::readKeyboard() {
  Wire.beginTransmission(kbAddr_);
  Wire.write(KB_REG_KEY);
  if (Wire.endTransmission() != 0 || Wire.requestFrom(kbAddr_, (uint8_t)1) != 1) return;
  uint8_t code = Wire.read();
  if (code == KB_NONE || code == KB_INVALID) return;
  Serial.printf("[kb] 0x%02X\n", code);
  switch (code) {
    case KB_DEL: tap(BTN_BACK); break;
    case KB_DEL_LONG: inject(BTN_BACK, HOLD_MS); break;
    case KB_ENTER: tap(BTN_CONFIRM); break;
    case KB_LEFT: tap(BTN_LEFT); break;
    case KB_RIGHT: tap(BTN_RIGHT); break;
    case KB_UP: tap(BTN_UP); break;
    case KB_DOWN: tap(BTN_DOWN); break;
    case KB_HOME: tap(BTN_HOME); break;
    default:
      if (code >= 0x20 && code < 0x7F) injectChar((char)code);
      break;
  }
}

#else  // X4: resistor ladders

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

#endif

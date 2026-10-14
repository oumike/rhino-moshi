/**
 * Xteink X4 board definition.
 *
 * The X4 is an ESP32-C3 driving a 4.26" 800x480 GDEQ0426T82 e-paper panel.
 * Pin assignments are cross-validated against two independent sources:
 *   - CrossPoint firmware  lib/hal/HalGPIO.h
 *   - Adafruit_CircuitPython_Xteink_X4  adafruit_xteink_x4.py
 * Both agree on every pin below.
 */
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// E-paper panel (SPI, custom pins -- not the ESP32-C3 hardware SPI defaults)
// ---------------------------------------------------------------------------
#define EPD_SCLK 8
#define EPD_MOSI 10
#define EPD_CS   21
#define EPD_DC   4
#define EPD_RST  5
#define EPD_BUSY 6

// MISO is shared between the SD card and the panel. The panel never drives it,
// but SPI.begin() wants a pin, and passing -1 would leave the SD card unusable
// if this firmware ever grows SD support.
#define SPI_MISO 7
#define SD_CS    12

// ---------------------------------------------------------------------------
// Buttons
//
// Only the power button has its own GPIO. The other six sit on two resistor
// ladders, each read through one ADC channel: pressing a button pulls the rail
// down to a distinct voltage. Thresholds below are in CircuitPython's 16-bit
// ADC space (0-65535); read_adc16() in buttons.cpp scales the ESP32's 12-bit
// readings up to match, so these constants stay comparable to the upstream
// library and to anything else documented for this hardware.
//
// Measured idle/pressed values (Adafruit, calibrated on hardware):
//   ADC1: idle ~58107, Back ~50972, Confirm ~39531, Left ~22083, Right ~63
//   ADC2: idle ~58107, Up ~32985, Down ~61
//
// A reading is button i when RANGES[i+1] < raw <= RANGES[i]; anything above
// RANGES[0] is idle. Run `rhino-companion calibrate` if your unit differs --
// board revisions can shift the ladder.
// ---------------------------------------------------------------------------
#define PIN_BUTTON_ADC_1 1  // Back, Confirm, Left, Right
#define PIN_BUTTON_ADC_2 2  // Up, Down
#define PIN_BUTTON_POWER 3  // digital, active-low, needs INPUT_PULLUP

static const int BUTTON_ADC_1_RANGES[] = {54000, 45000, 30000, 11000, -1};
static const int BUTTON_ADC_2_RANGES[] = {45000, 16000, -1};
static const uint8_t BUTTON_ADC_1_COUNT = 4;
static const uint8_t BUTTON_ADC_2_COUNT = 2;

// Button indices. Order matches CrossPoint's HalGPIO so the two are directly
// comparable, and matches the bit positions used in the input bitmask.
enum ButtonIndex : uint8_t {
  BTN_BACK = 0,
  BTN_CONFIRM = 1,
  BTN_LEFT = 2,
  BTN_RIGHT = 3,
  BTN_UP = 4,
  BTN_DOWN = 5,
  BTN_POWER = 6,
  BTN_COUNT = 7,
};

// ---------------------------------------------------------------------------
// Physical key positions
//
// On-screen labels are anchored to wherever the real key is, so the label
// points at the button rather than merely naming it. Two clusters, matching
// the two ADC ladders:
//
//   - Up/Down are the page-turn keys on the RIGHT EDGE.
//   - Back/Confirm/Left/Right are the row BELOW THE SCREEN, left to right.
//
// If the labels come out over the wrong keys, reorder BOTTOM_ROW_ORDER -- that
// is the only thing that needs to change.
// ---------------------------------------------------------------------------
static const uint8_t BOTTOM_ROW_ORDER[4] = {BTN_BACK, BTN_CONFIRM, BTN_LEFT,
                                            BTN_RIGHT};

// Where the right-edge keys sit vertically, as a fraction of screen height.
static const float SIDE_KEY_UP_FRAC = 0.38f;
static const float SIDE_KEY_DOWN_FRAC = 0.62f;

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------
#define PIN_BATTERY_ADC 0   // battery voltage through a 2:1 divider
#define PIN_USB_DETECT  20  // high while USB power is present

static const float BATTERY_DIVIDER = 2.0f;

// ---------------------------------------------------------------------------
// Panel geometry
//
// The driver's native orientation is landscape (800x480). The X4 is a portrait
// device, so rotation 1 or 3 is what you actually want -- which of the two
// depends on which way up you hold it. Rotation is stored in NVS and settable
// at runtime via POST /config, so you can flip it without reflashing.
//
// 3 is the one that comes out right way up on hardware, with the page-turn keys
// on the right edge as the button layout above assumes. 1 is the same axis
// inverted, i.e. upside down.
// ---------------------------------------------------------------------------
#define PANEL_NATIVE_WIDTH  800
#define PANEL_NATIVE_HEIGHT 480
#define DEFAULT_ROTATION    3

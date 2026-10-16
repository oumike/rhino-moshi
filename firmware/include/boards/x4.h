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

#define BOARD_NAME "X4"
#define BOARD_SLUG "x4"
// Panel class: e-paper, so the UI batches redraws and mixes partial and full
// refreshes. Black and white only.
#define DISPLAY_EPAPER 1
// The conversation view and the host dashboard, read-only: no keyboard to
// compose with. There's no PSRAM, so the transcript is kept short (see
// transcript.h).
#define HAS_TRANSCRIPT 1

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
#define SPI_SCLK EPD_SCLK
#define SPI_MOSI EPD_MOSI

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
// Measured on our X4 with this firmware (ESP32 ADC, 11 dB):
//   ADC1: idle 65520, Back ~55780, Confirm ~42710, Left ~23750, Right ~50
//   ADC2: idle 65520, Up ~35530, Down ~60
// The thresholds below sit midway between those neighbours.
//
// A reading is button i when RANGES[i+1] < raw <= RANGES[i]; anything above
// RANGES[0] is idle. Board revisions can shift the ladder: if a button never
// registers, log Buttons::rawAdc1()/rawAdc2() over serial and re-centre these.
// ---------------------------------------------------------------------------
#define PIN_BUTTON_ADC_1 1  // Back, Confirm, Left, Right
#define PIN_BUTTON_ADC_2 2  // Up, Down
#define PIN_BUTTON_POWER 3  // digital, active-low, needs INPUT_PULLUP

// Adafruit's thresholds (54000, 45000, 30000, 11000 / 45000, 16000) put our
// Back above the idle boundary, so it never registered.
static const int BUTTON_ADC_1_RANGES[] = {60500, 49000, 33000, 12000, -1};
static const int BUTTON_ADC_2_RANGES[] = {50500, 18000, -1};
static const uint8_t BUTTON_ADC_1_COUNT = 4;
static const uint8_t BUTTON_ADC_2_COUNT = 2;

// ---------------------------------------------------------------------------
// Physical key positions
//
// The X4 is held with its key row ABOVE the screen and the page-turn keys on
// the LEFT edge (see PANEL_ROTATION). Turned that way, the keys' wiring runs
// backwards, so Buttons swaps Up/Down and Left/Right: Up is still the upper
// side key, and Left the leftmost of the pair. The key row then reads, left to
// right, Left, Right, Confirm, Back.
//
// On-screen labels are drawn in a band along the top edge, each over its key.
// If they come out over the wrong keys, reorder KEY_ROW_ORDER -- that is the
// only thing that needs to change.
// ---------------------------------------------------------------------------
static const uint8_t KEY_ROW_ORDER[4] = {BTN_LEFT, BTN_RIGHT, BTN_CONFIRM, BTN_BACK};

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------
#define PIN_BATTERY_ADC 0   // battery voltage through a 2:1 divider
#define PIN_USB_DETECT  20  // high while USB power is present

static const float BATTERY_DIVIDER = 2.0f;

// ---------------------------------------------------------------------------
// Panel geometry
//
// The driver's native orientation is landscape (800x480); the X4 is used
// portrait. Rotation 1 puts the key row above the screen and the page-turn
// keys on the left edge, as the key layout above assumes; it is the only one
// used. (3 is the same axis turned 180 degrees.)
// ---------------------------------------------------------------------------
#define PANEL_NATIVE_WIDTH  800
#define PANEL_NATIVE_HEIGHT 480
#define PANEL_ROTATION      1

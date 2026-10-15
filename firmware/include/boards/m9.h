/**
 * Elecrow ThinkNode M9 board definition.
 *
 * ESP32-S3R8 (8 MB octal PSRAM, 16 MB flash) with a 2.4" 240x320 ST7789 TFT,
 * used landscape, and a QWERTY keyboard run by an STC8H companion MCU on I2C.
 * Pins from Meshtastic's variants/esp32s3/ELECROW-ThinkNode-M9 and its
 * STC8HKeyboard driver; MeshCore's port agrees.
 */
#pragma once

#include <Arduino.h>

#define BOARD_NAME "M9"
#define BOARD_SLUG "m9"
#define HAS_KEYBOARD 1

// ---------------------------------------------------------------------------
// SPI bus: shared by the display, the LoRa radio and the SD card. Every chip
// select must be high before any of them is touched.
// ---------------------------------------------------------------------------
#define SPI_SCLK 40
#define SPI_MOSI 47
#define SPI_MISO 38
#define SD_CS    48
#define LORA_CS  39

#define TFT_CS   16
#define TFT_DC   15
#define TFT_RST  14
#define TFT_BL   17
#define TFT_SPI_HZ 40000000
// Active low: Meshtastic's variant.h has TFT_BACKLIGHT_ON LOW and inverts its
// backlight PWM. Flip this if the screen stays dark.
#define TFT_BL_ON LOW

// The panel and keyboard sit on a switched rail: low powers it.
#define PIN_VEXT 18
#define VEXT_ON  LOW

// ---------------------------------------------------------------------------
// Keyboard
//
// The STC8H scans the matrix itself (shift and long presses included) and
// raises KB_INT when it has a key. Reading register KB_REG_KEY returns one
// code: printable ASCII, or one of the specials below. There are no release
// events. Two firmware revisions answer on different addresses.
// ---------------------------------------------------------------------------
#define KB_SDA 20
#define KB_SCL 21
#define KB_INT 12
#define KB_LED 46
static const uint8_t KB_ADDRS[] = {0x6C, 0x6D};
static const uint8_t KB_REG_KEY = 0x01;
static const uint8_t KB_REG_LONG_PRESS = 0x03;  // two bytes, ms, big-endian

enum KbCode : uint8_t {
  KB_NONE = 0x00,
  KB_DEL = 0x08,
  KB_ENTER = 0x0D,
  KB_MUTE = 0x81,
  KB_HOME = 0x82,
  KB_TIME = 0x83,
  KB_TORCH = 0x84,
  KB_FM = 0x85,
  KB_FM_LONG = 0x86,
  KB_PRESET = 0x87,
  KB_INVALID = 0x88,
  KB_DEL_LONG = 0x89,
  KB_LEFT = 0xB4,
  KB_UP = 0xB5,
  KB_DOWN = 0xB6,
  KB_RIGHT = 0xB7,
};

// No physical key layout to point at: the footer shows key names instead.
static const uint8_t BOTTOM_ROW_ORDER[4] = {BTN_BACK, BTN_CONFIRM, BTN_LEFT, BTN_RIGHT};

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------
#define PIN_BATTERY_ADC 13
#define PIN_EXT_POWER   1  // low while external power is present
static const float BATTERY_DIVIDER = 2.0f;

// ---------------------------------------------------------------------------
// Panel geometry: native 240x320 portrait, used landscape. 1 and 3 are the two
// landscape rotations; Flip screen in the menu swaps them.
// ---------------------------------------------------------------------------
#define PANEL_NATIVE_WIDTH  240
#define PANEL_NATIVE_HEIGHT 320
#define DEFAULT_ROTATION    3

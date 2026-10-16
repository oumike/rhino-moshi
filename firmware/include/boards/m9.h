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
// Through a PNP transistor, so active low.
#define TFT_BL_ON LOW
#define HAS_BACKLIGHT 1

// The panel and keyboard sit on a switched rail: low powers it.
#define PIN_VEXT 18
#define VEXT_ON  LOW

// ---------------------------------------------------------------------------
// Keyboard
//
// A companion MCU scans the matrix itself (shift and long presses included).
// Reading its key register returns one pending code, 0 for none: printable
// ASCII, or one of the specials below. There are no release events. Codes and
// registers as verified on hardware by camillia-mt (src/hal/hw_m9.h,
// src/keyboard.cpp).
//
// GPIO20/21 are also the S3's native USB pads, which the ROM's USB-Serial-JTAG
// claims at reset; that claim is dropped before the bus starts. GPIO46 is the
// schematic's KEY_LED but is unconnected and a strapping pin: leave it alone.
// ---------------------------------------------------------------------------
#define KB_SDA 20
#define KB_SCL 21
#define KB_INT 12  // active high; early controllers may never raise it
static const uint8_t KB_ADDRS[] = {0x6C, 0x6D};
static const uint8_t KB_REG_HW_VERSION = 0x00;
static const uint8_t KB_REG_FW_VERSION = 0xFE;
static const uint8_t KB_REG_KEY = 0x05;        // production STC8H controller
static const uint8_t KB_REG_KEY_EARLY = 0x01;  // early units: hw 0x03, fw 0x10

enum KbCode : uint8_t {
  KB_NONE = 0x00,
  KB_DEL = 0x08,
  KB_ENTER_LF = 0x0A,
  KB_ENTER = 0x0D,
  KB_DEL_7F = 0x7F,
  KB_MESSAGES = 0x81,    // far-left key (camillia's Messages): opens the session
  KB_HOME = 0x82,
  KB_UNDER_HOME = 0x83,  // button below Home (camillia's DMs): opens Prompt
  KB_UNDER_BACK = 0x84,  // button below Back (camillia's Tools): opens Keys
  KB_BACK_EARLY = 0x86,  // Back on early controllers; Map held on production
  KB_BACK = 0x87,        // Back on production controllers
  KB_INVALID = 0x88,
  KB_DEL_LONG = 0x89,    // Back or Del held
  KB_CENTER_LONG = 0xA3,
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
// landscape rotations; Flip screen in the menu swaps them. 1 (MADCTL 0xA0)
// puts the keyboard below the screen; camillia-mt's LovyanGFX rotation 3 is
// the same register value.
// ---------------------------------------------------------------------------
#define PANEL_NATIVE_WIDTH  240
#define PANEL_NATIVE_HEIGHT 320
#define DEFAULT_ROTATION    1

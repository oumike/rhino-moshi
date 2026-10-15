#include "board.h"
#if !DISPLAY_EPAPER

#include <SPI.h>
#include <esp_heap_caps.h>

#include "panel.h"

namespace panel {
namespace {

// Just enough ST7789 to put a frame up: the panel is only ever sent whole
// frames.
namespace tft {

const SPISettings SETTINGS(TFT_SPI_HZ, MSBFIRST, SPI_MODE0);
uint8_t rot = 0xFF;

void command(uint8_t c, const uint8_t* data = nullptr, size_t n = 0) {
  SPI.beginTransaction(SETTINGS);
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, LOW);
  SPI.write(c);
  digitalWrite(TFT_DC, HIGH);
  if (n) SPI.writeBytes(data, n);
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

void command1(uint8_t c, uint8_t d) { command(c, &d, 1); }

// MADCTL for rotations 0-3: the same as Adafruit's ST7789 driver.
void setRotation(uint8_t r) {
  static const uint8_t MADCTL[4] = {0xC0, 0xA0, 0x00, 0x60};
  rot = r & 3;
  command1(0x36, MADCTL[rot]);
}

void begin(uint8_t rotation) {
  pinMode(TFT_DC, OUTPUT);
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(5);
  digitalWrite(TFT_RST, LOW);
  delay(20);
  digitalWrite(TFT_RST, HIGH);
  delay(150);
  command(0x01);  // software reset
  delay(150);
  command(0x11);  // out of sleep
  delay(10);
  command1(0x3A, 0x55);  // 16 bits a pixel
  setRotation(rotation);
  command(0x21);  // inverted, as this panel needs
  command(0x13);  // normal display mode
  delay(10);
  command(0x29);  // on
  delay(10);
}

// Sends a whole width x height frame of host-order RGB565.
void pushFrame(const uint16_t* px, int w, int h) {
  uint8_t ca[4] = {0, 0, (uint8_t)((w - 1) >> 8), (uint8_t)(w - 1)};
  uint8_t ra[4] = {0, 0, (uint8_t)((h - 1) >> 8), (uint8_t)(h - 1)};
  command(0x2A, ca, 4);
  command(0x2B, ra, 4);
  SPI.beginTransaction(SETTINGS);
  digitalWrite(TFT_CS, LOW);
  digitalWrite(TFT_DC, LOW);
  SPI.write(0x2C);
  digitalWrite(TFT_DC, HIGH);
  SPI.writePixels(px, (uint32_t)w * h * 2);  // swaps to the panel's big-endian
  digitalWrite(TFT_CS, HIGH);
  SPI.endTransaction();
}

}  // namespace tft

// Frames are drawn into PSRAM and sent whole, so the screen never shows one
// half drawn. 320x240x2 = 150 KB, allocated in begin(): the canvas object
// exists from static init, when the UI takes a reference to it.
class Canvas : public GFXcanvas16 {
 public:
  Canvas() : GFXcanvas16(PANEL_NATIVE_HEIGHT, PANEL_NATIVE_WIDTH, false) {}
  bool allocate() {
    if (!buffer) buffer = (uint16_t*)heap_caps_calloc((size_t)width() * height(), 2, MALLOC_CAP_SPIRAM);
    buffer_owned = buffer != nullptr;
    return buffer_owned;
  }
};
Canvas canvasObj;
Canvas* const canvas = &canvasObj;

}  // namespace

void begin(uint8_t rotation) {
  // Every device on the shared bus deselected before any of them is spoken to.
  for (int cs : {SD_CS, LORA_CS, TFT_CS}) {
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
  }
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, VEXT_ON);
  delay(20);
  SPI.begin(SPI_SCLK, SPI_MISO, SPI_MOSI);
  tft::begin(rotation);
  if (canvas->allocate()) {
    canvas->fillScreen(color::BG);
    nextPage();
  } else {
    Serial.println("[panel] no memory for the frame buffer");
  }
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BL_ON);
}

Adafruit_GFX& gfx() { return *canvas; }

void startFrame(bool, uint8_t rotation) {
  if (tft::rot != (rotation & 3)) tft::setRotation(rotation);
}

bool nextPage() {
  uint16_t* buf = canvas->getBuffer();
  if (!buf) return false;
  tft::pushFrame(buf, canvas->width(), canvas->height());
  return false;
}

void dump(uint8_t) {
  uint16_t* buf = canvas->getBuffer();
  if (!buf) {
    Serial.println("SHOT none");
    return;
  }
  Serial.printf("SHOT565 %d %d\n", canvas->width(), canvas->height());
  dumpBase64(reinterpret_cast<const uint8_t*>(buf), (size_t)canvas->width() * canvas->height() * 2);
}

}  // namespace panel

#endif

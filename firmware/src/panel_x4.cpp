#include "board.h"
#if DISPLAY_EPAPER

#include <GxEPD2_BW.h>

#include "panel.h"

namespace panel {
namespace {

// GxEPD2 clocks the frame out a byte at a time, which costs ~170 ms per
// 48 KB copy and a refresh makes two or three. Whole-screen writes -- the only
// kind this UI makes -- go out in one burst instead; anything else, and the
// first write after init, falls through to the library.
class X4Panel : public GxEPD2_426_GDEQ0426T82 {
 public:
  static inline const uint8_t* lastFrame = nullptr;  // for dumpScreen()
  using GxEPD2_426_GDEQ0426T82::GxEPD2_426_GDEQ0426T82;
  using Base = GxEPD2_426_GDEQ0426T82;

  void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false,
                  bool mirror_y = false, bool pgm = false) {
    if (!fast(0x24, bitmap, x, y, w, h, invert, mirror_y, pgm)) Base::writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
  void writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h,
                                bool invert = false, bool mirror_y = false, bool pgm = false) {
    if (fast(0x26, bitmap, x, y, w, h, invert, mirror_y, pgm)) fast(0x24, bitmap, x, y, w, h, invert, mirror_y, pgm);
    else Base::writeImageForFullRefresh(bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
  void writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false,
                       bool mirror_y = false, bool pgm = false) {
    if (fast(0x26, bitmap, x, y, w, h, invert, mirror_y, pgm)) fast(0x24, bitmap, x, y, w, h, invert, mirror_y, pgm);
    else Base::writeImageAgain(bitmap, x, y, w, h, invert, mirror_y, pgm);
  }

 private:
  bool fast(uint8_t ram, const uint8_t* bitmap, int16_t x, int16_t y, int16_t w, int16_t h, bool invert,
            bool mirror_y, bool pgm) {
    if (x || y || w != WIDTH || h != HEIGHT || invert || mirror_y || pgm || !_init_display_done || _initial_write)
      return false;
    // The whole RAM, as the driver's _setPartialRamArea(0, 0, WIDTH, HEIGHT) sets it: y counts down.
    _writeCommand(0x11);
    _writeData(0x01);
    _writeCommand(0x44);
    _writeData(0);
    _writeData(0);
    _writeData((WIDTH - 1) % 256);
    _writeData((WIDTH - 1) / 256);
    _writeCommand(0x45);
    _writeData((HEIGHT - 1) % 256);
    _writeData((HEIGHT - 1) / 256);
    _writeData(0);
    _writeData(0);
    _writeCommand(0x4e);
    _writeData(0);
    _writeData(0);
    _writeCommand(0x4f);
    _writeData((HEIGHT - 1) % 256);
    _writeData((HEIGHT - 1) / 256);
    lastFrame = bitmap;
    _writeCommand(ram);
    _startTransfer();
    _pSPIx->writeBytes(bitmap, uint32_t(WIDTH) * HEIGHT / 8);
    _endTransfer();
    return true;
  }
};

using EpdDisplay = GxEPD2_BW<X4Panel, X4Panel::HEIGHT>;
EpdDisplay display(X4Panel(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

}  // namespace

void begin(uint8_t rotation) {
  SPI.begin(EPD_SCLK, SPI_MISO, EPD_MOSI, EPD_CS);
  // The library's default is 4 MHz; the SSD1677 takes writes at up to 20.
  display.init(0, true, 10, false, SPI, SPISettings(20000000, MSBFIRST, SPI_MODE0));
  display.setRotation(rotation);
}

Adafruit_GFX& gfx() { return display; }

void startFrame(bool full, uint8_t rotation) {
  display.setRotation(rotation);
  if (full) display.setFullWindow();
  else display.setPartialWindow(0, 0, display.width(), display.height());
  display.firstPage();
}

bool nextPage() { return display.nextPage(); }

void setBacklight(bool) {}

void dump(uint8_t rotation) {
  const uint8_t* f = X4Panel::lastFrame;
  if (!f) {
    Serial.println("SHOT none");
    return;
  }
  Serial.printf("SHOT %d %d %d\n", X4Panel::WIDTH, X4Panel::HEIGHT, rotation);
  dumpBase64(f, size_t(X4Panel::WIDTH) * X4Panel::HEIGHT / 8);
}

}  // namespace panel

#endif

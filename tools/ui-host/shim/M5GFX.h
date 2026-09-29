// ui-host shim for <M5GFX.h>: the REAL LovyanGFX core (same source the firmware links), but the
// "display" is an in-memory 240x135 16bpp sprite instead of the ST7789 panel. Every firmware draw
// call therefore renders pixel-for-pixel as on the device (same fonts, same rasterizer), and the
// harness dumps the result as a PNG. Only the M5GFX-specific surface the UI code touches is added.
#pragma once
#define LGFX_USE_V1
#include "lgfx/v1/platforms/common.hpp"
#include "lgfx/v1/lgfx_filesystem_support.hpp"
#include "lgfx/v1/LGFXBase.hpp"
#include "lgfx/v1/LGFX_Sprite.hpp"
#include <stdint.h>

namespace m5gfx
{
  using namespace lgfx;
  using board_t = lgfx::boards::board_t;

  class M5GFX : public lgfx::LGFX_Sprite
  {
  public:
    M5GFX() : LGFX_Sprite() {}
    board_t getBoard(void) const { return lgfx::boards::board_M5Cardputer; }
    void setBrightness(uint8_t b) { _brightness = b; }
    uint8_t getBrightness(void) const { return _brightness; }
    void sleep(void) {}
    void wakeup(void) {}
    void display(void) {}
    void waitDisplay(void) {}
    bool init(void) { return true; }
    bool begin(void) { return true; }
  private:
    uint8_t _brightness = 128;
  };

  class M5Canvas : public lgfx::LGFX_Sprite
  {
  public:
    M5Canvas() : LGFX_Sprite() {}
    M5Canvas(LovyanGFX *parent) : LGFX_Sprite(parent) { _psram = true; }
    void *frameBuffer(uint8_t) { return getBuffer(); }
  };
}

using M5GFX = m5gfx::M5GFX;
using M5Canvas = m5gfx::M5Canvas;

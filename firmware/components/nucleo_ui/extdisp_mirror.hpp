// Panel_Mirror — a LovyanGFX panel that TEES every write of the internal display onto an external panel.
//
// Installed in place of the internal ST7789 panel (d.setPanel(&mirror)), so it sees EVERY pixel the OS
// draws: canvas blits, direct draws, games, video, modals — with no app changes and no framebuffer (the
// 240x135 frame lands 1:1, centred, on the 320x240 ILI9341). The internal panel stays the source of truth:
// reads, raw commands, DMA, rotation and invert go to it alone; the external one only ever receives writes.
//
// Bus rule (the reason this is not a plain "forward both"): the external panel shares SPI2 with the SD
// card, and the OS reads the SD INSIDE display transactions (gbemu fetches ROM banks inside one
// startWrite per frame, the NFV video player decodes frames inside its startWrite). Holding SPI2 for the
// whole outer transaction would deadlock those tasks on their own SD read. So the external panel is opened
// and closed around EACH primitive (ext_begin/ext_end) and never stays open between calls; a pixel
// stream split across calls (setWindow + several writePixels/writeBlock) is resumed by re-addressing the
// window from a tracked cursor. The host gate (tools/extdisp-host) asserts the bus is closed after every
// call and that the external frame equals the internal one pixel for pixel.
#pragma once
#define LGFX_USE_V1
#include "lgfx/v1/panel/Panel_Device.hpp"

class Panel_Mirror : public lgfx::Panel_Device
{
public:
    typedef void (*brightness_cb_t)(uint8_t brightness);
    typedef void (*lock_cb_t)(void);

    // primary: the internal panel (already initialised + rotated). ext: the external panel, initialised,
    // landscape, cleared. Neither is owned.
    void attach(lgfx::IPanel *primary, lgfx::IPanel *ext);
    lgfx::IPanel *primary(void) const { return _pri; }
    lgfx::IPanel *external(void) const { return _ext; }
    int origin_x(void) const { return _ox; }
    int origin_y(void) const { return _oy; }
    uint32_t ext_transactions(void) const { return _ext_tx; }   // diagnostics: SPI2 claims made so far
    // Follows setBrightness() for an external backlight on a GPIO (nullptr: LED wired to a supply rail).
    void set_brightness_cb(brightness_cb_t cb) { _bl_cb = cb; }
    // Taken around every external primitive, so another task (the touch layer drawing its keys in the
    // letterbox) can hold the external panel between two of the mirror's primitives, never inside one.
    void set_ext_lock(lock_cb_t lock, lock_cb_t unlock) { _lock = lock; _unlock = unlock; }

    bool init(bool) override { return true; }
    void initBus(void) override {}
    void releaseBus(void) override {}
    bool initTouch(void) override { return false; }

    void beginTransaction(void) override;
    void endTransaction(void) override;

    void setBrightness(uint8_t brightness) override;
    lgfx::color_depth_t setColorDepth(lgfx::color_depth_t depth) override;
    void setInvert(bool invert) override;
    void setRotation(uint_fast8_t r) override;
    void setSleep(bool flg_sleep) override;
    void setPowerSave(bool flg_idle) override;

    void writeCommand(uint32_t cmd, uint_fast8_t length) override;
    void writeData(uint32_t data, uint_fast8_t length) override;

    void initDMA(void) override;
    void waitDMA(void) override;
    bool dmaBusy(void) override;
    void waitDisplay(void) override;
    bool displayBusy(void) override;
    void display(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h) override;
    bool isReadable(void) const override;
    bool isBusShared(void) const override { return true; }   // lgfx file draws then release around SD reads

    void writeBlock(uint32_t rawcolor, uint32_t len) override;
    void setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe, uint_fast16_t ye) override;
    void drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y, uint32_t rawcolor) override;
    void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor) override;
    void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, lgfx::pixelcopy_t *param, bool use_dma) override;
    void writeImageARGB(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, lgfx::pixelcopy_t *param) override;
    void writePixels(lgfx::pixelcopy_t *param, uint32_t len, bool use_dma) override;

    uint32_t readCommand(uint_fast16_t cmd, uint_fast8_t index, uint_fast8_t length) override;
    uint32_t readData(uint_fast8_t index, uint_fast8_t length) override;
    void readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void *dst, lgfx::pixelcopy_t *param) override;
    void copyRect(uint_fast16_t dst_x, uint_fast16_t dst_y, uint_fast16_t w, uint_fast16_t h, uint_fast16_t src_x, uint_fast16_t src_y) override;
    int32_t getScanLine(void) override;

protected:
    // One external transaction per primitive. Virtual so the host gate can count open/close.
    virtual void ext_begin(void) { if (_lock) _lock(); _ext_tx++; _ext->startWrite(); }
    virtual void ext_end(void)   { _ext->endWrite(); if (_unlock) _unlock(); }

private:
    lgfx::IPanel *_pri = nullptr;
    lgfx::IPanel *_ext = nullptr;
    brightness_cb_t _bl_cb = nullptr;
    lock_cb_t _lock = nullptr, _unlock = nullptr;
    int _ox = 0, _oy = 0;
    uint32_t _ext_tx = 0;
    // The open pixel stream, in internal-panel coordinates: window + where the next pixel goes.
    int _wx0 = 0, _wy0 = 0, _wx1 = -1, _wy1 = -1, _cx = 0, _cy = 0;

    void sync_dims(void);
    template <typename F> void ext_stream(uint32_t len, F &&write_n);
    void mirror_from_primary(int x, int y, int w, int h);
};

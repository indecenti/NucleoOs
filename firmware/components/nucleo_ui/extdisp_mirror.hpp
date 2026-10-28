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
//
// Frame hold (set_hold): opening the bus per primitive costs a lock + a bus acquire + CS for every pixel of
// an outline. With a hold, the external transaction stays open from the first primitive of a frame (an
// outer startWrite) to its endWrite, bounded by a time budget, and is given back early through
// release_hold() — which the SD driver calls before EVERY card transfer (nucleo_storage's hook): a task that
// reads the card mid-frame (a game fetching ROM banks, a decoder) releases its own hold first, so the
// "never deadlock on our own SD read" rule above still holds; another task waits at most the budget.
//
// Fit (set_fit): the compact frame is scaled 4/3 on the external panel (240x135 -> 320x180, nearest pixel,
// centred) instead of 1:1, so every compact screen of the OS fills the panel's width. Opaque images and pixel
// streams are scaled straight from their source pixels; transparent / affine images, alpha blends and scrolls
// are read back from the internal panel (the source of truth) and scaled from there.
//
// Solo mode (set_solo): the external panel becomes THE display. Every write goes to it alone, unshifted, and
// the mirror reports the external size (320x240), so whatever draws to `d` draws natively on it. The internal
// panel is left alone (the launcher draws its companion there). The external glass is write-only, so in solo
// isReadable() is false, readRect leaves the destination untouched and copyRect (scroll) is a no-op: a screen
// drawn in solo must not rely on reading the panel back. Switch only between frames (no open transaction).
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
    // Solo on/off. False (and nothing changes) while a transaction is open on the internal panel.
    bool set_solo(bool on);
    bool solo(void) const { return _solo; }
    // Solo only: the top `top` rows of the external panel are the system's (the store-app bar). The surface
    // becomes ext width x (ext height - top) and everything drawn lands below the band. 0 = the whole glass.
    bool set_inset(int top);
    // Display modes. ext_off: the external panel gets nothing (Cardputer only; solo is refused meanwhile).
    // pri_off: the internal backlight stays at 0 (second screen only) — the internal panel is still drawn,
    // it remains the source of truth for readbacks and screenshots. Between frames only, like solo.
    bool set_ext_off(bool on);
    bool ext_off(void) const { return _ext_off; }
    // Screen off with a backlight that cannot be switched (on the 5 V rail): the panel was painted black and
    // nothing may reach it until the screen wakes (an app still animating would light it back up).
    void set_dark(bool on) { _dark = on; }
    void set_pri_off(bool on);
    bool pri_off(void) const { return _pri_off; }
    // Fit on/off (mirror mode). Same rule as solo: false while a transaction is open on the internal panel.
    bool set_fit(bool on);
    bool fit(void) const { return _fit; }
    // The compact frame's size ON the external panel (240x135, or 320x180 when fitted).
    int frame_w(void) const { return sx(_pri->width()); }
    int frame_h(void) const { return sy(_pri->height()); }
    // Screenshots. One internal row in the panel's raw write format (the internal glass, even in solo).
    void pri_row(int y, uint16_t *out);
    // One row of the external panel, REBUILT from the internal one (the external glass is write-only): the
    // frame exactly as it was written (1:1 or fitted), the letterbox black. `tmp` holds one internal row.
    // False in solo, where the panel shows content of its own that exists nowhere else.
    bool ext_row(int y, uint16_t *out, uint16_t *tmp);
    int origin_x(void) const { return _ox; }
    int origin_y(void) const { return _oy; }
    uint32_t ext_transactions(void) const { return _ext_tx; }   // diagnostics: SPI2 claims made so far
    // Follows setBrightness() for an external backlight on a GPIO (nullptr: LED wired to a supply rail).
    void set_brightness_cb(brightness_cb_t cb) { _bl_cb = cb; }
    // Taken around every external primitive, so another task (the touch layer drawing its keys in the
    // letterbox) can hold the external panel between two of the mirror's primitives, never inside one.
    void set_ext_lock(lock_cb_t lock, lock_cb_t unlock) { _lock = lock; _unlock = unlock; }
    // Frame hold: now_ms (a monotonic clock), self (the calling task's identity) and the budget in ms.
    // Not set = no hold (one external transaction per primitive).
    typedef uint32_t (*now_ms_cb_t)(void);
    typedef void *(*self_cb_t)(void);
    void set_hold(now_ms_cb_t now_ms, self_cb_t self, uint32_t budget_ms) { _now = now_ms; _self = self; _budget = budget_ms; }
    // Off while ANOTHER task reads the card (the video reader on the other core): the bus is then handed back
    // after every primitive, so its transfers slot in between them. Call from the drawing task.
    void hold_enable(bool on) { _hold_off = !on; if (!on) release_hold(); }
    // Give a held external transaction back now — only when the caller is the task holding it (else no-op).
    void release_hold(void);

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
    // The external transaction around one primitive: a real open/close (bus_open/bus_close), unless a frame
    // hold keeps it open. bus_* are virtual so the host gate can count the real bus traffic.
    void ext_begin(void);
    void ext_end(void);
    virtual void bus_open(void)  { if (_lock) _lock(); _ext_tx++; _ext->startWrite(); }
    virtual void bus_close(void) { _ext->endWrite(); if (_unlock) _unlock(); }

private:
    lgfx::IPanel *_pri = nullptr;
    lgfx::IPanel *_ext = nullptr;
    brightness_cb_t _bl_cb = nullptr;
    lock_cb_t _lock = nullptr, _unlock = nullptr;
    int _ox = 0, _oy = 0;
    int _inset = 0;                                // solo: rows reserved at the top (set_inset)
    uint32_t _ext_tx = 0;
    bool _solo = false;
    bool _fit = false;
    bool _ext_off = false, _pri_off = false, _dark = false;
    uint8_t pri_level(uint8_t b) const { return _pri_off ? 0 : b; }
    // Fit scratch rows (raw panel pixels): one source row, one scaled row. Lives in the heap-allocated mirror,
    // so a Cardputer with no external panel never pays for it; the callers' stacks stay untouched.
    uint16_t _row[240];
    uint16_t _out[320];
    int sx(int x) const { return _fit && !_solo ? x * 4 / 3 : x; }   // internal -> external, before the origin
    int sy(int y) const { return _fit && !_solo ? y * 4 / 3 : y; }
    bool fitting(void) const { return _fit && !_solo; }
    void ext_fill(int x, int y, int w, int h, uint32_t rawcolor);   // a scaled rect (one open ext transaction)
    void emit_row(int x, int y, int n);                             // _row[0..n) of internal row y, scaled
    int _pri_open = 0;                 // nesting of beginTransaction on the internal panel (mirror mode)
    int _frame = 0;                    // an outer transaction is open (any mode): the hold may span it
    bool _held = false;                // the external transaction is being held open across primitives
    void *_owner = nullptr;            // ...by this task
    uint32_t _held_t0 = 0, _budget = 0;
    bool _hold_off = false;
    now_ms_cb_t _now = nullptr;
    self_cb_t _self = nullptr;
    void drop_hold(void);
    uint8_t _bri = 255;                // the brightness the OS asked for (the internal panel follows it, or 0 when pri_off)
    // The open pixel stream, in internal-panel coordinates: window + where the next pixel goes.
    int _wx0 = 0, _wy0 = 0, _wx1 = -1, _wy1 = -1, _cx = 0, _cy = 0;

    void sync_dims(void);
    template <typename F> void ext_stream(uint32_t len, F &&write_n);
    template <typename F> void fit_stream(uint32_t len, F &&row_run);   // fit: the stream as (x, y, n) row runs
    void mirror_from_primary(int x, int y, int w, int h);
};

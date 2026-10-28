// extdisp host gate (npm run extdisp:test).
//
// 1. The pure probe logic (extdisp_probe.c): pin validation, the XPT2046 fingerprint, the attach decision.
// 2. Panel_Mirror (extdisp_mirror.cpp) on the REAL LovyanGFX core: an LGFX_Device draws through the
//    mirror onto two in-memory panels (240x135 "internal", 320x240 "external"). Asserts that
//      - the internal frame is byte-identical to the same drawing done without the mirror (transparent),
//      - the external frame's centred window equals the internal frame pixel for pixel,
//      - nothing is written outside that window,
//      - the external bus is CLOSED after every single call, even inside an open startWrite (the
//        property that keeps SD reads inside display transactions from deadlocking on the shared SPI2).
// Writes build/extdisp-host/{internal,external}.rgb for the PNG step in run.mjs.
#define LGFX_USE_V1
#include "lgfx/v1/platforms/common.hpp"
#include "lgfx/v1/LGFXBase.hpp"
#include "lgfx/v1/LGFX_Sprite.hpp"
#include "extdisp_probe.h"
#include "extdisp_mirror.hpp"
#include "extdisp_touch.h"
#include "extdisp_chrome.hpp"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <vector>

using namespace lgfx;

static int s_fail = 0, s_pass = 0;
#define CHECK(cond, ...) do { if (cond) s_pass++; else { s_fail++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- 1. pure logic -------------------------------------------------------------------------------------
static void frame_of(int v, uint8_t rx[3]) { unsigned s = (unsigned)v << 3; rx[0] = 0; rx[1] = (uint8_t)(s >> 8); rx[2] = (uint8_t)s; }

static void test_probe_logic(void)
{
    uint8_t rx[3];
    frame_of(980, rx);  CHECK(xpt_sample(rx) == 980, "sample roundtrip");
    frame_of(4095, rx); CHECK(xpt_sample(rx) == 4095, "sample max");
    { uint8_t ff[3] = { 0xFF, 0xFF, 0xFF }; CHECK(xpt_sample(ff) == -1, "pulled-up MISO is not a frame"); }
    { uint8_t bad[3] = { 0, 0x12, 0x35 }; CHECK(xpt_sample(bad) == -1, "trailing bits set is not a frame"); }
    { uint8_t z[3] = { 0, 0, 0 }; CHECK(xpt_sample(z) == 0, "zeros decode to 0"); }

    CHECK(xpt_fingerprint(980, 1190), "2.5V internal reference");
    CHECK(xpt_fingerprint(745, 920), "3.3V reference");
    CHECK(!xpt_fingerprint(0, 0), "floating-low bus");
    CHECK(!xpt_fingerprint(4095, 4095), "pulled-up bus");
    CHECK(!xpt_fingerprint(-1, -1), "invalid frames");
    CHECK(!xpt_fingerprint(980, 980), "no diode delta");
    CHECK(!xpt_fingerprint(1190, 980), "reversed delta");

    extdisp_cfg_t c; extdisp_cfg_default(&c);
    CHECK(extdisp_cfg_check(&c) == NULL, "defaults valid");
    CHECK(c.cs == 5 && c.dc == 15 && c.rst == 13 && c.t_cs == 6 && c.t_irq == 4 && c.bl == -1, "documented default wiring");
    { extdisp_cfg_t b = c; b.cs = 12; CHECK(extdisp_cfg_check(&b) != NULL, "SD chip-select refused"); }
    { extdisp_cfg_t b = c; b.dc = 9;  CHECK(extdisp_cfg_check(&b) != NULL, "I2C SCL refused"); }
    { extdisp_cfg_t b = c; b.t_cs = 40; CHECK(extdisp_cfg_check(&b) != NULL, "SPI clock refused"); }
    { extdisp_cfg_t b = c; b.bl = 5; CHECK(extdisp_cfg_check(&b) != NULL, "duplicate pin refused"); }
    { extdisp_cfg_t b = c; b.rst = -1; b.t_irq = -1; b.bl = 3; CHECK(extdisp_cfg_check(&b) == NULL, "optional -1 + LED on G3 ok"); }
    { extdisp_cfg_t b = c; b.cs = -1; CHECK(extdisp_cfg_check(&b) != NULL, "cs required"); }
    { extdisp_cfg_t b = c; b.rotation = 2; CHECK(extdisp_cfg_check(&b) != NULL, "portrait refused"); }
    { extdisp_cfg_t b = c; b.mhz = 80; CHECK(extdisp_cfg_check(&b) == NULL, "80 MHz accepted (write-only panel)"); }
    { extdisp_cfg_t b = c; b.mhz = 81; CHECK(extdisp_cfg_check(&b) != NULL, "81 MHz refused"); }

    CHECK(extdisp_mode_parse("off") == EXTDISP_OFF && extdisp_mode_parse("on") == EXTDISP_ON, "mode parse");
    CHECK(extdisp_mode_parse(NULL) == EXTDISP_AUTO && extdisp_mode_parse("x") == EXTDISP_AUTO, "mode default auto");

    extdisp_probe_t p = { true, false, false, true };
    extdisp_decision_t d;
    d = extdisp_decide(&c, &p); CHECK(d.attach && d.touch, "auto + fingerprint attaches");
    { extdisp_probe_t q = p; q.touch_found = false; d = extdisp_decide(&c, &q); CHECK(!d.attach, "auto without fingerprint stays off"); }
    { extdisp_probe_t q = p; q.t_cs_busy = true; d = extdisp_decide(&c, &q); CHECK(!d.attach, "auto backs off a driven t_cs"); }
    { extdisp_probe_t q = p; q.dc_busy = true; d = extdisp_decide(&c, &q); CHECK(!d.attach, "auto backs off a driven dc"); }
    { extdisp_probe_t q = p; q.is_adv = false; extdisp_cfg_t on = c; on.mode = EXTDISP_ON;
      d = extdisp_decide(&on, &q); CHECK(!d.attach, "original Cardputer refused even when forced on"); }
    { extdisp_probe_t q = p; q.touch_found = false; extdisp_cfg_t on = c; on.mode = EXTDISP_ON;
      d = extdisp_decide(&on, &q); CHECK(d.attach && !d.touch, "forced on without touch attaches"); }
    { extdisp_cfg_t off = c; off.mode = EXTDISP_OFF; d = extdisp_decide(&off, &p); CHECK(!d.attach, "off never attaches"); }
    { extdisp_cfg_t on = c; on.mode = EXTDISP_ON; on.cs = 12; d = extdisp_decide(&on, &p); CHECK(!d.attach, "forced on still refuses bad pins"); }
    { extdisp_cfg_t a = c; a.t_cs = -1; d = extdisp_decide(&a, &p); CHECK(!d.attach, "auto without t_cs cannot detect"); }

    // Controller identity (V1.1 / V1.2 boards ship with either chip).
    {
        const uint8_t ili_d3[4] = { 0x00, 0x00, 0x93, 0x41 }, ili_04[4] = { 0, 0, 0, 0 };
        CHECK(extdisp_identify(ili_d3, ili_04) == EXTDISP_CTRL_ILI9341, "ILI9341 Read ID4");
        // ST7789V RDDID 85 85 52 after one dummy bit: 0 1000010 1 1000010 1 0101001 0 ...
        uint32_t v = (0x858552u << 7);
        const uint8_t st_04[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
        const uint8_t junk_d3[4] = { 0x12, 0x34, 0x56, 0x78 };
        CHECK(extdisp_identify(junk_d3, st_04) == EXTDISP_CTRL_ST7789, "ST7789V RDDID with dummy bit");
        const uint8_t ff[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, zz[4] = { 0, 0, 0, 0 };
        CHECK(extdisp_identify(ff, ff) == EXTDISP_CTRL_AUTO, "SDO unwired (pulled up) -> unknown");
        CHECK(extdisp_identify(zz, zz) == EXTDISP_CTRL_AUTO, "SDO unwired (low) -> unknown");
    }
    CHECK(extdisp_ctrl_parse("st7789") == EXTDISP_CTRL_ST7789 && extdisp_ctrl_parse("ili9341") == EXTDISP_CTRL_ILI9341
          && extdisp_ctrl_parse("x") == EXTDISP_CTRL_AUTO, "controller parse");
    {
        bool inv, rgb;
        extdisp_panel_flags(&c, EXTDISP_CTRL_ILI9341, &inv, &rgb); CHECK(!inv && !rgb, "ILI9341 defaults");
        extdisp_panel_flags(&c, EXTDISP_CTRL_ST7789, &inv, &rgb);  CHECK(inv && !rgb, "ST7789 defaults");
        extdisp_cfg_t f = c; f.invert = 0; f.rgb = 1;
        extdisp_panel_flags(&f, EXTDISP_CTRL_ST7789, &inv, &rgb);  CHECK(!inv && rgb, "forced flags win");
    }
    {
        int sx, sy;
        extdisp_touch_map(200, 200, 1, &sx, &sy);   CHECK(sx == 0 && sy == 0, "touch map corner");
        extdisp_touch_map(3900, 3900, 1, &sx, &sy); CHECK(sx == 319 && sy == 239, "touch map far corner");
        extdisp_touch_map(200, 200, 3, &sx, &sy);   CHECK(sx == 319 && sy == 239, "touch map rotation 3");
        extdisp_touch_map(-50, 9999, 1, &sx, &sy);  CHECK(sx == 319 && sy == 0, "touch map clamps");
        extdisp_cfg_t m = c; m.mirror = true;
        CHECK(extdisp_lgfx_rotation(&c) == 1 && extdisp_lgfx_rotation(&m) == 5, "mirror = lgfx flip flag");
        m.rotation = 3; CHECK(extdisp_lgfx_rotation(&m) == 7, "mirror keeps rotation 3");
        extdisp_touch_map(200, 200, 5, &sx, &sy);   CHECK(sx == 319 && sy == 0, "touch map follows the mirror");
        extdisp_touch_map(200, 200, 7, &sx, &sy);   CHECK(sx == 0 && sy == 239, "touch map mirror + rotation 3");
        CHECK(xpt_pressure(0, 4095) == 0, "no touch = 0 pressure");
        CHECK(xpt_pressure(900, 2400) > XPT_Z_TOUCH, "press above threshold");
    }
    CHECK(c.controller == EXTDISP_CTRL_AUTO && !c.mirror && c.invert == -1 && c.rgb == -1 && c.diag, "new defaults");

    int ox, oy;
    extdisp_mirror_origin(320, 240, 240, 135, &ox, &oy); CHECK(ox == 40 && oy == 52, "origin 240x135 -> 40,52");
    extdisp_mirror_origin(320, 240, 240, 130, &ox, &oy); CHECK(ox == 40 && oy == 55, "origin 240x130 -> 40,55");
}

// ---- 3. touch (extdisp_touch.c): calibration, soft keys, gestures, the calibration flow ----------------
static int feed(extdisp_gesture_t *g, uint32_t t, bool on, int x, int y, int *ev)
{
    return extdisp_gesture_feed(g, t, on, x, y, ev);
}

static void test_touch_logic(void)
{
    // The default calibration reproduces the phase-1 approximate map (every rotation / mirror).
    for (int rot : { 1, 3, 5, 7 }) {
        extdisp_cal_t k; extdisp_cal_default(rot, &k);
        int worst = 0;
        for (int rx = 400; rx <= 3700; rx += 330)
            for (int ry = 400; ry <= 3700; ry += 330) {
                int ax, ay, bx, by;
                extdisp_touch_map(rx, ry, rot, &ax, &ay);
                extdisp_cal_apply(&k, rx, ry, 320, 240, &bx, &by);
                int d = abs(ax - bx) > abs(ay - by) ? abs(ax - bx) : abs(ay - by);
                if (d > worst) worst = d;
            }
        CHECK(worst <= 1, "default calibration == approximate map (rotation %d, off by %d px)", rot, worst);
    }
    // An exact solve recovers a skewed, swapped, offset map; collinear points are refused.
    {
        auto truth = [](int rx, int ry, int *sx, int *sy) { *sx = (ry * 9) / 100 - 25 + rx / 200; *sy = 250 - (rx * 6) / 100; };
        int raw[3][2] = { { 300, 300 }, { 3800, 400 }, { 500, 3700 } }, scr[3][2];
        for (int i = 0; i < 3; i++) truth(raw[i][0], raw[i][1], &scr[i][0], &scr[i][1]);
        extdisp_cal_t k;
        CHECK(extdisp_cal_solve(raw, scr, &k), "solve");
        int probe[2] = { 2000, 2000 }, want[2];
        truth(2000, 2000, &want[0], &want[1]);
        CHECK(extdisp_cal_error(&k, probe, want) <= 2, "solve generalises (off by %d)", extdisp_cal_error(&k, probe, want));
        int col[3][2] = { { 100, 100 }, { 2000, 2000 }, { 4000, 4000 } };
        CHECK(!extdisp_cal_solve(col, scr, &k), "collinear points refused");
        int packed[7]; extdisp_cal_pack(5, &k, packed);
        extdisp_cal_t back;
        CHECK(extdisp_cal_unpack(packed, 5, &back) && back.c == k.c && back.f == k.f, "pack/unpack roundtrip");
        CHECK(!extdisp_cal_unpack(packed, 1, &back), "a calibration from another rotation is ignored");
        int zero[7] = { 5, 0, 0, 7, 0, 0, 9 };
        CHECK(!extdisp_cal_unpack(zero, 5, &back), "a zeroed calibration is ignored");
    }

    // Soft-key layout around the 240x135 frame at 40,52 on 320x240.
    extdisp_rect_t r[SK_COUNT];
    extdisp_sk_layout(320, 240, 40, 52, 240, 135, r);
    CHECK(r[SK_ESC].x == 0 && r[SK_ESC].w == 80 && r[SK_ESC].h == 52, "ESC top-left");
    CHECK(r[SK_MENU].x == 240 && r[SK_MENU].w == 80, "MENU top-right");
    CHECK(r[SK_LEFT].y == 187 && r[SK_LEFT].h == 53 && r[SK_RIGHT].x + r[SK_RIGHT].w == 320, "bottom row spans the band");
    CHECK(extdisp_sk_hit(r, 5, 5) == SK_ESC && extdisp_sk_hit(r, 315, 10) == SK_MENU && extdisp_sk_hit(r, 160, 20) == SK_STATUS, "top hits");
    CHECK(extdisp_sk_hit(r, 160, 220) == SK_OK && extdisp_sk_hit(r, 10, 200) == SK_LEFT && extdisp_sk_hit(r, 300, 239) == SK_RIGHT, "bottom hits");
    CHECK(extdisp_sk_hit(r, 160, 120) == SK_FRAME && extdisp_sk_hit(r, 5, 120) == SK_FRAME, "frame and side strips = gesture area");
    {
        extdisp_rect_t t[SK_COUNT];
        extdisp_sk_layout(320, 240, 0, 10, 320, 220, t);
        CHECK(t[SK_ESC].w == 0 && t[SK_OK].w == 0 && extdisp_sk_hit(t, 5, 5) == SK_FRAME, "thin bands get no keys");
    }

    // Gestures.
    extdisp_gesture_t g; int ev[EXT_MAX_EVENTS], n;
    extdisp_gesture_init(&g, r);
    n = feed(&g, 0, true, 160, 120, ev);   CHECK(n == 0, "frame press: nothing yet");
    n = feed(&g, 40, true, 163, 118, ev);  CHECK(n == 0, "small wobble stays a tap");
    n = feed(&g, 80, false, 0, 0, ev);     CHECK(n == 0 && g.down, "one flicker sample is not a release");
    n = feed(&g, 100, true, 162, 119, ev); CHECK(n == 0, "contact resumes");
    feed(&g, 140, false, 0, 0, ev);
    n = feed(&g, 180, false, 0, 0, ev);    CHECK(n == 1 && ev[0] == EXT_EV_ENTER && !g.down, "tap = Enter on release");

    extdisp_gesture_init(&g, r);
    feed(&g, 0, true, 160, 150, ev);
    n = feed(&g, 20, true, 160, 120, ev);  CHECK(n == 1 && ev[0] == EXT_EV_DOWN, "drag up 30 px = Down");
    n = feed(&g, 40, true, 160, 90, ev);   CHECK(n == 1 && ev[0] == EXT_EV_DOWN, "another step");
    n = feed(&g, 60, true, 200, 92, ev);   CHECK(n == 1 && ev[0] == EXT_EV_LEFT, "drag right = Left");
    feed(&g, 80, false, 0, 0, ev); n = feed(&g, 100, false, 0, 0, ev);
    CHECK(n == 0, "a drag does not end in Enter");

    extdisp_gesture_init(&g, r);
    feed(&g, 0, true, 160, 180, ev);
    n = feed(&g, 50, true, 160, 60, ev);   CHECK(n == EXT_MAX_EVENTS, "a fling is capped at %d events", EXT_MAX_EVENTS);

    extdisp_gesture_init(&g, r);
    feed(&g, 0, true, 100, 100, ev);
    n = feed(&g, EXT_LONG_MS, true, 101, 101, ev); CHECK(n == 1 && ev[0] == EXT_EV_BACK, "long press = Back");
    n = feed(&g, EXT_LONG_MS + 300, true, 101, 101, ev); CHECK(n == 0, "...once");
    feed(&g, 2000, false, 0, 0, ev); n = feed(&g, 2020, false, 0, 0, ev);
    CHECK(n == 0, "no Enter after a long press");

    extdisp_gesture_init(&g, r);
    n = feed(&g, 0, true, 100, 210, ev);   CHECK(n == 1 && ev[0] == EXT_EV_UP && g.pressed == SK_UP, "arrow fires on press, key lit");
    n = feed(&g, 200, true, 100, 210, ev); CHECK(n == 0, "no repeat before the delay");
    n = feed(&g, EXT_REPEAT_DELAY, true, 100, 210, ev); CHECK(n == 1 && ev[0] == EXT_EV_UP, "auto-repeat");
    n = feed(&g, EXT_REPEAT_DELAY + EXT_REPEAT_EVERY, true, 100, 210, ev); CHECK(n == 1, "repeat rate");
    feed(&g, 1000, false, 0, 0, ev); n = feed(&g, 1020, false, 0, 0, ev);
    CHECK(n == 0 && g.pressed == SK_NONE, "arrow release adds nothing");

    extdisp_gesture_init(&g, r);
    n = feed(&g, 0, true, 160, 210, ev);   CHECK(n == 0 && g.pressed == SK_OK, "OK lights on press");
    feed(&g, 40, false, 0, 0, ev); n = feed(&g, 60, false, 0, 0, ev);
    CHECK(n == 1 && ev[0] == EXT_EV_ENTER, "OK = Enter on release");
    extdisp_gesture_init(&g, r);
    feed(&g, 0, true, 10, 10, ev); feed(&g, 40, false, 0, 0, ev); n = feed(&g, 60, false, 0, 0, ev);
    CHECK(n == 1 && ev[0] == EXT_EV_BACK, "ESC = Back");
    extdisp_gesture_init(&g, r);
    feed(&g, 0, true, 300, 10, ev); feed(&g, 40, false, 0, 0, ev); n = feed(&g, 60, false, 0, 0, ev);
    CHECK(n == 1 && ev[0] == EXT_EV_MENU, "MENU = Control Center");
    extdisp_gesture_init(&g, r);
    feed(&g, 0, true, 160, 210, ev);
    feed(&g, 40, true, 160, 120, ev);      CHECK(g.pressed == SK_NONE, "slide off OK unlights it");
    feed(&g, 60, true, 160, 210, ev);      CHECK(g.pressed == SK_NONE, "...for good");
    feed(&g, 80, false, 0, 0, ev); n = feed(&g, 100, false, 0, 0, ev);
    CHECK(n == 0, "slide off = cancelled");
    extdisp_gesture_init(&g, r);
    feed(&g, 0, true, 160, 20, ev);
    n = feed(&g, EXT_CAL_HOLD_MS - 40, true, 160, 20, ev); CHECK(n == 0, "status: nothing before 3 s");
    n = feed(&g, EXT_CAL_HOLD_MS, true, 160, 20, ev);      CHECK(n == 1 && ev[0] == EXT_EV_CALIBRATE, "status hold = recalibrate");
    feed(&g, 4000, false, 0, 0, ev); n = feed(&g, 4020, false, 0, 0, ev);
    CHECK(n == 0, "status release adds nothing");

    // The calibration flow, on a panel whose raw axes are swapped and mirrored.
    auto raw_of = [](int sx, int sy, int *rx, int *ry) { *rx = 3900 - sy * 15; *ry = 250 + sx * 11; };
    int tg[4][2]; extdisp_cal_targets(320, 240, tg);
    CHECK(tg[0][0] == 20 && tg[2][0] == 299 && tg[2][1] == 219, "targets 20 px in");
    CHECK(tg[0][1] < 52 && tg[2][1] >= 187, "targets sit in the letterbox");
    extdisp_calib_t c; extdisp_calib_start(&c);
    uint32_t t = 0; int res = CAL_BUSY;
    CHECK(extdisp_calib_feed(&c, t, true, 100, 100) == CAL_BUSY && c.step == 0, "the finger that asked must lift first");
    for (int k = 0; k < EXT_RELEASE_N; k++) extdisp_calib_feed(&c, t += 20, false, 0, 0);
    for (int i = 0; i < 4; i++) {
        int rx, ry; raw_of(tg[i][0], tg[i][1], &rx, &ry);
        for (int s = 0; s < 20 && c.step == i; s++) res = extdisp_calib_feed(&c, t += 20, true, rx + (s & 1) * 6, ry - (s & 1) * 6);
        CHECK(c.step == i + 1, "target %d captured", i + 1);
        for (int s = 0; s < 3; s++) extdisp_calib_feed(&c, t += 20, true, 9, 9);   // still held: ignored
        for (int k = 0; k < EXT_RELEASE_N; k++) extdisp_calib_feed(&c, t += 20, false, 0, 0);
    }
    CHECK(res == CAL_DONE, "four targets = done");
    extdisp_cal_t k; int err = -1;
    CHECK(extdisp_calib_finish(&c, 320, 240, &k, &err) && err <= 3, "calibration accepted (check off by %d px)", err);
    { int rx, ry, sx, sy; raw_of(160, 120, &rx, &ry); extdisp_cal_apply(&k, rx, ry, 320, 240, &sx, &sy);
      CHECK(abs(sx - 160) <= 3 && abs(sy - 120) <= 3, "centre lands at %d,%d", sx, sy); }
    extdisp_calib_t bad = c; bad.raw[3][0] += 900;
    CHECK(!extdisp_calib_finish(&bad, 320, 240, &k, &err) && err > EXT_CAL_MAX_ERR, "a sloppy 4th tap is rejected (%d px)", err);
}

// ---- 2. the mirror on the real LovyanGFX core ---------------------------------------------------------
struct PanelSprite : public LGFX_Sprite { IPanel *panel(void) { return _panel; } };

// Counts external transactions; `open` must be 0 whenever control is back in the caller.
struct SpyMirror : public Panel_Mirror {
    int open = 0, opened = 0, max_open = 0;
    void bus_open(void) override { open++; opened++; if (open > max_open) max_open = open; Panel_Mirror::bus_open(); }
    void bus_close(void) override { open--; Panel_Mirror::bus_close(); }
};

static SpyMirror *s_spy;
static PanelSprite *s_inner, *s_outer;
static int s_ops = 0, s_leaks = 0, s_diverged = 0, s_first_diverged = -1;
// After EVERY call: the bus is closed and the external window equals the internal frame. Checking only
// at the end is not enough — a later readback-mirrored op (scroll) can repaint over an earlier error.
static void after_op(void)
{
    s_ops++;
    if (!s_spy) return;
    if (s_spy->open != 0) s_leaks++;
    for (int y = 0; y < 135; y++)
        for (int x = 0; x < 240; x++)
            if (s_inner->readPixel(x, y) != s_outer->readPixel(x + 40, y + 52)) {
                s_diverged++;
                if (s_first_diverged < 0) s_first_diverged = s_ops;
                return;
            }
}

static const uint16_t SENTINEL = 0x1234;

// The drawing script: every primitive family the OS uses on the internal panel. Run once on a plain
// sprite (reference) and once through the mirror; after_op() runs after every call.
static void draw_script(LovyanGFX &g)
{
    g.fillScreen(TFT_NAVY); after_op();
    g.setTextColor(TFT_WHITE, TFT_NAVY); after_op();
    g.drawString("NucleoOS external", 6, 4, &lgfx::fonts::Font2); after_op();
    g.setTextSize(2); g.setCursor(6, 24); g.print("240x135 1:1"); after_op(); g.setTextSize(1);
    g.drawLine(0, 0, 239, 134, TFT_YELLOW); after_op();
    g.drawCircle(200, 30, 20, TFT_GREEN); after_op();
    g.fillTriangle(150, 110, 180, 60, 210, 110, TFT_ORANGE); after_op();
    g.fillRoundRect(8, 48, 60, 24, 6, TFT_MAROON); after_op();
    g.drawFastHLine(0, 76, 240, TFT_CYAN); after_op();
    g.fillSmoothCircle(110, 60, 14, TFT_MAGENTA); after_op();          // alpha edges: readback + rewrite
    g.fillSmoothRoundRect(70, 84, 70, 16, 6, TFT_SKYBLUE); after_op();
    for (int i = 0; i < 40; i++) { g.drawPixel(220 + (i % 8), 90 + i / 8, (uint16_t)(i * 1597)); after_op(); }

    // An 8bpp canvas (the OS's shared back-buffer format) pushed with a clip band inside ONE startWrite,
    // like blit_dirty_bands: the external bus must still close after each push.
    LGFX_Sprite cv(&g);
    cv.setColorDepth(8);
    cv.createSprite(120, 60);
    cv.fillSprite(TFT_DARKGREEN);
    cv.drawString("canvas 8bpp", 4, 4, &lgfx::fonts::Font2);
    cv.fillCircle(90, 40, 15, TFT_RED);
    g.startWrite(); after_op();
    g.setClipRect(0, 100, 240, 20); cv.pushSprite(&g, 60, 90); after_op();
    g.setClipRect(0, 120, 240, 15); cv.pushSprite(&g, 60, 90); after_op();
    g.clearClipRect();
    g.endWrite(); after_op();
    cv.deleteSprite();

    // pushImage, partly off-screen (clipped).
    std::vector<uint16_t> img(37 * 23);
    for (size_t i = 0; i < img.size(); i++) img[i] = (uint16_t)(i * 2654435761u >> 16);
    g.pushImage(215, 118, 37, 23, img.data()); after_op();
    g.pushImage(-10, -5, 37, 23, img.data(), (uint16_t)img[0]); after_op();   // with a transparent colour

    // A pixel stream split across calls at odd sizes: resumes mid-row on the external panel.
    std::vector<uint16_t> px(50 * 30);
    for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(0xF800 * ((i / 7) & 1) | (i & 0x3F));
    g.startWrite(); after_op();
    g.setAddrWindow(150, 4, 50, 30); after_op();
    for (size_t off = 0; off < px.size(); off += 37) {
        size_t n = px.size() - off < 37 ? px.size() - off : 37;
        g.pushPixels(px.data() + off, (int32_t)n); after_op();
    }
    g.setAddrWindow(10, 104, 33, 9); after_op();
    for (int k = 0; k < 9; k++) { g.writeColor((uint16_t)(0x07E0 + k * 0x0841), 33); after_op(); }
    g.endWrite(); after_op();

    // Scroll (copyRect) — mirrored by reading the internal panel back.
    g.setScrollRect(0, 0, 240, 60); after_op();
    g.scroll(0, -7); after_op();
    g.scroll(5, 0); after_op();
    g.clearScrollRect();
}

static bool dump(const char *path, LGFX_Sprite &s)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    for (int y = 0; y < s.height(); y++)
        for (int x = 0; x < s.width(); x++) {
            uint32_t c = s.readPixel(x, y);   // rgb565
            uint8_t rgb[3] = { (uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC), (uint8_t)((c << 3) & 0xF8) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
    return true;
}

static void test_mirror(const char *outdir)
{
    PanelSprite ref, inner, outer;
    ref.setColorDepth(16);   ref.createSprite(240, 135);
    inner.setColorDepth(16); inner.createSprite(240, 135);
    outer.setColorDepth(16); outer.createSprite(320, 240);
    outer.fillSprite(SENTINEL);

    s_spy = nullptr;
    draw_script(ref);

    SpyMirror mirror;
    mirror.attach(inner.panel(), outer.panel());
    static int s_lk = 0, s_lk_depth = 0, s_lk_max = 0;
    mirror.set_ext_lock([] { s_lk++; if (++s_lk_depth > s_lk_max) s_lk_max = s_lk_depth; }, [] { s_lk_depth--; });
    s_spy = &mirror; s_inner = &inner; s_outer = &outer;
    CHECK(mirror.width() == 240 && mirror.height() == 135, "mirror carries the internal size");
    CHECK(mirror.origin_x() == 40 && mirror.origin_y() == 52, "centred at 40,52");

    LGFX_Device dev;
    dev.setPanel(&mirror);
    dev.setColorDepth(16);
    dev.setRotation(0);
    s_ops = 0; s_leaks = 0;
    draw_script(dev);

    CHECK(s_leaks == 0, "external bus left open after %d of %d calls", s_leaks, s_ops);
    CHECK(s_diverged == 0, "external diverged from internal after %d calls (first: call #%d)", s_diverged, s_first_diverged);
    CHECK(mirror.open == 0 && mirror.max_open == 1, "external transactions never nest (max %d)", mirror.max_open);
    CHECK(mirror.opened > 0, "external panel was written");
    CHECK(s_lk == mirror.opened && s_lk_depth == 0 && s_lk_max == 1, "ext lock taken once per primitive (%d/%d), never nested", s_lk, mirror.opened);

    int diff_ref = 0, diff_ext = 0, outside = 0;
    for (int y = 0; y < 135; y++)
        for (int x = 0; x < 240; x++) {
            uint32_t a = ref.readPixel(x, y), b = inner.readPixel(x, y), e = outer.readPixel(x + 40, y + 52);
            if (a != b) diff_ref++;
            if (b != e) diff_ext++;
        }
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 320; x++)
            if ((x < 40 || x >= 280 || y < 52 || y >= 187) && outer.readPixel(x, y) != SENTINEL) outside++;
    CHECK(diff_ref == 0, "internal frame differs from the unmirrored reference in %d px", diff_ref);
    CHECK(diff_ext == 0, "external window differs from the internal frame in %d px", diff_ext);
    CHECK(outside == 0, "%d px written outside the centred window", outside);

    // Brightness follows to an external backlight callback; sleep reaches both panels.
    static int s_bl = -1;
    mirror.set_brightness_cb([](uint8_t b) { s_bl = b; });
    dev.setBrightness(77);
    CHECK(s_bl == 77, "backlight callback follows setBrightness");

    char path[512];
    snprintf(path, sizeof path, "%s/internal.rgb", outdir); dump(path, inner);
    for (int y = 0; y < 240; y++)                     // the letterbox shows black on the real panel
        for (int x = 0; x < 320; x++)
            if (outer.readPixel(x, y) == SENTINEL) outer.drawPixel(x, y, (uint16_t)0);
    // The touch chrome around the frame: must not touch a single frame pixel.
    std::vector<uint32_t> before(240 * 135);
    for (int y = 0; y < 135; y++) for (int x = 0; x < 240; x++) before[y * 240 + x] = outer.readPixel(x + 40, y + 52);
    extdisp_rect_t keys[SK_COUNT];
    extdisp_sk_layout(320, 240, 40, 52, 240, 135, keys);
    const extdisp_frame_t fr = { 40, 52, 240, 135 };
    extdisp_chrome_draw(&outer, keys, fr, "ili9341? 27MHz", "hold: calibrate", true);
    extdisp_chrome_key(&outer, keys, SK_OK, true, nullptr, nullptr, false);    // one key shown pressed
    int touched = 0;
    for (int y = 0; y < 135; y++) for (int x = 0; x < 240; x++) if (outer.readPixel(x + 40, y + 52) != before[y * 240 + x]) touched++;
    CHECK(touched == 0, "chrome wrote %d px inside the frame", touched);
    int lit = 0;
    for (int y = 190; y < 237; y++) for (int x = 131; x < 189; x++) if (outer.readPixel(x, y) != 0) lit++;
    CHECK(lit > 1500, "pressed OK key is drawn (%d px)", lit);
    snprintf(path, sizeof path, "%s/external.rgb", outdir); dump(path, outer);
    extdisp_chrome_calib(&outer, 1, false, "Calibrate touch", "Step 2 of 4", "Tap the centre of the red target");
    CHECK(outer.readPixel(160, 2) == 0xFFFF, "calibration screen is white edge to edge");
    snprintf(path, sizeof path, "%s/external_calib.rgb", outdir); dump(path, outer);
    printf("extdisp: %d mirrored calls, %d external transactions\n", s_ops, mirror.opened);
}

// ---- solo: the external panel is THE display (launcher on 320x240), the internal one untouched ---------
static void test_solo(const char *outdir)
{
    PanelSprite inner, outer;
    inner.setColorDepth(16); inner.createSprite(240, 135); inner.fillSprite(TFT_NAVY);
    const uint32_t navy = inner.readPixel(0, 0);
    outer.setColorDepth(16); outer.createSprite(320, 240); outer.fillSprite(SENTINEL);
    SpyMirror mirror;
    mirror.attach(inner.panel(), outer.panel());
    LGFX_Device dev;
    dev.setPanel(&mirror);
    dev.setColorDepth(16);
    dev.setRotation(0);

    dev.startWrite();
    CHECK(!mirror.set_solo(true) && !mirror.solo(), "no switch while a frame is open on the internal panel");
    dev.endWrite();
    CHECK(mirror.set_solo(true) && mirror.solo(), "solo on between frames");
    dev.clearClipRect(); dev.clearScrollRect();                  // LGFXBase caches the clip box: re-read the size
    CHECK(dev.width() == 320 && dev.height() == 240, "solo reports the external size (%dx%d)", (int)dev.width(), (int)dev.height());
    CHECK(mirror.origin_x() == 0 && mirror.origin_y() == 0, "solo is unshifted");
    CHECK(!mirror.isReadable(), "solo is write-only");

    dev.fillScreen(TFT_DARKGREY);
    dev.fillRect(300, 220, 20, 20, TFT_RED);                      // the far corner exists only on the external
    dev.drawString("solo 320x240", 10, 10, &lgfx::fonts::Font4);
    LGFX_Sprite band(&dev);                                       // a 320-wide 8bpp strip, as the launcher pushes
    band.setColorDepth(8);
    band.createSprite(320, 40);
    band.fillSprite(TFT_BLUE);
    band.fillCircle(160, 20, 15, TFT_YELLOW);
    dev.startWrite();
    band.pushSprite(&dev, 0, 100);
    dev.endWrite();
    band.deleteSprite();
    std::vector<uint16_t> px(64 * 3);
    for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(i * 977);
    dev.startWrite();
    dev.setAddrWindow(250, 150, 64, 3);
    dev.pushPixels(px.data(), 100);
    dev.pushPixels(px.data() + 100, (int32_t)px.size() - 100);
    dev.endWrite();
    dev.setScrollRect(0, 0, 320, 60); dev.scroll(0, -7); dev.clearScrollRect();   // a no-op, never a crash
    dev.fillSmoothCircle(60, 200, 20, TFT_GREEN);                // anti-aliased: written as opaque runs, no readback
    CHECK(outer.readPixel(60, 200) == 0x07E0 && outer.readPixel(60, 185) == 0x07E0, "solo: an anti-aliased circle lands filled");
    CHECK(outer.readPixel(60, 225) != 0x07E0, "solo: the circle stays inside its radius");

    CHECK(mirror.open == 0 && mirror.max_open == 1, "solo: one external transaction per primitive (max %d)", mirror.max_open);
    CHECK(mirror.opened > 0, "solo wrote the external panel");
    int in_changed = 0;
    for (int y = 0; y < 135; y++) for (int x = 0; x < 240; x++) if (inner.readPixel(x, y) != navy) in_changed++;
    CHECK(in_changed == 0, "solo touched %d internal px", in_changed);
    CHECK(outer.readPixel(310, 230) == 0xF800, "solo reaches the external corner unshifted (%04x)", (unsigned)outer.readPixel(310, 230));
    CHECK(outer.readPixel(160, 120) == lgfx::color565(255, 255, 0), "solo: the 8bpp strip landed in place");
    {
        PanelSprite ref;                                          // the same stream on a plain 320x240 sprite
        ref.setColorDepth(16); ref.createSprite(320, 240);
        ref.startWrite(); ref.setAddrWindow(250, 150, 64, 3);
        ref.pushPixels(px.data(), 100); ref.pushPixels(px.data() + 100, (int32_t)px.size() - 100);
        ref.endWrite();
        int bad = 0;
        for (int y = 150; y < 153; y++) for (int x = 250; x < 314; x++) if (outer.readPixel(x, y) != ref.readPixel(x, y)) bad++;
        CHECK(bad == 0, "solo: a split pixel stream resumes mid-window (%d px off)", bad);
    }
    char path[512];
    snprintf(path, sizeof path, "%s/external_solo.rgb", outdir); dump(path, outer);

    CHECK(mirror.set_solo(false) && !mirror.solo(), "solo off");
    dev.clearClipRect(); dev.clearScrollRect();
    CHECK(dev.width() == 240 && dev.height() == 135 && mirror.origin_x() == 40 && mirror.origin_y() == 52, "back to the centred mirror");
    dev.fillRect(0, 0, 10, 10, TFT_GREEN);
    CHECK(inner.readPixel(5, 5) == outer.readPixel(45, 57) && inner.readPixel(5, 5) != navy, "mirroring resumes after solo");
}

// ---- fit: every compact frame scaled 4/3 (240x135 -> 320x180, centred at 0,30), nearest pixel ----------
static int fit_x(int x) { return x * 4 / 3; }
static int fit_diff(PanelSprite &inner, PanelSprite &outer)
{
    int bad = 0;
    for (int y = 0; y < 135; y++)
        for (int x = 0; x < 240; x++) {
            uint32_t c = inner.readPixel(x, y);
            for (int Y = fit_x(y); Y < fit_x(y + 1); Y++)
                for (int X = fit_x(x); X < fit_x(x + 1); X++)
                    if (outer.readPixel(X, Y + 30) != c) bad++;
        }
    return bad;
}
static PanelSprite *s_fit_in, *s_fit_out;
static int s_fit_ops = 0, s_fit_bad_ops = 0, s_fit_first = -1;
static void fit_check(void)
{
    s_fit_ops++;
    if (fit_diff(*s_fit_in, *s_fit_out)) { s_fit_bad_ops++; if (s_fit_first < 0) s_fit_first = s_fit_ops; }
}

static void test_fit(const char *outdir)
{
    PanelSprite inner, outer;
    inner.setColorDepth(16); inner.createSprite(240, 135); inner.fillSprite(TFT_BLACK);
    outer.setColorDepth(16); outer.createSprite(320, 240); outer.fillSprite(SENTINEL);
    SpyMirror mirror;
    mirror.attach(inner.panel(), outer.panel());
    CHECK(mirror.set_fit(true) && mirror.fit(), "fit on");
    CHECK(mirror.width() == 240 && mirror.height() == 135, "fit keeps the compact size for the OS");
    CHECK(mirror.frame_w() == 320 && mirror.frame_h() == 180, "fit frame is 320x180 (%dx%d)", mirror.frame_w(), mirror.frame_h());
    CHECK(mirror.origin_x() == 0 && mirror.origin_y() == 30, "fit frame centred at 0,30 (%d,%d)", mirror.origin_x(), mirror.origin_y());
    LGFX_Device dev;
    dev.setPanel(&mirror);
    dev.setColorDepth(16);
    dev.setRotation(0);

    // The same script as the 1:1 mirror (scaled image, transparent push, alpha edges, scroll), then compare.
    s_spy = nullptr;
    s_fit_in = &inner; s_fit_out = &outer;
    draw_script(dev);
    fit_check();
    CHECK(s_fit_bad_ops == 0, "fit: external differs from the scaled internal frame (%d px)", fit_diff(inner, outer));
    CHECK(mirror.open == 0 && mirror.max_open == 1, "fit: one external transaction per primitive (max %d)", mirror.max_open);

    // A pixel stream split mid-row and a fill stream, then a check.
    std::vector<uint16_t> px(37 * 5);
    for (size_t i = 0; i < px.size(); i++) px[i] = (uint16_t)(i * 2311 + 7);
    dev.startWrite();
    dev.setAddrWindow(101, 60, 37, 5);
    dev.pushPixels(px.data(), 50);
    dev.pushPixels(px.data() + 50, (int32_t)px.size() - 50);
    dev.setAddrWindow(3, 120, 11, 7);
    dev.writeColor((uint16_t)0xF81F, 11 * 7);
    dev.endWrite();
    CHECK(fit_diff(inner, outer) == 0, "fit: split streams scale exactly (%d px off)", fit_diff(inner, outer));
    int outside = 0;
    for (int y = 0; y < 240; y++) for (int x = 0; x < 320; x++) if ((y < 30 || y >= 210) && outer.readPixel(x, y) != SENTINEL) outside++;
    CHECK(outside == 0, "fit: %d px written into the letterbox bands", outside);
    {   // screenshots: every rebuilt external row equals the panel (the letterbox reads black, it was never written)
        std::vector<uint16_t> row(320), tmp(240);
        int bad = 0;
        for (int y = 30; y < 210; y++) {
            mirror.ext_row(y, row.data(), tmp.data());
            for (int x = 0; x < 320; x++) {
                uint16_t raw = row[x], c = (uint16_t)((raw >> 8) | (raw << 8));   // write format is byte-swapped
                if (c != (uint16_t)outer.readPixel(x, y)) bad++;
            }
        }
        CHECK(bad == 0, "fit: rebuilt screenshot rows differ from the panel in %d px", bad);
        mirror.ext_row(5, row.data(), tmp.data());
        CHECK(row[0] == 0 && row[319] == 0, "fit: screenshot letterbox is black");
    }
    char path[512];
    snprintf(path, sizeof path, "%s/external_fit.rgb", outdir); dump(path, outer);

    // Solo still wins over fit (the large surface is unscaled), and fit comes back after it.
    CHECK(mirror.set_solo(true), "solo over fit");
    dev.clearClipRect();
    CHECK(dev.width() == 320 && mirror.origin_y() == 0, "solo ignores fit");
    CHECK(mirror.set_solo(false), "solo off");
    dev.clearClipRect();
    CHECK(mirror.origin_y() == 30 && mirror.frame_h() == 180, "fit geometry restored after solo");
    CHECK(mirror.set_fit(false) && mirror.origin_y() == 52 && mirror.frame_w() == 240, "fit off: back to 1:1");
}

// ---- frame hold: one external transaction per frame, given back to the SD on demand ---------------------
static uint32_t s_clock_ms = 0;
static void *s_task = (void *)1;
static void test_hold(void)
{
    PanelSprite inner, outer;
    inner.setColorDepth(16); inner.createSprite(240, 135); inner.fillSprite(TFT_BLACK);
    outer.setColorDepth(16); outer.createSprite(320, 240); outer.fillSprite(SENTINEL);
    SpyMirror mirror;
    mirror.attach(inner.panel(), outer.panel());
    mirror.set_hold([]() -> uint32_t { return s_clock_ms; }, []() -> void * { return s_task; }, 8);
    LGFX_Device dev;
    dev.setPanel(&mirror);
    dev.setColorDepth(16);
    dev.setRotation(0);

    // Outside a frame: unchanged, one real open per primitive, closed after each.
    dev.drawPixel(1, 1, TFT_RED);
    CHECK(mirror.open == 0 && mirror.opened == 1, "hold: a lone primitive still opens and closes");

    // A frame of 200 pixels (an outline): ONE real open, closed by endWrite.
    int o0 = mirror.opened;
    dev.startWrite();
    for (int i = 0; i < 200; i++) dev.drawPixel(10 + i % 100, 10 + i / 100, TFT_GREEN);
    CHECK(mirror.open == 1, "hold: the bus stays open inside the frame");
    dev.endWrite();
    CHECK(mirror.open == 0 && mirror.opened - o0 == 1, "hold: 200 primitives = %d bus opens", mirror.opened - o0);
    CHECK(inner.readPixel(50, 10) == outer.readPixel(90, 62), "hold: what is drawn is unchanged");

    // The SD asks mid-frame on the SAME task: the hold is given back at once, reopened by the next primitive.
    o0 = mirror.opened;
    dev.startWrite();
    dev.drawPixel(5, 5, TFT_BLUE);
    mirror.release_hold();
    CHECK(mirror.open == 0, "hold: released for an SD transfer of the drawing task");
    dev.drawPixel(6, 5, TFT_BLUE);
    dev.endWrite();
    CHECK(mirror.open == 0 && mirror.opened - o0 == 2, "hold: reopened once after the SD transfer (%d)", mirror.opened - o0);

    // Another task cannot take it away (it waits for the bus instead) ...
    dev.startWrite();
    dev.drawPixel(7, 5, TFT_BLUE);
    s_task = (void *)2; mirror.release_hold(); s_task = (void *)1;
    CHECK(mirror.open == 1, "hold: another task's request does not close it");
    // ... and the budget bounds how long it waits.
    o0 = mirror.opened;
    s_clock_ms += 9;
    dev.drawPixel(8, 5, TFT_BLUE);
    CHECK(mirror.opened - o0 == 1, "hold: past the 8 ms budget the bus is given back and retaken");
    dev.endWrite();
    CHECK(mirror.open == 0 && mirror.max_open == 1, "hold: never nested, closed at the end (max %d)", mirror.max_open);
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    test_probe_logic();
    test_touch_logic();
    test_mirror(outdir);
    test_solo(outdir);
    test_fit(outdir);
    test_hold();
    printf("extdisp: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}

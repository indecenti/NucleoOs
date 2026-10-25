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
    { extdisp_cfg_t b = c; b.mhz = 80; CHECK(extdisp_cfg_check(&b) != NULL, "80 MHz refused"); }

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

// ---- 2. the mirror on the real LovyanGFX core ---------------------------------------------------------
struct PanelSprite : public LGFX_Sprite { IPanel *panel(void) { return _panel; } };

// Counts external transactions; `open` must be 0 whenever control is back in the caller.
struct SpyMirror : public Panel_Mirror {
    int open = 0, opened = 0, max_open = 0;
    void ext_begin(void) override { open++; opened++; if (open > max_open) max_open = open; Panel_Mirror::ext_begin(); }
    void ext_end(void) override { open--; Panel_Mirror::ext_end(); }
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
    snprintf(path, sizeof path, "%s/external.rgb", outdir); dump(path, outer);
    printf("extdisp: %d mirrored calls, %d external transactions\n", s_ops, mirror.opened);
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : ".";
    test_probe_logic();
    test_mirror(outdir);
    printf("extdisp: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}

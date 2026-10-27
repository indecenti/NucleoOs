// presence-host: drives the REAL presence_detect.c on synthetic CSI and renders the REAL presence_view.cpp
// with the real LovyanGFX into raw RGB dumps (run.mjs turns them into PNGs). No device involved.
//   test_presence.exe <outdir>
#include <M5GFX.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "presence_detect.h"
#include "presence_view.h"
#include "presence_alarm.h"
extern "C" {
#include "nucleo_i18n.h"
#include "nucleo_theme.h"
}

static int s_fail;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL %s\n", msg); s_fail++; } else printf("ok   %s\n", msg); } while (0)

// ── synthetic CSI ─────────────────────────────────────────────────────────────
static uint32_t s_rng = 12345;
static float rnd(void) { s_rng = s_rng * 1103515245u + 12345u; return ((s_rng >> 8) & 0xFFFF) / 65535.0f; }

// 52 subcarriers. `motion` scales a slow body-like modulation of amplitude and phase.
static float s_gain = 1;   // the radio's AGC: scales every tone of a packet alike
static void frame(pd_state_t *s, float motion, float t)
{
    int8_t b[104];
    for (int k = 0; k < 52; k++) {
        float amp = s_gain * 20 + s_gain * 4 * sinf(k * 0.3f) + (rnd() - 0.5f) * 0.6f + motion * 9 * sinf(t * 7.1f + k * 0.21f);
        float ph = k * 0.4f + motion * 1.4f * sinf(t * 5.3f + k * 0.13f) + (rnd() - 0.5f) * 0.03f;
        b[2 * k] = (int8_t)lrintf(amp * sinf(ph));
        b[2 * k + 1] = (int8_t)lrintf(amp * cosf(ph));
    }
    pd_feed(s, b, sizeof b, (int8_t)-52, false);
}

// Run `secs` of radio at 25 frames/s, stepping at 12.5 Hz.
static uint32_t s_now = 1000;
static void run(pd_state_t *s, float secs, float motion)
{
    for (int i = 0; i < (int)(secs * 25); i++) {
        frame(s, motion, s_now / 1000.0f);
        s_now += 40;
        if (i & 1) pd_step(s, s_now);
    }
}

// Two links side by side (router + Faro), each with its own motion.
static void run2(pd_state_t *a, float ma, pd_state_t *b, float mb, float secs)
{
    for (int i = 0; i < (int)(secs * 25); i++) {
        frame(a, ma, s_now / 1000.0f);
        frame(b, mb, s_now / 1000.0f + 3.3f);
        s_now += 40;
        if (i & 1) { pd_step(a, s_now); pd_step(b, s_now); }
    }
}

static void alarm_tests(void)
{
    pa_t a;
    pa_init(&a, 1, 0);                                   // arm after 10 s, siren 10 s
    CHECK(!pa_locked(&a) && pa_step(&a, 100, true) == PA_EV_NONE, "off: motion does nothing");
    pa_arm(&a, 1000);
    int ticks = 0;
    for (uint32_t t = 1000; t < 10900; t += 80) ticks += pa_step(&a, t, true) == PA_EV_TICK;
    CHECK(ticks == 10 && a.st == PA_ARMING, "arming: one tick a second, motion ignored");
    CHECK(pa_step(&a, 11000, false) == PA_EV_ARMED && a.st == PA_ARMED, "armed after the exit delay");
    CHECK(pa_step(&a, 11500, true) == PA_EV_NONE, "grace: no trigger right after arming");
    CHECK(pa_step(&a, 13000, true) == PA_EV_TRIGGER && a.trig_count == 1, "motion while armed fires");
    CHECK(pa_left_s(&a, 13000) == 10 && pa_left_s(&a, 22100) == 1, "siren countdown");
    CHECK(pa_step(&a, 23000, false) == PA_EV_AUTO_OFF && !pa_locked(&a), "auto-disarm after the siren time");
    pa_arm(&a, 30000); pa_step(&a, 40000, false);
    pa_disarm(&a, 41000);
    CHECK(!pa_locked(&a) && pa_step(&a, 42000, true) == PA_EV_NONE, "PIN disarm: motion no longer fires");
    pa_init(&a, 99, -3);
    CHECK(a.arm_i == PA_N_ARM - 1 && a.dur_i == 0, "options clamp");
}

static void detector_tests(void)
{
    pd_state_t s;
    pd_init(&s, 0.35f, s_now);
    CHECK(s.phase == PD_CALIB, "starts calibrating");
    run(&s, 3, 0);
    CHECK(s.phase == PD_CALIB && !s.present, "no verdict while calibrating");
    run(&s, 4, 0);
    CHECK(s.phase == PD_RUN, "calibration ends after 6 s");
    run(&s, 30, 0);
    CHECK(!s.present, "still room: clear for 30 s");
    int false_pos = 0;
    for (int i = 0; i < 20; i++) { run(&s, 1, 0); false_pos += s.present; }
    CHECK(false_pos == 0, "still room: no false presence over 20 more s");
    for (int i = 0; i < 6; i++) { s_gain = (i & 1) ? 1.0f : 1.6f; run(&s, 1, 0); false_pos += s.present; }
    s_gain = 1;
    CHECK(false_pos == 0, "AGC gain steps in a still room are not motion");
    run(&s, 2, 1);
    CHECK(s.present && s.moving, "walking: presence within 2 s");
    CHECK(s.n_ev == 1 && s.ev[0].end_ms == 0, "an episode opens");
    run(&s, 5, 0);
    CHECK(s.present && !s.moving, "stops: presence coasts");
    run(&s, 7, 0);
    CHECK(!s.present, "clear after the 10 s hold");
    CHECK(s.ev[0].end_ms != 0 && s.ev[0].peak > 0, "the episode closes with a peak");
    pd_calibrate(&s, s_now);
    CHECK(s.phase == PD_CALIB && !s.present, "recalibrate drops presence");
    CHECK(fabsf(pd_distance_m(-40) - 1) < 0.01f && pd_distance_m(-95) == 30, "RSSI distance model");
    CHECK(pd_quality(-50, 25) == 100 && pd_quality(-90, 25) == 0 && pd_quality(-50, 2) == 10, "quality");
}

// ── rendering ─────────────────────────────────────────────────────────────────
static const char *s_out;
static void dump(const char *name, uint8_t *rgb, int w, int h)
{
    char p[256]; snprintf(p, sizeof p, "%s/%s.%dx%d.rgb", s_out, name, w, h);
    FILE *f = fopen(p, "wb"); fwrite(rgb, 1, (size_t)w * h * 3, f); fclose(f);
}
static void grab(LGFX_Sprite &sp, uint8_t *rgb, int w, int rows, int y0)
{
    for (int y = 0; y < rows; y++)
        for (int x = 0; x < w; x++) {
            auto c = sp.readPixelRGB(x, y);
            uint8_t *o = rgb + ((size_t)(y0 + y) * w + x) * 3;
            o[0] = c.R8(); o[1] = c.G8(); o[2] = c.B8();
        }
}

// Layout gate: every text and map mark the view drew (presence_view.cpp, PV_LAYOUT_PROBE) must stay on
// screen and touch nothing else. Texts inside their own pill/tile/button are not boxes of their own.
static int s_layout_fail;
static void layout_check(const char *name, int W, int H)
{
    for (int i = 0; i < g_pv_nbox; i++) {
        const pv_box_t &a = g_pv_box[i];
        if (a.x < 0 || a.y < 0 || a.x + a.w > W || a.y + a.h > H) {
            printf("LAYOUT %s: \"%s\" off screen (%d,%d %dx%d)\n", name, a.what, a.x, a.y, a.w, a.h);
            s_layout_fail++;
        }
        for (int j = i + 1; j < g_pv_nbox; j++) {
            const pv_box_t &b = g_pv_box[j];
            if (a.kind == 'm' && b.kind == 'm') continue;
            if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h) {
                printf("LAYOUT %s: \"%s\" overlaps \"%s\"\n", name, a.what, b.what);
                s_layout_fail++;
            }
        }
    }
}

// The device composites the Cardputer screen in an 8 bpp 240x135 canvas, and the external panel in
// 320-wide strips of the same 32 KB (101 rows). Reproduce both exactly.
static void shot_compact(const char *name, const pv_model_t &m, bool summary)
{
    LGFX_Sprite sp; sp.setColorDepth(8); sp.createSprite(240, 135);
    sp.fillSprite(THEME_BG);
    g_pv_nbox = 0;
    if (summary) pv_draw_guide(sp, 0, 121, m); else pv_draw_compact(sp, 0, 121, m);
    layout_check(name, 240, 121);
    sp.fillRect(0, 121, 240, 14, THEME_LINE);   // where the framework's hint bar goes
    static uint8_t rgb[240 * 135 * 3];
    grab(sp, rgb, 240, 135, 0);
    dump(name, rgb, 240, 135);
    sp.deleteSprite();
}
static void shot_large(const char *name, const pv_model_t &m)
{
    LGFX_Sprite sp; sp.setColorDepth(8); sp.createSprite(320, 240 * 135 / 320);
    static uint8_t rgb[320 * 240 * 3];
    g_pv_nbox = 0;
    for (int oy = 0; oy < 240; oy += sp.height()) {
        pv_draw_large(sp, oy, m);
        if (oy == 0) layout_check(name, 320, 240);
        grab(sp, rgb, 320, 240 - oy < sp.height() ? 240 - oy : sp.height(), oy);
    }
    dump(name, rgb, 320, 240);
    sp.deleteSprite();
}

static void scenes(const char *lang)
{
    nucleo_i18n_set_lang(lang);
    static pd_state_t s;
    s_now = 1000;
    pd_init(&s, 0.35f, s_now);
    pv_model_t m = {};
    static pa_t al;
    pa_init(&al, 1, 2);
    m.alarm = &al;
    m.pd = &s; m.status = PV_OK; m.view = PV_RADAR; m.rate = 24; m.sweep = 5.2f; m.glyph = 0.55f; m.ssid = "HomeNet-5G";
    char n[64];
#define SHOT(tag, summary) do { m.now_ms = s_now; snprintf(n, sizeof n, "%s.%s", tag, lang); shot_compact(n, m, summary); } while (0)
#define LARGE(tag) do { m.now_ms = s_now; snprintf(n, sizeof n, "large.%s.%s", tag, lang); shot_large(n, m); } while (0)

    run(&s, 2.5f, 0);
    SHOT("calib", false); LARGE("calib"); SHOT("guide-calib", true);
    run(&s, 70, 0);
    SHOT("clear", false); LARGE("clear"); SHOT("guide-clear", true);
    m.view = PV_DETAILS; SHOT("details-clear", false); m.view = PV_RADAR;
    run(&s, 4, 1);
    SHOT("moving", false); LARGE("moving"); SHOT("guide-moving", true);
    m.view = PV_MENU; m.sel = 0; SHOT("menu-sens", false); m.sel = 3; SHOT("menu-info", false); m.view = PV_RADAR;
    m.view = PV_DETAILS; SHOT("details-moving", false); m.view = PV_RADAR;
    run(&s, 6, 0);
    m.glyph = 0.3f;
    SHOT("still", false); LARGE("still");
    m.view = PV_INFO; SHOT("info", false); m.view = PV_RADAR;
    m.rate = 0; SHOT("nodata", false); m.rate = 24;
    m.status = PV_NOWIFI; SHOT("nowifi", false); LARGE("nowifi");
    m.status = PV_BUSY; SHOT("busy", false);
    m.status = PV_NOWIFI; m.view = PV_MENU; m.sel = 1; SHOT("menu-nowifi", false);

    // Two Cardputers: the router link plus a Faro. Both detectors run on their own frames.
    static pd_state_t f;
    s_now = 1000;
    pd_init(&s, 0.35f, s_now); pd_init(&f, 0.35f, s_now);
    m.status = PV_OK; m.view = PV_RADAR; m.faro = &f; m.faro_rate = 98; m.glyph = 0.55f;
    run2(&s, 0, &f, 0, 75);
    SHOT("faro-clear", false); LARGE("faro-clear"); SHOT("guide-faro-clear", true);
    run2(&s, 0, &f, 1, 4);
    SHOT("faro-side", false); LARGE("faro-side"); SHOT("guide-faro-side", true);
    run2(&s, 1, &f, 1, 3);
    SHOT("faro-both", false); LARGE("faro-both");
    m.view = PV_DETAILS; SHOT("details-faro", false);
    m.view = PV_MENU; m.sel = PV_ROW_FARO; SHOT("menu-faro", false);
    // The alarm, with a Faro linked: control centre, exit delay, armed, fired.
    m.view = PV_MENU; m.sel = PV_ROW_ALARM; SHOT("cc-alarm", false); LARGE("cc");
    m.sel = PV_ROW_DURATION; SHOT("cc-duration", false);
    m.view = PV_RADAR;
    pa_arm(&al, s_now); s_now += 3200; pa_step(&al, s_now, false);
    m.pin_len = 0; SHOT("arming", false); LARGE("arming"); SHOT("guide-arming", true);
    s_now += 8000; pa_step(&al, s_now, false); run2(&s, 0, &f, 0, 2);
    m.pin_len = 2; SHOT("armed", false); LARGE("armed");
    run2(&s, 0, &f, 1, 2); pa_step(&al, s_now, true);
    m.pin_len = 3; SHOT("intruder", false); LARGE("intruder"); SHOT("guide-intruder", true);
    s_now += 300; m.pin_len = 0; m.pin_bad = true; SHOT("intruder-flash", false); LARGE("intruder-flash");
    pa_disarm(&al, s_now); m.pin_bad = false;
    m.view = PV_RADAR; m.faro = nullptr; m.beacon = true; m.rate = 100; m.beacon_sent = 12345;
    SHOT("beacon", false); LARGE("beacon");
    m.view = PV_MENU; SHOT("menu-beacon", false);
}

int main(int argc, char **argv)
{
    s_out = argc > 1 ? argv[1] : ".";
    detector_tests();
    alarm_tests();
    nucleo_theme_set("classic");
    for (const char *l : { "it", "en", "es", "fr", "de" }) scenes(l);
    CHECK(s_layout_fail == 0, "layout: no text overlaps or leaves the screen, in any scene or language");
    printf(s_fail ? "presence: %d FAILED\n" : "presence: all checks passed\n", s_fail);
    return s_fail ? 1 : 0;
}

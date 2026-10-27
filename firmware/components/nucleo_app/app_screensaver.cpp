// app_screensaver.cpp — NucleoOS Salvaschermo
//
// Three animated effects + a screen-off mode. Activated automatically by the
// launcher after a configurable idle time (5-600 s). Any key closes it.
//
// ANTI-FLICKER: Technique 1 exclusively.
//   poll_fn() returns true at the target frequency → the framework calls
//   on_draw() → the app draws the full frame on the CANVAS (SRAM sprite) →
//   the framework blits the canvas to the TFT in a single DMA transaction.
//   NEVER direct_draw=true with fillScreen: that combo writes directly to the
//   TFT in two passes and produces visible flicker.
//
// Effects (all static .bss, zero runtime heap):
//   MODE_CLOCK — DVD-style bouncing clock (canvas: fillScreen + text)
//   MODE_STARS — 3D starfield warp   (canvas: fillScreen + drawPixel)
//   MODE_FIRE  — procedural fire     (canvas: fillRect per cell, batch write)

#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "launcher_theme.h"
#include "app_gfx.h"
#include "app_ui.h"
#include "ui_glyph.h"       // UG_CHECK: the effect in use
#include "app_large.h"      // two screens: the effect list + preview on the 320x240 panel, the effect itself when running
#include "nucleo_touch.h"
#include "nucleo_i18n.h"    // TR(it,en): hint bar follows the system language
#include <M5GFX.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_timer.h"

#define SETTINGS_PATH "/sd/system/config/screensaver.json"
#define W 240
#define H 135

// ---- Modes -------------------------------------------------------------------
#define MODE_OFF   0
#define MODE_CLOCK 1
#define MODE_STARS 2
#define MODE_FIRE  3

// ---- Shared state (read by nucleo_app.cpp) ----------------------------------
static int32_t g_threshold_ms = 60000;
static int     g_mode         = MODE_CLOCK;
static bool    g_trigger      = false;
static bool    s_cfg_loaded   = false;   // screensaver.json read this boot (see cfg_ensure)

// ---- App state ----------------------------------------------------------------
static bool    s_running    = false;
static bool    s_auto       = false;
static int     s_tab        = 0;
static int     s_sel        = 0;
static bool    s_dirty      = true;
static int64_t s_frame_us   = 0;    // timestamp of the last frame (µs)
static int     s_saved_bright = 80;
static int     s_lg_scroll    = 0;   // two screens: the panel effect-list's first visible row
#define PX   152                       // two screens: the right pane (preview / timer) and its slider track
#define PW   162
#define SL_X (PX + 18)
#define SL_W (PW - 36)

static void fmt_timer(char *sv, size_t n)            // "never", "45 sec", "1 min", "3 min", "2m 30s"
{
    const int sec = (int)(g_threshold_ms / 1000);
    if (sec == 0)           snprintf(sv, n, "%s", TR5("mai", "never", "nunca", "jamais", "niemals"));   // set from Settings
    else if (sec < 60)      snprintf(sv, n, "%d sec", sec);
    else if (sec % 60 == 0) snprintf(sv, n, "%d min", sec / 60);
    else                    snprintf(sv, n, "%dm %ds", sec / 60, sec % 60);
}

// ---- Target frequencies (µs/frame) -------------------------------------------
#define CLOCK_US  100000   // 10 fps — seconds clock, smooth enough
#define STARS_US   33333   // ~30 fps
#define FIRE_US    16666   // ~60 fps — silky-smooth fire

// ==================== BOUNCING CLOCK =========================================
// Technique 1: each frame → fillScreen(0) on canvas → draw text → single blit.
// No delta tracking: the canvas is the intermediate state, the TFT sees a
// complete frame on every blit.

// The view the effects step and draw in: the Cardputer (240x135) or, with the panel attached, the whole
// 320x240 panel at its own resolution (never the compact frame scaled up). Set by saver_start.
static int s_vw = W, s_vh = H;
#define CL_BIG (s_vw > W)                                   // the panel: 7-segment Font7 hour + Font2 date
#define CL_TW  (CL_BIG ? 256 : 144)                         // "HH:MM:SS": Font0 x3 (8 x 18 px) / Font7 (6 x 32 + 2 x 32)
#define CL_TH  (CL_BIG ? 70 : 38)                           // hour + gap + date

static int16_t cl_x, cl_y;
static int8_t  cl_vx, cl_vy;
static uint8_t cl_ci;

static const uint16_t CL_COLS[] = {
    0xFFFF, 0xFE8C, 0x8FF3, 0xFBB6, 0xC5F5, 0x07FF, 0xF96B
};
#define NCL 7

static void clock_init(void)
{
    srand((unsigned)((esp_timer_get_time() >> 8) & 0xFFFFFFFF));
    cl_x  = (int16_t)((s_vw - CL_TW) / 3 + (int)(rand() % 36u));
    cl_y  = (int16_t)((s_vh - CL_TH) / 4 + (int)(rand() % 57u));
    cl_vx = (rand() & 1) ? 2 : -2;
    cl_vy = (rand() & 1) ? 1 : -1;
    cl_ci = (uint8_t)(rand() % NCL);
}

static void clock_step(void)
{
    // Update position with bounce and color change at the edges
    cl_x += cl_vx; cl_y += cl_vy;
    if (cl_x <= 0)            { cl_x = 0;                     cl_vx =  1; cl_ci = (cl_ci + 1) % NCL; }
    if (cl_x + CL_TW >= s_vw) { cl_x = (int16_t)(s_vw - CL_TW); cl_vx = -1; cl_ci = (cl_ci + 1) % NCL; }
    if (cl_y <= 0)            { cl_y = 0;                     cl_vy =  1; cl_ci = (cl_ci + 1) % NCL; }
    if (cl_y + CL_TH >= s_vh) { cl_y = (int16_t)(s_vh - CL_TH); cl_vy = -1; cl_ci = (cl_ci + 1) % NCL; }
}

static void clock_frame(void)                        // pure: draws the state, never moves it
{
    const bool big = CL_BIG;
    d.fillRect(0, 0, s_vw, s_vh, 0x0000);

    uint16_t col = CL_COLS[cl_ci];

    // Faint halo (dark rectangle behind the text)
    d.fillRoundRect(cl_x - 4, cl_y - 3, CL_TW + 8, CL_TH + 4, 4,
                    (uint16_t)(col >> 3 & 0x1CE3));

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char hms[12];
    snprintf(hms, sizeof(hms), "%02d:%02d:%02d",
             tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0, tm ? tm->tm_sec : 0);
    char dat[20];
    if (tm) strftime(dat, sizeof(dat), "%a %d %b", tm);
    else    snprintf(dat, sizeof(dat), "---");

    d.setFont(big ? (const lgfx::IFont *)&fonts::Font7 : &fonts::Font0);
    d.setTextSize(big ? 1 : 3);
    d.setTextDatum(top_center);
    d.setTextColor(col, 0x0000);
    d.drawString(hms, cl_x + CL_TW / 2, cl_y);
    d.setFont(big ? (const lgfx::IFont *)&fonts::Font2 : &fonts::Font0);
    d.setTextSize(1);
    d.setTextColor(0x8C71, 0x0000);
    d.drawString(dat, cl_x + CL_TW / 2, cl_y + (big ? 52 : 26));
    d.setTextDatum(top_left);
}

// ==================== STARFIELD WARP 3D =======================================
// Technique 1: fillScreen(0) on canvas every frame, then drawPixel the stars.
// The canvas is blitted once: the TFT sees one atomic frame with no tearing.

#define NSTARS 64
#define ZMAX   24

static struct { int16_t ox, oy; uint8_t z; } st[NSTARS];

static void stars_reset(int i)
{
    st[i].z  = ZMAX;
    st[i].ox = (int16_t)((rand() % (s_vw + 40)) - (s_vw / 2 + 20));
    st[i].oy = (int16_t)((rand() % (s_vh + 40)) - (s_vh / 2 + 20));
}

// A star's projection in the current view; false when it left it.
static bool star_at(int i, int *sx, int *sy)
{
    *sx = s_vw / 2 + (int)st[i].ox * 20 / (int)st[i].z;
    *sy = s_vh / 2 + (int)st[i].oy * 20 / (int)st[i].z;
    return *sx >= 0 && *sx < s_vw && *sy >= 0 && *sy < s_vh;
}

static void stars_init(void)
{
    srand((unsigned)(esp_timer_get_time() & 0xFFFFFFFF));
    for (int i = 0; i < NSTARS; i++) {
        stars_reset(i);
        st[i].z = (uint8_t)(1 + rand() % ZMAX);
    }
}

static void stars_step(void)
{
    int sx, sy;
    for (int i = 0; i < NSTARS; i++)
        if (--st[i].z == 0 || !star_at(i, &sx, &sy)) stars_reset(i);
}

static void stars_frame(void)                        // pure: black sky + the projected stars
{
    d.fillRect(0, 0, s_vw, s_vh, 0x0000);
    const int big = s_vw > W ? 1 : 0;                // the panel's stars are a pixel larger
    int sx, sy;
    for (int i = 0; i < NSTARS; i++) {
        if (st[i].z >= ZMAX || !star_at(i, &sx, &sy)) continue;   // just respawned: not shown yet
        int bright = ((ZMAX - (int)st[i].z) * 255) / ZMAX;
        uint16_t col = (st[i].z <= 6)
            ? d.color565(bright, bright, 255)
            : d.color565(bright, bright, bright);
        const int sz = 1 + big + (st[i].z <= 4);
        d.fillRect(sx, sy, sz, sz, col);
    }
}

// ==================== PROCEDURAL FIRE (Doom-fire) =============================
// 60×27 grid of 4×5 px cells = exactly 240×135 (full coverage).
// +1 hidden source row below the screen: fire_buf[FH-1] is always 255,
// it isn't rendered but feeds the flames that reach the bottom edge.
//
// Classic Doom-fire algorithm (Fabien Sanglard):
//   each cell looks at the cell below, with random lateral shift and decay
//   0-1: produces a flame tongue with natural drift and no artefacts.
//
// 4-phase palette: black → dark red → orange → yellow → white at the tips.
// Technique 1: everything on the canvas (d = canvas in on_draw), single blit by the framework.

#define FW   60          // cell width (60*4=240)
#define FHV  27          // visible cells in height (27*5=135)
#define FH   28          // +1 hidden source row

// Heap-on-demand (was .bss ~1.7 KB): only the FIRE mode needs it, and the saver is closed at boot.
// Allocated in fire_init(), freed on_exit; the fire_* helpers skip cleanly if the alloc failed.
static uint8_t (*fire_buf)[FW] = nullptr;
static uint16_t fire_pal[256];
static bool     fire_pal_ok = false;
static int      fire_breath = 0;     // global oscillation 0..8 for flame breathing

static void fire_init_pal(void)
{
    if (fire_pal_ok) return;
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        // MAXIMUM fire palette: black → vivid dark red → fire red → burning orange → yellow+orange → white-hot
        if      (i < 15)  { r = 0;   g = 0;   b = 0; }                                      // deep black
        else if (i < 40)  { r = (i - 15) * 9;  g = 0;  b = 0; }                            // burning dark red (0..225)
        else if (i < 80)  { r = 255; g = (i - 40) * 4; b = 0; }                            // vivid red-orange fire
        else if (i < 120) { r = 255; g = 160 + (i - 80); b = (i - 80) / 4; }              // intense orange
        else if (i < 160) { r = 255; g = 200 + (i - 120) / 2; b = 30 + (i - 120) / 2; }   // burning yellow-orange
        else if (i < 200) { r = 255; g = 220 + (i - 160) / 4; b = 60 + (i - 160) / 2; }   // bright yellow with a hint of blue
        else              { r = 255; g = 230 + (i - 200) / 5; b = 100 + (i - 200) / 3; }  // glowing bluish white
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        fire_pal[i] = d.color565(r, g, b);
    }
    fire_pal_ok = true;
}

static void fire_seed(void)
{
    // Chaotic source with a global breath (oscillation 0..8) + explosive spikes
    fire_breath = (fire_breath + 1) % 16;  // 0..15, cycle for a sinusoidal breath
    int breath_mod = 4 + (fire_breath < 8 ? fire_breath : 16 - fire_breath);  // 4..12

    for (int x = 0; x < FW; x++) {
        int base = 255;
        // Intensity variation with breath modulation
        if (rand() % 3 == 0)  base = 200 + (rand() % 55);
        if (rand() % 5 == 0)  base = 180 + (rand() % 75);    // more burning spikes
        if (rand() % 10 == 0) base = 100 + (rand() % 155);   // holes for vortices
        if (rand() % 20 == 0) base = 255;                     // rare explosive fireballs

        // Apply the global breath as a modulator
        base = (base * breath_mod) / 8;
        fire_buf[FH - 1][x] = (uint8_t)(base > 255 ? 255 : base);
    }
}

static void fire_step(void)
{
    // MAXIMUM Doom-fire: explosive drift, dynamic decay, vortices + sparks
    for (int y = 0; y < FH - 1; y++) {
        for (int x = 0; x < FW; x++) {
            int r  = rand() % 16;  // [0..15] ultra-variance
            int sx = x - ((r >> 2) - 1);  // wide shift [-1..2]
            if (sx < 0)   sx = 0;
            if (sx >= FW) sx = FW - 1;

            // Variable 0-4 decay for more intense vortices
            int decay = (r & 7) >> 1;  // [0..3], but often less
            int v = (int)fire_buf[y + 1][sx] - decay;

            // Booster effects:
            if (rand() % 100 < 15) {  // rare rising sparks
                v = (int)fire_buf[y + 1][sx] - (decay >> 1);
            }
            if (rand() % 100 < 8) {   // ultra-rare explosive fireballs
                v = (int)fire_buf[y + 1][sx];
            }

            fire_buf[y][x] = (uint8_t)(v < 0 ? 0 : v);
        }
    }
}

static void fire_init(void)
{
    if (!fire_buf) fire_buf = (uint8_t (*)[FW])calloc(FH, FW);   // ~1.7 KB only while the fire runs
    if (!fire_buf) return;
    fire_init_pal();
    memset(fire_buf, 0, (size_t)FH * FW);
    fire_breath = 0;  // reset breath
    for (int i = 0; i < FH; i++) { fire_seed(); fire_step(); }   // warm up: flames from the first frame
}

static void fire_frame(void)                         // pure: the grid stretched over the view (4x5 px cells here)
{
    if (!fire_buf) { d.fillRect(0, 0, s_vw, s_vh, 0x0000); return; }
    for (int y = 0; y < FHV; y++) {
        const int y0 = y * s_vh / FHV, y1 = (y + 1) * s_vh / FHV;
        for (int x = 0; x < FW; x++) {
            const int x0 = x * s_vw / FW;
            d.fillRect(x0, y0, (x + 1) * s_vw / FW - x0, y1 - y0, fire_pal[fire_buf[y][x]]);
        }
    }
}

// One effect: advance it one frame (once per frame), then draw it (pure: lg::frame paints it per strip).
static void effect_step(void)
{
    if (g_mode == MODE_CLOCK) clock_step();
    else if (g_mode == MODE_STARS) stars_step();
    else if (g_mode == MODE_FIRE && fire_buf) { fire_seed(); fire_step(); }   // source always full: flame at the bottom
}
static void effect_draw(void *)
{
    switch (g_mode) {
        case MODE_CLOCK: clock_frame(); break;
        case MODE_STARS: stars_frame(); break;
        case MODE_FIRE:  fire_frame();  break;
        default:         d.fillRect(0, 0, s_vw, s_vh, 0x0000);   // off: a black panel (the Cardputer's backlight is off)
    }
}

// ==================== START / STOP SAVER ======================================
static void saver_start(void)
{
    s_running = true;
    const bool big = nucleo_app_two_screens();       // the panel plays it at 320x240, natively
    s_vw = big ? 320 : W; s_vh = big ? 240 : H;
    // fullscreen: the canvas covers the whole 240×135, hint bar excluded
    nucleo_app_set_fullscreen(true);
    // DO NOT use direct_draw: drawing happens on the canvas, the framework blits
    // once → zero flicker (Technique 1 anti-flicker).
    if (g_mode == MODE_OFF) {
        s_saved_bright = nucleo_app_brightness();
        nucleo_app_set_brightness(0);
    } else if (g_mode == MODE_CLOCK) {
        clock_init();
    } else if (g_mode == MODE_STARS) {
        stars_init();
    } else {
        fire_init();
    }
    s_frame_us = esp_timer_get_time();
    nucleo_app_request_draw();  // force the first frame immediately
}

static void saver_stop(void)
{
    s_running = false;
    nucleo_app_set_fullscreen(false);
    if (g_mode == MODE_OFF)
        nucleo_app_set_brightness(s_saved_bright);
    s_dirty = true;
}

// ==================== PERSISTENCE ==============================================
static void load_settings(void)
{
    FILE *f = fopen(SETTINGS_PATH, "rb");
    if (!f) return;
    char buf[64];
    int n = (int)fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    int mode = MODE_CLOCK, sec = 60;
    sscanf(buf, "{\"mode\":%d,\"sec\":%d}", &mode, &sec);
    if (mode < 0 || mode > 3) mode = MODE_CLOCK;
    if (sec < 0) sec = 60;
    if (sec != 0 && sec < 5) sec = 5;              // 0 = never (set from Settings)
    if (sec > 600) sec = 600;
    g_mode         = mode;
    g_threshold_ms = (int32_t)sec * 1000;
}

static void save_settings(void)
{
    mkdir("/sd/system", 0775);
    mkdir("/sd/system/config", 0775);
    FILE *f = fopen(SETTINGS_PATH, "wb");
    if (!f) return;
    fprintf(f, "{\"mode\":%d,\"sec\":%d}\n", g_mode, (int)(g_threshold_ms / 1000));
    fclose(f);
}

// ---- app_ui_list callbacks (EFFETTO tab) -------------------------------------
static const char *mode_label(int i, void *)
{
    static const char *const N[]    = { "Spento","Orologio","Stelle","Fuoco","► Prova" };
    static const char *const N_EN[] = { "Off","Clock","Stars","Fire","► Test" };
    return (unsigned)i < 5 ? nucleo_tr(N[i], N_EN[i]) : "";
}
static const char *mode_right(int i, void *)
{
    static const char *const D[]    = { "schermo spento","rimbalzante","campo stellare","fuoco", nullptr };
    static const char *const D_EN[] = { "screen off","bouncing","starfield","fire", nullptr };
    return (unsigned)i < 4 ? nucleo_tr(D[i], D_EN[i]) : nullptr;
}
static unsigned short mode_color(int i, void *)
{
    if (i == 4)      return C_YELLOW;
    if (i == g_mode) return C_GREEN;
    return C_PURPLE;
}

// ==================== SETTINGS UI ==============================================
static void draw_settings(void)
{
    int ch = nucleo_app_content_height();

    if (s_tab == 0) {
        int y0 = app_ui_title(TR("EFFETTO", "EFFECT"), C_PURPLE, "TAB ►");
        app_ui_list(y0, ch - 24, 5, s_sel, mode_label, mode_right, mode_color, nullptr);
    } else {
        int y0 = app_ui_title("TIMER", C_PURPLE, "◄ TAB");
        int rem = ch - 24;
        d.fillRect(0, y0, W, rem, BG);

        char sv[20]; fmt_timer(sv, sizeof sv);

        const char *sub = TR("Attivazione automatica dopo", "Starts automatically after");
        d.setTextSize(1); d.setTextColor(C_GREY, BG);
        d.setCursor((W - (int)strlen(sub) * 6) / 2, y0 + 6);
        d.print(sub);

        int vw  = (int)strlen(sv) * 12;
        int vx  = (W - vw) / 2;
        int vy  = y0 + 28;
        d.setTextSize(2); d.setTextColor(C_YELLOW, BG);
        d.setCursor(vx, vy); d.print(sv);
        d.setTextColor(C_GREY, BG);
        d.setCursor(vx - 14, vy); d.print("<");
        d.setCursor(vx + vw + 2, vy); d.print(">");

        const char *hlp = "</> 5s   UP/DOWN 30s   (5-600 s)";
        d.setTextSize(1); d.setTextColor(C_GREY, BG);
        d.setCursor((W - (int)strlen(hlp) * 6) / 2, vy + 22);
        d.print(hlp);

        const char *sts = g_threshold_ms > 0 ? TR("Attivo", "On") : TR("Disabilitato", "Disabled");
        d.setTextColor(g_threshold_ms > 0 ? C_GREEN : C_RED, BG);
        d.setCursor((W - (int)strlen(sts) * 6) / 2, vy + 36);
        d.print(sts);
    }
}

// ==================== APP CALLBACKS ============================================
static bool on_back(int key)
{
    if (s_running) {
        saver_stop();
        if (s_auto) { s_auto = false; nucleo_app_exit(); }
        else        { nucleo_app_set_hint(TR5("TAB cambia   invio seleziona   esc esci", "TAB switch   enter select   esc back", "TAB cambia   intro elige   esc atras", "TAB change   entrer selecte   esc ret", "TAB wechsel   enter waehle   esc zur")); nucleo_app_request_draw(); }
        return true;
    }
    return false;
}

static void on_tab(void)
{
    if (s_running) {
        // If previewing from a selection (s_auto), TAB doesn't cycle — only BACK closes
        if (s_auto) return;
        // Otherwise cycle effects (manual settings only)
        saver_stop();
        g_mode = (g_mode + 1) % 4;
        s_sel  = g_mode;
        save_settings();
        saver_start();
        return;
    }
    s_tab ^= 1; s_dirty = true;
    save_settings();
    nucleo_app_request_draw();
}

// poll_fn: called ~50 Hz, returns true at the effect's target frequency.
// The framework calls on_draw only when true → blits to the TFT at the data rate,
// not the loop rate (anti-flicker, avoids duplicate frames).
static bool poll_fn(void)
{
    if (!s_running || g_mode == MODE_OFF) return false;
    int64_t now = esp_timer_get_time();
    int64_t interval = (g_mode == MODE_CLOCK) ? CLOCK_US :
                       (g_mode == MODE_STARS) ? STARS_US : FIRE_US;
    if (now - s_frame_us < interval) return false;
    s_frame_us = now;
    return true;
}

static void on_enter(void)
{
    load_settings(); s_cfg_loaded = true;
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_poll_handler(poll_fn);

    if (g_trigger) {
        g_trigger = false; s_auto = true;
        saver_start();
    } else {
        s_running = false; s_auto = false;
        s_tab = 0; s_sel = g_mode; s_dirty = true;
        nucleo_app_set_hint(TR5("TAB cambia   invio seleziona   esc esci", "TAB switch   enter select   esc back", "TAB cambia   intro elige   esc atras", "TAB change   entrer selecte   esc ret", "TAB wechsel   enter waehle   esc zur"));
        nucleo_app_request_draw();
    }
}

static void on_key(int key, char ch)
{
    if (s_running) {
        saver_stop();
        if (s_auto) { s_auto = false; nucleo_app_exit(); }
        else        { nucleo_app_set_hint(TR5("TAB cambia   invio seleziona   esc esci", "TAB switch   enter select   esc back", "TAB cambia   intro elige   esc atras", "TAB change   entrer selecte   esc ret", "TAB wechsel   enter waehle   esc zur")); nucleo_app_request_draw(); }
        return;
    }
    bool changed = false;
    int tx, ty;
    if (nucleo_touch_is_row(key, ch)) {                  // panel rows: an effect (tap again = use / test) or Timer
        const int i = s_lg_scroll + (ch - NUCLEO_TOUCH_ROW);
        if (i >= 5) s_tab = 1;
        else {
            const bool again = s_tab == 0 && i == s_sel;
            s_tab = 0; s_sel = i;
            if (i < 4) g_mode = i;
            if (again) { if (i < 4) save_settings(); else { s_auto = false; saver_start(); return; } }
        }
        changed = true;
    } else if (lg::tap(key, ch, &tx, &ty)) {             // the timer slider: the tap sets the time (5 s steps)
        int sec = 5 + (lg::slider_at(tx, SL_X, SL_W) * 595 / 100 + 2) / 5 * 5;
        g_threshold_ms = (int32_t)(sec < 5 ? 5 : sec > 600 ? 600 : sec) * 1000;
        s_tab = 1; save_settings(); changed = true;
    }
    if (s_tab == 0) {
        if (key == NK_DOWN && s_sel < 4) { s_sel++; if (s_sel < 4) g_mode = s_sel; changed = true; }
        if (key == NK_UP   && s_sel > 0) { s_sel--; if (s_sel < 4) g_mode = s_sel; changed = true; }
        if (key == NK_ENTER || key == NK_RIGHT) {
            if (s_sel < 4) { g_mode = s_sel; save_settings(); changed = true; }
            else           { s_auto = false; saver_start(); return; }
        }
    } else {
        int sec = (int)(g_threshold_ms / 1000);
        if (key == NK_RIGHT && sec < 600) { sec += 5;  if (sec > 600) sec = 600; changed = true; }
        if (key == NK_LEFT  && sec > 5)   { sec -= 5;  if (sec < 5)   sec = 5;   changed = true; }
        if (key == NK_DOWN  && sec < 600) { sec += 30; if (sec > 600) sec = 600; changed = true; }
        if (key == NK_UP    && sec > 5)   { sec -= 30; if (sec < 5)   sec = 5;   changed = true; }
        if (changed) { g_threshold_ms = (int32_t)sec * 1000; save_settings(); }
    }
    if (changed) { s_dirty = true; nucleo_app_request_draw(); }
}

static void on_tick(void) {}

// ===== two screens ==============================================================================================
// With the panel attached the saver plays THERE: while running, the effect fills the panel natively at 320x240
// (the same step/draw code with the view set to the panel, composed by lg::frame); while idle, the panel is the
// settings sheet: the effects and the Timer as rows on the left, beside them a preview of the picked effect or the
// timer slider. The Cardputer is the companion (what is picked, the keys, or "running - press a key"). Touch: tap
// a row to pick, again (or the preview) to use / test it; tap the slider to set the time; any tap stops the saver.

static const char *effect_name(int mode)
{
    switch (mode) {
        case MODE_OFF:   return TR("Spento", "Off");
        case MODE_STARS: return TR("Stelle", "Stars");
        case MODE_FIRE:  return TR("Fuoco", "Fire");
        default:         return TR("Orologio", "Clock");
    }
}
// A still, representative preview of an effect inside a box (the gallery is static; on device it is a glance).
static void preview_effect(int mode, int x, int y, int w, int h)
{
    d.fillRoundRect(x, y, w, h, 8, 0x0000);
    d.drawRoundRect(x, y, w, h, 8, LINE);
    const int cx = x + w / 2, cy = y + h / 2;
    if (mode == MODE_OFF) {
        lg::font(1); d.setTextColor(MUTED, 0x0000); d.setTextDatum(middle_center);
        d.drawString(TR("Schermo spento", "Screen off"), cx, cy); d.setTextDatum(top_left); return;
    }
    if (mode == MODE_CLOCK) {
        const uint16_t col = CL_COLS[2];
        d.fillRoundRect(cx - 74, cy - 22, 148, 44, 4, (uint16_t)((col >> 3) & 0x1CE3));
        d.setFont(&fonts::Font0); d.setTextColor(col, 0x0000); d.setTextSize(3); d.setTextDatum(middle_center);
        d.drawString("12:34:56", cx, cy - 4);
        d.setTextSize(1); d.setTextColor(0x8C71, 0x0000); d.drawString("Mar 05 Mag", cx, cy + 18);
        d.setTextDatum(top_left); return;
    }
    if (mode == MODE_STARS) {
        srand(12345);
        for (int i = 0; i < 44; i++) {
            const int sx = x + 5 + rand() % (w - 10), sy = y + 5 + rand() % (h - 10);
            const int b = 90 + rand() % 165, sz = b > 210 ? 2 : 1;
            d.fillRect(sx, sy, sz, sz, d.color565(b, b, (i % 4 == 0) ? 255 : b));
        }
        return;
    }
    for (int ry = 0; ry < h - 2; ry++) {             // FIRE: a hot-to-cool vertical gradient
        const int i = (h - 2 - ry) * 255 / (h - 2);
        int r, g, b;
        if      (i < 40)  { r = i * 6;  g = 0;              b = 0; }
        else if (i < 120) { r = 255;    g = (i - 40) * 3;   b = 0; }
        else if (i < 200) { r = 255;    g = 200 + (i - 120) / 4; b = (i - 120) / 3; }
        else              { r = 255;    g = 255;            b = 100 + (i - 200); }
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        d.drawFastHLine(x + 2, y + 1 + ry, w - 4, d.color565(r, g, b));
    }
}

// A row of the panel's list (lg::settings_list): an effect (a check = in use), Test, Timer.
static void lg_mode_row(int i, lg::Setting *s, void *)
{
    s->label = effect_name(i);
    if (i == g_mode) { s->rglyph = UG_CHECK; s->vcol = C_GREEN; }
    if (i == 4) { s->label = TR("Prova", "Test"); s->kind = lg::ROW_NAV; s->lcol = C_YELLOW; }   // Font2 has no "►"
    if (i == 5) { s->label = "Timer"; s->kind = lg::ROW_NAV; }        // its value: the header and the pane
}

// The timer pane: the value big, its state, and a slider over 5 s .. 10 min (tap anywhere on it to set).
static void lg_timer(int x, int y, int w, int h)
{
    char sv[20]; fmt_timer(sv, sizeof sv);
    lg::panel(x, y, w, h);
    const unsigned short cb = lg::card();
    lg::font(1); d.setTextColor(MUTED, cb); d.setTextDatum(top_center);
    d.drawString(TR("Si attiva dopo", "Starts after"), x + w / 2, y + 12);
    lg::font(3); d.setTextColor(C_YELLOW, cb); d.drawString(sv, x + w / 2, y + 36);
    lg::font(0); d.setTextColor(g_threshold_ms > 0 ? C_GREEN : C_RED, cb);
    d.drawString(g_threshold_ms > 0 ? TR("Attivo", "On") : TR("Disabilitato", "Disabled"), x + w / 2, y + 74);
    const int sec = (int)(g_threshold_ms / 1000), ty = y + h - 52;
    lg::slider(SL_X, ty + 4, SL_W, sec < 5 ? 0 : (sec - 5) * 100 / 595, C_YELLOW, 8);
    lg::font(4); d.setTextColor(MUTED, cb);
    d.setTextDatum(top_left);  d.drawString("5s", SL_X, ty + 18);
    d.setTextDatum(top_right); d.drawString("10 min", SL_X + SL_W, ty + 18);
    d.setTextDatum(top_left);
    lg::field(x, ty - 20, w, 46);
}

static void draw_large(void)
{
    nucleo_touch_clear();
    if (s_running) {                                 // the saver itself, native on the panel; any tap ends it
        effect_step();
        lg::frame(effect_draw, nullptr);
        nucleo_touch_commit();
        return;
    }
    char rt[20]; fmt_timer(rt, sizeof rt);
    const int y = lg::header(TR("Salvaschermo", "Screensaver"), rt, C_PURPLE);
    lg::settings_list(8, y + 2, PX - 12, 240 - y - 2, 34, 6, s_tab ? 5 : s_sel, &s_lg_scroll, lg_mode_row, nullptr, C_PURPLE);
    d.fillRect(0, y, 8, 240 - y, BG);                // the margin left of the list (drawn straight, no clear)
    d.fillRect(PX - 4, y, 324 - PX, 240 - y, BG);
    const int py = y + 6, ph = 240 - y - 12;
    if (s_tab) { lg_timer(PX, py, PW, ph); nucleo_touch_commit(); return; }
    preview_effect(s_sel < 4 ? s_sel : g_mode, PX, py, PW, ph - 28);
    nucleo_touch_add(PX, py, PW, ph - 28, NK_ENTER, 0);   // tap the preview = use it / test it
    lg::font(0); d.setTextColor(MUTED, BG); d.setTextDatum(top_center);
    d.drawString(s_sel == 4 ? TR("Tocca per provare", "Tap to test") : TR("Tocca di nuovo per usarlo", "Tap again to use it"),
                 PX + PW / 2, py + ph - 20);
    d.setTextDatum(top_left);
    nucleo_touch_commit();
}
static void draw_companion(void)
{
    const int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);
    const int y = lg::comp_head("SALVASCHERMO", C_PURPLE);
    if (s_running) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(C_GREEN, BG);
        d.drawString(TR("In esecuzione", "Running"), 8, y + 10);
        d.setFont(&fonts::Font0); d.setTextColor(FG, BG); d.drawString(effect_name(g_mode), 8, y + 36);
        lg::comp_keys(TR("un tasto per uscire", "a key to exit")); return;
    }
    if (s_tab == 0) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(s_sel == 4 ? C_YELLOW : (s_sel == g_mode ? C_GREEN : FG), BG);
        d.drawString(s_sel == 4 ? TR("Prova", "Test") : effect_name(s_sel), 8, y + 10);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        const char *dsc = mode_right(s_sel, nullptr);
        if (dsc) d.drawString(dsc, 8, y + 34);
        char p[20]; snprintf(p, sizeof p, "%d / 5", s_sel + 1);
        d.setTextColor(DIM, BG); d.drawString(p, 8, y + 50);
        lg::comp_keys(TR("su/giu  INVIO usa/prova  TAB timer", "up/dn  ENTER use/test  TAB timer"));
    } else {
        char sv[20]; fmt_timer(sv, sizeof sv);
        d.setFont(&fonts::FreeSansBold12pt7b); d.setTextColor(C_YELLOW, BG); d.drawString(sv, 8, y + 14);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); d.drawString(TR("attivazione automatica", "auto activation"), 8, y + 46);
        lg::comp_keys(TR("</> 5s  su/giu 30s  TAB effetto", "</> 5s  up/dn 30s  TAB effect"));
    }
}

// on_draw: for the saver, draws the full frame on the canvas (Technique 1).
// For settings, redraws only when s_dirty.
static void on_draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    if (s_running) {
        if (g_mode != MODE_OFF) { effect_step(); effect_draw(nullptr); }   // off: backlight 0, nothing to draw
        return;
    }
    if (!s_dirty) return;
    s_dirty = false;
    draw_settings();
}

static void on_exit(void) { if (s_running) saver_stop(); save_settings(); free(fire_buf); fire_buf = nullptr; }

// ==================== SYSTEM HOOKS =============================================
// The saved timeout/style used to be read only when this app was opened, so after a reboot the saver
// always fired at the 60 s default. Load once, lazily, from whichever entry point runs first.
static void cfg_ensure(void) { if (!s_cfg_loaded) { s_cfg_loaded = true; load_settings(); } }

extern "C" bool nucleo_screensaver_should_activate(int64_t idle_ms)
{
    cfg_ensure();
    return g_threshold_ms > 0 && idle_ms >= g_threshold_ms;
}

// Settings app hooks (Display > Screensaver). Timeout 0 = never; 5..600 s otherwise.
extern "C" int  nucleo_screensaver_timeout_s(void) { cfg_ensure(); return (int)(g_threshold_ms / 1000); }
extern "C" void nucleo_screensaver_set_timeout_s(int sec)
{
    cfg_ensure();
    if (sec < 0) sec = 0;
    if (sec != 0 && sec < 5) sec = 5;
    if (sec > 600) sec = 600;
    g_threshold_ms = (int32_t)sec * 1000; save_settings();
}
extern "C" int  nucleo_screensaver_mode(void) { cfg_ensure(); return g_mode; }   // 0 off-screen, 1 clock, 2 stars, 3 fire
extern "C" void nucleo_screensaver_set_mode(int mode)
{
    cfg_ensure();
    if (mode < MODE_OFF || mode > MODE_FIRE) return;
    g_mode = mode; save_settings();
}
extern "C" void nucleo_screensaver_set_trigger(void) { g_trigger = true; }

// ==================== REGISTRATION =============================================
extern "C" void nucleo_register_screensaver(void)
{
    static const nucleo_app_def_t app = {
        "screensaver", "Salvaschermo", "System",
        "Schermo spento o animato dopo inattività",
        'S', C_PURPLE,
        on_enter, on_key, on_tick, on_draw, on_exit, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}

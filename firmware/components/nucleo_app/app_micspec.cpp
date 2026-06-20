// Mic Spectrum ("Spettro") — real-time microphone analyzer.
//
// Inspired by Bruce's mic spectrum (a single PSRAM-backed scrolling waterfall) but rebuilt for a
// PSRAM-less Cardputer and pushed much further: ONE DSP engine (nucleo_micspec) feeds FOUR lenses —
// log-perceptual bars with peak-hold + AGC, a frugal scrolling waterfall, an auto-triggered scope,
// and a real autocorrelation TUNER (note + cents). A slim HUD (dB meter, dominant Hz, beat pulse)
// rides on top of every mode. GO-hold freezes the frame (and keeps voice PTT off our mic).
#include "nucleo_app.h"
#include "app_gfx.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
extern "C" {
#include "nucleo_micspec.h"
}
#include "nucleo_exclusive.h"   // dedicated-mode RAM reclaim (~70KB) + NX_VOICE frees the mic — like music/video

#include "launcher_theme.h"

// ---- modes ----
enum { M_BARS = 0, M_FALL, M_SCOPE, M_TUNE, N_MODES };
static const char *MODE_NAME[N_MODES] = { "BARRE", "CASCATA", "ONDA", "ACCORDATORE" };

static int   s_mode    = M_BARS;
static int   s_pal     = 0;        // palette index
static int   s_sens    = 100;      // manual sensitivity %, 40..250
static bool  s_frozen  = false;
static ms_snapshot_t s_snap;       // last copied frame
static bool  s_ok      = false;    // got at least one frame
static uint32_t s_seen = 0;        // last seq we folded into caps/history
static bool  s_was_running = false; // last polled engine running-state (edge -> one repaint)

// peak-hold caps (bars), slow fall
static float s_cap[MS_BANDS];

// waterfall history (lazy 7.4 KB; freed on exit). Ring of columns, each MS_BANDS tall.
#define HIST_W 232
static uint8_t *s_hist = nullptr;
static int s_hist_head = 0, s_hist_n = 0;

// ---- geometry ----
static const int HUD_H = 15;
static const int MAIN_T = 18;
static const int MAIN_B = 113;
static const int MAIN_H = MAIN_B - MAIN_T;   // 95
static const int CONTENT_B = H - HINT;       // 121

// ---- color helpers ----
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static inline uint16_t rgb565(int r, int g, int b)
{
    r = clamp8(r); g = clamp8(g); b = clamp8(b);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
struct stop_t { uint8_t r, g, b; };
static const stop_t PAL_THERMAL[] = { {0,0,8},{0,0,150},{0,185,205},{40,220,40},{255,220,0},{255,40,0} };
static const stop_t PAL_AURORA[]  = { {2,2,22},{0,120,95},{40,232,140},{120,80,235},{245,120,225} };
static const stop_t PAL_MONO[]    = { {0,8,6},{0,70,55},{30,170,140},{160,245,210} };
static const struct { const stop_t *s; int n; const char *name; } PALS[] = {
    { PAL_THERMAL, 6, "Thermal" }, { PAL_AURORA, 5, "Aurora" }, { PAL_MONO, 4, "Mono" },
};
static uint16_t grad(float t)
{
    const stop_t *s = PALS[s_pal].s; int n = PALS[s_pal].n;
    if (t <= 0) return rgb565(s[0].r, s[0].g, s[0].b);
    if (t >= 1) return rgb565(s[n-1].r, s[n-1].g, s[n-1].b);
    float fp = t * (n - 1); int i = (int)fp; float f = fp - i;
    return rgb565((int)(s[i].r + (s[i+1].r - s[i].r) * f),
                  (int)(s[i].g + (s[i+1].g - s[i].g) * f),
                  (int)(s[i].b + (s[i+1].b - s[i].b) * f));
}
static uint16_t level_col(int pct) { return pct >= 85 ? C_RED : pct >= 55 ? C_YELLOW : C_GREEN; }

// 256-entry gradient LUT for the active palette. grad() does a float lerp between palette stops; the
// waterfall calls it for EVERY cell (up to 232*32 ~= 7400 per frame), which dominated the mode's CPU
// and made frames arrive unevenly (read as flicker). Precompute once per palette and index by the
// 0..255 band byte instead of recomputing the lerp. Rebuilt in enter()/on_tab(), never per frame. 512 B.
static uint16_t s_grad_lut[256];
static void build_grad_lut(void) { for (int i = 0; i < 256; i++) s_grad_lut[i] = grad((float)i / 255.0f); }
static inline uint16_t grad8(uint8_t v) { return s_grad_lut[v]; }

// Apply manual sensitivity to a 0..255 band value.
static inline int sens_apply(int v) { int r = v * s_sens / 100; return r > 255 ? 255 : r; }

// ---- HUD: mode name, dB meter, dominant Hz, beat pulse ----
static void draw_hud(void)
{
    d.fillRect(0, 0, W, HUD_H, BG);
    d.setTextSize(1);
    d.setTextColor(grad(0.85f), BG);
    d.setCursor(4, 4); d.print(MODE_NAME[s_mode]);

    // dB meter bar (right of the name)
    int mx = 86, mw = 92;
    int lv = s_ok ? s_snap.level : 0;
    d.drawRoundRect(mx, 3, mw, 9, 2, 0x2945);
    d.fillRoundRect(mx, 3, mw * lv / 100, 9, 2, level_col(lv));

    // dominant frequency readout
    char hz[16];
    if (s_ok && s_snap.dom_hz > 0) snprintf(hz, sizeof hz, "%d Hz", s_snap.dom_hz);
    else snprintf(hz, sizeof hz, "-- Hz");
    d.setTextColor(MUTED, BG);
    d.setCursor(mx + mw + 6, 4); d.print(hz);

    // beat pulse dot (onset) far right + frozen tag
    int on = s_ok ? s_snap.onset : 0;
    if (on > 30) { int r = 2 + on / 60; d.fillCircle(W - 8, 7, r, grad(0.9f)); }
    d.fillCircle(W - 8, 7, 2, 0xFFFF);

    d.drawFastHLine(0, HUD_H, W, on > 80 ? grad(0.7f) : (uint16_t)0x2945);

    if (s_frozen) {
        d.setTextColor(C_YELLOW, BG);
        d.setCursor(W - 70, 4); d.print("|| FERMO");
    }
}

// pager dots + sensitivity tag at the very bottom of the content area
static void draw_pager(void)
{
    int y = CONTENT_B - 4, gap = 12, x0 = (W - (N_MODES - 1) * gap) / 2;
    for (int i = 0; i < N_MODES; i++) {
        if (i == s_mode) d.fillCircle(x0 + i * gap, y, 3, grad(0.85f));
        else             d.fillCircle(x0 + i * gap, y, 2, 0x4208);
    }
    char tg[20]; snprintf(tg, sizeof tg, "%s  x%d.%d", PALS[s_pal].name, s_sens / 100, (s_sens % 100) / 10);
    d.setTextSize(1); d.setTextColor(DIM, BG);
    d.setCursor(4, y - 3); d.print(tg);
}

// ---- mode: BARS ----
static void draw_bars(void)
{
    int n = MS_BANDS, gap = 1;
    int bw = (W - 8 - (n - 1) * gap) / n;            // ~6 px
    int x = 4, base = MAIN_B;
    for (int b = 0; b < n; b++) {
        int v = sens_apply(s_snap.bands[b]);
        int hgt = v * MAIN_H / 255;
        // fixed vertical gradient revealed by bar height (classic analyzer)
        for (int yy = base; yy > base - hgt; yy -= 3) {
            d.fillRect(x, yy - 2, bw, 3, grad8((uint8_t)((base - yy) * 255 / MAIN_H)));
        }
        // peak-hold cap
        int capv = (int)s_cap[b];
        int cy = base - capv * MAIN_H / 255;
        if (capv > 4) d.fillRect(x, cy, bw, 2, 0xFFFF);
        x += bw + gap;
    }
}

// ---- mode: WATERFALL ----
static void draw_fall(void)
{
    if (!s_hist) { draw_bars(); return; }              // OOM fallback
    int rowh = (MAIN_H + MS_BANDS - 1) / MS_BANDS;      // ~3 px
    int show = s_hist_n < HIST_W ? s_hist_n : HIST_W;
    for (int c = 0; c < show; c++) {
        int col = (s_hist_head - show + c + HIST_W * 2) % HIST_W;
        const uint8_t *cell = &s_hist[col * MS_BANDS];
        int x = 4 + (W - 8 - show) + c;                 // newest at the right edge
        for (int b = 0; b < MS_BANDS; b++) {
            int yy = MAIN_B - (b + 1) * rowh;           // low freq at bottom
            d.fillRect(x, yy, 1, rowh, grad8(cell[b]));
        }
    }
}

// ---- mode: SCOPE ----
static void draw_scope(void)
{
    int cy = (MAIN_T + MAIN_B) / 2;
    d.drawFastHLine(4, cy, W - 8, 0x2945);
    // auto vertical gain so quiet signals stay visible
    int peak = 1;
    for (int i = 0; i < MS_WAVE; i++) { int a = s_snap.wave[i]; if (a < 0) a = -a; if (a > peak) peak = a; }
    int amp = (MAIN_H / 2 - 2);
    int px = 4, py = cy;
    for (int i = 0; i < MS_WAVE && i < W - 8; i++) {
        int v = (int)((long)s_snap.wave[i] * amp * s_sens / 100 / peak);
        int y = cy - v;
        if (y < MAIN_T) y = MAIN_T;
        else if (y > MAIN_B) y = MAIN_B;
        int x = 4 + i;
        if (i) d.drawLine(px, py, x, y, grad(0.7f));
        px = x; py = y;
    }
}

// ---- mode: TUNER ----
static void draw_tuner(void)
{
    bool voiced = s_ok && s_snap.note_idx >= 0;
    int cents = voiced ? s_snap.cents : 0;
    uint16_t col = !voiced ? DIM : (cents > -6 && cents < 6) ? C_GREEN
                                  : (cents > -20 && cents < 20) ? C_YELLOW : C_RED;

    // big note letter, centered
    const char *nm = nucleo_micspec_note_name(voiced ? s_snap.note_idx : -1);
    d.setTextSize(6);
    int lw = (int)strlen(nm) * 36;
    d.setTextColor(col, BG);
    d.setCursor((W - lw) / 2 - 8, MAIN_T + 6); d.print(nm);
    if (voiced) {
        char oc[4]; snprintf(oc, sizeof oc, "%d", s_snap.octave);
        d.setTextSize(2); d.setCursor((W + lw) / 2 - 6, MAIN_T + 8); d.print(oc);
    }

    // cents needle gauge
    int gy = MAIN_B - 22, gx = 20, gw = W - 40;
    d.drawFastHLine(gx, gy, gw, 0x4208);
    for (int t = -50; t <= 50; t += 10) {                       // ticks
        int tx = gx + gw * (t + 50) / 100;
        int th = (t == 0) ? 7 : 3;
        d.drawFastVLine(tx, gy - th, th * 2, t == 0 ? C_GREEN : (uint16_t)0x4208);
    }
    if (voiced) {
        int cc = cents;
        if (cc < -50) cc = -50;
        else if (cc > 50) cc = 50;
        int nx = gx + gw * (cc + 50) / 100;
        d.fillTriangle(nx, gy - 9, nx - 5, gy - 16, nx + 5, gy - 16, col);
        d.fillRect(nx - 1, gy - 9, 3, 14, col);
    }

    // numeric readouts
    char ln[32];
    d.setTextSize(1);
    if (voiced) {
        snprintf(ln, sizeof ln, "%+d cent", cents);
        d.setTextColor(col, BG); d.setCursor((W - (int)strlen(ln) * 6) / 2, MAIN_B - 4); d.print(ln);
        snprintf(ln, sizeof ln, "%d.%02d Hz   chiarezza %d%%", s_snap.pitch_cHz / 100, s_snap.pitch_cHz % 100, s_snap.clarity);
    } else {
        snprintf(ln, sizeof ln, "suona o canta una nota...");
    }
    d.setTextColor(MUTED, BG); d.setCursor((W - (int)strlen(ln) * 6) / 2, MAIN_T - 1); d.print(ln);
}

// ---- snapshot folding (caps + waterfall history advance on a new frame) ----
static void fold_frame(void)
{
    // peak-hold caps
    for (int b = 0; b < MS_BANDS; b++) {
        int v = sens_apply(s_snap.bands[b]);
        if (v > s_cap[b]) s_cap[b] = v; else { s_cap[b] -= 3.0f; if (s_cap[b] < 0) s_cap[b] = 0; }
    }
    // push a waterfall column
    if (s_hist) {
        uint8_t *cell = &s_hist[s_hist_head * MS_BANDS];
        for (int b = 0; b < MS_BANDS; b++) cell[b] = (uint8_t)sens_apply(s_snap.bands[b]);
        s_hist_head = (s_hist_head + 1) % HIST_W;
        if (s_hist_n < HIST_W) s_hist_n++;
    }
}

// ---- per-loop poll (framework, ~50 Hz): copy a new frame + fold it, and report whether the screen
// needs a blit. Returning true ONLY on a fresh seq (or a running-state edge) gates the full-frame
// redraw to the DSP's ~31 Hz output instead of the loop's 50 Hz — no duplicate-frame repaints, which
// is the cadence redraw ANTI-FLICKER.md #4 forbids and the main source of the on-screen flicker.
static bool poll(void)
{
    if (s_frozen) return false;                       // frozen: the frame is held -> never auto-redraw
    bool run = nucleo_micspec_running();
    if (run != s_was_running) { s_was_running = run; return true; }  // started/stopped -> repaint once (live <-> splash)
    if (!run) return false;                           // error/idle splash is static
    if (nucleo_micspec_get(&s_snap) && (!s_ok || s_snap.seq != s_seen)) {
        s_ok = true; s_seen = s_snap.seq; fold_frame();
        return true;                                  // a genuinely new analysis frame -> blit
    }
    return false;                                     // same seq this loop -> skip the redraw entirely
}

// ---- error / waiting splash ----
static void draw_splash(const char *msg, uint16_t col)
{
    d.setTextSize(2); d.setTextColor(col, BG);
    d.setCursor((W - (int)strlen(msg) * 12) / 2, MAIN_T + 30); d.print(msg);
}

// ---- lifecycle ----
static void draw(void)
{
    d.fillRect(0, 0, W, CONTENT_B, BG);   // self-clear; matters only on the direct-draw fallback, harmless when buffered

    if (!nucleo_micspec_running()) {
        int e = nucleo_micspec_last_error();
        draw_hud();
        if (e == MS_ERR_BUSY) draw_splash("Mic occupato", C_YELLOW);
        else if (e == MS_ERR_OOM) draw_splash("Memoria insuff.", C_RED);
        else draw_splash("Mic non avviato", C_RED);
        return;
    }

    // Frame copy + caps/waterfall fold happen in poll() (framework loop), so draw() is a pure render:
    // it runs only when poll() saw a new frame, and the blit cadence tracks the DSP, not the loop.
    draw_hud();
    if (!s_ok) { draw_splash("...", MUTED); }
    else switch (s_mode) {
        case M_BARS:  draw_bars();  break;
        case M_FALL:  draw_fall();  break;
        case M_SCOPE: draw_scope(); break;
        case M_TUNE:  draw_tuner(); break;
    }
    draw_pager();
}

static void set_hint(void)
{
    nucleo_app_set_hint("L/R modo  1-4 salta  TAB tema  U/D sens  GO ferma");
}

static void on_key(int key, char ch)
{
    if (key == NK_RIGHT) s_mode = (s_mode + 1) % N_MODES;
    else if (key == NK_UP)   { s_sens += 10; if (s_sens > 250) s_sens = 250; }
    else if (key == NK_DOWN) { s_sens -= 10; if (s_sens < 40)  s_sens = 40; }
    else if (ch >= '1' && ch <= '0' + N_MODES) s_mode = ch - '1';
    else if (ch == 'f' || ch == 'F') s_frozen = !s_frozen;
    else return;
    nucleo_app_request_draw();
}

static bool on_back(int key)
{
    if (key == NK_LEFT) { s_mode = (s_mode + N_MODES - 1) % N_MODES; nucleo_app_request_draw(); return true; }
    return false;   // Esc -> close
}

static void on_tab(void) { s_pal = (s_pal + 1) % (int)(sizeof(PALS) / sizeof(PALS[0])); build_grad_lut(); nucleo_app_request_draw(); }

static void on_ptt(bool holding) { s_frozen = holding; nucleo_app_request_draw(); }   // hold GO = freeze (also blocks voice PTT on our mic)

static void enter(void)
{
    s_ok = false; s_seen = 0; s_frozen = false; s_was_running = false;
    build_grad_lut();                  // palette persists across sessions; prime the LUT before the first draw
    memset(s_cap, 0, sizeof s_cap);
    s_hist_head = 0; s_hist_n = 0;

    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_set_ptt_handler(on_ptt);
    nucleo_app_set_poll_handler(poll);   // drive the redraw from the DSP frame rate, not the 50 Hz loop
    set_hint();
    // Dedicated mode FIRST: NX_NET_APP suspends voice (the other GPIO43/mic owner) and frees ~70KB so the
    // DSP scratch + waterfall never starve. Wi-Fi STA stays up. Restored in on_exit() after the mic closes.
    nucleo_exclusive_enter(NX_NET_APP, nullptr);
    // Waterfall history (7.4 KB) allocated AFTER the reclaim, when the contiguous block is large. On the
    // PSRAM-less chip the largest free block is tiny under normal load, so allocating this BEFORE the
    // reclaim could fail — silently dropping CASCATA to the bars fallback (they then look identical).
    if (!s_hist) s_hist = (uint8_t *)malloc((size_t)HIST_W * MS_BANDS);   // NULL -> waterfall falls back to bars
    if (s_hist) memset(s_hist, 0, (size_t)HIST_W * MS_BANDS);
    nucleo_micspec_start();
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    nucleo_micspec_stop();                 // blocks until the mic RX is fully closed (GPIO43 released)
    nucleo_app_set_ptt_handler(nullptr);
    if (s_hist) { free(s_hist); s_hist = nullptr; }
    nucleo_exclusive_exit();               // ...THEN restore httpd/mDNS/voice/L1 (voice won't re-grab a mic we still held)
}

extern "C" void nucleo_register_micspec(void)
{
    static const nucleo_app_def_t app = {
        "micspec", "Mic Spectrum", "Media", "Analizzatore audio dal microfono",
        'W', C_PURPLE, enter, on_key, nullptr, draw, on_exit,
    };
    nucleo_app_register(&app);
}

// Mic Spectrum ("Spettro") — real-time microphone analyzer.
//
// Inspired by Bruce's mic spectrum (a single PSRAM-backed scrolling waterfall) but rebuilt for a
// PSRAM-less Cardputer and pushed much further: ONE DSP engine (nucleo_micspec) feeds FOUR lenses —
// log-perceptual bars with peak-hold + AGC, a frugal scrolling waterfall, an auto-triggered scope,
// and a real autocorrelation TUNER (note + cents). A readable HUD (big dB + dominant note, a level
// meter, dominant Hz, beat pulse) rides on top of every mode. GO-hold freezes the frame (and keeps
// voice PTT off our mic). Everything composites into the shared back-buffer — one blit, no flicker —
// and every live readout is time-smoothed so neither the bars nor the big digits strobe at ~31 fps.
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

// per-band smoothed display level — THE flicker fix. The DSP publishes RAW per-frame magnitudes that
// jump frame-to-frame with mic noise; rendered straight (s_snap.bands[b]) they read as a violent
// jitter, not a smooth analyzer. Envelope them here with a fast attack / slow decay (the classic
// "falling bars" VU) so motion is fluid. We smooth the RAW value (pre-sensitivity) so a U/D sens
// change stays instant. Reset in enter().
static float s_disp[MS_BANDS];
// scope vertical AGC, enveloped: recomputing the trace peak EVERY frame made quiet signals "breathe"
// (the whole wave rescaled each frame). Track a slow-release peak so the gain holds steady instead.
static float s_scope_gain = 256.0f;
// smoothed HUD readouts — the big digits would strobe if fed the raw per-frame level/dB/Hz.
static float s_lvl = 0.0f;     // meter level 0..100
static float s_db  = -90.0f;   // peak dBFS
static float s_hz  = 0.0f;     // dominant Hz
// tuner stabilisation — the DSP pitch fields are raw per-frame; rendered straight the needle and the
// ALZA/CALA verdict strobe and the tuner is useless to actually tune to. Hold the last note for a beat
// when the sound drops, smooth the cents, and lock an "in tune" state so the verdict can't flicker.
static int   s_tnote = -1, s_toct = 0;   // held note index / octave
static float s_tcents = 0.0f;            // smoothed cents deviation
static int   s_tmiss = 0;                // frames since a voiced pitch (hold-over)
static int   s_tlock = 0;                // consecutive in-tune frames

// waterfall history (lazy 7.4 KB; freed on exit). Ring of columns, each MS_BANDS tall.
#define HIST_W 232
static uint8_t *s_hist = nullptr;
static int s_hist_head = 0, s_hist_n = 0;

// ---- geometry ----
static const int HUD_H = 28;                 // header band: big dB + note, level meter, Hz, beat pulse
static const int MAIN_T = 32;
static const int MAIN_B = 110;
static const int MAIN_H = MAIN_B - MAIN_T;   // 78
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

// ---- HUD: big peak-dB + dominant note (row 1), level meter + dominant Hz + beat (row 2) ----
// All readouts are the SMOOTHED values (s_lvl/s_db/s_hz) so the large digits never strobe.
static void draw_hud(void)
{
    d.fillRect(0, 0, W, HUD_H, BG);
    char buf[16];

    int lvl = s_ok ? (int)(s_lvl + 0.5f) : 0;
    uint16_t lc = level_col(lvl);

    // row 1 — peak dB (left, colour-coded by loudness), big and readable
    d.setTextSize(2);
    if (s_ok && s_db > -85.0f) snprintf(buf, sizeof buf, "%ddB", (int)s_db);
    else                       snprintf(buf, sizeof buf, "--");
    d.setTextColor(lc, BG);
    d.setCursor(4, 1); d.print(buf);

    // row 1 — dominant note (right) ties every mode to the tuner; pause glyph when frozen
    if (s_frozen) {
        d.fillRect(W - 26, 2, 6, 15, C_YELLOW);
        d.fillRect(W - 14, 2, 6, 15, C_YELLOW);
    } else {
        bool has = s_ok && s_snap.note_idx >= 0;
        if (has) snprintf(buf, sizeof buf, "%s%d", nucleo_micspec_note_name(s_snap.note_idx), s_snap.octave);
        else     snprintf(buf, sizeof buf, "--");
        d.setTextSize(2);
        d.setTextColor(has ? THEME_ACC : DIM, BG);
        d.setCursor(W - 4 - (int)strlen(buf) * 12, 1); d.print(buf);
    }

    // row 2 — level meter (left)
    int my = 19, mw = 150, mh = 7;
    d.drawRoundRect(4, my, mw, mh, 2, LINE);
    if (lvl > 0) d.fillRoundRect(4, my, mw * lvl / 100, mh, 2, lc);

    // row 2 — dominant Hz (right of the meter) + beat pulse at the far edge
    d.setTextSize(1);
    if (s_ok && s_hz > 1.0f) snprintf(buf, sizeof buf, "%dHz", (int)(s_hz + 0.5f));
    else                     snprintf(buf, sizeof buf, "--Hz");
    d.setTextColor(MUTED, BG);
    d.setCursor(W - 16 - (int)strlen(buf) * 6, my); d.print(buf);

    int on = s_ok ? s_snap.onset : 0;                 // onset is already enveloped in the DSP -> smooth pulse
    if (on > 24) d.fillCircle(W - 7, my + 3, 2 + on / 70, grad(0.9f));
    d.fillCircle(W - 7, my + 3, 2, FG);

    d.drawFastHLine(0, HUD_H - 1, W, LINE);
}

// bottom strip: mode name (left) · page dots (centre) · palette + sensitivity tag (right)
static void draw_pager(void)
{
    int y = CONTENT_B - 4, gap = 12, x0 = (W - (N_MODES - 1) * gap) / 2;
    for (int i = 0; i < N_MODES; i++)
        d.fillCircle(x0 + i * gap, y, i == s_mode ? 3 : 2, i == s_mode ? THEME_ACC : DIM);

    d.setTextSize(1);
    d.setTextColor(THEME_ACC, BG);
    d.setCursor(4, y - 3); d.print(MODE_NAME[s_mode]);

    char tg[24]; snprintf(tg, sizeof tg, "%s x%d.%d", PALS[s_pal].name, s_sens / 100, (s_sens % 100) / 10);
    d.setTextColor(DIM, BG);
    d.setCursor(W - 4 - (int)strlen(tg) * 6, y - 3); d.print(tg);
}

// ---- mode: BARS ----
static void draw_bars(void)
{
    int n = MS_BANDS, gap = 1;
    int bw = (W - 8 - (n - 1) * gap) / n;            // ~6 px
    int x = 4, base = MAIN_B;
    d.drawFastHLine(4, base + 1, W - 8, LINE);       // baseline so silent bars still read as a floor
    for (int b = 0; b < n; b++) {
        int v = sens_apply((int)(s_disp[b] + 0.5f)); // smoothed, not raw -> fluid (no per-frame jitter)
        int hgt = v * MAIN_H / 255;
        // fixed vertical gradient revealed by bar height (classic analyzer)
        for (int yy = base; yy > base - hgt; yy -= 3) {
            d.fillRect(x, yy - 2, bw, 3, grad8((uint8_t)((base - yy) * 255 / MAIN_H)));
        }
        // peak-hold cap (bright, high-contrast against any palette)
        int capv = (int)s_cap[b];
        int cy = base - capv * MAIN_H / 255;
        if (capv > 4) d.fillRect(x, cy, bw, 2, FG);
        x += bw + gap;
    }
}

// ---- mode: WATERFALL ----
static void draw_fall(void)
{
    if (!s_hist) { draw_bars(); return; }              // OOM fallback
    int show = s_hist_n < HIST_W ? s_hist_n : HIST_W;
    for (int c = 0; c < show; c++) {
        int col = (s_hist_head - show + c + HIST_W * 2) % HIST_W;
        const uint8_t *cell = &s_hist[col * MS_BANDS];
        int x = 4 + (W - 8 - show) + c;                 // newest at the right edge
        for (int b = 0; b < MS_BANDS; b++) {
            // map the 32 bands proportionally over MAIN_H so the column fills the area exactly
            int y0 = MAIN_B - (b + 1) * MAIN_H / MS_BANDS;   // low freq at bottom
            int y1 = MAIN_B -  b      * MAIN_H / MS_BANDS;
            d.fillRect(x, y0, 1, y1 - y0, grad8(cell[b]));
        }
    }
}

// ---- mode: SCOPE ----
static void draw_scope(void)
{
    int cy = (MAIN_T + MAIN_B) / 2;
    // enveloped vertical gain (s_scope_gain, updated in fold_frame) so quiet signals stay visible
    // WITHOUT the whole trace rescaling every frame — that per-frame renormalize was the "breathing".
    long g = (long)s_scope_gain; if (g < 1) g = 1;
    int amp = (MAIN_H / 2 - 2);
    uint16_t wc = grad(0.78f);                        // bright trace; constant per frame (lerp hoisted out)
    uint16_t fc = grad(0.30f);                        // dim body fill under the trace
    int px = 4, py = cy;
    for (int i = 0; i < MS_WAVE && i < W - 8; i++) {
        int v = (int)((long)s_snap.wave[i] * amp * s_sens / 100 / g);
        int y = cy - v;
        if (y < MAIN_T) y = MAIN_T;
        else if (y > MAIN_B) y = MAIN_B;
        int x = 4 + i;
        if (y < cy)      d.drawFastVLine(x, y, cy - y, fc);   // fill centreline -> sample (both directions)
        else if (y > cy) d.drawFastVLine(x, cy, y - cy, fc);
        if (i) d.drawLine(px, py, x, y, wc);
        px = x; py = y;
    }
}

// ---- mode: TUNER ----
static void draw_tuner(void)
{
    if (s_tnote < 0) {                                          // nothing held: clear prompt, big enough to read
        d.setTextSize(2); d.setTextColor(MUTED, BG);
        const char *p = "Suona una nota";
        d.setCursor((W - (int)strlen(p) * 12) / 2, MAIN_T + 22); d.print(p);
        return;
    }
    bool intune = s_tlock >= 4;                                 // persisted in-tune -> green lock, not a 1-frame flash
    bool held   = s_tmiss > 0;                                  // sound dropped: showing the held note, dimmed
    int  cents  = (int)(s_tcents < 0 ? s_tcents - 0.5f : s_tcents + 0.5f);
    uint16_t col = intune ? C_GREEN
                 : (s_tcents > -15.0f && s_tcents < 15.0f) ? C_YELLOW : C_RED;

    // in-tune lock: a green frame + word so you KNOW the moment you've nailed it
    if (intune && !held) {
        d.drawRect(2, MAIN_T - 2, W - 4, 50, C_GREEN);
        d.setTextSize(1); d.setTextColor(C_GREEN, BG);
        d.setCursor(W - 50, MAIN_T - 1); d.print("INTONATO");
    }

    // big note letter (left) + octave; dimmed while it's only the held (no live sound) note
    const char *nm = nucleo_micspec_note_name(s_tnote);
    d.setTextSize(6);
    int lw = (int)strlen(nm) * 36, nx = 18;
    d.setTextColor(held ? DIM : col, BG);
    d.setCursor(nx, MAIN_T + 1); d.print(nm);
    char oc[4]; snprintf(oc, sizeof oc, "%d", s_toct);
    d.setTextSize(2); d.setTextColor(held ? DIM : FG, BG);
    d.setCursor(nx + lw + 3, MAIN_T + 26); d.print(oc);

    // actionable verdict (right): flat or sharp? — large + colour-coded, with a dead-band around centre
    const char *dir = intune ? "OK" : (s_tcents > 4.0f) ? "CALA" : (s_tcents < -4.0f) ? "ALZA" : "OK";
    d.setTextSize(3); d.setTextColor(col, BG);
    d.setCursor(W - 6 - (int)strlen(dir) * 18, MAIN_T + 12); d.print(dir);

    // cents needle gauge — ticks and needle sit ABOVE the baseline so the readout can use the row below
    int gy = MAIN_B - 14, gx = 16, gw = W - 32;
    d.drawFastHLine(gx, gy, gw, DIM);
    for (int t = -50; t <= 50; t += 10) {
        int tx = gx + gw * (t + 50) / 100, th = (t == 0) ? 6 : 3;
        d.drawFastVLine(tx, gy - th, th, t == 0 ? C_GREEN : DIM);
    }
    float cc = s_tcents < -50.0f ? -50.0f : s_tcents > 50.0f ? 50.0f : s_tcents;
    int px = gx + (int)(gw * (cc + 50.0f) / 100.0f);
    d.fillTriangle(px, gy, px - 6, gy - 12, px + 6, gy - 12, col);   // points down onto the line

    // numeric readout (bottom row, clear of the pager dots)
    char ln[40];
    snprintf(ln, sizeof ln, "%+d cent   %d.%02dHz   chiar. %d%%",
             cents, s_snap.pitch_cHz / 100, s_snap.pitch_cHz % 100, s_snap.clarity);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor((W - (int)strlen(ln) * 6) / 2, gy + 4); d.print(ln);
}

// ---- snapshot folding (caps + waterfall history advance on a new frame) ----
static void fold_frame(void)
{
    // smooth each band (fast attack, slow decay) and ride the peak-hold cap off the SMOOTHED level so
    // bars + waterfall move fluidly instead of strobing the raw DSP output (the on-screen "flicker").
    for (int b = 0; b < MS_BANDS; b++) {
        float raw = (float)s_snap.bands[b];                       // 0..255, pre-sensitivity
        float a   = (raw > s_disp[b]) ? 0.55f : 0.22f;            // snap up, ease down (~40ms / ~115ms)
        s_disp[b] += (raw - s_disp[b]) * a;
        int sv = sens_apply((int)(s_disp[b] + 0.5f));
        if (sv > s_cap[b]) s_cap[b] = sv; else { s_cap[b] -= 3.0f; if (s_cap[b] < 0) s_cap[b] = 0; }
    }
    // scope AGC: envelope the trace peak (slow release) so the wave doesn't rescale every frame
    int wpeak = 1;
    for (int i = 0; i < MS_WAVE; i++) { int amp = s_snap.wave[i]; if (amp < 0) amp = -amp; if (amp > wpeak) wpeak = amp; }
    if (wpeak > s_scope_gain) s_scope_gain = (float)wpeak; else s_scope_gain = s_scope_gain * 0.92f + wpeak * 0.08f;
    if (s_scope_gain < 64.0f) s_scope_gain = 64.0f;               // floor: don't over-amplify pure silence
    // smooth the HUD numbers — raw level/dB/Hz jump every frame, the big digits would flicker
    float lv = (float)s_snap.level;
    s_lvl += (lv - s_lvl) * (lv > s_lvl ? 0.5f : 0.2f);
    s_db  += ((float)s_snap.level_db - s_db) * 0.25f;
    if (s_snap.dom_hz > 0) s_hz += ((float)s_snap.dom_hz - s_hz) * 0.30f;
    // tuner: hold + smooth so the readout is steady enough to tune to (raw note/cents jump every frame)
    if (s_snap.note_idx >= 0) {
        if (s_snap.note_idx == s_tnote && s_snap.octave == s_toct)
            s_tcents += ((float)s_snap.cents - s_tcents) * 0.30f;   // same note: ease the cents
        else { s_tnote = s_snap.note_idx; s_toct = s_snap.octave; s_tcents = (float)s_snap.cents; }  // new note: snap
        s_tmiss = 0;
        bool intune = (s_tcents > -4.0f && s_tcents < 4.0f);
        s_tlock = intune ? (s_tlock < 30 ? s_tlock + 1 : 30) : 0;
    } else if (s_tmiss < 12) { s_tmiss++; }                          // ~0.4s hold-over: don't blink out on a gap
    else { s_tnote = -1; s_tlock = 0; }
    // push a waterfall column from the smoothed level (same source as the bars)
    if (s_hist) {
        uint8_t *cell = &s_hist[s_hist_head * MS_BANDS];
        for (int b = 0; b < MS_BANDS; b++) cell[b] = (uint8_t)sens_apply((int)(s_disp[b] + 0.5f));
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
    nucleo_app_set_hint("L/R modo  TAB tema  U/D sens  GO ferma");
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
    memset(s_disp, 0, sizeof s_disp);
    s_scope_gain = 256.0f;
    s_lvl = 0.0f; s_db = -90.0f; s_hz = 0.0f;
    s_tnote = -1; s_toct = 0; s_tcents = 0.0f; s_tmiss = 0; s_tlock = 0;
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

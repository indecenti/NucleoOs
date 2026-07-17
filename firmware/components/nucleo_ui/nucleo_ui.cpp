// On-device UI implemented with M5GFX (display, auto-detects Cardputer) + nucleo_kbd.
// Smartwatch UI version (double buffered, smooth animations)
#include "nucleo_ui.h"
#include "nucleo_kbd.h"
#include "nucleo_power.h"   // real battery level for the header gauge
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"   // modal loops below run on the watchdog-watched launcher task
#include "esp_log.h"
#include "esp_heap_caps.h"  // largest-free-block probe for the splash canvas fallback

M5GFX d;

// Foreground-app draw target (see app_gfx.h). Apps draw through nucleo_app_gfx(); the run
// loop points it at an off-screen canvas for a flicker-free composite, then back to &d.
static LovyanGFX *s_gfx_target = &d;
LovyanGFX *nucleo_app_gfx(void)         { return s_gfx_target; }

// ---- panel readback (for /api/screen) --------------------------------------
// Read the physical ST7789 directly, so a screenshot works even when the off-screen 32 KB canvas
// isn't allocated (the device draws direct-to-panel to save RAM on this no-PSRAM chip — the canvas
// is the exception, not the rule). The Cardputer's panel returns BYTE-SWAPPED RGB565 on readback
// (0x07E0 reads back as 0xE007); the caller un-swaps. Always available, no heap needed.
extern "C" void nucleo_ui_panel_size(int *w, int *h) { if (w) *w = d.width(); if (h) *h = d.height(); }
extern "C" bool nucleo_ui_read_row(int y, int w, uint16_t *out)
{
    if (!out || y < 0 || y >= d.height() || w <= 0 || w > d.width()) return false;
    d.readRect(0, y, w, 1, out);
    return true;
}
bool       nucleo_app_is_buffered(void) { return s_gfx_target != &d; }
void       nucleo_app_set_gfx(LovyanGFX *g) { s_gfx_target = g ? g : &d; }

// ---- shared off-screen back-buffer ------------------------------------------------------
// ONE canvas, reused by the launcher list band AND every foreground app — they are mutually
// exclusive (only one is on screen at a time). Allocated ONCE here at boot, while the heap is
// still clean (nucleo_ui_init runs before Wi-Fi/HTTP/mDNS fragment it), and KEPT for the whole
// session. That is the whole point: the buffered render path never has to re-allocate under
// memory pressure, which is exactly what used to fail and drop the scrolling list to a
// flickering direct draw. Blocking media modals (video/music) free it for the decoder via
// nucleo_screen_release() and it is lazily re-acquired afterwards (best-effort 16bpp->8bpp).
// Full-screen (240x135) so it covers EVERY composite: foreground-app content (240x121, pushed
// clipped to H-HINT), the list band (240x93), AND the Control Center sheet which paints the whole
// screen (a shorter canvas left the CC's bottom border clipped + stale rows below it). 240*135 @
// 8bpp = 32400 B. It used to be 137 (32880 B) so media players could borrow the spare rows as a
// frame buffer — but 32880 > the ~32768 contiguous block left after an audio decoder ran, so the
// canvas couldn't re-allocate and the menu fell to a broken/flickering direct draw. 135 fits with
// margin AND re-acquires cleanly; video now mallocs its OWN ~8 KB frame buffer. 8bpp (RGB332).
static const int SCREEN_W = 240, SCREEN_H = 135;
static M5Canvas s_screen(&d);
static bool     s_screen_alive = false;
static bool     s_screen_failed = false;   // last acquire failed -> don't thrash createSprite every frame
static int64_t  s_last_try_us = 0;         // when we last attempted a (re)acquire

// Explicit (re)allocation — ALWAYS attempts, e.g. from close_app() right after a decoder freed its
// RAM. On success clears the "failed" latch so the lazy getter buffers again.
bool nucleo_screen_acquire(void)
{
    if (s_screen_alive) return true;
    // M5Canvas(&d) defaults to _psram=true, so createSprite probes the SPIRAM pool FIRST. This board has
    // NO PSRAM, so that probe ALWAYS fails (caps 0x404 = SPIRAM|8BIT) → it fires the global OOM hook
    // (noisy boot "oom: alloc fail 32404 B" + a polluted /api/diag watermark) before silently falling
    // back to DMA-internal. setPsram(false) skips the doomed probe and allocates DMA-internal directly —
    // byte-identical buffer, no false OOM, on every path (boot + each media-app re-acquire). Idempotent.
    s_screen.setPsram(false);
    s_screen.setColorDepth(8);
    if (s_screen.createSprite(SCREEN_W, SCREEN_H)) { s_screen_alive = true; s_screen_failed = false; return true; }
    s_screen_failed = true;                               // remember: the heap can't fit it right now
    return false;
}
// Free the canvas so an audio app can give the contiguous block to the Helix MP3 decoder (~17 KB
// single alloc) — without this, MP3InitDecoder fails out-of-RAM and radio/music play SILENT on the
// PSRAM-less chip. The launcher re-acquires it (close_app / lazily) once the decoder frees its RAM.
// Clearing the latch here lets the next lazy access try once (RAM was just freed).
void nucleo_screen_release(void) { if (s_screen_alive) { s_screen.deleteSprite(); s_screen_alive = false; } s_screen_failed = false; }
// Lazy getter for the buffered render path. Tries once; if it can't fit (a decoder holds the RAM)
// it RE-tries at most ~every 400 ms — not every frame. That spacing matters both ways: a per-frame
// failed createSprite(32 KB) is the lag in the track list while a song plays; but NEVER retrying
// left the menu stuck drawing direct (flickery, "items vanish while scrolling") for the whole time
// after an audio app ran, because the heap only DEFRAGMENTS a beat after the decoder/Wi-Fi buffers
// free. The timed retry self-heals: within ~0.4 s of the RAM coming back the canvas re-acquires and
// the UI is buffered (flicker-free) again. release()/acquire() also clear the latch immediately.
M5Canvas *nucleo_screen(void)
{
    if (!s_screen_alive && (!s_screen_failed || esp_timer_get_time() - s_last_try_us > 400000)) {
        s_last_try_us = esp_timer_get_time();
        nucleo_screen_acquire();
    }
    return s_screen_alive ? &s_screen : nullptr;
}

extern "C" void nucleo_ui_set_brightness(unsigned char b) { d.setBrightness(b); }

// M5GFX identifies the board during d.init(); the ADV uses a different (I2C) keyboard.
extern "C" bool nucleo_ui_is_adv(void) { return d.getBoard() == lgfx::board_M5CardputerADV; }

static const int W = 240, H = 135, BAR = 16;
#include "nucleo_theme.h"

// Define compatibility macros for nucleo_ui.cpp
#define BG THEME_BG
#define ACC THEME_ACC
#define FG THEME_FG
#define SEL THEME_SEL
#define MUT THEME_MUTED
#define LINE THEME_LINE

extern "C" void nucleo_ui_init(void)
{
    nucleo_theme_init(); // Initialize dynamic theme
    d.init();
    d.setRotation(1);
    nucleo_theme_draw_bg(&d, W, H);
    nucleo_screen_acquire();        // grab the shared back-buffer now, while the heap is clean
    nucleo_kbd_init();
}

// Draw the header (title + hairline rule) on a canvas. The battery icon was removed: the ADC
// reading was unreliable on this unit, so it was misleading.
static void header(M5Canvas *canvas, const char *title)
{
    nucleo_theme_draw_bg(canvas, W, H);
    canvas->setTextColor(ACC, BG);
    canvas->setTextSize(2);
    canvas->setCursor(6, 4);
    canvas->print(title);

    canvas->drawFastHLine(0, 26, W, LINE);
    canvas->setTextSize(1);
}

static void hint(M5Canvas *canvas, const char *h)
{
    canvas->setTextColor(MUT, BG);
    canvas->setTextSize(1);
    canvas->setCursor(6, H - 12);
    canvas->print(h);
}

// Draw the body text of a message/home screen with READABLE, self-fitting fonts. With a handful of
// lines (<=4) each is drawn as large as it fits (size 2) so short prompts and confirmations are easy
// to read on the 240x135 panel; any line too wide for size 2 (URLs, long technical strings) auto-drops
// to size 1 so it is never clipped. Dense screens (>4 lines) stay compact at size 1 to avoid overflow.
// An empty string is a vertical spacer. Caller sets the text colour; header()/hint() own the chrome.
static void draw_body(M5Canvas &canvas, const char *const *lines, int n)
{
    bool big = (n <= 4);
    int y = 32;
    for (int i = 0; i < n; i++) {
        const char *ln = lines[i] ? lines[i] : "";
        if (!ln[0]) { y += big ? 10 : 8; continue; }        // blank line = spacer
        int sz = 1;
        if (big) { canvas.setTextSize(2); if (canvas.textWidth(ln) <= W - 16) sz = 2; }
        canvas.setTextSize(sz);
        canvas.setCursor(8, y);
        canvas.print(ln);
        y += (sz == 2) ? 20 : 13;
    }
}

extern "C" void nucleo_ui_message(const char *title, const char *const *lines, int n)
{
    M5Canvas local(&d);
    M5Canvas *shared = s_screen_alive ? &s_screen : nullptr;   // reuse the launcher buffer only if ALREADY up
    if (!shared) { local.setPsram(false); local.createSprite(W, H); }   // else our own (DMA-internal), never force 32 KB
    M5Canvas &canvas = shared ? *shared : local; // a transient modal never needs a SECOND 32 KB canvas
    
    for (;;) {
        header(&canvas, title);
        canvas.setTextColor(FG, BG);
        draw_body(canvas, lines, n);
        hint(&canvas, "[enter] continue");
        canvas.pushSprite(0, 0);

        nucleo_key_t k = nucleo_kbd_read();
        if (k.key == NK_ENTER) break;
        esp_task_wdt_reset();   // a slow typist/idle dialog must not trip the 8s task WDT (no-op if unwatched)
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    if (&canvas == &local) local.deleteSprite();   // free only what WE allocated; the shared buffer persists
}

extern "C" void nucleo_ui_home(const char *title, const char *const *lines, int n)
{
    // Home is usually static, but we'll draw it once via canvas to avoid flicker
    M5Canvas local(&d);
    M5Canvas *shared = s_screen_alive ? &s_screen : nullptr;   // reuse the launcher buffer only if ALREADY up
    if (!shared) { local.setPsram(false); local.createSprite(W, H); }   // else our own (DMA-internal), never force 32 KB
    M5Canvas &canvas = shared ? *shared : local; // a transient modal never needs a SECOND 32 KB canvas
    header(&canvas, title);
    canvas.setTextColor(FG, BG);
    draw_body(canvas, lines, n);
    canvas.pushSprite(0, 0);
    if (&canvas == &local) local.deleteSprite();   // free only what WE allocated; the shared buffer persists
}

// ---- animated boot splash ----------------------------------------------------------------
// "Nucleo" = nucleus: a glowing atomic core with three electrons weaving through tilted orbits.
// Everything below is the same model the web preview uses, ported to M5GFX primitives: there is
// no per-pixel alpha on an offscreen sprite, so glows are faked as layered fillSmoothCircle (dim
// + large drawn first, bright + small last) and the orbit rings as short anti-aliased polylines
// split into a "behind the core" half and an "in front" half for the 3-D weave. Runs on its own
// 16bpp canvas (max colour fidelity) created while the boot heap is still clean; deleted on exit.
#define NUCLEO_SPLASH_MS 2500

namespace {
    const float SPL_CX = 120.f, SPL_CY = 50.f;   // atom centre (room for wordmark + bar below)
    const float SPL_A  = 47.f,  SPL_B  = 15.f;   // electron-orbit ellipse: semi-major / semi-minor

    inline float spl_clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    inline uint16_t spl_565(int r, int g, int b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
    // colour scaled by k (0..1). The background is near-black, so scaling toward 0 IS a fade-in.
    inline uint16_t spl_k(float r, float g, float b, float k) {
        int R = (int)(r * k + 0.5f); R = R < 0 ? 0 : (R > 255 ? 255 : R);
        int G = (int)(g * k + 0.5f); G = G < 0 ? 0 : (G > 255 ? 255 : G);
        int B = (int)(b * k + 0.5f); B = B < 0 ? 0 : (B > 255 ? 255 : B);
        return spl_565(R, G, B);
    }
    // Soft radial glow as N opaque discs, large+dim outermost, small+bright innermost.
    void spl_glow(M5Canvas &cv, float x, float y, float rmax, float r, float g, float b, float kmax, int layers) {
        for (int i = 0; i < layers; i++) {
            float rr = rmax * (1.f - (float)i / layers);
            if (rr < 1.f) continue;
            cv.fillSmoothCircle((int)x, (int)y, (int)rr, spl_k(r, g, b, kmax * (float)(i + 1) / layers));
        }
    }
    struct SplPt { float x, y, d; };   // d = sin(param): >=0 means the near (front) half of the orbit
    inline SplPt spl_ept(int ring, float param, const float *ct, const float *st) {
        float ex = SPL_A * cosf(param), ey = SPL_B * sinf(param);
        SplPt p; p.x = SPL_CX + ex * ct[ring] - ey * st[ring];
                 p.y = SPL_CY + ex * st[ring] + ey * ct[ring];
                 p.d = sinf(param);
        return p;
    }
    void spl_ring_half(M5Canvas &cv, int ring, bool front, uint16_t col, const float *ct, const float *st) {
        const int seg = 28; bool have = false; float px = 0, py = 0;
        for (int k = 0; k <= seg; k++) {
            float param = (float)k / seg * 6.2831853f;
            bool on = front ? (sinf(param) >= 0.f) : (sinf(param) < 0.f);
            if (!on) { have = false; continue; }
            SplPt p = spl_ept(ring, param, ct, st);
            if (have) cv.drawLine((int)px, (int)py, (int)p.x, (int)p.y, col);
            px = p.x; py = p.y; have = true;
        }
    }
    void spl_electron(M5Canvas &cv, int ring, float t, bool front, float elec,
                      const float *spd, const float *ph, const float *ct, const float *st) {
        float param = spd[ring] * t + ph[ring];
        SplPt p = spl_ept(ring, param, ct, st);
        if ((p.d >= 0.f) != front) return;                 // drawn in the matching depth pass only
        float dep = (p.d + 1.f) * 0.5f;                    // 0 = far, 1 = near
        float sz  = (1.0f + dep * 1.4f) * (0.45f + 0.55f * elec);
        for (int kk = 8; kk >= 1; kk--) {                  // sampled fading trail (no state kept)
            float tp = param - (spd[ring] > 0 ? 1.f : -1.f) * kk * 0.10f;
            SplPt q = spl_ept(ring, tp, ct, st);
            if ((q.d >= 0.f) != front) continue;
            int r = (int)(sz * (1.f - kk / 12.f) + 0.5f); if (r < 1) r = 1;
            cv.fillCircle((int)q.x, (int)q.y, r, spl_k(54, 224, 255, (1.f - kk / 9.f) * 0.5f * elec));
        }
        spl_glow(cv, p.x, p.y, sz * 3.4f, 54, 224, 255, 0.5f * elec, 4);
        cv.fillSmoothCircle((int)p.x, (int)p.y, (int)(sz * 1.5f + 0.5f), spl_k(47, 214, 255, elec));
        cv.fillSmoothCircle((int)p.x, (int)p.y, (int)(sz * 0.7f + 0.5f), spl_k(238, 255, 255, elec));
        if (front && dep > 0.55f) {                        // tiny sparkle glint on the near electron
            float g = sz * 2.2f; uint16_t gc = spl_k(205, 250, 255, elec * 0.7f);
            cv.drawFastHLine((int)(p.x - g), (int)p.y, (int)(g * 2.f), gc);
            cv.drawFastVLine((int)p.x, (int)(p.y - g), (int)(g * 2.f), gc);
        }
    }
}

extern "C" void nucleo_ui_boot_splash(void)
{
    M5Canvas cv(&d);
    cv.setPsram(false);                 // PSRAM-less board: allocate DMA-internal, no false OOM
    // The 16bpp canvas wants ~63 KB of CONTIGUOUS DMA-internal heap. With the real NimBLE stack linked
    // (~22 KB non-releasable DRAM) the fragile ADV can't always spare that at boot -> the splash used to
    // skip SILENTLY (no animation, no clue why). Now: try 16bpp, fall back to 8bpp (~32 KB, glows band a
    // little but the animation shows), and if even that fails, log the largest free block so the skip is
    // diagnosable instead of mysterious. spl_565() colours convert to the canvas depth automatically.
    cv.setColorDepth(16);               // full colour for the glows/gradients
    if (!cv.createSprite(W, H)) {
        size_t lg = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        cv.setColorDepth(8);            // half the RAM (240x135x1 = ~32 KB) — keep the boot identity visible
        if (!cv.createSprite(W, H)) {
            ESP_LOGW("splash", "skipped: no contiguous heap for canvas (largest DMA block=%u B, need ~32K)", (unsigned)lg);
            return;
        }
        ESP_LOGW("splash", "8bpp fallback: 16bpp canvas (63K) did not fit (largest DMA block=%u B)", (unsigned)lg);
    }

    const float tilt[3] = { 0.f, 1.0471976f, 2.0943951f };      // 0, 60, 120 deg
    float ct[3], st[3];
    for (int i = 0; i < 3; i++) { ct[i] = cosf(tilt[i]); st[i] = sinf(tilt[i]); }
    const float spd[3] = { 2.0f, -2.45f, 2.95f };               // per-ring angular speed (rad/s)
    const float ph[3]  = { 0.f, 2.1f, 4.2f };                   // ... and phase offset

    // Deterministic starfield + nucleon cluster (no RNG needed; hash by index).
    const int NSTAR = 50; float sx[NSTAR], sy[NSTAR], sb[NSTAR], sw[NSTAR];
    for (int i = 0; i < NSTAR; i++) {
        float a = sinf((i + 1) * 12.9898f) * 43758.5453f; a -= floorf(a);
        float b = sinf((i + 9) * 12.9898f) * 43758.5453f; b -= floorf(b);
        float c = sinf((i + 17) * 12.9898f) * 43758.5453f; c -= floorf(c);
        float e = sinf((i + 23) * 12.9898f) * 43758.5453f; e -= floorf(e);
        sx[i] = a * W; sy[i] = b * H; sb[i] = 0.25f + c * 0.6f; sw[i] = e * 6.2831853f;
    }
    const int NNUC = 14; float nx[NNUC], ny[NNUC], nph[NNUC]; bool np[NNUC];
    for (int n = 0; n < NNUC; n++) {
        float ga = n * 2.399963f, rr = 8.4f * sqrtf((float)n / NNUC);
        nx[n] = cosf(ga) * rr; ny[n] = sinf(ga) * rr * 0.92f; np[n] = (n % 3 != 0);
        float h = sinf((n + 41) * 12.9898f) * 43758.5453f; h -= floorf(h); nph[n] = h * 6.2831853f;
    }

    const uint16_t bg = spl_565(2, 3, 12);
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        float t = (esp_timer_get_time() - t0) / 1e6f;
        if (t >= NUCLEO_SPLASH_MS / 1000.f) break;
        if (nucleo_kbd_read().key != NK_NONE) break;            // any key skips the splash

        float ign   = spl_clamp(t / 0.45f, 0, 1);               // nucleus ignites
        float ringR = spl_clamp((t - 0.25f) / 0.6f, 0, 1);      // orbits fade/draw in
        float elec  = spl_clamp((t - 0.55f) / 0.5f, 0, 1);      // electrons appear
        float word  = spl_clamp((t - 0.9f) / 0.55f, 0, 1);      // wordmark fades in
        float tagf  = spl_clamp((t - 1.3f) / 0.5f, 0, 1);
        float prog  = spl_clamp(t / 2.0f, 0, 1);                // loading bar 0..100%

        cv.fillScreen(bg);
        for (int i = 0; i < NSTAR; i++) {
            float tw = 0.55f + 0.45f * sinf(t * 2.2f + sw[i]);
            int X = (int)(sx[i] + t * (3.f + sb[i] * 6.f)) % W; if (X < 0) X += W;   // gentle parallax drift
            int Y = (int)sy[i]; float a = sb[i] * tw;
            if (sb[i] > 0.75f) {                                 // a few "near" stars: 2px + faint cross sparkle
                cv.fillRect(X, Y, 2, 2, spl_k(220, 232, 255, a));
                uint16_t sc = spl_k(150, 200, 255, a * 0.5f);
                cv.drawFastHLine(X - 2, Y, 6, sc); cv.drawFastVLine(X, Y - 2, 6, sc);
            } else {
                cv.drawPixel(X, Y, spl_k(150, 180, 220, a * 0.6f));
            }
        }
        { // a couple of meteor streaks across the splash (deterministic by epoch), drawn behind the atom
            float per = 1.3f; int idx = (int)(t / per); float lt = t - idx * per;
            if (lt < 0.55f) {
                float pr = lt / 0.55f;
                float r1 = sinf((idx + 1) * 12.9898f) * 43758.5453f; r1 -= floorf(r1);
                float r2 = sinf((idx + 5) * 78.233f)  * 43758.5453f; r2 -= floorf(r2);
                float hx = r1 * W * 0.7f + 150.f * pr, hy = r2 * 38.f + 70.f * pr;   // head slides down-right
                float a = sinf(pr * 3.14159f) * 0.7f;                                // fade in then out
                cv.drawGradientLine((int)(hx - 26.f), (int)(hy - 12.f), (int)hx, (int)hy,
                                    bg, spl_k(210, 232, 255, a));
            }
        }
        for (int ep = 0; ep < 2; ep++) {                        // two staggered emission rings (radiation)
            float pp = fmodf(t + ep * 0.55f, 1.1f) / 0.85f;
            if (pp < 1.f) cv.drawCircle((int)SPL_CX, (int)SPL_CY, (int)(10 + pp * 36),
                                        spl_k(120, 200, 255, (1.f - pp) * 0.28f * ign));
        }

        if (ringR > 0) for (int r = 0; r < 3; r++) spl_ring_half(cv, r, false, spl_k(20, 84, 108, ringR), ct, st);
        for (int r = 0; r < 3; r++) spl_electron(cv, r, t, false, elec, spd, ph, ct, st);

        float pulse = 1.f + 0.06f * sinf(t * 5.f);              // nucleus core + nucleons
        spl_glow(cv, SPL_CX, SPL_CY, 24.f * ign * pulse, 255, 150, 60,  0.5f * ign, 6);
        spl_glow(cv, SPL_CX, SPL_CY, 13.f * ign * pulse, 255, 200, 110, 0.7f * ign, 5);
        for (int n = 0; n < NNUC; n++) {
            float jx = cosf(nph[n] + t * 3.f) * 0.7f, jy = sinf(nph[n] + t * 3.f) * 0.7f;
            cv.fillSmoothCircle((int)(SPL_CX + nx[n] * ign + jx), (int)(SPL_CY + ny[n] * ign + jy), 2,
                                np[n] ? spl_k(255, 107, 61, ign) : spl_k(127, 155, 232, ign));
        }
        spl_glow(cv, SPL_CX, SPL_CY, 6.f * ign, 255, 244, 214, 0.95f * ign, 4);

        if (ringR > 0) for (int r = 0; r < 3; r++) spl_ring_half(cv, r, true, spl_k(47, 214, 255, ringR), ct, st);
        for (int r = 0; r < 3; r++) spl_electron(cv, r, t, true, elec, spd, ph, ct, st);

        if (word > 0) {                                         // wordmark (bg = scene bg, no box)
            cv.setFont(&fonts::FreeSansBold12pt7b);
            cv.setTextDatum(datum_t::top_center);
            cv.setTextColor(spl_k(234, 246, 255, word), bg);
            cv.drawString("NucleoOS", (int)SPL_CX, 92);
            float uw = cv.textWidth("NucleoOS") * 0.5f * spl_clamp((t - 1.05f) / 0.5f, 0, 1);  // accent underline draws in
            if (uw > 1.f) cv.fillRect((int)(SPL_CX - uw), 112, (int)(uw * 2.f), 1, spl_k(60, 224, 255, word));
        }
        if (tagf > 0) {
            cv.setFont(&fonts::Font0);
            cv.setTextSize(1);
            cv.setTextDatum(datum_t::top_center);
            cv.setTextColor(spl_k(120, 150, 180, tagf), bg);
            cv.drawString("web-native os", (int)SPL_CX, 117);
        }

        int bw = 120, bx = (W - bw) / 2, by = 129;              // loading bar
        cv.fillRect(bx, by, bw, 2, spl_565(18, 20, 26));
        cv.fillRect(bx, by, (int)(bw * prog), 2, spl_565(47, 214, 255));
        if (prog > 0.f && prog < 1.f) cv.fillRect(bx + (int)(bw * prog) - 1, by - 1, 2, 4, spl_565(220, 250, 255));

        cv.pushSprite(0, 0);
        esp_task_wdt_reset();              // long boot loop must not trip the task WDT (no-op if unwatched)
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    cv.deleteSprite();
}

extern "C" int nucleo_ui_menu(const char *title, const char *const *items, int n)
{
    int sel = 0;
    float smooth_y = 0.0f;
    uint32_t frame = 0;
    
    M5Canvas local(&d);
    M5Canvas *shared = s_screen_alive ? &s_screen : nullptr;   // reuse the launcher buffer only if ALREADY up
    if (!shared) { local.setPsram(false); local.createSprite(W, H); }   // else our own (DMA-internal), never force 32 KB
    M5Canvas &canvas = shared ? *shared : local; // a transient modal never needs a SECOND 32 KB canvas
    
    for (;;) {
        header(&canvas, title);
        
        float target_y = sel * 24.0f;
        smooth_y += (target_y - smooth_y) * 0.3f;
        
        int base_y = 38;
        
        for (int i = 0; i < n; i++) {
            float item_y = base_y + (i * 24.0f) - smooth_y;
            // Only draw if visible
            if (item_y > 20 && item_y < H) {
                if (i == sel) {
                    canvas.fillRoundRect(6, item_y, W - 12, 22, 6, SEL);
                    canvas.setTextColor(0x0000, SEL);
                    canvas.setTextSize(2);
                    // Marquee logic if text is too long
                    int tw = canvas.textWidth(items[i]);
                    if (tw > W - 20) {
                        int offset = (frame / 2) % (tw + 40);
                        canvas.setClipRect(8, item_y, W - 16, 22);
                        canvas.setCursor(10 - offset, item_y + 4);
                        canvas.print(items[i]);
                        canvas.setCursor(10 - offset + tw + 40, item_y + 4);
                        canvas.print(items[i]);
                        canvas.clearClipRect();
                    } else {
                        canvas.setCursor(12, item_y + 4); 
                        canvas.print(items[i]);
                    }
                } else {
                    canvas.setTextColor(FG, BG);
                    canvas.setTextSize(1);
                    canvas.setCursor(12, item_y + 7); 
                    canvas.print(items[i]);
                }
            }
        }
        
        hint(&canvas, "[;/.] move  [enter] ok  [`] back");
        canvas.pushSprite(0, 0);
        
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key == NK_UP) { sel = (sel + n - 1) % n; frame = 0; }
        else if (k.key == NK_DOWN) { sel = (sel + 1) % n; frame = 0; }
        else if (k.key == NK_ENTER) { if (&canvas == &local) local.deleteSprite(); return sel; }
        else if (k.key == NK_BACK) { if (&canvas == &local) local.deleteSprite(); return -1; }
        
        frame++;
        esp_task_wdt_reset();   // a slow typist/idle dialog must not trip the 8s task WDT (no-op if unwatched)
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

extern "C" void nucleo_ui_input(const char *title, char *buf, int len, int masked)
{
    M5Canvas local(&d);
    M5Canvas *shared = s_screen_alive ? &s_screen : nullptr;   // reuse the launcher buffer only if ALREADY up
    if (!shared) { local.setPsram(false); local.createSprite(W, H); }   // else our own (DMA-internal), never force 32 KB
    M5Canvas &canvas = shared ? *shared : local; // a transient modal never needs a SECOND 32 KB canvas
    int pos = (int)strlen(buf);
    
    for (;;) {
        header(&canvas, title);
        canvas.setTextColor(FG, BG); 
        canvas.setTextSize(2);
        canvas.setCursor(8, 40);
        if (masked) {
            for (int i = 0; i < pos; i++) canvas.print("*");
        } else {
            canvas.print(buf);
        }
        // Blinking cursor
        if ((esp_timer_get_time() / 500000) % 2 == 0) {
            canvas.print("_");
        }
        
        canvas.setTextSize(1);
        hint(&canvas, "[enter] ok  [del] erase  [`] cancel");
        canvas.pushSprite(0, 0);
        
        nucleo_key_t k = nucleo_kbd_read();
        if (k.key == NK_ENTER || k.key == NK_BACK) { buf[pos] = '\0'; break; }
        else if (k.key == NK_DEL) { if (pos > 0) buf[--pos] = '\0'; }
        else if (k.ch >= 32 && pos < len - 1) { buf[pos++] = k.ch; buf[pos] = '\0'; }
        
        esp_task_wdt_reset();   // a slow typist/idle dialog must not trip the 8s task WDT (no-op if unwatched)
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    if (&canvas == &local) local.deleteSprite();   // free only what WE allocated; the shared buffer persists
}

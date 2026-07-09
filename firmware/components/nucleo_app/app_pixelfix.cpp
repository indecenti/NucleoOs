// PixelFix — Tools. LCD pixel rehabilitation: six full-screen patterns that exercise
// stuck or retention-locked pixels. B/N flash stresses transistors at ~70fps; RGB ramp
// saturates every colour channel; checkerboard exercises neighbour contrast; noise loads
// the full spectrum randomly; pixel walker applies a high-contrast cross to every pixel
// in sequence; sweep stress-tests scanline response. Run a mode for a few minutes and
// check if stuck pixels have cleared. NX_NET_APP frees ~47 KB so the fill DMA has room.
#include "nucleo_app.h"
#include "app_gfx.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_exclusive.h"
#include "nucleo_fx3d.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <string.h>

// ---- modes -----------------------------------------------------------------
enum { MODE_BW=0, MODE_RGB, MODE_CHECK, MODE_NOISE, MODE_WALKER, MODE_SWEEP, N_MODES };

static const char *const MNAME[N_MODES] = {
    "B/N Flash", "RGB", "Scacchiera", "Disturbo", "Pixel Walker", "Scansione"
};
static const char *const MDESC[N_MODES] = {
    "Flash 70fps  tutti i pixel",
    "Rosso  Verde  Blu  Bianco  Nero",
    "Pattern alternato 30fps",
    "Rumore casuale  spettro pieno",
    "Croce singola pixel-per-pixel",
    "Striscia bianca  scansione"
};
// Target frame intervals (µs): trade speed for effect type.
// SPI @40MHz → blit 240×135 @16bpp = ~13ms → absolute ceiling ~74fps.
// BW fits because 14ms interval ≥ 13ms blit. Walker is blit-limited (~74fps).
static const int64_t MINTERVAL[N_MODES] = {
    14000,   // BW:     ~71fps — fits (interval ≈ blit time); hammers all transistors
    65000,   // RGB:    ~15fps/colour — saturates each channel thoroughly
    33000,   // CHECK:  ~30fps — neighbour-contrast cycling
    28000,   // NOISE:  ~36fps — full-spectrum random pixel load
    5500,    // WALKER: blit-limited (~74fps) — cross visits every pixel in ~7 min
    18000,   // SWEEP:  ~55fps — scanline stress with glow halos
};
static const uint16_t RGB_COLS[5] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000 };
static const uint16_t WALK_ARM[3] = { 0xF800, 0x07E0, 0x001F }; // R/G/B arms by x-thirds

// ---- state -----------------------------------------------------------------
static int     s_mode;
static bool    s_paused;
static bool    s_first_frame;   // clears canvas on mode change
static int     s_bw;            // 0=black 1=white
static int     s_rgb;           // RGB step 0..4
static int     s_check;         // checker parity 0/1
static int     s_wx, s_wy;      // walker current
static int     s_wpx, s_wpy;    // walker last-drawn (for incremental erase)
static int     s_sweep_y;       // sweep stripe Y
static int64_t s_label_us;      // label visible until
static int64_t s_hint_us;       // hint bar visible until
static int64_t s_last_us;

static void mode_set(int m, int64_t now)
{
    s_mode = ((m % N_MODES) + N_MODES) % N_MODES;
    s_paused = false; s_first_frame = true;
    s_bw = 0; s_rgb = 0; s_check = 0;
    s_wx = 0; s_wy = 0; s_wpx = -1; s_wpy = -1; s_sweep_y = 0;
    s_label_us = now + 2200000;
    s_last_us  = now;
}

// ---- poll ------------------------------------------------------------------
static bool poll(void)
{
    if (s_paused) return false;
    int64_t now = esp_timer_get_time();
    if ((now - s_last_us) < MINTERVAL[s_mode]) return false;
    s_last_us = now;
    return true;
}

// ---- draw ------------------------------------------------------------------
static void draw(void)
{
    int64_t now = esp_timer_get_time();

    // First frame after mode change: establish a clean black canvas.
    // Modes that fillScreen() every frame (BW/RGB/CHECK/NOISE/SWEEP) override
    // it immediately; WALKER needs it for the black background.
    if (s_first_frame) { d.fillScreen(0x0000); s_first_frame = false; }

    if (!s_paused) {
        switch (s_mode) {

        // ---- B/N flash: alternates entire screen at ~70 fps ----------------
        case MODE_BW:
            d.fillScreen(s_bw ? 0xFFFF : 0x0000);
            s_bw ^= 1;
            break;

        // ---- RGB ramp: saturates each colour channel in turn ---------------
        case MODE_RGB:
            d.fillScreen(RGB_COLS[s_rgb]);
            s_rgb = (s_rgb + 1) % 5;
            break;

        // ---- 12×12 checkerboard invert ~30 fps (exercises adjacent contrast)
        case MODE_CHECK: {
            const int BS = 12;
            for (int by = 0; by < H; by += BS)
                for (int bx = 0; bx < W; bx += BS)
                    d.fillRect(bx, by, BS, BS, ((bx/BS + by/BS + s_check) & 1) ? 0xFFFF : 0x0000);
            s_check ^= 1;
            break;
        }

        // ---- 8×8 random-colour noise (full-spectrum random pixel load) -----
        case MODE_NOISE: {
            const int BS = 8;
            for (int by = 0; by < H; by += BS)
                for (int bx = 0; bx < W; bx += BS)
                    d.fillRect(bx, by, BS, BS, (uint16_t)(esp_random() >> 16));
            break;
        }

        // ---- pixel walker: a bright cross sweeps every pixel in sequence ---
        // Incremental: only erase the old cross + draw the new one (~10 pixel
        // ops per frame) so the screen is fully black between passes.
        case MODE_WALKER: {
            uint16_t arm_c = WALK_ARM[(s_wx / 80) % 3];  // cycle arm colour by column-third
            // Erase previous 5-pixel cross
            if (s_wpx >= 0) {
                d.drawPixel(s_wpx,   s_wpy,   0x0000);
                if (s_wpx > 0)   d.drawPixel(s_wpx-1, s_wpy,   0x0000);
                if (s_wpx < W-1) d.drawPixel(s_wpx+1, s_wpy,   0x0000);
                if (s_wpy > 0)   d.drawPixel(s_wpx,   s_wpy-1, 0x0000);
                if (s_wpy < H-1) d.drawPixel(s_wpx,   s_wpy+1, 0x0000);
            }
            // Draw new cross: white centre, coloured arms
            d.drawPixel(s_wx, s_wy, 0xFFFF);
            if (s_wx > 0)   d.drawPixel(s_wx-1, s_wy,   arm_c);
            if (s_wx < W-1) d.drawPixel(s_wx+1, s_wy,   arm_c);
            if (s_wy > 0)   d.drawPixel(s_wx,   s_wy-1, arm_c);
            if (s_wy < H-1) d.drawPixel(s_wx,   s_wy+1, arm_c);
            s_wpx = s_wx; s_wpy = s_wy;
            // Advance one pixel; wrap to top-left on completion
            s_wx++; if (s_wx >= W) { s_wx = 0; s_wy++; if (s_wy >= H) { s_wy = 0; s_wpx = -1; } }
            break;
        }

        // ---- sweep: white stripe with soft halos scans top→bottom ---------
        case MODE_SWEEP: {
            const int SH = 10;
            d.fillScreen(0x0000);
            // Lead halo (dim, 2 px above)
            if (s_sweep_y >= 2) d.fillRect(0, s_sweep_y-2, W, 2, 0x2104);
            // Main stripe
            d.fillRect(0, s_sweep_y, W, SH, 0xFFFF);
            // Trail halo (dim, 2 px below)
            if (s_sweep_y + SH + 2 <= H) d.fillRect(0, s_sweep_y+SH, W, 2, 0x2104);
            s_sweep_y = (s_sweep_y + SH) % H;
            break;
        }

        } // switch
    }

    // ---- Mode label pill (2.2 s after each mode change) --------------------
    // Drawn on top of the mode frame so it appears on any background.
    if (now < s_label_us) {
        const int LW=192, LH=50, LX=(W-LW)/2, LY=(H-LH)/2;
        d.fillRoundRect(LX, LY, LW, LH, 8, INK);
        d.drawRoundRect(LX, LY, LW, LH, 8, 0xFFFF);
        d.drawRoundRect(LX+1, LY+1, LW-2, LH-2, 7, fx3d::scl(LINE, 80, 255));

        d.setTextSize(2); d.setTextColor(0xFFFF, INK);
        int nw = (int)strlen(MNAME[s_mode]) * 12;
        d.setCursor(LX + (LW-nw)/2, LY+5); d.print(MNAME[s_mode]);

        d.setTextSize(1); d.setTextColor(MUTED, INK);
        int dw = (int)strlen(MDESC[s_mode]) * 6;
        d.setCursor(LX + (LW-dw)/2, LY+24); d.print(MDESC[s_mode]);

        // Mode-position dots inside the pill
        const int DR=3, DS=11, DX0=W/2 - (N_MODES*DS)/2 + DR, DY=LY+LH-11;
        for (int i = 0; i < N_MODES; i++)
            d.fillCircle(DX0+i*DS, DY, DR, i==s_mode ? 0xFFFF : fx3d::scl(LINE, 80, 255));
    }

    // ---- Paused overlay ----------------------------------------------------
    if (s_paused) {
        const int PW=130, PH=34, PX=(W-PW)/2, PY=(H-PH)/2;
        d.fillRoundRect(PX, PY, PW, PH, 8, INK);
        d.drawRoundRect(PX, PY, PW, PH, 8, MUTED);
        d.setTextSize(2); d.setTextColor(MUTED, INK);
        int tw = 5*12; d.setCursor(PX+(PW-tw)/2, PY+9); d.print("PAUSA");
    }

    // ---- Key-hint bar (first 5 s) ------------------------------------------
    if (now < s_hint_us) {
        d.fillRect(0, H-11, W, 11, INK);
        d.setTextSize(1); d.setTextColor(MUTED, INK);
        const char *h = "TAB/frecce: modo   INVIO: pausa   ESC: esci";
        int hw = (int)strlen(h)*6; d.setCursor((W-hw)/2, H-9); d.print(h);
    }
}

// ---- input -----------------------------------------------------------------
static void on_key(int key, char ch)
{
    (void)ch;
    int64_t now = esp_timer_get_time();
    if (key == NK_ENTER) {
        s_paused = !s_paused;
        if (!s_paused) s_last_us = now;
        nucleo_app_request_draw();
        return;
    }
    if (key == NK_RIGHT || key == NK_DOWN) { mode_set(s_mode+1, now); nucleo_app_request_draw(); }
    if (key == NK_UP)                      { mode_set(s_mode-1, now); nucleo_app_request_draw(); }
}

static void tab_fn(void)  { mode_set(s_mode+1, esp_timer_get_time()); nucleo_app_request_draw(); }
static bool back_fn(int k) { if (k == NK_LEFT) { mode_set(s_mode-1, esp_timer_get_time()); nucleo_app_request_draw(); return true; } return false; }
static void on_exit(void) {}

// ---- lifecycle -------------------------------------------------------------
static void enter(void)
{
    int64_t now = esp_timer_get_time();
    mode_set(MODE_BW, now);
    s_hint_us = now + 5000000;
    nucleo_app_set_fullscreen(true);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab_fn);
    nucleo_app_set_back_handler(back_fn);
    nucleo_app_request_draw();
}

extern "C" void nucleo_register_pixelfix(void)
{
    static const nucleo_app_def_t app = {
        "pixel-fix", "PixelFix", "System", "Risana pixel bloccati e image retention LCD",
        'F', C_YELLOW, enter, on_key, nullptr, draw, on_exit, NX_NET_APP
    };
    nucleo_app_register(&app);
}

// Torch app: turn the Cardputer into a flashlight, a night-vision light and an emergency beacon.
//
// The only human-visible light source on the device is the LCD itself, so "max light" means:
// fill the whole panel with the light colour (every subpixel of it fully on) AND drive the backlight.
// The standard Cardputer has no RGB/status LED; its only other emitter is the IR transmitter, which
// is invisible and useless for lighting, so there is nothing else to switch on.
//
//   Colour   White (max light) · Red (astronomy / night walking: red keeps the eye dark-adapted, so
//            keep it DIM) · Orange (hazard amber, a breakdown at the roadside).
//   Pattern  Solid · Blink (0.5-3 Hz) · SOS (Morse ... --- ...).
//
// Blinking toggles the BACKLIGHT only (nucleo_ui_set_brightness, a PWM write): the colour frame is
// painted once, so a flash costs no redraw, cannot tear, and the dark half of every period draws
// almost nothing from the battery. Rates are capped at 3 Hz: faster flashing is the WCAG 2.3.1 seizure
// band, and a red strobe is the worst case of it.
//
// Controls: SPACE / ENTER on/off · 1-3 or LEFT/RIGHT colour · TAB pattern · UP/DOWN brightness (Solid,
// SOS) or speed (Blink). ESC exits (the framework restores the previous brightness via on_exit).
// Choices last for the boot, and the G0 quick-tap overlay uses the same colour + brightness.
//
// Global shortcut: a single quick tap on the G0 side button toggles a flashlight from ANYWHERE on the
// device (launcher, any app, Control Center) -- you don't have to open this app first. Holding G0
// instead is Push-to-Talk. See toggle_torch() in nucleo_app.cpp.
#include "nucleo_app.h"
#include "app_gfx.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_ui.h"          // set_brightness(0): a REAL dark half-period (app_set_brightness floors at 10%)
#include "esp_timer.h"
#include <M5GFX.h>
#include <stdio.h>
#include <string.h>
#include "nucleo_i18n.h"        // TR5: every string follows the system language

// Light colours: genuine content (the light itself), named per the UI-kit rule.
static const unsigned short L_WHITE  = 0xFFFF;
static const unsigned short L_RED    = 0xF800;   // pure red subpixels only
static const unsigned short L_ORANGE = 0xFC00;   // (255,128,0) hazard amber
static const unsigned short L_INK    = 0x0000;   // text ON the light (toast, lit footer)

enum { COL_WHITE, COL_RED, COL_ORANGE, COL_N };
enum { PAT_SOLID, PAT_BLINK, PAT_SOS, PAT_N };
static const unsigned short COLS[COL_N] = { L_WHITE, L_RED, L_ORANGE };
static const int HZ10[] = { 5, 10, 15, 20, 30 };          // blink rates in tenths of Hz (<= 3 Hz, see top)
#define HZ_N ((int)(sizeof HZ10 / sizeof HZ10[0]))

static bool s_on;                 // torch lit?
static int  s_prev_bright;        // brightness to restore when leaving the app
static int  s_col   = COL_WHITE;  // these four survive closing the app (for the boot): your light, your way
static int  s_pat   = PAT_SOLID;
static int  s_level = 100;        // backlight % while lit (10..100)
static int  s_hz    = 1;          // index into HZ10
static bool s_phase_on;           // current backlight phase of a blink / SOS
static int64_t s_phase_t0;        // when the current phase (or SOS element) started
static int  s_sos_i;              // SOS element index
static int64_t s_toast_until;     // lit: show what just changed, then back to pure light

static const char *col_name(int c)
{
    switch (c) {
        case COL_RED:    return TR5("Rossa", "Red", "Roja", "Rouge", "Rot");
        case COL_ORANGE: return TR5("Arancio", "Orange", "Naranja", "Orange", "Orange");
        default:         return TR5("Bianca", "White", "Blanca", "Blanche", "Weiss");
    }
}
static const char *pat_name(int p)
{
    switch (p) {
        case PAT_BLINK: return TR5("Lampeggio", "Blink", "Destello", "Clignot.", "Blinken");
        case PAT_SOS:   return "SOS";
        default:        return TR5("Fissa", "Solid", "Fija", "Fixe", "Dauer");
    }
}

// The G0 quick-tap overlay (nucleo_app.cpp) paints the light you picked here: an astronomer who set a
// dim red does not get blinded by a full-white flash on the next tap. Defaults = the old white / 100%.
extern "C" void nucleo_torch_overlay_style(unsigned short *col, int *pct)
{
    if (col) *col = COLS[s_col];
    if (pct) *pct = s_level;
}

// SOS in Morse units of 200 ms: S = ... O = --- (on, off) pairs; the last gap is the 7-unit word space.
static const unsigned char SOS[] = { 1,1, 1,1, 1,3,  3,1, 3,1, 3,3,  1,1, 1,1, 1,7 };
#define SOS_N ((int)sizeof SOS)
static const int SOS_UNIT_MS = 200;

static void tab(void);       // defined below (TAB cycles the pattern)
static bool back(int key);   // defined below (LEFT = previous colour)

static void backlight(bool on) { nucleo_ui_set_brightness(on ? (unsigned char)(s_level * 255 / 100) : 0); }

static void restart_pattern(void)
{
    s_phase_on = true; s_sos_i = 0; s_phase_t0 = esp_timer_get_time();
    if (s_on) backlight(true);
}

// Lit: backlight to the chosen level AND the footer takes the light colour with dark text — so the
// whole panel emits, not just the content area. Off: restore the user's brightness + the theme footer.
static void apply_state(void)
{
    if (s_on) {
        nucleo_app_set_brightness(s_level);
        nucleo_app_set_hint_colors(COLS[s_col], L_INK);
        restart_pattern();
    } else {
        nucleo_app_set_brightness(s_prev_bright);
        nucleo_app_set_hint_colors(INK, MUTED);
    }
}

static void changed(void)
{
    if (s_on) { s_toast_until = esp_timer_get_time() + 1200000; apply_state(); }
    nucleo_app_request_draw();
}

// Per-loop: drive the blink / SOS phases on the backlight (no redraw) and expire the toast (redraw).
static bool poll(void)
{
    if (!s_on) return false;
    int64_t now = esp_timer_get_time();
    bool redraw = false;
    if (s_toast_until && now >= s_toast_until) { s_toast_until = 0; redraw = true; }
    if (s_pat == PAT_BLINK) {
        int64_t half = 5000000LL / HZ10[s_hz];                      // 50% duty: half a period, in us
        if (now - s_phase_t0 >= half) { s_phase_t0 = now; s_phase_on = !s_phase_on; backlight(s_phase_on); }
    } else if (s_pat == PAT_SOS) {
        int64_t len = (int64_t)SOS[s_sos_i] * SOS_UNIT_MS * 1000;
        if (now - s_phase_t0 >= len) {
            s_phase_t0 = now; s_sos_i = (s_sos_i + 1) % SOS_N;
            s_phase_on = (s_sos_i % 2) == 0;                        // even = a mark, odd = the gap after it
            backlight(s_phase_on);
        }
    }
    return redraw;
}

static void enter(void)
{
    s_prev_bright = nucleo_app_brightness();   // remember so ESC restores the user's setting
    s_on = true;                               // a flashlight should light up the moment you open it
    s_toast_until = 0;
    apply_state();
    // A torch must not go dark on its own: veto the in-app idle screen-off (it used to drop the
    // backlight after the screensaver timeout, mid-walk).
    nucleo_app_set_keep_awake(true);
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    // <= 39 chars: the hint bar shows no more
    nucleo_app_set_hint(TR5("spazio on  1-3 col.  tab modo  U/D reg.", "spc on  1-3 color  tab mode  U/D adj.",
                            "esp. on  1-3 color  tab modo  U/D ajus.", "esp. on  1-3 coul.  tab mode  U/D regl.",
                            "Leer an  1-3 Farbe  tab Modus  U/D"));
    nucleo_app_request_draw();
}

static void set_col(int c) { s_col = (c + COL_N) % COL_N; changed(); }

static void on_key(int key, char ch)
{
    if (ch == ' ' || key == NK_ENTER) {        // ESC/back never reaches here: the framework closes the app
        s_on = !s_on; s_toast_until = 0;
        apply_state();
        nucleo_app_request_draw();
    }
    else if (ch >= '1' && ch <= '3') set_col(ch - '1');
    else if (key == NK_RIGHT) set_col(s_col + 1);
    else if (key == NK_UP || key == NK_DOWN) {
        int dir = key == NK_UP ? +1 : -1;
        if (s_pat == PAT_BLINK) {
            s_hz += dir; if (s_hz < 0) s_hz = 0; if (s_hz >= HZ_N) s_hz = HZ_N - 1;
        } else {
            s_level += dir * 10; if (s_level < 10) s_level = 10; if (s_level > 100) s_level = 100;
        }
        changed();
    }
}

// LEFT routes here (not to on_key): previous colour. Esc/Back falls through -> the framework closes.
static bool back(int key)
{
    if (key == NK_LEFT) { set_col(s_col - 1); return true; }
    return false;
}

static void tab(void) { s_pat = (s_pat + 1) % PAT_N; changed(); }

// Value of the one slider the current pattern exposes (Blink -> speed, else brightness).
static void slider_text(char *b, int n, int *pct)
{
    if (s_pat == PAT_BLINK) {
        int t = HZ10[s_hz];
        if (t % 10) snprintf(b, n, "%d.%d Hz", t / 10, t % 10); else snprintf(b, n, "%d Hz", t / 10);
        *pct = (s_hz + 1) * 100 / HZ_N;
    } else {
        snprintf(b, n, "%d%%", s_level);
        *pct = s_level;
    }
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    unsigned short lc = COLS[s_col];
    if (s_on) {
        // Pure light, no glyphs: any dark text would be light NOT emitted. The one exception is a short
        // toast right after a change, so you can see what U/D, TAB or 1-3 just did without switching off.
        d.fillRect(0, top, W, h, lc);
        if (s_toast_until) {
            char v[16]; int pct; slider_text(v, sizeof v, &pct);
            char t[40]; snprintf(t, sizeof t, "%s  %s  %s", col_name(s_col), pat_name(s_pat), v);
            int tw = (int)strlen(t) * 12, ts = 2;
            if (tw > W - 16) { ts = 1; tw = (int)strlen(t) * 6; }
            int bw = tw + 16, bh = ts * 8 + 12, bx = (W - bw) / 2, by = top + (h - bh) / 2;
            d.fillRoundRect(bx, by, bw, bh, 6, L_INK);
            d.setTextSize(ts); d.setTextColor(lc, L_INK);
            d.setCursor(bx + 8, by + 6); d.print(t);
        }
        return;
    }

    // Off: the setup screen — colour swatches, pattern tabs, the pattern's slider.
    const char *title = TR5("Torcia", "Torch", "Linterna", "Lampe", "Lampe");
    const char *rgt   = TR5("tap GO: ovunque", "tap GO: anywhere", "GO: en todo", "GO: partout", "GO: ueberall");
    if (10 + (int)strlen(title) * 12 + 8 > 238 - (int)strlen(rgt) * 6) rgt = "";
    int y = app_ui_title(title, lc == L_WHITE ? C_YELLOW : lc, rgt);

    // three big swatches, 1-3 quick-select: the light itself, with its number + name in ink on it
    const int sw = 70, sh = 30, gap = 8, x0 = (W - (3 * sw + 2 * gap)) / 2, sy = y + 4;
    for (int i = 0; i < COL_N; i++) {
        int x = x0 + i * (sw + gap);
        bool sel = i == s_col;
        d.fillRoundRect(x, sy, sw, sh, 6, sel ? COLS[i] : DIM);
        if (sel) d.drawRoundRect(x - 2, sy - 2, sw + 4, sh + 4, 8, FG);
        char lb[16]; snprintf(lb, sizeof lb, "%d %s", i + 1, col_name(i));
        int lw = (int)strlen(lb) * 6;
        d.setTextSize(1); d.setTextColor(sel ? L_INK : MUTED, sel ? COLS[i] : DIM);
        d.setCursor(x + (sw - lw) / 2, sy + (sh - 8) / 2); d.print(lb);
    }

    const char *pats[PAT_N] = { pat_name(PAT_SOLID), pat_name(PAT_BLINK), pat_name(PAT_SOS) };
    int ty = app_ui_tabs(sy + sh + 6, pats, PAT_N, s_pat, C_YELLOW);

    char v[16]; int pct; slider_text(v, sizeof v, &pct);
    const char *lbl = s_pat == PAT_BLINK ? TR5("Velocita", "Speed", "Velocidad", "Vitesse", "Tempo")
                                         : TR5("Luce", "Brightness", "Brillo", "Lumiere", "Helligkeit");
    app_ui_gauge(ty + 8, lbl, v, pct, lc == L_WHITE ? C_YELLOW : lc);
}

static void on_exit(void) { nucleo_app_set_brightness(s_prev_bright); }

extern "C" void nucleo_register_torch(void)
{
    static const nucleo_app_def_t app = {
        "torch", "Torch", "Tools", "Screen flashlight: white, red, orange; solid, blink, SOS",
        'T', C_YELLOW, enter, on_key, nullptr, draw, on_exit
    };
    nucleo_app_register(&app);
}

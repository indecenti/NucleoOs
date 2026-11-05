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
//   MODE_CLOCK — cyberdeck HUD clock    (canvas: grid + 7-segment digits + telemetry)
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
#include "nucleo_i18n.h"    // TR5(it,en,es,fr,de): the texts follow the system language
#include <M5GFX.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nucleo_power.h"   // the clock HUD shows the battery

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

// ==================== CYBERDECK CLOCK ========================================
// A HUD, not a bouncing label: 7-segment hours with their unlit segments showing, a neon glow and CRT scanlines,
// a 60-tick seconds rail, the date, and what a deck owner wants at a glance (battery, network, memory, uptime,
// today's next calendar event). Three looks, never the same information twice:
//   - the Cardputer alone: the whole HUD; a dot grid and a slow scan beam behind it, a two-frame glitch every ~7 s,
//     the bottom line turning through telemetry / network / next event;
//   - two screens: the PANEL has the time and the day (big digits, seconds, date, week, next event), calm and
//     repainted by rectangles; the CARDPUTER has the deck's status (battery, network, memory, uptime), no clock.
// Both canvases are 8-bit (RGB332), so every colour below is an exact RGB332 value (R/G in steps of 36, B in steps
// of 85): nothing turns olive. The draws are pure: all motion is derived from cl_frame, stepped in clock_step.

// The view the effects step and draw in: the Cardputer (240x135) or, with the panel attached, the whole
// 320x240 panel at its own resolution (never the compact frame scaled up). Set by saver_start.
static int s_vw = W, s_vh = H;

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
static const uint16_t CK_GRID = RGB(0, 36, 85);      // grid dots, empty ticks
static const uint16_t CK_OFF  = RGB(0, 0, 85);       // the unlit segments: present, never competing with the lit ones
static const uint16_t CK_DIM  = RGB(0, 73, 85);      // rules, brackets, the scan beam
static const uint16_t CK_GLOW = RGB(0, 109, 170);    // the neon bleed around the digits
static const uint16_t CK_CY   = RGB(0, 255, 255);    // the digits, live values
static const uint16_t CK_HI   = RGB(219, 255, 255);  // the current tick, the date
static const uint16_t CK_MG   = RGB(255, 36, 170);   // seconds, the glitch
static const uint16_t CK_AM   = RGB(255, 182, 0);    // warnings (low battery, clock not synced)
static const uint16_t CK_TX   = RGB(109, 146, 170);  // labels

static uint32_t cl_frame;                            // frames since the saver started
static int      cl_last_sec = -1;
static uint32_t cl_sec_frame;                        // the frame the current second began (colon blink)

// ---- what the HUD shows besides the time: sampled once a second (never inside a paint) --------------------------
static int      cl_bat = -1, cl_mv = -1, cl_rssi = 0;
static unsigned cl_heap_k = 0, cl_blk_k = 0;
static char     cl_ip[16], cl_ssid[33], cl_name[20], cl_tz[28];
static bool     cl_has_ev = false;                   // today's next calendar event
static char     cl_ev_t[6], cl_ev_x[40];

extern "C" int         nucleo_setup_rssi(void);      // dBm of the joined AP, 0 = not joined (nucleo_setup)
extern "C" const char *nucleo_setup_ip(void);        // "" when not joined
extern "C" const char *nucleo_setup_ssid(void);
extern "C" const char *nucleo_setup_mode(void);      // "sta" when a client of a network
extern "C" const char *nucleo_setup_device_name(void);
extern "C" const char *nucleo_setup_tz_id(void);     // IANA id, "Europe/Rome"
extern "C" bool        nucleo_calendar_next(char *hhmm, size_t hn, char *text, size_t tn);   // calendar_svc.cpp

static const char *str_or(const char *s) { return s ? s : ""; }

// Font0 is ASCII: fold the accented letters a calendar title is likely to carry, drop the rest of any UTF-8.
static void ascii_fold(char *out, size_t n, const char *in)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n; p++) {
        if (*p < 0x80) { out[o++] = (char)*p; continue; }
        if (*p == 0xC3 && p[1]) {
            const unsigned char c = *++p;
            const char *map = c < 0xA0 ? "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs" : "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
            out[o++] = map[(c & 0x1F)];
            continue;
        }
        while ((p[1] & 0xC0) == 0x80) p++;           // another script: skip the sequence
        out[o++] = '?';
    }
    out[o] = 0;
}

static void clock_sample(void)
{
    cl_bat    = nucleo_power_battery_pct();
    cl_mv     = nucleo_power_battery_mv();
    const bool sta = !strcmp(str_or(nucleo_setup_mode()), "sta") && str_or(nucleo_setup_ip())[0];
    cl_rssi   = sta ? nucleo_setup_rssi() : 0;
    snprintf(cl_ip, sizeof cl_ip, "%s", sta ? str_or(nucleo_setup_ip()) : "");
    snprintf(cl_ssid, sizeof cl_ssid, "%s", sta ? str_or(nucleo_setup_ssid()) : "");
    snprintf(cl_name, sizeof cl_name, "%s", str_or(nucleo_setup_device_name()));
    snprintf(cl_tz, sizeof cl_tz, "%s", str_or(nucleo_setup_tz_id()));
    for (char *p = cl_tz; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    cl_heap_k = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024);
    cl_blk_k  = (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024);
    char raw[64];
    cl_has_ev = nucleo_calendar_next(cl_ev_t, sizeof cl_ev_t, raw, sizeof raw);
    if (cl_has_ev) ascii_fold(cl_ev_x, sizeof cl_ev_x, raw); else cl_ev_x[0] = cl_ev_t[0] = 0;
}

static void clock_init(void)
{
    cl_frame = 0; cl_last_sec = -1; cl_sec_frame = 0;
    clock_sample();
}

static void clock_step(void)
{
    cl_frame++;
    const time_t t = time(NULL);
    const struct tm *tm = localtime(&t);
    const int sec = tm ? tm->tm_sec : 0;
    if (sec != cl_last_sec) { cl_last_sec = sec; cl_sec_frame = cl_frame; clock_sample(); }
}

// "Tue 29 Sep" in the system language (strftime has only the C locale here).
static void fmt_date(char *out, size_t n, const struct tm *tm)
{
    static const char *const DAY[5][7] = {
        { "Dom", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab" }, { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" },
        { "Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab" }, { "Dim", "Lun", "Mar", "Mer", "Jeu", "Ven", "Sam" },
        { "So", "Mo", "Di", "Mi", "Do", "Fr", "Sa" } };
    static const char *const MON[5][12] = {
        { "Gen", "Feb", "Mar", "Apr", "Mag", "Giu", "Lug", "Ago", "Set", "Ott", "Nov", "Dic" },
        { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
        { "Ene", "Feb", "Mar", "Abr", "May", "Jun", "Jul", "Ago", "Sep", "Oct", "Nov", "Dic" },
        { "Jan", "Fev", "Mar", "Avr", "Mai", "Jun", "Jul", "Aou", "Sep", "Oct", "Nov", "Dec" },
        { "Jan", "Feb", "Mrz", "Apr", "Mai", "Jun", "Jul", "Aug", "Sep", "Okt", "Nov", "Dez" } };
    const char *l = TR5("0", "1", "2", "3", "4");
    const int li = l[0] - '0';
    snprintf(out, n, "%s %02d %s", DAY[li][tm->tm_wday % 7], tm->tm_mday, MON[li][tm->tm_mon % 12]);
}

static void upcase(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32); }

// ISO 8601 week of the year (1..53).
static int iso_week(const struct tm *tm)
{
    auto weeks_in = [](int y) {
        auto p = [](int v) { return (v + v / 4 - v / 100 + v / 400) % 7; };
        return (p(y) == 4 || p(y - 1) == 3) ? 53 : 52;
    };
    const int y = tm->tm_year + 1900, wd = (tm->tm_wday + 6) % 7 + 1;   // Mon = 1 .. Sun = 7
    const int w = (tm->tm_yday + 1 - wd + 10) / 7;
    if (w < 1) return weeks_in(y - 1);
    if (w > weeks_in(y)) return 1;
    return w;
}

// Minutes from now to "HH:MM" today (0 when it is now or past).
static int mins_until(const char *hhmm, const struct tm *tm)
{
    if (!hhmm || strlen(hhmm) < 5) return 0;
    const int m = (hhmm[0] - '0') * 600 + (hhmm[1] - '0') * 60 + (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
    const int dm = m - (tm->tm_hour * 60 + tm->tm_min);
    return dm > 0 ? dm : 0;
}

// ---- HUD pieces ------------------------------------------------------------------------------------------------
static void hud_grid(int x0, int y0, int w, int h, int step)
{
    for (int y = y0 + step / 2; y < y0 + h; y += step)
        for (int x = x0 + step / 2; x < x0 + w; x += step) d.drawPixel(x, y, CK_GRID);
}

static void hud_brackets(int x, int y, int w, int h, int len, uint16_t col)
{
    d.drawFastHLine(x, y, len, col);                 d.drawFastVLine(x, y, len, col);
    d.drawFastHLine(x + w - len, y, len, col);       d.drawFastVLine(x + w - 1, y, len, col);
    d.drawFastHLine(x, y + h - 1, len, col);         d.drawFastVLine(x, y + h - len, len, col);
    d.drawFastHLine(x + w - len, y + h - 1, len, col); d.drawFastVLine(x + w - 1, y + h - len, len, col);
}

// 60 ticks over [x, x+w): the elapsed ones lit, the current one bright and taller, every 5th a little taller.
static void hud_ticks(int x, int y, int w, int h, int sec)
{
    for (int i = 0; i < 60; i++) {
        const int x0 = x + i * w / 60, tw = (x + (i + 1) * w / 60) - x0 - 1;
        const int ext = (i % 5 == 0) ? 2 : 0;
        const uint16_t c = i < sec ? (i % 5 == 0 ? CK_CY : CK_GLOW) : i == sec ? CK_HI : CK_GRID;
        if (i == sec) d.fillRect(x0, y - 3, tw, h + 6, c);
        else          d.fillRect(x0, y - ext, tw, h + ext, c);
    }
}

// A battery outline filled to pct (amber at 20 % and below); "--" in the caller when there is no reading.
static void hud_battery(int x, int y, int w, int h, int pct)
{
    d.drawRect(x, y, w, h, CK_TX);
    d.fillRect(x + w, y + h / 4, 2, h - h / 2, CK_TX);
    if (pct >= 0) d.fillRect(x + 2, y + 2, (w - 4) * (pct > 100 ? 100 : pct) / 100, h - 4, pct <= 20 ? CK_AM : CK_CY);
}

// Four signal bars, bottom-aligned at (x, y + 9), lit by the RSSI (0 = not joined: all dim).
static void hud_signal(int x, int y, int rssi)
{
    const int lv = !rssi ? 0 : rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -75 ? 2 : rssi > -85 ? 1 : 0;
    for (int i = 0; i < 4; i++) d.fillRect(x + i * 5, y + 9 - (3 + i * 2), 3, 3 + i * 2, i < lv ? (lv <= 1 ? CK_AM : CK_CY) : CK_GRID);
}

// A "label value" pair, label muted and value in col, from x; returns the x after it.
static int hud_kv(int x, int y, int size, const char *k, const char *v, uint16_t col)
{
    d.setFont(&fonts::Font0); d.setTextSize(size); d.setTextDatum(top_left);
    d.setTextColor(CK_TX); d.drawString(k, x, y);
    x += d.textWidth(k) + 3 * size;
    d.setTextColor(col); d.drawString(v, x, y);
    return x + d.textWidth(v);
}

// The 7-segment "HH:MM" with its top-left at (x, y): unlit segments, neon bleed, the lit digits.
// Font7 digits share one width, so "88:88" is exactly the ghost of any time.
static void hud_digits(int x, int y, int size, int hh, int mm, bool colon, uint16_t col, bool ghost)
{
    char hs[3], ms[3];
    snprintf(hs, sizeof hs, "%02d", hh % 100); snprintf(ms, sizeof ms, "%02d", mm % 100);
    d.setFont(&fonts::Font7); d.setTextSize(size); d.setTextDatum(top_left);
    const int xc = x + d.textWidth("88"), xm = xc + d.textWidth(":");
    if (ghost) { d.setTextColor(CK_OFF); d.drawString("88:88", x, y); }
    if (col == CK_CY) {                              // the neon bleed (the glitch copy has none)
        static const int8_t OFF[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
        d.setTextColor(CK_GLOW);
        for (int i = 0; i < 4; i++) {
            const int ox = OFF[i][0] * size, oy = OFF[i][1] * size;
            d.drawString(hs, x + ox, y + oy);
            if (colon) d.drawString(":", xc + ox, y + oy);
            d.drawString(ms, xm + ox, y + oy);
        }
    }
    d.setTextColor(col);
    d.drawString(hs, x, y);
    if (colon) d.drawString(":", xc, y);
    d.drawString(ms, xm, y);
}

static void hud_scanlines(int x, int y, int w, int h, int pitch)
{
    for (int yy = y + pitch - 1; yy < y + h; yy += pitch) d.drawFastHLine(x, yy, w, 0x0000);
}

// The two-frame glitch (the Cardputer alone only): a band of the digits redrawn shifted in magenta. Deterministic
// from cl_frame. The clip is narrowed, then restored (never cleared).
static void hud_glitch(int x, int y, int size, int w, int h, int hh, int mm)
{
    if (cl_frame < 20 || (cl_frame % 71) >= 2) return;   // ~7 s apart at 10 fps, two frames each
    uint32_t r = (cl_frame / 71 + 1) * 2654435761u;    // one band per glitch, the same for both of its frames
    r ^= r >> 15; r *= 2246822519u; r ^= r >> 13;
    const int bh = (5 + (int)((r >> 28) % 6u)) * size, by = y + (int)((r >> 8) % (uint32_t)(h - bh));
    const int dx = (((r >> 4) & 1) ? 1 : -1) * (4 + (int)((r >> 16) & 3)) * size;
    int32_t cx, cy, cw, ch;
    d.getClipRect(&cx, &cy, &cw, &ch);
    const int y0 = by > cy ? by : (int)cy, y1 = (by + bh) < (cy + ch) ? (by + bh) : (int)(cy + ch);
    if (y1 > y0) {
        d.setClipRect(cx, y0, cw, y1 - y0);
        d.fillRect(x - 8 * size, by, w + 16 * size, bh, 0x0000);
        hud_digits(x + dx, y, size, hh, mm, true, CK_MG, false);
        hud_digits(x + dx / 3, y, size, hh, mm, true, CK_CY, false);
        d.setClipRect(cx, cy, cw, ch);
    }
}

static void fmt_uptime(char *out, size_t n)
{
    const unsigned m = (unsigned)(esp_timer_get_time() / 60000000LL);
    if (m < 100 * 60) snprintf(out, n, "%02u:%02u", m / 60, m % 60);
    else              snprintf(out, n, "%ud%02u", m / 1440, (m / 60) % 24);
}

// "in 2h15" / "in 25 min" to an event, in the system language.
static void fmt_until(char *out, size_t n, int mins)
{
    const char *in = TR5("tra", "in", "en", "dans", "in");
    if (mins <= 0)      snprintf(out, n, "%s", TR5("ora", "now", "ahora", "maint.", "jetzt"));
    else if (mins < 60) snprintf(out, n, "%s %d min", in, mins);
    else                snprintf(out, n, "%s %dh%02d", in, mins / 60, mins % 60);
}

// The rows the Cardputer frame really has: the ADV's back-buffer is fitted to its largest block (240x130), so a
// bottom line laid out against H (135) loses its last rows there.
static int canvas_h(void) { const int h = (int)d.height(); return h > 0 && h < H ? h : H; }

// The Cardputer's top bar (mark, week, activity, battery) and its rule; returns the rule's y.
static int compact_top(const struct tm *tm)
{
    d.fillRect(3, 2, 3, 8, CK_CY);
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left);
    d.setTextColor(CK_CY); d.drawString("NUCLEO//OS", 8, 2);
    if (tm) {
        char wk[12]; snprintf(wk, sizeof wk, "%s %d", TR5("SETT", "WK", "SEM", "SEM", "KW"), iso_week(tm));
        d.setTextColor(CK_TX); d.drawString(wk, 72, 2);
    }
    for (int i = 0; i < 8; i++) {
        const int on = (int)(cl_frame % 14u), k = on < 8 ? on : 14 - on;   // ping-pong
        d.fillRect(126 + i * 5, 5, 2, 2, i == k ? CK_MG : CK_GRID);
    }
    char v[8];
    if (cl_bat >= 0) snprintf(v, sizeof v, "%d%%", cl_bat); else snprintf(v, sizeof v, "--");
    d.setTextDatum(top_right); d.setTextColor(cl_bat >= 0 && cl_bat <= 20 ? CK_AM : CK_CY);
    d.drawString(v, W - 3, 2);
    hud_battery(W - 3 - d.textWidth(v) - 14 - 5, 2, 14, 8, cl_bat);
    d.setTextDatum(top_left);
    d.drawFastHLine(0, 13, W, CK_DIM);
    d.fillRect(0, 12, 56, 2, CK_CY);
    d.fillRect(W - 24, 12, 24, 2, CK_MG);
    return 13;
}

// ==== the Cardputer ALONE: the whole HUD on 240x135 ===============================================================
// One canvas, one blit per frame (Technique 1), so the beam and the glitch are safe here. The bottom line turns
// every 5 s through the telemetry, the network and today's next event, so the little screen carries all of it.
static void clock_frame(void)                        // pure: draws the state, never moves it
{
    d.fillRect(0, 0, W, H, 0x0000);
    hud_grid(0, 0, W, H, 8);
    const int beam = (int)((cl_frame * 2) % (uint32_t)(H + 40)) - 20;   // the slow scan beam
    d.drawFastHLine(0, beam - 1, W, CK_GRID);
    d.drawFastHLine(0, beam, W, CK_DIM);

    const time_t t = time(NULL);
    const struct tm *tm = localtime(&t);
    const int hh = tm ? tm->tm_hour : 0, mm = tm ? tm->tm_min : 0, ss = tm ? tm->tm_sec : 0;
    const bool synced = tm && tm->tm_year + 1900 >= 2024;
    const bool colon = (cl_frame - cl_sec_frame) < 5;   // on for the first half of each second
    const int rule = compact_top(tm);

    d.setFont(&fonts::Font7); d.setTextSize(1);
    const int dw = d.textWidth("88:88"), dh = 48, sw = 52;
    const int dx = (W - dw - sw - 8) / 2, dy = rule + 9;
    hud_brackets(dx - 8, dy - 6, dw + sw + 24, dh + 12, 6, CK_DIM);
    hud_digits(dx, dy, 1, hh, mm, colon, CK_CY, true);
    char sec2[4]; snprintf(sec2, sizeof sec2, "%02d", ss);
    const int sx = dx + dw + 8;
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(CK_TX); d.drawString("SEC", sx + 2, dy + 2);
    d.setTextSize(4); d.setTextColor(CK_MG); d.drawString(sec2, sx, dy + 16);
    hud_scanlines(dx - 4, dy - 2, dw + sw + 16, dh + 4, 3);
    hud_glitch(dx, dy, 1, dw, dh, hh, mm);

    const int ry = dy + dh + 11, rw = dw + sw + 8;
    hud_ticks(dx, ry, rw, 4, ss);

    char date[24], year[12];
    if (tm) { fmt_date(date, sizeof date, tm); upcase(date); } else snprintf(date, sizeof date, "--- -- ---");
    if (synced) snprintf(year, sizeof year, "%04d", tm->tm_year + 1900); else snprintf(year, sizeof year, "NO SYNC");
    const int dty = ry + 12;
    d.setFont(&fonts::Font0); d.setTextSize(2);
    d.setTextColor(CK_HI); d.drawString(date, dx, dty);
    d.setTextDatum(top_right); d.setTextColor(synced ? CK_TX : CK_AM); d.drawString(year, dx + rw, dty);
    d.setTextDatum(top_left);

    const int by = canvas_h() - 14;                  // the ADV's fitted canvas is 130 rows, not 135
    d.drawFastHLine(0, by - 4, W, CK_DIM);
    const int pages = cl_has_ev ? 3 : 2, page = (int)((t / 5) % pages);
    if (page == 0) {
        char sig[12], mem[12], up[12];
        if (cl_rssi) snprintf(sig, sizeof sig, "%d", cl_rssi); else snprintf(sig, sizeof sig, "OFF");
        snprintf(mem, sizeof mem, "%uK", cl_heap_k);
        fmt_uptime(up, sizeof up);
        int x = hud_kv(4, by + 2, 1, "SIG", sig, !cl_rssi ? CK_TX : cl_rssi < -80 ? CK_AM : CK_CY);
        x = hud_kv(x + 14, by + 2, 1, "MEM", mem, CK_CY);
        hud_kv(x + 14, by + 2, 1, "UP", up, CK_CY);
    } else if (page == 1) {
        if (cl_ip[0]) {
            const int x = hud_kv(4, by + 2, 1, "IP", cl_ip, CK_CY) + 14;
            char ss[40]; lg::fit(ss, sizeof ss, cl_ssid, W - 4 - (x + d.textWidth("NET") + 3));   // a long SSID: cut with ".."
            hud_kv(x, by + 2, 1, "NET", ss, CK_TX);
        }
        else hud_kv(4, by + 2, 1, "NET", TR5("non connesso", "not connected", "sin conexion", "non connecte", "nicht verbunden"), CK_AM);
    } else {
        char lab[8], u[16]; snprintf(lab, sizeof lab, "%s", TR5("OGGI", "TODAY", "HOY", "AUJ.", "HEUTE"));
        fmt_until(u, sizeof u, tm ? mins_until(cl_ev_t, tm) : 0);
        int x = hud_kv(4, by + 2, 1, lab, cl_ev_t, CK_MG);
        char ev[40]; lg::fit(ev, sizeof ev, cl_ev_x, W - 4 - d.textWidth(u) - 8 - (x + 6));   // never under "in 25 min"
        d.setTextColor(CK_HI); d.drawString(ev, x + 6, by + 2);
        d.setTextDatum(top_right); d.setTextColor(CK_TX); d.drawString(u, W - 4, by + 2); d.setTextDatum(top_left);
    }
}

// ==== two screens, the Cardputer: the deck's status (no time, no date: the panel has them) ========================
static void status_frame(void)
{
    d.fillRect(0, 0, W, H, 0x0000);
    hud_grid(0, 0, W, H, 8);
    d.fillRect(3, 2, 3, 8, CK_CY);
    d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left);
    d.setTextColor(CK_CY); d.drawString("NUCLEO//OS", 8, 2);
    d.setTextColor(CK_TX); d.drawString(TR5("STATO", "STATUS", "ESTADO", "ETAT", "STATUS"), 72, 2);
    if (cl_name[0]) { char nm[16]; snprintf(nm, sizeof nm, "%.13s", cl_name); d.setTextDatum(top_right); d.drawString(nm, W - 3, 2); d.setTextDatum(top_left); }
    d.drawFastHLine(0, 13, W, CK_DIM);
    d.fillRect(0, 12, 56, 2, CK_CY);
    d.fillRect(W - 24, 12, 24, 2, CK_MG);
    d.drawFastVLine(118, 18, 58, CK_DIM);

    // power
    char v[16];
    d.setTextColor(CK_TX); d.drawString(TR5("BATTERIA", "BATTERY", "BATERIA", "BATTERIE", "AKKU"), 4, 18);
    if (cl_bat >= 0) snprintf(v, sizeof v, "%d%%", cl_bat); else snprintf(v, sizeof v, "--");
    d.setTextSize(3); d.setTextColor(cl_bat >= 0 && cl_bat <= 20 ? CK_AM : CK_CY); d.drawString(v, 4, 30);
    hud_battery(4, 57, 104, 8, cl_bat);
    d.setTextSize(1); d.setTextColor(CK_TX);
    if (cl_mv > 0) { snprintf(v, sizeof v, "%d mV", cl_mv); d.drawString(v, 4, 68); }

    // network
    d.setTextColor(CK_TX); d.drawString(TR5("RETE", "NETWORK", "RED", "RESEAU", "NETZ"), 124, 18);
    hud_signal(W - 22, 17, cl_rssi);
    if (cl_ip[0]) {
        // The SSID big when it fits, else small and cut with ".." (never chopped mid-word in silence).
        char ss[40]; const int sw = W - 4 - 124;
        d.setTextSize(2);
        const bool big = d.textWidth(cl_ssid) <= sw;
        if (!big) d.setTextSize(1);
        lg::fit(ss, sizeof ss, cl_ssid, sw);
        d.setTextColor(CK_HI); d.drawString(ss, 124, big ? 30 : 34);
        d.setTextSize(1); hud_kv(124, 52, 1, "IP", cl_ip, CK_CY);
        snprintf(v, sizeof v, "%d dBm", cl_rssi);
        d.setTextColor(cl_rssi < -80 ? CK_AM : CK_TX); d.drawString(v, 124, 66);
    } else {
        d.setTextSize(2); d.setTextColor(CK_AM); d.drawString("OFFLINE", 124, 30);
        d.setTextSize(1); d.setTextColor(CK_TX); d.drawString(TR5("nessuna rete", "no network", "sin red", "aucun reseau", "kein Netz"), 124, 52);
    }

    // system
    d.drawFastHLine(0, 81, W, CK_DIM);
    char mem[12], blk[12], up[12];
    snprintf(mem, sizeof mem, "%uK", cl_heap_k); snprintf(blk, sizeof blk, "%uK", cl_blk_k); fmt_uptime(up, sizeof up);
    static const int CX[3] = { 4, 84, 164 };
    const char *K[3] = { TR5("MEMORIA", "MEMORY", "MEMORIA", "MEMOIRE", "SPEICHER"), TR5("BLOCCO", "BLOCK", "BLOQUE", "BLOC", "BLOCK"),
                         TR5("ACCESO DA", "UPTIME", "ACTIVO", "ACTIF", "LAUFZEIT") };
    const char *V[3] = { mem, blk, up };
    for (int i = 0; i < 3; i++) {
        d.fillRect(CX[i] - 2, 86, 2, 24, i == 0 ? CK_MG : CK_DIM);
        d.setTextSize(1); d.setTextColor(CK_TX); d.drawString(K[i], CX[i] + 3, 86);
        d.setTextSize(2); d.setTextColor(CK_CY); d.drawString(V[i], CX[i] + 3, 96);
    }
    const int hb = canvas_h();
    d.drawFastHLine(0, hb - 15, W, CK_DIM);
    d.setTextSize(1); d.setTextColor(CK_CY); d.setTextDatum(top_center);   // the one action: lit, not muted
    d.drawString(TR5("un tasto per uscire", "any key to exit", "una tecla para salir", "une touche pour sortir", "Taste zum Beenden"), W / 2, hb - 10);
    d.setTextDatum(top_left);
}

// ==== two screens, the panel: the time and the day, 320x240 ======================================================
// Calm by design and repainted by rectangles: the whole panel once (start, a new minute, a new event, a repaint
// the OS asks for), then each second only the seconds rail, and twice a second only the colon. Every rectangle is
// composed off-screen and pushed once (lg::frame / lg::frame_rect): nothing on the panel is ever cleared and
// redrawn in place, and nothing moves that does not have to (no beam, no glitch here).
// panel_paint draws in panel coordinates shifted by s_ox (frame_rect's x is relative to its rectangle) and skips the
// blocks outside the window being painted.
static int s_ox, s_wx0, s_wy0, s_wx1, s_wy1;
static inline bool pn_vis(int x, int y, int w, int h) { return x < s_wx1 && x + w > s_wx0 && y < s_wy1 && y + h > s_wy0; }
#define PXX(x) ((x) - s_ox)

static struct { int dw, dx, dy, dh, xc, cw, ry; bool ok; } pl;
static void panel_layout(void)
{
    if (pl.ok) return;
    d.setFont(&fonts::Font7); d.setTextSize(2);
    pl.dw = d.textWidth("88:88"); pl.dh = 96;
    pl.dx = (320 - pl.dw) / 2; pl.dy = 40;
    pl.xc = pl.dx + d.textWidth("88"); pl.cw = d.textWidth(":");
    pl.ry = pl.dy + pl.dh + 16;
    pl.ok = true;
}
#define PN_RAIL_X (pl.dx - 2)
#define PN_RAIL_Y (pl.ry - 9)
#define PN_RAIL_W (pl.dw + 4)
#define PN_RAIL_H 26

static void panel_paint(void *)
{
    panel_layout();
    const time_t t = time(NULL);
    const struct tm *tm = localtime(&t);
    const int hh = tm ? tm->tm_hour : 0, mm = tm ? tm->tm_min : 0, ss = tm ? tm->tm_sec : 0;
    const bool synced = tm && tm->tm_year + 1900 >= 2024;
    const bool colon = (cl_frame - cl_sec_frame) < 5;
    d.fillRect(PXX(s_wx0), s_wy0, s_wx1 - s_wx0, s_wy1 - s_wy0, 0x0000);
    for (int y = 5; y < 240; y += 10)                    // the dot grid, inside the window only
        if (y >= s_wy0 && y < s_wy1)
            for (int x = 5; x < 320; x += 10) if (x >= s_wx0 && x < s_wx1) d.drawPixel(PXX(x), y, CK_GRID);

    if (pn_vis(0, 0, 320, 28)) {                         // the top bar: the mark, the time zone
        d.fillRect(PXX(8), 4, 5, 16, CK_CY);
        d.setFont(&fonts::Font0); d.setTextSize(2); d.setTextDatum(top_left);
        d.setTextColor(CK_CY); d.drawString("NUCLEO//OS", PXX(18), 5);
        if (cl_tz[0]) { d.setTextSize(1); d.setTextDatum(top_right); d.setTextColor(CK_TX); d.drawString(cl_tz, PXX(312), 9); d.setTextDatum(top_left); }
        d.drawFastHLine(PXX(0), 26, 320, CK_DIM);
        d.fillRect(PXX(0), 25, 90, 2, CK_CY);
        d.fillRect(PXX(280), 25, 40, 2, CK_MG);
    }
    if (pn_vis(pl.dx - 8, pl.dy - 6, pl.dw + 16, pl.dh + 12)) {   // the digits
        hud_brackets(PXX(pl.dx - 8), pl.dy - 6, pl.dw + 16, pl.dh + 12, 10, CK_DIM);
        hud_digits(PXX(pl.dx), pl.dy, 2, hh, mm, colon, CK_CY, true);
        hud_scanlines(PXX(pl.dx - 4), pl.dy - 2, pl.dw + 8, pl.dh + 4, 4);
    }
    if (pn_vis(PN_RAIL_X, PN_RAIL_Y, PN_RAIL_W, PN_RAIL_H)) {   // the seconds rail + the seconds
        hud_ticks(PXX(pl.dx), pl.ry, pl.dw - 56, 7, ss);
        char sec2[4]; snprintf(sec2, sizeof sec2, "%02d", ss);
        d.setFont(&fonts::Font0); d.setTextSize(3); d.setTextDatum(top_right);
        d.setTextColor(CK_MG); d.drawString(sec2, PXX(pl.dx + pl.dw), pl.ry - 8);
        d.setTextDatum(top_left);
    }
    if (pn_vis(0, 172, 320, 33)) {                       // the date, the year, the week and the day of the year
        char date[24], year[12], line[40];
        if (tm) { fmt_date(date, sizeof date, tm); upcase(date); } else snprintf(date, sizeof date, "--- -- ---");
        if (synced) snprintf(year, sizeof year, "%04d", tm->tm_year + 1900); else snprintf(year, sizeof year, "NO SYNC");
        d.setFont(&fonts::Font0); d.setTextSize(2);
        d.setTextColor(CK_HI); d.drawString(date, PXX(pl.dx), 176);
        d.setTextDatum(top_right); d.setTextColor(synced ? CK_TX : CK_AM); d.drawString(year, PXX(pl.dx + pl.dw), 176);
        d.setTextDatum(top_left);
        if (tm) {
            snprintf(line, sizeof line, "%s %d   %s %d/%d", TR5("SETTIMANA", "WEEK", "SEMANA", "SEMAINE", "WOCHE"), iso_week(tm),
                     TR5("GIORNO", "DAY", "DIA", "JOUR", "TAG"), tm->tm_yday + 1,
                     ((tm->tm_year + 1900) % 4 == 0 && ((tm->tm_year + 1900) % 100 != 0 || (tm->tm_year + 1900) % 400 == 0)) ? 366 : 365);
            d.setTextSize(1); d.setTextColor(CK_TX); d.drawString(line, PXX(pl.dx), 197);
        }
    }
    if (pn_vis(0, 205, 320, 35)) {                       // today's next event
        d.drawFastHLine(PXX(0), 206, 320, CK_DIM);
        d.fillRect(PXX(8), 211, 3, 26, CK_MG);
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(CK_TX);
        d.drawString(TR5("AGENDA DI OGGI", "TODAY", "HOY", "AUJOURD'HUI", "HEUTE"), PXX(16), 211);
        if (cl_has_ev && tm) {
            char u[16]; fmt_until(u, sizeof u, mins_until(cl_ev_t, tm));
            d.setTextDatum(top_right); d.drawString(u, PXX(312), 211); d.setTextDatum(top_left);
            d.setTextSize(2); d.setTextColor(CK_MG); d.drawString(cl_ev_t, PXX(16), 222);
            char ev[40]; lg::fit(ev, sizeof ev, cl_ev_x, 312 - (16 + 6 * 12));
            d.setTextColor(CK_HI); d.drawString(ev, PXX(16 + 6 * 12), 222);
        } else {
            d.setTextSize(2); d.setTextColor(CK_GLOW); d.drawString("--:--", PXX(16), 222);
            d.setTextSize(1); d.setTextColor(CK_TX);
            d.drawString(TR5("nessun altro evento oggi", "no more events today", "no hay mas eventos hoy", "plus d'evenement aujourd'hui", "heute keine Termine mehr"), PXX(16 + 6 * 12), 226);
        }
    }
}

// One rectangle of the panel, composed and pushed once; false when there is no canvas (the caller repaints all).
static bool panel_rect(int x, int y, int w, int h)
{
    s_ox = x; s_wx0 = x; s_wy0 = y; s_wx1 = x + w; s_wy1 = y + h;
    const bool ok = lg::frame_rect(panel_paint, nullptr, x, y, w, h);
    s_ox = 0; s_wx0 = 0; s_wy0 = 0; s_wx1 = 320; s_wy1 = 240;
    return ok;
}

static struct { bool ok; unsigned gen; int key, sec; bool colon; char sig[96]; } pn;

static void panel_update(void)
{
    panel_layout();
    const time_t t = time(NULL);
    const struct tm *tm = localtime(&t);
    const int key = tm ? (tm->tm_yday * 1440 + tm->tm_hour * 60 + tm->tm_min) : 0, ss = tm ? tm->tm_sec : 0;
    const bool colon = (cl_frame - cl_sec_frame) < 5;
    char sig[96];                                        // what the slow parts show: a change repaints the panel
    snprintf(sig, sizeof sig, "%d|%s|%s|%s|%d", tm && tm->tm_year + 1900 >= 2024, cl_tz, cl_ev_t, cl_ev_x, cl_has_ev);
    const unsigned gen = nucleo_app_repaint_gen();
    if (!pn.ok || gen != pn.gen || key != pn.key || strcmp(sig, pn.sig)) {
        s_ox = 0; s_wx0 = 0; s_wy0 = 0; s_wx1 = 320; s_wy1 = 240;
        lg::frame(panel_paint, nullptr);
        pn.ok = true; pn.gen = gen; pn.key = key; pn.sec = ss; pn.colon = colon;
        snprintf(pn.sig, sizeof pn.sig, "%s", sig);
        return;
    }
    if (ss != pn.sec) {
        if (!panel_rect(PN_RAIL_X, PN_RAIL_Y, PN_RAIL_W, PN_RAIL_H)) { pn.ok = false; return; }
        pn.sec = ss;
    }
    if (colon != pn.colon) {
        if (!panel_rect(pl.xc - 4, pl.dy - 4, pl.cw + 8, pl.dh + 8)) { pn.ok = false; return; }
        pn.colon = colon;
    }
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
    if (g_mode == MODE_CLOCK || nucleo_app_two_screens()) clock_step();   // the status deck samples it beside any effect
    if (g_mode == MODE_STARS) stars_step();
    else if (g_mode == MODE_FIRE && fire_buf) { fire_seed(); fire_step(); }   // source always full: flame at the bottom
}
static void effect_draw(void *)
{
    switch (g_mode) {
        case MODE_CLOCK: if (s_vw > W) panel_paint(nullptr); else clock_frame(); break;
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
        if (big) clock_init();                       // the Cardputer's status deck beside the effect
        stars_init();
    } else {
        if (big) clock_init();
        fire_init();
    }
    pn.ok = false;                                   // the panel's clock: painted whole on its first frame
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
    static const char *const D[]    = { "schermo spento","hud cyberdeck","campo stellare","fuoco", nullptr };
    static const char *const D_EN[] = { "screen off","cyberdeck hud","starfield","fire", nullptr };
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
        int y0 = app_ui_title(TR5("EFFETTO", "EFFECT", "EFECTO", "EFFET", "EFFEKT"), C_PURPLE, "TAB ►");
        app_ui_list(y0, ch - 24, 5, s_sel, mode_label, mode_right, mode_color, nullptr);
    } else {
        int y0 = app_ui_title("TIMER", C_PURPLE, "◄ TAB");
        int rem = ch - 24;
        d.fillRect(0, y0, W, rem, BG);

        char sv[20]; fmt_timer(sv, sizeof sv);

        const char *sub = TR5("Attivazione automatica dopo", "Starts automatically after", "Se activa sola tras", "Demarre seul apres", "Startet automatisch nach");
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

        const char *sts = g_threshold_ms > 0 ? TR5("Attivo", "On", "Activo", "Actif", "An") : TR5("Disabilitato", "Disabled", "Desactivado", "Desactive", "Aus");
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
        case MODE_OFF:   return TR5("Spento", "Off", "Apagado", "Eteint", "Aus");
        case MODE_STARS: return TR5("Stelle", "Stars", "Estrellas", "Etoiles", "Sterne");
        case MODE_FIRE:  return TR5("Fuoco", "Fire", "Fuego", "Feu", "Feuer");
        default:         return TR5("Orologio", "Clock", "Reloj", "Horloge", "Uhr");
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
        d.drawString(TR5("Schermo spento", "Screen off", "Pantalla apagada", "Ecran eteint", "Bildschirm aus"), cx, cy); d.setTextDatum(top_left); return;
    }
    if (mode == MODE_CLOCK) {                        // a still of the HUD: grid, brackets, 12:34, the rail, the date
        hud_grid(x + 2, y + 2, w - 4, h - 4, 8);
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextDatum(top_left);
        d.fillRect(x + 8, y + 8, 3, 8, CK_CY);
        d.setTextColor(CK_CY); d.drawString("NUCLEO//OS", x + 14, y + 8);
        d.drawFastHLine(x + 6, y + 20, w - 12, CK_DIM);
        d.setFont(&fonts::Font7); d.setTextSize(1);
        const int dw = d.textWidth("88:88"), dx = cx - dw / 2, dy = cy - 30;
        hud_brackets(dx - 6, dy - 5, dw + 12, 58, 6, CK_DIM);
        hud_digits(dx, dy, 1, 12, 34, true, CK_CY, true);
        hud_scanlines(dx - 3, dy - 2, dw + 6, 52, 3);
        hud_ticks(dx, dy + 60, dw, 4, 41);
        struct tm ex = {}; ex.tm_wday = 2; ex.tm_mday = 5; ex.tm_mon = 4;   // a sample date
        char dat[20]; fmt_date(dat, sizeof dat, &ex); upcase(dat);
        d.setFont(&fonts::Font0); d.setTextSize(1); d.setTextColor(CK_HI); d.drawString(dat, dx, dy + 72);
        d.setTextDatum(top_right); d.setTextColor(CK_MG); d.drawString("41", dx + dw, dy + 72);
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
    if (i == 4) { s->label = TR5("Prova", "Test", "Probar", "Tester", "Testen"); s->kind = lg::ROW_NAV; s->lcol = C_YELLOW; }   // Font2 has no "►"
    if (i == 5) { s->label = "Timer"; s->kind = lg::ROW_NAV; }        // its value: the header and the pane
}

// The timer pane: the value big, its state, and a slider over 5 s .. 10 min (tap anywhere on it to set).
static void lg_timer(int x, int y, int w, int h)
{
    char sv[20]; fmt_timer(sv, sizeof sv);
    lg::panel(x, y, w, h);
    const unsigned short cb = lg::card();
    lg::font(1); d.setTextColor(MUTED, cb); d.setTextDatum(top_center);
    d.drawString(TR5("Si attiva dopo", "Starts after", "Se activa tras", "Demarre apres", "Startet nach"), x + w / 2, y + 12);
    lg::font(3); d.setTextColor(C_YELLOW, cb); d.drawString(sv, x + w / 2, y + 36);
    lg::font(0); d.setTextColor(g_threshold_ms > 0 ? C_GREEN : C_RED, cb);
    d.drawString(g_threshold_ms > 0 ? TR5("Attivo", "On", "Activo", "Actif", "An") : TR5("Disabilitato", "Disabled", "Desactivado", "Desactive", "Aus"), x + w / 2, y + 74);
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
        if (g_mode == MODE_CLOCK) panel_update();    // by rectangles: never a whole repaint per frame
        else lg::frame(effect_draw, nullptr);
        nucleo_touch_commit();
        return;
    }
    char rt[20]; fmt_timer(rt, sizeof rt);
    const int y = lg::header(TR5("Salvaschermo", "Screensaver", "Salvapantallas", "Economiseur", "Bildschirmschoner"), rt, C_PURPLE);
    lg::settings_list(8, y + 2, PX - 12, 240 - y - 2, 34, 6, s_tab ? 5 : s_sel, &s_lg_scroll, lg_mode_row, nullptr, C_PURPLE);
    d.fillRect(0, y, 8, 240 - y, BG);                // the margin left of the list (drawn straight, no clear)
    d.fillRect(PX - 4, y, 324 - PX, 240 - y, BG);
    const int py = y + 6, ph = 240 - y - 12;
    if (s_tab) { lg_timer(PX, py, PW, ph); nucleo_touch_commit(); return; }
    preview_effect(s_sel < 4 ? s_sel : g_mode, PX, py, PW, ph - 28);
    nucleo_touch_add(PX, py, PW, ph - 28, NK_ENTER, 0);   // tap the preview = use it / test it
    lg::font(0); d.setTextColor(MUTED, BG); d.setTextDatum(top_center);
    d.drawString(s_sel == 4 ? TR5("Tocca per provare", "Tap to test", "Toca para probar", "Touchez pour tester", "Tippen zum Testen") : TR5("Tocca di nuovo per usarlo", "Tap again to use it", "Toca otra vez para usarlo", "Touchez encore pour l'utiliser", "Nochmal tippen zum Nutzen"),
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
        d.drawString(TR5("In esecuzione", "Running", "En marcha", "En cours", "Laeuft"), 8, y + 10);
        d.setFont(&fonts::Font0); d.setTextColor(FG, BG); d.drawString(effect_name(g_mode), 8, y + 36);
        lg::comp_keys(TR5("un tasto per uscire", "a key to exit", "una tecla para salir", "une touche pour sortir", "Taste zum Beenden")); return;
    }
    if (s_tab == 0) {
        d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(s_sel == 4 ? C_YELLOW : (s_sel == g_mode ? C_GREEN : FG), BG);
        d.drawString(s_sel == 4 ? TR5("Prova", "Test", "Probar", "Tester", "Testen") : effect_name(s_sel), 8, y + 10);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG);
        const char *dsc = mode_right(s_sel, nullptr);
        if (dsc) d.drawString(dsc, 8, y + 34);
        char p[20]; snprintf(p, sizeof p, "%d / 5", s_sel + 1);
        d.setTextColor(DIM, BG); d.drawString(p, 8, y + 50);
        lg::comp_keys(TR5("su/giu  INVIO usa/prova  TAB timer", "up/dn  ENTER use/test  TAB timer", "arr/abj  INTRO usa/prueba  TAB tiempo", "haut/bas  ENTREE utilise/teste  TAB temps", "auf/ab  ENTER nutzen/testen  TAB Zeit"));
    } else {
        char sv[20]; fmt_timer(sv, sizeof sv);
        d.setFont(&fonts::FreeSansBold12pt7b); d.setTextColor(C_YELLOW, BG); d.drawString(sv, 8, y + 14);
        d.setFont(&fonts::Font0); d.setTextColor(MUTED, BG); d.drawString(TR5("attivazione automatica", "auto activation", "activacion automatica", "activation auto", "Autostart"), 8, y + 46);
        lg::comp_keys(TR5("</> 5s  su/giu 30s  TAB effetto", "</> 5s  up/dn 30s  TAB effect", "</> 5s  arr/abj 30s  TAB efecto", "</> 5s  haut/bas 30s  TAB effet", "</> 5s  auf/ab 30s  TAB Effekt"));
    }
}

// on_draw: for the saver, draws the full frame on the canvas (Technique 1).
// For settings, redraws only when s_dirty.
// Two screens, running: the Cardputer is the deck's status (battery, network, memory, uptime) whatever the effect,
// never a second copy of what the panel shows.
static void effect_draw_compact(void)
{
    if (g_mode == MODE_OFF) { d.fillRect(0, 0, W, H, 0x0000); return; }   // backlight 0
    status_frame();                                  // the deck's status: the panel already shows the effect / the time
}

static void on_draw(void)
{
    if (nucleo_app_two_screens()) { if (s_running) effect_draw_compact(); else draw_companion(); return; }
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

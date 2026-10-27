// Cronometro & Timer — the everyday "time" companion to Clock/Alarm the launcher was missing.
//
// One app, two modes toggled with TAB:
//   * CRONOMETRO (stopwatch): centi-second precision, up to 4 laps, start/pause/reset.
//   * TIMER (countdown): set MM:SS with the arrows or a 1-9 minute quick-preset, start/pause/reset;
//     a loud, blinking alarm fires at zero until any key acknowledges it.
//
// Style: matches Clock — big centred digits on the theme background, an app_ui_title header, the
// footer hint bar spells the live controls, all strings bilingual via TR(). Anti-flicker is the
// canvas+blit default (technique 1): a poll handler asks for a redraw only while something moves
// (~20 fps for the stopwatch centiseconds, slower for the countdown, a 4 Hz flash on finish), so an
// idle screen stops repainting. Monotonic esp_timer_get_time() drives both clocks; zero heap.
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_audio.h"     // procedural beeps for start/stop + the finish alarm
#include "nucleo_i18n.h"      // TR("it","en")
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "app_gfx.h"
#include "app_large.h"        // two screens: the instrument on the 320x240 panel, the Cardputer as companion
#include "nucleo_touch.h"

// The UI strings, one row per string in its languages: a call site loads an index, not five literals.
static const char *const S5[][5] = {
    { "Cronometro", "Stopwatch", "Cronometro", "Chrono", "Stoppuhr" },
    { "Giro", "Lap", "Vuelta", "Tour", "Runde" },
    { "PAUSA", "PAUSE", "PAUSA", "PAUSE", "PAUSE" },
    { "FINITO!", "TIME'S UP!", "TIEMPO!", "FINI!", "FERTIG!" },
    { "premi un tasto", "press any key", "pulsa cualquier", "appuyer touche", "Taste druecken" },
    { "1-9 = minuti", "1-9 = minutes", "1-9 = minutos", "1-9 = minutes", "1-9 = Minuten" },
    { "INVIO pausa | su=giro | TAB timer", "ENTER pause | up=lap | TAB timer", "INTRO pausa | ar=vuelta | TAB timer", "ENTREE pause | haut=tour | TAB tmr", "ENTER pause | oben=Runde | TAB Chr" },
    { "INVIO riprendi | R azzera | TAB timer", "ENTER resume | R reset | TAB timer", "INTRO reanuda | R reset | TAB timer", "ENTREE reprendre | R remet | TAB tmr", "ENTER weiter | R zurueck | TAB Chrono" },
    { "INVIO avvia | TAB timer", "ENTER start | TAB timer", "INTRO inicia | TAB timer", "ENTREE debut | TAB timer", "ENTER start | TAB Chrono" },
    { "INVIO pausa | R azzera | TAB crono", "ENTER pause | R reset | TAB chrono", "INTRO pausa | R reset | TAB crono", "ENTREE pause | R remet | TAB crono", "ENTER pause | R zurueck | TAB Chrono" },
    { "INVIO riprendi | R azzera", "ENTER resume | R reset", "INTRO reanuda | R reset", "ENTREE repr | R remet", "ENTER weiter | R zurueck" },
    { "frecce imposta | INVIO avvia | TAB crono", "arrows set | ENTER start | TAB chrono", "flechas cfg | INTRO inicia | TAB crn", "fleches fixer | ENTREE debut | TAB crn", "Pfeile stelle | ENTER start | TAB Cr" },
    { "Azzera", "Reset", "Reiniciar", "Remise", "Reset" },
    { "Avvia", "Start", "Iniciar", "Demarrer", "Start" },
};
static const char *s5(int i) { const char *const *v = S5[i]; return nucleo_tr5(v[0], v[1], v[2], v[3], v[4]); }

// ── modes ───────────────────────────────────────────────────────────────────
enum { MODE_STOP, MODE_TIMER };
static int s_mode = MODE_STOP;

// ── stopwatch state ─────────────────────────────────────────────────────────
static bool    s_sw_run;
static int64_t s_sw_start_us;    // moment the current run segment began
static int64_t s_sw_accum_us;    // elapsed banked across previous pause(s)
#define MAXLAP 3
static int     s_sw_lap[MAXLAP]; // lap elapsed-times (ms), newest first (fits above the hint bar)
static int     s_sw_nlap;

// ── timer state ─────────────────────────────────────────────────────────────
static int     s_tm_min = 5, s_tm_sec = 0;  // editable duration
static int     s_tm_field;                   // 0 = minutes, 1 = seconds (which one the arrows edit)
static bool    s_tm_run;
static bool    s_tm_done;                     // reached zero — alarm blinking until acknowledged
static int64_t s_tm_end_us;                   // absolute deadline while running
static int     s_tm_rem_ms;                   // remaining time banked at pause (0 = not paused → use the set value)
static int64_t s_tm_alarm_us;                 // last finish-beep timestamp (re-beep ~1 Hz while unacknowledged)

// ── frame pacing (poll handler) ─────────────────────────────────────────────
static int64_t s_last_frame_us;

#define CFG "/sd/system/config/chrono.json"

// ── helpers ──────────────────────────────────────────────────────────────────
static int sw_elapsed_ms(void)
{
    int64_t e = s_sw_accum_us;
    if (s_sw_run) e += esp_timer_get_time() - s_sw_start_us;
    return (int)(e / 1000);
}
static int tm_set_ms(void)  { return (s_tm_min * 60 + s_tm_sec) * 1000; }
static int tm_left_ms(void)
{
    if (s_tm_run) { int64_t l = s_tm_end_us - esp_timer_get_time(); return l > 0 ? (int)(l / 1000) : 0; }
    if (s_tm_rem_ms > 0) return s_tm_rem_ms;   // paused
    return tm_set_ms();                        // idle → shows the configured duration
}

// MM:SS.cc (centiseconds) for the stopwatch, minutes capped at 99.
static void fmt_swatch(int ms, char *out, int n)
{
    int cs = (ms / 10) % 100, s = (ms / 1000) % 60, m = ms / 60000;
    if (m > 99) m = 99;
    snprintf(out, n, "%02d:%02d.%02d", m, s, cs);
}
// MM:SS with ceiling, so a running countdown shows 00:01 for the whole final second, not 00:00.
static void fmt_mmss(int ms, char *out, int n)
{
    if (ms < 0) ms = 0;
    int total = (ms + 999) / 1000, s = total % 60, m = total / 60;
    if (m > 99) m = 99;
    snprintf(out, n, "%02d:%02d", m, s);
}

// A size-N string centred in the full width, drawn on the theme background (no smear).
static void draw_centered(const char *str, int size, int y, unsigned short col)
{
    LovyanGFX &G = d;
    int wpx = (int)strlen(str) * 6 * size;
    G.setTextSize(size);
    G.setTextColor(col, BG);
    G.setCursor((W - wpx) / 2, y);
    G.print(str);
}

// ── persistence (remember the last timer + mode) ─────────────────────────────
static void load_cfg(void)
{
    FILE *f = fopen(CFG, "rb"); if (!f) return;
    char buf[96]; int n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
    if (n <= 0) return;
    buf[n] = 0;
    int mode = 0, mm = 5, ss = 0;
    sscanf(buf, "{\"mode\":%d,\"min\":%d,\"sec\":%d", &mode, &mm, &ss);
    s_mode = (mode == MODE_TIMER) ? MODE_TIMER : MODE_STOP;
    s_tm_min = (mm < 0 || mm > 99) ? 5 : mm;
    s_tm_sec = (ss < 0 || ss > 59) ? 0 : ss;
}
static void save_cfg(void)
{
    mkdir("/sd/system", 0775); mkdir("/sd/system/config", 0775);
    FILE *f = fopen(CFG, "wb"); if (!f) return;
    fprintf(f, "{\"mode\":%d,\"min\":%d,\"sec\":%d}\n", s_mode, s_tm_min, s_tm_sec);
    fclose(f);
}

// ── stopwatch actions ────────────────────────────────────────────────────────
static void sw_toggle(void)
{
    if (s_sw_run) { s_sw_accum_us += esp_timer_get_time() - s_sw_start_us; s_sw_run = false; nucleo_audio_tone(900, 45, 60); }
    else          { s_sw_start_us = esp_timer_get_time(); s_sw_run = true;  nucleo_audio_tone(1500, 45, 70); }
    nucleo_app_request_draw();
}
static void sw_reset(void)
{
    s_sw_run = false; s_sw_accum_us = 0; s_sw_start_us = 0; s_sw_nlap = 0;
    nucleo_audio_tone(700, 60, 55); nucleo_app_request_draw();
}
static void sw_lap(void)
{
    if (!s_sw_run) return;
    for (int i = MAXLAP - 1; i > 0; i--) s_sw_lap[i] = s_sw_lap[i - 1];   // shift down, newest at [0]
    s_sw_lap[0] = sw_elapsed_ms();
    if (s_sw_nlap < MAXLAP) s_sw_nlap++;
    nucleo_audio_tone(2000, 35, 65); nucleo_app_request_draw();
}

// ── timer actions ─────────────────────────────────────────────────────────────
static void tm_start(void)
{
    int rem = (s_tm_rem_ms > 0) ? s_tm_rem_ms : tm_set_ms();
    if (rem <= 0) { nucleo_audio_tone(600, 60, 55); return; }   // nothing set — reject with a low blip
    s_tm_end_us = esp_timer_get_time() + (int64_t)rem * 1000;
    s_tm_run = true; s_tm_done = false; s_tm_rem_ms = 0;
    nucleo_audio_tone(1500, 45, 70); nucleo_app_request_draw();
}
static void tm_pause(void)
{
    s_tm_rem_ms = tm_left_ms(); s_tm_run = false;
    nucleo_audio_tone(900, 45, 60); nucleo_app_request_draw();
}
static void tm_reset(void)
{
    s_tm_run = false; s_tm_done = false; s_tm_rem_ms = 0;
    nucleo_audio_tone(700, 60, 55); nucleo_app_request_draw();
}
static void tm_ack(void)   // acknowledge a firing alarm
{
    s_tm_done = false; s_tm_rem_ms = 0; nucleo_app_request_draw();
}
static void tm_bump(int field, int delta)
{
    if (field == 0) { s_tm_min += delta; if (s_tm_min < 0) s_tm_min = 99; if (s_tm_min > 99) s_tm_min = 0; }
    else            { s_tm_sec += delta; if (s_tm_sec < 0) s_tm_sec = 59; if (s_tm_sec > 59) s_tm_sec = 0; }
    s_tm_rem_ms = 0;   // editing invalidates any paused remainder
    nucleo_app_request_draw();
}

// ── drawing ───────────────────────────────────────────────────────────────────
static void draw_stopwatch(int top, int h)
{
    LovyanGFX &G = d;
    int ms = sw_elapsed_ms();
    const char *right = s_sw_run ? "REC" : (ms ? "II" : "");
    int y0 = app_ui_title(s5(0 /* Stopwatch */), s_sw_run ? C_GREEN : C_BLUE, right);

    char big[16]; fmt_swatch(ms, big, sizeof(big));
    int laps_h = s_sw_nlap ? (s_sw_nlap * 12 + 4) : 0;
    int digit_y = y0 + (top + h - y0 - laps_h - 24) / 2;   // centre the digits in the space above the laps
    draw_centered(big, 4, digit_y, s_sw_run ? 0xFFFF : (ms ? C_BLUE : MUTED));

    // laps: newest first, size 1, dimming with age
    int ly = digit_y + 40;
    for (int i = 0; i < s_sw_nlap; i++) {
        char row[28], t[16]; fmt_swatch(s_sw_lap[i], t, sizeof(t));
        snprintf(row, sizeof(row), "%s %d  %s", s5(1 /* Lap */), s_sw_nlap - i, t);
        G.setTextSize(1); G.setTextColor(i == 0 ? FG : MUTED, BG);
        G.setCursor((W - (int)strlen(row) * 6) / 2, ly); G.print(row);
        ly += 12;
    }
}

static void draw_timer(int top, int h, bool flash_on)
{
    LovyanGFX &G = d;
    int left = tm_left_ms();
    bool low = s_tm_run && left <= 10000;
    unsigned short accent = s_tm_done ? C_RED : (s_tm_run ? (low ? C_RED : C_YELLOW) : C_BLUE);
    const char *right = s_tm_done ? "!!!" : (s_tm_run ? "II" : (s_tm_rem_ms > 0 ? s5(2 /* PAUSE */) : ""));
    int y0 = app_ui_title("Timer", accent, right);
    int cy = y0 + (top + h - y0) / 2 - 18;

    if (s_tm_done) {
        // Blinking full-content alarm banner — the text background must track the flashing fill.
        unsigned short bg = flash_on ? C_RED : BG, fg = flash_on ? BG : C_RED;
        if (flash_on) G.fillRect(0, y0 + 2, W, top + h - y0 - 2, C_RED);
        const char *big = s5(3 /* TIME'S UP! */);
        G.setTextSize(3); G.setTextColor(fg, bg);
        G.setCursor((W - (int)strlen(big) * 18) / 2, cy - 8); G.print(big);
        const char *m = s5(4 /* press any key */);
        G.setTextSize(1); G.setTextColor(flash_on ? BG : MUTED, bg);
        G.setCursor((W - (int)strlen(m) * 6) / 2, cy + 30); G.print(m);
        return;
    }

    if (s_tm_run || s_tm_rem_ms > 0) {
        // Running / paused: one big MM:SS + a remaining-fraction bar.
        char big[8]; fmt_mmss(left, big, sizeof(big));
        draw_centered(big, 6, cy - 24, low ? C_RED : accent);
        int total = tm_set_ms(); if (total < 1) total = 1;
        int bw = W - 40, filled = (int)((int64_t)bw * left / total);
        G.fillRoundRect(20, cy + 30, bw, 6, 3, INK);
        G.fillRoundRect(20, cy + 30, filled, 6, 3, low ? C_RED : accent);
    } else {
        // Idle editor: MM and SS drawn apart so the active field can glow.
        char mm[4], ss[4]; snprintf(mm, sizeof(mm), "%02d", s_tm_min); snprintf(ss, sizeof(ss), "%02d", s_tm_sec);
        int size = 6, cw = 6 * size, total_w = 5 * cw, x = (W - total_w) / 2, ty = cy - 24;
        G.setTextSize(size);
        G.setTextColor(s_tm_field == 0 ? C_BLUE : FG, BG); G.setCursor(x, ty);          G.print(mm);
        G.setTextColor(MUTED, BG);                          G.setCursor(x + 2 * cw, ty); G.print(":");
        G.setTextColor(s_tm_field == 1 ? C_BLUE : FG, BG); G.setCursor(x + 3 * cw, ty); G.print(ss);
        // underline the active field
        int ux = (s_tm_field == 0) ? x : x + 3 * cw;
        G.fillRoundRect(ux, ty + 6 * size + 2, 2 * cw - 4, 3, 1, C_BLUE);
        G.setTextSize(1); G.setTextColor(MUTED, BG);
        const char *m = s5(5 /* 1-9 = minutes */);
        G.setCursor((W - (int)strlen(m) * 6) / 2, ty + 6 * size + 10); G.print(m);
    }
}

static const char *hint_text(void)
{
    const char *hint;
    if (s_mode == MODE_STOP) {
        if (s_sw_run)               hint = s5(6 /* ENTER pause | up=lap | TAB timer */);
        else if (sw_elapsed_ms())   hint = s5(7 /* ENTER resume | R reset | TAB timer */);
        else                        hint = s5(8 /* ENTER start | TAB timer */);
    } else {
        if (s_tm_done)              hint = s5(4 /* press any key */);
        else if (s_tm_run)          hint = s5(9 /* ENTER pause | R reset | TAB chrono */);
        else if (s_tm_rem_ms > 0)   hint = s5(10 /* ENTER resume | R reset */);
        else                        hint = s5(11 /* arrows set | ENTER start | TAB chrono */);
    }
    return hint;
}

// blink phase for the finish alarm: on for 250 ms, off for 250 ms
static bool flash_phase(void) { return ((esp_timer_get_time() / 250000) & 1) == 0; }

// ===== two screens ==============================================================================================
// The 320x240 panel is the instrument itself, composed in strips (lg::frame: the digits tick without flicker):
// the kit's header (chevron = back) and the two modes as lg::tabs pills (tap the other one to switch), then
//   STOPWATCH: the time in 7-segment digits, a big start/stop disc, a lap / reset disc, the laps column;
//   TIMER idle: MM:SS you tap (pick the field) and drag up/down (set it), preset chips, a Start pill;
//   TIMER running / paused: a ring that empties with the time left (tap it = pause / resume) + a Reset chip;
//   TIMER finished: the whole panel flashes, a tap silences it.
// The Cardputer shows the time and the keys. Keyboard behaviour is unchanged.
#define LW   320
#define LH   240
#define LTOP 62
static const unsigned char PRESET_MIN[6] = { 1, 3, 5, 10, 15, 25 };

static void lt(const char *s, int x, int y, int f, unsigned short c, uint8_t datum = top_center)
{
    LovyanGFX &G = d;
    lg::font(f); G.setTextDatum(datum); G.setTextColor(c); G.drawString(s, x, y); G.setTextDatum(top_left);
}
static void l7(const char *s, int x, int y, float sz, unsigned short c)   // 7-segment digits, 48 px at size 1
{
    LovyanGFX &G = d;
    G.setFont(&fonts::Font7); G.setTextSize(sz); G.setTextDatum(top_center); G.setTextColor(c);
    G.drawString(s, x, y); G.setTextSize(1); G.setTextDatum(top_left);
}
// A round control: fill, a play / pause / no glyph, an optional label under it; a tap = (key, ch).
static void ldisc(int cx, int cy, int r, unsigned short col, int glyph, const char *label, int key, char ch)
{
    LovyanGFX &G = d;
    G.fillCircle(cx, cy, r, col);
    const int s = r / 3, gy = label ? cy - 8 : cy;
    if (glyph == 0) G.fillTriangle(cx - s * 2 / 3, gy - s, cx - s * 2 / 3, gy + s, cx + s, gy, INK);
    else if (glyph == 1) { G.fillRect(cx - s * 3 / 4, gy - s, s / 2 + 1, 2 * s, INK); G.fillRect(cx + s / 4, gy - s, s / 2 + 1, 2 * s, INK); }
    if (label) lt(label, cx, glyph == 2 ? cy - 8 : cy + s - 2, 1, INK);
    if (key) nucleo_touch_add(cx - r, cy - r, 2 * r, 2 * r, key, ch);
}

static void lp_band(unsigned short acc)
{
    lg::header(s5(0 /* Stopwatch */), nullptr, acc);
    const char *nm[2] = { s5(0 /* Stopwatch */), "Timer" };
    static const char keys[2] = { 1, 2 };                            // a pill taps as NK_CHAR 1 / 2 (on_key)
    lg::tabs(lg::HEAD, nm, 2, s_mode, acc, keys);
}

static void lp_stopwatch(void)
{
    const int ms = sw_elapsed_ms();
    char t[16]; fmt_swatch(ms, t, sizeof t);
    l7(t, LW / 2, LTOP + 2, 1, s_sw_run ? FG : (ms ? C_BLUE : MUTED));
    ldisc(70, 178, 50, s_sw_run ? C_RED : C_GREEN, s_sw_run ? 1 : 0, nullptr, NK_ENTER, 0);
    if (s_sw_run)  ldisc(160, 196, 32, C_BLUE, 2, s5(1 /* Lap */), NK_UP, 0);
    else if (ms)   ldisc(160, 196, 32, MUTED, 2, s5(12 /* Reset */), NK_CHAR, 'r');
    for (int i = 0; i < MAXLAP; i++) {                              // the laps column, newest on top
        const int y = 120 + i * 38;
        char lb[16], v[16];
        snprintf(lb, sizeof lb, "%s %d", s5(1 /* Lap */), s_sw_nlap - i);
        if (i < s_sw_nlap) fmt_swatch(s_sw_lap[i], v, sizeof v); else snprintf(v, sizeof v, "--:--.--");
        if (i < s_sw_nlap) lt(lb, 208, y, 0, MUTED, top_left);
        lt(v, 208, y + 16, 1, i < s_sw_nlap ? (i ? FG : C_BLUE) : DIM, top_left);
    }
}

static void lp_timer(void)
{
    LovyanGFX &G = d;
    if (s_tm_done) {
        const bool on = flash_phase();
        G.fillRect(0, LTOP, LW, LH - LTOP, on ? C_RED : BG);
        lt(s5(3 /* TIME'S UP! */), LW / 2, 120, 3, on ? INK : C_RED);
        lt(s5(4 /* press any key */), LW / 2, 170, 0, on ? INK : MUTED);
        nucleo_touch_add(0, LTOP, LW, LH - LTOP, NK_ENTER, 0);
        return;
    }
    const int left = tm_left_ms();
    char t[8]; fmt_mmss(left, t, sizeof t);
    if (s_tm_run || s_tm_rem_ms > 0) {
        const bool low = s_tm_run && left <= 10000;
        const unsigned short col = low ? C_RED : (s_tm_run ? C_YELLOW : C_BLUE);
        const int cx = 118, cy = 152, total = tm_set_ms() > 0 ? tm_set_ms() : 1;
        G.fillArc(cx, cy, 66, 80, 0, 360, LINE);
        const int deg = (int)((int64_t)360 * left / total);
        if (deg > 0) G.fillArc(cx, cy, 66, 80, 270, 270 + deg, col);
        lt(t, cx, cy - 18, 3, low ? C_RED : FG);
        if (!s_tm_run) lt(s5(2 /* PAUSE */), cx, cy + 20, 1, C_BLUE);
        nucleo_touch_add(cx - 80, cy - 80, 160, 160, NK_ENTER, 0);
        ldisc(266, 196, 34, MUTED, 2, s5(12 /* Reset */), NK_CHAR, 'r');
        return;
    }
    // idle editor: the digits are the control (tap = pick MM or SS, drag up / down = set it)
    char mm[4], ss[4]; snprintf(mm, sizeof mm, "%02d", s_tm_min); snprintf(ss, sizeof ss, "%02d", s_tm_sec);
    l7(mm, 92, LTOP + 2, 1.5f, s_tm_field == 0 ? C_BLUE : FG);
    l7(":", LW / 2, LTOP + 2, 1.5f, MUTED);
    l7(ss, 228, LTOP + 2, 1.5f, s_tm_field == 1 ? C_BLUE : FG);
    G.fillRoundRect(s_tm_field == 0 ? 50 : 186, LTOP + 76, 84, 4, 2, C_BLUE);
    nucleo_touch_add(20, LTOP, 130, 82, NK_LEFT, 0);
    nucleo_touch_add(170, LTOP, 130, 82, NK_RIGHT, 0);
    nucleo_touch_add_pad(0, LTOP, LW, 82, 14);
    for (int i = 0; i < 6; i++) {                                   // preset chips: the minutes at one tap
        const int x = 8 + i * 51; char p[6]; snprintf(p, sizeof p, "%d'", PRESET_MIN[i]);
        const bool on = s_tm_min == PRESET_MIN[i] && !s_tm_sec;
        G.fillRoundRect(x, 146, 47, 30, 9, on ? C_BLUE : LINE);
        lt(p, x + 23, 161, 1, on ? INK : FG, middle_center);
        nucleo_touch_add(x, 144, 51, 36, NK_CHAR, (char)(NUCLEO_TOUCH_ROW + i));
    }
    G.fillRoundRect(70, 186, 180, 44, 14, C_GREEN);
    lt(s5(13 /* Start */), LW / 2, 208, 1, INK, middle_center);
    nucleo_touch_add(60, 182, 200, 58, NK_ENTER, 0);
}

static void lg_paint(void *)
{
    d.fillRect(0, 0, LW, LH, BG);
    const unsigned short acc = s_mode == MODE_STOP ? (s_sw_run ? C_GREEN : C_BLUE) : (s_tm_done ? C_RED : C_YELLOW);
    if (s_mode == MODE_STOP) lp_stopwatch(); else lp_timer();
    lp_band(acc);
    lg::field(0, 0, LW, LH);                                         // a stray tap is nothing (not Enter / an arrow)
}
static void draw_large(void)
{
    nucleo_touch_clear();
    lg::frame(lg_paint, nullptr, 0, 0, true);
    nucleo_touch_commit();
}

// The Cardputer meanwhile: the running time big, the keys underneath.
static void draw_companion(void)
{
    int y = lg::comp_head(s_mode == MODE_STOP ? s5(0 /* Stopwatch */) : "Timer",
                          s_mode == MODE_STOP ? C_BLUE : C_YELLOW);
    char t[16];
    if (s_mode == MODE_STOP) fmt_swatch(sw_elapsed_ms(), t, sizeof t); else fmt_mmss(tm_left_ms(), t, sizeof t);
    draw_centered(t, 4, y + 22, s_tm_done && s_mode == MODE_TIMER ? C_RED : FG);
    lg::comp_keys(hint_text());
}

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height();
    d.fillRect(0, top, W, h, BG);
    if (s_mode == MODE_STOP) draw_stopwatch(top, h);
    else                     draw_timer(top, h, flash_phase());
    nucleo_app_set_hint(hint_text());
}

// ── input ─────────────────────────────────────────────────────────────────────
static void on_tab(void);
static void on_key(int key, char ch)
{
    if (nucleo_touch_is_row(key, ch)) {                              // a preset chip on the panel (idle timer)
        int i = ch - NUCLEO_TOUCH_ROW;
        if (s_mode == MODE_TIMER && !s_tm_run && !s_tm_done && i < 6) { s_tm_min = PRESET_MIN[i]; s_tm_sec = 0; s_tm_rem_ms = 0; nucleo_app_request_draw(); }
        return;
    }
    if (key == NK_CHAR && (ch == 1 || ch == 2)) { if (ch - 1 != s_mode) on_tab(); return; }   // a mode pill on the panel
    if (ch == 'r' || ch == 'R') { if (s_mode == MODE_STOP) sw_reset(); else tm_reset(); return; }

    if (s_mode == MODE_STOP) {
        if (key == NK_ENTER || ch == ' ') { sw_toggle(); return; }
        if (key == NK_UP)                 { sw_lap();    return; }   // lap while running
        return;
    }

    // TIMER
    if (s_tm_done) { tm_ack(); return; }                            // any key silences the alarm
    if (key == NK_ENTER || ch == ' ') { if (s_tm_run) tm_pause(); else tm_start(); return; }
    if (s_tm_run) return;                                           // running: only ENTER/R do anything
    if (ch >= '1' && ch <= '9') { s_tm_min = ch - '0'; s_tm_sec = 0; s_tm_rem_ms = 0; nucleo_app_request_draw(); return; }
    if (ch == '0')              { s_tm_min = 0; s_tm_sec = 0; s_tm_rem_ms = 0; nucleo_app_request_draw(); return; }
    if (key == NK_RIGHT)        { s_tm_field = 1; nucleo_app_request_draw(); return; }   // select seconds (LEFT→min is in back())
    if (key == NK_UP)           { tm_bump(s_tm_field, +1); return; }
    if (key == NK_DOWN)         { tm_bump(s_tm_field, -1); return; }
}

// LEFT + BACK route here. LEFT edits/navigates and is swallowed; BACK exits (unless it must ack an alarm).
static bool on_back(int key)
{
    if (key == NK_LEFT) {
        if (s_mode == MODE_TIMER && !s_tm_run && !s_tm_done) { s_tm_field = 0; nucleo_app_request_draw(); }
        return true;   // never let a stray LEFT close the app
    }
    if (s_tm_done) { tm_ack(); return true; }   // BACK acknowledges the alarm first
    return false;                                // real exit
}

static void on_tab(void)
{
    s_mode = (s_mode == MODE_STOP) ? MODE_TIMER : MODE_STOP;
    if (s_tm_done) tm_ack();
    nucleo_audio_tone(1200, 30, 55);
    nucleo_app_request_draw();
}

// Redraw pacing: 20 fps for the stopwatch, 8 fps for a running countdown, 4 Hz for the finish flash,
// and nothing while idle (request_draw handles key-driven changes).
static bool poll(void)
{
    // fire the alarm the instant a running countdown crosses zero
    if (s_tm_run && esp_timer_get_time() >= s_tm_end_us) {
        s_tm_run = false; s_tm_done = true; s_tm_rem_ms = 0; s_tm_alarm_us = 0;
    }
    int interval_us;
    if (s_tm_done)                             interval_us = 250000;   // 4 Hz blink
    else if (s_mode == MODE_STOP && s_sw_run)  interval_us = nucleo_app_two_screens() ? 100000 : 50000;   // centiseconds (10 fps on the panel)
    else if (s_mode == MODE_TIMER && s_tm_run) interval_us = 125000;   // 8 fps countdown
    else return false;                                                  // idle: no periodic redraw

    // keep the alarm audible: one short beep per second until acknowledged
    if (s_tm_done) {
        int64_t now = esp_timer_get_time();
        if (now - s_tm_alarm_us >= 1000000) { s_tm_alarm_us = now; nucleo_audio_tone(2300, 110, 90); }
    }

    int64_t now = esp_timer_get_time();
    if (now - s_last_frame_us < interval_us) return false;
    s_last_frame_us = now;
    return true;
}

static void enter(void)
{
    load_cfg();
    s_sw_run = false; s_sw_accum_us = 0; s_sw_start_us = 0; s_sw_nlap = 0;
    s_tm_run = false; s_tm_done = false; s_tm_rem_ms = 0; s_tm_field = 0; s_last_frame_us = 0;
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    save_cfg();
    s_sw_run = false; s_tm_run = false;   // stop the clocks; poll returns false so redraws cease
}

extern "C" void nucleo_register_chrono(void)
{
    static const nucleo_app_def_t app = {
        "chrono", "Cronometro", "Office", "Cronometro e timer da tasca",
        'T', C_BLUE, enter, on_key, nullptr, draw, on_exit, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}

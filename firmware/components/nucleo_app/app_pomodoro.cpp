// Pomodoro — Francesco Cirillo's focus technique as a first-class device app.
//
// A guided work/break cycle, not just a countdown: FOCUS (25') → SHORT BREAK (5') repeated, and every
// `long_every` focus sessions a LONG BREAK (15-30'). The app advances the phases itself (optionally
// auto-starting the next), counts completed pomodoros, and keeps a daily tally + streak so the device
// becomes a distraction-free desk timer that a phone can't be.
//
// Three tabs (TAB cycles): TIMER (the running phase — big digits + progress + the "pomodoro dots", and
// a calm breathing pacer during breaks), STATS (a ring of today's pomodoros vs the daily goal + totals
// + streak), SETUP (durations, cadence, goal, auto-start, sound, breathing guide).
//
// Audio: gentle *earcons* — short arpeggios built from musical notes at moderate strength, never the
// harsh single square-beep. Distinct motifs announce focus vs break vs long break, reward a completed
// pomodoro, and celebrate the daily goal; the final three seconds tick softly. All gated by the Sound
// setting and the OS volume.
//
// Style: native UI kit — app_ui_tabs / app_ui_list, per-phase accent, every string bilingual via TR().
// Anti-flicker is the canvas+blit default: a poll handler asks for a redraw only while a phase runs or
// a banner blinks (breaths animate a touch faster), so an idle screen never repaints. Monotonic
// esp_timer drives the clock; wall-clock time() only rolls the daily stats. Zero heap — all state .bss.
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_audio.h"     // procedural tones we compose earcons from
#include "nucleo_i18n.h"      // TR("it","en")
#include "nucleo_notify.h"    // phase-change notifications -> web Notification Center + on-screen banner
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "app_gfx.h"

// Tomato identity (also the registered launcher accent). Focus is red, breaks cool.
static const unsigned short TOMATO = C_RED;

// ── tabs ──────────────────────────────────────────────────────────────────────
enum { TAB_TIMER, TAB_STATS, TAB_SETUP, NTAB };
static int s_tab = TAB_TIMER;

// ── phases ────────────────────────────────────────────────────────────────────
enum { PH_FOCUS, PH_SHORT, PH_LONG };
static int s_phase = PH_FOCUS;

// ── settings (persisted) ──────────────────────────────────────────────────────
static int  s_focus_min  = 25;
static int  s_short_min  = 5;
static int  s_long_min   = 15;
static int  s_long_every = 4;    // long break after this many focus sessions
static int  s_goal       = 8;    // daily pomodoro target (STATS ring)
static bool s_autostart  = true; // roll straight into the next phase
static bool s_sound      = true; // earcons
static bool s_breath     = true; // breathing pacer during breaks

// ── run state ─────────────────────────────────────────────────────────────────
static bool    s_run;            // a phase is counting down
static bool    s_done;           // a phase hit zero and is waiting for acknowledgement (autostart off)
static int     s_pending;        // phase to enter once the banner is acknowledged
static int64_t s_end_us;         // absolute deadline while running
static int     s_rem_ms;         // remaining banked at pause (0 = not paused)
static int     s_completed;      // focus sessions done in the current set (0..s_long_every) -> dots
static int64_t s_alarm_us;       // last finish reminder
static int64_t s_flash_until;    // brief celebratory / new-phase flash
static int     s_tick_sec;       // last second we played a final-countdown tick for
static int64_t s_last_frame_us;  // poll pacing

// ── daily stats (persisted) ───────────────────────────────────────────────────
static int s_stat_day;           // day-index (time()/86400) the today-counters belong to; 0 = unknown
static int s_last_active_day;    // last day-index a pomodoro completed (for the streak)
static int s_today_pomos;        // pomodoros completed today
static int s_today_focus_min;    // focus minutes accumulated today
static int s_total_pomos;        // lifetime pomodoros
static int s_streak;             // consecutive days with >=1 pomodoro

// ── setup selection ───────────────────────────────────────────────────────────
enum { CF_FOCUS, CF_SHORT, CF_LONG, CF_EVERY, CF_GOAL, CF_AUTO, CF_SOUND, CF_BREATH, CF_N };
static int s_cf_sel;

#define CFG "/sd/system/config/pomodoro.json"

// ── earcons ───────────────────────────────────────────────────────────────────
// Notes (equal-tempered Hz). Mid-range + moderate strength = present but never piercing.
enum { N_A4 = 440, N_C5 = 523, N_D5 = 587, N_E5 = 659, N_F5 = 698, N_G5 = 784,
       N_A5 = 880, N_C6 = 1047, N_E6 = 1319, N_G6 = 1568 };
static void note(int f, int ms, int strg) { if (s_sound) nucleo_audio_tone(f, ms, strg); }

static void chime_enter(int ph)          // announce the phase we just entered
{
    if      (ph == PH_FOCUS) { note(N_E5, 55, 52); note(N_G5, 55, 52); note(N_C6, 95, 54); }   // rising, hopeful
    else if (ph == PH_SHORT) { note(N_C6, 55, 46); note(N_A5, 55, 46); note(N_F5, 95, 46); }   // descending, relax
    else                     { note(N_C5, 90, 44); note(N_E5, 90, 44); note(N_G5, 150, 44); }  // long: slow + warm
}
static void chime_focus_done(void) { note(N_C5, 55, 56); note(N_E5, 55, 56); note(N_G5, 55, 56); note(N_C6, 120, 58); } // reward
static void chime_break_done(void) { note(N_G5, 70, 54); note(N_C6, 110, 54); }                                        // back to work
static void chime_go(void)         { note(N_G5, 45, 50); note(N_C6, 70, 52); }
static void chime_resume(void)     { note(N_E5, 45, 48); note(N_A5, 70, 50); }
static void chime_pause(void)      { note(N_A5, 45, 44); note(N_E5, 70, 42); }
static void chime_reset(void)      { note(N_A4, 80, 40); }
static void chime_tap(void)        { note(N_A5, 16, 30); }                                       // subtle UI tick
static void chime_tick(void)       { note(N_E5, 18, 26); }                                       // final-seconds pulse
static void chime_remind(void)     { note(N_F5, 55, 42); note(N_A5, 80, 44); }                   // gentle "still waiting"
static void chime_goal(void)       { note(N_C6, 65, 58); note(N_E6, 65, 58); note(N_G6, 65, 58); note(N_C6, 55, 56); note(N_G6, 140, 60); }
static void chime_reject(void)     { note(330, 60, 38); }

// ── helpers ───────────────────────────────────────────────────────────────────
static unsigned short phase_col(int ph) { return ph == PH_FOCUS ? TOMATO : ph == PH_SHORT ? C_GREEN : C_BLUE; }
static const char    *phase_name(int ph) { return ph == PH_FOCUS ? TR("FOCUS", "FOCUS")
                                                : ph == PH_SHORT ? TR("PAUSA", "BREAK")
                                                                 : TR("PAUSA LUNGA", "LONG BREAK"); }
static int phase_min(int ph) { return ph == PH_FOCUS ? s_focus_min : ph == PH_SHORT ? s_short_min : s_long_min; }
static int phase_set_ms(void) { return phase_min(s_phase) * 60000; }

static int tm_left_ms(void)
{
    if (s_run)        { int64_t l = s_end_us - esp_timer_get_time(); return l > 0 ? (int)(l / 1000) : 0; }
    if (s_rem_ms > 0) return s_rem_ms;       // paused
    return phase_set_ms();                    // idle -> full phase
}
static int today_index(void) { time_t t = time(NULL); return (t > 1600000000) ? (int)(t / 86400) : 0; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// MM:SS with a ceiling, so a running countdown shows 00:01 for the whole final second.
static void fmt_mmss(int ms, char *out, int n)
{
    if (ms < 0) ms = 0;
    int total = (ms + 999) / 1000, s = total % 60, m = total / 60;
    if (m > 99) m = 99;
    snprintf(out, n, "%02d:%02d", m, s);
}
static void draw_centered(const char *str, int size, int y, unsigned short col)
{
    int wpx = (int)strlen(str) * 6 * size;
    d.setTextSize(size); d.setTextColor(col, BG);
    d.setCursor((W - wpx) / 2, y); d.print(str);
}

// ── persistence (tiny order-independent JSON int reader) ──────────────────────
static int jget(const char *buf, const char *key, int def)
{
    char pat[24]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return def;
    p = strchr(p, ':'); if (!p) return def;
    p++; while (*p == ' ') p++;
    int neg = (*p == '-'); if (neg) p++;
    if (*p < '0' || *p > '9') return def;
    int v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    return neg ? -v : v;
}
static void load_cfg(void)
{
    FILE *f = fopen(CFG, "rb"); if (!f) return;
    char buf[256]; int n = (int)fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
    if (n <= 0) return;
    buf[n] = 0;
    s_focus_min       = clampi(jget(buf, "focus", 25), 1, 90);
    s_short_min       = clampi(jget(buf, "short", 5),  1, 30);
    s_long_min        = clampi(jget(buf, "long", 15),  5, 60);
    s_long_every      = clampi(jget(buf, "every", 4),  2, 8);
    s_goal            = clampi(jget(buf, "goal", 8),   1, 24);
    s_autostart       = jget(buf, "auto", 1) != 0;
    s_sound           = jget(buf, "snd", 1) != 0;
    s_breath          = jget(buf, "brt", 1) != 0;
    s_stat_day        = jget(buf, "day", 0);
    s_last_active_day = jget(buf, "lad", 0);
    s_today_pomos     = clampi(jget(buf, "tp", 0), 0, 999);
    s_today_focus_min = clampi(jget(buf, "tf", 0), 0, 100000);
    s_total_pomos     = clampi(jget(buf, "tot", 0), 0, 100000000);
    s_streak          = clampi(jget(buf, "stk", 0), 0, 100000);
    s_phase           = clampi(jget(buf, "ph", PH_FOCUS), PH_FOCUS, PH_LONG);
    s_completed       = clampi(jget(buf, "cmp", 0), 0, s_long_every);
}
static void save_cfg(void)
{
    mkdir("/sd/system", 0775); mkdir("/sd/system/config", 0775);
    FILE *f = fopen(CFG, "wb"); if (!f) return;
    fprintf(f, "{\"focus\":%d,\"short\":%d,\"long\":%d,\"every\":%d,\"goal\":%d,\"auto\":%d,\"snd\":%d,\"brt\":%d,"
               "\"day\":%d,\"lad\":%d,\"tp\":%d,\"tf\":%d,\"tot\":%d,\"stk\":%d,\"ph\":%d,\"cmp\":%d}\n",
            s_focus_min, s_short_min, s_long_min, s_long_every, s_goal, s_autostart ? 1 : 0, s_sound ? 1 : 0,
            s_breath ? 1 : 0, s_stat_day, s_last_active_day, s_today_pomos, s_today_focus_min, s_total_pomos,
            s_streak, s_phase, s_completed);
    fclose(f);
}

// Reset the today-counters when the calendar day rolls over (only when the clock is actually set).
static void roll_day(void)
{
    int dy = today_index();
    if (dy == 0) return;                                  // clock not synced — leave stats untouched
    if (s_stat_day == 0) { s_stat_day = dy; return; }     // first known day
    if (dy == s_stat_day) return;                         // same day
    s_stat_day = dy; s_today_pomos = 0; s_today_focus_min = 0;
    if (s_last_active_day && dy > s_last_active_day + 1) s_streak = 0;  // a full day missed breaks the streak
}

// ── timer actions ─────────────────────────────────────────────────────────────
static void tm_begin(void)   // start/resume the countdown; the caller owns the earcon
{
    int rem = (s_rem_ms > 0) ? s_rem_ms : phase_set_ms();
    if (rem <= 0) { chime_reject(); return; }
    s_end_us = esp_timer_get_time() + (int64_t)rem * 1000;
    s_run = true; s_done = false; s_rem_ms = 0; s_tick_sec = 0;
    nucleo_app_request_draw();
}
static void tm_pause(void) { s_rem_ms = tm_left_ms(); s_run = false; chime_pause(); nucleo_app_request_draw(); }
static void tm_reset(void) { s_run = false; s_done = false; s_rem_ms = 0; chime_reset(); nucleo_app_request_draw(); }

// Enter a phase from the top, announce it, and (optionally) start it counting.
static void enter_phase(int ph, bool start)
{
    s_phase = ph; s_rem_ms = 0; s_done = false;
    chime_enter(ph);
    if (start) tm_begin(); else nucleo_app_request_draw();
}

// A running phase reached zero: bank stats, pick the next phase, notify, chime, and advance.
static void complete_phase(void)
{
    s_run = false; s_rem_ms = 0;
    int finished = s_phase, next;
    bool goal_now = false;

    if (finished == PH_FOCUS) {
        roll_day();
        int dy = today_index();
        if (dy && s_today_pomos == 0) {                                // first pomodoro of the day: extend the streak
            if      (s_last_active_day == 0)      s_streak = 1;         // first ever
            else if (dy == s_last_active_day + 1) s_streak += 1;        // consecutive day
            else if (dy != s_last_active_day)     s_streak = 1;         // gap -> restart
            s_last_active_day = dy;
        }
        s_today_pomos++; s_today_focus_min += s_focus_min; s_total_pomos++;
        s_completed++;
        goal_now = (s_goal > 0 && s_today_pomos == s_goal);
        next = (s_completed >= s_long_every) ? PH_LONG : PH_SHORT;
        if (next == PH_LONG) s_completed = 0;                          // fresh set after the long break

        nucleo_notify_emit("pomodoro", NOTIFY_SUCCESS, "pomodoro",
                           TR("Pomodoro completato", "Pomodoro complete"),
                           next == PH_LONG ? TR("Pausa lunga: stacca!", "Long break: step away!")
                                           : TR("Pausa breve", "Short break"),
                           "app:pomodoro");
        if (goal_now)
            nucleo_notify_emit("pomodoro", NOTIFY_SUCCESS, "pomodoro-goal",
                               TR("Obiettivo raggiunto!", "Daily goal reached!"),
                               TR("Ottimo lavoro oggi", "Great work today"), "app:pomodoro");
    } else {
        next = PH_FOCUS;
        nucleo_notify_emit("pomodoro", NOTIFY_INFO, "pomodoro",
                           TR("Pausa finita", "Break over"),
                           TR("Torna alla concentrazione", "Back to focus"), "app:pomodoro");
    }

    if (finished == PH_FOCUS) { if (goal_now) chime_goal(); else chime_focus_done(); }
    else                      chime_break_done();
    save_cfg();

    if (s_autostart) { s_flash_until = esp_timer_get_time() + (goal_now ? 1600000 : 1200000); enter_phase(next, true); }
    else             { s_pending = next; s_done = true; s_alarm_us = 0; if (goal_now) s_flash_until = esp_timer_get_time() + 1600000; nucleo_app_request_draw(); }
}

static void tm_ack(void)   // acknowledge a finished-phase banner -> roll into the pending phase
{
    s_done = false;
    enter_phase(s_pending, s_autostart);
}
static void tm_skip(void)  // abandon the current phase (no stats): a skipped focus earns only a short break
{
    enter_phase(s_phase == PH_FOCUS ? PH_SHORT : PH_FOCUS, false);   // enter_phase announces the new phase
}

// ── setup editing ─────────────────────────────────────────────────────────────
static void cf_adjust(int field, int delta)
{
    switch (field) {
        case CF_FOCUS:  s_focus_min  = clampi(s_focus_min  + delta, 1, 90); break;
        case CF_SHORT:  s_short_min  = clampi(s_short_min  + delta, 1, 30); break;
        case CF_LONG:   s_long_min   = clampi(s_long_min   + delta, 5, 60); break;
        case CF_EVERY:  s_long_every = clampi(s_long_every + delta, 2, 8);  break;
        case CF_GOAL:   s_goal       = clampi(s_goal       + delta, 1, 24); break;
        case CF_AUTO:   s_autostart  = !s_autostart; break;
        case CF_SOUND:  s_sound      = !s_sound;     break;
        case CF_BREATH: s_breath     = !s_breath;    break;
    }
    if (!s_run && !s_done) s_rem_ms = 0;   // a duration edit refreshes the idle countdown
    chime_tap();
    nucleo_app_request_draw();
}
static const char *cf_label(int i, void *)
{
    switch (i) {
        case CF_FOCUS:  return TR("Concentrazione", "Focus");
        case CF_SHORT:  return TR("Pausa breve", "Short break");
        case CF_LONG:   return TR("Pausa lunga", "Long break");
        case CF_EVERY:  return TR("Lunga ogni", "Long every");
        case CF_GOAL:   return TR("Obiettivo/giorno", "Daily goal");
        case CF_AUTO:   return TR("Auto-avvio", "Auto-start");
        case CF_SOUND:  return TR("Suono", "Sound");
        case CF_BREATH: return TR("Guida respiro", "Breathing guide");
    }
    return "";
}
static const char *cf_value(int i, void *)
{
    static char b[16];
    switch (i) {
        case CF_FOCUS:  snprintf(b, sizeof b, "%d min", s_focus_min); break;
        case CF_SHORT:  snprintf(b, sizeof b, "%d min", s_short_min); break;
        case CF_LONG:   snprintf(b, sizeof b, "%d min", s_long_min);  break;
        case CF_EVERY:  snprintf(b, sizeof b, "x%d", s_long_every);   break;
        case CF_GOAL:   snprintf(b, sizeof b, "%d", s_goal);          break;
        case CF_AUTO:   snprintf(b, sizeof b, "%s", s_autostart ? TR("SI", "ON") : TR("NO", "OFF")); break;
        case CF_SOUND:  snprintf(b, sizeof b, "%s", s_sound     ? TR("SI", "ON") : TR("NO", "OFF")); break;
        case CF_BREATH: snprintf(b, sizeof b, "%s", s_breath    ? TR("SI", "ON") : TR("NO", "OFF")); break;
    }
    return b;
}

// ── drawing ───────────────────────────────────────────────────────────────────
static bool flash_phase(void) { return ((esp_timer_get_time() / 250000) & 1) == 0; }   // 2 Hz blink

static void draw_dots(int cy, unsigned short col)
{
    int n = clampi(s_long_every, 1, 8);
    int gap = 16, total = (n - 1) * gap, x0 = (W - total) / 2;
    for (int i = 0; i < n; i++) {
        int x = x0 + i * gap;
        if (i < s_completed) d.fillCircle(x, cy, 4, col);
        else { d.fillCircle(x, cy, 4, LINE); d.fillCircle(x, cy, 2, BG); }
    }
}

// FOCUS (and, with the breathing guide off, breaks): big digits + progress bar + pomodoro dots.
static void draw_focus_view(int y0)
{
    unsigned short col = phase_col(s_phase);
    bool flashing = (s_flash_until > esp_timer_get_time());
    draw_centered(phase_name(s_phase), 2, y0 + 2, (flashing && !flash_phase()) ? MUTED : col);

    char od[16]; snprintf(od, sizeof od, "%s %d", TR("oggi", "today"), s_today_pomos);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    d.setCursor(238 - (int)strlen(od) * 6, y0 + 6); d.print(od);

    int left = tm_left_ms();
    bool low = s_run && left <= 10000 && s_phase == PH_FOCUS;
    char big[8]; fmt_mmss(left, big, sizeof big);
    draw_centered(big, 5, y0 + 22, low ? C_YELLOW : (s_run ? col : FG));

    int total = phase_set_ms(); if (total < 1) total = 1;
    int bw = W - 48, x = 24, by = y0 + 66, filled = (int)((int64_t)bw * left / total);
    d.drawRoundRect(x, by, bw, 6, 3, LINE);
    d.fillRect(x + 1, by + 1, bw - 2, 4, BG);
    if (filled > 1) d.fillRoundRect(x, by, filled, 6, 3, col);

    draw_dots(y0 + 80, col);
}

// BREAK breathing pacer: a calm ring that grows on the inhale and shrinks on the exhale (4s each), a
// chevron marking the direction, time + phase on the top line. Turns a break into a guided micro-rest.
static void draw_break_breathe(int y0, int bottom)
{
    unsigned short col = phase_col(s_phase);
    const char *nm = phase_name(s_phase);
    char t[8]; fmt_mmss(tm_left_ms(), t, sizeof t);
    int nmw = (int)strlen(nm) * 12, tw = (int)strlen(t) * 12, gap = 14, tot = nmw + gap + tw, x = (W - tot) / 2;
    d.setTextSize(2); d.setTextColor(col, BG); d.setCursor(x, y0 + 2); d.print(nm);
    d.setTextColor(FG, BG); d.setCursor(x + nmw + gap, y0 + 2); d.print(t);

    // Breathing phase: 0..3999 inhale (grow), 4000..7999 exhale (shrink), cosine-eased.
    int ms = (int)((esp_timer_get_time() / 1000) % 8000);
    bool inhale = ms < 4000;
    float local = (inhale ? ms : ms - 4000) / 4000.0f;
    float e = 0.5f - 0.5f * cosf(3.14159265f * local);
    float frac = inhale ? e : 1.0f - e;
    const int RMIN = 14, RMAX = 30;
    int r = RMIN + (int)((RMAX - RMIN) * frac + 0.5f);

    int cx = W / 2, cy = y0 + 18 + (bottom - (y0 + 18)) / 2;
    d.fillArc(cx, cy, RMAX, RMAX + 2, 0, 360, LINE);     // faint outer guide (full breath)
    d.fillArc(cx, cy, r - 3, r, 0, 360, col);            // the breathing ring itself
    // direction chevron at the centre
    if (inhale) d.fillTriangle(cx - 7, cy + 3, cx + 7, cy + 3, cx, cy - 6, col);
    else        d.fillTriangle(cx - 7, cy - 3, cx + 7, cy - 3, cx, cy + 6, col);
}

static void draw_banner(int y0)   // phase finished, autostart off: what ended, where next, blinking
{
    unsigned short col = phase_col(s_phase);
    bool on = flash_phase();
    draw_centered(phase_name(s_phase), 2, y0 + 2, on ? col : MUTED);
    draw_centered(TR("FINITO!", "DONE!"), 3, y0 + 26, on ? col : MUTED);
    char sub[40]; snprintf(sub, sizeof sub, "%s %s", TR("prossima:", "next:"), phase_name(s_pending));
    draw_centered(sub, 1, y0 + 58, MUTED);
    draw_centered(TR("premi un tasto", "press any key"), 1, y0 + 72, on ? col : DIM);
}

static void draw_timer_tab(int y0, int bottom)
{
    if (s_done)                                                draw_banner(y0);
    else if (s_phase != PH_FOCUS && s_run && s_breath)         draw_break_breathe(y0, bottom);
    else                                                       draw_focus_view(y0);
}

static void draw_ring(int cx, int cy, int r, int pct, unsigned short col)
{
    pct = clampi(pct, 0, 100);
    int r0 = r - 8;
    d.fillArc(cx, cy, r0, r, 0, 360, LINE);                       // track
    if (pct > 0) d.fillArc(cx, cy, r0, r, 0, 360 * pct / 100, col);
    d.fillCircle(cx, cy, r0, BG);                                 // hollow centre
}

static void draw_stats_tab(int y0, int bottom)
{
    bool hit = (s_goal > 0 && s_today_pomos >= s_goal);
    int cx = 52, cy = y0 + (bottom - y0) / 2, r = 34;
    int pct = s_goal > 0 ? s_today_pomos * 100 / s_goal : 0;
    draw_ring(cx, cy, r, pct, hit ? C_YELLOW : TOMATO);
    char c[8]; snprintf(c, sizeof c, "%d", s_today_pomos);
    d.setTextSize(3); d.setTextColor(FG, BG); d.setCursor(cx - (int)strlen(c) * 9, cy - 12); d.print(c);
    d.setTextSize(1); d.setTextColor(MUTED, BG);
    char g[12]; snprintf(g, sizeof g, "/ %d", s_goal);
    d.setCursor(cx - (int)strlen(g) * 3, cy + 14); d.print(g);
    if (hit) {
        const char *gm = TR("obiettivo!", "goal!");
        d.setTextColor(C_YELLOW, BG); d.setCursor(cx - (int)strlen(gm) * 3, cy + r + 2); d.print(gm);
    }

    int rx = 108, ry = y0 + 6, step = 26;
    struct { const char *lab; char val[16]; unsigned short col; } rows[3];
    snprintf(rows[0].val, sizeof rows[0].val, "%dh %02dm", s_today_focus_min / 60, s_today_focus_min % 60);
    rows[0].lab = TR("Focus oggi", "Focus today"); rows[0].col = C_GREEN;
    snprintf(rows[1].val, sizeof rows[1].val, "%d", s_total_pomos);
    rows[1].lab = TR("Totale", "Total");           rows[1].col = C_BLUE;
    snprintf(rows[2].val, sizeof rows[2].val, "%d %s", s_streak, TR("gg", "d"));
    rows[2].lab = TR("Serie", "Streak");           rows[2].col = C_YELLOW;
    for (int i = 0; i < 3; i++) {
        int yy = ry + i * step;
        d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(rx, yy); d.print(rows[i].lab);
        d.setTextSize(2); d.setTextColor(rows[i].col, BG); d.setCursor(rx, yy + 10); d.print(rows[i].val);
    }
}

static void set_hint(void)
{
    const char *hint;
    if (s_tab == TAB_TIMER) {
        if (s_done)             hint = TR("un tasto = prossima   TAB schede", "any key = next   TAB tabs");
        else if (s_run)         hint = TR("INVIO pausa  S salta  R azzera", "ENTER pause  S skip  R reset");
        else if (s_rem_ms > 0)  hint = TR("INVIO riprendi  R azzera  TAB schede", "ENTER resume  R reset  TAB tabs");
        else                    hint = TR("INVIO avvia  S salta  TAB schede", "ENTER start  S skip  TAB tabs");
    } else if (s_tab == TAB_STATS) {
        hint = TR("TAB schede   esc esci", "TAB tabs   esc back");
    } else {
        hint = TR("su/giu scegli  </> cambia  TAB schede", "up/dn pick  </> change  TAB tabs");
    }
    nucleo_app_set_hint(hint);
}

static void draw(void)
{
    int top = nucleo_app_content_top(), bottom = top + nucleo_app_content_height();
    d.fillRect(0, top, W, bottom - top, BG);

    static const char *names[NTAB];
    names[TAB_TIMER] = TR("Timer", "Timer");
    names[TAB_STATS] = TR("Statistiche", "Stats");
    names[TAB_SETUP] = TR("Setup", "Setup");
    int y0 = app_ui_tabs(top, names, NTAB, s_tab, TOMATO);

    if      (s_tab == TAB_TIMER) draw_timer_tab(y0, bottom);
    else if (s_tab == TAB_STATS) draw_stats_tab(y0, bottom);
    else                         app_ui_list(y0, bottom - y0, CF_N, s_cf_sel, cf_label, cf_value, nullptr, nullptr);
    set_hint();
}

// ── input ─────────────────────────────────────────────────────────────────────
static void on_key(int key, char ch)
{
    if (s_tab == TAB_SETUP) {
        if (key == NK_RIGHT)              { cf_adjust(s_cf_sel, +1); return; }
        if (key == NK_ENTER || ch == ' ') { if (s_cf_sel >= CF_AUTO) cf_adjust(s_cf_sel, 0); return; }   // toggles
        if (app_ui_list_key(key, ch, &s_cf_sel, CF_N, cf_label, nullptr)) { nucleo_app_request_draw(); return; }
        return;
    }
    if (s_tab == TAB_STATS) return;   // read-only

    // TIMER tab
    if (s_done) { tm_ack(); return; }                                  // any key rolls into the next phase
    if (ch == 'r' || ch == 'R')       { tm_reset(); return; }
    if (ch == 's' || ch == 'S')       { tm_skip();  return; }
    if (key == NK_ENTER || ch == ' ') {
        if (s_run) { tm_pause(); return; }
        bool resuming = s_rem_ms > 0;
        tm_begin();
        if (s_run) { if (resuming) chime_resume(); else chime_go(); }
        return;
    }
}

// LEFT + BACK route here. LEFT edits (setup) / is swallowed (timer); BACK acks a banner, else exits.
static bool on_back(int key)
{
    if (key == NK_LEFT) {
        if (s_tab == TAB_SETUP) cf_adjust(s_cf_sel, -1);
        return true;                                                   // never let a stray LEFT close the app
    }
    if (s_done) { tm_ack(); return true; }
    return false;                                                      // real exit
}

static void on_tab(void)
{
    s_tab = (s_tab + 1) % NTAB;
    chime_tap();
    nucleo_app_request_draw();
}

// Redraw pacing: ~14 fps while a break breathes (smooth pulse), 4 Hz otherwise for the countdown /
// banner, nothing while idle. Also where a running phase is detected crossing zero.
static bool poll(void)
{
    if (s_run && esp_timer_get_time() >= s_end_us) complete_phase();

    // Soft ticks over the final three seconds of a running phase.
    if (s_run) {
        int sec = (tm_left_ms() + 999) / 1000;
        if (sec >= 1 && sec <= 3) { if (sec != s_tick_sec) { s_tick_sec = sec; chime_tick(); } }
        else s_tick_sec = 0;
    } else s_tick_sec = 0;

    bool breathing = (s_phase != PH_FOCUS) && s_run && s_breath;
    int64_t now = esp_timer_get_time();
    int interval_us;
    if (s_done)                        interval_us = 250000;
    else if (breathing)                interval_us = 100000;   // ~10 fps: smooth for a slow 4s in/out breath
    else if (s_run)                    interval_us = 250000;
    else if (s_flash_until > now)      interval_us = 250000;
    else return false;

    // Gentle reminder while a finished-phase banner waits (autostart off).
    if (s_done && s_sound && now - s_alarm_us >= 4000000) { s_alarm_us = now; chime_remind(); }

    if (now - s_last_frame_us < interval_us) return false;
    s_last_frame_us = now;
    return true;
}

static void enter(void)
{
    load_cfg();
    int day = today_index();
    if (day) {
        if (s_stat_day && day != s_stat_day) { s_today_pomos = 0; s_today_focus_min = 0; s_stat_day = day; }
        if (s_last_active_day && day > s_last_active_day + 1) s_streak = 0;
    }
    s_tab = TAB_TIMER; s_run = false; s_done = false; s_rem_ms = 0;
    s_flash_until = 0; s_last_frame_us = 0; s_tick_sec = 0; s_cf_sel = 0;
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(on_tab);
    nucleo_app_set_back_handler(on_back);
    nucleo_app_request_draw();
}

static void on_exit(void)
{
    s_run = false; s_done = false;
    save_cfg();
}

extern "C" void nucleo_register_pomodoro(void)
{
    static const nucleo_app_def_t app = {
        "pomodoro", "Pomodoro", "Office", "Focus timer (Pomodoro): cicli, pause, respiro, statistiche",
        'P', C_RED, enter, on_key, nullptr, draw, on_exit
    };
    nucleo_app_register(&app);
}

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
// pomodoro, and celebrate the daily goal; the final three seconds tick softly.
//
// Language: the OS native TFT engine is IT/EN only (docs/i18n.md) — but this app speaks all FIVE OS
// languages via the local PT() helper, reading nucleo_i18n_lang(). es/fr/de are accent-folded because
// the TFT font (Font0) is ASCII, matching the launcher's own accent-free native strings; every literal
// lives in flash, so five languages cost zero per-string RAM. (The web twin can carry full accents.)
//
// Style: native UI kit — app_ui_tabs / app_ui_list, per-phase accent. Anti-flicker is the canvas+blit
// default: a poll handler asks for a redraw only while a phase runs or a banner blinks. Monotonic
// esp_timer drives the clock; wall-clock time() only rolls the daily stats. Zero heap — all state .bss.
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_audio.h"     // procedural tones we compose earcons from
#include "nucleo_i18n.h"      // nucleo_i18n_lang() — the OS-wide language signal
#include "nucleo_notify.h"    // phase-change notifications -> web Notification Center + on-screen banner
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "app_gfx.h"
#include "app_large.h"     // two screens: the timer on the 320x240 panel, the Cardputer as companion
#include "nucleo_touch.h"

// Tomato identity (also the registered launcher accent). Focus is red, breaks cool.
static const unsigned short TOMATO = C_RED;

// Five-language string pick. The native engine only offers TR(it,en); this covers the other three OS
// languages the web shell supports, so a device set to es/fr/de shows Pomodoro in that language instead
// of the Italian base. Accent-folded (Font0 is ASCII). All args are flash literals -> zero RAM.
static const char *PT(const char *it, const char *en, const char *es, const char *fr, const char *de)
{
    const char *l = nucleo_i18n_lang();
    switch (l[0]) {
        case 'i': return it;
        case 'e': return l[1] == 's' ? es : en;   // "es" vs "en"
        case 'f': return fr;
        case 'd': return de;
        default:  return en;
    }
}

// The UI strings, one row per string in its languages: a call site loads an index, not five literals.
static const char *const S5[][5] = {
    { "FOCUS", "FOCUS", "FOCUS", "FOCUS", "FOKUS" },
    { "PAUSA", "BREAK", "DESCANSO", "PAUSE", "PAUSE" },
    { "PAUSA LUNGA", "LONG BREAK", "DESCANSO LARGO", "PAUSE LONGUE", "LANGE PAUSE" },
    { "Pomodoro completato", "Pomodoro complete", "Pomodoro completado", "Pomodoro termine", "Pomodoro fertig" },
    { "Pausa lunga: stacca!", "Long break: step away!", "Descanso largo: desconecta!", "Pause longue: coupez!", "Lange Pause: abschalten!" },
    { "Pausa breve", "Short break", "Descanso corto", "Pause courte", "Kurze Pause" },
    { "Obiettivo raggiunto!", "Daily goal reached!", "Meta diaria alcanzada!", "Objectif du jour atteint!", "Tagesziel erreicht!" },
    { "Ottimo lavoro oggi", "Great work today", "Buen trabajo hoy", "Bon travail", "Gute Arbeit heute" },
    { "Pausa finita", "Break over", "Descanso terminado", "Pause finie", "Pause vorbei" },
    { "Torna alla concentrazione", "Back to focus", "Vuelve a concentrarte", "Retour au focus", "Zurueck zum Fokus" },
    { "Concentrazione", "Focus", "Concentracion", "Concentration", "Fokus" },
    { "Pausa lunga", "Long break", "Descanso largo", "Pause longue", "Lange Pause" },
    { "Lunga ogni", "Long every", "Larga cada", "Longue toutes", "Lang alle" },
    { "Obiettivo/giorno", "Daily goal", "Meta diaria", "Objectif/jour", "Tagesziel" },
    { "Auto-avvio", "Auto-start", "Auto-inicio", "Auto-demarrage", "Auto-Start" },
    { "Suono", "Sound", "Sonido", "Son", "Ton" },
    { "Guida respiro", "Breathing guide", "Guia respiracion", "Guide respiration", "Atemhilfe" },
    { "SI", "ON", "SI", "OUI", "EIN" },
    { "NO", "OFF", "NO", "NON", "AUS" },
    { "oggi", "today", "hoy", "auj", "heute" },
    { "FINITO!", "DONE!", "LISTO!", "TERMINE!", "FERTIG!" },
    { "prossima:", "next:", "sig:", "suiv:", "nachste:" },
    { "premi un tasto", "press any key", "pulsa una tecla", "appuyez", "Taste druecken" },
    { "Focus oggi", "Focus today", "Focus hoy", "Focus auj", "Fokus heute" },
    { "Totale", "Total", "Total", "Total", "Gesamt" },
    { "gg", "d", "d", "j", "T" },
    { "Serie", "Streak", "Racha", "Serie", "Serie" },
    { "obiettivo!", "goal!", "meta!", "objectif!", "Ziel!" },
    { "un tasto = prossima   TAB schede", "any key = next   TAB tabs", "tecla = sig   TAB pest.", "touche = suiv   TAB ongl.", "Taste = nachste   TAB Tabs" },
    { "INVIO pausa  S salta  R azzera", "ENTER pause  S skip  R reset", "ENTER pausa  S saltar  R reinic", "ENTREE pause  S passer  R remise", "ENTER Pause  S weiter  R Reset" },
    { "INVIO riprendi  R azzera  TAB", "ENTER resume  R reset  TAB", "ENTER seguir  R reinic  TAB", "ENTREE reprendre  R remise", "ENTER weiter  R Reset  TAB" },
    { "INVIO avvia  S salta  TAB schede", "ENTER start  S skip  TAB tabs", "ENTER iniciar  S saltar  TAB", "ENTREE demarrer  S passer  TAB", "ENTER Start  S weiter  TAB" },
    { "TAB schede   esc esci", "TAB tabs   esc back", "TAB pestanas   esc salir", "TAB onglets   esc retour", "TAB Tabs   esc zurueck" },
    { "su/giu scegli  </> cambia  TAB", "up/dn pick  </> change  TAB", "arr/ab elegir  </> cambiar", "haut/bas  </> changer  TAB", "hoch/runter  </> andern  TAB" },
    { "Timer", "Timer", "Timer", "Minuteur", "Timer" },
    { "Statistiche", "Stats", "Estadisticas", "Stats", "Statistik" },
    { "Setup", "Setup", "Ajustes", "Reglages", "Setup" },
    { "Salta", "Skip", "Saltar", "Passer", "Weiter" },
    { "Azzera", "Reset", "Reiniciar", "Remise", "Reset" },
};
static const char *s5(int i) { const char *const *v = S5[i]; return PT(v[0], v[1], v[2], v[3], v[4]); }

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
static const char    *phase_name(int ph)
{
    return ph == PH_FOCUS ? s5(0 /* FOCUS */)
         : ph == PH_SHORT ? s5(1 /* BREAK */)
                          : s5(2 /* LONG BREAK */);
}
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
    LovyanGFX &G = d;
    int wpx = (int)strlen(str) * 6 * size;
    G.setTextSize(size); G.setTextColor(col, BG);
    G.setCursor((W - wpx) / 2, y); G.print(str);
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
                           s5(3 /* Pomodoro complete */),
                           next == PH_LONG ? s5(4 /* Long break: step away! */)
                                           : s5(5 /* Short break */),
                           "app:pomodoro");
        if (goal_now)
            nucleo_notify_emit("pomodoro", NOTIFY_SUCCESS, "pomodoro-goal",
                               s5(6 /* Daily goal reached! */),
                               s5(7 /* Great work today */), "app:pomodoro");
    } else {
        next = PH_FOCUS;
        nucleo_notify_emit("pomodoro", NOTIFY_INFO, "pomodoro",
                           s5(8 /* Break over */),
                           s5(9 /* Back to focus */), "app:pomodoro");
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
        case CF_FOCUS:  return s5(10 /* Focus */);
        case CF_SHORT:  return s5(5 /* Short break */);
        case CF_LONG:   return s5(11 /* Long break */);
        case CF_EVERY:  return s5(12 /* Long every */);
        case CF_GOAL:   return s5(13 /* Daily goal */);
        case CF_AUTO:   return s5(14 /* Auto-start */);
        case CF_SOUND:  return s5(15 /* Sound */);
        case CF_BREATH: return s5(16 /* Breathing guide */);
    }
    return "";
}
static const char *cf_value(int i, void *)
{
    static char b[16];
    const char *on  = s5(17 /* ON */);
    const char *off = s5(18 /* OFF */);
    switch (i) {
        case CF_FOCUS:  snprintf(b, sizeof b, "%d min", s_focus_min); break;
        case CF_SHORT:  snprintf(b, sizeof b, "%d min", s_short_min); break;
        case CF_LONG:   snprintf(b, sizeof b, "%d min", s_long_min);  break;
        case CF_EVERY:  snprintf(b, sizeof b, "x%d", s_long_every);   break;
        case CF_GOAL:   snprintf(b, sizeof b, "%d", s_goal);          break;
        case CF_AUTO:   snprintf(b, sizeof b, "%s", s_autostart ? on : off); break;
        case CF_SOUND:  snprintf(b, sizeof b, "%s", s_sound     ? on : off); break;
        case CF_BREATH: snprintf(b, sizeof b, "%s", s_breath    ? on : off); break;
    }
    return b;
}

// ── drawing ───────────────────────────────────────────────────────────────────
static bool flash_phase(void) { return ((esp_timer_get_time() / 250000) & 1) == 0; }   // 2 Hz blink

static void draw_dots(int cy, unsigned short col)
{
    LovyanGFX &G = d;
    int n = clampi(s_long_every, 1, 8);
    int gap = 16, total = (n - 1) * gap, x0 = (W - total) / 2;
    for (int i = 0; i < n; i++) {
        int x = x0 + i * gap;
        if (i < s_completed) G.fillCircle(x, cy, 4, col);
        else { G.fillCircle(x, cy, 4, LINE); G.fillCircle(x, cy, 2, BG); }
    }
}

// FOCUS (and, with the breathing guide off, breaks): big digits + progress bar + pomodoro dots.
static void draw_focus_view(int y0)
{
    LovyanGFX &G = d;
    unsigned short col = phase_col(s_phase);
    bool flashing = (s_flash_until > esp_timer_get_time());
    draw_centered(phase_name(s_phase), 2, y0 + 2, (flashing && !flash_phase()) ? MUTED : col);

    char od[16]; snprintf(od, sizeof od, "%s %d", s5(19 /* today */), s_today_pomos);
    G.setTextSize(1); G.setTextColor(MUTED, BG);
    G.setCursor(238 - (int)strlen(od) * 6, y0 + 6); G.print(od);

    int left = tm_left_ms();
    bool low = s_run && left <= 10000 && s_phase == PH_FOCUS;
    char big[8]; fmt_mmss(left, big, sizeof big);
    draw_centered(big, 5, y0 + 22, low ? C_YELLOW : (s_run ? col : FG));

    int total = phase_set_ms(); if (total < 1) total = 1;
    int bw = W - 48, x = 24, by = y0 + 66, filled = (int)((int64_t)bw * left / total);
    G.drawRoundRect(x, by, bw, 6, 3, LINE);
    G.fillRect(x + 1, by + 1, bw - 2, 4, BG);
    if (filled > 1) G.fillRoundRect(x, by, filled, 6, 3, col);

    draw_dots(y0 + 80, col);
}

// BREAK breathing pacer: a calm ring that grows on the inhale and shrinks on the exhale (4s each), a
// chevron marking the direction, time + phase on the top line. Turns a break into a guided micro-rest.
static void draw_break_breathe(int y0, int bottom)
{
    LovyanGFX &G = d;
    unsigned short col = phase_col(s_phase);
    const char *nm = phase_name(s_phase);
    char t[8]; fmt_mmss(tm_left_ms(), t, sizeof t);
    int nmw = (int)strlen(nm) * 12, tw = (int)strlen(t) * 12, gap = 14, tot = nmw + gap + tw, x = (W - tot) / 2;
    G.setTextSize(2); G.setTextColor(col, BG); G.setCursor(x, y0 + 2); G.print(nm);
    G.setTextColor(FG, BG); G.setCursor(x + nmw + gap, y0 + 2); G.print(t);

    // Breathing phase: 0..3999 inhale (grow), 4000..7999 exhale (shrink), cosine-eased.
    int ms = (int)((esp_timer_get_time() / 1000) % 8000);
    bool inhale = ms < 4000;
    float local = (inhale ? ms : ms - 4000) / 4000.0f;
    float e = 0.5f - 0.5f * cosf(3.14159265f * local);
    float frac = inhale ? e : 1.0f - e;
    const int RMIN = 14, RMAX = 30;
    int r = RMIN + (int)((RMAX - RMIN) * frac + 0.5f);

    int cx = W / 2, cy = y0 + 18 + (bottom - (y0 + 18)) / 2;
    G.fillArc(cx, cy, RMAX, RMAX + 2, 0, 360, LINE);     // faint outer guide (full breath)
    G.fillArc(cx, cy, r - 3, r, 0, 360, col);            // the breathing ring itself
    // direction chevron at the centre
    if (inhale) G.fillTriangle(cx - 7, cy + 3, cx + 7, cy + 3, cx, cy - 6, col);
    else        G.fillTriangle(cx - 7, cy - 3, cx + 7, cy - 3, cx, cy + 6, col);
}

static void draw_banner(int y0)   // phase finished, autostart off: what ended, where next, blinking
{
    unsigned short col = phase_col(s_phase);
    bool on = flash_phase();
    draw_centered(phase_name(s_phase), 2, y0 + 2, on ? col : MUTED);
    draw_centered(s5(20 /* DONE! */), 3, y0 + 26, on ? col : MUTED);
    char sub[40]; snprintf(sub, sizeof sub, "%s %s", s5(21 /* next: */), phase_name(s_pending));
    draw_centered(sub, 1, y0 + 58, MUTED);
    draw_centered(s5(22 /* press any key */), 1, y0 + 72, on ? col : DIM);
}

static void draw_timer_tab(int y0, int bottom)
{
    if (s_done)                                                draw_banner(y0);
    else if (s_phase != PH_FOCUS && s_run && s_breath)         draw_break_breathe(y0, bottom);
    else                                                       draw_focus_view(y0);
}

static void draw_ring(int cx, int cy, int r, int pct, unsigned short col)
{
    LovyanGFX &G = d;
    pct = clampi(pct, 0, 100);
    int r0 = r - 8;
    G.fillArc(cx, cy, r0, r, 0, 360, LINE);                       // track
    if (pct > 0) G.fillArc(cx, cy, r0, r, 0, 360 * pct / 100, col);
    G.fillCircle(cx, cy, r0, BG);                                 // hollow centre
}

// One STATS row: its label, value (into val[16]) and colour.
static const char *stat_row(int i, char *val, unsigned short *col)
{
    if (i == 0) { snprintf(val, 16, "%dh %02dm", s_today_focus_min / 60, s_today_focus_min % 60); *col = C_GREEN;
                  return s5(23 /* Focus today */); }
    if (i == 1) { snprintf(val, 16, "%d", s_total_pomos); *col = C_BLUE; return s5(24 /* Total */); }
    snprintf(val, 16, "%d %s", s_streak, s5(25 /* d */)); *col = C_YELLOW;
    return s5(26 /* Streak */);
}

static void draw_stats_tab(int y0, int bottom)
{
    LovyanGFX &G = d;
    bool hit = (s_goal > 0 && s_today_pomos >= s_goal);
    int cx = 52, cy = y0 + (bottom - y0) / 2, r = 34;
    int pct = s_goal > 0 ? s_today_pomos * 100 / s_goal : 0;
    draw_ring(cx, cy, r, pct, hit ? C_YELLOW : TOMATO);
    char c[8]; snprintf(c, sizeof c, "%d", s_today_pomos);
    G.setTextSize(3); G.setTextColor(FG, BG); G.setCursor(cx - (int)strlen(c) * 9, cy - 12); G.print(c);
    G.setTextSize(1); G.setTextColor(MUTED, BG);
    char g[12]; snprintf(g, sizeof g, "/ %d", s_goal);
    G.setCursor(cx - (int)strlen(g) * 3, cy + 14); G.print(g);
    if (hit) {
        const char *gm = s5(27 /* goal! */);
        G.setTextColor(C_YELLOW, BG); G.setCursor(cx - (int)strlen(gm) * 3, cy + r + 2); G.print(gm);
    }

    int rx = 108, ry = y0 + 6, step = 26;
    for (int i = 0; i < 3; i++) {
        int yy = ry + i * step; char val[16]; unsigned short col;
        const char *lab = stat_row(i, val, &col);
        G.setTextSize(1); G.setTextColor(MUTED, BG); G.setCursor(rx, yy); G.print(lab);
        G.setTextSize(2); G.setTextColor(col, BG); G.setCursor(rx, yy + 10); G.print(val);
    }
}

static const char *hint_text(void)
{
    const char *hint;
    if (s_tab == TAB_TIMER) {
        if (s_done)             hint = s5(28 /* any key = next   TAB tabs */);
        else if (s_run)         hint = s5(29 /* ENTER pause  S skip  R reset */);
        else if (s_rem_ms > 0)  hint = s5(30 /* ENTER resume  R reset  TAB */);
        else                    hint = s5(31 /* ENTER start  S skip  TAB tabs */);
    } else if (s_tab == TAB_STATS) {
        hint = s5(32 /* TAB tabs   esc back */);
    } else {
        hint = s5(33 /* up/dn pick  </> change  TAB */);
    }
    return hint;
}

static const char *const *tab_names(void)
{
    static const char *names[NTAB];
    names[TAB_TIMER] = s5(34 /* Timer */);
    names[TAB_STATS] = s5(35 /* Stats */);
    names[TAB_SETUP] = s5(36 /* Setup */);
    return names;
}

// ===== two screens ==============================================================================================
// The 320x240 panel, composed in strips (lg::frame: the countdown and the breathing pacer never flicker): the
// header (chevron = back, today's count) and the three tabs as pills you tap, then
//   TIMER: the phase as a ring that empties with the time left (tap it = start / pause / resume; during a
//          break with the guide on, a disc inside it breathes 4 s in / 4 s out), the set's pomodoro dots and
//          the Skip / Reset controls beside it; a finished phase fills the panel, a tap rolls on;
//   STATS: today's ring against the goal + focus time, total, streak;
//   SETUP: the rows (tap = pick, tap again = change), the picked number with its - / + steppers.
// The Cardputer shows the phase, the time and the keys. Keyboard behaviour is unchanged.
#define LW 320
#define LH 240
#define LTOP 62
static int s_lg_scroll;

static void lt(const char *s, int x, int y, int f, unsigned short c)
{
    LovyanGFX &G = d;
    lg::font(f); G.setTextDatum(middle_center); G.setTextColor(c); G.drawString(s, x, y); G.setTextDatum(top_left);
}

static void lp_timer(void)
{
    LovyanGFX &G = d;
    const unsigned short col = phase_col(s_phase);
    if (s_done) {
        G.fillRect(0, LTOP, LW, LH - LTOP, flash_phase() ? col : BG);
        lt(s5(20 /* DONE! */), LW / 2, 120, 3, flash_phase() ? INK : col);
        char sub[40]; snprintf(sub, sizeof sub, "%s %s", s5(21 /* next: */), phase_name(s_pending));
        lt(sub, LW / 2, 170, 1, flash_phase() ? INK : FG);
        nucleo_touch_add(0, LTOP, LW, LH - LTOP, NK_ENTER, 0);
        return;
    }
    const int cx = 100, cy = 151, left = tm_left_ms(), total = phase_set_ms() > 0 ? phase_set_ms() : 1;
    G.fillArc(cx, cy, 70, 84, 0, 360, LINE);
    const int deg = (int)((int64_t)360 * left / total);
    if (deg > 0) G.fillArc(cx, cy, 70, 84, 270, 270 + deg, col);
    if (s_phase != PH_FOCUS && s_run && s_breath) {                     // the breathing disc, 4 s in / 4 s out
        int ms = (int)((esp_timer_get_time() / 1000) % 8000);
        float e = 0.5f - 0.5f * cosf(3.14159265f * (ms < 4000 ? ms : ms - 4000) / 4000.0f);
        G.fillCircle(cx, cy, 24 + (int)(40 * (ms < 4000 ? e : 1.0f - e)), LINE);
    }
    char t[8]; fmt_mmss(left, t, sizeof t);
    lt(phase_name(s_phase), cx, cy - 26, 1, col);
    lt(t, cx, cy + 6, 3, s_run ? FG : MUTED);
    nucleo_touch_add(cx - 84, cy - 84, 168, 168, NK_ENTER, 0);
    const int n = clampi(s_long_every, 1, 8);                            // the set: one dot per pomodoro
    for (int i = 0; i < n; i++) {
        const int x = 212 + (i % 4) * 24, y = 86 + (i / 4) * 24;
        G.fillCircle(x, y, 8, i < s_completed ? col : LINE);
        if (i >= s_completed) G.fillCircle(x, y, 5, BG);
    }
    lg::button(200, 140, 108, 38, s5(37 /* Skip */), LINE, FG, NK_CHAR, 's');
    lg::button(200, 190, 108, 38, s5(38 /* Reset */), LINE, FG, NK_CHAR, 'r');
}

static void lp_stats(void)
{
    LovyanGFX &G = d;
    const int cx = 90, cy = 150, pct = s_goal > 0 ? s_today_pomos * 100 / s_goal : 0;
    G.fillArc(cx, cy, 62, 78, 0, 360, LINE);
    if (pct > 0) G.fillArc(cx, cy, 62, 78, 270, 270 + 360 * clampi(pct, 0, 100) / 100, pct >= 100 ? C_YELLOW : TOMATO);
    char v[16]; snprintf(v, sizeof v, "%d", s_today_pomos);
    lt(v, cx, cy - 8, 3, FG);
    snprintf(v, sizeof v, "/ %d", s_goal);
    lt(v, cx, cy + 24, 0, MUTED);
    for (int i = 0; i < 3; i++) {
        unsigned short col; const char *lab = stat_row(i, v, &col);
        lg::font(0); G.setTextColor(MUTED); G.drawString(lab, 186, 78 + i * 54);
        lg::font(2); G.setTextColor(col); G.drawString(v, 186, 96 + i * 54);
    }
}

static void lp_setup(void)
{
    const int rows = (LH - LTOP) / 30;
    int sc = s_lg_scroll;                                                // the list's own scroll rule, to place the steppers
    if (s_cf_sel < sc) sc = s_cf_sel;
    if (s_cf_sel >= sc + rows) sc = s_cf_sel - rows + 1;
    if (sc > CF_N - rows) sc = CF_N - rows;
    if (sc < 0) sc = 0;
    const int ry = LTOP + (s_cf_sel - sc) * 30;
    const bool num = s_cf_sel < CF_AUTO;
    if (num) { nucleo_touch_add(158, ry, 44, 30, NK_LEFT, 0); nucleo_touch_add(204, ry, 44, 30, NK_RIGHT, 0); }   // before the rows: they win
    lg::text_list(0, LTOP, LW, LH - LTOP, CF_N, s_cf_sel, &s_lg_scroll, cf_label, cf_value, nullptr, nullptr, TOMATO, 30);
    if (num) for (int k = 0; k < 2; k++) {
        const int x = 160 + k * 46;
        d.fillRoundRect(x, ry + 3, 40, 24, 8, INK);
        lt(k ? "+" : "-", x + 20, ry + 15, 1, TOMATO);
    }
}

static void lg_paint(void *)
{
    d.fillRect(0, 0, LW, LH, BG);
    char od[16]; snprintf(od, sizeof od, "%s %d", s5(19 /* today */), s_today_pomos);
    int y = lg::header("Pomodoro", od, TOMATO);
    static const char keys[NTAB] = { 1, 2, 3 };
    lg::tabs(y, tab_names(), NTAB, s_tab, TOMATO, keys);
    if (s_tab == TAB_TIMER) lp_timer(); else if (s_tab == TAB_STATS) lp_stats(); else lp_setup();
    lg::field(0, 0, LW, LH);                                         // a stray tap is nothing (not Enter / an arrow)
}
static void draw_large(void)
{
    nucleo_touch_clear();
    lg::frame(lg_paint, nullptr);
    nucleo_touch_commit();
}

// The Cardputer meanwhile: the phase, its time, today's count and the keys.
static void draw_companion(void)
{
    int y = lg::comp_head("POMODORO", TOMATO);
    char t[8]; fmt_mmss(tm_left_ms(), t, sizeof t);
    draw_centered(phase_name(s_phase), 2, y + 4, phase_col(s_phase));
    draw_centered(t, 5, y + 26, s_run ? FG : MUTED);
    lg::comp_keys(hint_text());
}

static void draw(void)
{
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), bottom = top + nucleo_app_content_height();
    d.fillRect(0, top, W, bottom - top, BG);

    int y0 = app_ui_tabs(top, tab_names(), NTAB, s_tab, TOMATO);

    if      (s_tab == TAB_TIMER) draw_timer_tab(y0, bottom);
    else if (s_tab == TAB_STATS) draw_stats_tab(y0, bottom);
    else                         app_ui_list(y0, bottom - y0, CF_N, s_cf_sel, cf_label, cf_value, nullptr, nullptr);
    nucleo_app_set_hint(hint_text());
}

// ── input ─────────────────────────────────────────────────────────────────────
static void on_key(int key, char ch)
{
    if (key == NK_CHAR && ch >= 1 && ch <= NTAB) { s_tab = ch - 1; chime_tap(); nucleo_app_request_draw(); return; }   // a tab pill
    if (s_tab == TAB_SETUP) {
        if (int t = lg::row_tap(key, ch, s_lg_scroll, CF_N, &s_cf_sel)) {   // panel: tap = pick, again = change
            if (t == 2) cf_adjust(s_cf_sel, s_cf_sel >= CF_AUTO ? 0 : 1); else nucleo_app_request_draw();
            return;
        }
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

// Redraw pacing: ~10 fps while a break breathes (smooth pulse), 4 Hz otherwise for the countdown /
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
        'P', C_RED, enter, on_key, nullptr, draw, on_exit, 0, nullptr, draw_large
    };
    nucleo_app_register(&app);
}

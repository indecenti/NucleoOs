// Allarme / antifurto — Tools. Arms after an exit delay, then watches the chosen SENSORS and fires.
// Two firing MODES, both self-resetting:
//   - Sirena:      loud, piercing, continuous wail + red flashing screen.
//   - Silenzioso:  no sound, no light — the screen stays dark; the hit is only counted/timestamped and
//                  published on the event bus, so the device betrays nothing in the room.
// In BOTH modes the trigger lasts a BOUNDED window (auto-riarmo, default 20 s): the siren stops, the
// alarm re-arms itself and the screen goes dark again — no key needed. The panel is NEVER left lit in
// this app, in ANY state (settings included): 15 s after the last key it goes dark and stays dark;
// the siren keeps wailing behind it. Any key lights it again for 15 s. Sensor sources:
//   - Microfono: loud-noise detection (RMS peak + debounce so silence/clicks don't false-trigger).
//     Works on BOTH boards (board-aware mic HAL) — this is the ONLY source on the non-ADV Cardputer.
//   - Movimento: BMI270 shake/tilt (ADV only).
// Tabbed settings (TAB), big fonts: sorgente, MODO, sensibilita audio/movimento, ritardo, PIN, test
// sirena, auto-riarmo (0/10/20/30/60 s). ESC is blocked while armed/triggered — the PIN is the way
// out and ALWAYS disarms. FOR PERSONAL USE.
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_i18n.h"        // TR(it,en): hint follows the system language
#include "nucleo_imu.h"
#include "nucleo_audio.h"
#include "nucleo_codec.h"
#include "nucleo_ui.h"          // set_brightness(0): a REAL dark panel (app_set_brightness floors at 10%)
#include "nucleo_exclusive.h"
#include <time.h>
#include <sys/stat.h>
#include "nucleo_power.h"       // battery % stamped into every log line
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_gfx.h"

extern "C" void nucleo_voice_suspend(bool suspend);   // free the mic from the voice engine while armed
// Event bus (link-time extern, like nucleo_app.cpp: a REQUIRES on nucleo_eventbus would cycle).
// Every hit is published — the only output a SILENT alarm has.
extern "C" unsigned int nucleo_event_publish(const char *topic, const char *payload);

enum { ST_DISARMED, ST_ARMING, ST_ARMED, ST_TRIGGERED };
enum { SRC_MIC = 0, SRC_MOTION = 1, SRC_BOTH = 2 };
enum { MODE_SIREN = 0, MODE_SILENT = 1 };

// Selected-settings-row capsule = the app's registered accent (was a stray dark tint 0x12B2).
static const unsigned short ACCENT = C_RED;

static int      s_state = ST_DISARMED;
static int64_t  s_arm_t0, s_last_siren;
static int      s_arm_shown;
static float    s_ref_lx, s_ref_ly;
static bool     s_flash, s_siren_hi;
static char     s_entry[8];
static int      s_entry_len;
static bool     s_bad;

// options
static int      s_src = SRC_MIC;        // default works on every board; bumped to BOTH if IMU present
static int      s_sens_motion = 10;     // 1..20 (precise), higher = more sensitive
static int      s_sens_audio = 14;      // default high enough that a clap/voice fires (no shouting)
static int64_t  s_last_armed_draw = 0;  // throttle the live-bar redraw -> no flicker
static int      s_delay_idx = 1;
static unsigned s_pin_hash = 0;         // FNV-1a of the PIN: the plaintext never sits in RAM or on SD
static int      s_mode = MODE_SIREN;    // Sirena | Silenzioso
static int      s_rearm_idx = 2;        // index into REARM[] -> 20 s: the trigger window in BOTH modes
static bool     s_settings, s_pin_edit;
static int      s_set_sel;
static char     s_pin_buf[8];
static int      s_pin_buf_len;
// Changing the PIN is a two-stage challenge: type the CURRENT one, then the new one.
enum { PIN_OLD = 0, PIN_NEW };
enum { PP_CHANGE = 0, PP_WIPE };        // what the challenge unlocks
static int      s_pin_stage, s_pin_purpose;
static bool     s_pin_bad;              // wrong current PIN -> stay on stage 1 and say so
static int64_t  s_pin_ok_until;         // confirmation-toast deadline
static const char *s_toast = "";        // what that toast says
static bool     s_dirty_cfg;            // options changed -> persist on the way out

// trigger bookkeeping + stealth screen
static int64_t  s_trig_t0;              // when the current trigger fired (auto-rearm deadline)
static int      s_trig_count;           // hits this session (a silent alarm's visible output)
static char     s_trig_last[8];         // "HH:MM" of the last hit, "" = none
static bool     s_scr_off;              // WE blanked the backlight (armed = dark panel)
static int64_t  s_scr_hold_until;       // keep it lit until this deadline, then blank again

// mic state
static i2s_chan_handle_t s_mic = NULL;
static int16_t  s_mic_buf[320];
static int      s_audio_consec;
static float    s_audio_level;          // 0..1 live peak (for the bar)
static int      s_saved_vol = -1;

// Sensitivity is now a precise NUMBER 1..20 (higher = more sensitive = lower trigger threshold), mapped
// linearly to the physical threshold. Defaults tuned so a clap / raised voice fires (no shouting).
#define SENS_MAX 20
static float audio_thr(void)   { return 0.30f  - (float)(s_sens_audio  - 1) * (0.30f  - 0.015f) / (SENS_MAX - 1); }  // peak 0..1
static float motion_e_thr(void){ return 0.45f  - (float)(s_sens_motion - 1) * (0.45f  - 0.030f) / (SENS_MAX - 1); }  // shake energy
static float motion_t_thr(void){ return 0.30f  - (float)(s_sens_motion - 1) * (0.30f  - 0.020f) / (SENS_MAX - 1); }  // tilt delta
static const char *const SRC_NAME[3] = { "Microfono", "Movimento", "Entrambi" };
static const int         DELAYS[4]   = { 3, 5, 10, 15 };
// Auto-riarmo is not a bool: it IS the trigger window. After this many seconds the siren stops, the
// alarm re-arms itself and the screen goes dark — in the silent mode too. 0 = off (hold until PIN).
static const int         REARM[5]    = { 0, 10, 20, 30, 60 };
static int64_t rearm_us(void) { return (int64_t)REARM[s_rearm_idx] * 1000000; }

static bool motion_on(void) { return (s_src == SRC_MOTION || s_src == SRC_BOTH) && nucleo_imu_present(); }
static bool audio_on(void)  { return (s_src == SRC_MIC || s_src == SRC_BOTH); }

// ---- PIN + persisted options -------------------------------------------------------------------
// The PIN is stored as a 32-bit FNV-1a hash, never as text: a 4-digit code is brute-forceable by
// anyone holding the SD card anyway, but at least the card doesn't show it at a glance. Options live
// next to the other native apps' config (screensaver.json et al.) so a changed PIN survives a reboot
// — an antifurto that forgets its code on every power cycle is useless.
#define ALARM_CFG "/sd/system/config/alarm.json"
static unsigned pin_hash(const char *p)
{
    unsigned h = 2166136261u;
    for (; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
    return h;
}
static bool pin_check(const char *p) { return pin_hash(p) == s_pin_hash; }
static bool cfg_load(void)
{
    s_pin_hash = pin_hash("0000");                       // factory default until the SD says otherwise
    FILE *f = fopen(ALARM_CFG, "rb");
    if (!f) return false;
    char buf[160];
    int n = (int)fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (n <= 0) return false;
    buf[n] = 0;
    unsigned h = 0; int src = s_src, mode = s_mode, sa = s_sens_audio, sm = s_sens_motion, dl = s_delay_idx, re = s_rearm_idx;
    if (sscanf(buf, "{\"pin\":%u,\"src\":%d,\"mode\":%d,\"sa\":%d,\"sm\":%d,\"delay\":%d,\"rearm\":%d}",
               &h, &src, &mode, &sa, &sm, &dl, &re) != 7) return false;
    if (h) s_pin_hash = h;
    if ((unsigned)src <= SRC_BOTH)  s_src = src;
    if ((unsigned)mode <= MODE_SILENT) s_mode = mode;
    if (sa >= 1 && sa <= SENS_MAX)  s_sens_audio = sa;
    if (sm >= 1 && sm <= SENS_MAX)  s_sens_motion = sm;
    if ((unsigned)dl < 4)           s_delay_idx = dl;
    if ((unsigned)re < 5)           s_rearm_idx = re;
    return true;
}
// ---- SD event log --------------------------------------------------------------------------------
// EVERY alarm event is appended to the SD as one NDJSON line — armed, triggered, re-armed, disarmed,
// wrong PIN, log wiped — with the wall clock, the uptime, the sensor reading that caused it and the
// full configuration at that moment, so a hit can be judged (real? false? which sensor? how loud
// against which threshold?) long after the fact. Opened and closed per line: a power cut can lose at
// most the line being written, never the file. Size-rotated into ONE .1 backup.
#define ALARM_LOG     "/sd/data/Alarm/alarm.ndjson"
#define ALARM_LOG_OLD "/sd/data/Alarm/alarm.1.ndjson"
#define ALARM_LOG_MAX (256 * 1024)
static void log_evt(const char *ev, const char *src, int lvl_pct)
{
    mkdir("/sd/data", 0775);
    mkdir("/sd/data/Alarm", 0775);
    struct stat st;
    if (stat(ALARM_LOG, &st) == 0 && st.st_size > ALARM_LOG_MAX) {   // rotate: keep exactly one backup
        remove(ALARM_LOG_OLD);
        rename(ALARM_LOG, ALARM_LOG_OLD);
    }
    FILE *f = fopen(ALARM_LOG, "ab");
    if (!f) return;
    char ts[24];
    time_t t = time(NULL);
    struct tm tmv;
    if (t >= 1600000000 && localtime_r(&t, &tmv))
        snprintf(ts, sizeof ts, "%04d-%02d-%02dT%02d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    else
        snprintf(ts, sizeof ts, "?");                                 // clock not set yet: uptime still tells the order
    int bat = nucleo_power_battery_available() ? nucleo_power_battery_pct() : -1;
    fprintf(f, "{\"t\":\"%s\",\"up\":%d,\"ev\":\"%s\",\"src\":\"%s\",\"mode\":\"%s\","
               "\"n\":%d,\"lvl\":%d,\"thr\":%d,\"sens_a\":%d,\"sens_m\":%d,"
               "\"delay\":%d,\"rearm\":%d,\"bat\":%d}\n",
            ts, (int)(esp_timer_get_time() / 1000000), ev, src ? src : "",
            s_mode == MODE_SILENT ? "silent" : "siren", s_trig_count, lvl_pct,
            (int)(audio_thr() * 100.0f + 0.5f), s_sens_audio, s_sens_motion,
            DELAYS[s_delay_idx], REARM[s_rearm_idx], bat);
    fclose(f);
}
// Wiping the log is itself logged: the file is never silently empty about who emptied it.
static void log_wipe(void)
{
    remove(ALARM_LOG);
    remove(ALARM_LOG_OLD);
    s_trig_count = 0; s_trig_last[0] = 0;
    log_evt("wiped", NULL, 0);
}
static long log_size(void)
{
    struct stat st;
    long n = (stat(ALARM_LOG, &st) == 0) ? (long)st.st_size : 0;
    if (stat(ALARM_LOG_OLD, &st) == 0) n += (long)st.st_size;
    return n;
}

static void cfg_save(void)
{
    if (!s_dirty_cfg) return;
    s_dirty_cfg = false;
    mkdir("/sd/system", 0775);
    mkdir("/sd/system/config", 0775);
    FILE *f = fopen(ALARM_CFG, "wb");
    if (!f) return;
    fprintf(f, "{\"pin\":%u,\"src\":%d,\"mode\":%d,\"sa\":%d,\"sm\":%d,\"delay\":%d,\"rearm\":%d}\n",
            s_pin_hash, s_src, s_mode, s_sens_audio, s_sens_motion, s_delay_idx, s_rearm_idx);
    fclose(f);
}

// ---- siren: loud (max volume), piercing (2.9/3.8 kHz), continuous warble ----
static void siren_vol_grab(void) { if (s_saved_vol < 0) { s_saved_vol = nucleo_audio_volume(); nucleo_audio_set_volume(100); } }
static void siren_vol_release(void) { if (s_saved_vol >= 0) { nucleo_audio_set_volume(s_saved_vol); s_saved_vol = -1; } }

// ---- mic lifecycle ----
static void mic_start(void)
{
    if (s_mic || !audio_on()) return;
    nucleo_voice_suspend(true);                  // the voice engine owns the mic otherwise
    nucleo_codec_mic(true);                       // power the ADV ADC (no-op on original)
    if (nucleo_codec_mic_open(16000, &s_mic) != ESP_OK) s_mic = NULL;
    s_audio_consec = 0; s_audio_level = 0;
}
static void mic_stop(void)
{
    if (s_mic) { nucleo_codec_mic_close(s_mic); s_mic = NULL; }
    nucleo_codec_mic(false);
    nucleo_voice_suspend(false);
}
static bool mic_loud(void)
{
    if (!s_mic) return false;
    size_t got = 0;
    if (nucleo_codec_mic_read(s_mic, s_mic_buf, sizeof s_mic_buf, &got, 0) != ESP_OK || got < 2) return false;
    int n = (int)(got / 2); int32_t peak = 0;
    for (int i = 0; i < n; i++) { int32_t a = s_mic_buf[i]; if (a < 0) a = -a; if (a > peak) peak = a; }
    s_audio_level = (float)peak / 32768.0f;
    if (s_audio_level > audio_thr()) { return (++s_audio_consec >= 2); }   // 2 frames = no single-click trigger
    s_audio_consec = 0;
    return false;
}

// ---- settings (TAB) ----
// Named rows: the row order changed (Modo was inserted), so every index is a symbol now.
enum { R_SRC = 0, R_MODE, R_SENS_A, R_SENS_M, R_DELAY, R_PIN, R_TEST, R_REARM, R_LOG, ASET_ROWS };
static const char *aset_label(int i)
{
    static const char *const L[ASET_ROWS] = { "Sorgente", "Modo", "Sens. audio", "Sens. movimento",
                                              "Ritardo armo", "PIN", "Test sirena", "Auto-riarmo",
                                              "Cancella log" };
    return (i >= 0 && i < ASET_ROWS) ? L[i] : "";
}
static const char *aset_right(int i)
{
    static char b[16];
    switch (i) {
        case R_SRC: if (s_src != SRC_MIC && !nucleo_imu_present()) return "Mic (no IMU)"; return SRC_NAME[s_src];
        case R_MODE: return s_mode == MODE_SILENT ? "Silenzioso" : "Sirena";
        case R_SENS_A: snprintf(b, sizeof b, "%d/20", s_sens_audio); return b;
        case R_SENS_M: if (!nucleo_imu_present()) return "n/d"; snprintf(b, sizeof b, "%d/20", s_sens_motion); return b;
        case R_DELAY: snprintf(b, sizeof b, "%ds", DELAYS[s_delay_idx]); return b;
        case R_PIN: return "INVIO";
        case R_TEST: return "INVIO";
        case R_REARM: if (!REARM[s_rearm_idx]) return "off"; snprintf(b, sizeof b, "%ds", REARM[s_rearm_idx]); return b;
        case R_LOG: { long n = log_size(); if (!n) return "vuoto"; snprintf(b, sizeof b, "%ldKB", (n + 1023) / 1024); return b; }
    }
    return "";
}
static void siren_test(void)
{
    siren_vol_grab();
    for (int i = 0; i < 10; i++) { nucleo_audio_siren(150); esp_task_wdt_reset(); }   // ~1.5 s of the real continuous siren
    nucleo_audio_siren_stop();
    siren_vol_release();
}
static void set_change(int dir)
{
    s_dirty_cfg = true;
    switch (s_set_sel) {
        case R_SRC:  s_src  = (s_src + dir + 3) % 3; break;
        case R_MODE: s_mode = (s_mode + dir + 2) % 2; break;
        case R_SENS_A: s_sens_audio  += dir; if (s_sens_audio  < 1) s_sens_audio  = 1; if (s_sens_audio  > SENS_MAX) s_sens_audio  = SENS_MAX; break;
        case R_SENS_M: s_sens_motion += dir; if (s_sens_motion < 1) s_sens_motion = 1; if (s_sens_motion > SENS_MAX) s_sens_motion = SENS_MAX; break;
        case R_DELAY: s_delay_idx = (s_delay_idx + dir + 4) % 4; break;
        case R_REARM: s_rearm_idx = (s_rearm_idx + dir + 5) % 5; break;
    }
    nucleo_app_request_draw();
}

// ---- helpers ----
static void center(const char *t, int y, int size, unsigned short col, unsigned short bg)
{
    int tw = (int)strlen(t) * 6 * size;
    d.setTextSize(size); d.setTextColor(col, bg);
    d.setCursor((W - tw) / 2, y); d.print(t);
}
static void draw_pin_dots(int cy, int len, unsigned short col, unsigned short bg)
{
    for (int i = 0; i < 4; i++) {
        int dx = W / 2 - 30 + i * 20;
        if (i < len) d.fillCircle(dx, cy, 5, col);
        else         d.drawCircle(dx, cy, 5, bg == col ? MUTED : col);
    }
}

// ---- stealth screen: an ARMED alarm sits behind a DARK panel -----------------------------------
// nucleo_app_set_brightness() floors at 10% (the system-wide "never fully dark" guard), so a real
// blank goes straight to the panel driver — the same escape hatch /api/display uses. The stored level
// is untouched, so waking is just re-applying it. Any key lights the screen for SCR_HOLD_KEY_US (long
// enough to type the PIN); after that, and after every auto re-arm, it goes dark again.
#define SCR_HOLD_ARM_US  (3LL  * 1000000)   // show the "ARMATO" confirmation this long, then dark
#define SCR_HOLD_KEY_US  (15LL * 1000000)   // a keypress lights the panel this long (PIN entry)
static void screen_wake(int64_t hold_us)
{
    s_scr_hold_until = esp_timer_get_time() + hold_us;
    if (!s_scr_off) return;
    s_scr_off = false;
    nucleo_app_set_brightness(nucleo_app_brightness());   // re-apply the user's stored level
    nucleo_app_force_repaint();                           // the panel still holds the pre-blank frame
    nucleo_app_request_draw();
}
static void screen_blank(void)
{
    if (s_scr_off) return;
    s_scr_off = true;
    s_scr_hold_until = esp_timer_get_time() + 2000000;   // reused as the next re-assert deadline
    nucleo_ui_set_brightness(0);
}
// Re-assert the dark panel every ~2 s while armed: the torch overlay, a notification banner or the
// Control Center can re-light the backlight behind the app's back, and with draw() short-circuited
// that would leave a frozen, lit screen — exactly what a stealth alarm must not do.
static void screen_keep_dark(int64_t now)
{
    if (!s_scr_off || now < s_scr_hold_until) return;
    s_scr_hold_until = now + 2000000;
    nucleo_ui_set_brightness(0);
}

static void disarm_to_idle(void)
{
    log_evt(s_state == ST_ARMING ? "cancel" : "disarm", NULL, 0);
    mic_stop(); nucleo_audio_siren_stop(); siren_vol_release();
    s_state = ST_DISARMED; s_entry_len = 0; s_flash = false; s_bad = false;
    screen_wake(SCR_HOLD_KEY_US);          // disarmed is a normal, visible screen
    nucleo_app_request_draw();
}
static void start_arming(void)
{
    if (!motion_on() && !audio_on()) return;
    s_state = ST_ARMING; s_arm_t0 = esp_timer_get_time(); s_arm_shown = DELAYS[s_delay_idx]; s_entry_len = 0;
    nucleo_app_request_draw();
}
static void capture_ref(void)
{
    float lx = 0, ly = 0, deg = 0;
    for (int i = 0; i < 6; i++) { nucleo_imu_sample(); nucleo_imu_level(&lx, &ly, &deg); }
    s_ref_lx = lx; s_ref_ly = ly;
}
static void go_armed(void)
{
    if (motion_on()) capture_ref();
    mic_start();
    s_state = ST_ARMED;
    log_evt("armed", motion_on() ? (audio_on() ? "mic+motion" : "motion") : "mic", 0);
    screen_wake(SCR_HOLD_ARM_US);          // brief "ARMATO" confirmation, then the panel goes dark
    nucleo_app_request_draw();
}

// ---- trigger + auto re-arm ---------------------------------------------------------------------
// A hit is ALWAYS counted, timestamped and published on the event bus (the web Notification Center
// picks "alarm.trigger" up) — that log is the whole output of the silent mode, which makes no sound
// and no light. nucleo_notify_emit is deliberately NOT used: it plays a chime when no web client is
// connected, which would break the silence.
static void stamp_now(char *out, size_t n)
{
    time_t t = time(NULL);
    struct tm tmv;
    if (t < 1600000000 || !localtime_r(&t, &tmv)) { snprintf(out, n, "--:--"); return; }   // clock not set yet
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}
static void fire_trigger(const char *src)
{
    mic_stop();                                  // free the I2S: the siren needs it
    s_trig_count++;
    stamp_now(s_trig_last, sizeof s_trig_last);
    char p[128];
    snprintf(p, sizeof p, "{\"src\":\"%s\",\"mode\":\"%s\",\"count\":%d,\"at\":\"%s\"}",
             src, s_mode == MODE_SILENT ? "silent" : "siren", s_trig_count, s_trig_last);
    nucleo_event_publish("alarm.trigger", p);
    log_evt("trigger", src, (int)(s_audio_level * 100.0f + 0.5f));
    s_state = ST_TRIGGERED; s_trig_t0 = esp_timer_get_time();
    s_last_siren = 0; s_entry_len = 0; s_bad = false;
    if (s_mode == MODE_SILENT) { screen_blank(); return; }   // mute AND dark: nothing in the room changes
    siren_vol_grab();
    screen_wake(SCR_HOLD_KEY_US);                            // the red flashing screen is part of the siren mode
    nucleo_app_request_draw();
}
// Trigger window elapsed: stop the noise, re-arm on the spot (no exit countdown — nobody is leaving)
// and go dark again. Same path in both modes.
static void rearm_now(void)
{
    nucleo_audio_siren_stop(); siren_vol_release();
    s_entry_len = 0; s_bad = false; s_flash = false;
    if (motion_on()) capture_ref();
    mic_start();
    s_state = ST_ARMED;
    log_evt("rearm", NULL, 0);
    // Dark again — unless a key was pressed in the last SCR_HOLD_KEY_US (someone is standing there
    // typing the PIN): then the ARMED poll blanks it when that window expires, so the panel never
    // dies mid-keystroke.
    if (esp_timer_get_time() >= s_scr_hold_until) screen_blank();
}

static void tab(void)
{
    if (s_scr_off) { screen_wake(SCR_HOLD_KEY_US); return; }
    s_scr_hold_until = esp_timer_get_time() + SCR_HOLD_KEY_US;
    if (s_state == ST_ARMED || s_state == ST_TRIGGERED) return;
    if (s_settings) cfg_save();                 // leaving the sheet: write the options to SD
    s_settings = !s_settings; s_set_sel = 0; s_pin_edit = false;
    nucleo_app_request_draw();
}
static bool back(int key)
{
    if (s_scr_off) { screen_wake(SCR_HOLD_KEY_US); return true; }   // dark panel: the key only wakes it
    s_scr_hold_until = esp_timer_get_time() + SCR_HOLD_KEY_US;
    if (s_settings) {
        if (s_pin_edit) {
            if (s_pin_buf_len > 0) s_pin_buf_len--;      // backspace: fix a mistyped digit
            else s_pin_edit = false;
            nucleo_app_request_draw(); return true;
        }
        if (key == NK_LEFT) { if (s_set_sel != R_PIN && s_set_sel != R_TEST && s_set_sel != R_LOG) set_change(-1); return true; }
        s_settings = false; cfg_save(); nucleo_app_request_draw(); return true;
    }
    if (s_state == ST_ARMING) { disarm_to_idle(); return true; }
    if (s_state == ST_ARMED || s_state == ST_TRIGGERED) return true;   // block ESC — PIN required
    return false;
}

static bool poll(void)
{
    int64_t now = esp_timer_get_time();

    // ONE rule, every state, settings included: the panel is NEVER left lit. SCR_HOLD_KEY_US after
    // the last keypress it goes dark and STAYS dark (re-asserted, since the torch/banner/Control
    // Center can re-light it behind our back). The first key lights it again. The exit countdown is
    // the single exception — it must stay readable while you walk away — and it lasts <= 15 s anyway.
    if (s_state == ST_ARMING) s_scr_hold_until = now + 2000000;
    screen_keep_dark(now);
    if (!s_scr_off && now > s_scr_hold_until) screen_blank();
    if (s_settings) return false;

    if (s_state == ST_ARMING) {
        if (motion_on()) nucleo_imu_sample();
        int rem = DELAYS[s_delay_idx] - (int)((now - s_arm_t0) / 1000000);
        if (rem <= 0) { go_armed(); return true; }
        if (rem != s_arm_shown) { s_arm_shown = rem; nucleo_app_request_draw(); }
        return false;
    }
    if (s_state == ST_ARMED) {
        bool trig = false; const char *src = "mic";
        if (motion_on()) {
            nucleo_imu_sample();
            float lx = 0, ly = 0, deg = 0; nucleo_imu_level(&lx, &ly, &deg);
            float e = nucleo_imu_energy();
            float td = sqrtf((lx - s_ref_lx) * (lx - s_ref_lx) + (ly - s_ref_ly) * (ly - s_ref_ly));
            if (e > motion_e_thr() || td > motion_t_thr()) { trig = true; src = "motion"; }
        }
        if (!trig && audio_on() && mic_loud()) trig = true;
        if (trig) { fire_trigger(src); return false; }
        if (!s_scr_off && now - s_last_armed_draw > 120000) {     // throttle live-bar redraw to ~8 fps -> no flicker
            s_last_armed_draw = now; nucleo_app_request_draw();
        }
        return false;
    }
    if (s_state == ST_TRIGGERED) {
        // Bounded trigger window: after REARM[] seconds the alarm resets ITSELF — the siren stops and
        // the screen goes dark — with no key pressed. 0 = off keeps the old "until the PIN" behaviour.
        if (rearm_us() && now - s_trig_t0 >= rearm_us()) { rearm_now(); return false; }
        if (s_mode == MODE_SILENT) {                              // silent: no wail, no flashing, no light
            if (!s_scr_off && now - s_last_armed_draw > 250000) { s_last_armed_draw = now; nucleo_app_request_draw(); }
            return false;
        }
        s_siren_hi = !s_siren_hi; s_flash = !s_flash;
        if (!s_scr_off) nucleo_app_request_draw();               // dark panel: don't composite the flash
        nucleo_audio_siren(150);                                 // the WAIL never stops with the screen — only the light does
        return false;
    }
    return false;
}

static void on_key(int key, char ch)
{
    if (s_scr_off) { screen_wake(SCR_HOLD_KEY_US); return; }   // dark panel: the first key just wakes it
    s_scr_hold_until = esp_timer_get_time() + SCR_HOLD_KEY_US; // any key renews the lit window
    if (s_settings) {
        if (s_pin_edit) {
            // Stage 1: prove you know the CURRENT PIN. Stage 2: type the new one. A wrong current
            // PIN never reaches stage 2, so a stranger who finds the device armed-but-open can't
            // silently re-key it.
            if (ch >= '0' && ch <= '9' && s_pin_buf_len < 4) {
                s_pin_buf[s_pin_buf_len++] = ch;
                s_pin_bad = false;
                if (s_pin_buf_len == 4) {
                    s_pin_buf[4] = 0;
                    if (s_pin_stage == PIN_OLD) {
                        if (!pin_check(s_pin_buf)) { s_pin_bad = true; s_pin_buf_len = 0; log_evt("pin_bad", "settings", 0); }
                        else if (s_pin_purpose == PP_WIPE) {     // erasing the evidence needs the PIN
                            log_wipe();
                            s_pin_edit = false;
                            s_toast = "Log cancellati"; s_pin_ok_until = esp_timer_get_time() + 2000000;
                        } else { s_pin_stage = PIN_NEW; s_pin_buf_len = 0; }
                    } else {
                        s_pin_hash = pin_hash(s_pin_buf);
                        s_dirty_cfg = true; cfg_save();          // a new PIN is persisted IMMEDIATELY
                        s_pin_edit = false;
                        s_toast = "PIN aggiornato"; s_pin_ok_until = esp_timer_get_time() + 2000000;
                    }
                }
                nucleo_app_request_draw();
            } else if (key == NK_ENTER) { s_pin_edit = false; nucleo_app_request_draw(); }
            return;
        }
        if (key == NK_UP)        { s_set_sel = (s_set_sel + ASET_ROWS - 1) % ASET_ROWS; nucleo_app_request_draw(); }
        else if (key == NK_DOWN) { s_set_sel = (s_set_sel + 1) % ASET_ROWS; nucleo_app_request_draw(); }
        else if (key == NK_RIGHT || key == NK_ENTER) {
            if (s_set_sel == R_PIN || s_set_sel == R_LOG) {
                s_pin_edit = true; s_pin_stage = PIN_OLD; s_pin_buf_len = 0; s_pin_bad = false;
                s_pin_purpose = (s_set_sel == R_LOG) ? PP_WIPE : PP_CHANGE;
                nucleo_app_request_draw();
            }
            else if (s_set_sel == R_TEST) siren_test();
            else set_change(+1);
        }
        return;
    }
    if (s_state == ST_ARMED || s_state == ST_TRIGGERED) {
        if (ch >= '0' && ch <= '9') {
            if (s_entry_len < 4) s_entry[s_entry_len++] = ch;
            s_bad = false;
            if (s_entry_len == 4) {
                s_entry[4] = 0;
                // The PIN is the DELIBERATE way out: it always disarms. (Re-arming here — the old
                // auto-rearm bool — made the app inescapable, since Esc is blocked while armed.)
                if (pin_check(s_entry)) disarm_to_idle();
                else { s_bad = true; s_entry_len = 0; log_evt("pin_bad", "keypad", 0); }   // tamper attempt
            }
            nucleo_app_request_draw();
        }
        return;
    }
    if (s_state == ST_DISARMED) { if (key == NK_ENTER || ch == 'a' || ch == 'A') start_arming(); return; }
    if (s_state == ST_ARMING)   { if (key == NK_ENTER) disarm_to_idle(); }
}

// ---- big-font settings list (framework style, readable) ----
static void draw_settings(int y0, int bottom)
{
    if (s_pin_edit) {
        bool old = (s_pin_stage == PIN_OLD);
        bool wipe = (s_pin_purpose == PP_WIPE);
        center(old ? "PIN attuale" : "Nuovo PIN", y0 + 14, 2, old ? C_YELLOW : FG, BG);
        draw_pin_dots((y0 + bottom) / 2, s_pin_buf_len, old ? C_YELLOW : C_BLUE, BG);
        if (s_pin_bad)   center("PIN errato - riprova", bottom - 16, 1, C_RED, BG);
        else if (!old)   center("4 cifre  -  ESC cancella", bottom - 16, 1, MUTED, BG);
        else if (wipe)   center("PIN per cancellare i log", bottom - 16, 1, C_RED, BG);
        else             center("conferma il PIN in uso", bottom - 16, 1, MUTED, BG);
        return;
    }
    if (esp_timer_get_time() < s_pin_ok_until) center(s_toast,        y0 + 4, 2, C_GREEN, BG);
    else                                       center("Impostazioni", y0 + 4, 2, C_BLUE,  BG);
    int rowh = 18, y = y0 + 26, maxrows = (bottom - y) / rowh;
    int start = 0; if (s_set_sel >= maxrows) start = s_set_sel - maxrows + 1;
    for (int i = start; i < ASET_ROWS && i < start + maxrows; i++) {
        bool on = (i == s_set_sel);
        if (on) d.fillRoundRect(4, y, W - 8, rowh - 2, 3, ACCENT);
        d.setTextSize(2); d.setTextColor(on ? FG : MUTED, on ? ACCENT : BG); d.setCursor(10, y + 2); d.print(aset_label(i));
        const char *rv = aset_right(i);
        int rw = (int)strlen(rv) * 12; d.setTextColor(on ? C_GREEN : DIM, on ? ACCENT : BG); d.setCursor(W - 10 - rw, y + 2); d.print(rv);
        y += rowh;
    }
}

static void draw(void)
{
    if (s_scr_off) return;          // panel is dark (armed/stealth): don't burn CPU compositing it
    int top = nucleo_app_content_top(), h = nucleo_app_content_height(), bottom = top + h;
    unsigned short bg = BG;
    if (s_state == ST_TRIGGERED && s_mode != MODE_SILENT) bg = s_flash ? C_RED : 0x3800;
    d.fillRect(0, top, W, h, bg);

    static char rb[16];
    const char *rl = "off"; unsigned short acc = MUTED;
    if (s_state == ST_ARMING)         { rl = "armo"; acc = C_YELLOW; }
    else if (s_state == ST_ARMED)     { rl = s_mode == MODE_SILENT ? "MUTO" : "ON"; acc = C_GREEN; }
    else if (s_state == ST_TRIGGERED) { rl = "!!!";  acc = C_RED;    }
    if (s_trig_count > 0) { snprintf(rb, sizeof rb, "%s %d", rl, s_trig_count); rl = rb; }   // hit counter, always in view
    int y0 = app_ui_title("Allarme", acc, rl);
    int cy = (y0 + bottom) / 2;

    if (s_settings) { draw_settings(y0, bottom); return; }

    if (s_state == ST_DISARMED) {
        center("DISARMATO", cy - 22, 3, FG, BG);
        center("INVIO per armare", cy + 6, 2, MUTED, BG);
        char ln[44];
        const char *src = (s_src != SRC_MIC && !nucleo_imu_present()) ? "Microfono" : SRC_NAME[s_src];
        const char *md  = (s_mode == MODE_SILENT) ? "silenzioso" : "sirena";
        if (s_trig_count > 0) snprintf(ln, sizeof ln, "%s  %d eventi  ultimo %s", md, s_trig_count, s_trig_last);
        else                  snprintf(ln, sizeof ln, "%s  %s  ritardo %ds", src, md, DELAYS[s_delay_idx]);
        center(ln, bottom - 14, 1, DIM, BG);
    } else if (s_state == ST_ARMING) {
        char nb[8]; snprintf(nb, sizeof nb, "%d", s_arm_shown > 0 ? s_arm_shown : 1);
        center(nb, y0 + 12, 6, C_YELLOW, BG);
        center("allontanati...", bottom - 16, 2, MUTED, BG);
    } else if (s_state == ST_ARMED) {
        center("ARMATO", y0 + 8, 3, C_GREEN, BG);
        int by = cy + 4, bw = W - 64, bx = 32;
        if (motion_on()) {
            float frac = nucleo_imu_energy() / motion_e_thr(); if (frac > 1) frac = 1;
            d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(bx, by - 10); d.print("movimento");
            d.drawRect(bx, by, bw, 8, MUTED); d.fillRect(bx + 1, by + 1, (int)((bw - 2) * frac), 6, frac > 0.8f ? C_RED : C_GREEN);
            by += 22;
        }
        if (audio_on()) {
            float frac = s_audio_level / audio_thr(); if (frac > 1) frac = 1;
            d.setTextSize(1); d.setTextColor(MUTED, BG); d.setCursor(bx, by - 10); d.print("audio");
            d.drawRect(bx, by, bw, 8, MUTED); d.fillRect(bx + 1, by + 1, (int)((bw - 2) * frac), 6, frac > 0.8f ? C_RED : C_BLUE);
        }
        center(s_bad ? "PIN errato" : "digita il PIN", bottom - 14, 2, s_bad ? C_RED : MUTED, BG);
    } else {   // TRIGGERED
        bool silent = (s_mode == MODE_SILENT);
        center(silent ? "! RILEVATO !" : "! ALLARME !", y0 + 10, 3, silent ? C_YELLOW : FG, bg);
        draw_pin_dots(cy + 6, s_entry_len, FG, bg);
        if (s_bad) { center("PIN errato", bottom - 16, 2, FG, bg); return; }
        char ln[40];
        int rem = rearm_us() ? (int)((rearm_us() - (esp_timer_get_time() - s_trig_t0)) / 1000000) + 1 : 0;
        if (rem > 0) snprintf(ln, sizeof ln, "%s %s   riarmo in %ds", silent ? "muto" : "sirena", s_trig_last, rem);
        else         snprintf(ln, sizeof ln, "%s %s   PIN per fermare", silent ? "muto" : "sirena", s_trig_last);
        center(ln, bottom - 14, 1, FG, bg);
    }
}

static void enter(void)
{
    s_state = ST_DISARMED; s_settings = false; s_pin_edit = false;
    s_entry_len = 0; s_flash = false; s_bad = false;
    s_scr_off = false; s_scr_hold_until = 0;
    s_trig_count = 0; s_trig_last[0] = 0;
    s_pin_bad = false; s_pin_ok_until = 0; s_dirty_cfg = false;
    bool had_cfg = cfg_load();                   // PIN + options survive a reboot
    // Exploit the IMU when it's there — but never override a saved, deliberate "Microfono" choice.
    if (!had_cfg && nucleo_imu_present() && s_src == SRC_MIC) s_src = SRC_BOTH;
    nucleo_app_set_hint(TR("invio arma   tab modo/opzioni   tasto = accendi schermo",
                           "enter arm   tab mode/options   any key = screen on"));
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    nucleo_app_request_draw();
}

// Never leave the panel dark behind us: the app owns the backlight only while it is foreground.
static void on_exit(void) { cfg_save(); mic_stop(); nucleo_audio_siren_stop(); siren_vol_release(); screen_wake(0); }

extern "C" void nucleo_register_alarm(void)
{
    static const nucleo_app_def_t app = {
        "alarm", "Allarme", "Office", "Antifurto: sirena o silenzioso, riarmo automatico",
        '!', C_RED, enter, on_key, nullptr, draw, on_exit,
        // Declarative exclusive: drop the ANIMA L1 index (~24 KB) and stop a running recording that
        // would fight for the mic. NOT NX_HTTPD/NX_DISCOVERY: the silent mode's ONLY output is the
        // alarm.trigger event reaching the browser, which needs the server up. NOT NX_VOICE either —
        // mic_start/stop already own the voice engine via nucleo_voice_suspend().
        NX_ANIMA_L1 | NX_RECORDER
    };
    nucleo_app_register(&app);
}

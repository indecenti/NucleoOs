// Alarm / anti-theft — Tools. Arms after an exit delay, then watches the chosen SENSORS and fires.
// Two firing MODES, both self-resetting:
//   - Sirena:      loud, piercing, continuous wail + red flashing screen.
//   - Silenzioso:  no sound, no light — the screen stays dark; the hit is counted/timestamped,
//                  published on the event bus, and RECORDED: the mic keeps rolling into a WAV on the
//                  SD for the configured minutes, across re-arms and further hits.
// In BOTH modes the trigger lasts a BOUNDED window (auto re-arm, default 20 s): the siren stops, the
// alarm re-arms itself and the screen goes dark again — no key needed. The panel is NEVER left lit in
// this app, in ANY state (settings included): the app OPENS lit for 10 s, then goes dark 15 s after
// the last key and STAYS dark — the siren keeps wailing behind it. Any key lights it again for 15 s
// and is swallowed by the wake. Sensor sources:
//   - Microfono: loud-noise detection (RMS peak + debounce so silence/clicks don't false-trigger).
//     Works on BOTH boards (board-aware mic HAL) — this is the ONLY source on the non-ADV Cardputer.
//   - Movimento: BMI270 shake/tilt (ADV only).
// Tabbed settings (TAB), big fonts: source, MODE, audio/motion sensitivity, delay, PIN, siren
// test, auto re-arm (0/10/20/30/60 s). ESC is blocked while armed/triggered — the PIN is the way
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
#include <stdlib.h>
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_gfx.h"
#include "app_large.h"       // two screens: the alarm on the 320x240 panel, the Cardputer as companion
#include "nucleo_touch.h"

// The UI strings, one row per string in its languages: a call site loads an index, not five literals.
static const char *const S5[][5] = {
    { "invio arma  tab modo/reg  tasto=schermo", "enter arm  tab mode/rec  key=screen", "intro arma  tab modo/grab  tecla=scr", "entree arme  tab mode/enr  touche=ecr", "eingabe arm  tab modus/aufn  taste=scr" },
};
static const char *s5(int i) { const char *const *v = S5[i]; return nucleo_tr5(v[0], v[1], v[2], v[3], v[4]); }
static const char *const S2[][2] = {
    { "solo Mic", "Mic only" },
    { "Silenzioso", "Silent" },
    { "Sirena", "Siren" },
    { "n/d", "n/a" },
    { "INVIO", "ENTER" },
    { "vuoto", "empty" },
    { "armo", "arming" },
    { "MUTO", "MUTE" },
    { "PIN attuale", "Current PIN" },
    { "Nuovo PIN", "New PIN" },
    { "PIN errato - riprova", "Wrong PIN - retry" },
    { "4 cifre  -  ESC cancella", "4 digits  -  ESC cancel" },
    { "PIN per cancellare i log", "PIN to clear the logs" },
    { "conferma il PIN in uso", "confirm the current PIN" },
    { "Allarme", "Alarm" },
    { "DISARMATO", "DISARMED" },
    { "INVIO per armare", "ENTER to arm" },
    { "Microfono", "Microphone" },
    { "silenzioso", "silent" },
    { "sirena", "siren" },
    { "%s  %d eventi  ultimo %s", "%s  %d events  last %s" },
    { "%s  %s  ritardo %ds", "%s  %s  delay %ds" },
    { "allontanati...", "walk away..." },
    { "ARMATO", "ARMED" },
    { "movimento", "motion" },
    { "PIN errato", "Wrong PIN" },
    { "digita il PIN", "type the PIN" },
    { "! RILEVATO !", "! DETECTED !" },
    { "! ALLARME !", "! ALARM !" },
    { "%s %s  riarmo in %ds", "%s %s  re-arm in %ds" },
    { "muto", "mute" },
    { "%s %s  PIN per fermare", "%s %s  PIN to stop" },
    { "ARMA", "ARM" },
    { "eventi", "events" },
    { "ALLARME", "ALARM" },
};
static const char *s2(int i) { return nucleo_tr(S2[i][0], S2[i][1]); }

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
static int64_t  s_arm_t0;
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
static int      s_rec_idx = 3;          // index into REC_MIN[] -> 3 min of ambient recording (silent mode)
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
static const char *s_toast = "";        // what that toast says (Italian literal)
static const char *s_toast_en = "";     // ...and its English twin, picked with TR() at paint time
static bool     s_dirty_cfg;            // options changed -> persist on the way out
static int      s_lg_scroll;            // the panel's settings-list scroll

// trigger bookkeeping + stealth screen
static int64_t  s_trig_t0;              // when the current trigger fired (auto-rearm deadline)
static int      s_trig_count;           // hits this session (a silent alarm's visible output)
static char     s_trig_last[8];         // "HH:MM" of the last hit, "" = none
static int64_t  s_settle_until;         // arm/re-arm grace: ignore hits until the room (and the DMA) settle
// Ambient recording (silent mode only): survives re-arms and further hits — see the recorder block.
static FILE    *s_rec;
static unsigned s_rec_bytes;            // PCM payload written so far (WAV header patched on close)
static int64_t  s_rec_until, s_rec_cap, s_rec_t0;
static unsigned s_rec_synced;           // payload length last written INTO the header (crash safety)
static char     s_rec_name[40];
static bool     s_scr_off;              // WE blanked the backlight (armed = dark panel)
static int64_t  s_scr_hold_until;       // keep it lit until this deadline, then blank again

// mic state
static i2s_chan_handle_t s_mic = NULL;
static int16_t *s_mic_buf;              // 640 B, heap while the mic is open (never .bss)
static int      s_audio_consec;
static float    s_audio_level;          // 0..1 live peak (for the bar)
static int      s_saved_vol = -1;

// Sensitivity is now a precise NUMBER 1..20 (higher = more sensitive = lower trigger threshold), mapped
// linearly to the physical threshold. Defaults tuned so a clap / raised voice fires (no shouting).
#define SENS_MAX 20
static float audio_thr(void)   { return 0.30f  - (float)(s_sens_audio  - 1) * (0.30f  - 0.015f) / (SENS_MAX - 1); }  // peak 0..1
static float motion_e_thr(void){ return 0.45f  - (float)(s_sens_motion - 1) * (0.45f  - 0.030f) / (SENS_MAX - 1); }  // shake energy
static float motion_t_thr(void){ return 0.30f  - (float)(s_sens_motion - 1) * (0.30f  - 0.020f) / (SENS_MAX - 1); }  // tilt delta
static const char *const SRC_NAME[3]    = { "Microfono", "Movimento", "Entrambi" };
static const char *const SRC_NAME_EN[3] = { "Microphone", "Motion", "Both" };   // picked at paint time
static const int         DELAYS[4]   = { 3, 5, 10, 15 };
// Auto re-arm is not a bool: it IS the trigger window. After this many seconds the siren stops, the
// alarm re-arms itself and the screen goes dark — in the silent mode too. 0 = off (hold until PIN).
static const int         REARM[5]    = { 0, 10, 20, 30, 60 };
// Ambient recording length, minutes. 0 = off. The alarm re-arms itself DURING the recording and can
// fire again — neither stops it: a further hit only pushes the stop deadline out (never past the cap).
static const int         REC_MIN[6]  = { 0, 1, 2, 3, 5, 10 };
#define REC_CAP_US (10LL * 60 * 1000000)   // hard ceiling from the first hit: ~19 MB of WAV, and no more
static int64_t rearm_us(void) { return (int64_t)REARM[s_rearm_idx] * 1000000; }

static bool motion_on(void) { return (s_src == SRC_MOTION || s_src == SRC_BOTH) && nucleo_imu_present(); }
static bool audio_on(void)  { return s_src == SRC_MIC || s_src == SRC_BOTH || !nucleo_imu_present(); }

// ---- PIN + persisted options -------------------------------------------------------------------
// The PIN is stored as a 32-bit FNV-1a hash, never as text: a 4-digit code is brute-forceable by
// anyone holding the SD card anyway, but at least the card doesn't show it at a glance. Options live
// next to the other native apps' config (screensaver.json et al.) so a changed PIN survives a reboot
// — an anti-theft alarm that forgets its code on every power cycle is useless.
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
    // "rec" was added later: read it separately so an older config file still loads (and a newer one
    // still opens on an older build) instead of failing the whole strict-format scanf.
    const char *r = strstr(buf, "\"rec\":");
    if (r) { int v = atoi(r + 6); if ((unsigned)v < 6) s_rec_idx = v; }
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
static void log_evt_i(const char *ev, const char *src, int lvl_pct, const char *info)
{
    struct stat st;
    if (stat(ALARM_LOG, &st) == 0 && st.st_size > ALARM_LOG_MAX) {   // rotate: keep exactly one backup
        remove(ALARM_LOG_OLD);
        rename(ALARM_LOG, ALARM_LOG_OLD);
    }
    FILE *f = fopen(ALARM_LOG, "ab");
    if (!f) {                                                        // first run: create the folders once
        mkdir("/sd/data", 0775);
        mkdir("/sd/data/Alarm", 0775);
        f = fopen(ALARM_LOG, "ab");
    }
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
               "\"delay\":%d,\"rearm\":%d,\"rec\":%d,\"bat\":%d,\"info\":\"%s\"}\n",
            ts, (int)(esp_timer_get_time() / 1000000), ev, src ? src : "",
            s_mode == MODE_SILENT ? "silent" : "siren", s_trig_count, lvl_pct,
            (int)(audio_thr() * 100.0f + 0.5f), s_sens_audio, s_sens_motion,
            DELAYS[s_delay_idx], REARM[s_rearm_idx], REC_MIN[s_rec_idx], bat, info ? info : "");
    fclose(f);
}
static void log_evt(const char *ev, const char *src, int lvl_pct) { log_evt_i(ev, src, lvl_pct, ""); }
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
    fprintf(f, "{\"pin\":%u,\"src\":%d,\"mode\":%d,\"sa\":%d,\"sm\":%d,\"delay\":%d,\"rearm\":%d,\"rec\":%d}\n",
            s_pin_hash, s_src, s_mode, s_sens_audio, s_sens_motion, s_delay_idx, s_rearm_idx, s_rec_idx);
    fclose(f);
}


// ---- ambient recording (silent mode) --------------------------------------------------------------
// A silent alarm that only counts hits tells you something happened; a recording tells you WHAT. On a
// silent trigger the mic stays open and its frames are written to a plain 16 kHz mono 16-bit WAV on
// the SD — the same frames the detector is already reading, so there is no second stream and no gap.
//
// The recording deliberately OUTLIVES the alarm cycle: the auto re-arm re-arms underneath it and a
// further hit can fire while it runs. Neither closes the file — a further hit only pushes the stop
// deadline out, never past REC_CAP_US from the first one. Siren mode never records: the speaker and
// the mic share the I2S/WS line on this hardware, so the wail and the capture cannot coexist.
#define REC_DIR "/sd/data/Alarm/rec"
#define REC_RATE 16000
static void wav_head(FILE *f, unsigned data_bytes)
{
    unsigned char h[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
        16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0,
        'd','a','t','a', 0,0,0,0
    };
    unsigned riff = 36 + data_bytes, rate = REC_RATE, brate = REC_RATE * 2;
    memcpy(h + 4,  &riff,  4);
    memcpy(h + 24, &rate,  4);
    memcpy(h + 28, &brate, 4);
    memcpy(h + 40, &data_bytes, 4);
    fwrite(h, 1, sizeof h, f);
}
static void rec_close(const char *why)
{
    if (!s_rec) return;
    fflush(s_rec);
    fseek(s_rec, 0, SEEK_SET);          // patch the two sizes now that the payload length is known
    wav_head(s_rec, s_rec_bytes);
    fclose(s_rec);
    s_rec = NULL;
    char info[64];
    snprintf(info, sizeof info, "%s %ds %uKB", s_rec_name,
             (int)((esp_timer_get_time() - s_rec_t0) / 1000000), (s_rec_bytes + 1023) / 1024);
    log_evt_i("rec_stop", why, 0, info);
    nucleo_app_request_draw();
}
static void rec_open(void)
{
    if (s_rec || !REC_MIN[s_rec_idx]) return;
    time_t t = time(NULL);
    struct tm tmv;
    if (t >= 1600000000 && localtime_r(&t, &tmv))
        snprintf(s_rec_name, sizeof s_rec_name, "%04d%02d%02d-%02d%02d%02d.wav",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    else
        snprintf(s_rec_name, sizeof s_rec_name, "up%d.wav", (int)(esp_timer_get_time() / 1000000));
    char path[80];
    snprintf(path, sizeof path, "%s/%s", REC_DIR, s_rec_name);
    s_rec = fopen(path, "wb");
    if (!s_rec) {                                    // first run: the folder does not exist yet
        mkdir("/sd/data", 0775); mkdir("/sd/data/Alarm", 0775); mkdir(REC_DIR, 0775);
        s_rec = fopen(path, "wb");
    }
    if (!s_rec) { log_evt_i("rec_fail", "open", 0, s_rec_name); return; }   // no SD: the alarm still works
    s_rec_bytes = 0; s_rec_synced = 0;
    s_rec_t0 = esp_timer_get_time();
    s_rec_cap = s_rec_t0 + REC_CAP_US;
    wav_head(s_rec, 0);
    fflush(s_rec);                                   // the file must be real on the card from second 0
    log_evt_i("rec_start", "mic", 0, s_rec_name);
}
// A hit while the tape rolls: extend, never restart — and never past the absolute cap.
static void rec_bump(void)
{
    if (!REC_MIN[s_rec_idx]) return;
    int64_t now = esp_timer_get_time();
    if (!s_rec) rec_open();
    if (!s_rec) return;
    s_rec_until = now + (int64_t)REC_MIN[s_rec_idx] * 60 * 1000000;
    if (s_rec_until > s_rec_cap) s_rec_until = s_rec_cap;
}
static void rec_clock(char *out, size_t n)
{
    int64_t now = esp_timer_get_time();
    int el  = (int)((now - s_rec_t0) / 1000000);
    int rem = (int)((s_rec_until - now) / 1000000); if (rem < 0) rem = 0;
    snprintf(out, n, "REC %d:%02d -%ds", el / 60, el % 60, rem);
}
static void rec_sync(void)
{
    if (!s_rec) return;
    long pos = ftell(s_rec);
    fseek(s_rec, 0, SEEK_SET);
    wav_head(s_rec, s_rec_bytes);
    fseek(s_rec, pos, SEEK_SET);
    fflush(s_rec);
}
static void rec_write(const void *buf, size_t n)
{
    if (!s_rec) return;
    if (fwrite(buf, 1, n, s_rec) != n) { rec_close("write error"); return; }   // SD full / pulled out
    s_rec_bytes += (unsigned)n;
    if (s_rec_bytes - s_rec_synced >= 64000) { s_rec_synced = s_rec_bytes; rec_sync(); }   // ~2 s
}

// ---- siren: loud (max volume), piercing (2.9/3.8 kHz), continuous warble ----
static void siren_vol_grab(void) { if (s_saved_vol < 0) { s_saved_vol = nucleo_audio_volume(); nucleo_audio_set_volume(100); } }
static void siren_vol_release(void) { if (s_saved_vol >= 0) { nucleo_audio_set_volume(s_saved_vol); s_saved_vol = -1; } }

// ---- mic lifecycle ----
// The voice engine owns the mic unless we suspend it, and the suspension is held for the WHOLE armed
// life — not per mic open. It used to be released inside mic_stop(), which runs on every trigger: the
// voice engine could then grab GPIO43 in the ~20 s siren window and the auto re-arm would reopen the
// mic into a busy I2S — leaving the alarm armed but DEAF. Released only on disarm / app exit.
static bool s_voice_held;
static int64_t s_mic_retry;             // next attempt after a failed open
static void voice_hold(bool on)
{
    if (on == s_voice_held) return;
    s_voice_held = on;
    nucleo_voice_suspend(on);
}
#define MIC_FRAME 320                             // samples per read = 20 ms @ 16 kHz
static void mic_ensure(void)                      // open regardless of the source setting
{
    if (s_mic) return;
    if (!s_mic_buf) s_mic_buf = (int16_t *)malloc(MIC_FRAME * sizeof(int16_t));
    if (!s_mic_buf) return;                       // out of heap: stay silent rather than crash
    voice_hold(true);
    nucleo_codec_mic(true);                       // power the ADV ADC (no-op on original)
    if (nucleo_codec_mic_open(16000, &s_mic) != ESP_OK) s_mic = NULL;
    s_audio_consec = 0; s_audio_level = 0;
}
static void mic_start(void) { if (audio_on()) mic_ensure(); }
static void mic_stop(void)
{
    if (s_mic) { nucleo_codec_mic_close(s_mic); s_mic = NULL; }
    nucleo_codec_mic(false);
    free(s_mic_buf); s_mic_buf = NULL;
}
// ONE read path: it updates the live level AND feeds the recorder, so the tape gets exactly the
// frames the detector saw — no second stream, no gap while a hit is being processed.
static bool mic_frame(void)
{
    if (!s_mic || !s_mic_buf) return false;
    size_t got = 0;
    if (nucleo_codec_mic_read(s_mic, s_mic_buf, MIC_FRAME * sizeof(int16_t), &got, 0) != ESP_OK || got < 2) return false;
    int n = (int)(got / 2); int32_t peak = 0;
    for (int i = 0; i < n; i++) { int32_t a = s_mic_buf[i]; if (a < 0) a = -a; if (a > peak) peak = a; }
    s_audio_level = (float)peak / 32768.0f;
    rec_write(s_mic_buf, got);
    return true;
}
// Drain what the DMA holds (one frame is 20 ms; the poll runs faster, but an SD write can stall it),
// so nothing is lost from the recording and no stale audio is left to re-trigger the alarm.
#define MIC_DRAIN 8
static bool mic_loud(void)
{
    bool loud = false;
    for (int i = 0; i < MIC_DRAIN && mic_frame(); i++) {
        if (s_audio_level > audio_thr()) { if (++s_audio_consec >= 2) { loud = true; break; } }
        else s_audio_consec = 0;
    }
    return loud;
}
// Same drain, detection off: used while a trigger is being served so the tape keeps rolling.
static void mic_pump(void) { for (int i = 0; i < MIC_DRAIN && mic_frame(); i++) {} }

// ---- settings (TAB) ----
// Named rows: the row order changed (Modo was inserted), so every index is a symbol now.
enum { R_SRC = 0, R_MODE, R_SENS_A, R_SENS_M, R_DELAY, R_PIN, R_TEST, R_REARM, R_REC, R_LOG, ASET_ROWS };
static const char *aset_label(int i)
{
    static const char *const L[ASET_ROWS] = { "Sorgente", "Modo", "Sens. audio", "Sens. moto",
                                              "Ritardo armo", "PIN", "Test sirena", "Auto-riarmo",
                                              "Registra muto", "Cancella log" };
    static const char *const L_EN[ASET_ROWS] = { "Source", "Mode", "Audio sens.", "Motion sens.",
                                                 "Arm delay", "PIN", "Siren test", "Auto re-arm",
                                                 "Silent rec", "Clear log" };
    return (i >= 0 && i < ASET_ROWS) ? TR(L[i], L_EN[i]) : "";
}
static const char *aset_right(int i)
{
    static char b[16];
    switch (i) {
        case R_SRC: if (s_src != SRC_MIC && !nucleo_imu_present()) return s2(0 /* Mic only */); return TR(SRC_NAME[s_src], SRC_NAME_EN[s_src]);
        case R_MODE: return s_mode == MODE_SILENT ? s2(1 /* Silent */) : s2(2 /* Siren */);
        case R_SENS_A: snprintf(b, sizeof b, "%d/20", s_sens_audio); return b;
        case R_SENS_M: if (!nucleo_imu_present()) return s2(3 /* n/a */); snprintf(b, sizeof b, "%d/20", s_sens_motion); return b;
        case R_DELAY: snprintf(b, sizeof b, "%ds", DELAYS[s_delay_idx]); return b;
        case R_PIN: return s2(4 /* ENTER */);
        case R_TEST: return s2(4 /* ENTER */);
        case R_REARM: if (!REARM[s_rearm_idx]) return "off"; snprintf(b, sizeof b, "%ds", REARM[s_rearm_idx]); return b;
        case R_REC: if (s_mode != MODE_SILENT) return s2(3 /* n/a */);      // the siren owns the I2S: no capture
                    if (!REC_MIN[s_rec_idx]) return "off";
                    snprintf(b, sizeof b, "%dmin", REC_MIN[s_rec_idx]); return b;
        case R_LOG: { long n = log_size(); if (!n) return s2(5 /* empty */); snprintf(b, sizeof b, "%ldKB", (n + 1023) / 1024); return b; }
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
        case R_REC:   s_rec_idx   = (s_rec_idx + dir + 6) % 6; break;
    }
    nucleo_app_request_draw();
}

// ---- helpers ----
static void center(const char *t, int y, int size, unsigned short col, unsigned short bg)
{
    LovyanGFX &G = d;
    int tw = (int)strlen(t) * 6 * size;
    G.setTextSize(size); G.setTextColor(col, bg);
    G.setCursor((W - tw) / 2, y); G.print(t);
}
static void draw_pin_dots(int cx, int cy, int len, unsigned short col, unsigned short bg)
{
    LovyanGFX &G = d;
    for (int i = 0; i < 4; i++) {
        int dx = cx - 30 + i * 20;
        if (i < len) G.fillCircle(dx, cy, 5, col);
        else         G.drawCircle(dx, cy, 5, bg == col ? MUTED : col);
    }
}

// ---- stealth screen: an ARMED alarm sits behind a DARK panel -----------------------------------
// nucleo_app_set_brightness() floors at 10% (the system-wide "never fully dark" guard), so a real
// blank goes straight to the panel driver — the same escape hatch /api/display uses. The stored level
// is untouched, so waking is just re-applying it. Any key lights the screen for SCR_HOLD_KEY_US (long
// enough to type the PIN); after that, and after every auto re-arm, it goes dark again.
#define SCR_HOLD_OPEN_US (10LL * 1000000)   // the app OPENS lit and stays lit this long
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
    nucleo_app_request_draw();                           // the external panel goes black too (two screens)
    s_entry_len = 0; s_bad = false;                      // the keypad session ends with the light
    if (s_pin_edit) { s_pin_edit = false; s_pin_buf_len = 0; }
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
    rec_close("disarmed");                       // the PIN is a deliberate end to the whole session
    mic_stop(); voice_hold(false); nucleo_audio_siren_stop(); siren_vol_release();
    s_state = ST_DISARMED; s_entry_len = 0; s_flash = false; s_bad = false;
    screen_wake(SCR_HOLD_KEY_US);          // disarmed is a normal, visible screen
    nucleo_app_request_draw();
}
static void start_arming(void)
{
    if (!motion_on() && !audio_on()) {          // ENTER used to do nothing at all here
        s_toast = "Nessun sensore attivo"; s_toast_en = "No sensor enabled"; s_pin_ok_until = esp_timer_get_time() + 2000000;
        nucleo_app_request_draw();
        return;
    }
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
    s_settle_until = esp_timer_get_time() + 1500000;   // let the mic DMA + the hand that pressed ENTER settle
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
    if (s_mode == MODE_SIREN) mic_stop();        // free the I2S for the wail (siren mode cannot record)
    s_trig_count++;
    stamp_now(s_trig_last, sizeof s_trig_last);
    char p[128];
    snprintf(p, sizeof p, "{\"src\":\"%s\",\"mode\":\"%s\",\"count\":%d,\"at\":\"%s\"}",
             src, s_mode == MODE_SILENT ? "silent" : "siren", s_trig_count, s_trig_last);
    nucleo_event_publish("alarm.trigger", p);
    log_evt("trigger", src, (int)(s_audio_level * 100.0f + 0.5f));
    s_state = ST_TRIGGERED; s_trig_t0 = esp_timer_get_time();
    s_entry_len = 0; s_bad = false;
    if (s_mode == MODE_SILENT) {                 // mute AND dark: nothing in the room changes
        if (REC_MIN[s_rec_idx]) {
            mic_ensure();                        // a motion-only setup has no mic open yet
            rec_bump();                          // start the tape, or push its deadline out
        }
        screen_blank();
        return;
    }
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
    // 3 s of grace: without it the siren's own tail (and the stale I2S buffer) re-triggers the alarm
    // the instant it re-arms, looping a hit every REARM seconds forever.
    s_settle_until = esp_timer_get_time() + 3000000;
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
    if (s_state == ST_ARMING && !s_scr_off) s_scr_hold_until = now + 2000000;
    screen_keep_dark(now);
    if (!s_scr_off && now > s_scr_hold_until) screen_blank();

    // The tape outlives the trigger AND the auto re-arm: it is serviced here, above the state
    // machine, so no state transition can drop a frame or leave the file open.
    if (s_rec) {
        if (now >= s_rec_until) {
            rec_close("done");
            if (!audio_on() && s_mic) mic_stop();          // motion-only: the mic was only for the tape
        } else if (s_state == ST_TRIGGERED || !audio_on()) {
            mic_pump();                                    // ARMED+audio already drains inside mic_loud()
        }
    }
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
        if (trig && now < s_settle_until) { trig = false; s_audio_consec = 0; }   // arm/re-arm grace
        if (trig) { fire_trigger(src); return false; }
        // A failed mic_open would otherwise leave the alarm armed but deaf until the next re-arm.
        if (audio_on() && !s_mic && now - s_mic_retry > 1000000) { s_mic_retry = now; mic_start(); }
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
    if (s_pin_ok_until && now > s_pin_ok_until) {   // toast expired: one redraw to clear it
        s_pin_ok_until = 0; nucleo_app_request_draw();
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
                            s_toast = "Log cancellati"; s_toast_en = "Logs cleared"; s_pin_ok_until = esp_timer_get_time() + 2000000;
                        } else { s_pin_stage = PIN_NEW; s_pin_buf_len = 0; }
                    } else {
                        s_pin_hash = pin_hash(s_pin_buf);
                        s_dirty_cfg = true; cfg_save();          // a new PIN is persisted IMMEDIATELY
                        s_pin_edit = false;
                        s_toast = "PIN aggiornato"; s_toast_en = "PIN updated"; s_pin_ok_until = esp_timer_get_time() + 2000000;
                    }
                }
                nucleo_app_request_draw();
            } else if (key == NK_ENTER) { s_pin_edit = false; nucleo_app_request_draw(); }
            return;
        }
        if (int t = lg::row_tap(key, ch, s_lg_scroll, ASET_ROWS, &s_set_sel)) {   // panel: tap = pick, again = act
            if (t == 1) { nucleo_app_request_draw(); return; }
            key = NK_ENTER;
        }
        if (ch >= '1' && ch <= '9' && ch - '1' < ASET_ROWS) { s_set_sel = ch - '1'; nucleo_app_request_draw(); }
        else if (key == NK_UP)   { s_set_sel = (s_set_sel + ASET_ROWS - 1) % ASET_ROWS; nucleo_app_request_draw(); }
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

// The title's right-hand status ("off", "ON 2", "REC 1"...) and the state's accent.
static const char *status_text(unsigned short *acc)
{
    static char rb[16];
    const char *rl = "off"; *acc = MUTED;
    if (s_state == ST_ARMING)         { rl = s2(6 /* arming */); *acc = C_YELLOW; }
    else if (s_state == ST_ARMED)     { rl = s_mode == MODE_SILENT ? s2(7 /* MUTE */) : "ON"; *acc = C_GREEN; }
    else if (s_state == ST_TRIGGERED) { rl = "!!!";  *acc = C_RED;    }
    if (s_rec) rl = "REC";                                                                   // the tape is rolling
    if (s_trig_count > 0) { snprintf(rb, sizeof rb, "%s %d", rl, s_trig_count); rl = rb; }   // hit counter, always in view
    return rl;
}

// ---- big-font settings list (framework style, readable) ----
static void draw_settings(int y0, int bottom)
{
    LovyanGFX &G = d;
    if (s_pin_edit) {
        bool old = (s_pin_stage == PIN_OLD);
        bool wipe = (s_pin_purpose == PP_WIPE);
        center(old ? s2(8 /* Current PIN */) : s2(9 /* New PIN */), y0 + 14, 2, old ? C_YELLOW : FG, BG);
        draw_pin_dots(W / 2, (y0 + bottom) / 2, s_pin_buf_len, old ? C_YELLOW : C_BLUE, BG);
        if (s_pin_bad)   center(s2(10 /* Wrong PIN - retry */), bottom - 16, 1, C_RED, BG);
        else if (!old)   center(s2(11 /* 4 digits  -  ESC cancel */), bottom - 16, 1, MUTED, BG);
        else if (wipe)   center(s2(12 /* PIN to clear the logs */), bottom - 16, 1, C_RED, BG);
        else             center(s2(13 /* confirm the current PIN */), bottom - 16, 1, MUTED, BG);
        return;
    }
    // No "Impostazioni" caption: the app title sits right above, and those 22 px are worth two more
    // rows on a 135 px panel. The toast takes that space only while it is up.
    bool toast = esp_timer_get_time() < s_pin_ok_until;
    if (toast) center(TR(s_toast, s_toast_en), y0 + 2, 2, C_GREEN, BG);
    int rowh = 18, y = y0 + (toast ? 22 : 2), maxrows = (bottom - y) / rowh;
    int start = 0; if (s_set_sel >= maxrows) start = s_set_sel - maxrows + 1;
    for (int i = start; i < ASET_ROWS && i < start + maxrows; i++) {
        bool on = (i == s_set_sel);
        if (on) G.fillRoundRect(4, y, W - 12, rowh - 2, 3, ACCENT);
        G.setTextSize(2); G.setTextColor(on ? FG : MUTED, on ? ACCENT : BG); G.setCursor(10, y + 2); G.print(aset_label(i));
        const char *rv = aset_right(i);
        int rw = (int)strlen(rv) * 12; G.setTextColor(on ? C_GREEN : DIM, on ? ACCENT : BG); G.setCursor(W - 14 - rw, y + 2); G.print(rv);
        y += rowh;
    }
    // Scrollbar: 10 rows, 5 visible — without it you cannot tell where you are in the list.
    int track_y = y0 + (toast ? 22 : 2), track_h = maxrows * rowh;
    if (ASET_ROWS > maxrows && track_h > 8) {
        int kh = track_h * maxrows / ASET_ROWS; if (kh < 8) kh = 8;
        int ky = track_y + (track_h - kh) * start / (ASET_ROWS - maxrows);
        G.fillRect(W - 4, track_y, 2, track_h, LINE);
        G.fillRect(W - 4, ky, 2, kh, ACCENT);
    }
}

static void draw_companion(void);
static void draw(void)
{
    LovyanGFX &G = d;
    if (s_scr_off) return;          // panel is dark (armed/stealth): don't burn CPU compositing it
    if (nucleo_app_two_screens()) { draw_companion(); return; }
    int top = nucleo_app_content_top(), h = nucleo_app_content_height(), bottom = top + h;
    unsigned short bg = BG;
    if (s_state == ST_TRIGGERED && s_mode != MODE_SILENT) bg = s_flash ? C_RED : 0x3800;
    G.fillRect(0, top, W, h, bg);

    unsigned short acc;
    const char *rl = status_text(&acc);
    int y0 = app_ui_title(s2(14 /* Alarm */), acc, rl);
    int cy = (y0 + bottom) / 2;

    if (s_settings) { draw_settings(y0, bottom); return; }

    if (s_state == ST_DISARMED) {
        center(s2(15 /* DISARMED */), cy - 22, 3, FG, BG);
        center(s2(16 /* ENTER to arm */), cy + 6, 2, MUTED, BG);
        char ln[44];
        const char *src = (s_src != SRC_MIC && !nucleo_imu_present()) ? s2(17 /* Microphone */) : TR(SRC_NAME[s_src], SRC_NAME_EN[s_src]);
        const char *md  = (s_mode == MODE_SILENT) ? s2(18 /* silent */) : s2(19 /* siren */);
        bool toast = esp_timer_get_time() < s_pin_ok_until;
        if (toast)                 snprintf(ln, sizeof ln, "%s", TR(s_toast, s_toast_en));
        else if (s_trig_count > 0) snprintf(ln, sizeof ln, s2(20 /* %s  %d events  last %s */), md, s_trig_count, s_trig_last);
        else                       snprintf(ln, sizeof ln, s2(21 /* %s  %s  delay %ds */), src, md, DELAYS[s_delay_idx]);
        center(ln, bottom - 14, 1, toast ? C_RED : DIM, BG);
    } else if (s_state == ST_ARMING) {
        char nb[8]; snprintf(nb, sizeof nb, "%d", s_arm_shown > 0 ? s_arm_shown : 1);
        center(nb, y0 + 12, 6, C_YELLOW, BG);
        center(s2(22 /* walk away... */), bottom - 16, 2, MUTED, BG);
    } else if (s_state == ST_ARMED) {
        center(s2(23 /* ARMED */), y0 + 4, 3, C_GREEN, BG);            // 28..52
        int by = y0 + 42, bw = W - 64, bx = 32;              // first bar 66..74, label at 56
        if (motion_on()) {
            float frac = nucleo_imu_energy() / motion_e_thr(); if (frac > 1) frac = 1;
            G.setTextSize(1); G.setTextColor(MUTED, BG); G.setCursor(bx, by - 10); G.print(s2(24 /* motion */));
            G.drawRect(bx, by, bw, 8, MUTED); G.fillRect(bx + 1, by + 1, (int)((bw - 2) * frac), 6, frac > 0.8f ? C_RED : C_GREEN);
            by += 22;                                        // second bar 88..96
        }
        if (audio_on()) {
            bool dead = !s_mic;                              // open failed: the alarm would be armed but deaf
            float frac = dead ? 0 : s_audio_level / audio_thr(); if (frac > 1) frac = 1;
            G.setTextSize(1); G.setTextColor(dead ? C_RED : MUTED, BG); G.setCursor(bx, by - 10);
            G.print(dead ? "audio: mic KO" : "audio");
            G.drawRect(bx, by, bw, 8, dead ? C_RED : MUTED);
            G.fillRect(bx + 1, by + 1, (int)((bw - 2) * frac), 6, frac > 0.8f ? C_RED : C_BLUE);
        }
        if (s_bad) center(s2(25 /* Wrong PIN */), bottom - 18, 2, C_RED, BG);
        else if (s_rec) {                                    // two size-1 lines fit where one size-2 did
            char rc[40];
            rec_clock(rc, sizeof rc);
            center(rc, bottom - 18, 1, C_RED, BG);
            center(s2(26 /* type the PIN */), bottom - 9, 1, MUTED, BG);
        } else center(s2(26 /* type the PIN */), bottom - 18, 2, MUTED, BG);
    } else {   // TRIGGERED
        bool silent = (s_mode == MODE_SILENT);
        center(silent ? s2(27 /* ! DETECTED ! */) : s2(28 /* ! ALARM ! */), y0 + 10, 3, silent ? C_YELLOW : FG, bg);
        draw_pin_dots(W / 2, cy + 6, s_entry_len, FG, bg);
        if (s_bad) { center(s2(25 /* Wrong PIN */), bottom - 16, 2, FG, bg); return; }
        char ln[40];
        int rem = rearm_us() ? (int)((rearm_us() - (esp_timer_get_time() - s_trig_t0)) / 1000000) + 1 : 0;
        if (rem > 0) snprintf(ln, sizeof ln, s2(29 /* %s %s  re-arm in %ds */), silent ? s2(30 /* mute */) : s2(19 /* siren */), s_trig_last, rem);
        else         snprintf(ln, sizeof ln, s2(31 /* %s %s  PIN to stop */), silent ? s2(30 /* mute */) : s2(19 /* siren */), s_trig_last);
        center(ln, bottom - 22, 1, FG, bg);
        if (s_rec) { char rc[40]; rec_clock(rc, sizeof rc); center(rc, bottom - 12, 1, C_RED, bg); }
    }
}

// ===== two screens ==============================================================================================
// The 320x240 panel carries the whole alarm, composed in strips (lg::frame: the live meters and the siren flash
// never flicker), and goes black with the Cardputer (stealth: a tap wakes both, like a key):
//   DISARMED: a big ARM disc you tap + the setup at a glance (source, mode, delay, events); the header's menu
//             glyph opens the settings;
//   ARMING: the exit countdown as a ring that empties (tap = cancel);
//   ARMED / TRIGGERED: state, live meters and the PIN dots on the left, a PIN keypad on the right (its keys ARE
//             the PIN digits); the siren mode flashes the whole panel red;
//   SETTINGS: the rows (tap = pick, tap again = act / change), - / + steppers on the picked value row; a PIN
//             challenge brings the keypad back.
// The Cardputer shows the state and what to type. Keyboard behaviour is unchanged.
#define LW 320
#define LH 240

static void lt(const char *t, int x, int y, int f, unsigned short c)
{
    LovyanGFX &G = d;
    lg::font(f); G.setTextDatum(middle_center); G.setTextColor(c); G.drawString(t, x, y); G.setTextDatum(top_left);
}
static const char *set_label(int i, void *) { return aset_label(i); }
static const char *set_right(int i, void *) { return aset_right(i); }

// 1-9, <, 0 at x0,y0: three columns of 50x48 keys.
static void lp_keypad(int x0, int y0, bool del)
{
    LovyanGFX &G = d;
    static const char K[] = "123456789<0";
    for (int i = 0; i < 11; i++) {
        if (i == 9 && !del) continue;
        const int x = x0 + (i % 3) * 52, y = y0 + (i / 3) * 50;
        G.fillRoundRect(x, y, 48, 46, 10, LINE);
        char t[2] = { K[i], 0 };
        lt(t, x + 24, y + 23, 2, FG);
        nucleo_touch_add(x, y, 52, 50, i == 9 ? NK_BACK : NK_CHAR, i == 9 ? 0 : K[i]);
    }
}

static void lp_settings(int y0)
{
    if (s_pin_edit) {
        const bool old = s_pin_stage == PIN_OLD;
        lt(old ? s2(8 /* Current PIN */) : s2(9 /* New PIN */), 80, y0 + 40, 2, old ? C_YELLOW : FG);
        draw_pin_dots(80, y0 + 90, s_pin_buf_len, old ? C_YELLOW : C_BLUE, BG);
        if (s_pin_bad) lt(s2(10 /* Wrong PIN - retry */), 80, y0 + 140, 0, C_RED);
        lp_keypad(162, y0 + 6, true);
        return;
    }
    const int rh = 34, rows = (LH - y0) / rh;
    int sc = s_lg_scroll;                                            // the list's own scroll rule, to place the steppers
    if (s_set_sel < sc) sc = s_set_sel;
    if (s_set_sel >= sc + rows) sc = s_set_sel - rows + 1;
    if (sc > ASET_ROWS - rows) sc = ASET_ROWS - rows;
    if (sc < 0) sc = 0;
    const int ry = y0 + (s_set_sel - sc) * rh;
    const bool num = s_set_sel != R_PIN && s_set_sel != R_TEST && s_set_sel != R_LOG;
    if (num) { nucleo_touch_add(140, ry, 44, rh, NK_LEFT, 0); nucleo_touch_add(186, ry, 44, rh, NK_RIGHT, 0); }   // before the rows: they win
    lg::text_list(0, y0, LW, LH - y0, ASET_ROWS, s_set_sel, &s_lg_scroll, set_label, set_right, nullptr, nullptr, ACCENT, rh);
    if (num) for (int k = 0; k < 2; k++) {
        d.fillRoundRect(142 + k * 46, ry + 4, 40, rh - 8, 8, INK);
        lt(k ? "+" : "-", 162 + k * 46, ry + rh / 2, 1, ACCENT);
    }
}

static void lp_watch(int y0, unsigned short bg)                   // ARMED / TRIGGERED: state + meters | keypad
{
    const bool trig = s_state == ST_TRIGGERED;
    lt(trig ? (s_mode == MODE_SILENT ? s2(27 /* ! DETECTED ! */) : s2(28 /* ! ALARM ! */)) : s2(23 /* ARMED */),
       80, y0 + 22, 2, trig ? FG : C_GREEN);
    int by = y0 + 52;
    if (!trig && motion_on()) {
        float f = nucleo_imu_energy() / motion_e_thr();
        lt(s2(24 /* motion */), 80, by, 0, MUTED);
        lg::bar(12, by + 12, 136, 10, (int)(f * 100), f > 0.8f ? C_RED : C_GREEN);
        by += 36;
    }
    if (!trig && audio_on()) {
        float f = s_mic ? s_audio_level / audio_thr() : 0;
        lt(s_mic ? "audio" : "audio: mic KO", 80, by, 0, s_mic ? MUTED : C_RED);
        lg::bar(12, by + 12, 136, 10, (int)(f * 100), f > 0.8f ? C_RED : C_BLUE);
    }
    draw_pin_dots(80, 170, s_entry_len, trig ? FG : C_GREEN, bg);
    if (s_bad) lt(s2(25 /* Wrong PIN */), 80, 200, 1, trig ? FG : C_RED);
    else if (s_rec) { char rc[40]; rec_clock(rc, sizeof rc); lt(rc, 80, 200, 0, C_RED); }
    else lt(s2(26 /* type the PIN */), 80, 200, 0, trig ? FG : MUTED);
    lp_keypad(162, y0 + 6, false);
}

static void lg_paint(void *)
{
    LovyanGFX &G = d;
    if (s_scr_off) { G.fillRect(0, 0, LW, LH, 0); return; }           // stealth: the panel is dark too
    unsigned short bg = BG, acc;
    if (s_state == ST_TRIGGERED && s_mode != MODE_SILENT) bg = s_flash ? C_RED : 0x3800;
    G.fillRect(0, 0, LW, LH, bg);
    const bool open = s_state == ST_DISARMED || s_settings;
    const char *rl = status_text(&acc);
    const int y0 = lg::header(s2(14 /* Alarm */), rl, acc, open || s_state == ST_ARMING ? NK_BACK : 0,
                              s_state == ST_DISARMED && !s_settings ? NK_TAB : 0);
    if (s_settings) lp_settings(y0);
    else if (s_state == ST_DISARMED) {
        G.fillCircle(84, 136, 70, ACCENT);
        lt(s2(32 /* ARM */), 84, 126, 3, INK);
        lt(s2(15 /* DISARMED */), 84, 160, 0, INK);
        nucleo_touch_add(14, 66, 140, 140, NK_ENTER, 0);
        static const unsigned char R[3] = { R_SRC, R_MODE, R_DELAY };
        for (int i = 0; i < 3; i++) lg::kv(170, 58 + i * 30, 140, aset_label(R[i]), aset_right(R[i]), FG);
        char ln[24]; snprintf(ln, sizeof ln, "%d  %s", s_trig_count, s_trig_last);
        lg::kv(170, 148, 140, s2(33 /* events */), ln, s_trig_count ? C_RED : MUTED);
        if (esp_timer_get_time() < s_pin_ok_until) lt(TR(s_toast, s_toast_en), LW / 2, 222, 1, C_GREEN);
    } else if (s_state == ST_ARMING) {
        const int left = s_arm_shown > 0 ? s_arm_shown : 1;
        G.fillArc(LW / 2, 128, 66, 80, 0, 360, LINE);
        G.fillArc(LW / 2, 128, 66, 80, 270, 270 + 360 * left / DELAYS[s_delay_idx], C_YELLOW);
        char nb[8]; snprintf(nb, sizeof nb, "%d", left);
        lt(nb, LW / 2, 128, 3, C_YELLOW);
        lt(s2(22 /* walk away... */), LW / 2, 224, 1, MUTED);
        nucleo_touch_add(LW / 2 - 80, 48, 160, 160, NK_ENTER, 0);
    } else lp_watch(y0, bg);
    lg::field(0, 0, LW, LH);                                         // a stray tap is nothing (not an arrow)
}
static void draw_large(void)
{
    nucleo_touch_clear();
    lg::frame(lg_paint, nullptr);
    nucleo_touch_commit();
}

// The Cardputer meanwhile: the state in big letters and what to do.
static void draw_companion(void)
{
    unsigned short acc;
    status_text(&acc);
    int y = lg::comp_head(s2(34 /* ALARM */), acc);
    const bool watch = s_state == ST_ARMED || s_state == ST_TRIGGERED;
    center(s_state == ST_DISARMED ? s2(15 /* DISARMED */) : s_state == ST_ARMING ? s2(6 /* arming */)
           : s_state == ST_ARMED ? s2(23 /* ARMED */) : s2(28 /* ! ALARM ! */), y + 24, 3, acc, BG);
    lg::comp_keys(watch ? s2(26 /* type the PIN */) : s2(16 /* ENTER to arm */));
}

static void enter(void)
{
    s_state = ST_DISARMED; s_settings = false; s_pin_edit = false;
    s_entry_len = 0; s_flash = false; s_bad = false;
    // The app opens LIT: a zero deadline made the very first poll blank the panel instantly.
    s_scr_off = false; s_scr_hold_until = esp_timer_get_time() + SCR_HOLD_OPEN_US;
    s_trig_count = 0; s_trig_last[0] = 0;
    s_pin_bad = false; s_pin_ok_until = 0; s_dirty_cfg = false;
    s_settle_until = 0; s_mic_retry = 0; s_audio_level = 0;
    s_rec = NULL; s_rec_bytes = 0; s_rec_until = 0; s_rec_cap = 0;
    bool had_cfg = cfg_load();                   // PIN + options survive a reboot
    // Exploit the IMU when it's there — but never override a saved, deliberate "Microfono" choice.
    if (!had_cfg && nucleo_imu_present() && s_src == SRC_MIC) s_src = SRC_BOTH;
    nucleo_app_set_hint(s5(0 /* enter arm  tab mode/rec  key=screen */));
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    nucleo_app_request_draw();
}

// Never leave the panel dark behind us: the app owns the backlight only while it is foreground.
static void on_exit(void)
{
    cfg_save(); rec_close("app closed"); mic_stop(); voice_hold(false);
    nucleo_audio_siren_stop(); siren_vol_release();
    screen_wake(0);                          // never hand the launcher a dark panel
}

extern "C" void nucleo_register_alarm(void)
{
    static const nucleo_app_def_t app = {
        "alarm", "Allarme", "Office", "Antifurto: sirena o silenzioso, riarmo automatico",
        '!', C_RED, enter, on_key, nullptr, draw, on_exit,
        // Declarative exclusive: drop the ANIMA L1 index (~24 KB) and stop a running recording that
        // would fight for the mic. NOT NX_HTTPD/NX_DISCOVERY: the silent mode's ONLY output is the
        // alarm.trigger event reaching the browser, which needs the server up. NOT NX_VOICE either —
        // mic_start/stop already own the voice engine via nucleo_voice_suspend().
        NX_ANIMA_L1 | NX_RECORDER, nullptr, draw_large
    };
    nucleo_app_register(&app);
}

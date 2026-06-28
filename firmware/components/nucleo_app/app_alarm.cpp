// Allarme / antifurto — Tools. Arms after an exit delay, then watches the chosen SENSORS and fires a
// loud, piercing, continuous siren until the PIN is typed. Two sensor sources, combinable:
//   - Microfono: loud-noise detection (RMS peak + debounce so silence/clicks don't false-trigger).
//     Works on BOTH boards (board-aware mic HAL) — this is the ONLY source on the non-ADV Cardputer.
//   - Movimento: BMI270 shake/tilt (ADV only).
// Tabbed settings (TAB), big fonts: sorgente, sensibilita audio/movimento, ritardo, PIN, test sirena,
// auto-riarmo. ESC is blocked while armed/triggered. FOR PERSONAL USE.
#include "nucleo_app.h"
#include "app_ui.h"
#include "launcher_theme.h"
#include "nucleo_imu.h"
#include "nucleo_audio.h"
#include "nucleo_codec.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_gfx.h"

extern "C" void nucleo_voice_suspend(bool suspend);   // free the mic from the voice engine while armed

enum { ST_DISARMED, ST_ARMING, ST_ARMED, ST_TRIGGERED };
enum { SRC_MIC = 0, SRC_MOTION = 1, SRC_BOTH = 2 };

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
static char     s_pin[8] = "0000";
static bool     s_autorearm = false;
static bool     s_settings, s_pin_edit;
static int      s_set_sel;
static char     s_pin_buf[8];
static int      s_pin_buf_len;

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

static bool motion_on(void) { return (s_src == SRC_MOTION || s_src == SRC_BOTH) && nucleo_imu_present(); }
static bool audio_on(void)  { return (s_src == SRC_MIC || s_src == SRC_BOTH); }

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
#define ASET_ROWS 7
static const char *aset_label(int i)
{
    static const char *L[ASET_ROWS] = { "Sorgente", "Sens. audio", "Sens. movimento", "Ritardo armo", "PIN", "Test sirena", "Auto-riarmo" };
    return (i >= 0 && i < ASET_ROWS) ? L[i] : "";
}
static const char *aset_right(int i)
{
    static char b[16];
    switch (i) {
        case 0: if (s_src != SRC_MIC && !nucleo_imu_present()) return "Mic (no IMU)"; return SRC_NAME[s_src];
        case 1: snprintf(b, sizeof b, "%d/20", s_sens_audio); return b;
        case 2: if (!nucleo_imu_present()) return "n/d"; snprintf(b, sizeof b, "%d/20", s_sens_motion); return b;
        case 3: snprintf(b, sizeof b, "%ds", DELAYS[s_delay_idx]); return b;
        case 4: return "INVIO";
        case 5: return "INVIO";
        case 6: return s_autorearm ? "ON" : "off";
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
    switch (s_set_sel) {
        case 0: s_src = (s_src + dir + 3) % 3; break;
        case 1: s_sens_audio  += dir; if (s_sens_audio  < 1) s_sens_audio  = 1; if (s_sens_audio  > SENS_MAX) s_sens_audio  = SENS_MAX; break;
        case 2: s_sens_motion += dir; if (s_sens_motion < 1) s_sens_motion = 1; if (s_sens_motion > SENS_MAX) s_sens_motion = SENS_MAX; break;
        case 3: s_delay_idx = (s_delay_idx + dir + 4) % 4; break;
        case 6: s_autorearm = !s_autorearm; break;
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

static void disarm_to_idle(void)
{
    mic_stop(); nucleo_audio_siren_stop(); siren_vol_release();
    s_state = ST_DISARMED; s_entry_len = 0; s_flash = false; s_bad = false;
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
    s_state = ST_ARMED; nucleo_app_request_draw();
}

static void tab(void)
{
    if (s_state == ST_ARMED || s_state == ST_TRIGGERED) return;
    s_settings = !s_settings; s_set_sel = 0; s_pin_edit = false;
    nucleo_app_request_draw();
}
static bool back(int key)
{
    if (s_settings) {
        if (s_pin_edit) { s_pin_edit = false; nucleo_app_request_draw(); return true; }
        if (key == NK_LEFT) { if (s_set_sel <= 3 || s_set_sel == 6) set_change(-1); return true; }
        s_settings = false; nucleo_app_request_draw(); return true;
    }
    if (s_state == ST_ARMING) { disarm_to_idle(); return true; }
    if (s_state == ST_ARMED || s_state == ST_TRIGGERED) return true;   // block ESC — PIN required
    return false;
}

static bool poll(void)
{
    if (s_settings) return false;
    int64_t now = esp_timer_get_time();

    if (s_state == ST_ARMING) {
        if (motion_on()) nucleo_imu_sample();
        int rem = DELAYS[s_delay_idx] - (int)((now - s_arm_t0) / 1000000);
        if (rem <= 0) { go_armed(); return true; }
        if (rem != s_arm_shown) { s_arm_shown = rem; nucleo_app_request_draw(); }
        return false;
    }
    if (s_state == ST_ARMED) {
        bool trig = false;
        if (motion_on()) {
            nucleo_imu_sample();
            float lx = 0, ly = 0, deg = 0; nucleo_imu_level(&lx, &ly, &deg);
            float e = nucleo_imu_energy();
            float td = sqrtf((lx - s_ref_lx) * (lx - s_ref_lx) + (ly - s_ref_ly) * (ly - s_ref_ly));
            if (e > motion_e_thr() || td > motion_t_thr()) trig = true;
        }
        if (!trig && audio_on() && mic_loud()) trig = true;
        if (trig) {
            mic_stop();                                  // free the I2S for the siren
            siren_vol_grab();
            s_state = ST_TRIGGERED; s_last_siren = 0; s_entry_len = 0; s_bad = false;
            nucleo_app_request_draw();
        } else if (now - s_last_armed_draw > 120000) {   // throttle live-bar redraw to ~8 fps -> no flicker
            s_last_armed_draw = now; nucleo_app_request_draw();
        }
        return false;
    }
    if (s_state == ST_TRIGGERED) {
        s_siren_hi = !s_siren_hi; s_flash = !s_flash;
        nucleo_app_request_draw();
        nucleo_audio_siren(150);                                 // continuous, gapless, piercing wail (I2S stays open)
        return false;
    }
    return false;
}

static void on_key(int key, char ch)
{
    if (s_settings) {
        if (s_pin_edit) {
            if (ch >= '0' && ch <= '9' && s_pin_buf_len < 4) {
                s_pin_buf[s_pin_buf_len++] = ch;
                if (s_pin_buf_len == 4) { s_pin_buf[4] = 0; strcpy(s_pin, s_pin_buf); s_pin_edit = false; }
                nucleo_app_request_draw();
            } else if (key == NK_ENTER) { s_pin_edit = false; nucleo_app_request_draw(); }
            return;
        }
        if (key == NK_UP)        { s_set_sel = (s_set_sel + ASET_ROWS - 1) % ASET_ROWS; nucleo_app_request_draw(); }
        else if (key == NK_DOWN) { s_set_sel = (s_set_sel + 1) % ASET_ROWS; nucleo_app_request_draw(); }
        else if (key == NK_RIGHT || key == NK_ENTER) {
            if (s_set_sel == 4) { s_pin_edit = true; s_pin_buf_len = 0; nucleo_app_request_draw(); }
            else if (s_set_sel == 5) siren_test();
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
                if (strcmp(s_entry, s_pin) == 0) { bool re = s_autorearm; disarm_to_idle(); if (re) start_arming(); }
                else { s_bad = true; s_entry_len = 0; }
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
        center("Nuovo PIN", y0 + 14, 2, FG, BG);
        draw_pin_dots((y0 + bottom) / 2, s_pin_buf_len, C_BLUE, BG);
        center("4 cifre  -  ESC annulla", bottom - 16, 1, MUTED, BG);
        return;
    }
    center("Impostazioni", y0 + 4, 2, C_BLUE, BG);
    int rowh = 18, y = y0 + 26, maxrows = (bottom - y) / rowh;
    int start = 0; if (s_set_sel >= maxrows) start = s_set_sel - maxrows + 1;
    for (int i = start; i < ASET_ROWS && i < start + maxrows; i++) {
        bool on = (i == s_set_sel);
        if (on) d.fillRoundRect(4, y, W - 8, rowh - 2, 3, 0x12B2);
        d.setTextSize(2); d.setTextColor(on ? FG : MUTED, on ? 0x12B2 : BG); d.setCursor(10, y + 2); d.print(aset_label(i));
        const char *rv = aset_right(i);
        int rw = (int)strlen(rv) * 12; d.setTextColor(on ? C_GREEN : DIM, on ? 0x12B2 : BG); d.setCursor(W - 10 - rw, y + 2); d.print(rv);
        y += rowh;
    }
}

static void draw(void)
{
    int top = nucleo_app_content_top(), h = nucleo_app_content_height(), bottom = top + h;
    unsigned short bg = BG;
    if (s_state == ST_TRIGGERED) bg = s_flash ? C_RED : 0x3800;
    d.fillRect(0, top, W, h, bg);

    const char *rl = "off"; unsigned short acc = MUTED;
    if (s_state == ST_ARMING)         { rl = "armo"; acc = C_YELLOW; }
    else if (s_state == ST_ARMED)     { rl = "ON";   acc = C_GREEN;  }
    else if (s_state == ST_TRIGGERED) { rl = "!!!";  acc = C_RED;    }
    int y0 = app_ui_title("Allarme", acc, rl);
    int cy = (y0 + bottom) / 2;

    if (s_settings) { draw_settings(y0, bottom); return; }

    if (s_state == ST_DISARMED) {
        center("DISARMATO", cy - 22, 3, FG, BG);
        center("INVIO per armare", cy + 6, 2, MUTED, BG);
        char ln[44];
        const char *src = (s_src != SRC_MIC && !nucleo_imu_present()) ? "Microfono" : SRC_NAME[s_src];
        snprintf(ln, sizeof ln, "%s   ritardo %ds", src, DELAYS[s_delay_idx]);
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
        center("! ALLARME !", y0 + 10, 3, FG, bg);
        draw_pin_dots(cy + 6, s_entry_len, FG, bg);
        center(s_bad ? "PIN errato" : "PIN per fermare", bottom - 16, 2, FG, bg);
    }
}

static void enter(void)
{
    s_state = ST_DISARMED; s_settings = false; s_pin_edit = false;
    s_entry_len = 0; s_flash = false; s_bad = false;
    if (nucleo_imu_present() && s_src == SRC_MIC) s_src = SRC_BOTH;   // exploit the IMU when it's there
    nucleo_app_set_hint("INVIO arma   TAB impostazioni   esc esci");
    nucleo_app_set_poll_handler(poll);
    nucleo_app_set_tab_handler(tab);
    nucleo_app_set_back_handler(back);
    nucleo_app_request_draw();
}

static void on_exit(void) { mic_stop(); nucleo_audio_siren_stop(); siren_vol_release(); }

extern "C" void nucleo_register_alarm(void)
{
    static const nucleo_app_def_t app = {
        "alarm", "Allarme", "Tools", "Antifurto: microfono + movimento, sirena",
        '!', C_RED, enter, on_key, nullptr, draw, on_exit, 0
    };
    nucleo_app_register(&app);
}

// Host implementations of everything app_alarm.cpp calls into: simulated clock, fake mic, fake IMU,
// fake panel, and an SD sandbox. The harness (test_alarm.cpp) drives these to script a scenario.
#include <stdint.h>
#include "stubs/nucleo_app.h"
#include "stubs/launcher_theme.h"
#include "stubs/app_ui.h"
#include "stubs/app_gfx.h"
#include "stubs/nucleo_imu.h"
#include "stubs/nucleo_audio.h"
#include "stubs/nucleo_codec.h"
#include "stubs/nucleo_ui.h"
#include "stubs/nucleo_power.h"
#include "stubs/nucleo_i18n.h"
#include "host_stubs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <direct.h>

// ---- simulated time --------------------------------------------------------
long long g_now_us = 1000000;             // never start at 0: the app compares deadlines against it
extern "C" int64_t esp_timer_get_time(void) { return (int64_t)g_now_us; }
extern "C" void esp_task_wdt_reset(void) {}

// ---- captured app registration + handlers ----------------------------------
nucleo_app_def_t g_app;
bool (*g_poll)(void);
void (*g_tab)(void);
bool (*g_back)(int);
const char *g_hint = "";

extern "C" void nucleo_app_register(const nucleo_app_def_t *a) { g_app = *a; }
extern "C" void nucleo_app_set_hint(const char *h) { g_hint = h; }
extern "C" int  nucleo_app_content_top(void) { return 0; }
extern "C" int  nucleo_app_content_height(void) { return H - HINT; }   // 121, as on the device
extern "C" void nucleo_app_set_tab_handler(void (*fn)(void)) { g_tab = fn; }
extern "C" void nucleo_app_set_back_handler(bool (*fn)(int)) { g_back = fn; }
extern "C" void nucleo_app_set_poll_handler(bool (*fn)(void)) { g_poll = fn; }

int  g_draw_req;
int  g_repaints;
extern "C" void nucleo_app_request_draw(void) { g_draw_req++; }
extern "C" void nucleo_app_force_repaint(void) { g_repaints++; }

// ---- backlight -------------------------------------------------------------
int g_bright_stored = 80;      // what nucleo_app_set_brightness holds (floored at 10, as on device)
int g_panel = 80;              // what the panel actually shows: 0 = really dark
extern "C" void nucleo_app_set_brightness(int pct)
{
    if (pct < 10) pct = 10;
    if (pct > 100) pct = 100;
    g_bright_stored = pct;
    g_panel = pct;
}
extern "C" int  nucleo_app_brightness(void) { return g_bright_stored; }
extern "C" void nucleo_ui_set_brightness(unsigned char b) { g_panel = b; }

// ---- IMU -------------------------------------------------------------------
bool  g_imu = false;
float g_imu_energy = 0, g_imu_lx = 0, g_imu_ly = 0;
int   g_imu_samples;
extern "C" bool  nucleo_imu_present(void) { return g_imu; }
extern "C" void  nucleo_imu_sample(void) { g_imu_samples++; }
extern "C" void  nucleo_imu_level(float *lx, float *ly, float *deg) { *lx = g_imu_lx; *ly = g_imu_ly; *deg = 0; }
extern "C" float nucleo_imu_energy(void) { return g_imu_energy; }

// ---- audio out -------------------------------------------------------------
int g_volume = 40, g_siren_calls, g_siren_stops;
extern "C" int  nucleo_audio_volume(void) { return g_volume; }
extern "C" void nucleo_audio_set_volume(int v) { g_volume = v; }
extern "C" void nucleo_audio_siren(int ms) { g_siren_calls++; g_now_us += (long long)ms * 1000; }
extern "C" void nucleo_audio_siren_stop(void) { g_siren_stops++; }

// ---- mic -------------------------------------------------------------------
// The fake mic hands out one 20 ms frame per simulated 20 ms, filled with a peak the test chooses,
// so both the detector thresholds and the recorder byte-count behave like the real thing.
bool  g_mic_open_ok = true;      // flip to false to simulate the voice engine holding the I2S
int   g_mic_level_pct = 0;       // 0..100 peak of the generated frame
bool  g_mic_powered, g_mic_is_open;
int   g_mic_opens, g_mic_closes;
int   g_voice_suspended;         // +1 suspend / -1 resume
static long long s_next_frame_us;

extern "C" void nucleo_codec_mic(bool on) { g_mic_powered = on; }
extern "C" esp_err_t nucleo_codec_mic_open(int rate, i2s_chan_handle_t *out)
{
    (void)rate;
    if (!g_mic_open_ok) { *out = NULL; return 1; }
    g_mic_opens++; g_mic_is_open = true;
    s_next_frame_us = g_now_us;
    *out = (i2s_chan_handle_t)0x1;
    return ESP_OK;
}
extern "C" void nucleo_codec_mic_close(i2s_chan_handle_t h) { (void)h; g_mic_closes++; g_mic_is_open = false; }
extern "C" esp_err_t nucleo_codec_mic_read(i2s_chan_handle_t h, void *buf, size_t sz, size_t *got, int to)
{
    (void)h; (void)to;
    if (!g_mic_is_open || g_now_us < s_next_frame_us) { *got = 0; return 1; }
    s_next_frame_us += 20000;                       // one 320-sample frame per 20 ms
    short *p = (short *)buf;
    size_t n = sz / 2;
    short peak = (short)(32767 * g_mic_level_pct / 100);
    for (size_t i = 0; i < n; i++) p[i] = (i % 2) ? peak : (short)-peak;
    *got = n * 2;
    return ESP_OK;
}
extern "C" void nucleo_voice_suspend(bool s) { g_voice_suspended += s ? 1 : -1; }

// ---- misc ------------------------------------------------------------------
extern "C" bool nucleo_power_battery_available(void) { return true; }
extern "C" int  nucleo_power_battery_pct(void) { return 77; }
const char *nucleo_tr(const char *it, const char *en) { (void)en; return it; }

char g_events[64][160];
int  g_event_n;
extern "C" unsigned int nucleo_event_publish(const char *topic, const char *payload)
{
    if (g_event_n < 64) snprintf(g_events[g_event_n++], 160, "%s %s", topic, payload);
    return (unsigned)g_event_n;
}

// ---- fake panel: record text boxes so layout can be asserted ----------------
HostGfx  g_gfx;
g_box_t  g_text[MAXG];
char     g_txt[MAXG][48];
int      g_texts;

void HostGfx::print(const char *t)
{
    if (g_texts < MAXG) {
        g_text[g_texts].x = cx;
        g_text[g_texts].y = cy;
        g_text[g_texts].w = (int)strlen(t) * 6 * size;
        g_text[g_texts].h = 8 * size;
        snprintf(g_txt[g_texts], 48, "%s", t);
        g_texts++;
    }
}
void HostGfx::fillRect(int, int, int, int, unsigned short) {}
void HostGfx::drawRect(int, int, int, int, unsigned short) {}
void HostGfx::fillRoundRect(int, int, int, int, int, unsigned short) {}
void HostGfx::fillCircle(int, int, int, unsigned short) {}
void HostGfx::drawCircle(int, int, int, unsigned short) {}
void HostGfx::drawFastHLine(int, int, int, unsigned short) {}
void HostGfx::fillScreen(unsigned short) {}

int app_ui_title(const char *text, unsigned short accent, const char *right)
{
    (void)accent;
    int top = nucleo_app_content_top();
    g_gfx.setTextSize(2); g_gfx.setCursor(10, top + 2); g_gfx.print(text);
    if (right && right[0]) { g_gfx.setTextSize(1); g_gfx.setCursor(238 - (int)strlen(right) * 6, top + 7); g_gfx.print(right); }
    return top + 24;
}

// ---- SD sandbox ------------------------------------------------------------
static char s_root[512];
void host_sd_root(const char *dir) { snprintf(s_root, sizeof s_root, "%s", dir); }
static const char *mapp(const char *p)
{
    static char buf[700];
    snprintf(buf, sizeof buf, "%s%s", s_root, p);
    return buf;
}
extern "C" FILE *host_fopen(const char *p, const char *m) { return fopen(mapp(p), m); }
extern "C" int   host_mkdir(const char *p, int mode) { (void)mode; return _mkdir(mapp(p)); }
extern "C" int   host_stat(const char *p, struct stat *st) { return stat(mapp(p), st); }
extern "C" int   host_remove(const char *p) { return remove(mapp(p)); }
extern "C" int   host_rename(const char *a, const char *b)
{
    char first[700];
    snprintf(first, sizeof first, "%s", mapp(a));
    return rename(first, mapp(b));
}

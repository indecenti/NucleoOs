// Handles the harness uses to script a scenario and inspect what the app did.
#pragma once
#include "stubs/nucleo_app.h"
#include <stdbool.h>

extern long long g_now_us;                 // simulated clock (µs)
extern nucleo_app_def_t g_app;             // captured registration
extern bool (*g_poll)(void);
extern void (*g_tab)(void);
extern bool (*g_back)(int);
extern const char *g_hint;

extern int  g_draw_req, g_repaints;
extern int  g_bright_stored, g_panel;      // g_panel == 0 -> the screen is really off

extern bool  g_imu;                        // IMU present?
extern float g_imu_energy, g_imu_lx, g_imu_ly;
extern int   g_imu_samples;

extern int g_volume, g_siren_calls, g_siren_stops;

extern bool g_mic_open_ok;                 // false -> mic_open fails (voice engine holds the I2S)
extern int  g_mic_level_pct;               // peak of the generated frames, 0..100
extern bool g_mic_powered, g_mic_is_open;
extern int  g_mic_opens, g_mic_closes, g_voice_suspended;

extern char g_events[64][160];
extern int  g_event_n;

void host_sd_root(const char *dir);

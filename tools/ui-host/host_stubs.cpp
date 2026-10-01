// ui-host: host implementations of everything the REAL launcher / UI sources link against. The
// display and the shared back-buffer mirror nucleo_ui.cpp (same sizes, same 8bpp canvas), so the
// buffered and the direct-draw paths both run exactly as on the device.
#include <M5GFX.h>
#include "host_state.h"
#include "nucleo_app.h"
#include "nucleo_kbd.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

HostState g_host;

// ---- display + shared back-buffer (mirror of nucleo_ui.cpp) -----------------------------------
M5GFX d;
static LovyanGFX *s_gfx_target = &d;
LovyanGFX *nucleo_app_gfx(void)         { return s_gfx_target; }
bool       nucleo_app_is_buffered(void) { return s_gfx_target != &d; }
void       nucleo_app_set_gfx(LovyanGFX *g) { s_gfx_target = g ? g : &d; }
static M5Canvas s_screen(&d);
static bool s_screen_alive = false;
bool nucleo_screen_acquire(void)
{
    if (s_screen_alive) return true;
    if (!g_host.canvas_ok) return false;
    s_screen.setColorDepth(8);
    s_screen_alive = s_screen.createSprite(240, g_host.canvas_rows) != nullptr;
    return s_screen_alive;
}
void nucleo_screen_release(void) { if (s_screen_alive) { s_screen.deleteSprite(); s_screen_alive = false; } }
M5Canvas *nucleo_screen(void) { if (!g_host.canvas_ok) nucleo_screen_release(); return nucleo_screen_acquire() ? &s_screen : nullptr; }
void host_display_begin(void) { d.setColorDepth(16); d.createSprite(240, 135); d.fillScreen(0); }

extern "C" bool nucleo_ui_is_adv(void) { return g_host.adv; }
extern "C" void nucleo_ui_set_brightness(unsigned char b) { d.setBrightness(b); }

// ---- clocks ------------------------------------------------------------------------------------
extern "C" int64_t esp_timer_get_time(void) { return g_host.now_us; }
extern "C" time_t host_time(time_t *out) { if (out) *out = g_host.wall; return g_host.wall; }
extern "C" esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
extern "C" void esp_restart(void) {}
extern "C" uint32_t esp_get_free_heap_size(void) { return 17000; }
extern "C" size_t heap_caps_get_largest_free_block(unsigned) { return g_host.largest_block ? g_host.largest_block : g_host.canvas_ok ? 40000 : 12000; }
extern "C" size_t heap_caps_get_free_size(unsigned) { return 30000; }
extern "C" size_t heap_caps_get_minimum_free_size(unsigned) { return 9000; }
extern "C" const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t a = { "0.4.0+host", "nucleoos", "09:00:00", "Sep 29 2026", "v5.4-host" };
    return &a;
}
// Settings' "wifi" worker runs inline (scan / join / finish complete before the next tick); others stay inert.
extern "C" BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t, void *arg, UBaseType_t, TaskHandle_t *out)
{
    if (out) *out = nullptr;
    if (fn && name && !strcmp(name, "wifi")) fn(arg);
    return pdPASS;
}
extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *out, BaseType_t) { if (out) *out = nullptr; return pdPASS; }
extern "C" void vTaskDelete(TaskHandle_t) {}
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }
extern "C" void vTaskDelay(TickType_t) {}
extern "C" TickType_t xTaskGetTickCount(void) { return (TickType_t)(g_host.now_us / 1000); }

// ---- network / security / power / audio (read-only status the chrome shows) -------------------
extern "C" const char *nucleo_setup_mode(void) { return g_host.wifi_mode; }
extern "C" const char *nucleo_setup_ssid(void) { return g_host.ssid; }
extern "C" const char *nucleo_setup_ip(void)   { return g_host.ip; }
extern "C" const char *nucleo_auth_pin(void)   { return g_host.pin; }
extern "C" int  nucleo_setup_rssi(void)        { return g_host.rssi; }
extern "C" bool nucleo_setup_time_synced(void) { return g_host.time_synced; }
extern "C" bool nucleo_setup_ap_intended(void) { return g_host.ap_intended; }
extern "C" bool nucleo_setup_ap_active(void)   { return g_host.ap_active; }
extern "C" const char *nucleo_setup_ap_ssid(void) { return "NucleoOS-3F2A"; }
extern "C" void nucleo_setup_start_ap(void)    { g_host.ap_active = true; }
extern "C" void nucleo_setup_stop_ap(void)     { g_host.ap_active = false; }
extern "C" bool nucleo_setup_config_loaded(void) { return g_host.cfg_loaded; }
extern "C" bool nucleo_evilportal_running(void) { return false; }
extern "C" int  nucleo_evilportal_captures(void) { return 0; }
extern "C" bool nucleo_wifiatk_deauth_running(void) { return false; }
extern "C" unsigned long nucleo_wifiatk_frames(void) { return 0; }
extern "C" bool nucleo_wifiatk_beacon_running(void) { return false; }
extern "C" int  nucleo_wifiatk_beacon_count(void) { return 0; }
extern "C" int  nucleo_power_battery_pct(void) { return g_host.battery; }
extern "C" void nucleo_app_set_brightness(int pct) { if (pct < 10) pct = 10; if (pct > 100) pct = 100; g_host.brightness = pct; }
extern "C" int  nucleo_app_brightness(void) { return g_host.brightness; }
extern "C" void nucleo_app_persist_prefs(void) {}
extern "C" int  nucleo_audio_volume(void) { return g_host.volume; }
extern "C" void nucleo_audio_set_volume(int pct) { g_host.volume = pct < 0 ? 0 : pct > 100 ? 100 : pct; }
extern "C" bool nucleo_audio_is_muted(void) { return g_host.muted; }
extern "C" void nucleo_audio_set_mute(bool m) { g_host.muted = m; }
extern "C" bool nucleo_audio_is_playing(void) { return g_host.playing; }
extern "C" bool nucleo_audio_is_paused(void) { return false; }
extern "C" const char *nucleo_audio_path(void) { return g_host.track; }

// ---- app framework surface used by app_ui / launcher ----------------------------------------------
void launcher_render_set_hint(const char *h);
extern "C" void nucleo_app_set_hint(const char *h) { launcher_render_set_hint(h ? h : ""); }
extern "C" int  nucleo_app_content_top(void) { return 0; }                 // as nucleo_app.cpp
extern "C" int  nucleo_app_content_height(void) { return 135 - 14; }       // H - HINT (not fullscreen)
extern "C" void nucleo_app_request_draw(void) {}
extern "C" void nucleo_app_force_repaint(void) {}
// Apps get their keys from the scene (app_key); only the blocking modals poll. They read the scripted
// keys, then 40 idle polls (a menu's glide settles), then ENTER, which ends every modal.
extern "C" nucleo_key_t nucleo_kbd_read(void)
{
    nucleo_key_t k; memset(&k, 0, sizeof k);
    if (g_host.kbd_i < 0) return k;
    if (g_host.kbd_i < g_host.kbd_n) return g_host.kbd[g_host.kbd_i++];
    if (++g_host.kbd_i > g_host.kbd_n + 40) { k.key = NK_ENTER; g_host.kbd_i = -1; }
    return k;
}
extern "C" unsigned char nucleo_kbd_mods(void) { return 0; }
extern "C" bool nucleo_kbd_char_down(char) { return false; }

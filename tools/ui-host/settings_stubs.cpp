// ui-host: the backends the native Settings app (app_wifi.cpp) reads and writes, as a small simulated
// device: two saved networks, a Wi-Fi scan with realistic neighbours, hotspot, voice pack, BLE, web
// sessions, screensaver, ANIMA mode, SD card. Setters just update the state so a scene can act on it.
#include "host_state.h"
#include "nucleo_storage.h"
#include <string.h>
#include <stdio.h>

struct Net { const char *ssid; int rssi, ch, secure; const char *auth; };
static const Net SCAN[] = {
    { "Casa-Rossi",        -52, 6,  1, "WPA2" },
    { "Casa-Rossi_EXT",    -71, 11, 1, "WPA2" },
    { "Vodafone-A1B2C3",   -78, 1,  1, "WPA2/3" },
    { "FRITZ!Box 7530 KL", -83, 36, 1, "WPA3" },
    { "Bar Centrale Free", -88, 1,  0, "open" },
};
static const int SCAN_N = sizeof SCAN / sizeof SCAN[0];
static char s_saved[4][33] = { "Casa-Rossi", "Ufficio-5G" };
static int  s_saved_n = 2;
static char s_devname[33] = "cardputer-rossi";
static char s_ap_ssid[33] = "NucleoOS-3F2A";
static char s_ap_pass[65] = "nucleo-3f2a";
static bool s_tts = true, s_ble = false, s_voice = false, s_remote = true;
static int  s_tts_speed = 100, s_saver_s = 120, s_saver_mode = 1, s_anima_mode = 1, s_sessions = 2;

extern "C" {
int         nucleo_setup_channel(void) { return 6; }
const char *nucleo_setup_device_name(void) { return s_devname; }
void        nucleo_setup_set_device_name(const char *n) { snprintf(s_devname, sizeof s_devname, "%s", n); }
void        nucleo_setup_set_datetime(int y, int mo, int d, int h, int mi)
{
    struct tm t = {}; t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi;
    g_host.wall = _mkgmtime(&t); g_host.time_synced = true;
}
int         nucleo_setup_scan(void) { return SCAN_N; }
int         nucleo_setup_scan_count(void) { return SCAN_N; }
const char *nucleo_setup_scan_ssid(int i) { return i >= 0 && i < SCAN_N ? SCAN[i].ssid : ""; }
int         nucleo_setup_scan_rssi(int i) { return i >= 0 && i < SCAN_N ? SCAN[i].rssi : -100; }
int         nucleo_setup_scan_channel(int i) { return i >= 0 && i < SCAN_N ? SCAN[i].ch : 0; }
int         nucleo_setup_scan_secure(int i) { return i >= 0 && i < SCAN_N ? SCAN[i].secure : 0; }
const char *nucleo_setup_scan_auth_label(int i) { return i >= 0 && i < SCAN_N ? SCAN[i].auth : ""; }
bool        nucleo_setup_join(const char *, const char *) { return true; }
void        nucleo_setup_forget(void) { s_saved_n = 0; }
bool        nucleo_setup_factory_reset(void) { s_saved_n = 0; snprintf(s_devname, sizeof s_devname, "nucleo-01"); return true; }
bool        nucleo_setup_net_is_known(const char *ssid) { for (int i = 0; i < s_saved_n; i++) if (!strcmp(s_saved[i], ssid)) return true; return false; }
bool        nucleo_setup_net_has_password(const char *ssid) { return nucleo_setup_net_is_known(ssid); }
void        nucleo_setup_forget_ssid(const char *ssid)
{
    for (int i = 0; i < s_saved_n; i++)
        if (!strcmp(s_saved[i], ssid)) { for (int j = i; j < s_saved_n - 1; j++) memcpy(s_saved[j], s_saved[j + 1], 33); s_saved_n--; return; }
}
int         nucleo_setup_net_count(void) { return s_saved_n; }
const char *nucleo_setup_net_ssid(int i) { return i >= 0 && i < s_saved_n ? s_saved[i] : ""; }
int         nucleo_setup_net_priority(int i) { return i == 0 ? 10 : 0; }
void        nucleo_setup_net_set_priority(const char *, int) {}
const char *nucleo_setup_ap_pass(void) { return s_ap_pass; }
bool        nucleo_setup_ap_secure(void) { return true; }
bool        nucleo_setup_ap_rescue(void) { return false; }
void        nucleo_setup_set_ap_ssid(const char *s) { snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s", s); }
void        nucleo_setup_set_ap_pass(const char *p) { snprintf(s_ap_pass, sizeof s_ap_pass, "%s", p); }
int         nucleo_auth_revoke(const char *) { int n = s_sessions; s_sessions = 0; return n; }
int         nucleo_auth_session_count(void) { return s_sessions; }
bool        nucleo_auth_factory_reset(void) { s_sessions = 0; return true; }
bool        nucleo_mailcfg_erase_all(void) { return true; }
bool        nucleo_keydeck_forget(void) { return true; }
bool        nucleo_remote_enabled(void) { return s_remote; }
void        nucleo_remote_set_enabled(bool on) { s_remote = on; }
bool        nucleo_tts_available(void) { return true; }
bool        nucleo_tts_enabled(void) { return s_tts; }
void        nucleo_tts_set_enabled(bool on) { s_tts = on; }
int         nucleo_tts_speed_clamp(int p) { return p < 70 ? 70 : p > 160 ? 160 : p; }
int         nucleo_tts_speed(void) { return s_tts_speed; }
void        nucleo_tts_set_speed(int p) { s_tts_speed = nucleo_tts_speed_clamp(p); }
bool        nucleo_ble_radio_present(void) { return true; }
bool        nucleo_ble_pref_enabled(void) { return s_ble; }
void        nucleo_ble_set_pref(bool on) { s_ble = on; }
int         nucleo_screensaver_timeout_s(void) { return s_saver_s; }
void        nucleo_screensaver_set_timeout_s(int s) { s_saver_s = s; }
int         nucleo_screensaver_mode(void) { return s_saver_mode; }
void        nucleo_screensaver_set_mode(int m) { s_saver_mode = m; }
int         nucleo_anima_ui_online_mode(void) { return s_anima_mode; }
void        nucleo_anima_ui_set_online_mode(int m) { s_anima_mode = m; }
int         nucleo_power_battery_mv(void) { return 3890; }
bool        nucleo_voice_always_on(void) { return s_voice; }
void        nucleo_voice_set_always_on(bool on) { s_voice = on; }
const nucleo_storage_info_t *nucleo_storage_info(void)
{
    static nucleo_storage_info_t si = { true, "FAT32", 31914983424ULL, 27020730368ULL, 0 };
    return &si;
}
}

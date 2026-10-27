// ui-host stub for app_wifiatk.cpp (Deauth Flood) — a simulated Wi-Fi offensive engine: a scan that finds a
// handful of nearby APs (so the target LIST renders) and, once armed, a running flood with live counters (so
// the RUNNING dashboard renders). FICTIONAL SSIDs / BSSIDs only — never real networks. Authorized-test tool.
#include <string.h>
#include <stdio.h>

extern "C" {

struct AP { const char *ssid; int ch; int rssi; const char *auth; bool prot; };
static const AP APS[] = {
    { "Caffe_Centrale",  1, -46, "open", false },
    { "FRITZ!Box 7530",  6, -58, "WPA2", false },
    { "TIM-19284756",   11, -63, "WPA2", false },
    { "Vodafone-7788",   6, -71, "WPA3", true  },   // PMF / immune
    { "Ufficio_2G",      1, -74, "WPA2", false },
    { "IoT_Lab",         9, -80, "WPA2", false },
};
static const int NAP = (int)(sizeof(APS) / sizeof(APS[0]));
static bool g_running = false;

int         nucleo_wifiatk_scan(void)             { return NAP; }
int         nucleo_wifiatk_target_count(void)     { return NAP; }
const char *nucleo_wifiatk_target_ssid(int i)     { return (i >= 0 && i < NAP) ? APS[i].ssid : ""; }
int         nucleo_wifiatk_target_channel(int i)  { return (i >= 0 && i < NAP) ? APS[i].ch : 0; }
int         nucleo_wifiatk_target_rssi(int i)     { return (i >= 0 && i < NAP) ? APS[i].rssi : 0; }
const char *nucleo_wifiatk_target_auth(int i)     { return (i >= 0 && i < NAP) ? APS[i].auth : ""; }
bool        nucleo_wifiatk_target_protected(int i){ return (i >= 0 && i < NAP) ? APS[i].prot : false; }
const char *nucleo_wifiatk_target_bssid(int i)    { static char b[18]; snprintf(b, sizeof b, "DE:AD:00:00:00:%02X", i & 0xFF); return b; }
int         nucleo_wifiatk_targets_vulnerable(void){ int n = 0; for (auto &a : APS) if (!a.prot) n++; return n; }

int         nucleo_wifiatk_deauth_start(int)      { g_running = true; return 0; }
void        nucleo_wifiatk_deauth_stop(void)      { g_running = false; }
bool        nucleo_wifiatk_deauth_running(void)   { return g_running; }

unsigned long nucleo_wifiatk_frames(void)         { return 48213; }
int         nucleo_wifiatk_clients(void)          { return 7; }
int         nucleo_wifiatk_clients_active(void)   { return 2; }
int         nucleo_wifiatk_inject_health(void)    { return 86; }
unsigned    nucleo_wifiatk_reconnects(void)       { return 11; }
int         nucleo_wifiatk_cur_rssi(void)         { return -58; }
int         nucleo_wifiatk_targets_active(void)   { return 1; }
int         nucleo_wifiatk_cur_channel(void)      { return 6; }
const char *nucleo_wifiatk_cur_ssid(void)         { return "FRITZ!Box 7530"; }
unsigned    nucleo_wifiatk_uptime_s(void)         { return 95; }
int         nucleo_wifiatk_handshake_aps(void)    { return 1; }
int         nucleo_wifiatk_handshake_count(void)  { return 1; }
int         nucleo_wifiatk_handshake_pmkidn(void) { return 0; }
bool        nucleo_wifiatk_handshake_ready(void)  { return true; }
bool        nucleo_wifiatk_handshake_pmkid(void)  { return false; }

}  // extern "C"

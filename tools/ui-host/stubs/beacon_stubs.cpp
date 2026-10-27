// ui-host stub for app_beacon.cpp (Beacon Spam) — a simulated beacon-flood engine: once armed it reports a
// running flood with live frame / SSID counters + injection health, and a working custom-SSID list so the
// CUSTOM screen renders what you type. FICTIONAL numbers. Authorized-test tool.
#include <string.h>
#include <stdio.h>

extern "C" {

static bool g_running = false;

int           nucleo_wifiatk_beacon_start(int) { g_running = true; return 0; }
void          nucleo_wifiatk_beacon_stop(void) { g_running = false; }
bool          nucleo_wifiatk_beacon_running(void) { return g_running; }
unsigned long nucleo_wifiatk_beacon_frames(void)  { return 132450; }
int           nucleo_wifiatk_beacon_count(void)   { return 64; }
int           nucleo_wifiatk_beacon_mode(void)    { return 0; }   // Funny
int           nucleo_wifiatk_beacon_health(void)  { return 92; }
unsigned      nucleo_wifiatk_beacon_uptime_s(void){ return 73; }

// Custom SSID list (what the operator types on the CUSTOM screen).
static char  g_custom[16][33];
static int   g_custom_n = 0;
void          nucleo_wifiatk_beacon_custom_clear(void) { g_custom_n = 0; }
bool          nucleo_wifiatk_beacon_custom_add(const char *ssid)
{
    if (!ssid || !ssid[0] || g_custom_n >= 16) return false;
    snprintf(g_custom[g_custom_n++], 33, "%s", ssid);
    return true;
}
int           nucleo_wifiatk_beacon_custom_count(void) { return g_custom_n; }
const char   *nucleo_wifiatk_beacon_custom_ssid(int i)  { return (i >= 0 && i < g_custom_n) ? g_custom[i] : ""; }

}  // extern "C"

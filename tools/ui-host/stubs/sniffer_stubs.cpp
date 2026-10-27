// ui-host stub for app_sniffer.cpp (WiFi Sniffer) — a simulated promiscuous capture: once started it reports
// a running capture with live frame counters + a .pcap path, so the RUNNING dashboard renders. FICTIONAL
// counts. Authorized-test tool.
#include <stdio.h>

extern "C" {

static bool g_running = false;

int  nucleo_wifiatk_sniffer_start(int, int) { g_running = true; return 0; }
void nucleo_wifiatk_sniffer_stop(void)      { g_running = false; }
bool nucleo_wifiatk_sniffer_running(void)   { return g_running; }

unsigned    nucleo_wifiatk_sniffer_pkts(void)    { return 5137; }
unsigned    nucleo_wifiatk_sniffer_drops(void)   { return 18; }
int         nucleo_wifiatk_sniffer_channel(void) { return 6; }
const char *nucleo_wifiatk_sniffer_path(void)    { return "/sd/sniffer/capture-006.pcap"; }
unsigned    nucleo_wifiatk_sniffer_beacons(void) { return 2841; }
unsigned    nucleo_wifiatk_sniffer_probes(void)  { return 612; }
unsigned    nucleo_wifiatk_sniffer_data(void)    { return 1588; }
unsigned    nucleo_wifiatk_sniffer_eapol(void)   { return 4; }
unsigned    nucleo_wifiatk_sniffer_deauth(void)  { return 37; }

// The RUNNING screen frees the shared canvas on the device; on the host these are harmless no-ops.
void nucleo_screen_release(void) {}
bool nucleo_screen_acquire(void) { return true; }

}  // extern "C"

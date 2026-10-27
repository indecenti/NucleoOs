// ui-host stub for app_airspace.cpp — a simulated passive Wi-Fi monitor that is "listening" with live frame
// counters and a couple of raised alerts, so the two-screen dashboard renders with real content. FICTIONAL
// counts. Defensive app: it never transmits.
#include "nucleo_sentinel.h"
#include "nucleo_wifiatk.h"

extern "C" {

// The monitor came up.
void    sentinel_airspace_start(void) {}
void    sentinel_airspace_stop(void)  {}
// Deauth flood + evil-twin raised this session (SENTINEL_A_DEAUTH_FLOOD | SENTINEL_A_EVIL_TWIN).
uint8_t sentinel_airspace_alerts(void) { return 0x01 | 0x04; }

bool nucleo_wifiatk_sniffer_running(void) { return true; }
int  nucleo_wifiatk_sniffer_channel(void) { return 6; }
unsigned nucleo_wifiatk_sniffer_deauth(void)  { return 214; }
unsigned nucleo_wifiatk_sniffer_beacons(void) { return 1893; }

}  // extern "C"

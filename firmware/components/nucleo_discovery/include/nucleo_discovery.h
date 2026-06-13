// Advertises NucleoOS on the LAN via mDNS so clients can auto-discover it.
#pragma once
#include "esp_err.h"

// Start mDNS: hostname "<id>.local" and service _nucleoos._tcp on port 80.
esp_err_t nucleo_discovery_start(const char *device_id);

// Tear the mDNS responder down completely. Security apps call this before owning the radio so the
// rogue AP / attack interface never advertises "NucleoOS" on the air — that broadcast is an instant
// give-away that the network is a hobby ESP32, not the consumer router it pretends to be.
void nucleo_discovery_stop(void);

// Bring mDNS back up with the device id captured at the last start (no-op if it was never started).
esp_err_t nucleo_discovery_resume(void);

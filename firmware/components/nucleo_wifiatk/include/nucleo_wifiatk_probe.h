// Pure 802.11 probe-request SSID parser — no ESP-IDF, no hardware, so it compiles into both the
// firmware (nucleo_wifiatk's KARMA sniffer) and the host gate (probe-ctest.c). The KARMA lure listens
// for the directed probe requests devices emit for networks in their saved list; this extracts the
// SSID those probes are asking for. FOR AUTHORIZED TESTING ONLY — byte parsing only.
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// Extract the SSID from an 802.11 PROBE REQUEST management frame (FC byte 0x40). On success returns
// the SSID length (1..32) and writes a NUL-terminated, printable-ASCII copy into `out` (needs cap >=
// len+1). Returns -1 when the frame is not a probe request, is too short/malformed, is a broadcast
// (wildcard, empty SSID), or the SSID contains non-printable bytes (hidden/garbage).
int wifiatk_probe_ssid(const uint8_t *frame, int len, char *out, int cap);

#ifdef __cplusplus
}
#endif

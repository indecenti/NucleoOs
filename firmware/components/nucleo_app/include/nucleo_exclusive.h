// nucleo_exclusive — reusable "dedicated mode" for native apps.
//
// The Cardputer (ESP32-S3, no PSRAM) has only ~18KB free heap at runtime once the whole OS is up.
// A memory-hungry native app (e.g. an SSH client linking libssh2+mbedTLS, ~40-50KB) can't run in
// that budget. This module lets an app temporarily SUSPEND heavy subsystems to reclaim RAM, then
// RESTORE them on exit — the same trick single-purpose firmwares (Bruce) get for free by not running
// a full OS. By default Wi-Fi STA stays up; pass NX_WIFI (or NX_DEEP_OFFLINE) only for a standalone,
// no-client offline app that needs the Wi-Fi RAM back too.
//
// Usage:
//   nucleo_exclusive_info_t inf;
//   nucleo_exclusive_enter(NX_HTTPD | NX_ANIMA_L1 | NX_DISCOVERY, &inf);   // ~70KB freed
//   ... do the heavy work while inf.free_after / inf.largest_after are big enough ...
//   nucleo_exclusive_exit();                                              // bring everything back
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NX_HTTPD     0x01   // stop the HTTP server (~30KB task stack) — restarted on exit
#define NX_ANIMA_L1  0x02   // unload the ANIMA L1 retrieval index (~31KB) — reloads lazily on next query
#define NX_DISCOVERY 0x04   // stop mDNS advertising (~10KB) — resumed on exit
#define NX_VOICE     0x08   // disable the voice engine — re-enabled on exit
#define NX_RECORDER  0x10   // stop the recorder if it's mid-recording (no auto-restart)
#define NX_WIFI      0x20   // DEEP-OFFLINE: tear down the Wi-Fi radio (~30-50KB: driver+lwip+TLS) —
                            // re-joined (or AP fallback) on exit. Kills ALL network: pair only with
                            // NX_HTTPD|NX_DISCOVERY. NOT in NX_ALL (SSH/network apps must keep Wi-Fi).
#define NX_ALL       (NX_HTTPD | NX_ANIMA_L1 | NX_DISCOVERY | NX_VOICE | NX_RECORDER)
// Canonical reclaim mask for a network app (~70KB freed, Wi-Fi STA stays up). One source of truth so
// the net-app callers (SSH/Recorder/Video + the offensive engines) never drift on flag order. NEVER OR
// in NX_WIFI here: every user of this mask needs the radio (streaming/TLS/upload, or owns it for injection).
#define NX_NET_APP   (NX_HTTPD | NX_ANIMA_L1 | NX_DISCOVERY | NX_VOICE)
// Fully-offline reclaim for a standalone heavy app (e.g. on-device TTS): everything network down +
// L1/voice suspended. The biggest contiguous-RAM win this PSRAM-less chip can reach short of the
// framebuffer release. Restored verbatim on exit.
#define NX_DEEP_OFFLINE (NX_HTTPD | NX_DISCOVERY | NX_VOICE | NX_ANIMA_L1 | NX_WIFI)

typedef struct {
    size_t free_before, free_after;        // MALLOC_CAP_INTERNAL free heap before/after reclaim
    size_t largest_before, largest_after;  // largest contiguous block before/after
    uint32_t stopped;                      // which flags were actually applied
} nucleo_exclusive_info_t;

// Enter exclusive mode: stop the requested subsystems. Idempotent — a second enter while already
// active is a no-op that just reports current state. Fills `out` (may be NULL). Returns true if active.
bool nucleo_exclusive_enter(uint32_t flags, nucleo_exclusive_info_t *out);

// Restore everything this mode stopped (network services last). Safe to call when not active.
void nucleo_exclusive_exit(void);

bool nucleo_exclusive_active(void);

#ifdef __cplusplus
}
#endif

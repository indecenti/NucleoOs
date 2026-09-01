// nucleo_update — native release-update: boot check + on-device OTA install.
//
// The device asks ONE tiny question a day: GET version.json (~30 bytes) from the web-flasher
// Pages site, throttled on NVS, heap-gated and arbiter-serialized like every other TLS touch on
// this PSRAM-less chip. What it learns persists in NVS, so the boot dialog needs NO network.
// The install path streams nucleoos-latest-ota.bin straight into the OTA slot (never buffered),
// SHA-256-verified against SHA256SUMS from the same deploy; bootloader rollback covers a bad
// image. UI lives in app_updates.cpp; decisions in update_policy.c (host-gated).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPD_IDLE = 0,
    UPD_CHECKING,       // fetching version.json (manual or boot check)
    UPD_CHECK_DONE,     // check finished; latest tag (if any) is in NVS/state
    UPD_CHECK_FAIL,     // manual check failed (offline / busy) — boot checks fail silently
    UPD_DOWNLOADING,    // streaming the OTA image into the slot
    UPD_VERIFYING,      // finalizing + SHA-256 compare
    UPD_REBOOTING,      // set_boot done; restart imminent
    UPD_FAILED,         // install failed; err says why (short, already localized)
} upd_phase_t;

typedef struct {
    upd_phase_t phase;
    int         pct;        // 0..100 while downloading (-1 = length unknown)
    int         recv_kb;    // bytes received so far / 1024
    int         total_kb;   // content length / 1024 (0 = unknown)
    char        latest[24]; // last learned tag ("" = never)
    char        err[64];    // human-readable failure (localized by the engine)
} nucleo_update_state_t;

// Boot decision, NVS only (no network): a newer, non-dismissed release was learned earlier.
// Safe to call before any check ran; lazy-inits the NVS handle.
bool nucleo_update_dialog_pending(void);

// SYNCHRONOUS boot-time version check. Call from the boot sequence in the pre-httpd window — after
// the launcher canvas is freed and BEFORE httpd/L1/mDNS start — where the largest contiguous heap
// block is big enough for the external TLS handshake (it isn't once the OS is fully up). Throttled
// 24h on NVS; returns quickly when not due or when there's no network yet. Writes the learned tag
// to NVS so nucleo_update_dialog_pending() can offer the update on this same boot.
void nucleo_update_boot_check(void);

// Last tag learned from the network ("" if never). Points at internal storage; copy if kept.
const char *nucleo_update_latest_tag(void);

// "Don't show this version again": persists the current latest tag as dismissed.
void nucleo_update_dismiss_latest(void);

// Spawn the one-shot background check task. from_boot=true waits for Wi-Fi STA (up to ~5 min),
// honours the 24h NVS throttle and, on a newer non-dismissed release, emits the system
// notification (native banner/chime + notify.post to the web). from_boot=false is the manual
// path: no wait, no throttle, phase goes CHECKING -> CHECK_DONE/CHECK_FAIL for the UI.
// Returns false if a check/install task is already running.
bool nucleo_update_kick_check(bool from_boot);

// Spawn the OTA install worker. Call AFTER the app entered its exclusive/reclaimed posture.
// Returns false if already running. Progress via nucleo_update_get_state(); on success the
// device reboots by itself (phase UPD_REBOOTING is the last thing the UI sees).
bool nucleo_update_start(void);

// Snapshot the engine state for the UI (thread-safe copy).
void nucleo_update_get_state(nucleo_update_state_t *out);

// True while a check or install task is alive (the app blocks exit during an install).
bool nucleo_update_busy(void);

#ifdef __cplusplus
}
#endif

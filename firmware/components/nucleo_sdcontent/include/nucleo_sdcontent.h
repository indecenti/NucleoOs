// SD content self-install — device API. Fetches the web OS + ANIMA payload for THIS firmware from GitHub
// Pages (the host the OTA updater trusts), verifies every file by SHA-256 and writes it to the SD,
// resumable, never touching user/device state (content_policy.c is the gatekeeper). The download logic is
// sdc_engine.c (host-tested end to end); this layer adds the HTTPS transport, NVS and localization.
//
// The download is ARMED (NVS) and RUN at the next boot, in a dedicated minimal boot (main.c): only the SD,
// the UI language and a STA-only Wi-Fi link are up, so the TLS handshake has a clean heap. Requests are
// 6 KB Range windows on one keep-alive connection — the device's mbedTLS receives records of at most 8 KB,
// GitHub sends long bodies in 16 KB records (see sdc_engine.h).
// Entry points (docs/sd-content-install.md §8.5): the first-run wizard (after the Wi-Fi join, skippable),
// a boot dialog, Settings ▸ SD, and the web rescue page.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "sdc_engine.h"     // sdc_state_t, sdc_run_phase_t, sdc_code_t

#ifdef __cplusplus
extern "C" {
#endif

// Does the SD lack content matching the running firmware? Reads /sd/system/content.json only (no network):
// true when it is missing, incomplete, or for another firmware — false when complete for this firmware,
// when the user chose "Skip" for this firmware, or when no SD is mounted (nothing to fill).
bool nucleo_sdcontent_needed(void);

// Arm a download for the NEXT boot (NVS flag) and reboot, OR disarm. Returns false if the NVS write failed.
bool nucleo_sdcontent_arm(bool on);
bool nucleo_sdcontent_armed(void);          // is a download armed for this boot?

// "Skip" for this firmware version: content.json declined=true, so the wizard / boot dialog stop offering it
// until a newer firmware (Settings ▸ SD can still install it).
void nucleo_sdcontent_decline(void);

// RUN the download now (BLOCKING, minutes). Call only in the dedicated install boot (main.c). Clears the arm
// flag first (a crash never loops), persists the failure reason to NVS (/api/status .sdc_diag).
bool nucleo_sdcontent_run(void);

// Live progress snapshot (valid during a run).
const sdc_state_t *nucleo_sdcontent_state(void);

// The last install failure reason, persisted to NVS (the install boot has no httpd). true if present.
bool nucleo_sdcontent_last_diag(char *out, size_t n);

// The last install boot's heap profile "stage free/largest KB | ..." (NVS), for /api/status .sdc_heap.
bool nucleo_sdcontent_last_heap(char *out, size_t n);

// Called by the glue once the Wi-Fi link is up, right before the first TLS handshake (weak; main.c frees
// the 32 KB canvas here so the handshake gets a block the Wi-Fi bring-up could not fragment).
void nucleo_sdcontent_on_net_ready(void);

// Progress hook, called by the engine; weak no-op default, main.c paints the boot-window bar.
void nucleo_sdcontent_on_progress(const sdc_state_t *st);

#ifdef __cplusplus
}
#endif

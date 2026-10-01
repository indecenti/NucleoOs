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

// Should the device OFFER the download? Looks at the card itself (no network): true only when the web OS /
// ANIMA files are missing, a run was interrupted, or they are from an older release (then it is an UPDATE:
// only the changed files are fetched). A hand copy of this release, a complete install, a "Later", or no SD
// at all: false. See nucleo_sdcontent_status() / sdc_content_status().
bool nucleo_sdcontent_needed(void);

// What the SD holds, for the wizard, Settings ▸ Device and the web rescue page (the card's own files, no
// network; the values mirror sdc_status_t, plus NO_SD).
typedef enum {
    NUCLEO_SDC_NO_SD = 0,     // no card mounted
    NUCLEO_SDC_MISSING,       // the web OS is not on the card
    NUCLEO_SDC_PARTIAL,       // a run for this firmware was interrupted (a rerun resumes)
    NUCLEO_SDC_OUTDATED,      // there, but from another release: an update fetches only the changed files
    NUCLEO_SDC_SKIPPED,       // not there, the user chose "Later" for this firmware
    NUCLEO_SDC_COMPLETE,      // installed and verified here, for this firmware
    NUCLEO_SDC_MANUAL,        // copied by hand from this release's -sd.zip (not verified yet)
    NUCLEO_SDC_UNKNOWN,       // there, but nothing says which release (a dev sync, an old zip)
} nucleo_sdc_status_t;
nucleo_sdc_status_t nucleo_sdcontent_status(void);

// The release the card's content is from ("0.4.2"), when known; "" otherwise. Valid after _status().
const char *nucleo_sdcontent_card_tag(void);

// This firmware's release, "x.y.z" (the content it wants; the -sd.zip to copy by hand is named after it).
const char *nucleo_sdcontent_fw_tag(void);

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

// SD content self-install — device engine. Fetches the web OS + ANIMA payload for THIS firmware from
// GitHub Pages (the same host the OTA updater trusts), verifies every file by SHA-256 and writes it to
// the SD, resumable and without ever touching user/device state (content_policy.c is the gatekeeper).
//
// RAM reality on this no-PSRAM chip: a TLS session needs a ~40 KB contiguous block that only exists in
// the pre-httpd boot window (the OS fully up leaves ~15 KB). So a download is ARMED (NVS) and RUN at the
// next boot, in that window, with a direct-draw progress screen — exactly the OTA updater's pattern.
// Entry points (docs/sd-content-install.md §8.5): the first-run wizard (after the Wi-Fi join, skippable),
// a boot dialog, Settings ▸ SD, and the web rescue page.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SDC_IDLE = 0,
    SDC_CHECKING,       // fetching the manifest
    SDC_DOWNLOADING,    // streaming files
    SDC_DONE,           // every file installed; content.json written
    SDC_FAILED,         // run failed; err says why (short, localized)
} sdc_run_phase_t;

typedef struct {
    sdc_run_phase_t phase;
    int  files_total;
    int  files_done;      // installed + skipped (verified)
    int  files_written;   // actually downloaded this run
    int  pct;             // 0..100 by file count (-1 unknown)
    int  recv_kb;         // bytes written this run / 1024
    char cur[40];         // current file (basename), for the progress line
    char err[72];         // localized failure reason
} sdc_state_t;

// Does the SD lack content matching the running firmware? Reads /sd/system/content.json only (no network,
// no NVS): true when it is missing, incomplete, or its tag != this firmware's. The wizard/boot/Settings
// use this to decide whether to offer the download. False when the SD is not mounted (nothing to fill).
bool nucleo_sdcontent_needed(void);

// Arm a download for the NEXT boot (NVS flag) and reboot, OR disarm. The first-run wizard's "Download"
// button calls arm(true); "Skip" leaves it disarmed and marks content.json so it is not re-offered until
// a firmware change. Returns false if the NVS write failed (nothing rebooted).
bool nucleo_sdcontent_arm(bool on);
bool nucleo_sdcontent_armed(void);          // is a download armed for this boot?

// Mark "the user declined for this firmware version" so the boot dialog / wizard stop offering it (until a
// newer firmware makes the tag mismatch again). Writes content.json with declined=true.
void nucleo_sdcontent_decline(void);

// RUN the download now (BLOCKING, ~minutes). MUST be called in the big-heap boot window (pre-httpd), with
// Wi-Fi bringing up STA. Streams the manifest, plans each file (content_policy), downloads+verifies+writes
// the ones that differ, merges nothing it must not, writes content.json on success. Feeds the WDT, draws
// progress via the weak hook below. Returns true if the payload is complete. Clears the arm flag first, so
// a crash can never loop. Safe to re-run: verified files are skipped (resume).
bool nucleo_sdcontent_run(void);

// Live progress snapshot (valid during a run; the UI polls it).
const sdc_state_t *nucleo_sdcontent_state(void);

// Progress hook: the engine calls this after each file so a boot-window drawer can paint a bar. Weak,
// default no-op; main.c provides the real one (it owns M5GFX). Never call UI from here directly (C).
void nucleo_sdcontent_on_progress(const sdc_state_t *st);

#ifdef __cplusplus
}
#endif

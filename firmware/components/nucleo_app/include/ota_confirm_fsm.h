// OTA deferred confirm + trial — the PURE decision core, isolated so it can be host-tested (no FreeRTOS, no
// flash). main.c owns the state and does the flash work; the host gate (tools/anima-host/ota-confirm-ctest.c)
// compiles this SAME header and drives every path.
//
// Behaviour the device must give:
//   • A freshly-OTA'd image (PENDING_VERIFY) is CONFIRMED only once the OS has proved itself: httpd up
//     (ocf_arm), then the app loop running OCF_AFTER_US measured from its FIRST turn (a long blocking start-up
//     prologue never counts as proof). A panic or watchdog reset before that rolls the image back.
//   • A failed confirm (flash busy, a transient malloc) is retried every OCF_RETRY_US — never dropped, or the
//     next restart would roll a good image back.
//   • A DELIBERATE restart inside the window (the web-profile handoff, a Solo launch, the wizard) confirms first
//     — but only once the loop has run, so an esp_restart() on a failure path before the UI exists still rolls
//     back. Such an EARLY confirm proved little, so it leaves a TRIAL marker (RTC memory, survives resets): a
//     boot that then crash-resets (panic / watchdog) while the marker is set rolls the image back on purpose
//     (it is VALID by then, so only an explicit esp_ota_mark_app_invalid_rollback_and_reboot can). The marker
//     clears once a boot's loop has itself run OCF_AFTER_US.
//   • "Settled" (a restart now would not roll a good image back): nothing pending, or the loop has run.
#ifndef OTA_CONFIRM_FSM_H
#define OTA_CONFIRM_FSM_H

#include <stdbool.h>
#include <stdint.h>

#define OCF_AFTER_US  (20LL * 1000000)   // healthy app-loop run that proves a new image
#define OCF_RETRY_US  (5LL * 1000000)    // retry spacing after a failed confirm

typedef struct {
    bool    armed;      // a pending image waits for its confirm
    int64_t loop_t0;    // first app-loop turn (0 = the loop has not run yet in this boot)
    int64_t next_try;   // a failed attempt's retry time (0 = loop_t0 + OCF_AFTER_US)
    int     tries;      // failed attempts so far (diagnostics)
} ocf_t;

static inline void ocf_arm(ocf_t *f) { f->armed = true; f->next_try = 0; f->tries = 0; }

// One app-loop turn at `now`. True = attempt the confirm now (the caller then reports ocf_result).
static inline bool ocf_tick(ocf_t *f, int64_t now)
{
    if (!f->loop_t0) f->loop_t0 = now > 0 ? now : 1;
    if (!f->armed) return false;
    int64_t due = f->next_try ? f->next_try : f->loop_t0 + OCF_AFTER_US;
    return now >= due;
}

// The outcome of an attempt made at `now`: disarm on success, schedule a retry otherwise.
static inline void ocf_result(ocf_t *f, bool ok, int64_t now)
{
    if (ok) { f->armed = false; return; }
    f->tries++;
    f->next_try = now + OCF_RETRY_US;
}

// A deliberate restart is under way: confirm first? Only a pending image whose loop has already run.
static inline bool ocf_on_restart(const ocf_t *f) { return f->armed && f->loop_t0 != 0; }

// Would a restart right now keep a good image? (POST /api/sdcontent/arm and /api/reboot refuse until then.)
static inline bool ocf_settled(const ocf_t *f, bool image_pending) { return !image_pending || f->loop_t0 != 0; }

// Boot-time trial verdict: an early-confirmed image whose last boot ended in a crash reset goes back.
static inline bool ocf_trial_rollback(bool marker, bool crash_reset) { return marker && crash_reset; }

// The trial ends once this boot's loop has run OCF_AFTER_US (the image has now proved itself for real).
static inline bool ocf_trial_cleared(const ocf_t *f, int64_t now) { return f->loop_t0 != 0 && now - f->loop_t0 >= OCF_AFTER_US; }

#endif

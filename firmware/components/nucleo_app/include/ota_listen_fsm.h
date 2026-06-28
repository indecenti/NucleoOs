// OTA listening-handshake — the PURE decision core, isolated so it can be host-tested (no M5GFX/FreeRTOS).
//
// Behaviour the device must give (the user's spec): an OTA request arrives -> the device LAUNCHES Remote
// Control (its enter frees the 32 KB canvas + the L1 index = the server-listening posture that lets a flash
// through); the update proceeds only once that posture is live; when the OTA ends, the Remote Control WE
// opened is closed and everything returns to how it was. A Remote Control the USER opened by hand is left
// untouched (we never close what we didn't open).
//
// The run loop calls ota_listen_step() once per tick with the live inputs and acts on the returned action;
// ota_post() (httpd) blocks on the `ready` latch before esp_ota_begin. This header is the single source of
// that logic — the firmware and the host gate (tools/anima-host/ota-listen-ctest.c) compile the SAME code.
#ifndef OTA_LISTEN_FSM_H
#define OTA_LISTEN_FSM_H

#include <stdbool.h>

typedef enum {
    OTA_LISTEN_NONE = 0,   // nothing to do this tick
    OTA_LISTEN_LAUNCH,     // launch Remote Control now (frees RAM), then re-tick
    OTA_LISTEN_READY,      // Remote Control is foreground & RAM is freed -> ota_post may proceed
    OTA_LISTEN_RESTORE,    // OTA ended -> close the Remote Control WE opened, back to the launcher
} ota_listen_action_t;

typedef struct {
    bool force;             // IN : an OTA wants listening mode (httpd raised it)
    bool active_is_remote;  // IN : the Remote Control app is the current foreground app
    bool owned;             // IN/OUT: WE auto-launched Remote Control for this OTA (so WE close it after)
    bool ready;             // IN/OUT: the listening posture is confirmed live (ota_post waits on this)
} ota_listen_state_t;

// Pure transition: reads s, mutates s->owned/s->ready, returns the action the caller must perform.
// Deterministic and side-effect-free beyond the struct, so the host test can drive every path.
static inline ota_listen_action_t ota_listen_step(ota_listen_state_t *s)
{
    if (s->force) {
        if (s->active_is_remote) {     // posture already live (we just launched it, or the user had it open)
            s->ready = true;
            return OTA_LISTEN_READY;
        }
        s->owned = true;               // about to open it ourselves -> remember to close it when the OTA ends
        return OTA_LISTEN_LAUNCH;
    }
    // force cleared = the OTA ended (done or aborted). The posture is no longer guaranteed.
    s->ready = false;
    if (s->owned) {                    // restore "as before" only for a Remote Control WE opened
        s->owned = false;
        if (s->active_is_remote) return OTA_LISTEN_RESTORE;
    }
    return OTA_LISTEN_NONE;
}

#endif // OTA_LISTEN_FSM_H

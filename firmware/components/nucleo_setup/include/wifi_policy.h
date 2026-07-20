// Wi-Fi supervisor decision core — PURE C, no ESP-IDF dependencies, host-testable.
//
// Every decision about WHEN the background supervisor may touch the radio lives here, so the
// scan-race family of bugs ("incorrect password" / "couldn't authenticate" on the hotspot, AP
// flapping or left down) is provable on the PC (tools/anima-host/wifi-check.mjs, `npm run
// wifi:test`) before it ever reaches a device. nucleo_setup.c owns the esp_wifi mechanics and
// only executes the actions returned from here.
//
// Invariants this module enforces (the "fixed once and for all" contract):
//  I1  While ANY client is associated with the soft-AP — or was active within the grace
//      window — the supervisor never scans and never starts a join attempt.
//  I2  A join attempt never takes a live fallback hotspot down: with the AP interface up,
//      joins run in APSTA (beacons continue), never pure STA.
//  I3  After a failed join cycle the radio always settles back to a beaconing AP; the check
//      is on the ACTUAL driver mode, never on the "ap"/"sta" intent strings.
//  I4  A failed one-shot manual join never arms the background retry loop (s_auto keeps its
//      previous value), so a wrong password cannot start an eternal AP-flapping loop.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Radio mode as the policy sees it. Values intentionally mirror esp_wifi's wifi_mode_t
// (WIFI_MODE_NULL/STA/AP/APSTA = 0..3) so firmware casts are direct; asserted in nucleo_setup.c.
typedef enum { WP_MODE_NULL = 0, WP_MODE_STA = 1, WP_MODE_AP = 2, WP_MODE_APSTA = 3 } wp_mode_t;

// What the supervisor must do on this tick.
typedef enum {
    WP_ACT_IDLE = 0,      // nothing to do (wait)
    WP_ACT_DEFER,         // soft-AP busy: retry window pushed forward, do NOT touch the radio
    WP_ACT_RECONNECT,     // held IP but the link is silently dead: force a driver reconnect
    WP_ACT_TRY_JOIN,      // run one scan+join cycle now
    WP_ACT_DROP_AP,       // joined + hotspot idle past grace: stop beaconing (battery)
} wp_action_t;

typedef struct {
    // Tuning (filled by wp_init; override before first tick if ever needed).
    uint32_t backoff_min_ms;      // first/steady retry interval while nothing is wrong
    uint32_t backoff_max_ms;      // exponential backoff ceiling
    uint32_t ap_grace_ms;         // radio-quiet window after ANY soft-AP client activity
    // Internal state.
    uint32_t backoff_ms;
    uint32_t next_retry_ms;
    int      link_miss;           // consecutive "associated?" poll failures while holding an IP
    uint32_t last_ap_activity_ms; // stamp of the latest client connect/disconnect on the soft-AP
    bool     ap_activity_seen;    // false until the first activity stamp (stamp 0 is valid)
} wp_state_t;

// Everything wp_tick needs from the live system. The caller samples, the policy decides.
typedef struct {
    bool      auto_join;        // user intends a client (STA) link (s_auto)
    bool      have_ip;          // an STA IP is currently held
    bool      link_alive;       // driver says we are associated (esp_wifi_sta_get_ap_info == OK)
    int       driver_sta_count; // stations associated with our soft-AP (driver list; 0 if AP down)
    wp_mode_t mode;             // actual driver mode right now
} wp_tick_in_t;

// Consecutive dead polls (at the supervisor's 2 s tick) before a held IP is declared stale.
#define WP_LINK_MISS_LIMIT 3

void wp_init(wp_state_t *s, uint32_t now_ms);

// Feed a soft-AP client event (WIFI_EVENT_AP_STACONNECTED / _STADISCONNECTED). Starts the
// grace window: a client that just failed a WPA2 handshake gets quiet air for its retries.
void wp_ap_activity(wp_state_t *s, uint32_t now_ms);

// True while the soft-AP must not be disturbed: someone is associated (driver count — covers
// a client mid 4-way handshake, which IS in the list from association on) or client activity
// happened within the grace window. Also the mid-cycle abort check between join candidates.
bool wp_ap_busy(const wp_state_t *s, int driver_sta_count, uint32_t now_ms);

// One supervisor tick. Mutates only policy state; the caller executes the returned action.
wp_action_t wp_tick(wp_state_t *s, const wp_tick_in_t *in, uint32_t now_ms);

// Report the outcome of a WP_ACT_TRY_JOIN cycle: resets (success) or doubles (failure) the
// backoff and re-arms the retry window.
void wp_cycle_done(wp_state_t *s, bool joined, uint32_t now_ms);

// Mode a join attempt must run in, given the current driver mode. AP interface up -> APSTA
// (the hotspot keeps beaconing through the attempt); otherwise pure STA. (Invariant I2)
wp_mode_t wp_join_mode(wp_mode_t current_mode);

// True if the driver mode has the AP interface up.
bool wp_ap_iface_up(wp_mode_t mode);

// True when, after a failed join cycle, the AP interface is gone and the fallback hotspot
// must be (re)started. Decides from the ACTUAL driver mode. (Invariant I3)
bool wp_need_ap_restore(wp_mode_t driver_mode);

// Value s_auto must take after a manual one-shot join: success arms the supervisor to keep
// the link; failure keeps the PREVIOUS intent — never arms the retry loop. (Invariant I4)
bool wp_join_result_auto(bool prev_auto, bool joined);

#ifdef __cplusplus
}
#endif

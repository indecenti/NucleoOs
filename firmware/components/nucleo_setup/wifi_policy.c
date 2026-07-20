// Wi-Fi supervisor decision core — see wifi_policy.h for the invariants. Pure C on purpose:
// compiled unchanged into the firmware AND by the host gate (tools/anima-host/wifi-check.mjs).
// All time comparisons are wraparound-safe (signed diff of unsigned ms), so a device that has
// been up for 49.7 days (uint32 ms wrap) keeps scheduling correctly.
#include "wifi_policy.h"

#define WP_BACKOFF_MIN_MS 8000u
#define WP_BACKOFF_MAX_MS 60000u
#define WP_AP_GRACE_MS    90000u

static inline bool time_reached(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

void wp_init(wp_state_t *s, uint32_t now_ms)
{
    s->backoff_min_ms = WP_BACKOFF_MIN_MS;
    s->backoff_max_ms = WP_BACKOFF_MAX_MS;
    s->ap_grace_ms    = WP_AP_GRACE_MS;
    s->backoff_ms     = s->backoff_min_ms;
    s->next_retry_ms  = now_ms;            // first attempt eligible immediately
    s->link_miss      = 0;
    s->last_ap_activity_ms = 0;
    s->ap_activity_seen    = false;
}

void wp_ap_activity(wp_state_t *s, uint32_t now_ms)
{
    s->last_ap_activity_ms = now_ms;
    s->ap_activity_seen    = true;
}

bool wp_ap_busy(const wp_state_t *s, int driver_sta_count, uint32_t now_ms)
{
    if (driver_sta_count > 0) return true;                 // someone is on the hotspot right now
    if (!s->ap_activity_seen) return false;
    // Inside the grace window after the last client connect/disconnect. A phone that failed a
    // WPA2 handshake retries within seconds — it must find quiet, on-channel air when it does.
    return !time_reached(now_ms, s->last_ap_activity_ms + s->ap_grace_ms);
}

wp_action_t wp_tick(wp_state_t *s, const wp_tick_in_t *in, uint32_t now_ms)
{
    if (!in->auto_join) {                  // explicit hotspot (or idle) — supervisor hands off
        s->backoff_ms = s->backoff_min_ms;
        s->link_miss  = 0;
        return WP_ACT_IDLE;
    }
    if (in->have_ip) {
        s->backoff_ms = s->backoff_min_ms;
        if (in->link_alive) {
            s->link_miss = 0;
            // Joined and healthy. If the fallback hotspot is still beaconing and idle past the
            // grace window, it can finally be dropped (battery) — a client that followed us
            // through the join (web UI) keeps it alive via wp_ap_busy until they leave.
            if (wp_ap_iface_up(in->mode) && !wp_ap_busy(s, in->driver_sta_count, now_ms))
                return WP_ACT_DROP_AP;
            return WP_ACT_IDLE;
        }
        // Holding an IP but the driver says "not associated": silent link loss. Tolerate
        // WP_LINK_MISS_LIMIT consecutive polls before forcing a reconnect.
        if (++s->link_miss >= WP_LINK_MISS_LIMIT) {
            s->link_miss = 0;
            return WP_ACT_RECONNECT;
        }
        return WP_ACT_IDLE;
    }
    s->link_miss = 0;
    if (wp_ap_busy(s, in->driver_sta_count, now_ms)) {
        // Invariant I1: never disturb an in-use hotspot. Keep pushing the retry window so the
        // moment the AP goes quiet there is still a full backoff_min of calm before a scan.
        s->next_retry_ms = now_ms + s->backoff_min_ms;
        return WP_ACT_DEFER;
    }
    if (!time_reached(now_ms, s->next_retry_ms)) return WP_ACT_IDLE;
    return WP_ACT_TRY_JOIN;
}

void wp_cycle_done(wp_state_t *s, bool joined, uint32_t now_ms)
{
    if (joined) {
        s->backoff_ms = s->backoff_min_ms;
    } else {
        uint32_t b = s->backoff_ms * 2u;
        s->backoff_ms = b > s->backoff_max_ms ? s->backoff_max_ms : b;
    }
    s->next_retry_ms = now_ms + s->backoff_ms;
}

wp_mode_t wp_join_mode(wp_mode_t current_mode)
{
    return wp_ap_iface_up(current_mode) ? WP_MODE_APSTA : WP_MODE_STA;
}

bool wp_ap_iface_up(wp_mode_t mode)
{
    return mode == WP_MODE_AP || mode == WP_MODE_APSTA;
}

bool wp_need_ap_restore(wp_mode_t driver_mode)
{
    return !wp_ap_iface_up(driver_mode);
}

bool wp_join_result_auto(bool prev_auto, bool joined)
{
    return joined ? true : prev_auto;
}

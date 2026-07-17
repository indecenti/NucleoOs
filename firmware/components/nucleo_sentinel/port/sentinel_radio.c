// sentinel_radio — device binding for tracker detection: wire nucleo_ble's raw
// advertisement observer into the pure detection core and keep a spinlock-guarded
// persistence table the native app polls. Runs while a BLE scan is active.
#include "nucleo_sentinel.h"
#include "sentinel_ble.h"
#include "sentinel_track.h"
#include "nucleo_ble.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <string.h>
#include <stdlib.h>

// 5-minute persistence window, >=3 sightings to raise "following".
#define SENTINEL_FOLLOW_WINDOW_S 300
#define SENTINEL_FOLLOW_HITS     3
#define SENTINEL_TTL_S           120   // forget a tracker unseen for 2 minutes

// The tracker table (~0.9 KB) is HEAP-allocated only while Sentinel runs — never
// static .bss. On this no-PSRAM chip every resident KB is boot heap the OS can't
// use (see the boot-RAM discipline / the FIDO OOM lesson).
static sentinel_tracker_table *s_tab;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_on = false;

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

// NimBLE host-task callback: classify (pure, lock-free) then fold into the table.
static void adv_cb(const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                   const uint8_t *adv, uint8_t adv_len, void *ctx) {
    (void)addr_type; (void)ctx;
    sentinel_ble_class c;
    if (!sentinel_ble_classify(adv, adv_len, &c)) return;
    uint32_t now = now_s();
    portENTER_CRITICAL(&s_lock);
    if (s_tab) sentinel_track_seen(s_tab, addr, c.type, c.flags, rssi, now);
    portEXIT_CRITICAL(&s_lock);
}

void sentinel_tracker_start(void) {
    if (!s_tab) s_tab = malloc(sizeof *s_tab);
    if (!s_tab) return;                          // no heap — stay off, don't crash
    sentinel_track_init(s_tab, SENTINEL_FOLLOW_WINDOW_S, SENTINEL_FOLLOW_HITS);
    s_on = true;
    nucleo_ble_set_adv_observer(adv_cb, NULL);
}

void sentinel_tracker_stop(void) {
    nucleo_ble_set_adv_observer(NULL, NULL);      // no new callbacks
    s_on = false;
    sentinel_tracker_table *t;
    portENTER_CRITICAL(&s_lock);                  // wait out any in-flight adv_cb
    t = s_tab; s_tab = NULL;
    portEXIT_CRITICAL(&s_lock);
    if (t) free(t);
}

void sentinel_tracker_tick(void) {
    if (!s_on) return;
    uint32_t now = now_s();
    portENTER_CRITICAL(&s_lock);
    if (s_tab) sentinel_track_expire(s_tab, now, SENTINEL_TTL_S);
    portEXIT_CRITICAL(&s_lock);
}

int sentinel_tracker_count(void) {
    portENTER_CRITICAL(&s_lock);
    int n = s_tab ? s_tab->n : 0;
    portEXIT_CRITICAL(&s_lock);
    return n;
}

int sentinel_tracker_following(void) {
    portENTER_CRITICAL(&s_lock);
    int c = s_tab ? sentinel_track_following(s_tab) : 0;
    portEXIT_CRITICAL(&s_lock);
    return c;
}

bool sentinel_tracker_get(int idx, sentinel_view_t *out) {
    uint32_t now = now_s();
    bool ok = false;
    portENTER_CRITICAL(&s_lock);
    if (s_tab && idx >= 0 && idx < s_tab->n) {
        const sentinel_track_entry *e = &s_tab->e[idx];
        memcpy(out->addr, e->addr, 6);
        out->type = (int)e->type;
        out->flags = e->flags;
        out->rssi = e->rssi;
        out->hits = e->hits;
        out->age_s = now - e->first_seen;
        out->following = e->following;
        ok = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

const char *sentinel_type_name(int type) {
    return sentinel_tracker_name((sentinel_tracker_t)type);
}

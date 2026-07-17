// sentinel_airspace — device binding for the Wi-Fi airspace monitor. Reuses
// nucleo_wifiatk's promiscuous capture (which already owns the radio, hops the
// band, and restores the OS network on stop) in its no-pcap monitor mode, and
// feeds every management frame to the pure anomaly detector (sentinel_wifi).
#include "nucleo_sentinel.h"
#include "sentinel_wifi.h"
#include "nucleo_wifiatk.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>

// 10 s window; >=8 deauth or >=40 distinct beacons in it reads as a flood.
#define AIR_WINDOW_S      10
#define AIR_DEAUTH_THRESH 8
#define AIR_BEACON_THRESH 40

// The anomaly monitor (~0.9 KB) is HEAP-allocated only while Airspace runs — never
// static .bss (boot-RAM discipline on this no-PSRAM chip).
static sentinel_wifi_mon *s_mon;
static portMUX_TYPE s_wlock = portMUX_INITIALIZER_UNLOCKED;
static bool s_air_on = false;

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

// WiFi-task observer: parse (pure) then fold into the anomaly monitor.
static void air_obs(const uint8_t *frame, uint16_t len, int8_t rssi, uint8_t ch, void *ctx) {
    (void)rssi; (void)ch; (void)ctx;
    sentinel_wifi_frame fr;
    if (!sentinel_wifi_parse(frame, len, &fr)) return;
    uint32_t now = now_s();
    portENTER_CRITICAL(&s_wlock);
    if (s_mon) sentinel_wifi_mon_feed(s_mon, &fr, now);
    portEXIT_CRITICAL(&s_wlock);
}

void sentinel_airspace_start(void) {
    if (!s_mon) s_mon = malloc(sizeof *s_mon);
    if (!s_mon) return;                          // no heap — stay off, don't crash
    sentinel_wifi_mon_init(s_mon, AIR_WINDOW_S, AIR_DEAUTH_THRESH, AIR_BEACON_THRESH);
    s_air_on = true;
    nucleo_wifiatk_sniffer_observer(air_obs, NULL);
    nucleo_wifiatk_monitor_start();
}

void sentinel_airspace_stop(void) {
    nucleo_wifiatk_sniffer_observer(NULL, NULL);  // no new callbacks
    nucleo_wifiatk_sniffer_stop();
    s_air_on = false;
    sentinel_wifi_mon *m;
    portENTER_CRITICAL(&s_wlock);                 // wait out any in-flight air_obs
    m = s_mon; s_mon = NULL;
    portEXIT_CRITICAL(&s_wlock);
    if (m) free(m);
}

uint8_t sentinel_airspace_alerts(void) {
    portENTER_CRITICAL(&s_wlock);
    uint8_t a = s_mon ? s_mon->alerts : 0;
    portEXIT_CRITICAL(&s_wlock);
    return a;
}

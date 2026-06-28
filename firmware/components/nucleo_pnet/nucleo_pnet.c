// nucleo_pnet.c — best-effort ESP-NOW datagram layer (see nucleo_pnet.h).
#include "nucleo_pnet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "pnet";
static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static bool          s_inited;
static QueueHandle_t s_rxq;

// The ESP-NOW recv callback runs in the Wi-Fi task: do the minimum (copy into the queue) and return.
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_rxq || len <= 0 || len > PNET_MAXMSG) return;
    pnet_pkt_t p;
    memcpy(p.mac, info->src_addr, 6);
    p.len = len;
    memcpy(p.buf, data, len);
    xQueueSend(s_rxq, &p, 0);   // drop on overflow — a stale game frame is worthless anyway
}

// Register a peer once (idempotent). channel 0 = "use the current Wi-Fi channel".
static void ensure_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; p.ifidx = WIFI_IF_STA; p.encrypt = false;
    esp_now_add_peer(&p);
}

// Park on a known channel ONLY when nothing else dictates one — same rule as nucleo_link, plus AP.
static void lock_channel(void) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    wifi_ap_record_t ap;
    bool sta_assoc = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
    bool ap_on     = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    if (sta_assoc || ap_on) return;            // ride the network's channel (peers meet there)
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);   // no network: local-match channel
}

bool pnet_start(void) {
    if (s_inited) return true;
    s_rxq = xQueueCreate(12, sizeof(pnet_pkt_t));
    if (!s_rxq) { ESP_LOGE(TAG, "rxq alloc"); return false; }
    // esp_now is a global singleton: another path may have left it inited — treat EXIST as success.
    esp_err_t e = esp_now_init();
    if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_init %s", esp_err_to_name(e));
        vQueueDelete(s_rxq); s_rxq = NULL; return false;
    }
    esp_now_register_recv_cb(recv_cb);
    ensure_peer(BCAST);
    lock_channel();
    // System default is WIFI_PS_MIN_MODEM: the STA naps between DTIM beacons with the radio OFF, so
    // broadcast HELLO and unicast game frames get DROPPED (flaky discovery + mid-match stalls).
    // Real-time ESP-NOW needs the receiver always listening — kill power-save for the match.
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_inited = true;
    return true;
}

void pnet_stop(void) {
    if (!s_inited) return;
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    if (s_rxq) { vQueueDelete(s_rxq); s_rxq = NULL; }
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);   // restore the system default power-save
    s_inited = false;
}

const char *pnet_name(void) {
    extern const char *nucleo_setup_device_name(void);   // resolved at link (no REQUIRES cycle)
    const char *n = nucleo_setup_device_name();
    return (n && n[0]) ? n : "nucleo";
}

int pnet_channel(void) {
    uint8_t pri = 0; wifi_second_chan_t sec;
    esp_wifi_get_channel(&pri, &sec);
    return pri;
}

int pnet_send(const uint8_t *mac6, const void *buf, int len) {
    if (!s_inited || len <= 0 || len > PNET_MAXMSG) return -1;
    const uint8_t *dst = mac6 ? mac6 : BCAST;
    ensure_peer(dst);
    return esp_now_send(dst, (const uint8_t *)buf, len) == ESP_OK ? 0 : -1;
}

bool pnet_recv(pnet_pkt_t *out) {
    if (!s_inited || !s_rxq || !out) return false;
    return xQueueReceive(s_rxq, out, 0) == pdTRUE;
}

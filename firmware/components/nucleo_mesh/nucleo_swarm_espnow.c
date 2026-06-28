// nucleo_swarm_espnow.c — Swarm device engine. See the header. Mirrors nucleo_link_espnow.c.
#include "nucleo_swarm_espnow.h"
#include "nucleo_mesh.h"
#include "nucleo_chorus.h"
#include "nucleo_swarm.h"
#include "nucleo_swarm_sec.h"
#include "nucleo_board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "swarm";
static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
#define MANIFEST_MS  2000              // re-advertise our presence this often

typedef struct { uint8_t mac[6]; int len; uint8_t buf[256]; } rxpkt_t;

static bool              s_inited;
static SemaphoreHandle_t s_lock;       // recursive: guards mesh/chorus/member + last line
static QueueHandle_t     s_rxq;
static TaskHandle_t      s_task;
static volatile bool     s_run;

static nmesh_ctx_t   s_mesh;
static chorus_ctx_t  s_chorus;
static swarm_member_t s_member;
static uint32_t      s_seq;            // our outbound gossip sequence
static char          s_id[NM_ID_MAX + 1];
static char          s_last[48];

static void lock(void)   { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGiveRecursive(s_lock); }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void set_last(const char *fmt, const char *arg) {
    lock(); snprintf(s_last, sizeof s_last, fmt, arg ? arg : ""); unlock();
}

// ---- injected crypto (mbedtls) ---------------------------------------------
static int sw_hmac(void *u, const uint8_t *key, int klen, const uint8_t *msg, int mlen, uint8_t out[SW_MAC_OUT]) {
    (void)u;
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    return mbedtls_md_hmac(info, key, klen, msg, mlen, out) == 0 ? 0 : -1;
}
static int sw_kdf(void *u, const char *pass, int pl, const uint8_t *salt, int sl, uint8_t out[SW_PSK_LEN]) {
    (void)u;
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const uint8_t *)pass, pl,
                                         salt, sl, 100000, SW_PSK_LEN, out) == 0 ? 0 : -1;
}

// ---- ESP-NOW peer ----------------------------------------------------------
static void ensure_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; p.ifidx = WIFI_IF_STA; p.encrypt = false;
    esp_now_add_peer(&p);
}

// ---- mesh I/O seam ---------------------------------------------------------
// Outbound: seal the nmesh frame for our swarm (open = no-op), then broadcast.
static int io_send(void *u, const uint8_t *buf, int len) {
    (void)u;
    uint8_t f[NM_MTU];                 // nm_encode reserves NM_TAG_RESERVE, so sealed always fits
    if (len < 0 || len > (int)sizeof f) return -1;
    memcpy(f, buf, len);
    int sl = swarm_seal_out(&s_member, f, len, sizeof f, sw_hmac, NULL);
    if (sl < 0) return -1;
    ensure_peer(BCAST);
    return esp_now_send(BCAST, f, sl) == ESP_OK ? 0 : -1;
}
// Inbound: a foreign event arrived on our bus. Handle presence + ping/pong.
static void io_inject(void *u, const char *src, const char *topic, const char *payload) {
    (void)u;
    if (!strcmp(topic, "chorus.cap")) {
        chorus_on_manifest(&s_chorus, src, payload, now_ms());
        set_last("vede %s", src);
    } else if (!strcmp(topic, "mesh.ping")) {
        char j[64]; snprintf(j, sizeof j, "{\"from\":\"%s\"}", s_id);
        nmesh_on_local_event(&s_mesh, ++s_seq, "mesh.pong", j, NULL);
        set_last("ping da %s", src);
    } else if (!strcmp(topic, "mesh.pong")) {
        set_last("pong da %s", src);
    }
}
static const nmesh_io_t IO = { NULL, io_send, io_inject };

// ---- RX callback (Wi-Fi task ctx) -> queue ---------------------------------
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_rxq || len <= 0 || len > 256) return;
    rxpkt_t p; memcpy(p.mac, info->src_addr, 6); p.len = len; memcpy(p.buf, data, len);
    xQueueSend(s_rxq, &p, 0);
}

// ---- service pump (own task) -----------------------------------------------
static void emit_manifest(void) {
    char j[160];
    uint16_t freekb = (uint16_t)(esp_get_free_heap_size() / 1024);
    if (chorus_self_manifest(&s_chorus, freekb, 0, j, sizeof j) < 0) return;
    nmesh_on_local_event(&s_mesh, ++s_seq, "chorus.cap", j, NULL);
}
static void pump_once(void) {
    rxpkt_t p;
    while (xQueueReceive(s_rxq, &p, 0) == pdTRUE) {
        int plen = swarm_admit(&s_member, p.mac, p.buf, p.len, sw_hmac, NULL);
        if (plen > 0) nmesh_on_frame(&s_mesh, p.buf, plen);   // verified + tag stripped
    }
}
static void svc_task(void *arg) {
    (void)arg;
    uint32_t last_manif = 0;
    while (s_run) {
        lock();
        pump_once();
        uint32_t t = now_ms();
        if (t - last_manif >= MANIFEST_MS) { emit_manifest(); last_manif = t; }
        unlock();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_task = NULL; vTaskDelete(NULL);
}

// ---- channel (mirror nucleo_link: park unassociated devices so they meet) --
static void lock_channel(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}

// ---- public API ------------------------------------------------------------
const char *swarm_svc_name(void) {
    extern const char *nucleo_setup_device_name(void);
    const char *n = nucleo_setup_device_name(); return (n && n[0]) ? n : "nucleo";
}

bool swarm_svc_start(void) {
    if (s_inited) return true;
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_rxq  = xQueueCreate(16, sizeof(rxpkt_t));
    if (!s_lock || !s_rxq) { ESP_LOGE(TAG, "alloc"); return false; }
    if (esp_now_init() != ESP_OK) { ESP_LOGE(TAG, "esp_now_init"); return false; }
    esp_now_register_recv_cb(recv_cb);
    ensure_peer(BCAST);
    lock_channel();

    snprintf(s_id, sizeof s_id, "%s", swarm_svc_name());
    nmesh_init(&s_mesh, &IO, s_id);
    chorus_init(&s_chorus, s_id, CH_CAP_PRESENCE, "");   // v1: presence only, no domain shards
    swarm_member_init(&s_member);                        // OPEN until a passphrase is set
    s_seq = 0; s_last[0] = 0;
    s_inited = true; s_run = true;
    xTaskCreate(svc_task, "swarm", 4096, NULL, tskIDLE_PRIORITY + 2, &s_task);
    return true;
}

void swarm_svc_stop(void) {
    if (!s_inited) return;
    s_run = false; for (int i = 0; i < 40 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    lock();
    esp_now_unregister_recv_cb(); esp_now_deinit();
    unlock();
    if (s_rxq)  { vQueueDelete(s_rxq);  s_rxq = NULL; }
    if (s_lock) { vSemaphoreDelete(s_lock); s_lock = NULL; }
    s_inited = false;
}

int swarm_svc_channel(void) {
    uint8_t pri = 0; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec); return pri;
}
bool swarm_svc_is_open(void) { bool o; lock(); o = swarm_is_open(&s_member); unlock(); return o; }

int swarm_svc_set_passphrase(const char *name, const char *pass) {
    uint8_t salt[1 + sizeof(SW_SALT) + 32];
    int n = snprintf((char *)salt, sizeof salt, "%s\x1f%s", SW_SALT, name ? name : "");
    lock();
    int r = swarm_join(&s_member, pass, salt, n, sw_kdf, NULL);
    unlock();
    return r;
}

// directory: compact over fresh, non-stale chorus peers
static const chorus_peer_t *fresh_peer(int want) {
    uint32_t t = now_ms(); int seen = 0;
    for (int i = 0; i < CH_PEERS; i++) {
        const chorus_peer_t *p = &s_chorus.peers[i];
        if (!p->used) continue;
        uint32_t age = t >= p->last_ms ? t - p->last_ms : 0;
        if (age > CH_TTL_MS) continue;
        if (seen++ == want) return p;
    }
    return NULL;
}
int swarm_svc_peer_count(void) {
    int n = 0; lock();
    uint32_t t = now_ms();
    for (int i = 0; i < CH_PEERS; i++) {
        const chorus_peer_t *p = &s_chorus.peers[i];
        if (!p->used) continue;
        uint32_t age = t >= p->last_ms ? t - p->last_ms : 0;
        if (age <= CH_TTL_MS) n++;
    }
    unlock(); return n;
}
const char *swarm_svc_peer_id(int i)   { lock(); const chorus_peer_t *p = fresh_peer(i); const char *r = p ? p->id : ""; unlock(); return r; }
int         swarm_svc_peer_free(int i) { lock(); const chorus_peer_t *p = fresh_peer(i); int r = p ? p->free_kb : 0; unlock(); return r; }
int         swarm_svc_peer_busy(int i) { lock(); const chorus_peer_t *p = fresh_peer(i); int r = p ? p->busy : 0; unlock(); return r; }

void swarm_svc_ping(void) {
    char j[64]; snprintf(j, sizeof j, "{\"from\":\"%s\"}", s_id);
    lock(); nmesh_on_local_event(&s_mesh, ++s_seq, "mesh.ping", j, NULL); unlock();
    set_last("ping inviato", NULL);
}
void swarm_svc_last(char *out, int cap) { lock(); snprintf(out, cap, "%s", s_last); unlock(); }

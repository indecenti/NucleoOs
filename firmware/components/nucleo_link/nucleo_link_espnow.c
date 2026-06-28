// nucleo_link_espnow.c — ESP-NOW + SD engine for the Vicino app. See nucleo_link_espnow.h.
//
// One persistent service task (started while the app is foreground) drains the ESP-NOW RX queue and
// drives the protocol every few ms — that gives real throughput (vs the 5Hz on_tick) while the UI just
// reads status and issues requests. Inbound frames auto-dispatch by format: 248-byte = Bruce codec,
// 'NL'-magic = the evolved core. Files STREAM through SD (one frame buffered, never the whole file).
#include "nucleo_link_espnow.h"
#include "nucleo_link_bruce.h"
#include "nucleo_board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "vicino";
static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
#define BRUCE_PACE_MS 40            // Bruce has no flow control; pace like its delay(100), a bit faster
#define MAXPEERS      8
#define INBOX_DEF     NUCLEO_SD_MOUNT "/data/Vicino"

typedef struct { uint8_t mac[6]; int len; uint8_t buf[256]; } rxpkt_t;

// ---- engine state (guarded by s_lock) --------------------------------------
static bool             s_inited;
static SemaphoreHandle_t s_lock;
static QueueHandle_t    s_rxq;
static TaskHandle_t     s_task;
static volatile bool    s_run;
static char             s_inbox[96] = INBOX_DEF;

static nlink_ctx_t      s_ctx;             // evolved (Nucleo) protocol core

// transfer source/sink: a FILE* for files, or a memory buffer for commands.
static FILE            *s_file;
static uint8_t          s_mem[512];        // command payload (tiny)
static uint32_t         s_mem_len;
static bool             s_is_cmd;          // current transfer carries a command, not a file
static char             s_part[160];       // receiver .part path
static char             s_final[160];      // receiver final path

static nlink_status_t   s_st;
static uint32_t         s_rate_t0, s_rate_b0;

static bool             s_offer_pending;
static nlink_offer_t    s_offer;
static char             s_offer_from[NLINK_NAME_MAX + 1];

static bool             s_cmd_pending;
static char             s_cmd_buf[160];
static char             s_cmd_from[NLINK_NAME_MAX + 1];

static struct { uint8_t mac[6]; char name[NLINK_NAME_MAX + 1]; int proto; uint32_t seen; } s_peers[MAXPEERS];
static int              s_npeers;

// Bruce-mode (naive) send/recv run alongside the core.
static struct { bool active; FILE *f; bruce_msg_t msg; uint32_t total, sent, last_ms; uint8_t dst[6]; } s_bsend;
static struct { bool active; FILE *f; char path[160]; uint32_t total, got; } s_brecv;

static void lock(void)   { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGiveRecursive(s_lock); }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---- peers -----------------------------------------------------------------
static void ensure_peer(const uint8_t *mac) {
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; p.ifidx = WIFI_IF_STA; p.encrypt = false;
    esp_now_add_peer(&p);
}
static void add_peer(const uint8_t *mac, const char *name, int proto) {
    for (int i = 0; i < s_npeers; i++)
        if (!memcmp(s_peers[i].mac, mac, 6)) {
            s_peers[i].seen = now_ms(); s_peers[i].proto = proto;
            if (name && name[0]) strncpy(s_peers[i].name, name, NLINK_NAME_MAX);
            return;
        }
    if (s_npeers >= MAXPEERS) return;
    memcpy(s_peers[s_npeers].mac, mac, 6);
    snprintf(s_peers[s_npeers].name, sizeof s_peers[s_npeers].name, "%s",
             (name && name[0]) ? name : "");
    if (!s_peers[s_npeers].name[0])
        snprintf(s_peers[s_npeers].name, sizeof s_peers[s_npeers].name,
                 "%02X%02X%02X", mac[3], mac[4], mac[5]);
    s_peers[s_npeers].proto = proto; s_peers[s_npeers].seen = now_ms();
    s_npeers++;
}

// ---- path helpers ----------------------------------------------------------
static void mkdirs(const char *path) {
    char tmp[160]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0775); *p = '/'; }
    mkdir(tmp, 0775);
}
static void unique_dest(const char *name, char *out, int cap) {
    mkdirs(s_inbox);
    const char *base = name && name[0] ? name : "file.bin";
    char stem[96], ext[32]; const char *dot = strrchr(base, '.');
    if (dot) { snprintf(stem, sizeof stem, "%.*s", (int)(dot - base), base); snprintf(ext, sizeof ext, "%s", dot); }
    else     { snprintf(stem, sizeof stem, "%s", base); ext[0] = 0; }
    snprintf(out, cap, "%s/%s%s", s_inbox, stem, ext);
    struct stat stt; int i = 1;
    while (stat(out, &stt) == 0 && i < 1000) snprintf(out, cap, "%s/%s_%d%s", s_inbox, stem, i++, ext);
}
static uint32_t file_crc(const char *path, uint32_t len) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    uint32_t crc = 0, left = len; uint8_t b[256]; size_t n;
    while (left && (n = fread(b, 1, left < sizeof b ? left : sizeof b, f)) > 0) { crc = nlink_crc32(crc, b, n); left -= (uint32_t)n; }
    fclose(f); return crc;
}

// ---- core I/O seam ---------------------------------------------------------
static int io_send(void *u, const uint8_t *peer, const uint8_t *buf, int len) {
    (void)u; const uint8_t *dst = peer ? peer : BCAST; ensure_peer(dst);
    return esp_now_send(dst, buf, len) == ESP_OK ? 0 : -1;
}
static int io_read(void *u, uint32_t seq, uint8_t *buf) {
    (void)u; uint32_t off = seq * NLINK_CHUNK;
    if (s_is_cmd) {
        if (off >= s_mem_len) return 0;
        uint32_t n = s_mem_len - off; if (n > NLINK_CHUNK) n = NLINK_CHUNK;
        memcpy(buf, s_mem + off, n); return (int)n;
    }
    if (!s_file || fseek(s_file, (long)off, SEEK_SET) != 0) return -1;
    return (int)fread(buf, 1, NLINK_CHUNK, s_file);
}
static int io_write(void *u, uint32_t seq, const uint8_t *buf, int len) {
    (void)u; uint32_t off = seq * NLINK_CHUNK;
    if (s_is_cmd) {
        if (off + (uint32_t)len > sizeof s_mem) return -1;
        memcpy(s_mem + off, buf, len); if (off + len > s_mem_len) s_mem_len = off + len; return 0;
    }
    if (!s_file || fseek(s_file, (long)off, SEEK_SET) != 0) return -1;
    return fwrite(buf, 1, len, s_file) == (size_t)len ? 0 : -1;
}
static uint32_t io_verify(void *u) {
    (void)u;
    if (s_is_cmd) return nlink_crc32(0, s_mem, s_st.total);
    if (s_file) fflush(s_file);
    return file_crc(s_part, s_st.total);
}

static void reset_transfer(void) {
    if (s_file) { fclose(s_file); s_file = NULL; }
    s_is_cmd = false; s_mem_len = 0; s_part[0] = s_final[0] = 0;
}
static void finish(bool ok) {
    if (!ok) { s_st.state = NL_ST_FAIL; reset_transfer(); s_st.active = 0; return; }
    s_st.state = NL_ST_DONE; s_st.done = s_st.total;
    if (!s_st.sending) {
        if (s_is_cmd) {                                   // received a command -> gate it
            snprintf(s_cmd_buf, sizeof s_cmd_buf, "%.*s", (int)s_mem_len, (char *)s_mem);
            snprintf(s_cmd_from, sizeof s_cmd_from, "%s", s_offer_from);
            s_cmd_pending = true;
        } else if (s_part[0] && s_final[0]) {             // received a file -> publish from .part
            if (s_file) { fclose(s_file); s_file = NULL; }
            rename(s_part, s_final);
        }
    }
    reset_transfer(); s_st.active = 0;
}

// ---- core events -----------------------------------------------------------
static void io_event(void *u, int ev, const nlink_evt_t *e) {
    (void)u;
    switch (ev) {
        case NL_EV_PEER: add_peer(e->peer, e->name, NLINK_PROTO_NUCLEO); break;
        case NL_EV_OFFER:
            s_offer = e->offer;
            snprintf(s_offer_from, sizeof s_offer_from, "%s", e->name ? e->name : "");
            s_offer_pending = true;                       // UI must call offer_answer()
            break;
        case NL_EV_PROGRESS:
            s_st.done = e->done_bytes; s_st.total = e->total_bytes;
            { uint32_t t = now_ms(); if (t > s_rate_t0 + 250) {
                s_st.rate_bps = (s_st.done - s_rate_b0) * 1000 / (t - s_rate_t0);
                s_rate_t0 = t; s_rate_b0 = s_st.done; } }
            break;
        case NL_EV_COMPLETE: finish(true);  break;
        case NL_EV_FAILED:   s_st.reason = e->reason; finish(false); break;
        default: break;
    }
}
static const nlink_io_t IO = { io_send, io_read, io_write, io_verify, io_event, NULL };

// ---- Bruce mode ------------------------------------------------------------
static void bruce_send_pump(void) {
    if (!s_bsend.active) return;
    uint32_t t = now_ms(); if (t - s_bsend.last_ms < BRUCE_PACE_MS) return;
    s_bsend.last_ms = t;
    uint8_t chunk[BRUCE_DATA_SIZE];
    size_t n = fread(chunk, 1, BRUCE_DATA_SIZE, s_bsend.f);
    bruce_file_chunk(&s_bsend.msg, chunk, (uint32_t)n);
    ensure_peer(s_bsend.dst);
    esp_now_send(s_bsend.dst, (const uint8_t *)&s_bsend.msg, BRUCE_MSG_SIZE);
    s_st.done = s_bsend.msg.bytesSent; s_st.total = s_bsend.msg.totalBytes;
    if (s_bsend.msg.done) {
        fclose(s_bsend.f); s_bsend.f = NULL; s_bsend.active = false;
        s_st.state = NL_ST_DONE; s_st.active = 0;
    }
}
static void bruce_on_frame(const uint8_t *mac, const uint8_t *data) {
    const bruce_msg_t *m = (const bruce_msg_t *)data;
    if (m->ping) {                                        // reply pong + remember peer
        bruce_msg_t pong; bruce_build_pong(&pong); ensure_peer(mac);
        esp_now_send(mac, (const uint8_t *)&pong, BRUCE_MSG_SIZE);
        add_peer(mac, NULL, NLINK_PROTO_BRUCE); return;
    }
    if (m->pong) { add_peer(mac, NULL, NLINK_PROTO_BRUCE); return; }
    // file frame (Bruce sends filename/filepath every frame)
    if (!s_brecv.active) {
        char nm[BRUCE_FILENAME_SIZE + 1]; snprintf(nm, sizeof nm, "%.*s", BRUCE_FILENAME_SIZE, m->filename);
        unique_dest(nm[0] ? nm : "bruce.bin", s_brecv.path, sizeof s_brecv.path);
        s_brecv.f = fopen(s_brecv.path, "wb");
        s_brecv.active = s_brecv.f != NULL; s_brecv.total = m->totalBytes; s_brecv.got = 0;
        s_st.active = 1; s_st.sending = 0; s_st.proto = NLINK_PROTO_BRUCE; s_st.state = NL_ST_RUN;
        snprintf(s_st.name, sizeof s_st.name, "%s", nm);
    }
    if (s_brecv.f && m->dataSize) {
        fwrite(m->data, 1, m->dataSize, s_brecv.f);
        s_brecv.got += m->dataSize; s_st.done = s_brecv.got; s_st.total = s_brecv.total;
    }
    if (m->done) {
        if (s_brecv.f) { fclose(s_brecv.f); s_brecv.f = NULL; }
        s_brecv.active = false; s_st.state = NL_ST_DONE; s_st.active = 0;
    }
}

// ---- RX callback (Wi-Fi task ctx) -> queue ---------------------------------
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_rxq || len <= 0 || len > 256) return;
    rxpkt_t p; memcpy(p.mac, info->src_addr, 6); p.len = len; memcpy(p.buf, data, len);
    xQueueSend(s_rxq, &p, 0);
}

// ---- service pump (own task) -----------------------------------------------
static void pump_once(void) {
    rxpkt_t p;
    while (xQueueReceive(s_rxq, &p, 0) == pdTRUE) {
        if (bruce_is_msg(p.buf, p.len)) { bruce_on_frame(p.mac, p.buf); }
        else if (p.len >= NLINK_HDR && p.buf[0] == 'N' && p.buf[1] == 'L')
            nlink_on_frame(&s_ctx, p.mac, p.buf, p.len, now_ms());
    }
    nlink_tick(&s_ctx, now_ms());
    bruce_send_pump();
    s_st.state = (s_ctx.role && nlink_busy(&s_ctx)) ? s_ctx.state : s_st.state;
}
static void svc_task(void *arg) {
    (void)arg;
    while (s_run) { lock(); pump_once(); unlock(); vTaskDelay(pdMS_TO_TICKS(5)); }
    s_task = NULL; vTaskDelete(NULL);
}

// ---- channel ---------------------------------------------------------------
static void lock_channel(void) {
    // Associated STA: ESP-NOW already rides the AP channel (peers use channel 0 = current). If we're
    // NOT associated, two idle devices won't meet — park both on a known channel so discovery works.
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)         // not connected as STA
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}

// ---- public API ------------------------------------------------------------
bool nlink_svc_start(void) {
    if (s_inited) return true;
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_rxq  = xQueueCreate(16, sizeof(rxpkt_t));
    if (!s_lock || !s_rxq) { ESP_LOGE(TAG, "alloc"); goto fail; }
    // esp_now is a global singleton: another component (the Swarm app, or our own web-API path) may
    // have left it inited — treat EXIST as success and just (re)claim the single recv callback.
    { esp_err_t e = esp_now_init();
      if (e != ESP_OK && e != ESP_ERR_ESPNOW_EXIST) { ESP_LOGE(TAG, "esp_now_init %s", esp_err_to_name(e)); goto fail; } }
    esp_now_register_recv_cb(recv_cb);
    ensure_peer(BCAST);
    lock_channel();
    nlink_init(&s_ctx, &IO, nlink_svc_name());
    memset(&s_st, 0, sizeof s_st);
    s_npeers = 0; s_offer_pending = s_cmd_pending = false;
    s_inited = true; s_run = true;
    // svc_task STREAMS files through SD (fopen/fwrite/fread + CRC) and builds the protocol frames on its
    // stack: FATFS needs real headroom (cf. nucleo_eth's 6144). 4096 overflows mid-transfer -> the device
    // looks frozen. Keep it generous; the app is exclusive so RAM is free.
    if (xTaskCreate(svc_task, "vicino", 7168, NULL, tskIDLE_PRIORITY + 2, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "svc task"); s_inited = false; s_run = false;
        esp_now_unregister_recv_cb(); esp_now_deinit(); goto fail;
    }
    return true;
fail:
    if (s_rxq)  { vQueueDelete(s_rxq);  s_rxq = NULL; }
    if (s_lock) { vSemaphoreDelete(s_lock); s_lock = NULL; }
    return false;
}
void nlink_svc_stop(void) {
    if (!s_inited) return;
    s_run = false; for (int i = 0; i < 40 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    lock();
    reset_transfer();
    if (s_bsend.f) { fclose(s_bsend.f); s_bsend.f = NULL; } s_bsend.active = false;
    if (s_brecv.f) { fclose(s_brecv.f); s_brecv.f = NULL; } s_brecv.active = false;
    esp_now_unregister_recv_cb(); esp_now_deinit();
    unlock();
    if (s_rxq)  { vQueueDelete(s_rxq);  s_rxq = NULL; }
    if (s_lock) { vSemaphoreDelete(s_lock); s_lock = NULL; }
    s_inited = false;
}
void nlink_svc_pump(void) { /* engine self-pumps on its task; kept for callers that want a nudge */ }

const char *nlink_svc_name(void) {
    extern const char *nucleo_setup_device_name(void);
    const char *n = nucleo_setup_device_name(); return (n && n[0]) ? n : "nucleo";
}
int nlink_svc_channel(void) {
    uint8_t pri = 0; wifi_second_chan_t sec; esp_wifi_get_channel(&pri, &sec); return pri;
}
const char *nlink_svc_inbox(void) { return s_inbox; }
void nlink_svc_set_inbox(const char *path) { if (path && path[0]) { snprintf(s_inbox, sizeof s_inbox, "%s", path); } }

void nlink_svc_discover(nlink_proto_t proto) {
    lock();
    if (proto == NLINK_PROTO_BRUCE) {
        bruce_msg_t ping; bruce_build_ping(&ping); ensure_peer(BCAST);
        esp_now_send(BCAST, (const uint8_t *)&ping, BRUCE_MSG_SIZE);
    } else nlink_discover(&s_ctx);
    unlock();
}
int  nlink_svc_peer_count(void) { return s_npeers; }
const char *nlink_svc_peer_name(int i)  { return (i >= 0 && i < s_npeers) ? s_peers[i].name : ""; }
const uint8_t *nlink_svc_peer_mac(int i){ return (i >= 0 && i < s_npeers) ? s_peers[i].mac : BCAST; }
int  nlink_svc_peer_proto(int i)        { return (i >= 0 && i < s_npeers) ? s_peers[i].proto : 0; }
void nlink_svc_peer_clear(void) { lock(); s_npeers = 0; unlock(); }

static bool begin_send_common(const char *path, uint32_t *out_size) {
    struct stat stt; if (stat(path, &stt) != 0) return false;
    s_file = fopen(path, "rb"); if (!s_file) return false;
    *out_size = (uint32_t)stt.st_size; return true;
}

bool nlink_svc_send_file(int peer_i, const char *path, nlink_proto_t proto) {
    if (peer_i < 0 || peer_i >= s_npeers) return false;
    lock();
    if (s_st.active || s_bsend.active) { unlock(); return false; }
    uint32_t size = 0;
    if (!begin_send_common(path, &size)) { unlock(); return false; }
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    s_st.active = 1; s_st.sending = 1; s_st.proto = proto; s_st.reason = 0;
    s_st.done = 0; s_st.total = size; s_rate_t0 = now_ms(); s_rate_b0 = 0;
    snprintf(s_st.name, sizeof s_st.name, "%s", base);
    snprintf(s_st.peer, sizeof s_st.peer, "%s", s_peers[peer_i].name);

    if (proto == NLINK_PROTO_BRUCE) {
        char dir[64] = ""; const char *slash = strrchr(path, '/');
        if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - path), path);
        bruce_file_init(&s_bsend.msg, base, dir, size);
        memcpy(s_bsend.dst, s_peers[peer_i].mac, 6);
        s_bsend.f = s_file; s_file = NULL;             // Bruce path owns the FILE*
        s_bsend.total = size; s_bsend.sent = 0; s_bsend.last_ms = 0; s_bsend.active = true;
        s_st.state = NL_ST_RUN;
    } else {
        fclose(s_file); s_file = NULL;                 // crc pre-pass, then reopen for streaming
        uint32_t crc = file_crc(path, size);
        s_file = fopen(path, "rb"); if (!s_file) { s_st.active = 0; unlock(); return false; }
        s_is_cmd = false;
        nlink_send_begin(&s_ctx, s_peers[peer_i].mac, esp_random(), base, size, NL_MODE_FILE, crc);
    }
    unlock(); return true;
}

void nlink_svc_listen(nlink_proto_t proto) {
    lock(); (void)proto; nlink_recv_listen(&s_ctx); unlock();   // inbound auto-detects format anyway
}
bool nlink_svc_offer_pending(char *name, int ncap, char *from, int fcap, uint32_t *size) {
    lock(); bool p = s_offer_pending;
    if (p) { snprintf(name, ncap, "%s", s_offer.name); snprintf(from, fcap, "%s", s_offer_from);
             if (size) *size = s_offer.total_size; }
    unlock(); return p;
}
void nlink_svc_offer_answer(bool accept) {
    lock();
    if (!s_offer_pending) { unlock(); return; }
    s_offer_pending = false;
    if (!accept) { nlink_recv_accept(&s_ctx, false, 0); unlock(); return; }
    s_st.active = 1; s_st.sending = 0; s_st.proto = NLINK_PROTO_NUCLEO; s_st.reason = 0;
    s_st.done = 0; s_st.total = s_offer.total_size; s_rate_t0 = now_ms(); s_rate_b0 = 0;
    snprintf(s_st.name, sizeof s_st.name, "%s", s_offer.name);
    snprintf(s_st.peer, sizeof s_st.peer, "%s", s_offer_from);
    uint32_t have_prefix = 0;
    if (s_offer.mode == NL_MODE_CMD) {
        s_is_cmd = true; s_mem_len = 0;
    } else {
        s_is_cmd = false;
        unique_dest(s_offer.name, s_final, sizeof s_final);
        snprintf(s_part, sizeof s_part, "%s.part", s_final);
        struct stat stt;                               // resume: reuse a matching .part
        if (stat(s_part, &stt) == 0) {
            have_prefix = (uint32_t)stt.st_size / NLINK_CHUNK;
            if (have_prefix > s_offer.n_chunks) have_prefix = 0;
        }
        s_file = fopen(s_part, have_prefix ? "rb+" : "wb+");
        if (!s_file) { nlink_recv_accept(&s_ctx, false, 0); s_st.active = 0; unlock(); return; }
    }
    nlink_recv_accept(&s_ctx, true, have_prefix);
    unlock();
}

bool nlink_svc_send_cmd(int peer_i, const char *cmd, nlink_proto_t proto) {
    if (peer_i < 0 || peer_i >= s_npeers || !cmd || !cmd[0]) return false;
    lock();
    if (s_st.active || s_bsend.active) { unlock(); return false; }
    uint32_t len = (uint32_t)strnlen(cmd, sizeof s_mem - 1);
    memcpy(s_mem, cmd, len); s_mem_len = len; s_is_cmd = true;
    s_st.active = 1; s_st.sending = 1; s_st.proto = proto; s_st.done = 0; s_st.total = len;
    snprintf(s_st.name, sizeof s_st.name, "%.40s", cmd);
    snprintf(s_st.peer, sizeof s_st.peer, "%s", s_peers[peer_i].name);
    if (proto == NLINK_PROTO_BRUCE) {                  // Bruce: a single non-file message
        bruce_msg_t m; memset(&m, 0, sizeof m);
        m.totalBytes = m.bytesSent = m.dataSize = len; m.done = 1;
        memcpy(m.data, cmd, len < BRUCE_DATA_SIZE ? len : BRUCE_DATA_SIZE);
        ensure_peer(s_peers[peer_i].mac);
        esp_now_send(s_peers[peer_i].mac, (const uint8_t *)&m, BRUCE_MSG_SIZE);
        s_st.state = NL_ST_DONE; s_st.active = 0; s_is_cmd = false;
    } else {
        uint32_t crc = nlink_crc32(0, s_mem, len);
        nlink_send_begin(&s_ctx, s_peers[peer_i].mac, esp_random(), cmd, len, NL_MODE_CMD, crc);
    }
    unlock(); return true;
}
bool nlink_svc_cmd_pending(char *cmd, int ccap, char *from, int fcap) {
    lock(); bool p = s_cmd_pending;
    if (p) { snprintf(cmd, ccap, "%s", s_cmd_buf); snprintf(from, fcap, "%s", s_cmd_from); }
    unlock(); return p;
}
void nlink_svc_cmd_confirm(bool ok) {
    lock(); s_cmd_pending = false;
    if (ok) { extern void nucleo_anima_app_ask(const char *q); nucleo_anima_app_ask(s_cmd_buf); }
    unlock();
}

void nlink_svc_status(nlink_status_t *out) { lock(); *out = s_st; unlock(); }
void nlink_svc_cancel(void) {
    lock();
    if (s_ctx.role && nlink_busy(&s_ctx)) nlink_abort(&s_ctx);
    if (s_bsend.active) { if (s_bsend.f) { fclose(s_bsend.f); s_bsend.f = NULL; } s_bsend.active = false; }
    if (s_brecv.active) { if (s_brecv.f) { fclose(s_brecv.f); s_brecv.f = NULL; } s_brecv.active = false; }
    reset_transfer(); s_st.active = 0; s_st.state = NL_ST_FAIL; s_st.reason = NL_R_ABORT;
    unlock();
}

// xz_core.c — the XiaoZhi client (xz_core.h). Protocol: the service's docs/websocket.md and docs/ota.md
// (github.com/78/xiaozhi-esp32, MIT): an OTA POST gives the WebSocket URL + token (and an activation code until the
// owner binds the device), then per turn: hello / hello, listen start (manual), Opus frames up, listen stop,
// stt / llm / tts JSON and Opus frames down.
#if defined(NAP_MODULE) || !defined(ESP_PLATFORM)   // store only: compiled as a module and by the host test
#include "xz_core.h"
#include "nucleo_tcp.h"
#include "nucleo_mic.h"
#include "nucleo_secret.h"
#include "nucleo_audio.h"
#include "opus.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// libopus's pseudostack (built with NONTHREADSAFE_PSEUDOSTACK): we point it at our own block before each codec call
// and drop it after, so its temporaries live in the heap only while a codec does.
extern char *global_stack, *scratch_ptr;

#define RATE         16000
#define FRAME        960            // 60 ms at 16 kHz: what we send, and what one received frame decodes to
#define MAX_REC_MS   15000
#define ENC_PSEUDO   (16 * 1024)    // measured peak 12.6 KB for a 60 ms VOIP frame at complexity 0
#define DEC_PSEUDO   (8 * 1024)     // measured peak 5.5 KB
#define GUARD        1024           // the pseudostack's last KB stays untouched, or we stop (checked per frame)
#define WS_MAX       4096           // largest WebSocket payload we keep (JSON, an Opus frame); bigger is skipped

static xz_view_t V;
static char s_dir[64], s_lang[12];
static char s_ws_url[160], s_ws_token[200], s_ota_url[160], s_challenge[48];
static char s_cid[40], s_mac[20], s_session[64];
static int s_tcp = -1;                                  // the connection in use (OTA or WebSocket)
static FILE *s_rec, *s_pkts;                            // the take, its Opus packets / the answer's packets
static OpusEncoder *s_enc;
static OpusDecoder *s_dec;
static char *s_pseudo;
static int s_pseudo_size;
static int16_t *s_pcm;                                  // FRAME (encode) or 2 * FRAME (decode) samples
static long s_total, s_done;                            // encode / play progress
static int64_t s_t0, s_next_poll;
static uint8_t *s_ws_buf;                               // WS_MAX + 1, while a WebSocket is open
static bool s_reply_audio, s_pcm_open, s_answer_done, s_rec_end;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void wdt(void) { esp_task_wdt_reset(); }

static void path(char *out, int cap, const char *name) { snprintf(out, cap, "%s/%s", s_dir, name); }

static void heap_mark(void)
{
    V.heap_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    V.heap_block = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

static const char *const STATE_NAME[] = { "setup", "ota", "activate", "idle", "recording", "encoding", "exchange",
                                          "waiting", "playing", "error" };

static void set_state(xz_state_t s, const char *status)
{
    V.state = s;
    if (status) snprintf(V.status, sizeof V.status, "%s", status);
    heap_mark();
    V.seq++;
    // One line per phase: the numbers a first run on a device is for (free heap, largest block).
    ESP_LOGW("xiaozhi", "%s: %s (heap %u, block %u)", STATE_NAME[s], V.status, (unsigned)V.heap_free, (unsigned)V.heap_block);
}

static void fail(const char *what, int code)
{
    char m[72];
    if (code) snprintf(m, sizeof m, "%s (%d)", what, code); else snprintf(m, sizeof m, "%s", what);
    set_state(XZ_ERROR, m);
}

// ---- configuration: <dir>/config.json (server, ws url); the token and the identity in nucleo_secret when they fit
static void config_save(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return;
    cJSON_AddStringToObject(o, "server", V.server);
    cJSON_AddStringToObject(o, "ws", s_ws_url);
    // A token that fits a secret (63 bytes) stays off the SD card; a longer one has to live in the file.
    if (strlen(s_ws_token) < 64) nucleo_secret_set("token", s_ws_token[0] ? s_ws_token : NULL);
    else { nucleo_secret_set("token", NULL); cJSON_AddStringToObject(o, "token", s_ws_token); }
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) return;
    char p[96]; path(p, sizeof p, "config.json");
    FILE *f = fopen(p, "wb");
    if (f) { fputs(txt, f); fclose(f); }
    cJSON_free(txt);
}

static void config_load(void)
{
    V.server[0] = s_ws_url[0] = s_ws_token[0] = 0;
    char p[96]; path(p, sizeof p, "config.json");
    FILE *f = fopen(p, "rb");
    if (f) {
        char buf[600]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
        cJSON *o = cJSON_Parse(buf);
        if (o) {
            const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, "server"));
            if (s) snprintf(V.server, sizeof V.server, "%s", s);
            s = cJSON_GetStringValue(cJSON_GetObjectItem(o, "ws"));
            if (s) snprintf(s_ws_url, sizeof s_ws_url, "%s", s);
            s = cJSON_GetStringValue(cJSON_GetObjectItem(o, "token"));
            if (s) snprintf(s_ws_token, sizeof s_ws_token, "%s", s);
            cJSON_Delete(o);
        }
    }
    if (!s_ws_token[0]) nucleo_secret_get("token", s_ws_token, sizeof s_ws_token);
}

// The identity the service sees: a random UUID v4 (Client-Id) and a random locally administered MAC (Device-Id),
// made once and kept in secrets. Never the radio's real MAC: nothing ties the service's record to this hardware.
static void identity(void)
{
    if (!nucleo_secret_get("cid", s_cid, sizeof s_cid) || strlen(s_cid) != 36) {
        uint8_t r[16];
        for (int i = 0; i < 16; i += 4) { uint32_t x = esp_random(); memcpy(r + i, &x, 4); }
        r[6] = (r[6] & 0x0F) | 0x40; r[8] = (r[8] & 0x3F) | 0x80;
        snprintf(s_cid, sizeof s_cid, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
        nucleo_secret_set("cid", s_cid);
    }
    if (!nucleo_secret_get("mac", s_mac, sizeof s_mac) || strlen(s_mac) != 17) {
        uint8_t m[6];
        for (int i = 0; i < 6; i++) m[i] = (uint8_t)esp_random();
        m[0] = (m[0] & 0xFC) | 0x02;                     // locally administered, unicast
        snprintf(s_mac, sizeof s_mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
        nucleo_secret_set("mac", s_mac);
    }
}

// ---- URLs and the transport ------------------------------------------------------------------------------------
typedef struct { bool tls, ws; char host[80]; int port; char path[120]; } url_t;

static bool url_parse(const char *u, url_t *o)
{
    memset(o, 0, sizeof *o);
    if (!strncmp(u, "https://", 8)) { o->tls = true; u += 8; }
    else if (!strncmp(u, "http://", 7)) u += 7;
    else if (!strncmp(u, "wss://", 6)) { o->tls = o->ws = true; u += 6; }
    else if (!strncmp(u, "ws://", 5)) { o->ws = true; u += 5; }
    else return false;
    const char *slash = strchr(u, '/');
    const char *end = slash ? slash : u + strlen(u);
    const char *colon = memchr(u, ':', (size_t)(end - u));
    const char *he = colon ? colon : end;
    if (he == u || he - u >= (int)sizeof o->host) return false;
    memcpy(o->host, u, (size_t)(he - u));
    o->port = colon ? atoi(colon + 1) : (o->tls ? 443 : 80);
    if (o->port <= 0 || o->port > 65535) return false;
    snprintf(o->path, sizeof o->path, "%s", slash ? slash : "/");
    return true;
}

static int net_open(const url_t *u)
{
    return u->tls ? nucleo_tcp_connect_tls(u->host, u->port, 10000) : nucleo_tcp_connect(u->host, u->port, 8000);
}

static void net_close(void)
{
    if (s_tcp > 0) nucleo_tcp_close(s_tcp);
    s_tcp = -1;
}

static bool send_all(const void *p, int n) { return nucleo_tcp_send(s_tcp, p, n, 8000) == n; }

static bool recv_exact(uint8_t *p, int n, int timeout_ms)
{
    const int64_t until = now_ms() + timeout_ms;
    while (n > 0) {
        const int left = (int)(until - now_ms());
        if (left <= 0) return false;
        const int r = nucleo_tcp_recv(s_tcp, p, n, left > 500 ? 500 : left);
        if (r == NUCLEO_TCP_TIMEOUT) continue;
        if (r <= 0) return false;
        p += r; n -= r;
    }
    return true;
}

// ---- HTTP: one request, Connection: close, the whole answer read into `out` (de-chunked) -----------------------
static int http_post(const char *url, const char *body, char *out, int cap)
{
    url_t u;
    if (!url_parse(url, &u) || u.ws) return -100;
    s_tcp = net_open(&u);
    if (s_tcp < 0) { const int e = s_tcp; s_tcp = -1; return e; }
    char head[640];
    const int bl = (int)strlen(body);
    const int hl = snprintf(head, sizeof head,
        "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: nucleoos-cardputer/0.1\r\nAccept-Language: %s\r\n"
        "Device-Id: %s\r\nClient-Id: %s\r\nActivation-Version: 1\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", u.path, u.host, s_lang, s_mac, s_cid, bl);
    if (hl >= (int)sizeof head || !send_all(head, hl) || !send_all(body, bl)) { net_close(); return -101; }
    int n = 0;
    const int64_t until = now_ms() + 15000;
    while (n < cap - 1 && now_ms() < until) {
        const int r = nucleo_tcp_recv(s_tcp, out + n, cap - 1 - n, 500);
        if (r == NUCLEO_TCP_TIMEOUT) continue;
        if (r <= 0) break;
        n += r;
    }
    net_close();
    out[n] = 0;
    if (n < 12 || strncmp(out, "HTTP/1.", 7)) return -102;
    const int status = atoi(out + 9);
    char *b = strstr(out, "\r\n\r\n");
    if (!b) return -102;
    b += 4;
    const bool chunked = strcasestr(out, "transfer-encoding: chunked") && strcasestr(out, "transfer-encoding: chunked") < b;
    if (chunked) {                                       // size CRLF data CRLF ... 0 CRLF
        char *w = out, *r = b;
        for (;;) {
            const long sz = strtol(r, NULL, 16);
            char *nl = strstr(r, "\r\n");
            if (sz <= 0 || !nl || nl + 2 + sz > out + n) break;
            memmove(w, nl + 2, (size_t)sz); w += sz; r = nl + 2 + sz + 2;
        }
        *w = 0;
    } else memmove(out, b, strlen(b) + 1);
    return status;
}

// ---- WebSocket (RFC 6455, client side: our frames masked, one frame per message) --------------------------------
static void b64(const uint8_t *in, int n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63]; out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

static bool ws_send(int opcode, const void *data, int len)
{
    uint8_t h[14]; int hl = 0;
    h[hl++] = (uint8_t)(0x80 | opcode);
    if (len < 126) h[hl++] = (uint8_t)(0x80 | len);
    else { h[hl++] = 0x80 | 126; h[hl++] = (uint8_t)(len >> 8); h[hl++] = (uint8_t)len; }
    const uint32_t k = esp_random();
    uint8_t mask[4]; memcpy(mask, &k, 4);
    memcpy(h + hl, mask, 4); hl += 4;
    if (!send_all(h, hl)) return false;
    uint8_t chunk[256];
    const uint8_t *p = (const uint8_t *)data;
    for (int off = 0; off < len; off += (int)sizeof chunk) {
        const int n = len - off < (int)sizeof chunk ? len - off : (int)sizeof chunk;
        for (int i = 0; i < n; i++) chunk[i] = p[off + i] ^ mask[(off + i) & 3];
        if (!send_all(chunk, n)) return false;
    }
    return true;
}

static bool ws_text(cJSON *o)
{
    char *t = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!t) return false;
    const bool ok = ws_send(1, t, (int)strlen(t));
    cJSON_free(t);
    return ok;
}

// One frame: its opcode (1 text, 2 binary, 8 close...) and payload in s_ws_buf; -1 on a broken stream.
// A ping is answered here. A payload over WS_MAX is read and dropped (returns opcode 0).
static int ws_recv(int *len, int timeout_ms)
{
    uint8_t h[8];
    if (!recv_exact(h, 2, timeout_ms)) return -1;
    const int op = h[0] & 0x0F;
    uint64_t n = h[1] & 0x7F;
    if (n == 126) { if (!recv_exact(h, 2, 3000)) return -1; n = (uint64_t)h[0] << 8 | h[1]; }
    else if (n == 127) { if (!recv_exact(h, 8, 3000)) return -1; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | h[i]; }
    if (h[1] & 0x80 && n < 126) { uint8_t m[4]; if (!recv_exact(m, 4, 3000)) return -1; }   // servers never mask
    if (n > WS_MAX) {
        uint8_t junk[256];
        while (n) { const int k = n > sizeof junk ? (int)sizeof junk : (int)n; if (!recv_exact(junk, k, 3000)) return -1; n -= k; }
        *len = 0;
        return 0;
    }
    if (n && !recv_exact(s_ws_buf, (int)n, 5000)) return -1;
    s_ws_buf[n] = 0;
    *len = (int)n;
    if (op == 9) ws_send(10, s_ws_buf, (int)n);           // ping -> pong
    return op;
}

static bool ws_open(void)
{
    url_t u;
    if (!url_parse(s_ws_url, &u) || !u.ws) { fail("bad ws url", 0); return false; }
    s_tcp = net_open(&u);
    if (s_tcp < 0) { const int e = s_tcp; s_tcp = -1; fail(e == NUCLEO_TCP_TLS ? "certificate refused" : "no connection", e); return false; }
    uint8_t key[16];
    for (int i = 0; i < 16; i += 4) { uint32_t x = esp_random(); memcpy(key + i, &x, 4); }
    char k64[28]; b64(key, 16, k64);
    char auth[220] = "";
    if (s_ws_token[0]) snprintf(auth, sizeof auth, "Authorization: %s%s\r\n", strchr(s_ws_token, ' ') ? "" : "Bearer ", s_ws_token);
    char head[900];
    const int hl = snprintf(head, sizeof head,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n%sProtocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n\r\n",
        u.path, u.host, k64, auth, s_mac, s_cid);
    if (hl >= (int)sizeof head || !send_all(head, hl)) { net_close(); fail("send failed", 0); return false; }
    // The answer's headers, byte by byte (nothing after them is read here): 101 or nothing.
    char resp[512]; int n = 0;
    while (n < (int)sizeof resp - 1) {
        if (!recv_exact((uint8_t *)resp + n, 1, 8000)) break;
        n++;
        if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    resp[n] = 0;
    if (strncmp(resp, "HTTP/1.1 101", 12)) {
        const int st = n > 12 ? atoi(resp + 9) : 0;
        net_close(); fail(st == 401 ? "token refused: activate again" : "websocket refused", st); return false;
    }
    s_ws_buf = (uint8_t *)malloc(WS_MAX + 1);
    if (!s_ws_buf) { net_close(); fail("no memory", 0); return false; }
    return true;
}

static void ws_close(void)
{
    if (s_tcp > 0 && s_ws_buf) ws_send(8, "", 0);
    net_close();
    free(s_ws_buf); s_ws_buf = NULL;
}

// ---- OTA: the WebSocket address, the token, the activation code ------------------------------------------------
static void ota_body(char *out, int cap)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "version", 2);
    cJSON_AddStringToObject(o, "language", s_lang);
    cJSON_AddNumberToObject(o, "flash_size", 8 * 1024 * 1024);
    cJSON_AddStringToObject(o, "mac_address", s_mac);
    cJSON_AddStringToObject(o, "uuid", s_cid);
    cJSON_AddStringToObject(o, "chip_model_name", "esp32s3");
    cJSON *app = cJSON_AddObjectToObject(o, "application");
    cJSON_AddStringToObject(app, "name", "xiaozhi");
    cJSON_AddStringToObject(app, "version", "2.0.0");
    cJSON *board = cJSON_AddObjectToObject(o, "board");
    cJSON_AddStringToObject(board, "type", "m5stack-cardputer-adv");
    cJSON_AddStringToObject(board, "name", "nucleoos-cardputer");
    char *t = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    snprintf(out, cap, "%s", t ? t : "{}");
    if (t) cJSON_free(t);
}

// The Solo boot joins Wi-Fi in the background: the first polls can come before it. Wait for it (said on screen)
// instead of failing; true when the network is there.
static bool network_ready(void)
{
    if (nucleo_tcp_network_up()) return true;
    if (strcmp(V.status, "waiting for Wi-Fi")) { snprintf(V.status, sizeof V.status, "waiting for Wi-Fi"); V.seq++; }
    return false;
}

static void ota_step(void)
{
    if (!network_ready()) return;
    char body[512]; ota_body(body, sizeof body);
    char *resp = (char *)malloc(6144);
    if (!resp) { fail("no memory", 0); return; }
    const int st = http_post(s_ota_url, body, resp, 6144);
    if (st != 200) { free(resp); fail(st == NUCLEO_TCP_TLS ? "certificate refused" : "activation server", st); return; }
    cJSON *o = cJSON_Parse(resp);
    free(resp);
    if (!o) { fail("bad answer", 0); return; }
    cJSON *ws = cJSON_GetObjectItem(o, "websocket");
    const char *wu = cJSON_GetStringValue(cJSON_GetObjectItem(ws, "url"));
    const char *wt = cJSON_GetStringValue(cJSON_GetObjectItem(ws, "token"));
    if (wu) snprintf(s_ws_url, sizeof s_ws_url, "%s", wu);
    if (wt) snprintf(s_ws_token, sizeof s_ws_token, "%s", wt);
    cJSON *act = cJSON_GetObjectItem(o, "activation");
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(act, "code"));
    const char *ch = cJSON_GetStringValue(cJSON_GetObjectItem(act, "challenge"));
    snprintf(V.code, sizeof V.code, "%s", code ? code : "");
    snprintf(s_challenge, sizeof s_challenge, "%s", ch ? ch : "");
    cJSON_Delete(o);
    config_save();
    if (V.code[0]) { set_state(XZ_ACTIVATE, "bind the device"); ESP_LOGW("xiaozhi", "activation code %s", V.code); s_next_poll = now_ms() + 3000; }
    else if (!s_ws_url[0]) fail("no websocket offered", 0);
    else set_state(XZ_IDLE, "ready");
}

static void activate_step(void)
{
    if (now_ms() < s_next_poll || !network_ready()) return;
    char url[176];
    const size_t l = strlen(s_ota_url);
    snprintf(url, sizeof url, "%s%sactivate", s_ota_url, l && s_ota_url[l - 1] == '/' ? "" : "/");
    char resp[512];
    const int st = http_post(url, "{}", resp, sizeof resp);
    s_next_poll = now_ms() + 5000;
    if (st == 200) { V.code[0] = 0; set_state(XZ_OTA, "activated"); }   // ask again for the settings
    else if (st != 202) { V.seq++; snprintf(V.status, sizeof V.status, "waiting (%d)", st); }
}

// ---- codec memory ------------------------------------------------------------------------------------------------
static bool pseudo_take(int size)
{
    s_pseudo = (char *)malloc((size_t)size);
    if (!s_pseudo) return false;
    s_pseudo_size = size;
    memset(s_pseudo + size - GUARD, 0x5A, GUARD);
    return true;
}
static void pseudo_point(void) { global_stack = scratch_ptr = s_pseudo; }
static bool pseudo_intact(void)
{
    for (int i = s_pseudo_size - GUARD; i < s_pseudo_size; i++) if ((uint8_t)s_pseudo[i] != 0x5A) return false;
    return true;
}
static void codec_free(void)
{
    free(s_enc); s_enc = NULL;
    free(s_dec); s_dec = NULL;
    free(s_pseudo); s_pseudo = NULL;
    free(s_pcm); s_pcm = NULL;
    global_stack = scratch_ptr = NULL;
}

// ---- a turn ------------------------------------------------------------------------------------------------------
static void files_close(void)
{
    if (s_rec) { fclose(s_rec); s_rec = NULL; }
    if (s_pkts) { fclose(s_pkts); s_pkts = NULL; }
}

void xz_talk(bool on)
{
    if (on) {
        if (V.state != XZ_IDLE && V.state != XZ_ERROR) return;
        if (!s_ws_url[0]) { set_state(XZ_SETUP, "no server"); return; }
        char p[96]; path(p, sizeof p, "take.pcm");
        s_rec = fopen(p, "wb");
        if (!s_rec) { fail("SD card", 0); return; }
        const int e = nucleo_mic_open(NUCLEO_MIC_RATE);
        if (e) { files_close(); fail(e == NUCLEO_MIC_DENIED ? "no microphone permission" : "microphone busy", e); return; }
        s_pcm = (int16_t *)malloc(FRAME * sizeof(int16_t));
        if (!s_pcm) { nucleo_mic_close(); files_close(); fail("no memory", 0); return; }
        V.user[0] = V.reply[0] = V.emotion[0] = 0;
        V.rec_ms = 0; V.level = 0;
        s_t0 = now_ms();
        set_state(XZ_RECORDING, "listening");
    } else if (V.state == XZ_RECORDING) {
        nucleo_mic_close();
        files_close();
        free(s_pcm); s_pcm = NULL;
        if (V.rec_ms < 400) { set_state(XZ_IDLE, "too short: hold while you speak"); return; }
        set_state(XZ_ENCODING, "encoding");
        s_total = V.rec_ms / 60; s_done = 0;
    }
}

static void record_step(void)
{
    for (int i = 0; i < 4; i++) {
        const int n = nucleo_mic_read(s_pcm, FRAME, 0);
        if (n <= 0) break;
        fwrite(s_pcm, sizeof(int16_t), (size_t)n, s_rec);
        V.rec_ms += n * 1000 / RATE;
    }
    const int lv = nucleo_mic_level();
    if (abs(lv - V.level) >= 8) { V.level = lv; V.seq++; }   // a meter step, not every sample: direct drawing
    if (V.rec_ms >= MAX_REC_MS) xz_talk(false);
}

static void encode_step(void)
{
    if (!s_enc) {                                        // the encoder exists only during this phase
        heap_mark();
        s_enc = (OpusEncoder *)malloc((size_t)opus_encoder_get_size(1));
        s_pcm = (int16_t *)malloc(FRAME * sizeof(int16_t));
        if (!s_enc || !s_pcm || !pseudo_take(ENC_PSEUDO)) { codec_free(); fail("no memory to encode", (int)V.heap_block); return; }
        pseudo_point();
        if (opus_encoder_init(s_enc, RATE, 1, OPUS_APPLICATION_VOIP) != OPUS_OK) { codec_free(); fail("encoder", 0); return; }
        opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(0));
        opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(16000));
        char p[96]; path(p, sizeof p, "take.pcm"); s_rec = fopen(p, "rb");
        path(p, sizeof p, "take.opu"); s_pkts = fopen(p, "wb");
        if (!s_rec || !s_pkts) { files_close(); codec_free(); fail("SD card", 0); return; }
        s_rec_end = false;
    }
    uint8_t pkt[400];
    for (int i = 0; i < 3; i++) {                        // ~3 frames per poll: the UI keeps moving
        const size_t n = fread(s_pcm, sizeof(int16_t), FRAME, s_rec);
        if (n < FRAME) s_rec_end = true;      // the take ends here (no feof in the ABI)
        if (n == 0) break;
        if (n < FRAME) memset(s_pcm + n, 0, (FRAME - n) * sizeof(int16_t));
        pseudo_point();
        const int len = opus_encode(s_enc, s_pcm, FRAME, pkt, sizeof pkt);
        wdt();
        if (!pseudo_intact()) { files_close(); codec_free(); fail("codec scratch overrun", 0); return; }
        if (len > 0) { const uint8_t l2[2] = { (uint8_t)len, (uint8_t)(len >> 8) }; fwrite(l2, 1, 2, s_pkts); fwrite(pkt, 1, (size_t)len, s_pkts); }
        s_done++;
    }
    V.progress = s_total ? (int)(s_done * 100 / s_total) : 100;
    V.seq++;
    if (s_rec_end || s_done >= s_total + 1) {
        files_close();
        codec_free();
        set_state(XZ_EXCHANGE, "connecting");
    }
}

static void handle_json(const char *txt)
{
    cJSON *o = cJSON_Parse(txt);
    if (!o) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(o, "type"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(o, "text"));
    const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(o, "state"));
    if (!type) type = "";
    if (!strcmp(type, "stt") && text) snprintf(V.user, sizeof V.user, "%s", text);
    else if (!strcmp(type, "llm")) {
        const char *em = cJSON_GetStringValue(cJSON_GetObjectItem(o, "emotion"));
        if (em) snprintf(V.emotion, sizeof V.emotion, "%s", em);
    } else if (!strcmp(type, "tts") && state) {
        if (!strcmp(state, "sentence_start") && text) {  // keep the newest sentences that fit
            const size_t have = strlen(V.reply), add = strlen(text);
            if (have + add + 2 >= sizeof V.reply) V.reply[0] = 0;
            if (V.reply[0]) strncat(V.reply, " ", sizeof V.reply - strlen(V.reply) - 1);
            strncat(V.reply, text, sizeof V.reply - strlen(V.reply) - 1);
        } else if (!strcmp(state, "stop")) {
            s_answer_done = true;                        // the answer is complete
        }
    } else if (!strcmp(type, "alert") && text) snprintf(V.reply, sizeof V.reply, "%s", text);
    cJSON_Delete(o);
    V.seq++;
}

static cJSON *msg(const char *type)
{
    cJSON *o = cJSON_CreateObject();
    if (s_session[0]) cJSON_AddStringToObject(o, "session_id", s_session);
    cJSON_AddStringToObject(o, "type", type);
    return o;
}

static void exchange_step(void)
{
    if (!network_ready()) return;
    if (!ws_open()) return;
    set_state(XZ_EXCHANGE, "saying hello");
    cJSON *h = cJSON_CreateObject();
    cJSON_AddStringToObject(h, "type", "hello");
    cJSON_AddNumberToObject(h, "version", 1);
    cJSON_AddStringToObject(h, "transport", "websocket");
    cJSON *ap = cJSON_AddObjectToObject(h, "audio_params");
    cJSON_AddStringToObject(ap, "format", "opus");
    cJSON_AddNumberToObject(ap, "sample_rate", RATE);
    cJSON_AddNumberToObject(ap, "channels", 1);
    cJSON_AddNumberToObject(ap, "frame_duration", 60);
    if (!ws_text(h)) { ws_close(); fail("send failed", 0); return; }
    s_session[0] = 0;
    const int64_t until = now_ms() + 10000;              // the service answers hello within 10 s
    bool hello = false;
    while (!hello && now_ms() < until) {
        int len = 0;
        const int op = ws_recv(&len, (int)(until - now_ms()));
        if (op < 0 || op == 8) break;
        if (op != 1) continue;
        cJSON *o = cJSON_Parse((const char *)s_ws_buf);
        if (!o) continue;
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(o, "type"));
        if (t && !strcmp(t, "hello")) {
            hello = true;
            const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(o, "session_id"));
            snprintf(s_session, sizeof s_session, "%s", sid ? sid : "");
        }
        cJSON_Delete(o);
    }
    if (!hello) { ws_close(); fail("no hello from the server", 0); return; }
    cJSON *l = msg("listen");
    cJSON_AddStringToObject(l, "state", "start");
    cJSON_AddStringToObject(l, "mode", "manual");
    if (!ws_text(l)) { ws_close(); fail("send failed", 0); return; }
    set_state(XZ_EXCHANGE, "sending");
    char p[96]; path(p, sizeof p, "take.opu");
    FILE *f = fopen(p, "rb");
    uint8_t pkt[400];
    bool ok = f != NULL;
    while (ok) {
        uint8_t l2[2];
        if (fread(l2, 1, 2, f) != 2) break;
        const int len = l2[0] | l2[1] << 8;
        if (len <= 0 || len > (int)sizeof pkt || fread(pkt, 1, (size_t)len, f) != (size_t)len) break;
        ok = ws_send(2, pkt, len);
    }
    if (f) fclose(f);
    cJSON *st = msg("listen");
    cJSON_AddStringToObject(st, "state", "stop");
    if (!ok || !ws_text(st)) { ws_close(); fail("send failed", 0); return; }
    path(p, sizeof p, "reply.opu");
    s_pkts = fopen(p, "wb");
    if (!s_pkts) { ws_close(); fail("SD card", 0); return; }
    s_reply_audio = false; s_answer_done = false;
    s_next_poll = now_ms() + 30000;                      // silence limit while waiting (refreshed by traffic)
    set_state(XZ_WAITING, "thinking");
}

static void finish_exchange(void)
{
    files_close();
    ws_close();
    if (s_reply_audio) { set_state(XZ_PLAYING, "speaking"); s_done = 0; }
    else set_state(XZ_IDLE, V.user[0] ? "no spoken answer" : "nothing heard");
}

static void wait_step(void)
{
    for (int i = 0; i < 16; i++) {                       // drain what arrived, bounded
        if (!nucleo_tcp_readable(s_tcp, 0)) break;
        int len = 0;
        const int op = ws_recv(&len, 5000);
        if (op < 0 || op == 8) { finish_exchange(); return; }
        s_next_poll = now_ms() + 30000;
        if (op == 1) handle_json((const char *)s_ws_buf);
        else if (op == 2 && len > 0 && len <= 1275) {
            const uint8_t l2[2] = { (uint8_t)len, (uint8_t)(len >> 8) };
            fwrite(l2, 1, 2, s_pkts); fwrite(s_ws_buf, 1, (size_t)len, s_pkts);
            s_reply_audio = true;
        }
        if (s_answer_done) { finish_exchange(); return; }
    }
    if (now_ms() > s_next_poll) { finish_exchange(); if (V.state == XZ_IDLE) fail("no answer", 0); }
}

static void play_step(void)
{
    if (!s_dec) {
        s_dec = (OpusDecoder *)malloc((size_t)opus_decoder_get_size(1));
        s_pcm = (int16_t *)malloc(2 * FRAME * sizeof(int16_t));
        if (!s_dec || !s_pcm || !pseudo_take(DEC_PSEUDO)) { codec_free(); fail("no memory to play", (int)V.heap_block); return; }
        pseudo_point();
        opus_decoder_init(s_dec, RATE, 1);              // whatever rate the server encoded at, decoded to 16 kHz
        char p[96]; path(p, sizeof p, "reply.opu");
        s_pkts = fopen(p, "rb");
        struct stat sb; s_total = stat(p, &sb) == 0 ? sb.st_size : 1;
        if (!s_pkts) { codec_free(); fail("SD card", 0); return; }
        if (nucleo_audio_pcm_open_ex(RATE, 1, 2 * FRAME) != 0) { files_close(); codec_free(); fail("speaker busy", 0); return; }
        s_pcm_open = true;
        heap_mark();
    }
    uint8_t pkt[1276];
    for (int i = 0; i < 2; i++) {
        uint8_t l2[2];
        int len = 0;
        if (fread(l2, 1, 2, s_pkts) == 2) len = l2[0] | l2[1] << 8;
        if (len <= 0 || len > 1275 || fread(pkt, 1, (size_t)len, s_pkts) != (size_t)len) {
            files_close();
            nucleo_audio_pcm_close(); s_pcm_open = false;
            codec_free();
            V.progress = 100;
            set_state(XZ_IDLE, "ready");
            return;
        }
        s_done += len + 2;
        pseudo_point();
        const int n = opus_decode(s_dec, pkt, len, s_pcm, 2 * FRAME, 0);
        wdt();
        if (!pseudo_intact()) { files_close(); nucleo_audio_pcm_close(); s_pcm_open = false; codec_free(); fail("codec scratch overrun", 0); return; }
        if (n > 0) nucleo_audio_pcm_write(s_pcm, (size_t)n * sizeof(int16_t));
    }
    const int pr = (int)(s_done * 100 / (s_total ? s_total : 1));
    if (pr != V.progress) { V.progress = pr; V.seq++; }
}

bool xz_poll(void)
{
    const uint32_t seq = V.seq;
    switch (V.state) {
    case XZ_OTA:       ota_step(); break;
    case XZ_ACTIVATE:  activate_step(); break;
    case XZ_RECORDING: record_step(); break;
    case XZ_ENCODING:  encode_step(); break;
    case XZ_EXCHANGE:  exchange_step(); break;
    case XZ_WAITING:   wait_step(); break;
    case XZ_PLAYING:   play_step(); break;
    default: break;
    }
    return V.seq != seq;
}

const xz_view_t *xz_view(void) { return &V; }

static void start_from_config(void)
{
    s_ota_url[0] = 0;
    if (!V.server[0]) { set_state(XZ_SETUP, "choose a server"); return; }
    if (!strncmp(V.server, "ws://", 5) || !strncmp(V.server, "wss://", 6)) {
        snprintf(s_ws_url, sizeof s_ws_url, "%s", V.server);
        set_state(XZ_IDLE, "ready");
        return;
    }
    snprintf(s_ota_url, sizeof s_ota_url, "%s", V.server);
    set_state(XZ_OTA, "contacting the service");    // every start: the service may move the WebSocket
}

void xz_init(const char *dir, const char *lang)
{
    memset(&V, 0, sizeof V);
    snprintf(s_dir, sizeof s_dir, "%s", dir);
    snprintf(s_lang, sizeof s_lang, "%s", lang && lang[0] ? lang : "en-US");
    mkdir(s_dir, 0775);
    identity();
    config_load();
    start_from_config();
}

bool xz_set_server(const char *url)
{
    url_t u;
    if (!url || !url_parse(url, &u)) return false;
    snprintf(V.server, sizeof V.server, "%s", url);
    s_ws_url[0] = s_ws_token[0] = 0;
    config_save();
    start_from_config();
    return true;
}

void xz_forget(void)
{
    nucleo_secret_set("cid", NULL);
    nucleo_secret_set("mac", NULL);
    nucleo_secret_set("token", NULL);
    s_ws_url[0] = s_ws_token[0] = 0;
    config_save();
    identity();
    start_from_config();
}

void xz_shutdown(void)
{
    if (V.state == XZ_RECORDING) nucleo_mic_close();
    if (s_pcm_open) { nucleo_audio_pcm_close(); s_pcm_open = false; }
    files_close();
    if (s_ws_buf) ws_close(); else net_close();
    codec_free();
}
#endif  // NAP_MODULE || !ESP_PLATFORM

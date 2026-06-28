#include "nucleo_ws.h"
#include "nucleo_eventbus.h"
#include "nucleo_auth.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"           // low-rate timer that keeps the persistent /ws off lru_purge's radar
#include "freertos/FreeRTOS.h"   // portMUX critical section guarding the message pool

static const char *TAG = "ws";
#define MAX_CLIENTS 1   // HARD single-client: NucleoOS web serves exactly ONE browser at a time, EVER.
                        // A new connection EVICTS the previous one (last-wins — see add_client), which both
                        // enforces the 1-client cap AND lets the legit client reload/reconnect without being
                        // locked out by its own lingering socket (a hard reject would block it for the ~20s
                        // keep-alive timeout). Two DISTINCT browsers can never be active simultaneously.

static httpd_handle_t s_server;
static int  s_fds[MAX_CLIENTS];     // 0 = empty slot
static bool s_shell[MAX_CLIENTS];   // parallel to s_fds: this client connected to /ws?shell=1 (the OS shell)
static esp_timer_handle_t s_lru_pin_timer;   // keeps the idle /ws socket off lru_purge while a client is connected

static int count_clients(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i]) n++;
    return n;
}
int nucleo_ws_client_count(void) { return count_clients(); }

// Clients that identified as the OS shell — the launcher gates its remote handoff on this, NOT the raw
// count, so a standalone app page or a bare /ws probe can't blank the screen / reclaim the device's RAM.
int nucleo_ws_shell_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i] && s_shell[i]) n++;
    return n;
}

// Announce the live client count so the web shell + Log Viewer can show sessions, and
// (indirectly) so anyone watching can see when the device is handed to a remote browser.
static void publish_count(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "{\"clients\":%d}", count_clients());
    nucleo_event_publish("system.clients", buf);
}

// Keep the PERSISTENT /ws socket off lru_purge's eviction radar. The WS is mostly idle, so under a burst
// (cold-load assets + crawl + status) it would be the oldest-idle socket the httpd recycles → the handoff
// flaps (the 4-socket cap only mitigated this). Periodically — and on every outbound frame — bump its LRU
// counter so a TRANSIENT socket is purged instead. httpd_sess_update_lru_counter is NOT thread-safe, so it
// must run ON the httpd task via httpd_queue_work (the publisher/timer task only queues the work).
static void ws_touch_lru(void *arg) { int fd = (int)(intptr_t)arg; if (s_server && fd > 0) httpd_sess_update_lru_counter(s_server, fd); }
static void ws_pin(void) { if (!s_server) return; int fd = s_fds[0]; if (fd > 0) httpd_queue_work(s_server, ws_touch_lru, (void *)(intptr_t)fd); }
static void ws_pin_cb(void *arg) { (void)arg; ws_pin(); }
static void ws_pin_arm(void)   // armed when a client attaches; a clientless device pays nothing
{
    if (!s_lru_pin_timer) {
        esp_timer_create_args_t a = { .callback = ws_pin_cb, .name = "ws_lru" };
        if (esp_timer_create(&a, &s_lru_pin_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_lru_pin_timer);                       // idempotent (INVALID_STATE if already stopped: ignore)
    esp_timer_start_periodic(s_lru_pin_timer, 5000000);    // 5 s: below the burst window, far above any real cost
}
static void ws_pin_stop(void) { if (s_lru_pin_timer) esp_timer_stop(s_lru_pin_timer); }

static void add_client(int fd, bool is_shell)
{
    if (fd < 0) return;   // httpd_req_to_sockfd() failed; never store -1 (empty slot sentinel is 0, so -1 would read as a live client)
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i] == fd) { if (is_shell) s_shell[i] = true; return; }   // already tracked (upgrade shell bit)
    // Single-client policy (last-wins): evict any existing DIFFERENT client so this new one becomes the
    // sole active session. Closing the old socket frees the slot immediately, so a reload/reconnect of the
    // same user always succeeds; two distinct browsers can never coexist. The old socket's close hook
    // (nucleo_ws_notify_close) is a harmless no-op since we already cleared its slot here.
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_fds[i] != 0 && s_fds[i] != fd) {
            // Don't let a bare /ws (a standalone app presence page, no shell=1) evict the live OS shell:
            // that would zero nucleo_ws_shell_count() and force exit_remote after the grace window even
            // though the web OS is still in use. The shell keeps the slot; the newcomer isn't tracked
            // (its socket stays open + handshake succeeds, it just gets no deltas). A reconnecting SHELL
            // (is_shell) still evicts the old one, so the legit reload path is unaffected.
            if (!is_shell && s_shell[i]) return;
            int old = s_fds[i]; s_fds[i] = 0; s_shell[i] = false;
            if (s_server) httpd_sess_trigger_close(s_server, old);   // close the previous client's socket
            ESP_LOGI(TAG, "single-client: evicted previous fd=%d for new fd=%d", old, fd);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i] == 0) { s_fds[i] = fd; s_shell[i] = is_shell; ws_pin_arm(); publish_count(); return; }
}
static void drop_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i] == fd) { s_fds[i] = 0; s_shell[i] = false; if (count_clients() == 0) ws_pin_stop(); publish_count(); return; }
}

// HTTP server close hook: fires when any socket closes. We only track WS fds, so dropping
// a non-WS fd is a harmless no-op. This is what makes a closed browser tab detected at once.
void nucleo_ws_notify_close(int fd) { drop_client(fd); }

// A WS send must run on the httpd task (httpd_ws_send_frame_async), but events arrive on the
// publisher's task — so each frame is handed across via httpd_queue_work. The payload used to be
// a fresh malloc+strdup per client per event; on this no-PSRAM board that constant churn was the
// main heap-fragmentation source. Instead we carry the frames in a small fixed pool of static
// slots: zero heap allocation on the hot path, bounded RAM, and if the pool is momentarily full
// the delta is simply dropped — the client notices the seq gap and resyncs via {"op":"subscribe"}.
#define WS_MSG_MAX  320                 // matches the on_event() framing buffer
#define WS_POOL     3                   // in-flight frames for the single client: 1 sending + 2
                                        // queued for an event burst. Pool-full just drops the delta
                                        // and the client resyncs via {"op":"subscribe"}. 6->3 frees
                                        // ~1 KB .bss (ws_msg_t = 328 B) for the PSRAM-less heap.
typedef struct { volatile bool busy; int fd; char text[WS_MSG_MAX]; } ws_msg_t;
static ws_msg_t s_pool[WS_POOL];
static portMUX_TYPE s_pool_mux = portMUX_INITIALIZER_UNLOCKED;   // pool touched from two tasks

static ws_msg_t *pool_take(void)        // claim a free slot (publisher task)
{
    ws_msg_t *m = NULL;
    portENTER_CRITICAL(&s_pool_mux);
    for (int i = 0; i < WS_POOL; i++) if (!s_pool[i].busy) { s_pool[i].busy = true; m = &s_pool[i]; break; }
    portEXIT_CRITICAL(&s_pool_mux);
    return m;
}
static void pool_release(ws_msg_t *m)   // return a slot (httpd task, after send)
{
    portENTER_CRITICAL(&s_pool_mux);
    m->busy = false;
    portEXIT_CRITICAL(&s_pool_mux);
}

static void send_async(void *arg)
{
    ws_msg_t *m = arg;
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)m->text, .len = strlen(m->text) };
    if (httpd_ws_send_frame_async(s_server, m->fd, &f) != ESP_OK) drop_client(m->fd);
    pool_release(m);
}

static void broadcast(const char *text)
{
    ws_pin();   // events ARE flowing -> the WS is genuinely "in use"; refresh its LRU rank for free between timer ticks
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!s_fds[i]) continue;
        ws_msg_t *m = pool_take();
        if (!m) return;                 // pool exhausted: drop the delta, client will resync
        m->fd = s_fds[i];
        snprintf(m->text, sizeof(m->text), "%s", text);
        if (httpd_queue_work(s_server, send_async, m) != ESP_OK) pool_release(m);
    }
}

// Live sink from the event bus: wrap one event and broadcast it. `src` (origin device for
// mesh-injected events) is not surfaced over the wire here — the browser sees a uniform stream.
static void on_event(uint32_t seq, const char *topic, const char *payload, const char *src)
{
    (void)src;
    char buf[320];
    snprintf(buf, sizeof(buf), "{\"t\":\"%s\",\"seq\":%u,\"d\":%s}", topic, (unsigned)seq, payload);
    broadcast(buf);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {            // handshake: gate before accepting the socket
        NUCLEO_AUTH_GUARD(req);
        // The OS web shell connects to /ws?shell=1; only such a client drives the remote handoff (see
        // nucleo_ws_shell_count). Every /ws client is still tracked + still receives the event stream.
        bool is_shell = false;
        char q[40];
        if (httpd_req_get_url_query_len(req) > 0 &&
            httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
            strstr(q, "shell=1")) is_shell = true;
        add_client(httpd_req_to_sockfd(req), is_shell);
        ESP_LOGI(TAG, "client connected%s", is_shell ? " (shell)" : "");
        return ESP_OK;
    }

    // Incoming frame: read length, then payload.
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) return ESP_FAIL;
    if (frame.len == 0 || frame.len > 256) return ESP_OK;
    uint8_t buf[257] = {0};
    frame.payload = buf;
    if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) return ESP_FAIL;

    // Minimal protocol: {"op":"subscribe","since":N} -> reply with buffered delta.
    int fd = httpd_req_to_sockfd(req);
    add_client(fd, false);   // re-track defensively; preserves the shell bit set at the GET handshake
    if (strstr((char *)buf, "\"subscribe\"")) {
        const char *p = strstr((char *)buf, "\"since\"");
        uint32_t since = p ? (uint32_t)strtoul(strchr(p, ':') + 1, NULL, 10) : 0;
        // static (not stack): these ~2 KB buffers on the tight httpd task stack risked an
        // overflow -> reboot when the shell sent "subscribe". Safe: httpd serves on one task.
        static char out[1024];
        int n = nucleo_event_copy_since(since, out, sizeof(out));
        static char msg[1100];
        snprintf(msg, sizeof(msg), "{\"op\":\"sync\",\"resync\":%s,\"events\":%s}",
                 n < 0 ? "true" : "false", n < 0 ? "[]" : out);
        httpd_ws_frame_t r = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg, .len = strlen(msg) };
        httpd_ws_send_frame(req, &r);
    }
    return ESP_OK;
}

esp_err_t nucleo_ws_register(httpd_handle_t server)
{
    s_server = server;
    httpd_uri_t ws = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };
    esp_err_t err = httpd_register_uri_handler(server, &ws);
    if (err == ESP_OK) nucleo_event_set_sink(on_event);
    return err;
}

#include "nucleo_ws.h"
#include "nucleo_eventbus.h"
#include "nucleo_auth.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"   // portMUX critical section guarding the message pool

static const char *TAG = "ws";
#define MAX_CLIENTS 1   // HARD single-client: NucleoOS web serves exactly ONE browser at a time, EVER.
                        // A new connection EVICTS the previous one (last-wins — see add_client), which both
                        // enforces the 1-client cap AND lets the legit client reload/reconnect without being
                        // locked out by its own lingering socket (a hard reject would block it for the ~20s
                        // keep-alive timeout). Two DISTINCT browsers can never be active simultaneously.

static httpd_handle_t s_server;
static int s_fds[MAX_CLIENTS];   // 0 = empty slot

static int count_clients(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i]) n++;
    return n;
}
int nucleo_ws_client_count(void) { return count_clients(); }

// Announce the live client count so the web shell + Log Viewer can show sessions, and
// (indirectly) so anyone watching can see when the device is handed to a remote browser.
static void publish_count(void)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "{\"clients\":%d}", count_clients());
    nucleo_event_publish("system.clients", buf);
}

static void add_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i] == fd) return;   // already tracked
    // Single-client policy (last-wins): evict any existing DIFFERENT client so this new one becomes the
    // sole active session. Closing the old socket frees the slot immediately, so a reload/reconnect of the
    // same user always succeeds; two distinct browsers can never coexist. The old socket's close hook
    // (nucleo_ws_notify_close) is a harmless no-op since we already cleared its slot here.
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_fds[i] != 0 && s_fds[i] != fd) {
            int old = s_fds[i]; s_fds[i] = 0;
            if (s_server) httpd_sess_trigger_close(s_server, old);   // close the previous client's socket
            ESP_LOGI(TAG, "single-client: evicted previous fd=%d for new fd=%d", old, fd);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i] == 0) { s_fds[i] = fd; publish_count(); return; }
}
static void drop_client(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) if (s_fds[i] == fd) { s_fds[i] = 0; publish_count(); return; }
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
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!s_fds[i]) continue;
        ws_msg_t *m = pool_take();
        if (!m) return;                 // pool exhausted: drop the delta, client will resync
        m->fd = s_fds[i];
        snprintf(m->text, sizeof(m->text), "%s", text);
        if (httpd_queue_work(s_server, send_async, m) != ESP_OK) pool_release(m);
    }
}

// Live sink from the event bus: wrap one event and broadcast it.
static void on_event(uint32_t seq, const char *topic, const char *payload)
{
    char buf[320];
    snprintf(buf, sizeof(buf), "{\"t\":\"%s\",\"seq\":%u,\"d\":%s}", topic, (unsigned)seq, payload);
    broadcast(buf);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {            // handshake: gate before accepting the socket
        NUCLEO_AUTH_GUARD(req);
        add_client(httpd_req_to_sockfd(req));
        ESP_LOGI(TAG, "client connected");
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
    add_client(fd);
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

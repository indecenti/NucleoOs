#include "nucleo_webfs.h"
#include "nucleo_board.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"   // largest-free-block check for the large-file circuit breaker
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // vTaskDelay/pdMS_TO_TICKS — the bounded "wait for heap" before serving

static const char *TAG = "webfs";

// Circuit breaker for the static catch-all: refuse to START a large full-file stream when contiguous
// SRAM is already low. Streaming a multi-MB asset (e.g. an offline voice-model part) through the
// single-task web server while it's heap-starved is what tips it over — every other request
// (/api/status) then times out. Below the floor we answer 503 (the client backs off and retries),
// keeping the server alive to recover. Healthy heap + normal-size assets are never affected.
#define NUCLEO_WEBFS_LARGE_FILE     (512 * 1024)   // "large" = bigger than any normal UI asset
#define NUCLEO_WEBFS_LARGE_MIN_HEAP (32 * 1024)    // largest_free_block floor to allow a large stream

// When a browser pulls a UI asset (it is LOADING the web OS), free RAM for the server BEFORE serving if
// the largest contiguous block has dropped under this floor — so a connecting client ALWAYS has the heap
// the single-task server needs to push the shell + its assets on this PSRAM-less chip. Reactive + measured:
// fires only under the floor, frees only an IDLE offline index (it reloads from SD on the next ANIMA
// query), and is ungated by any key. Wired at boot to nucleo_anima_l1_unload_if_idle (see nucleo_httpd).
#define NUCLEO_WEBFS_SERVE_MIN_HEAP  (40 * 1024)   // largest_free_block we want before serving a UI asset
#define NUCLEO_WEBFS_HEAP_WAIT_STEPS 12            // ...and how patient to be getting there (up to ~1.2 s):
#define NUCLEO_WEBFS_HEAP_WAIT_MS    100           // we'd rather the FIRST load be slow than truncate an asset
static void (*s_reclaim_cb)(void);
void nucleo_webfs_set_reclaim_cb(void (*cb)(void)) { s_reclaim_cb = cb; }

// Richiesta one-shot: un asset GRANDE (lib/modello Vosk, shard WebLLM) va servito ma il blocco
// contiguo e' sotto la soglia. L'unico blocco abbastanza grande da liberare e' il framebuffer da
// 32 KB del launcher, ma la sprite si puo' cancellare SOLO dal task app (un deleteSprite dal task
// httpd sarebbe una race col suo render) -> alziamo un flag che il loop app consuma per lanciare
// display_sleep() (libera la canvas, stesso percorso del blank idle a 10s). "RAM just-in-time":
// scatta SOLO mentre un asset pesante e' davvero richiesto (es. il vosk OFFLINE dalla SD).
static volatile bool s_heap_request;
bool nucleo_webfs_take_heap_request(void) { bool r = s_heap_request; s_heap_request = false; return r; }

// Public: ask the app task to free the 32 KB launcher canvas and wait briefly for it — the same path a
// heavy webfs transfer uses. Called by /api/transcribe so the cloud Whisper/teacher TLS clears the heap
// gate on a tight unit (its httpd-task context can't free httpd, and the held canvas alone starves it to
// ~20 KB free). Returns the largest contiguous block after the wait. No-op cost if already free.
size_t nucleo_webfs_reclaim_canvas(void)
{
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < NUCLEO_WEBFS_LARGE_MIN_HEAP) {
        s_heap_request = true;
        for (int i = 0; i < NUCLEO_WEBFS_HEAP_WAIT_STEPS &&
             heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < NUCLEO_WEBFS_LARGE_MIN_HEAP; i++)
            vTaskDelay(pdMS_TO_TICKS(NUCLEO_WEBFS_HEAP_WAIT_MS));
    }
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

// Text formats carry an explicit charset=utf-8: the OS UI is Italian (accented chars) and
// .js/.json have no in-band charset declaration, so without this the browser falls back to
// locale-dependent sniffing and can mangle "è/à/ù". Binary types need no charset.
static const char *content_type(const char *path)
{
    const char *e = strrchr(path, '.');
    if (!e) return "application/octet-stream";
    if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(e, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(e, ".js") || !strcmp(e, ".mjs")) return "text/javascript; charset=utf-8";
    if (!strcmp(e, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(e, ".webmanifest")) return "application/manifest+json; charset=utf-8";
    if (!strcmp(e, ".svg")) return "image/svg+xml; charset=utf-8";
    if (!strcmp(e, ".png")) return "image/png";
    if (!strcmp(e, ".jpg") || !strcmp(e, ".jpeg")) return "image/jpeg";
    if (!strcmp(e, ".wav")) return "audio/wav";
    if (!strcmp(e, ".wasm")) return "application/wasm";   // WebAssembly streaming compile needs this MIME (wllama/WebLLM libs)
    return "application/octet-stream";                     // .bin / .gguf model weights, etc.
}

// Long-cache the immutable visual assets (wallpaper, icons) so the shell doesn't re-fetch them
// on every load; everything else (HTML/JS/CSS — the parts that change on a release) stays
// revalidated so an OTA/SD update is picked up without a hard refresh. The service worker still
// owns app-level versioning; this is only the HTTP layer being honest about what is static.
static bool is_immutable_asset(const char *type)
{
    return strncmp(type, "image/", 6) == 0 || strncmp(type, "audio/", 6) == 0;
}

// Translate a request URI to an absolute SD path (no path traversal allowed).
static void map_uri(const char *uri, char *out, size_t n)
{
    char clean[200];
    size_t i = 0;
    for (; uri[i] && uri[i] != '?' && i < sizeof(clean) - 1; i++) clean[i] = uri[i];
    clean[i] = '\0';
    if (strstr(clean, "..")) { out[0] = '\0'; return; }

    if (strncmp(clean, "/apps/", 6) == 0) {
        const char *p = clean + 6;
        const char *slash = strchr(p, '/');
        char id[24];
        const char *rest;
        if (slash) {
            size_t l = (size_t)(slash - p);
            if (l >= sizeof(id)) l = sizeof(id) - 1;
            memcpy(id, p, l); id[l] = '\0';
            rest = (slash[1] == '\0') ? "index.html" : slash + 1;
        } else {
            strncpy(id, p, sizeof(id) - 1); id[sizeof(id) - 1] = '\0';
            rest = "index.html";
        }
        snprintf(out, n, NUCLEO_SD_MOUNT "/apps/%s/www/%s", id, rest);
    } else {
        const char *rest = (strcmp(clean, "/") == 0) ? "/index.html" : clean;
        snprintf(out, n, NUCLEO_SD_MOUNT "/www/shell%s", rest);
    }
    size_t L = strlen(out);
    if (L && out[L - 1] == '/') strncat(out, "index.html", n - L - 1);
}

static esp_err_t static_get(httpd_req_t *req)
{
    char path[256];
    map_uri(req->uri, path, sizeof(path));
    if (!path[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path"); return ESP_FAIL; }

    // A client is loading the web OS: GUARANTEE the server has the RAM to serve it before we start. If the
    // largest contiguous block is under the floor, reclaim the idle ANIMA index (~31 KB) and, if still tight,
    // WAIT in short steps for the heap pool to coalesce — we'd rather take a little longer than hand back a
    // truncated asset, so the first load always completes. Bounded (a genuinely stuck heap still returns
    // control and the serve proceeds best-effort), a no-op when heap is healthy, ungated by any key.
    for (int i = 0; s_reclaim_cb && i < NUCLEO_WEBFS_HEAP_WAIT_STEPS &&
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < NUCLEO_WEBFS_SERVE_MIN_HEAP; i++) {
        s_reclaim_cb();                                              // drop the idle index (idempotent once freed)
        if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= NUCLEO_WEBFS_SERVE_MIN_HEAP) break;
        vTaskDelay(pdMS_TO_TICKS(NUCLEO_WEBFS_HEAP_WAIT_MS));        // let the idle task coalesce the freed blocks
    }

    // A Range request (a media seek, or voice.js's 1-byte probe of vosk.js) is served RAW with 206 —
    // never the .gz sibling, since a byte range over gzip-encoded bytes is meaningless to the client.
    // Without this the webfs ignored Range and streamed the WHOLE file as 200; on a large asset
    // (vosk.js) that resets mid-transfer under low heap (the ERR_CONNECTION_RESET in the console).
    // Honoring Range keeps the read bounded (1 byte for the probe), so it stops competing for the
    // scarce RAM/socket time of the single-task web server.
    char range_hdr[64];
    bool want_range = httpd_req_get_hdr_value_str(req, "Range", range_hdr, sizeof(range_hdr)) == ESP_OK;

    // Prefer a pre-compressed "<path>.gz" sibling when the client accepts gzip (and isn't ranging).
    // Essential on this low-heap device: large uncompressed assets (e.g. 83KB shell.js) came back
    // empty under memory pressure because the WiFi/LWIP stack couldn't push the whole stream; gzip
    // shrinks shell.js to ~26KB and the rest well under, so they transfer reliably. Falls back to raw.
    bool gz = false;
    FILE *f = NULL;
    if (!want_range) {
        char ae[80];
        if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", ae, sizeof(ae)) == ESP_OK && strstr(ae, "gzip")) {
            char gzp[260];
            int gl = snprintf(gzp, sizeof(gzp), "%s.gz", path);
            if (gl > 0 && gl < (int)sizeof(gzp)) { f = fopen(gzp, "rb"); if (f) gz = true; }
        }
    }
    if (!f) f = fopen(path, "rb");
    if (!f) {
        ESP_LOGD(TAG, "404 %s", path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;   // the client got a complete 404 (benign probe: /favicon.ico, missing wllama variants).
                         // ESP_FAIL would make IDF log "uri handler execution failed" AND tear down the socket
                         // (bad on a 4-socket lru server) — ESP_OK keeps keep-alive and silences the noise.
    }

    // Large-file circuit breaker: a .gz sibling is always a small UI asset, but a RAW read of a large
    // file can collapse the single-task server under low contiguous heap. This now covers RANGED reads
    // too (ANIMA Forge must-fix #3): the WebLLM SD model pull streams a large shard via a Range-window
    // fetcher, and a ranged read-storm would otherwise BYPASS this breaker (the old `!want_range` gate).
    // Under the heap floor we 503+Retry-After so the client backs off — download.js reduces the window
    // and the scheduler pauses an SD pull — keeping the verifier/status endpoints alive. (ESP-IDF-only
    // path; the JS coexistence logic is host-gated, confirm this firmware half on the next flash.)
    if (!gz) {
        fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
        // Byte che trasferiremo DAVVERO: un ranged-read minuscolo (voice.js sonda vosk.js con un Range
        // di 1 byte per testarne la presenza) e' economico a prescindere dalla dimensione del file —
        // non va mai gated ne' fa reclaim. Solo un trasferimento bulk vero puo' ingolfare il server.
        long want_bytes = fsz;
        if (want_range) { long rs = 0, re = -1; int n = sscanf(range_hdr, "bytes=%ld-%ld", &rs, &re);
            if (n == 2 && re >= rs) want_bytes = re - rs + 1; else if (n >= 1 && rs <= fsz) want_bytes = fsz - rs; }
        if (fsz > NUCLEO_WEBFS_LARGE_FILE && want_bytes > 4096) {
            // Asset pesante. Se il blocco contiguo e' sotto la soglia, chiedi al task app di restituire
            // i 32 KB del framebuffer e aspetta brevemente che arrivino — RAM liberata SOLO quando un
            // asset pesante viene davvero richiesto. Ancora corto? 503 e il client fa backoff.
            if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < NUCLEO_WEBFS_LARGE_MIN_HEAP) {
                s_heap_request = true;
                for (int i = 0; i < NUCLEO_WEBFS_HEAP_WAIT_STEPS &&
                     heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < NUCLEO_WEBFS_LARGE_MIN_HEAP; i++)
                    vTaskDelay(pdMS_TO_TICKS(NUCLEO_WEBFS_HEAP_WAIT_MS));
            }
            if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < NUCLEO_WEBFS_LARGE_MIN_HEAP) {
                ESP_LOGW(TAG, "503 low-heap, defer large file (%ld B%s): %s", fsz, want_range ? ", ranged" : "", path);
                fclose(f);
                httpd_resp_set_status(req, "503 Service Unavailable");
                httpd_resp_set_hdr(req, "Retry-After", "3");
                httpd_resp_set_type(req, "text/plain");
                httpd_resp_sendstr(req, "low memory, retry");
                return ESP_OK;
            }
        }
    }

    const char *type = content_type(path);   // type from the ORIGINAL extension, even when serving .gz
    httpd_resp_set_type(req, type);
    if (gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    } else {
        httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");   // raw file: byte ranges are meaningful
    }
    // We always send the exact Content-Type for a known extension, so forbid MIME sniffing —
    // a .json or user asset can never be reinterpreted as HTML/script by the browser.
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Cache-Control",
                       is_immutable_asset(type) ? "public, max-age=604800" : "no-cache");

    long start = 0, end = 0;
    bool ranged = false;
    // FUNCTION scope: httpd_resp_set_hdr() keeps the value by pointer and the headers are only
    // serialised on the first send_chunk below, after buf[] is filled — a block-scoped buffer
    // would get its stack slot reused by buf[] and corrupt Content-Range (the 206 path). Same
    // footgun as nucleo_fsapi's read_get.
    char cr[64];
    if (want_range && !gz) {                                  // resolve the range against the raw file
        fseek(f, 0, SEEK_END); long total = ftell(f); fseek(f, 0, SEEK_SET);
        long rs = 0, re = -1;
        int parsed = sscanf(range_hdr, "bytes=%ld-%ld", &rs, &re);
        if (parsed >= 1 && rs >= 0 && rs < total) {
            start = rs;
            end = (parsed == 2 && re >= 0 && re < total) ? re : total - 1;
            if (end < start) end = total - 1;
            ranged = true;
            snprintf(cr, sizeof(cr), "bytes %ld-%ld/%ld", start, end, total);
            httpd_resp_set_hdr(req, "Content-Range", cr);
            httpd_resp_set_status(req, "206 Partial Content");
            fseek(f, start, SEEK_SET);
        } else if (parsed >= 1) {                             // start past EOF -> 416
            snprintf(cr, sizeof(cr), "bytes */%ld", total);
            httpd_resp_set_hdr(req, "Content-Range", cr);
            httpd_resp_set_status(req, "416 Range Not Satisfiable");
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_OK;
        }
    }

    long remaining = ranged ? (end - start + 1) : -1;        // -1 = stream to EOF
    char buf[1024];   // small fixed buffer: stream, never load whole file into RAM
    size_t r;
    while ((r = fread(buf, 1, (remaining >= 0 && remaining < (long)sizeof(buf)) ? (size_t)remaining : sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { fclose(f); return ESP_FAIL; }
        if (remaining >= 0) { remaining -= (long)r; if (remaining <= 0) break; }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t nucleo_webfs_register(httpd_handle_t server)
{
    httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = static_get };
    return httpd_register_uri_handler(server, &any);
}

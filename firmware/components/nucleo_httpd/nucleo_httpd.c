#include "nucleo_httpd.h"
#include "nucleo_storage.h"
#include "nucleo_log.h"
#include "nucleo_registry.h"
#include "nucleo_webfs.h"
#include "nucleo_ws.h"
#include "nucleo_fsapi.h"
#include "nucleo_recorder.h"
#include "nucleo_ir.h"
#include "nucleo_gpio.h"
#include "nucleo_setup.h"
#include "nucleo_auth.h"
#include "nucleo_voice.h"
#include "nucleo_board.h"
#include "nucleo_anima.h"
#include "nucleo_tts.h"
#include "nucleo_eventbus.h"
#include "nucleo_arb.h"     // heavy-work arbiter: serialize outbound TLS so two fetches can't both OOM
#include "nucleo_power.h"   // real battery level for /api/status
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp/strncasecmp (SSRF host classification)
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"     // close() — we own socket teardown once we set config.close_fn
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"      // esp_app_get_description(): real firmware version/build for /api/diag
#include "esp_system.h"
#include "esp_http_client.h"   // /api/proxy: server-side page fetch (the browser app is same-origin -> no CORS)
#include "esp_crt_bundle.h"    // built-in TLS trust store, for proxying HTTPS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"   // per-core idle hook: the CPU-load probe rides the idle loop (free cycles)
#include "cJSON.h"

static const char *TAG = "httpd";

// ---------------------------------------------------------------------------
// CPU-load probe (per core). Mechanism: register an idle hook on each core that
// increments a counter and returns false ("call me as fast as possible"). The
// hook only runs in cycles the core would otherwise spin away in its idle loop,
// so it steals nothing from real work — it just turns idle slack into a count.
// A 500 ms timer converts the count delta into counts/ms and tracks the highest
// rate ever seen per core as that core's "fully idle" baseline; load% is then
// 100*(1 - rate/peak), lightly smoothed. Self-calibrating, no Kconfig, ~nothing.
// ---------------------------------------------------------------------------
#ifdef CONFIG_FREERTOS_NUMBER_OF_CORES
#define NUCLEO_CPU_CORES CONFIG_FREERTOS_NUMBER_OF_CORES
#else
#define NUCLEO_CPU_CORES portNUM_PROCESSORS
#endif

static volatile uint32_t s_idle_cnt[NUCLEO_CPU_CORES];   // bumped from each core's idle hook
static uint32_t          s_idle_prev[NUCLEO_CPU_CORES];  // last sample, timer-owned
static float             s_idle_peak[NUCLEO_CPU_CORES];  // max counts/ms = fully-idle baseline
static float             s_cpu_load[NUCLEO_CPU_CORES];   // 0..100, smoothed
static int64_t           s_cpu_prev_us;
static esp_timer_handle_t s_cpu_timer;

static bool idle_hook_cpu0(void) { s_idle_cnt[0]++; return false; }
#if NUCLEO_CPU_CORES > 1
static bool idle_hook_cpu1(void) { s_idle_cnt[1]++; return false; }
#endif

static void cpu_sample_cb(void *arg)
{
    int64_t now = esp_timer_get_time();
    float dt_ms = (now - s_cpu_prev_us) / 1000.0f;
    if (dt_ms <= 0.0f) return;
    s_cpu_prev_us = now;
    for (int c = 0; c < NUCLEO_CPU_CORES; c++) {
        uint32_t cur = s_idle_cnt[c];
        uint32_t d   = cur - s_idle_prev[c];   // wraps cleanly (unsigned)
        s_idle_prev[c] = cur;
        float rate = d / dt_ms;
        if (rate > s_idle_peak[c]) s_idle_peak[c] = rate;   // ratchet up to the true idle ceiling
        float load = s_idle_peak[c] > 0.0f ? 100.0f * (1.0f - rate / s_idle_peak[c]) : 0.0f;
        if (load < 0.0f) load = 0.0f; else if (load > 100.0f) load = 100.0f;
        s_cpu_load[c] = s_cpu_load[c] * 0.5f + load * 0.5f;   // one-pole smoothing
    }
}

static void cpu_probe_init(void)
{
    if (s_cpu_timer) return;   // once per boot, survives httpd stop/restart
    s_cpu_prev_us = esp_timer_get_time();
    esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu0, 0);
#if NUCLEO_CPU_CORES > 1
    esp_register_freertos_idle_hook_for_cpu(idle_hook_cpu1, 1);
#endif
    const esp_timer_create_args_t a = { .callback = cpu_sample_cb, .name = "cpuload" };
    if (esp_timer_create(&a, &s_cpu_timer) == ESP_OK)
        esp_timer_start_periodic(s_cpu_timer, 500000);   // 500 ms cadence
}

// Public accessors so native on-device UI (the remote-session smartwatch monitor in
// nucleo_app) can show the same per-core load without duplicating the probe. Symbols
// resolve at final link; the native side declares them extern "C".
int nucleo_cpu_core_count(void) { return NUCLEO_CPU_CORES; }
int nucleo_cpu_load_pct(int core)
{
    if (core < 0 || core >= NUCLEO_CPU_CORES) return -1;
    int v = (int)(s_cpu_load[core] + 0.5f);
    return v < 0 ? 0 : (v > 100 ? 100 : v);
}

// GET /api/cpu -> per-core load snapshot for the System Monitor's CPU tab. Tiny + public,
// like /api/status: just the smoothed loads, an average, task count and clock.
static esp_err_t cpu_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(root, "cores", NUCLEO_CPU_CORES);
    cJSON_AddNumberToObject(root, "freq_mhz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(root, "tasks", uxTaskGetNumberOfTasks());
    cJSON *arr = cJSON_AddArrayToObject(root, "load");
    float sum = 0.0f;
    for (int c = 0; c < NUCLEO_CPU_CORES; c++) {
        float v = s_cpu_load[c];
        sum += v;
        cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)((int)(v * 10 + 0.5f)) / 10.0));   // 1 decimal
    }
    cJSON_AddNumberToObject(root, "load_avg", (double)((int)(sum / NUCLEO_CPU_CORES * 10 + 0.5f)) / 10.0);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return ESP_OK;
}

// The single OS web server handle. Kept so nucleo_httpd_stop() can tear it down and free
// port 80 — the Evil Portal app (authorized captive-portal testing) needs to bind its own
// server there while running, then calls nucleo_httpd_start() again on exit to bring the OS
// web UI back. NULL while stopped.
static httpd_handle_t s_server = NULL;

// GET /api/status -> live OS snapshot (device, storage capacity, app count).
static esp_err_t status_get(httpd_req_t *req)
{
    nucleo_storage_refresh();
    const nucleo_storage_info_t *st = nucleo_storage_info();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "os", "NucleoOS");
    cJSON_AddStringToObject(root, "version", "0.1.0");
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    // Heap diagnostics. free_heap is "free right now"; the number that actually tells you
    // whether SRAM is tight is min_free_heap — the lowest the free pool has EVER dropped to
    // since boot (the true headroom watermark). largest_free_block vs free_heap reveals
    // fragmentation (a big gap = fragmented). MALLOC_CAP_INTERNAL is the scarce on-chip SRAM
    // (no PSRAM on this module), so it is the pool worth watching.
    cJSON_AddNumberToObject(root, "free_heap", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddNumberToObject(root, "min_free_heap", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "largest_free_block", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    cJSON *storage = cJSON_AddObjectToObject(root, "storage");
    cJSON_AddBoolToObject(storage, "mounted", st->mounted);
    cJSON_AddStringToObject(storage, "fs", st->mounted ? st->fs_type : "?");
    cJSON_AddNumberToObject(storage, "total_bytes", st->total_bytes);
    cJSON_AddNumberToObject(storage, "free_bytes", st->free_bytes);
    cJSON_AddNumberToObject(storage, "error", st->mount_error);
    cJSON_AddStringToObject(storage, "error_name", esp_err_to_name(st->mount_error));

    cJSON *apps = cJSON_AddObjectToObject(root, "apps");
    cJSON_AddNumberToObject(apps, "installed", nucleo_registry_count());

    cJSON *net = cJSON_AddObjectToObject(root, "network");
    cJSON_AddStringToObject(net, "mode", nucleo_setup_mode());   // "sta" | "ap"
    cJSON_AddStringToObject(net, "ssid", nucleo_setup_ssid());
    cJSON_AddStringToObject(net, "ip", nucleo_setup_ip());
    cJSON_AddBoolToObject(net, "time_synced", nucleo_setup_time_synced());
    cJSON_AddNumberToObject(net, "time", (double)time(NULL));     // epoch seconds (UTC)

    // OTA snapshot so the Updates app can show the running slot + image state.
    cJSON *ota = cJSON_AddObjectToObject(root, "ota");
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    cJSON_AddStringToObject(ota, "running", run ? run->label : "?");
    cJSON_AddStringToObject(ota, "next", next ? next->label : "?");
    const char *sstr = "unknown";
    esp_ota_img_states_t ost;
    if (run && esp_ota_get_state_partition(run, &ost) == ESP_OK) {
        switch (ost) {
            case ESP_OTA_IMG_VALID:          sstr = "valid"; break;
            case ESP_OTA_IMG_PENDING_VERIFY: sstr = "pending"; break;
            case ESP_OTA_IMG_NEW:            sstr = "new"; break;
            case ESP_OTA_IMG_INVALID:        sstr = "invalid"; break;
            case ESP_OTA_IMG_ABORTED:        sstr = "aborted"; break;
            default:                         sstr = "undefined"; break;
        }
    }
    cJSON_AddStringToObject(ota, "state", sstr);
    cJSON_AddBoolToObject(ota, "rollback_enabled", true);   // CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE

    // Heavy-work arbiter: is a TLS/SD/heap job holding the single budget right now, and the
    // teardown heap-floor watermark. The shell shows a "busy" banner and leaves client-only apps
    // alone; a load test reads heap_free_min to confirm RAM never dipped below a safe floor.
    nucleo_arb_stat_t ab; nucleo_arb_snapshot(&ab);
    cJSON *arb = cJSON_AddObjectToObject(root, "arbiter");
    cJSON_AddBoolToObject(arb, "busy", ab.busy);
    cJSON_AddStringToObject(arb, "job", ab.job);
    cJSON_AddNumberToObject(arb, "held_ms", ab.held_ms);
    cJSON_AddNumberToObject(arb, "waiters", ab.waiters_fg + ab.waiters_bg);
    cJSON_AddNumberToObject(arb, "grants", ab.grants);
    cJSON_AddNumberToObject(arb, "denials", ab.denials);
    cJSON_AddNumberToObject(arb, "yields", ab.yields);
    cJSON_AddNumberToObject(arb, "heap_free_min", ab.heap_free_min);

    // Battery: real cell level off the ADC (nucleo_power). pct/mv are -1 when no reading is
    // available, so the web gauge can show "--" instead of a fabricated 100 %.
    cJSON *bat = cJSON_AddObjectToObject(root, "battery");
    cJSON_AddNumberToObject(bat, "pct", nucleo_power_battery_pct());
    cJSON_AddNumberToObject(bat, "mv", nucleo_power_battery_mv());

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return ESP_OK;
}

// Add one heap region's stats under `parent[key]`. On a no-PSRAM module the pool that matters
// is INTERNAL (scarce on-chip SRAM). `frag_pct` = 100*(1 - largest_free_block/total_free): how
// chopped-up the free pool is. A high value means we have bytes but no contiguous block — the
// exact failure mode behind "70 KB free yet a 32 KB alloc fails". `free_blocks` corroborates it.
static void add_heap_region(cJSON *parent, const char *key, uint32_t caps)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps);
    cJSON *r = cJSON_AddObjectToObject(parent, key);
    size_t total = info.total_free_bytes + info.total_allocated_bytes;
    cJSON_AddNumberToObject(r, "total_bytes", total);
    cJSON_AddNumberToObject(r, "free_bytes", info.total_free_bytes);
    cJSON_AddNumberToObject(r, "allocated_bytes", info.total_allocated_bytes);
    cJSON_AddNumberToObject(r, "largest_free_block", info.largest_free_block);
    cJSON_AddNumberToObject(r, "min_free_bytes", info.minimum_free_bytes);   // worst-case watermark since boot
    cJSON_AddNumberToObject(r, "free_blocks", info.free_blocks);
    cJSON_AddNumberToObject(r, "allocated_blocks", info.allocated_blocks);
    int frag = (info.total_free_bytes > 0)
        ? (int)(100 - (100ULL * info.largest_free_block) / info.total_free_bytes)
        : 0;
    cJSON_AddNumberToObject(r, "frag_pct", frag);
}

// GET /api/heap -> per-region heap diagnostics (fragmentation investigation). Public + read-only,
// like /api/status. INTERNAL is the scarce SRAM to watch on this no-PSRAM board; DMA is the
// subset usable for peripheral transfers; DEFAULT is what generic malloc() draws from.
static esp_err_t heap_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
    add_heap_region(root, "internal", MALLOC_CAP_INTERNAL);
    add_heap_region(root, "dma",      MALLOC_CAP_DMA);
    add_heap_region(root, "default",  MALLOC_CAP_DEFAULT);
    // Min free bytes ever on THIS (httpd) task's stack — the cascade runs here, so after a knowledge
    // query this is the deepest L1 usage. Lets us size the stack with data (overflow -> panic) over WiFi.
    cJSON_AddNumberToObject(root, "httpd_stack_free_min", uxTaskGetStackHighWaterMark(NULL));

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);   // same-origin only: no CORS header (diagnostics aren't cross-origin readable)
    cJSON_free(out);
    return ESP_OK;
}

// GET /api/wifi/scan -> run an active scan and return the visible APs as
// {"networks":[{"ssid","rssi","channel","auth"}]}. Drives the same nucleo_setup_scan() the native
// Wi-Fi app uses (blocking ~1-2 s, results de-duped + sorted by RSSI, serialized so the two callers
// can't scan in parallel). Same-origin diagnostic like /api/status -> no auth gate, no CORS header.
static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    int n = nucleo_setup_scan();
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid",    nucleo_setup_scan_ssid(i));
        cJSON_AddNumberToObject(o, "rssi",    nucleo_setup_scan_rssi(i));
        cJSON_AddNumberToObject(o, "channel", nucleo_setup_scan_channel(i));
        cJSON_AddStringToObject(o, "auth",    nucleo_setup_scan_auth_label(i));
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{\"networks\":[]}");
    cJSON_free(out);
    return ESP_OK;
}

// GET /api/wifi/known -> saved networks (the multi-network store), most-preferred ordering not
// guaranteed; the web manager sorts client-side. Reveals SSID + priority only (never passwords).
// {"networks":[{"ssid","priority","current"}],"mode":"sta|ap","ssid":"..."}. Auth-gated: it is
// user configuration, and join/forget below mutate the radio.
static esp_err_t wifi_known_get(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    const char *cur = nucleo_setup_ssid();
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "networks");
    int n = nucleo_setup_net_count();
    for (int i = 0; i < n; i++) {
        const char *ss = nucleo_setup_net_ssid(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ss);
        cJSON_AddNumberToObject(o, "priority", nucleo_setup_net_priority(i));
        cJSON_AddBoolToObject(o, "current", cur[0] && !strcmp(ss, cur));
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddStringToObject(root, "mode", nucleo_setup_mode());
    cJSON_AddStringToObject(root, "ssid", cur);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{\"networks\":[]}");
    cJSON_free(out);
    return ESP_OK;
}

// POST /api/wifi/join {"ssid":"..","pass":".."} -> join now and remember it (blocking up to ~12 s).
// Empty/absent pass = open network. Auth-gated (mutates the radio + stores credentials).
static esp_err_t wifi_join_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    int blen = req->content_len;
    if (blen <= 0 || blen > 256) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }
    char body[257] = {0};
    int got = 0, r;
    while (got < blen) {
        r = httpd_req_recv(req, body + got, blen - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        got += r;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *cs = cJSON_GetObjectItem(root, "ssid");
    cJSON *cp = cJSON_GetObjectItem(root, "pass");
    if (!cJSON_IsString(cs) || !cs->valuestring[0]) {
        cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid"); return ESP_FAIL;
    }
    bool ok = nucleo_setup_join(cs->valuestring, cJSON_IsString(cp) ? cp->valuestring : "");
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (ok) {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddBoolToObject(res, "ok", true);
        cJSON_AddStringToObject(res, "ip", nucleo_setup_ip());
        char *o = cJSON_PrintUnformatted(res); cJSON_Delete(res);
        httpd_resp_sendstr(req, o ? o : "{\"ok\":true}"); cJSON_free(o);
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false}");
    }
    return ESP_OK;
}

// POST /api/wifi/forget {"ssid":".."} -> forget one saved network (auto-reselects if it was current).
// {"all":true} forgets every saved network and drops to the hotspot. Auth-gated.
static esp_err_t wifi_forget_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    int blen = req->content_len;
    if (blen <= 0 || blen > 256) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }
    char body[257] = {0};
    int got = 0, r;
    while (got < blen) {
        r = httpd_req_recv(req, body + got, blen - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        got += r;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "all"))) {
        nucleo_setup_forget();
    } else {
        cJSON *cs = cJSON_GetObjectItem(root, "ssid");
        if (!cJSON_IsString(cs) || !cs->valuestring[0]) {
            cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid"); return ESP_FAIL;
        }
        nucleo_setup_forget_ssid(cs->valuestring);
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// GET /api/logs -> recent console log (the nucleo_log RAM ring: ESP_LOGx incl. reset cause, panic,
// OOM), so the device can be debugged over HTTP even with no SD card and no serial. Plain text,
// oldest->newest. (This used to serve a stale /sd/dj_log.txt left by an old DJ build, which hid the
// real boot/crash log — restored here so reboots are diagnosable over Wi-Fi.)
static esp_err_t logs_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char buf[NUCLEO_LOG_RING_SZ + 1];                 // ~2 KB on the 30 KB httpd stack — safe
    size_t n = nucleo_log_get(buf, sizeof buf);
    httpd_resp_send(req, buf, n);
    return ESP_OK;
}

// ANIMA online master switches (the full set is forward-declared with the other web handlers further
// down; /api/diag sits above that block, so it needs the two it reads here).
bool nucleo_anima_online_enabled(void);
bool nucleo_anima_online_only_enabled(void);

// Compact token for the cause of the LAST reset — the first thing a health diagnosis needs (a panic /
// brownout / watchdog reads completely differently from a clean SW restart or a fresh power-on).
static const char *diag_reset_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:   return "POWERON";    // cold start / battery reseat — benign
        case ESP_RST_SW:        return "SW";         // esp_restart() (our OTA/reboot path) — benign
        case ESP_RST_PANIC:     return "PANIC";      // crash (the bad one)
        case ESP_RST_INT_WDT:   return "INT_WDT";    // interrupt watchdog
        case ESP_RST_TASK_WDT:  return "TASK_WDT";   // task watchdog — a task hung
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";   // voltage sag — battery marginal (see stability notes)
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_USB:       return "USB";
        case ESP_RST_JTAG:      return "JTAG";
        default:                return "UNKNOWN";
    }
}

// GET /api/diag -> ONE consolidated, on-demand health snapshot for the Log Viewer's "Diagnose" digest.
// Everything a remote AI (or a human) needs to judge Cardputer + ANIMA health in a single fetch:
// reset cause, real firmware build, heap/fragmentation watermarks, Wi-Fi link quality, the heavy-work
// arbiter, CPU load, the OOM watermark, and ANIMA's tier/abstain telemetry. Pull-based and read-only:
// it costs nothing at rest (no task, no SD write, no TLS) — the device only pays when the user asks.
// Same-origin diagnostic like /api/heap (the Log Viewer is served from the device) -> no auth, no CORS.
static esp_err_t diag_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddNumberToObject(root, "ts", (double)time(NULL));

    // ── sys: identity + why the last boot ended + OTA/SD posture ──────────────────────────────────
    cJSON *sys = cJSON_AddObjectToObject(root, "sys");
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(sys, "fw",     app ? app->version : "?");
    cJSON_AddStringToObject(sys, "proj",   app ? app->project_name : "?");
    cJSON_AddStringToObject(sys, "built",  app ? app->date : "?");
    cJSON_AddStringToObject(sys, "idf",    app ? app->idf_ver : "?");
    cJSON_AddNumberToObject(sys, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(sys, "reset",  diag_reset_str(esp_reset_reason()));
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(sys, "slot", run ? run->label : "?");
    esp_ota_img_states_t ost; const char *ostr = "unknown";
    if (run && esp_ota_get_state_partition(run, &ost) == ESP_OK)
        ostr = ost == ESP_OTA_IMG_VALID ? "valid" : ost == ESP_OTA_IMG_PENDING_VERIFY ? "pending" :
               ost == ESP_OTA_IMG_INVALID ? "invalid" : ost == ESP_OTA_IMG_ABORTED ? "aborted" : "new";
    cJSON_AddStringToObject(sys, "ota", ostr);
    nucleo_storage_refresh();
    const nucleo_storage_info_t *st = nucleo_storage_info();
    cJSON_AddBoolToObject(sys, "sd", st->mounted);
    cJSON_AddNumberToObject(sys, "sd_free",  st->free_bytes);
    cJSON_AddNumberToObject(sys, "sd_total", st->total_bytes);

    // ── mem: the no-PSRAM SRAM that everything fights over. min = worst-ever watermark; frag = bytes
    //    present but not contiguous (the "70 KB free yet a 32 KB alloc fails" failure mode). ───────────
    cJSON *mem = cJSON_AddObjectToObject(root, "mem");
    size_t ifree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t ilblk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    cJSON_AddNumberToObject(mem, "free", ifree);
    cJSON_AddNumberToObject(mem, "min",  heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(mem, "lblk", ilblk);
    cJSON_AddNumberToObject(mem, "frag", ifree ? (int)(100 - (100ULL * ilblk) / ifree) : 0);
    cJSON_AddNumberToObject(mem, "dma_free", heap_caps_get_free_size(MALLOC_CAP_DMA));
    cJSON_AddNumberToObject(mem, "stack_httpd", uxTaskGetStackHighWaterMark(NULL));   // deepest L1/TLS usage on this task

    // ── net: link quality drives every online-ANIMA decision (a weak RSSI explains a flaky teacher) ──
    cJSON *net = cJSON_AddObjectToObject(root, "net");
    cJSON_AddStringToObject(net, "mode", nucleo_setup_mode());
    cJSON_AddStringToObject(net, "ssid", nucleo_setup_ssid());
    cJSON_AddStringToObject(net, "ip",   nucleo_setup_ip());
    cJSON_AddNumberToObject(net, "rssi", nucleo_setup_rssi());
    cJSON_AddBoolToObject(net,   "tsync", nucleo_setup_time_synced());
    cJSON_AddNumberToObject(net, "clients", nucleo_ws_client_count());

    // ── anima: the whole point of the "ANIMA health" ask — tier mix, abstain rate, which brain is live ─
    cJSON *an = cJSON_AddObjectToObject(root, "anima");
    cJSON_AddBoolToObject(an, "online_en",    nucleo_anima_online_enabled());
    cJSON_AddBoolToObject(an, "online_only",  nucleo_anima_online_only_enabled());   // online-only vs hybrid
    cJSON_AddBoolToObject(an, "online_avail", nucleo_anima_online_available());
    char prov[16] = "", model[40] = "";
    bool teacher = nucleo_anima_teacher_info(prov, sizeof prov, model, sizeof model);
    cJSON_AddBoolToObject(an,   "teacher",  teacher);
    cJSON_AddStringToObject(an, "provider", prov);
    cJSON_AddStringToObject(an, "model",    model);
    cJSON_AddNumberToObject(an, "l1_mode",    nucleo_anima_l1_get_mode());   // 0 AUTO · 1 ON · 2 OFF
    cJSON_AddBoolToObject(an,   "l1_serving", nucleo_anima_l1_serving());
    cJSON_AddNumberToObject(an, "l1_heap",    nucleo_anima_l1_heap_bytes());
    anima_diag_t ad; nucleo_anima_diag(&ad);
    cJSON_AddNumberToObject(an, "q",      ad.queries);
    cJSON_AddNumberToObject(an, "none",   ad.t_none);
    cJSON_AddNumberToObject(an, "cmd",    ad.t_command);
    cJSON_AddNumberToObject(an, "fact",   ad.t_fact);
    cJSON_AddNumberToObject(an, "stitch", ad.t_stitch);
    cJSON_AddNumberToObject(an, "remote", ad.t_remote);
    cJSON_AddNumberToObject(an, "last_conf", ad.last_conf);
    cJSON_AddStringToObject(an, "last", ad.last_intent);

    // ── arb: the single heavy-work token. denials climbing = the device is shedding load under pressure ─
    nucleo_arb_stat_t ab; nucleo_arb_snapshot(&ab);
    cJSON *arb = cJSON_AddObjectToObject(root, "arb");
    cJSON_AddBoolToObject(arb,   "busy",    ab.busy);
    cJSON_AddStringToObject(arb, "job",     ab.job);
    cJSON_AddNumberToObject(arb, "grants",  ab.grants);
    cJSON_AddNumberToObject(arb, "denials", ab.denials);
    cJSON_AddNumberToObject(arb, "yields",  ab.yields);
    cJSON_AddNumberToObject(arb, "hfmin",   ab.heap_free_min);

    // ── cpu: per-core load + task count (collo is RAM not CPU, but a pegged core still corroborates a hang) ─
    cJSON *cpu = cJSON_AddObjectToObject(root, "cpu");
    cJSON_AddNumberToObject(cpu, "cores", nucleo_cpu_core_count());
    cJSON_AddNumberToObject(cpu, "freq",  CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    cJSON_AddNumberToObject(cpu, "tasks", uxTaskGetNumberOfTasks());
    cJSON *load = cJSON_AddArrayToObject(cpu, "load");
    for (int c = 0; c < nucleo_cpu_core_count(); c++)
        cJSON_AddItemToArray(load, cJSON_CreateNumber(nucleo_cpu_load_pct(c)));

    // ── oom: the smoking gun. A non-zero count means an allocation was rejected since boot. ──────────
    unsigned oc = 0, osz = 0, ocaps = 0; nucleo_log_oom(&oc, &osz, &ocaps);
    cJSON *oom = cJSON_AddObjectToObject(root, "oom");
    cJSON_AddNumberToObject(oom, "count",     oc);
    cJSON_AddNumberToObject(oom, "last_size", osz);
    cJSON_AddNumberToObject(oom, "last_caps", ocaps);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    cJSON_free(out);
    return ESP_OK;
}

// POST /api/ota -> stream a new firmware .bin into the next OTA slot and reboot into it.
// Enables Wi-Fi firmware updates (no USB):  curl --data-binary @nucleoos.bin http://IP/api/ota
static esp_err_t ota_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);   // firmware flashing requires a paired session
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition"); return ESP_FAIL; }

    // Single-flight heavy-op gate (OS-wide "one download at a time"): an OTA is the heaviest download —
    // never let it overlap an outbound TLS fetch (proxy/llm, which take the SAME arbiter token) or a
    // second OTA. Concurrent heavy ops are exactly what socket-starve / OOM this PSRAM-less chip. 503 if busy.
    uint32_t tk = nucleo_arb_acquire(ARB_FG, "ota", 0);
    if (!tk) { httpd_resp_set_status(req, "503 Service Unavailable"); httpd_resp_set_hdr(req, "Retry-After", "2");
               httpd_resp_sendstr(req, "{\"busy\":true,\"job\":\"download\"}"); return ESP_FAIL; }

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err)); nucleo_arb_release(tk); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin"); return ESP_FAIL; }

    char buf[1024]; int r, total = 0;
    while ((r = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        if (total == 0 && (unsigned char)buf[0] != 0xE9) {   // ESP image magic — reject junk uploads fast
            esp_ota_abort(h); nucleo_arb_release(tk);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "not an ESP firmware image");
            return ESP_FAIL;
        }
        if (esp_ota_write(h, buf, r) != ESP_OK) { esp_ota_abort(h); nucleo_arb_release(tk); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write"); return ESP_FAIL; }
        total += r;
    }
    if (r < 0 || esp_ota_end(h) != ESP_OK) { nucleo_arb_release(tk); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "incomplete/invalid image"); return ESP_FAIL; }
    if (esp_ota_set_boot_partition(part) != ESP_OK) { nucleo_arb_release(tk); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot"); return ESP_FAIL; }

    nucleo_arb_release(tk);              // success: release the gate before the reboot (token is RAM-only)
    ESP_LOGI(TAG, "OTA ok: %d bytes -> %s, rebooting", total, part->label);
    char out[96]; snprintf(out, sizeof(out), "{\"ok\":true,\"bytes\":%d,\"slot\":\"%s\",\"reboot\":true}", total, part->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    vTaskDelay(pdMS_TO_TICKS(800));     // let the response flush, then boot the new image
    esp_restart();
    return ESP_OK;
}

// POST /api/reboot -> restart the device. Cheap reboot path (no 1.2 MB firmware re-flash) so an
// SD-only release can make the on-device ANIMA L1 reload a freshly-pushed index (it caches the
// AKB2 header + offsets at boot). Auth-gated like OTA.
static esp_err_t reboot_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":true}");
    ESP_LOGI(TAG, "reboot requested via /api/reboot");
    vTaskDelay(pdMS_TO_TICKS(800));     // let the response flush, then restart
    esp_restart();
    return ESP_OK;
}

// Minimal in-place URL-decode (%XX and '+') for query values.
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') { *o++ = ' '; }
        else if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = { p[1], p[2], 0 }; *o++ = (char)strtol(h, NULL, 16); p += 2;
        } else { *o++ = *p; }
    }
    *o = 0;
}

// SSRF guard for the server-side fetchers (/api/proxy, /api/llm). They will dial ANY host the
// caller names, so without this a page could aim them at the device's own LAN — the router admin
// panel, a NAS, a metadata service — turning the box into a pivot. We reject targets that are
// loopback or RFC1918/link-local *IP literals*. This is the feasible on-device defense: it stops
// the trivial "http://192.168.1.1/" abuse while leaving real external browsing untouched. It does
// NOT defeat DNS-rebinding (a public name resolving to a private IP) — esp_http_client resolves
// internally and we can't cheaply inspect the result; documented as a known limit.
static bool url_target_blocked(const char *url)
{
    const char *h = strstr(url, "://");
    if (!h) return false;
    h += 3;
    const char *at = NULL;
    for (const char *p = h; *p && *p != '/'; p++) { if (*p == '@') at = p; }   // strip userinfo
    if (at) h = at + 1;

    char host[100]; size_t i = 0;
    for (; h[i] && h[i] != '/' && h[i] != ':' && i < sizeof(host) - 1; i++) host[i] = h[i];
    host[i] = '\0';

    if (host[0] == '[') {                                  // IPv6 literal: block loopback/ULA/link-local
        const char *b = host + 1;
        if (!strncmp(b, "::1", 3) || !strncasecmp(b, "fe80", 4) ||
            !strncasecmp(b, "fc", 2) || !strncasecmp(b, "fd", 2)) return true;
        return false;
    }
    if (!strcasecmp(host, "localhost")) return true;

    unsigned a, b2, c, d;                                  // IPv4 literal -> classify the range
    if (sscanf(host, "%u.%u.%u.%u", &a, &b2, &c, &d) == 4 && a < 256 && b2 < 256 && c < 256 && d < 256) {
        if (a == 127 || a == 10 || a == 0) return true;                 // loopback / private / this-host
        if (a == 192 && b2 == 168) return true;                         // private
        if (a == 172 && b2 >= 16 && b2 <= 31) return true;             // private
        if (a == 169 && b2 == 254) return true;                        // link-local / cloud metadata
        if (a == 100 && b2 >= 64 && b2 <= 127) return true;            // CGNAT
    }
    return false;   // a hostname or a public IP: allowed
}

// ANIMA online-mode controls (defined in nucleo_anima*.c; forward-declared like app_anima.cpp).
void nucleo_anima_set_online(bool on);
bool nucleo_anima_online_enabled(void);
void nucleo_anima_set_online_only(bool on);
bool nucleo_anima_online_only_enabled(void);
bool nucleo_anima_online_available(void);                                  // STA up + master switch on
bool nucleo_anima_teacher_info(char *provider, int pcap, char *model, int mcap);  // active teacher (no key)
// Offline L1 brain serving policy (RAM optimization; defined in nucleo_anima_l1.c).
bool nucleo_anima_l1_serving(void);
int  nucleo_anima_l1_get_mode(void);
void nucleo_anima_l1_set_mode(int mode);
void nucleo_anima_l1_set_external_brain(bool on);

// Append an ANIMA-scheduled reminder to the OS calendar (/sd/system/config/calendar.json:
// { "events": { "YYYY-MM-DD": [ {"time","text"} ] } }). `spec` is the add_event tool's content-channel
// payload "off=<days>;time=<HH:MM|>;text=<...>"; the day offset is resolved against the RTC. Reads the
// existing JSON, appends, writes atomically (temp+rename), and fills a localized confirmation. The
// executor (here) does the side effect under the same pairing gate as create_file.
static bool anima_apply_event(const char *spec, bool en, char *reply, size_t rcap)
{
    if (!spec || !spec[0]) return false;
    int off = 0; char tm[8] = ""; const char *text = "";
    const char *p  = strstr(spec, "off=");   if (p)  off = atoi(p + 4);
    const char *pt = strstr(spec, ";time="); if (pt) { pt += 6; int i = 0; while (pt[i] && pt[i] != ';' && i < 7) { tm[i] = pt[i]; i++; } tm[i] = 0; }
    const char *px = strstr(spec, ";text="); if (px) text = px + 6;
    if (!text[0]) return false;

    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    t.tm_mday += off; t.tm_hour = 12; t.tm_min = 0; t.tm_sec = 0;   // noon: avoid a DST edge in mktime
    mktime(&t);
    char date[16]; strftime(date, sizeof(date), "%Y-%m-%d", &t);

    const char *path = NUCLEO_SD_MOUNT "/system/config/calendar.json";
    cJSON *root = NULL;
    bool had_data = false;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        had_data = n > 0;
        // 32 KB cap, sized to the device: the raw file + its cJSON tree (~2-3x) must coexist in a
        // ~40-76 KB-free heap. (The old 200 KB cap could never be parsed here anyway.)
        if (n > 0 && n < 32768) { char *b = malloc((size_t)n + 1); if (b) { size_t rd = fread(b, 1, (size_t)n, f); b[rd] = 0; root = cJSON_Parse(b); free(b); } }
        fclose(f);
    }
    // Fail-closed: if the calendar EXISTS but couldn't be loaded (oversized, OOM, corrupt JSON),
    // refuse the add. The old fallback rewrote the file with ONLY the new event — silently erasing
    // the whole calendar while still replying "Added".
    if (had_data && !root) return false;
    if (!root) root = cJSON_CreateObject();
    cJSON *events = cJSON_GetObjectItem(root, "events");
    if (!cJSON_IsObject(events)) { cJSON_DeleteItemFromObject(root, "events"); events = cJSON_AddObjectToObject(root, "events"); }
    cJSON *day = cJSON_GetObjectItem(events, date);
    if (!cJSON_IsArray(day)) { cJSON_DeleteItemFromObject(events, date); day = cJSON_AddArrayToObject(events, date); }
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "time", tm);
    cJSON_AddStringToObject(ev, "text", text);
    cJSON_AddItemToArray(day, ev);

    char *outc = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    bool ok = false;
    if (outc) {
        mkdir(NUCLEO_SD_MOUNT "/system", 0775); mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
        char tmp[160]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        FILE *o = fopen(tmp, "wb");
        if (o) { fwrite(outc, 1, strlen(outc), o); fclose(o); remove(path); ok = (rename(tmp, path) == 0); if (!ok) remove(tmp); }
        cJSON_free(outc);
    }
    if (ok) {
        nucleo_event_publish("calendar.changed", "{\"by\":\"anima\"}");
        if (tm[0]) snprintf(reply, rcap, en ? "Added \"%s\" on %s at %s." : "Aggiunto \"%s\" il %s alle %s.", text, date, tm);
        else       snprintf(reply, rcap, en ? "Added \"%s\" on %s."       : "Aggiunto \"%s\" il %s.",       text, date);
    }
    return ok;
}

// Pre-gate for an ONLINE (TLS) ANIMA escalation: a mbedTLS handshake + a long X.509 chain verify
// needs both a big contiguous allocation (the rx record buffer) AND a large SUM of small
// allocations; attempting it near the cliff OOMs and takes the single-task web server down. The
// two bars (NUCLEO_TLS_MIN_BLOCK/_FREE) are defined ONCE in nucleo_anima.h, shared with the fetch
// guard in nucleo_anima_online.c. This only trips when RAM is too tight to plausibly succeed,
// converting a crash into a graceful answer.
// Enough heap to risk a server-side TLS handshake right now? (largest block AND total free)
static inline bool nucleo_tls_heap_ok(void)
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= NUCLEO_TLS_MIN_BLOCK
        && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= NUCLEO_TLS_MIN_FREE;
}

// GET /api/anima?q=...  -> ANIMA offline assistant (L0). Understands a typed Italian line
// and returns an action (launch an app / answer / live system value). See docs/anima.md.
static esp_err_t anima_get(httpd_req_t *req)
{
    char q[160] = { 0 };
    char lang[4] = "it";
    char query[208];
    int mode_ov = 0;   // per-request online mode from the web app: 0=none, 1=off, 2=on(hybrid), 3=only
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[160];
        if (httpd_query_key_value(query, "q", val, sizeof(val)) == ESP_OK) {
            url_decode(val);
            strncpy(q, val, sizeof(q) - 1);
        }
        char lv[8];
        if (httpd_query_key_value(query, "lang", lv, sizeof(lv)) == ESP_OK && (lv[0] == 'e' || lv[0] == 'E'))
            strcpy(lang, "en");
        char rv[4];
        if (httpd_query_key_value(query, "reset", rv, sizeof(rv)) == ESP_OK && rv[0] == '1')
            nucleo_anima_reset_session();   // "pulisci conversazione" -> forget device-side context
        char mv[8];
        if (httpd_query_key_value(query, "mode", mv, sizeof(mv)) == ESP_OK && mv[0]) {
            if (mv[0] == 'o' && mv[1] == 'f') mode_ov = 1;        // off  -> offline-only
            else if (!strcmp(mv, "only"))     mode_ov = 3;        // only -> online-only
            else                              mode_ov = 2;        // on   -> hybrid
        }
    }
    bool en = lang[0] == 'e';

    // Low-heap guard (see NUCLEO_TLS_MIN_BLOCK/_FREE): when SRAM is too tight for a safe
    // TLS handshake, force this turn OFFLINE so a doomed online attempt can't OOM the web server.
    // The online path UNLOADS the L1 index (~31 KB) right before the handshake, so judge feasibility
    // against the POST-reclaim heap (current largest block + what L1 would free) — not the tighter
    // heap with L1 still resident, which would otherwise veto EVERY online turn on this PSRAM-less
    // chip and silently answer "no internet". A genuinely low turn still falls back safely: the fetch
    // helper (online_tls_heap_too_low) re-checks the real contiguous block before mbedTLS allocates.
    size_t l1_reclaim = nucleo_anima_l1_heap_bytes();   // freed right before the handshake in the online path
#if NUCLEO_HEAPLOG
    // Snapshot + IN/OUT delta are gated as a unit: when heap logging is off this whole block (and the
    // heap_caps reads) vanishes — no cost, no set-but-unused warning on heap_in.
    size_t heap_in = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "anima IN q='%.40s' mode=%d free=%u largest=%u l1=%u",
             q, mode_ov, (unsigned)heap_in,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)l1_reclaim);
#endif
    // NEVER downgrade an explicit ONLINE-ONLY request (mode_ov==3) to offline: that would silently
    // answer from the offline cascade in "solo online" mode — exactly what the user forbids. The online
    // path re-checks the real contiguous block right before mbedTLS (online_tls_heap_too_low) and the
    // online-only cascade returns an honest "no answer" instead of OOM-ing. Hybrid (2) and default (0)
    // still fall back to offline under pressure, which is their contract.
    if (mode_ov != 1 && mode_ov != 3 &&
        (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) + l1_reclaim < NUCLEO_TLS_MIN_BLOCK ||
         heap_caps_get_free_size(MALLOC_CAP_INTERNAL)          + l1_reclaim < NUCLEO_TLS_MIN_FREE)) {
        mode_ov = 1;   // off -> offline-only for this turn
    }

    // Spine gate: serialize the cascade against the native ANIMA worker so only one owner touches the
    // shared L1/session state (fixes the concurrent use-after-free that rebooted the device). httpd must
    // NEVER block -> try-only; on contention return a lean 503 "busy, retry" (no new endpoint, ~30 B).
    if (!nucleo_anima_try_lock()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "1");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"busy\":true,\"retry_after_ms\":250}");
        return ESP_OK;
    }

    // Apply a per-request online mode (web ANIMA mode selector), saving/restoring the device-global
    // state so this never clobbers the native Settings toggle. Mirrors the sim's `mode` param.
    bool saved_online = nucleo_anima_online_enabled();
    bool saved_only   = nucleo_anima_online_only_enabled();
    if (mode_ov == 1)      { nucleo_anima_set_online(false); nucleo_anima_set_online_only(false); }
    else if (mode_ov == 2) { nucleo_anima_set_online(true);  nucleo_anima_set_online_only(false); }
    else if (mode_ov == 3) { nucleo_anima_set_online(true);  nucleo_anima_set_online_only(true); }

    anima_result_t r = nucleo_anima_query(q, lang);

    if (mode_ov) {
        // Compare-and-restore: restore the saved mode ONLY if the globals still hold OUR override.
        // The native Settings tab writes these flags from the UI task without the spine lock — a
        // blind restore here silently cancelled a toggle the user made during a long online query.
        bool want_online = (mode_ov != 1), want_only = (mode_ov == 3);
        if (nucleo_anima_online_enabled() == want_online && nucleo_anima_online_only_enabled() == want_only) {
            nucleo_anima_set_online(saved_online); nucleo_anima_set_online_only(saved_only);
        }
    }
    nucleo_anima_unlock();                 // spine released; r is a local copy, the reply-building below touches no shared ANIMA state

    // Resolve a live SYSTEM value into the reply template's {value} placeholder (localized).
    char reply[1100];  // capabilities/agenda answers are long; a code snippet from the online model longer
    if (r.action == ANIMA_ACT_SYSTEM) {
        char value[640]; snprintf(value, sizeof(value), "%s", en ? "unavailable" : "non disponibile");
        if (!strcmp(r.arg, "time")) {
            time_t now = time(NULL); struct tm *tm = localtime(&now);
            if (tm && now > 1672531200) nucleo_tts_speak_time(value, (int)sizeof(value), tm->tm_hour, tm->tm_min, en ? "en" : "it");
            else snprintf(value, sizeof(value), "%s", en ? "I don't know the time: the clock isn't set" : "Non conosco l'ora: l'orologio non e' impostato");
        } else if (!strcmp(r.arg, "storage")) {
            nucleo_storage_refresh();
            const nucleo_storage_info_t *st = nucleo_storage_info();
            if (st->mounted)
                snprintf(value, sizeof(value), en ? "%.1f GB free of %.1f GB" : "%.1f GB liberi su %.1f GB",
                         st->free_bytes / 1e9, st->total_bytes / 1e9);
        } else if (!strcmp(r.arg, "date") || !strcmp(r.arg, "year") || !strcmp(r.arg, "season")) {
            // Computed-from-state: derive date/year/season from the (NTP-synced) RTC. Always exact.
            static const char *WD_IT[] = {"domenica","lunedi","martedi","mercoledi","giovedi","venerdi","sabato"};
            static const char *WD_EN[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
            static const char *MO_IT[] = {"gennaio","febbraio","marzo","aprile","maggio","giugno","luglio","agosto","settembre","ottobre","novembre","dicembre"};
            static const char *MO_EN[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
            static const char *SE_IT[] = {"inverno","primavera","estate","autunno"};
            static const char *SE_EN[] = {"winter","spring","summer","autumn"};
            time_t now = time(NULL); struct tm *tm = localtime(&now);
            if (tm) {
                int y = tm->tm_year + 1900, mo = tm->tm_mon, wd = tm->tm_wday, d = tm->tm_mday;
                if (!strcmp(r.arg, "year")) {
                    snprintf(value, sizeof(value), "%d", y);
                } else if (!strcmp(r.arg, "season")) {
                    // Astronomical seasons (Northern hemisphere): they turn at the equinoxes/
                    // solstices ~the 20th-22nd, NOT the 1st — so 3 June is still SPRING, not summer.
                    int s = ((mo == 2 && d >= 20) || mo == 3 || mo == 4 || (mo == 5 && d <= 20)) ? 1   // spring
                          : ((mo == 5 && d >= 21) || mo == 6 || mo == 7 || (mo == 8 && d <= 22)) ? 2   // summer
                          : ((mo == 8 && d >= 23) || mo == 9 || mo == 10 || (mo == 11 && d <= 20)) ? 3 // autumn
                          : 0;                                                                          // winter
                    snprintf(value, sizeof(value), "%s", en ? SE_EN[s] : SE_IT[s]);
                } else {  // date
                    if (en) snprintf(value, sizeof(value), "Today is %s, %s %d %d", WD_EN[wd], MO_EN[mo], d, y);
                    else    snprintf(value, sizeof(value), "Oggi e %s %d %s %d", WD_IT[wd], d, MO_IT[mo], y);
                }
            }
        } else if (!strcmp(r.arg, "agenda")) {
            // Read today's appointments from the OS calendar (/system/config/calendar.json,
            // { events: { "YYYY-MM-DD": [ {time,text,id} ] } }) and filter by the RTC date.
            snprintf(value, sizeof(value), en ? "you have no events today" : "oggi non hai impegni");
            time_t now = time(NULL); struct tm *tm = localtime(&now);
            FILE *f = tm ? fopen(NUCLEO_SD_MOUNT "/system/config/calendar.json", "rb") : NULL;
            if (f) {
                fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                char *buf = (sz > 0 && sz < 32768) ? malloc(sz + 1) : NULL;   // same cap as apply_event
                if (buf && fread(buf, 1, sz, f) == (size_t)sz) {
                    buf[sz] = 0;
                    char key[16];   // bound the args so the formatter proves it fits (real values unchanged)
                    snprintf(key, sizeof(key), "%04d-%02d-%02d", (tm->tm_year + 1900) % 10000, (tm->tm_mon + 1) % 100, tm->tm_mday % 100);
                    cJSON *root = cJSON_Parse(buf);
                    cJSON *evs = root ? cJSON_GetObjectItem(root, "events") : NULL;
                    cJSON *today = evs ? cJSON_GetObjectItem(evs, key) : NULL;
                    int cnt = (today && cJSON_IsArray(today)) ? cJSON_GetArraySize(today) : 0;
                    if (cnt > 0) {
                        char list[200] = ""; cJSON *ev;
                        cJSON_ArrayForEach(ev, today) {
                            const cJSON *t = cJSON_GetObjectItem(ev, "time"), *tx = cJSON_GetObjectItem(ev, "text");
                            const char *ts = cJSON_IsString(t) ? t->valuestring : "", *txs = cJSON_IsString(tx) ? tx->valuestring : "";
                            char one[96]; snprintf(one, sizeof(one), "%s%s%s", ts, ts[0] ? " " : "", txs);
                            if (list[0] && strlen(list) + strlen(one) + 3 < sizeof(list)) strncat(list, "; ", sizeof(list) - strlen(list) - 1);
                            if (strlen(list) + strlen(one) + 1 < sizeof(list)) strncat(list, one, sizeof(list) - strlen(list) - 1);
                        }
                        if (en) snprintf(value, sizeof(value), "you have %d today: %s", cnt, list);
                        else    snprintf(value, sizeof(value), "oggi hai %d %s: %s", cnt, cnt == 1 ? "impegno" : "impegni", list);
                    }
                    if (root) cJSON_Delete(root);
                }
                free(buf); fclose(f);
            }
        } else if (!strcmp(r.arg, "capabilities")) {
            // DYNAMIC "what can I do": the live app list (from the registry) + the pillars.
            const nucleo_app_t *apps = nucleo_registry_apps(); int n = nucleo_registry_count();
            char applist[80] = ""; int shown = 0;
            for (int i = 0; i < n && shown < 5; i++) {
                const char *nm = apps[i].name[0] ? apps[i].name : apps[i].id;
                if (applist[0] && strlen(applist) + strlen(nm) + 3 < sizeof(applist)) strncat(applist, ", ", sizeof(applist) - strlen(applist) - 1);
                if (strlen(applist) + strlen(nm) + 1 < sizeof(applist)) { strncat(applist, nm, sizeof(applist) - strlen(applist) - 1); shown++; }
            }
            if (en) snprintf(value, sizeof(value),
                "I can open your %d apps (%s...), tell time/date/season/space/battery, give a city's weather online, manage your calendar (add reminders, read today's), solve math/physics/geometry/vectors and Ohm's law, convert units, write spreadsheet formulas, create files, and answer about NucleoOS, C, electronics and general topics. I fix typos and, when a topic is both a fact and a skill, I ask which you mean.", n, applist);
            else snprintf(value, sizeof(value),
                "Posso aprire le tue %d app (%s...), darti ora/data/stagione/spazio/batteria, il meteo online di una citta, gestire il calendario (aggiungo promemoria e leggo gli impegni), risolvere matematica/fisica/geometria/vettori e la legge di Ohm, fare conversioni, scrivere formule per il foglio di calcolo, creare file, e rispondere su NucleoOS, C, elettronica e cultura generale. Correggo i refusi e, se un argomento e sia nozione sia skill, ti chiedo quale intendi.", n, applist);
        } else if (!strcmp(r.arg, "network")) {
            // Live Wi-Fi state (the same source as /api/status): mode + SSID + IP.
            const char *mode = nucleo_setup_mode(), *ssid = nucleo_setup_ssid(), *ip = nucleo_setup_ip();
            if (mode && !strcmp(mode, "ap"))
                snprintf(value, sizeof(value), en ? "I'm a Wi-Fi hotspot \"%s\", IP %s" : "Sono un hotspot Wi-Fi \"%s\", IP %s", ssid, ip);
            else if (ssid && ssid[0])
                snprintf(value, sizeof(value), en ? "connected to \"%s\", IP %s" : "connesso a \"%s\", IP %s", ssid, ip);
            else
                snprintf(value, sizeof(value), en ? "not connected" : "non connesso");
        } else if (!strcmp(r.arg, "ram")) {
            // Free heap right now (MALLOC_CAP_DEFAULT), in KB — the live RAM headroom.
            unsigned kb = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024);
            snprintf(value, sizeof(value), en ? "%u KB of RAM free" : "%u KB di RAM liberi", kb);
        } else if (!strcmp(r.arg, "version")) {
            snprintf(value, sizeof(value), "NucleoOS 0.1.0");
        } else if (!strcmp(r.arg, "uptime")) {
            long s = (long)(esp_timer_get_time() / 1000000);
            int dd = (int)(s / 86400), hh = (int)((s % 86400) / 3600), mm = (int)((s % 3600) / 60);
            if (dd)      snprintf(value, sizeof(value), en ? "%dd %dh" : "%dg %dh", dd, hh);
            else if (hh) snprintf(value, sizeof(value), "%dh %dm", hh, mm);
            else         snprintf(value, sizeof(value), "%dm", mm);
        }
        const char *ph = strstr(r.reply, "{value}");
        if (ph) snprintf(reply, sizeof(reply), "%.*s%s%s", (int)(ph - r.reply), r.reply, value, ph + 7);
        else    snprintf(reply, sizeof(reply), "%s", r.reply);
    } else {
        snprintf(reply, sizeof(reply), "%s", r.reply);
    }

    // Tool execution (function calling). Side-effecting tools require a paired session
    // (queries stay public; only the write is gated). create_file -> empty .txt on the SD.
    char tool_path[96] = "";
    if (r.action == ANIMA_ACT_TOOL && strcmp(r.intent, "create_file") == 0 && r.arg[0]) {
        const char *bn = strrchr(r.arg, '/'); bn = bn ? bn + 1 : r.arg;
        if (!nucleo_auth_request_ok(req)) {
            snprintf(reply, sizeof(reply), en ? "Pairing required to create files (enter the PIN)."
                                              : "Per creare file devo essere associato (inserisci il PIN).");
            nucleo_anima_observe("create_file", false);   // blocked -> close the loop (no stale state)
        } else {
            char path[128]; snprintf(path, sizeof(path), NUCLEO_SD_MOUNT "%s", r.arg);  // /data/<Folder>/<name>
            char dir[128]; snprintf(dir, sizeof(dir), "%s", path);                       // ensure the folder exists
            char *slash = strrchr(dir, '/'); if (slash && slash != dir) { *slash = 0; mkdir(dir, 0775); }  // idempotent
            FILE *ex = fopen(path, "rb");
            if (ex) {                                  // never silently overwrite (data loss)
                fclose(ex);
                snprintf(reply, sizeof(reply), en ? "%s already exists — not overwritten."
                                                  : "%s esiste gia: non lo sovrascrivo.", bn);
                nucleo_anima_note_file(r.arg);         // it exists -> follow-up "aprilo" can open it
                nucleo_anima_observe("create_file", true);
            } else {
                FILE *cf = fopen(path, "wb");
                if (cf) { const char *body = nucleo_anima_tool_content();   // compose-then-act payload ("" -> empty file)
                          if (body && body[0]) fwrite(body, 1, strlen(body), cf);
                          fclose(cf); snprintf(tool_path, sizeof(tool_path), "%s", r.arg);
                          nucleo_anima_note_file(r.arg);     // remember only a real, created file
                          nucleo_anima_observe("create_file", true);
                          nucleo_event_publish("fs.changed", "{\"op\":\"create\",\"by\":\"anima\"}"); }  // File Commander refresh
                else    { snprintf(reply, sizeof(reply), en ? "Can't create %s." : "Non riesco a creare %s.", bn);
                          nucleo_anima_observe("create_file", false); }
            }
        }
    }
    // add_event -> append the reminder to the OS calendar (same pairing gate as create_file).
    else if (r.action == ANIMA_ACT_TOOL && strcmp(r.intent, "add_event") == 0) {
        if (!nucleo_auth_request_ok(req)) {
            snprintf(reply, sizeof(reply), en ? "Pairing required to add events (enter the PIN)."
                                              : "Per aggiungere eventi devo essere associato (inserisci il PIN).");
            nucleo_anima_observe("add_event", false);
        } else if (anima_apply_event(nucleo_anima_tool_content(), en, reply, sizeof(reply))) {
            nucleo_anima_observe("add_event", true);
        } else {
            snprintf(reply, sizeof(reply), en ? "I couldn't add the event." : "Non sono riuscito ad aggiungere l'evento.");
            nucleo_anima_observe("add_event", false);
        }
    }

    const char *tier = r.tier == ANIMA_TIER_COMMAND ? "command" :
                       r.tier == ANIMA_TIER_FACT ? "fact" :
                       r.tier == ANIMA_TIER_REMOTE ? "remote" : "none";   // REMOTE = weather/fx/grok/wiki; was mis-serialized as "none"
    const char *action = r.action == ANIMA_ACT_LAUNCH ? "launch" :
                         r.action == ANIMA_ACT_SYSTEM ? "system" :
                         r.action == ANIMA_ACT_ANSWER ? "answer" :
                         r.action == ANIMA_ACT_TOOL ? "tool" : "none";

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "query", q);
    cJSON_AddStringToObject(root, "tier", tier);
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "arg", r.arg);
    // A long online answer (a multi-line code snippet) overruns the fixed reply[] — serve the full
    // heap-channel text verbatim when present (cJSON handles long strings; the web renders the fenced block).
    const char *long_reply = nucleo_anima_long_reply();
    cJSON_AddStringToObject(root, "reply", (long_reply && long_reply[0]) ? long_reply : reply);
    if (tool_path[0]) cJSON_AddStringToObject(root, "path", tool_path);
    cJSON_AddStringToObject(root, "tool", r.action == ANIMA_ACT_TOOL ? r.intent : "");
    cJSON_AddNumberToObject(root, "confidence", r.confidence);
    // micro-thought: the structured decision behind the reply (observable plan).
    const char *domain = strcmp(r.intent, "clarify") == 0 ? "clarify" :   // before FACT: L1 clarify is tier FACT
                         strcmp(r.intent, "weather") == 0 ? "meteo" :
                         strcmp(r.intent, "fx") == 0 ? "cambio" :
                         strcmp(r.intent, "grok") == 0 ? "online" :
                         r.tier == ANIMA_TIER_REMOTE ? "online" :
                         r.tier == ANIMA_TIER_FACT ? "knowledge" :
                         strcmp(r.intent, "capabilities") == 0 ? "faq" :
                         strcmp(r.intent, "agenda") == 0 ? "agenda" :
                         (strcmp(r.intent,"calc")==0 || strcmp(r.intent,"percent")==0 ||
                          strcmp(r.intent,"convert")==0 || strcmp(r.intent,"ohm")==0) ? "calc" :
                         r.action == ANIMA_ACT_TOOL ? "tool" :
                         r.action == ANIMA_ACT_SYSTEM ? "system" :
                         r.action == ANIMA_ACT_LAUNCH ? "app" :
                         r.action == ANIMA_ACT_ANSWER ? "faq" : "none";
    cJSON_AddStringToObject(root, "domain", domain);
    cJSON_AddStringToObject(root, "intent", r.intent);
    cJSON_AddStringToObject(root, "lang", lang);
    cJSON_AddNumberToObject(root, "budget", r.budget);
    cJSON_AddBoolToObject(root, "memory", r.from_memory ? 1 : 0);
    cJSON_AddStringToObject(root, "state", r.state);            // FSM state (controller observability)
    cJSON_AddBoolToObject(root, "awaiting", r.awaiting ? 1 : 0); // 1 = reply is a question awaiting input
    cJSON_AddStringToObject(root, "corrected", r.corrected);    // typo-corrected query understood ("" if clean)
    cJSON_AddStringToObject(root, "trace", r.trace);            // visible reasoning step-log (" · " joined)

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);   // same-origin only: the assistant can't be driven cross-origin
    cJSON_free(out);

#if NUCLEO_HEAPLOG
    // Per-query accounting. delta = heap NOT given back this turn (a steady-state leak if > 0 over
    // many identical queries). stack = bytes of THIS task's stack still unused at its deepest point
    // (uxTaskGetStackHighWaterMark, words->bytes): the cascade + a TLS handshake run in this 16 KB
    // httpd task, and a small remaining margin here means the next reboot may be a stack overflow,
    // NOT a heap OOM — a distinction the free/largest numbers alone can't make.
    size_t heap_out = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "anima OUT tier=%s delta=%d free=%u largest=%u min=%u stackleft=%u",
             tier, (int)((long)heap_in - (long)heap_out), (unsigned)heap_out,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
#endif
    return ESP_OK;
}

// GET /api/anima/verify?kind=numeric|fact&key=...&asserted=...&lang=it
// CROSS-SUBSTRATE GROUNDED VERIFICATION (ANIMA Forge): judge ONE structured claim — extracted by the
// browser from a GENERATIVE (M4 local-LLM / M3 Grok) answer — against the device's own zero-hallucination
// brain. Read-only, no side effects, same-origin. Returns {checks:[{status,evidence}]} so the web client
// folds it into the verdict (confirmed/contradicted/unknown → pass/veto/warn). The CASCADE side
// (nucleo_anima_verify_claim) is host-gated by tools/anima-host/forge-verify-claim.test.mjs; this thin
// wrapper compiles only under ESP-IDF — confirm on the next flash to the .166.
static esp_err_t anima_verify_get(httpd_req_t *req)
{
    char kind[16] = { 0 }, key[160] = { 0 }, asserted[128] = { 0 }, lang[4] = "it";
    char query[440];
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[200];
        if (httpd_query_key_value(query, "kind", v, sizeof v) == ESP_OK)     { url_decode(v); strncpy(kind, v, sizeof(kind) - 1); }
        if (httpd_query_key_value(query, "key", v, sizeof v) == ESP_OK)      { url_decode(v); strncpy(key, v, sizeof(key) - 1); }
        if (httpd_query_key_value(query, "asserted", v, sizeof v) == ESP_OK) { url_decode(v); strncpy(asserted, v, sizeof(asserted) - 1); }
        char lv[8];
        if (httpd_query_key_value(query, "lang", lv, sizeof lv) == ESP_OK && (lv[0] == 'e' || lv[0] == 'E')) strcpy(lang, "en");
    }
    // Spine gate, same pattern as anima_get: verify_claim walks L1/HDC (shared index, the HDC global
    // scratch) — concurrent with a native query it double-frees g_simcnt/g_bcnt. Try-only, never block.
    if (!nucleo_anima_try_lock()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "1");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"busy\":true,\"retry_after_ms\":250}");
        return ESP_OK;
    }
    char ev[256];
    anima_verify_t vr = nucleo_anima_verify_claim(kind, key, asserted, lang, ev, sizeof ev);
    nucleo_anima_unlock();
    const char *st = vr == ANIMA_VERIFY_CONFIRMED ? "confirmed" : vr == ANIMA_VERIFY_CONTRADICTED ? "contradicted" : "unknown";
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "checks");
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "status", st);
    cJSON_AddStringToObject(c, "evidence", ev);
    cJSON_AddItemToArray(arr, c);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return ESP_OK;
}

// GET /api/apps -> installed apps with display fields for the desktop shell.
static esp_err_t apps_get(httpd_req_t *req)
{
    const nucleo_app_t *apps = nucleo_registry_apps();
    int n = nucleo_registry_count();

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "apps");
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "id", apps[i].id);
        cJSON_AddStringToObject(a, "name", apps[i].name);
        cJSON_AddStringToObject(a, "route", apps[i].web_route);
        cJSON_AddStringToObject(a, "icon", apps[i].icon);
        cJSON_AddBoolToObject(a, "enabled", apps[i].enabled);
        cJSON_AddItemToArray(arr, a);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return ESP_OK;
}

// GET /api/associations -> serves the file-association map for the shell.
static esp_err_t assoc_get(httpd_req_t *req)
{
    FILE *f = fopen(NUCLEO_SD_MOUNT "/system/registry/file-associations.json", "rb");
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no associations"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char buf[512]; size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0)
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { fclose(f); return ESP_FAIL; }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// GET /api/proxy?url=<encoded> -> fetch the page server-side and stream it back. The Browser app
// is served BY this device, so a same-origin endpoint sidesteps the browser's CORS wall entirely
// (no third-party CORS proxy). esp_http_client + the built-in cert bundle handle HTTPS + redirects;
// the body is streamed in small chunks so we never buffer a whole page (RAM is scarce here).
typedef struct { httpd_req_t *req; bool started; char ctype[80]; } proxy_ctx_t;

static esp_err_t proxy_evt(esp_http_client_event_t *e)
{
    proxy_ctx_t *c = (proxy_ctx_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (e->header_key && !strcasecmp(e->header_key, "Content-Type"))
            snprintf(c->ctype, sizeof c->ctype, "%s", e->header_value ? e->header_value : "");
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (!c->started) {                          // first byte: commit headers, then stream
            c->started = true;
            httpd_resp_set_type(c->req, c->ctype[0] ? c->ctype : "text/html; charset=utf-8");
        }
        if (e->data_len > 0 &&
            httpd_resp_send_chunk(c->req, (const char *)e->data, e->data_len) != ESP_OK)
            return ESP_FAIL;                        // client gone -> abort the fetch
    }
    return ESP_OK;
}

static esp_err_t proxy_get(httpd_req_t *req)
{
    char query[1600], url[1100] = { 0 };
    if (httpd_req_get_url_query_len(req) == 0 ||
        httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "url", url, sizeof url) != ESP_OK || !url[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
        return ESP_FAIL;
    }
    url_decode(url);
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url must be http(s)");
        return ESP_FAIL;
    }
    if (url_target_blocked(url)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "target host not allowed");
        return ESP_FAIL;
    }

    // Server-side TLS on a PSRAM-less chip: free the L1 index (~31 KB; reloads on the next ANIMA query)
    // so the handshake has heap, then refuse gracefully if it's STILL too tight — never let mbedTLS
    // OOM-crash the web server (an unguarded fetch here took the device down once). The reclaim is
    // gated (_if_idle): freeing L1 under a running cascade on another task is a use-after-free.
    if (!nucleo_tls_heap_ok()) nucleo_anima_l1_unload_if_idle();
    if (!nucleo_tls_heap_ok()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "low memory for TLS fetch, retry");
        return ESP_FAIL;
    }

    proxy_ctx_t ctx = { .req = req, .started = false, .ctype = { 0 } };
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = proxy_evt,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,   // trust public CAs without per-site certs
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 1536,                        // long request URLs need a bigger TX buffer
        .max_redirection_count = 5,                    // follow 30x (google.com -> www., etc.)
        .user_agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
    };
    // Take the single heavy-work budget across the whole TLS window (init..cleanup). try-only
    // (timeout 0) so the single httpd task NEVER blocks: if another fetch/transcribe holds it,
    // tell the client to retry — same graceful 503 as the low-heap bail above.
    uint32_t tk = nucleo_arb_acquire(ARB_FG, "proxy", 0);
    if (!tk) { httpd_resp_set_status(req, "503 Service Unavailable");
               httpd_resp_sendstr(req, "busy with another TLS fetch, retry"); return ESP_FAIL; }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { nucleo_arb_release(tk); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "client init"); return ESP_FAIL; }

    esp_err_t err = esp_http_client_perform(client);   // follows redirects, streams via proxy_evt
    esp_http_client_cleanup(client);
    nucleo_arb_release(tk);                            // TLS torn down -> free the budget (samples heap floor)

    if (!ctx.started) {                                // nothing streamed (network/TLS error or empty body)
        if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err)); return ESP_FAIL; }
        httpd_resp_set_type(req, "text/html; charset=utf-8");
    }
    httpd_resp_send_chunk(req, NULL, 0);               // terminate the chunked response
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

// {GET,POST} /api/llm?url=<encoded https endpoint> -> forward to an OpenAI-compatible LLM API
// (Groq) server-side and stream the (SSE) response back. Same-origin, so the chat app dodges the
// browser CORS wall (like /api/proxy). The caller's Authorization: Bearer <key> header is relayed
// as-is — the key lives in the browser, we only pass it through. A POST with a body STREAMS the request
// straight through (no full-body buffer → context size is NOT heap-bounded); a GET reuses proxy_evt.
// GET /api/anima/caps — tell the web UI which cloud teacher is configured, WITHOUT revealing the key.
// {hasKey · online · enabled · provider?("anthropic"|"openai") · model?}. Lets the shell/app drive
// onboarding and pick the Cloud engine without ever reading the raw key off the SD. Public (read-only,
// no secret) — same exposure as /api/status.
static esp_err_t anima_caps_get(httpd_req_t *req)
{
    char provider[16] = "", model[64] = "";
    bool has_key = nucleo_anima_teacher_info(provider, sizeof provider, model, sizeof model);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "hasKey", has_key);
    cJSON_AddBoolToObject(root, "online", nucleo_anima_online_available());
    cJSON_AddBoolToObject(root, "enabled", nucleo_anima_online_enabled());
    if (has_key) { cJSON_AddStringToObject(root, "provider", provider); cJSON_AddStringToObject(root, "model", model); }
    // Offline L1 brain policy, so the UI can show "offline AI: stood down" and offer the re-enable toggle.
    { int l1m = nucleo_anima_l1_get_mode();
      cJSON_AddStringToObject(root, "l1Mode", l1m == 1 ? "on" : l1m == 2 ? "off" : "auto");
      cJSON_AddBoolToObject(root, "l1Serving", nucleo_anima_l1_serving()); }
    char *out = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    return ESP_OK;
}

// POST /api/anima/l1 — set the offline L1 brain policy from the ANIMA web app (same-origin; no secret,
// so no CORS header — only the device's own pages call it). Body: {"mode":"auto|on|off"?, "browserLLM":bool?}.
//   mode       — user override: "on" forces the offline brain on, "off" never serves, "auto" (default)
//                stands it down whenever a stronger brain is available.
//   browserLLM — true when a browser-hosted generative LLM is the active engine, so AUTO frees L1's RAM.
// Returns the resulting {l1Mode,l1Serving}. set_mode/set_external_brain free the index immediately if
// the policy turns L1 off, so the heap is reclaimed the instant the user goes online/local-LLM.
static esp_err_t anima_l1_post(httpd_req_t *req)
{
    // Spine gate: set_mode/set_external_brain can free the L1 index — doing that while the native
    // worker is mid-query is the same use-after-free anima_get guards against. Try-only, never block.
    if (!nucleo_anima_try_lock()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Retry-After", "1");
        httpd_resp_sendstr(req, "{\"busy\":true,\"retry_after_ms\":600}");
        return ESP_OK;
    }
    int blen = req->content_len;
    if (blen > 0 && blen < 256) {
        char buf[256]; int got = 0, r;
        while (got < blen) { r = httpd_req_recv(req, buf + got, blen - got); if (r <= 0) break; got += r; }
        buf[got > 0 ? got : 0] = 0;
        cJSON *in = cJSON_Parse(buf);
        if (in) {
            cJSON *m = cJSON_GetObjectItem(in, "mode");
            if (cJSON_IsString(m))
                nucleo_anima_l1_set_mode(!strcmp(m->valuestring, "on") ? 1 : !strcmp(m->valuestring, "off") ? 2 : 0);
            cJSON *b = cJSON_GetObjectItem(in, "browserLLM");
            if (cJSON_IsBool(b)) nucleo_anima_l1_set_external_brain(cJSON_IsTrue(b));
            cJSON_Delete(in);
        }
    }
    nucleo_anima_unlock();
    int l1m = nucleo_anima_l1_get_mode();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "l1Mode", l1m == 1 ? "on" : l1m == 2 ? "off" : "auto");
    cJSON_AddBoolToObject(root, "l1Serving", nucleo_anima_l1_serving());
    char *out = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    return ESP_OK;
}

// GET/POST /api/tts — voce offline on-device: interruttore "parla" + VELOCITA' di lettura.
// GET  -> {"enabled":bool,"available":bool,"speed":int%,"speed_min/max/step":int}.
// POST body {"enabled":bool}? imposta l'interruttore; {"speed":int%}? imposta la velocita' (clampata,
// persistita, invalida la cache); {"say":"testo","lang":"it|en"}? fa parlare subito il device (test/azione
// web — usa la velocita' gia' impostata in questo POST). Same-origin device UI (come /api/anima/l1):
// preferenza di dispositivo, niente di pagato/distruttivo -> no auth.
static esp_err_t tts_handler(httpd_req_t *req)
{
    if (req->method == HTTP_POST) {
        int blen = req->content_len;
        if (blen > 0 && blen < 1024) {
            char buf[1024]; int got = 0, r;
            while (got < blen) { r = httpd_req_recv(req, buf + got, blen - got); if (r <= 0) break; got += r; }
            buf[got > 0 ? got : 0] = 0;
            cJSON *in = cJSON_Parse(buf);
            if (in) {
                cJSON *e = cJSON_GetObjectItem(in, "enabled");
                if (cJSON_IsBool(e)) nucleo_tts_set_enabled(cJSON_IsTrue(e));
                cJSON *sp = cJSON_GetObjectItem(in, "speed");
                if (cJSON_IsNumber(sp)) nucleo_tts_set_speed(sp->valueint);   // PRIMA del say -> il test usa il nuovo passo
                cJSON *say = cJSON_GetObjectItem(in, "say");
                if (cJSON_IsString(say) && say->valuestring[0]) {
                    cJSON *l = cJSON_GetObjectItem(in, "lang");
                    nucleo_tts_say(say->valuestring, cJSON_IsString(l) ? l->valuestring : NULL);
                }
                cJSON_Delete(in);
            }
        }
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", nucleo_tts_enabled());
    cJSON_AddBoolToObject(root, "available", nucleo_tts_available());
    cJSON_AddNumberToObject(root, "speed", nucleo_tts_speed());
    cJSON_AddNumberToObject(root, "speed_min", TTS_SPEED_MIN);
    cJSON_AddNumberToObject(root, "speed_max", TTS_SPEED_MAX);
    cJSON_AddNumberToObject(root, "speed_step", TTS_SPEED_STEP);
    char *out = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    return ESP_OK;
}

static esp_err_t llm_proxy(httpd_req_t *req)
{
    char query[1400], url[1100] = { 0 };
    if (httpd_req_get_url_query_len(req) == 0 ||
        httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "url", url, sizeof url) != ESP_OK || !url[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
        return ESP_FAIL;
    }
    url_decode(url);
    if (strncmp(url, "https://", 8) != 0) {   // LLM APIs are TLS-only; refuse plaintext
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "https only");
        return ESP_FAIL;
    }
    if (url_target_blocked(url)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "target host not allowed");
        return ESP_FAIL;
    }

    char auth[300] = { 0 };
    httpd_req_get_hdr_value_str(req, "Authorization", auth, sizeof auth);   // empty if absent
    // Anthropic (Claude) is NOT OpenAI-compatible: it authenticates with x-api-key + anthropic-version
    // instead of a Bearer token. Forward those (and the browser-direct opt-in) so the same proxy can
    // reach api.anthropic.com when a client routes a Claude call through here instead of calling it direct.
    char xkey[300] = { 0 }, aver[40] = { 0 }, adirect[8] = { 0 };
    httpd_req_get_hdr_value_str(req, "x-api-key", xkey, sizeof xkey);
    httpd_req_get_hdr_value_str(req, "anthropic-version", aver, sizeof aver);
    httpd_req_get_hdr_value_str(req, "anthropic-dangerous-direct-browser-access", adirect, sizeof adirect);

    // The request body (chat JSON) is STREAMED straight through to the upstream — never buffered whole — so a
    // large context costs NO heap (the old 32 KB malloc cap is gone). RAM stays flat regardless of context
    // size: only a ~1 KB scratch buffer moves at a time (mirrors nucleo_anima_transcribe's audio upload). The
    // 1 MB bound is a SANITY cap on a pathological upload that would hog the single httpd task — NOT a RAM limit.
    int blen = req->content_len;
    if (blen > (1 << 20)) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large"); return ESP_FAIL; }
    bool stream_req = (req->method == HTTP_POST && blen > 0);

    // Same TLS heap discipline as /api/proxy: reclaim the L1 index (only if no cascade is mid-query),
    // then refuse gracefully (never OOM) if the heap is still too tight for the handshake itself.
    if (!nucleo_tls_heap_ok()) nucleo_anima_l1_unload_if_idle();
    if (!nucleo_tls_heap_ok()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "low memory for TLS fetch, retry");
        return ESP_FAIL;
    }

    proxy_ctx_t ctx = { .req = req, .started = false, .ctype = { 0 } };
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,                           // models can take a while; be generous
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .method = (req->method == HTTP_POST) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
    };
    if (!stream_req) { cfg.event_handler = proxy_evt; cfg.user_data = &ctx; }   // GET/empty POST: response streamed by proxy_evt

    // Heavy-work budget across the TLS window (see /api/proxy). try-only: never block the httpd task.
    uint32_t tk = nucleo_arb_acquire(ARB_FG, "llm", 0);
    if (!tk) { httpd_resp_set_status(req, "503 Service Unavailable");
               httpd_resp_sendstr(req, "busy with another TLS fetch, retry"); return ESP_FAIL; }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { nucleo_arb_release(tk); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "client init"); return ESP_FAIL; }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (auth[0])    esp_http_client_set_header(client, "Authorization", auth);
    if (xkey[0])    esp_http_client_set_header(client, "x-api-key", xkey);
    if (aver[0])    esp_http_client_set_header(client, "anthropic-version", aver);
    if (adirect[0]) esp_http_client_set_header(client, "anthropic-dangerous-direct-browser-access", adirect);

    if (!stream_req) {
        // GET or bodyless POST: one-shot perform(); proxy_evt streams the response back chunk by chunk.
        esp_err_t err = esp_http_client_perform(client);
        esp_http_client_cleanup(client);
        nucleo_arb_release(tk);
        if (!ctx.started) {
            if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err)); return ESP_FAIL; }
            httpd_resp_set_type(req, "application/json");
        }
        httpd_resp_send_chunk(req, NULL, 0);
        return err == ESP_OK ? ESP_OK : ESP_FAIL;
    }

    // POST with a body: STREAM the request through (open + write loop reading from the browser), then STREAM
    // the response back (read loop + send_chunk). The body never resides in RAM whole, so the usable context
    // is bounded by latency, not heap. Same shape as nucleo_anima_transcribe's chunked upload (proven).
    esp_err_t err = esp_http_client_open(client, blen);   // sends request headers incl. Content-Length: blen
    if (err != ESP_OK) { esp_http_client_cleanup(client); nucleo_arb_release(tk); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err)); return ESP_FAIL; }
    {
        char buf[1024]; int remaining = blen, fail = 0;
        while (remaining > 0) {
            int want = remaining < (int)sizeof buf ? remaining : (int)sizeof buf;
            int rd = httpd_req_recv(req, buf, want);
            if (rd <= 0) { fail = 1; break; }                              // browser disconnected mid-upload
            if (esp_http_client_write(client, buf, rd) < 0) { fail = 1; break; }
            remaining -= rd;
        }
        if (fail) { esp_http_client_close(client); esp_http_client_cleanup(client); nucleo_arb_release(tk);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "relay write failed"); return ESP_FAIL; }
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    char *ct = NULL; if (esp_http_client_get_header(client, "Content-Type", &ct) != ESP_OK) ct = NULL;
    char ctype[96]; snprintf(ctype, sizeof ctype, "%s", (ct && ct[0]) ? ct : "application/json");
    httpd_resp_set_type(req, ctype);
    char sline[40] = "200 OK";
    if (status != 200) {                                  // propagate the REAL code: the web UIs + the calibration tier-probe read r.status
        snprintf(sline, sizeof sline, "%d %s", status, status >= 500 ? "Upstream Error" : "Upstream");
        httpd_resp_set_status(req, sline);
    }
    {
        char rbuf[1024]; int n;
        while ((n = esp_http_client_read(client, rbuf, sizeof rbuf)) > 0) {
            if (httpd_resp_send_chunk(req, rbuf, n) != ESP_OK) break;       // browser gone -> stop relaying
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    nucleo_arb_release(tk);                               // TLS torn down -> free the budget
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// GET /api/transcribe?path=<sd audio>&lang=auto|it|en&summarize=1
// The OS-wide transcription service: streams the recording to cloud Whisper (auto language
// detect, or forced when lang != auto — the OS-language fallback), then summarizes with the
// cloud teacher. Writes <base>.txt + <base>.sum.txt next to the audio and returns the JSON.
// The native Recorder app calls the same nucleo_anima_transcribe/summarize C functions, so the
// transcript/summary sidecars are identical whether triggered from the web or the Cardputer.
static esp_err_t transcribe_get(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);                 // triggers paid cloud calls -> require a paired session
    char query[400], rawp[220] = {0}, lang[16] = "auto", sums[4] = "1";
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        httpd_query_key_value(query, "path", rawp, sizeof rawp);
        httpd_query_key_value(query, "lang", lang, sizeof lang);
        httpd_query_key_value(query, "summarize", sums, sizeof sums);
    }
    url_decode(rawp);
    size_t L = strlen(rawp);
    bool okpath = strncmp(rawp, "/data/Recordings/", 17) == 0 && !strstr(rawp, "..") &&
                  L > 4 && (!strcasecmp(rawp + L - 4, ".mp3") || !strcasecmp(rawp + L - 4, ".wav"));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (!okpath) { httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad path\"}"); return ESP_OK; }
    if (!lang[0]) strcpy(lang, "auto");
    bool dosum = sums[0] != '0';

    char abs[256];     snprintf(abs, sizeof abs, NUCLEO_SD_MOUNT "%s", rawp);
    char absbase[256]; snprintf(absbase, sizeof absbase, "%.*s", (int)(strlen(abs) - 4), abs);  // strip 4-char ext

    char *text = malloc(4096); char detlang[16] = {0};
    if (!text) { httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}"); return ESP_OK; }
    int tl = nucleo_anima_transcribe(abs, lang, text, 4096, detlang, sizeof detlang);

    cJSON *root = cJSON_CreateObject();
    if (tl < 0) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "transcription unavailable (needs online Whisper key)");
    } else {
        char p[270];
        snprintf(p, sizeof p, "%s.txt", absbase);
        FILE *f = fopen(p, "w"); if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }
        const char *uselang = detlang[0] ? detlang : (strcmp(lang, "auto") ? lang : "it");
        char *summary = NULL;
        if (dosum && (summary = malloc(2048))) {
            int sl = nucleo_anima_summarize(text, uselang, summary, 2048);
            if (sl > 0) { snprintf(p, sizeof p, "%s.sum.txt", absbase); f = fopen(p, "w"); if (f) { fwrite(summary, 1, strlen(summary), f); fclose(f); } }
            else summary[0] = 0;
        }
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "text", text);
        cJSON_AddStringToObject(root, "language", uselang);
        cJSON_AddStringToObject(root, "summary", summary ? summary : "");
        free(summary);
        remove(abs); // ELIMINA IL FILE ORIGINALE (.wav o .mp3) DOPO LA CONVERSIONE
    }
    free(text);
    char *out = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    httpd_resp_sendstr(req, out ? out : "{\"ok\":false}");
    free(out);
    return ESP_OK;
}

// POST /api/voice/learn -> arm the voice engine for learning mode. Body: {"word":"apri"}
static esp_err_t voice_learn_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    int blen = req->content_len;
    if (blen <= 0 || blen > 256) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }
    char body[257] = {0};
    int got = 0, r;
    while (got < blen) {
        r = httpd_req_recv(req, body + got, blen - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        got += r;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *cw = cJSON_GetObjectItem(root, "word");
    if (!cJSON_IsString(cw) || !cw->valuestring[0]) {
        cJSON_Delete(root); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing word"); return ESP_FAIL;
    }
    esp_err_t err = nucleo_voice_arm_learning_mode(cw->valuestring);
    cJSON_Delete(root);
    
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) httpd_resp_sendstr(req, "{\"ok\":true}");
    else               httpd_resp_sendstr(req, "{\"ok\":false}");
    return ESP_OK;
}

// POST /api/voice/always -> "voice always-on" toggle. Body: {"on":true|false}.
// Live-applies (sticky pin → engine up/down now) AND persists voice.alwaysOn to
// settings.json, so it survives reboot without the caller having to save the file.
static esp_err_t voice_always_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    int blen = req->content_len;
    if (blen <= 0 || blen > 64) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body"); return ESP_FAIL; }
    char body[65] = {0};
    int got = 0, r;
    while (got < blen) {
        r = httpd_req_recv(req, body + got, blen - got);
        if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv"); return ESP_FAIL; }
        got += r;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    bool on = cJSON_IsTrue(cJSON_GetObjectItem(root, "on"));
    cJSON_Delete(root);
    nucleo_voice_set_always_on(on);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, on ? "{\"ok\":true,\"on\":true}" : "{\"ok\":true,\"on\":false}");
    return ESP_OK;
}

// Socket close hook: notify the WS layer so a dropped browser is detected the instant its
// socket closes (drives the launcher's "remote handoff" reclaim). Overriding close_fn means
// we are responsible for actually closing the fd.
static void on_sock_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    nucleo_ws_notify_close(sockfd);
    close(sockfd);
}

// POST /api/time/set {"ts":1749500000}
// Browser pushes its clock to the device when NTP hasn't synced yet (captive portal, no internet).
// No auth required: wrong time is low-risk and NTP will override it as soon as internet is available.
static esp_err_t time_set_post(httpd_req_t *req)
{
    char body[48];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[n] = '\0';
    cJSON *j = cJSON_Parse(body);
    if (!j) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *ts = cJSON_GetObjectItemCaseSensitive(j, "ts");
    time_t t = cJSON_IsNumber(ts) ? (time_t)(long long)cJSON_GetNumberValue(ts) : 0;
    cJSON_Delete(j);
    if (t <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ts"); return ESP_FAIL; }
    nucleo_setup_set_time(t);
    char buf[48];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"time\":%lu}", (unsigned long)time(NULL));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

esp_err_t nucleo_httpd_start(void)
{
    if (s_server) return ESP_OK;                     // already running (idempotent)
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 6;                     // 4->6. (Tried 8 to dodge lru-purge of the persistent /ws, but a realistic-load stress then drove min_free internal heap to 156 B — near-OOM — because each extra concurrent keep-alive socket holds httpd scratch buffers that fragment the scarce heap. 6 keeps min_free ~10-13 KB and still gives 0 WS flaps under a single real browser; the rare flap under heavy parallel load is the lesser evil vs near-OOM on this PSRAM-less, battery-powered chip.)
    config.uri_match_fn = httpd_uri_match_wildcard;  // enable the "/*" static catch-all
    config.max_uri_handlers = 48;   // headroom for the pairing routes + /api/anima + /api/reboot + /api/heap + /api/proxy + /api/llm (GET+POST) + /api/transcribe
    config.close_fn = on_sock_close;                 // detect client disconnects immediately
    // The shell opens many parallel connections (assets + several /api/fs + /ws). lru_purge
    // recycles the oldest idle socket instead of refusing/resetting new ones — this (plus the
    // throttled /api/status FAT scan) stops the ERR_CONNECTION_RESET seen in the browser.
    config.lru_purge_enable = true;
    config.stack_size = 30720;                       // 16K->24K->30K: the offline L1 cascade (l1_encode's acc[1KB]+w atop anima_query->try_cascade->l1_query) overflowed 16K -> panic on every knowledge query; 24K fixed flat-L1, 30K covers the deeper AKB5 re-entrant (akb5_query -> re-entrant l1_query per shard). HWM in /api/heap. ANIMA runs the whole L0/L1 cascade IN this task,
                                                     // then a TLS handshake whose X.509 chain verify is
                                                     // deeply recursive — Wikipedia's long cert chain
                                                     // overflowed the old 8 KB stack (silent ~60s hang);
                                                     // groq/open-meteo's shorter chains fit. 16 KB clears it.
    // Large drag-and-drop uploads (up to 640 MB) can pause mid-stream while the SD card
    // flushes a block. A 5 s socket timeout would abort them; 30 s gives generous slack,
    // and the upload handler additionally forgives a bounded run of these timeouts.
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    // TCP keep-alive on every accepted socket. A client that vanishes without a FIN (Wi-Fi drops,
    // laptop sleeps, tab killed mid-flight) would otherwise pin its socket for a full 30 s recv
    // window each cycle. Probing reaps it in ~idle+interval*count ≈ 20 s, returning the socket to
    // a pool that is only LWIP_MAX_SOCKETS deep — this keeps connections available without raising
    // the socket cap (no extra RAM), and complements the on_sock_close hook + lru_purge fallback.
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err)); return err; }

    // Register specific API routes BEFORE the static catch-all so they win.
    httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_get };
    httpd_uri_t apps = { .uri = "/api/apps", .method = HTTP_GET, .handler = apps_get };
    httpd_uri_t assoc = { .uri = "/api/associations", .method = HTTP_GET, .handler = assoc_get };
    httpd_uri_t logs = { .uri = "/api/logs", .method = HTTP_GET, .handler = logs_get };
    httpd_uri_t diag = { .uri = "/api/diag", .method = HTTP_GET, .handler = diag_get };   // consolidated health snapshot
    httpd_uri_t heap = { .uri = "/api/heap", .method = HTTP_GET, .handler = heap_get };
    httpd_uri_t cpu = { .uri = "/api/cpu", .method = HTTP_GET, .handler = cpu_get };
    httpd_uri_t wifiscan = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get };
    httpd_uri_t wifiknown  = { .uri = "/api/wifi/known",  .method = HTTP_GET,  .handler = wifi_known_get };   // saved networks
    httpd_uri_t wifijoin   = { .uri = "/api/wifi/join",   .method = HTTP_POST, .handler = wifi_join_post };   // join + remember
    httpd_uri_t wififorget = { .uri = "/api/wifi/forget", .method = HTTP_POST, .handler = wifi_forget_post }; // forget one/all
    httpd_uri_t ota = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_uri_t reboot = { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post };
    httpd_uri_t anima = { .uri = "/api/anima", .method = HTTP_GET, .handler = anima_get };
    httpd_uri_t anima_verify = { .uri = "/api/anima/verify", .method = HTTP_GET, .handler = anima_verify_get };
    httpd_uri_t anima_caps = { .uri = "/api/anima/caps", .method = HTTP_GET, .handler = anima_caps_get };  // teacher provider/model (no key)
    httpd_uri_t anima_l1 = { .uri = "/api/anima/l1", .method = HTTP_POST, .handler = anima_l1_post };      // offline L1 brain policy
    httpd_uri_t tts_get  = { .uri = "/api/tts", .method = HTTP_GET,  .handler = tts_handler };             // voce on-device: stato
    httpd_uri_t tts_post = { .uri = "/api/tts", .method = HTTP_POST, .handler = tts_handler };             // voce on-device: set/test
    httpd_uri_t proxy = { .uri = "/api/proxy", .method = HTTP_GET, .handler = proxy_get };
    httpd_uri_t llm_get  = { .uri = "/api/llm", .method = HTTP_GET,  .handler = llm_proxy };  // model list
    httpd_uri_t llm_post = { .uri = "/api/llm", .method = HTTP_POST, .handler = llm_proxy };  // chat completions
    httpd_uri_t transcribe = { .uri = "/api/transcribe", .method = HTTP_GET, .handler = transcribe_get };  // Whisper STT + summary
    httpd_uri_t voice_learn = { .uri = "/api/voice/learn", .method = HTTP_POST, .handler = voice_learn_post };
    httpd_uri_t voice_always = { .uri = "/api/voice/always", .method = HTTP_POST, .handler = voice_always_post };
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &apps);
    httpd_register_uri_handler(server, &assoc);
    httpd_register_uri_handler(server, &logs);
    httpd_register_uri_handler(server, &diag);       // /api/diag (Log Viewer "Diagnose" digest)
    httpd_register_uri_handler(server, &heap);
    httpd_register_uri_handler(server, &cpu);
    httpd_register_uri_handler(server, &wifiscan);   // /api/wifi/scan (web WiFi Scanner app)
    httpd_register_uri_handler(server, &wifiknown);  // /api/wifi/known  (saved networks)
    httpd_register_uri_handler(server, &wifijoin);   // /api/wifi/join   (join + remember)
    httpd_register_uri_handler(server, &wififorget); // /api/wifi/forget (forget one/all)
    httpd_register_uri_handler(server, &ota);
    httpd_register_uri_handler(server, &reboot);
    httpd_register_uri_handler(server, &anima);
    httpd_register_uri_handler(server, &anima_verify);   // ANIMA Forge cross-substrate grounded verify
    httpd_register_uri_handler(server, &anima_caps);     // /api/anima/caps (teacher provider/model, no key)
    httpd_register_uri_handler(server, &anima_l1);        // /api/anima/l1 (offline L1 brain policy)
    httpd_register_uri_handler(server, &tts_get);         // /api/tts (voce on-device)
    httpd_register_uri_handler(server, &tts_post);
    httpd_register_uri_handler(server, &proxy);
    httpd_register_uri_handler(server, &llm_get);
    httpd_register_uri_handler(server, &llm_post);
    httpd_register_uri_handler(server, &transcribe);   // /api/transcribe (Whisper STT + summary)
    httpd_register_uri_handler(server, &voice_learn);
    httpd_register_uri_handler(server, &voice_always);   // /api/voice/always (persistent always-on toggle)
    httpd_uri_t time_set = { .uri = "/api/time/set", .method = HTTP_POST, .handler = time_set_post };
    httpd_register_uri_handler(server, &time_set);        // /api/time/set (browser → device clock push)
    nucleo_auth_register(server);      // /api/pair, /api/auth/status (public; gate the rest)
    nucleo_fsapi_register(server);     // /api/fs/*
    nucleo_recorder_register(server);  // /api/rec/* (mic -> WAV on SD)
    nucleo_ir_register(server);        // /api/ir/* (send, tvbgone, db) — before webfs catch-all
    nucleo_gpio_register(server);      // /api/gpio (raw pin read/write, safe-pin allowlist)
    // /api/link/* (Vicino ESP-NOW transfer engine). Extern decl + final-link resolve (like the display
    // endpoint below): nucleo_link is pulled in by nucleo_app, so no REQUIRES/cycle is needed here.
    extern esp_err_t nucleo_link_api_register(httpd_handle_t);
    nucleo_link_api_register(server);
    // /api/display (off/on the panel) lives in nucleo_app, which owns the brightness state. We
    // only forward-declare it: a REQUIRES on nucleo_app would cycle; the symbol resolves at final
    // link (main pulls nucleo_app in). Same arrangement nucleo_app uses for nucleo_anima/auth.
    extern esp_err_t nucleo_app_register_display(httpd_handle_t);
    if (nucleo_app_register_display(server) != ESP_OK) ESP_LOGW(TAG, "display endpoint register failed");
    nucleo_ws_register(server);        // /ws live deltas
    nucleo_webfs_register(server);  // serves shell + app UIs from SD (catch-all, last)
    cpu_probe_init();   // start the per-core idle-hook load probe (once per boot)
    s_server = server;
    ESP_LOGI(TAG, "HTTP server up — free=%u min=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    return ESP_OK;
}

// Stop the OS web server and release port 80. Idempotent. After this the device serves no
// web UI until nucleo_httpd_start() is called again. Used by the Evil Portal so its own
// captive server can own port 80; the OS UI is intentionally dark while the portal runs.
esp_err_t nucleo_httpd_stop(void)
{
    if (!s_server) return ESP_OK;
    // A live dictation stream worker holds an async copy of its request: stopping httpd under it is
    // a use-after-free in the worker. Wind it down first (bounded wait inside).
    nucleo_recorder_stream_abort();
    esp_err_t err = httpd_stop(s_server);            // frees all registered handlers + the task
    s_server = NULL;
    ESP_LOGI(TAG, "HTTP server stopped (port 80 released)");
    return err;
}

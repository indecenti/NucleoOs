// nucleo_ir — RMT-driven IR transmit + HTTP surface. See nucleo_ir.h for the design rationale.
#include "nucleo_ir.h"
#include "nucleo_auth.h"

#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp

static const char *TAG = "ir";

// Cardputer IR LED. Overridable for other boards via -DNUCLEO_IR_GPIO=<n>.
#ifndef NUCLEO_IR_GPIO
#define NUCLEO_IR_GPIO 44
#endif
#define IR_RESOLUTION_HZ 1000000   // 1 tick = 1 µs (IR timings are specified in µs)
#define RMT_MAX_TICKS    32767      // 15-bit duration field per RMT symbol half

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static SemaphoreHandle_t    s_lock;
static uint16_t             s_cur_carrier;
static bool                 s_ready;

// ---- TV-B-Gone sweep state ------------------------------------------------------------------
static TaskHandle_t       s_sweep_task;
static volatile bool      s_sweep_abort;
static volatile bool      s_sweep_running;
static volatile int       s_sweep_sent;
static volatile int       s_sweep_total;
static char               s_sweep_region[12];

// ---- IR jammer state (declared up here so nucleo_ir_busy can see it) -------------------------
static TaskHandle_t  s_jam_task;
static volatile bool s_jam_run, s_jam_abort;
static char          s_jam_mode[12];

// ---- carrier (re-applied only when the protocol's frequency actually changes) ---------------
static void ir_set_carrier(uint16_t hz) {
    if (hz == s_cur_carrier) return;
    rmt_disable(s_chan);
    rmt_carrier_config_t c = { .frequency_hz = hz, .duty_cycle = 0.33f };
    rmt_apply_carrier(s_chan, &c);
    rmt_enable(s_chan);
    s_cur_carrier = hz;
}

// ---- pack an alternating mark/space µs list into RMT symbols and blast it `repeats` times ----
static esp_err_t ir_blast(const uint16_t *dur, int n, uint16_t carrier, int repeats) {
    if (!s_ready || !dur || n <= 0) return ESP_ERR_INVALID_STATE;
    if (repeats <= 0) repeats = 1;
    int nsym = (n + 1) / 2;
    rmt_symbol_word_t *sym = calloc(nsym, sizeof(rmt_symbol_word_t));
    if (!sym) return ESP_ERR_NO_MEM;
    for (int i = 0, k = 0; i < n; i += 2, k++) {
        uint16_t mark = dur[i] > RMT_MAX_TICKS ? RMT_MAX_TICKS : dur[i];
        sym[k].level0 = 1; sym[k].duration0 = mark;
        if (i + 1 < n) {
            uint16_t space = dur[i + 1] > RMT_MAX_TICKS ? RMT_MAX_TICKS : dur[i + 1];
            sym[k].level1 = 0; sym[k].duration1 = space;
        } else {
            sym[k].level1 = 0; sym[k].duration1 = 1000;   // pad a final lone mark with brief idle
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Enable the channel only for the transmit window. Left enabled 24/7 it holds an APB pm-lock that
    // pins the bus clock (and the driver heap) for an IR LED used a few times a session. ir_set_carrier
    // early-returns when the frequency is unchanged, so the explicit enable here is required (it can't
    // be left to the carrier path). Disabled again right after → pm-lock released between blasts.
    rmt_enable(s_chan);
    ir_set_carrier(carrier);
    rmt_transmit_config_t tx = { .loop_count = 0 };
    esp_err_t err = ESP_OK;
    for (int r = 0; r < repeats && err == ESP_OK; r++) {
        err = rmt_transmit(s_chan, s_enc, sym, (size_t)nsym * sizeof(rmt_symbol_word_t), &tx);
        if (err == ESP_OK) err = rmt_tx_wait_all_done(s_chan, 2000);
        if (r + 1 < repeats) vTaskDelay(pdMS_TO_TICKS(40));   // inter-frame gap
    }
    rmt_disable(s_chan);
    xSemaphoreGive(s_lock);
    free(sym);
    return err;
}

// ---- public send API ------------------------------------------------------------------------
esp_err_t nucleo_ir_send_proto(nir_proto_t proto, uint32_t address, uint32_t command, int repeats) {
    uint16_t dur[160], carrier = 38000;
    int n = nir_encode(proto, address, command, dur, (int)(sizeof dur / sizeof dur[0]), &carrier);
    if (n <= 0) return ESP_ERR_INVALID_ARG;       // <0 error, 0 = RAW (use send_raw)
    if (repeats <= 0) repeats = nir_proto_default_repeats(proto);
    return ir_blast(dur, n, carrier, repeats);
}

esp_err_t nucleo_ir_send_raw(const uint16_t *durations, int count, uint16_t carrier_hz, int repeats) {
    if (carrier_hz == 0) carrier_hz = 38000;
    return ir_blast(durations, count, carrier_hz, repeats);
}

bool nucleo_ir_busy(void) {
    if (s_sweep_running || s_jam_run) return true;
    if (!s_lock) return false;
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) { xSemaphoreGive(s_lock); return false; }
    return true;
}

// ---- built-in TV power code table ------------------------------------------------------------
// A clean-room starter set of common TV power codes, expressed as public protocol facts
// (brand/protocol/address/command). It is deliberately small: the web app (apps/ir-remote) holds
// the large, user-extensible device database and can import community .ir files — no reflash.
typedef struct { const char *region; const char *brand; nir_proto_t proto; uint16_t addr; uint16_t cmd; } tvcode_t;
static const tvcode_t TVDB[] = {
    { "asia", "Samsung",   NIR_PROTO_SAMSUNG, 0x07, 0x02 },
    { "asia", "Sony",      NIR_PROTO_SONY12,  0x01, 0x15 },
    { "asia", "LG",        NIR_PROTO_NEC,     0x04, 0x08 },
    { "asia", "Toshiba",   NIR_PROTO_NEC,     0x40, 0x12 },
    { "asia", "Hisense",   NIR_PROTO_NEC,     0x00, 0x57 },
    { "asia", "Sharp",     NIR_PROTO_NEC,     0x02, 0x5E },
    { "asia", "JVC",       NIR_PROTO_JVC,     0x03, 0x17 },
    { "eu",   "Philips",   NIR_PROTO_RC5,     0x00, 0x0C },
    { "eu",   "Grundig",   NIR_PROTO_RC5,     0x00, 0x0C },
    { "eu",   "Loewe",     NIR_PROTO_RC5,     0x00, 0x0C },
    { "eu",   "Vestel",    NIR_PROTO_NEC,     0x00, 0x0C },
    { "us",   "Vizio",     NIR_PROTO_NEC,     0x00, 0x08 },
    { "us",   "TCL",       NIR_PROTO_NEC,     0x04, 0x08 },
    { "us",   "Insignia",  NIR_PROTO_NEC,     0x00, 0x57 },
    { "us",   "RCA",       NIR_PROTO_NEC,     0x00, 0x3A },
    { "us",   "Element",   NIR_PROTO_NEC,     0x00, 0x08 },
};
#define TVDB_N ((int)(sizeof(TVDB) / sizeof(TVDB[0])))

static bool region_match(const char *filter, const char *entry) {
    if (!filter || !filter[0] || strcasecmp(filter, "all") == 0) return true;
    return strcasecmp(filter, entry) == 0;
}

static void sweep_task(void *arg) {
    (void)arg;
    for (int i = 0; i < TVDB_N && !s_sweep_abort; i++) {
        if (!region_match(s_sweep_region, TVDB[i].region)) continue;
        nucleo_ir_send_proto(TVDB[i].proto, TVDB[i].addr, TVDB[i].cmd, 0);
        s_sweep_sent++;
        vTaskDelay(pdMS_TO_TICKS(120));   // pace so each TV has time to react
    }
    s_sweep_running = false;
    s_sweep_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t nucleo_ir_tvbgone_start(const char *region) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_sweep_running) return ESP_ERR_INVALID_STATE;
    snprintf(s_sweep_region, sizeof s_sweep_region, "%s", region ? region : "all");
    int total = 0;
    for (int i = 0; i < TVDB_N; i++) if (region_match(s_sweep_region, TVDB[i].region)) total++;
    s_sweep_total = total; s_sweep_sent = 0; s_sweep_abort = false; s_sweep_running = true;
    if (xTaskCreate(sweep_task, "ir_sweep", 4096, NULL, 5, &s_sweep_task) != pdPASS) {
        s_sweep_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void nucleo_ir_tvbgone_stop(void) { s_sweep_abort = true; }

bool nucleo_ir_tvbgone_status(int *sent, int *total) {
    if (sent)  *sent  = s_sweep_sent;
    if (total) *total = s_sweep_total;
    return s_sweep_running;
}

// ---- IR jammer ------------------------------------------------------------------------------
// Floods the band with long carrier marks + tiny gaps so a receiver's AGC never recovers. Each
// burst goes through ir_blast (so it serialises on the same mutex as normal sends — never races).
static void jam_task(void *arg) {
    (void)arg;
    uint16_t noise[40];
    uint32_t seed = 0x1234567u;
    int sweep = 100;
    while (!s_jam_abort) {
        if (strcasecmp(s_jam_mode, "constant") == 0) {
            for (int i = 0; i < 40; i += 2) { noise[i] = 260; noise[i + 1] = 12; }
        } else if (strcasecmp(s_jam_mode, "random") == 0) {
            for (int i = 0; i < 40; i += 2) {
                seed = seed * 1103515245u + 12345u; noise[i]     = 120 + (seed >> 16) % 320;
                seed = seed * 1103515245u + 12345u; noise[i + 1] = 8 + (seed >> 16) % 30;
            }
        } else { // sweep
            for (int i = 0; i < 40; i += 2) { noise[i] = sweep; noise[i + 1] = 12; sweep += 16; if (sweep > 420) sweep = 100; }
        }
        ir_blast(noise, 40, 38000, 6);
        vTaskDelay(1);   // yield so the idle/watchdog tasks run between bursts
    }
    s_jam_run = false; s_jam_task = NULL; vTaskDelete(NULL);
}

esp_err_t nucleo_ir_jammer_start(const char *mode) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (s_jam_run || s_sweep_running) return ESP_ERR_INVALID_STATE;
    snprintf(s_jam_mode, sizeof s_jam_mode, "%s", mode && mode[0] ? mode : "sweep");
    s_jam_abort = false; s_jam_run = true;
    if (xTaskCreate(jam_task, "ir_jam", 4096, NULL, 5, &s_jam_task) != pdPASS) { s_jam_run = false; return ESP_FAIL; }
    return ESP_OK;
}
void nucleo_ir_jammer_stop(void) { s_jam_abort = true; }
bool nucleo_ir_jammer_running(void) { return s_jam_run; }   // staged API: getter of the live start/stop pair, no caller yet

// ---- HTTP handlers --------------------------------------------------------------------------
// Same-origin device endpoints (called by the device's own ir-remote app): no CORS header, and
// the control verbs are auth-gated exactly like /api/ota and /api/rec/*.
static char *read_body(httpd_req_t *req, int max) {
    int len = req->content_len;
    if (len <= 0 || len > max) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0, r;
    while (got < len) { r = httpd_req_recv(req, buf + got, len - got); if (r <= 0) { free(buf); return NULL; } got += r; }
    buf[got] = 0;
    return buf;
}

static esp_err_t json_reply(httpd_req_t *req, bool ok, const char *body) {
    httpd_resp_set_type(req, "application/json");
    if (!ok) httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
}

static esp_err_t send_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    char *body = read_body(req, 8192);
    if (!body) return json_reply(req, false, "{\"ok\":false,\"err\":\"body\"}");
    cJSON *in = cJSON_Parse(body);
    free(body);
    if (!in) return json_reply(req, false, "{\"ok\":false,\"err\":\"json\"}");

    esp_err_t result = ESP_ERR_INVALID_ARG;
    int repeats = 0;
    cJSON *rp = cJSON_GetObjectItem(in, "repeats");
    if (cJSON_IsNumber(rp)) repeats = rp->valueint;

    cJSON *raw = cJSON_GetObjectItem(in, "raw");
    if (cJSON_IsArray(raw)) {
        int n = cJSON_GetArraySize(raw);
        if (n > 0 && n <= NIR_MAX_DURATIONS) {
            uint16_t *dur = malloc((size_t)n * sizeof(uint16_t));
            if (dur) {
                for (int i = 0; i < n; i++) {
                    cJSON *e = cJSON_GetArrayItem(raw, i);
                    int v = cJSON_IsNumber(e) ? e->valueint : 0;
                    dur[i] = (uint16_t)(v < 0 ? 0 : (v > 0xFFFF ? 0xFFFF : v));
                }
                uint16_t carrier = 38000;
                cJSON *c = cJSON_GetObjectItem(in, "carrier");
                if (cJSON_IsNumber(c)) carrier = (uint16_t)c->valueint;
                result = nucleo_ir_send_raw(dur, n, carrier, repeats);
                free(dur);
            } else result = ESP_ERR_NO_MEM;
        }
    } else {
        cJSON *p  = cJSON_GetObjectItem(in, "protocol");
        cJSON *a  = cJSON_GetObjectItem(in, "address");
        cJSON *cm = cJSON_GetObjectItem(in, "command");
        if (cJSON_IsString(p) && cJSON_IsNumber(cm)) {
            nir_proto_t proto = nir_proto_from_name(p->valuestring);
            uint32_t addr = cJSON_IsNumber(a) ? (uint32_t)a->valuedouble : 0;
            uint32_t cmd  = (uint32_t)cm->valuedouble;
            if (proto < NIR_PROTO__COUNT && proto != NIR_PROTO_RAW)
                result = nucleo_ir_send_proto(proto, addr, cmd, repeats);
        }
    }
    cJSON_Delete(in);
    return json_reply(req, result == ESP_OK, result == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false}");
}

static esp_err_t tvbgone_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    char *body = read_body(req, 512);
    char action[12] = "start", region[12] = "all";
    if (body) {
        cJSON *in = cJSON_Parse(body);
        free(body);
        if (in) {
            cJSON *a = cJSON_GetObjectItem(in, "action");
            cJSON *r = cJSON_GetObjectItem(in, "region");
            if (cJSON_IsString(a)) snprintf(action, sizeof action, "%s", a->valuestring);
            if (cJSON_IsString(r)) snprintf(region, sizeof region, "%s", r->valuestring);
            cJSON_Delete(in);
        }
    }
    if (strcasecmp(action, "stop") == 0) {
        nucleo_ir_tvbgone_stop();
        return json_reply(req, true, "{\"ok\":true,\"running\":false}");
    }
    esp_err_t err = nucleo_ir_tvbgone_start(region);
    if (err != ESP_OK) return json_reply(req, false, "{\"ok\":false,\"err\":\"busy\"}");
    return json_reply(req, true, "{\"ok\":true,\"running\":true}");
}

static esp_err_t tvbgone_get(httpd_req_t *req) {
    int sent = 0, total = 0;
    bool running = nucleo_ir_tvbgone_status(&sent, &total);
    char out[96];
    snprintf(out, sizeof out, "{\"running\":%s,\"sent\":%d,\"total\":%d}",
             running ? "true" : "false", sent, total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t db_get(httpd_req_t *req) {
    // Static capability info for the UI: protocols we can encode, TV-B-Gone size, and the GPIO.
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ready", s_ready);
    cJSON_AddNumberToObject(root, "gpio", NUCLEO_IR_GPIO);
    cJSON *protos = cJSON_AddArrayToObject(root, "protocols");
    for (int i = 1; i < NIR_PROTO__COUNT; i++)   // skip RAW(0) in the human list
        cJSON_AddItemToArray(protos, cJSON_CreateString(nir_proto_name((nir_proto_t)i)));
    cJSON *tvb = cJSON_AddObjectToObject(root, "tvbgone");
    cJSON_AddNumberToObject(tvb, "count", TVDB_N);
    cJSON *regions = cJSON_AddArrayToObject(tvb, "regions");
    cJSON_AddItemToArray(regions, cJSON_CreateString("all"));
    cJSON_AddItemToArray(regions, cJSON_CreateString("us"));
    cJSON_AddItemToArray(regions, cJSON_CreateString("eu"));
    cJSON_AddItemToArray(regions, cJSON_CreateString("asia"));
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    return ESP_OK;
}

static esp_err_t jammer_post(httpd_req_t *req) {
    NUCLEO_AUTH_GUARD(req);
    char *body = read_body(req, 256);
    char action[12] = "start", mode[12] = "sweep";
    if (body) {
        cJSON *in = cJSON_Parse(body);
        free(body);
        if (in) {
            cJSON *a = cJSON_GetObjectItem(in, "action");
            cJSON *m = cJSON_GetObjectItem(in, "mode");
            if (cJSON_IsString(a)) snprintf(action, sizeof action, "%s", a->valuestring);
            if (cJSON_IsString(m)) snprintf(mode, sizeof mode, "%s", m->valuestring);
            cJSON_Delete(in);
        }
    }
    if (strcasecmp(action, "stop") == 0) { nucleo_ir_jammer_stop(); return json_reply(req, true, "{\"ok\":true,\"running\":false}"); }
    esp_err_t err = nucleo_ir_jammer_start(mode);
    return json_reply(req, err == ESP_OK, err == ESP_OK ? "{\"ok\":true,\"running\":true}" : "{\"ok\":false,\"err\":\"busy\"}");
}

esp_err_t nucleo_ir_register(httpd_handle_t server) {
    httpd_uri_t routes[] = {
        { .uri = "/api/ir/send",    .method = HTTP_POST, .handler = send_post },
        { .uri = "/api/ir/tvbgone", .method = HTTP_POST, .handler = tvbgone_post },
        { .uri = "/api/ir/tvbgone", .method = HTTP_GET,  .handler = tvbgone_get },
        { .uri = "/api/ir/db",      .method = HTTP_GET,  .handler = db_get },
        { .uri = "/api/ir/jammer",  .method = HTTP_POST, .handler = jammer_post },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++)
        httpd_register_uri_handler(server, &routes[i]);
    return ESP_OK;
}

// ---- init -----------------------------------------------------------------------------------
esp_err_t nucleo_ir_init(void) {
    if (s_ready) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    rmt_tx_channel_config_t txcfg = {
        .gpio_num       = NUCLEO_IR_GPIO,
        .clk_src        = RMT_CLK_SRC_DEFAULT,
        .resolution_hz  = IR_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&txcfg, &s_chan);
    if (err != ESP_OK) { ESP_LOGW(TAG, "rmt channel on GPIO%d failed: %s", NUCLEO_IR_GPIO, esp_err_to_name(err)); return err; }

    rmt_copy_encoder_config_t copy_cfg = {};
    err = rmt_new_copy_encoder(&copy_cfg, &s_enc);
    if (err != ESP_OK) { ESP_LOGW(TAG, "rmt encoder failed: %s", esp_err_to_name(err)); return err; }

    // Apply the default carrier on the freshly-created (disabled) channel, but DON'T enable it: the
    // channel rests disabled and ir_blast enables it only around a transmit. This keeps the APB
    // pm-lock (and the carrier/clock) off whenever the IR LED isn't actively sending.
    rmt_carrier_config_t carrier = { .frequency_hz = 38000, .duty_cycle = 0.33f };
    rmt_apply_carrier(s_chan, &carrier);
    s_cur_carrier = 38000;
    s_ready = true;
    ESP_LOGI(TAG, "IR TX ready on GPIO%d (RMT, 38/40/36 kHz; enabled per-transmit)", NUCLEO_IR_GPIO);
    return ESP_OK;
}

// SD content self-install — device glue. See nucleo_sdcontent.h. The download itself is sdc_engine.c
// (host-tested end to end); this file is the HTTPS transport, NVS (arm flag + last failure), the heap and
// Wi-Fi gates, and the localized messages.
#include "nucleo_sdcontent.h"
#include "sdc_engine.h"
#include "nucleo_board.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

extern const char *nucleo_setup_ip(void);                              // nucleo_setup (resolved at link)
extern const char *nucleo_i18n_lang(void) __attribute__((weak));       // nucleo_storage (resolved at link)

static const char *TAG = "sdcontent";

#define SDC_BASE_HOST   "https://indecenti.github.io/NucleoOs/"   // == UPD_BASE (OTA updater)
#define SDC_NVS_NS      "sdc"
#define SDC_NVS_ARM     "arm"
#define SDC_NVS_DIAG    "diag"
// Bytes per request. A 6 KB body plus GitHub's ~1 KB of headers stays under the 8 KB TLS record cap
// (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN) whatever record size the server picks — measured: such responses
// arrive in records of <= 4169 bytes, while a full-body GET of the manifest arrives in 16401-byte records.
#define SDC_WINDOW      6144u
// Minimum contiguous block to attempt the TLS handshake (mbedTLS is in low-memory mode: asymmetric 8K/4K
// buffers, dynamic buffers, CA/config freed after the handshake).
#define TLS_MIN_BLOCK   22000u

static sdc_state_t s_st;
const sdc_state_t *nucleo_sdcontent_state(void) { return &s_st; }

// Which packs a run installs. "core" = a complete web OS + ANIMA (~50 MB, what the wizard offers); the
// optional packs (arcade emulators, the PC/Android downloads) come later from Settings ▸ SD.
static char s_packs[48] = "core";

__attribute__((weak)) void nucleo_sdcontent_on_progress(const sdc_state_t *st) { (void)st; }
// Called once the STA link is up, right before the first TLS handshake: main.c frees the 32 KB canvas here.
__attribute__((weak)) void nucleo_sdcontent_on_net_ready(void) { }

// The install boot's heap at each stage, persisted (NVS "heap", /api/status .sdc_heap) on every run — the
// install boot has no httpd, so this is how its real RAM is read over the network afterwards.
static char s_heap[96];
static void heap_mark(const char *stage)
{
    size_t n = strlen(s_heap);
    if (n >= sizeof s_heap - 24) return;
    snprintf(s_heap + n, sizeof s_heap - n, "%s%s %u/%u", n ? " | " : "", stage,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
}

static const char *L5(const char *it, const char *en, const char *es, const char *fr, const char *de)
{
    const char *l = nucleo_i18n_lang ? nucleo_i18n_lang() : 0;
    if (!l) return en;
    if (l[0] == 'i' && l[1] == 't') return it;
    if (l[0] == 'e' && l[1] == 's') return es;
    if (l[0] == 'f' && l[1] == 'r') return fr;
    if (l[0] == 'd' && l[1] == 'e') return de;
    return en;
}

// "0.5.0" from the app descriptor's "0.5.0+9.gabc123".
static void fw_ver3(char *out, size_t cap)
{
    const esp_app_desc_t *a = esp_app_get_description();
    int v0 = 0, v1 = 0, v2 = 0;
    if (a) sscanf(a->version, "%d.%d.%d", &v0, &v1, &v2);
    snprintf(out, cap, "%d.%d.%d", v0, v1, v2);
}

// ---- NVS: arm flag + last failure -------------------------------------------------------------------

static nvs_handle_t nvs_open_sdc(bool write)
{
    nvs_handle_t h = 0;
    if (nvs_open(SDC_NVS_NS, write ? NVS_READWRITE : NVS_READONLY, &h) != ESP_OK) return 0;
    return h;
}

bool nucleo_sdcontent_armed(void)
{
    nvs_handle_t h = nvs_open_sdc(false);
    if (!h) return false;
    uint8_t v = 0; nvs_get_u8(h, SDC_NVS_ARM, &v); nvs_close(h);
    return v == 1;
}

bool nucleo_sdcontent_arm(bool on)
{
    nvs_handle_t h = nvs_open_sdc(true);
    if (!h) return false;
    esp_err_t e = nvs_set_u8(h, SDC_NVS_ARM, on ? 1 : 0);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return false;
    if (on) { ESP_LOGW(TAG, "SD content download armed — rebooting into the install boot"); esp_restart(); }
    return true;
}

static void diag_store(const char *s)
{
    nvs_handle_t h = nvs_open_sdc(true);
    if (!h) return;
    if (s) nvs_set_str(h, SDC_NVS_DIAG, s); else nvs_erase_key(h, SDC_NVS_DIAG);
    if (s_heap[0]) nvs_set_str(h, "heap", s_heap);
    nvs_commit(h); nvs_close(h);
}
bool nucleo_sdcontent_last_heap(char *out, size_t n)
{
    if (!out || n == 0) return false;
    out[0] = 0;
    nvs_handle_t h = nvs_open_sdc(false);
    if (!h) return false;
    size_t len = n;
    bool ok = nvs_get_str(h, "heap", out, &len) == ESP_OK && out[0];
    nvs_close(h);
    return ok;
}

bool nucleo_sdcontent_last_diag(char *out, size_t n)
{
    if (!out || n == 0) return false;
    out[0] = 0;
    nvs_handle_t h = nvs_open_sdc(false);
    if (!h) return false;
    size_t len = n;
    bool ok = nvs_get_str(h, SDC_NVS_DIAG, out, &len) == ESP_OK && out[0];
    nvs_close(h);
    return ok;
}

// ---- detect / decline -------------------------------------------------------------------------------

bool nucleo_sdcontent_needed(void)
{
    struct stat sb;
    if (stat(NUCLEO_SD_MOUNT, &sb) != 0) return false;                  // no card: nothing to fill
    char ver3[24]; fw_ver3(ver3, sizeof ver3);
    return sdc_content_needed(NUCLEO_SD_MOUNT, ver3);
}

static char s_card_tag[24], s_fw_tag[24];
nucleo_sdc_status_t nucleo_sdcontent_status(void)
{
    s_card_tag[0] = 0;
    struct stat sb;
    if (stat(NUCLEO_SD_MOUNT, &sb) != 0) return NUCLEO_SDC_NO_SD;
    char ver3[24]; fw_ver3(ver3, sizeof ver3);
    return (nucleo_sdc_status_t)sdc_content_status(NUCLEO_SD_MOUNT, ver3, s_card_tag, sizeof s_card_tag);
}
const char *nucleo_sdcontent_card_tag(void) { return s_card_tag; }
const char *nucleo_sdcontent_fw_tag(void) { if (!s_fw_tag[0]) fw_ver3(s_fw_tag, sizeof s_fw_tag); return s_fw_tag; }

void nucleo_sdcontent_decline(void)
{
    char ver3[24]; fw_ver3(ver3, sizeof ver3);
    sdc_content_write(NUCLEO_SD_MOUNT, ver3, false, true);
}

// ---- HTTPS transport: Range windows on ONE keep-alive connection ------------------------------------
// esp_http_client_close() really closes the socket, so it is NOT called between windows: after the body is
// read (and flushed), the next esp_http_client_open() on the same host reuses the connection (the client
// re-dials only when the server said "Connection: close" or a request failed — then cli_drop()).

static esp_http_client_handle_t s_cli;
static int s_tls_err;

static void cli_drop(void)
{
    if (!s_cli) return;
    esp_http_client_close(s_cli);
    esp_http_client_cleanup(s_cli);
    s_cli = NULL;
}

static int dev_get(void *ctx, const char *url, uint32_t off, uint32_t len, uint8_t *buf, int *status)
{
    (void)ctx;
    *status = 0;
    if (!s_cli) {
        esp_http_client_config_t cfg = {
            .url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 2048, .buffer_size_tx = 1024, .keep_alive_enable = true,
        };
        s_cli = esp_http_client_init(&cfg);
        if (!s_cli) return -1;
    } else if (esp_http_client_set_url(s_cli, url) != ESP_OK) {
        cli_drop();
        return -1;
    }
    char range[40];
    snprintf(range, sizeof range, "bytes=%u-%u", (unsigned)off, (unsigned)(off + len - 1));
    esp_http_client_set_header(s_cli, "Range", range);
    esp_err_t e = esp_http_client_open(s_cli, 0);
    if (e != ESP_OK) { s_tls_err = (int)e; cli_drop(); return -1; }
    int64_t cl = esp_http_client_fetch_headers(s_cli);
    int st = esp_http_client_get_status_code(s_cli);
    *status = st;
    if (st != 206 && st != 200) {                                        // 404 / 416 / 5xx: no body we want
        if (esp_http_client_flush_response(s_cli, NULL) != ESP_OK || !esp_http_client_is_persistent_connection(s_cli)) cli_drop();
        return -1;
    }
    // A server that ignored Range on a long body would now send 16 KB records the TLS layer cannot take.
    if (st == 200 && cl > (int64_t)len) { cli_drop(); return -1; }
    int got = 0;
    while (got < (int)len) {
        int r = esp_http_client_read(s_cli, (char *)buf + got, (int)len - got);
        if (r < 0) { *status = 0; cli_drop(); return -1; }
        if (r == 0) break;
        got += r;
    }
    if (esp_http_client_flush_response(s_cli, NULL) != ESP_OK || !esp_http_client_is_persistent_connection(s_cli)) cli_drop();
    return got;
}

static void io_progress(void *ctx, const sdc_state_t *st) { (void)ctx; nucleo_sdcontent_on_progress(st); }
static void io_tick(void *ctx) { (void)ctx; esp_task_wdt_reset(); }

// ---- the run ----------------------------------------------------------------------------------------

static void localize(void)
{
    const char *m;
    switch (s_st.code) {
    case SDC_E_NO_SD:        m = L5("Nessuna scheda SD", "No SD card", "Sin tarjeta SD", "Pas de carte SD", "Keine SD-Karte"); break;
    case SDC_E_SD_RO:        m = L5("SD non scrivibile", "SD not writable", "SD no escribible", "SD non inscriptible", "SD nicht beschreibbar"); break;
    case SDC_E_MANIFEST:     m = L5("Contenuti non raggiungibili", "Content unreachable", "Contenido inaccesible", "Contenu injoignable", "Inhalt nicht erreichbar"); break;
    case SDC_E_MANIFEST_BAD: m = L5("Manifest non valido", "Invalid manifest", "Manifest invalido", "Manifeste invalide", "Ungueltiges Manifest"); break;
    case SDC_E_TAG:          m = L5("Aggiorna prima il firmware", "Update the firmware first", "Actualiza el firmware", "Mettez a jour le firmware", "Erst Firmware updaten"); break;
    case SDC_E_FETCH:        m = L5("Download interrotto", "Download interrupted", "Descarga interrumpida", "Telechargement interrompu", "Download abgebrochen"); break;
    case SDC_E_VERIFY:       m = L5("File corrotto", "Corrupted file", "Archivo corrupto", "Fichier corrompu", "Datei beschaedigt"); break;
    case SDC_E_WRITE:        m = L5("Scrittura SD fallita", "SD write failed", "Error al escribir la SD", "Ecriture SD impossible", "SD-Schreibfehler"); break;
    case SDC_E_NOMEM:        m = L5("RAM insufficiente", "Not enough RAM", "Sin RAM", "RAM insuffisante", "Zu wenig RAM"); break;
    default:                 m = L5("Download fallito", "Download failed", "Descarga fallida", "Echec", "Fehler"); break;
    }
    const char *slash = strrchr(s_st.detail, '/');
    const char *d = slash ? slash + 1 : s_st.detail;
    if (s_st.code == SDC_E_MANIFEST || s_st.code == SDC_E_FETCH)
        snprintf(s_st.err, sizeof s_st.err, "%s (HTTP %d)%s%.28s", m, s_st.http, d[0] ? " " : "", d);
    else if (d[0])
        snprintf(s_st.err, sizeof s_st.err, "%s: %.40s", m, d);
    else
        snprintf(s_st.err, sizeof s_st.err, "%s", m);
}

static bool fail_here(sdc_code_t code, const char *detail)
{
    s_st.code = code; s_st.phase = SDC_FAILED;
    snprintf(s_st.detail, sizeof s_st.detail, "%s", detail ? detail : "");
    localize();
    diag_store(s_st.err);
    nucleo_sdcontent_on_progress(&s_st);
    return false;
}

bool nucleo_sdcontent_run(void)
{
    memset(&s_st, 0, sizeof s_st);
    s_st.phase = SDC_IDLE; s_st.pct = -1;        // IDLE here = waiting for the Wi-Fi link ("Connecting")
    nucleo_sdcontent_on_progress(&s_st);

    nucleo_sdcontent_arm(false);                 // FIRST: a crash or failure must never become a boot loop
    s_heap[0] = 0;
    heap_mark("boot");                           // canvas still held, Wi-Fi coming up

    for (int t = 0; t < 120 && (!nucleo_setup_ip() || !nucleo_setup_ip()[0]); t++) {
        vTaskDelay(pdMS_TO_TICKS(250)); esp_task_wdt_reset();
        if ((t & 3) == 3) nucleo_sdcontent_on_progress(&s_st);   // keep the "Connecting" bar alive (1 Hz)
    }
    if (!nucleo_setup_ip() || !nucleo_setup_ip()[0]) {
        s_st.code = SDC_E_FETCH; s_st.phase = SDC_FAILED;
        snprintf(s_st.err, sizeof s_st.err, "%s", L5("Nessuna rete Wi-Fi", "No Wi-Fi network", "Sin red Wi-Fi", "Pas de reseau Wi-Fi", "Kein WLAN"));
        heap_mark("nonet");
        diag_store(s_st.err); nucleo_sdcontent_on_progress(&s_st);
        return false;
    }
    heap_mark("link");
    nucleo_sdcontent_on_net_ready();             // main.c frees the 32 KB canvas for the handshake
    heap_mark("tls");

    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGW(TAG, "install boot heap: free=%u largest=%u (need >=%u)", (unsigned)freeb, (unsigned)largest, TLS_MIN_BLOCK);
    if (largest < TLS_MIN_BLOCK) {
        char kb[16]; snprintf(kb, sizeof kb, "%u KB", (unsigned)(largest / 1024));
        return fail_here(SDC_E_NOMEM, kb);
    }

    char ver3[24]; fw_ver3(ver3, sizeof ver3);
    char base[96]; snprintf(base, sizeof base, "%ssd/%s/", SDC_BASE_HOST, ver3);
    ESP_LOGW(TAG, "install v%s from %s, packs=%s, window=%u", ver3, base, s_packs, SDC_WINDOW);

    sdc_cfg_t cfg = { .root = NUCLEO_SD_MOUNT, .base_url = base, .ver3 = ver3, .packs = s_packs,
                      .window = SDC_WINDOW, .retries = 3 };
    sdc_io_t io = { .ctx = NULL, .get = dev_get, .progress = io_progress, .tick = io_tick };
    s_tls_err = 0;
    bool ok = sdc_engine_run(&cfg, &io, &s_st);
    cli_drop();
    { size_t n = strlen(s_heap);                 // the lowest free heap the whole run reached
      if (n < sizeof s_heap - 16) snprintf(s_heap + n, sizeof s_heap - n, " | min %u",
                                           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024)); }
    ESP_LOGW(TAG, "install %s: code=%d http=%d tls=%d requests=%d files %d/%d written=%d detail=%s",
             ok ? "OK" : "FAILED", s_st.code, s_st.http, s_tls_err, s_st.requests,
             s_st.files_done, s_st.files_total, s_st.files_written, s_st.detail);
    if (ok) { diag_store(NULL); return true; }   // clears the failure, keeps the heap profile
    localize();
    diag_store(s_st.err);
    nucleo_sdcontent_on_progress(&s_st);
    return false;
}

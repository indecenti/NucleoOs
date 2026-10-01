// SD content self-install — device engine. See nucleo_sdcontent.h. Reuses the OTA updater's proven
// pattern: esp_http_client + the cert bundle, streaming mbedtls SHA-256, run only in the big-heap
// boot window. content_policy.c decides what may be written; this file does the I/O and never writes
// a path the policy refused.
#include "nucleo_sdcontent.h"
#include "content_policy.h"
#include "nucleo_board.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

// pulled from nucleo_storage (free space) + nucleo_setup (STA IP wait) without their headers.
extern const char *nucleo_setup_ip(void);

static const char *TAG = "sdcontent";

#define SDC_BASE_HOST   "https://indecenti.github.io/NucleoOs/"   // == UPD_BASE (OTA updater)
#define SDC_NVS_NS      "sdc"
#define SDC_NVS_ARM     "arm"
#define CONTENT_JSON    NUCLEO_SD_MOUNT "/system/content.json"
#define MANIFEST_DIR    NUCLEO_SD_MOUNT "/system/content"
#define MANIFEST_PART   MANIFEST_DIR "/manifest.part"
#define MANIFEST_FILE   MANIFEST_DIR "/manifest.txt"
#define TLS_MIN_BLOCK   40000u          // a GitHub TLS handshake needs ~40 KB contiguous

static sdc_state_t s_st;
const sdc_state_t *nucleo_sdcontent_state(void) { return &s_st; }

// Which packs this run installs. Default "core" — a complete, working web OS + ANIMA (~50 MB). The
// optional packs (arcade emulators, the PC/Android downloads) are added later from Settings ▸ SD, so the
// first-boot download stays lean and matches the "~50 MB" the wizard offers.
static char s_packs[48] = "core";
static bool pack_wanted(const char *pack)
{
    size_t pl = strlen(pack);
    for (const char *p = s_packs; *p; ) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == pl && strncmp(p, pack, pl) == 0) return true;
        p += len; if (*p == ',') p++;
    }
    return false;
}

// Weak default; main.c overrides to paint the boot-window progress bar.
__attribute__((weak)) void nucleo_sdcontent_on_progress(const sdc_state_t *st) { (void)st; }
static void progress(void) { nucleo_sdcontent_on_progress(&s_st); }

// ---- small helpers ----------------------------------------------------------------------------------

// "0.5.0" from the app descriptor's "0.5.0+9.gabc123".
static void fw_ver3(char *out, size_t cap)
{
    const esp_app_desc_t *a = esp_app_get_description();
    int v0 = 0, v1 = 0, v2 = 0;
    if (a) sscanf(a->version, "%d.%d.%d", &v0, &v1, &v2);
    snprintf(out, cap, "%d.%d.%d", v0, v1, v2);
}

static void hex32(const unsigned char *d, char *out)      // 32 bytes -> 64 lowercase hex + NUL
{
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i * 2] = h[d[i] >> 4]; out[i * 2 + 1] = h[d[i] & 15]; }
    out[64] = 0;
}

// Create every parent directory of an SD absolute path ("/sd/a/b/c" -> mkdir /sd/a, /sd/a/b).
static void mkdir_parents(const char *abspath)
{
    char tmp[260];
    snprintf(tmp, sizeof tmp, "%s", abspath);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) ESP_LOGW(TAG, "mkdir %s: errno %d", tmp, errno);
            *p = '/';
        }
    }
}

static bool file_stat(const char *abspath, uint32_t *size)
{
    struct stat st;
    if (stat(abspath, &st) != 0) return false;
    if (size) *size = (uint32_t)st.st_size;
    return true;
}

// SHA-256 of an on-disk file -> 64 hex. false on read error.
static bool file_sha(const char *abspath, char *out_hex)
{
    FILE *f = fopen(abspath, "rb");
    if (!f) return false;
    mbedtls_sha256_context c; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
    static char buf[1024]; size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update(&c, (const unsigned char *)buf, r);
    fclose(f);
    unsigned char dig[32]; mbedtls_sha256_finish(&c, dig); mbedtls_sha256_free(&c);
    hex32(dig, out_hex);
    return true;
}

// ---- HTTP (one keep-alive handle, reused across every file) -----------------------------------------

static esp_http_client_handle_t s_cli;

static bool http_open(const char *url)
{
    if (!s_cli) {
        esp_http_client_config_t cfg = {
            .url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 2048, .keep_alive_enable = true,
        };
        s_cli = esp_http_client_init(&cfg);
        if (!s_cli) return false;
    } else {
        esp_http_client_set_url(s_cli, url);
    }
    if (esp_http_client_open(s_cli, 0) != ESP_OK) {        // re-dial once on a dropped keep-alive
        esp_http_client_cleanup(s_cli); s_cli = NULL;
        return http_open(url);
    }
    esp_http_client_fetch_headers(s_cli);
    int status = esp_http_client_get_status_code(s_cli);
    if (status != 200) { ESP_LOGW(TAG, "GET %s -> HTTP %d", url, status); esp_http_client_close(s_cli); return false; }
    return true;
}

static void http_done(void) { if (s_cli) esp_http_client_close(s_cli); }

// Download `url` to `dstpart`, hashing as it streams. On success `sha_hex` holds the file's SHA-256.
// Returns bytes written, or -1 on error. Feeds the WDT per chunk.
static long http_to_file(const char *url, const char *dstpart, char *sha_hex)
{
    if (!http_open(url)) return -1;
    FILE *f = fopen(dstpart, "wb");
    if (!f) { http_done(); ESP_LOGE(TAG, "open %s: errno %d", dstpart, errno); return -1; }
    mbedtls_sha256_context c; mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0);
    static char buf[2048];
    long total = 0; int r;
    bool io_err = false;
    while ((r = esp_http_client_read(s_cli, buf, sizeof buf)) > 0) {
        if (fwrite(buf, 1, r, f) != (size_t)r) { io_err = true; break; }   // SD full / write error
        mbedtls_sha256_update(&c, (const unsigned char *)buf, r);
        total += r;
        esp_task_wdt_reset();
    }
    fclose(f);
    http_done();
    unsigned char dig[32]; mbedtls_sha256_finish(&c, dig); mbedtls_sha256_free(&c);
    if (io_err || r < 0) { unlink(dstpart); return -1; }
    hex32(dig, sha_hex);
    return total;
}

// ---- content.json (tiny, hand-parsed: no cJSON dependency for 3 fields) ------------------------------

static bool content_read(char *tag, size_t tagcap, bool *complete)
{
    *complete = false; if (tag) tag[0] = 0;
    FILE *f = fopen(CONTENT_JSON, "rb");
    if (!f) return false;
    char buf[192]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f);
    buf[n] = 0;
    const char *t = strstr(buf, "\"tag\"");
    if (t) { t = strchr(t, ':'); if (t) { t = strchr(t, '"'); if (t) { t++; size_t k = 0;
        while (t[k] && t[k] != '"' && k < tagcap - 1) { tag[k] = t[k]; k++; } tag[k] = 0; } } }
    *complete = strstr(buf, "\"complete\":true") || strstr(buf, "\"complete\": true");
    return true;
}

static void content_write(const char *tag, bool complete, bool declined)
{
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    FILE *f = fopen(CONTENT_JSON, "w");
    if (!f) { ESP_LOGE(TAG, "content.json write: errno %d", errno); return; }
    fprintf(f, "{\"tag\":\"%s\",\"complete\":%s,\"declined\":%s}\n",
            tag, complete ? "true" : "false", declined ? "true" : "false");
    fclose(f);
}

// ---- public: detect / arm / decline -----------------------------------------------------------------

bool nucleo_sdcontent_needed(void)
{
    struct stat st;
    if (stat(NUCLEO_SD_MOUNT, &st) != 0) return false;      // no card -> nothing to fill
    char tag[24], ver3[24]; bool complete;
    fw_ver3(ver3, sizeof ver3);
    if (!content_read(tag, sizeof tag, &complete)) return true;   // never installed
    if (!complete) return true;                                   // interrupted
    return strcmp(tag, ver3) != 0;                                // installed, but for another firmware
}

static nvs_handle_t nvs_open_sdc(bool write)
{
    nvs_handle_t h = 0;
    nvs_open(SDC_NVS_NS, write ? NVS_READWRITE : NVS_READONLY, &h);
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
    if (on) { ESP_LOGW(TAG, "SD content download armed — rebooting into the boot-window installer"); esp_restart(); }
    return true;
}

void nucleo_sdcontent_decline(void)
{
    char ver3[24]; fw_ver3(ver3, sizeof ver3);
    content_write(ver3, false, true);     // complete=false, declined=true: not offered again until a new firmware
}

// ---- the run ----------------------------------------------------------------------------------------

// language pick without pulling nucleo_i18n into REQUIRES: weak ref like the UI modals.
extern const char *nucleo_i18n_lang(void) __attribute__((weak));
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
static bool fail_run(const char *msg) { snprintf(s_st.err, sizeof s_st.err, "%s", msg); s_st.phase = SDC_FAILED; progress(); return false; }

// Download one file (WRITE/CREATE): url -> /sd/<path>.part -> verify sha -> rename. One retry.
static bool fetch_file(const char *base, const sdc_file_t *f)
{
    char url[320], dst[260], part[266], got[65];
    snprintf(url, sizeof url, "%s%s", base, f->path);
    snprintf(dst, sizeof dst, "%s/%s", NUCLEO_SD_MOUNT, f->path);
    snprintf(part, sizeof part, "%s.part", dst);
    mkdir_parents(dst);
    for (int attempt = 0; attempt < 2; attempt++) {
        long n = http_to_file(url, part, got);
        if (n >= 0 && (uint32_t)n == f->size && strncmp(got, f->sha, 64) == 0) {
            unlink(dst);
            if (rename(part, dst) != 0) { unlink(part); ESP_LOGE(TAG, "rename %s: errno %d", dst, errno); return false; }
            // keep the .gz twin consistent: if the manifest brings a raw file, a stale .gz beside it would
            // shadow it (webfs serves .gz first). Remove a sibling .gz the manifest did not also list — the
            // next manifest line that IS the .gz will re-create it; a raw-only file loses its stale twin.
            return true;
        }
        ESP_LOGW(TAG, "file '%s' attempt %d: n=%ld want=%u sha_ok=%d", f->path, attempt, n, (unsigned)f->size, n >= 0 && strncmp(got, f->sha, 64) == 0);
        unlink(part);
    }
    return false;
}

bool nucleo_sdcontent_run(void)
{
    memset(&s_st, 0, sizeof s_st);
    s_st.phase = SDC_CHECKING; s_st.pct = -1; progress();

    // Clear the arm flag FIRST: a crash/failure must never turn into a download-every-boot loop.
    nucleo_sdcontent_arm(false);

    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < TLS_MIN_BLOCK)
        return fail_run(L5("Memoria insufficiente", "Not enough memory", "Memoria insuficiente", "Memoire insuffisante", "Zu wenig Speicher"));

    // Wait for the STA IP (creds were saved by the wizard just before the reboot).
    for (int t = 0; t < 120 && (!nucleo_setup_ip() || !nucleo_setup_ip()[0]); t++) { vTaskDelay(pdMS_TO_TICKS(250)); esp_task_wdt_reset(); }
    if (!nucleo_setup_ip() || !nucleo_setup_ip()[0])
        return fail_run(L5("Nessuna rete Wi-Fi", "No Wi-Fi network", "Sin red Wi-Fi", "Pas de reseau Wi-Fi", "Kein WLAN"));

    char ver3[24]; fw_ver3(ver3, sizeof ver3);
    char base[96]; snprintf(base, sizeof base, "%ssd/%s/", SDC_BASE_HOST, ver3);

    // 1) fetch the manifest to a file.
    mkdir(NUCLEO_SD_MOUNT "/system", 0775); mkdir(MANIFEST_DIR, 0775);
    char murl[128], msha[65];
    snprintf(murl, sizeof murl, "%ssd-manifest.txt", base);
    if (http_to_file(murl, MANIFEST_PART, msha) < 0)
        return fail_run(L5("Contenuti non disponibili online", "Content not available online", "Contenido no disponible online", "Contenu indisponible en ligne", "Inhalt online nicht verfuegbar"));
    unlink(MANIFEST_FILE); rename(MANIFEST_PART, MANIFEST_FILE);

    // 2) header + count pass.
    FILE *mf = fopen(MANIFEST_FILE, "r");
    if (!mf) return fail_run(L5("Manifest illeggibile", "Manifest unreadable", "Manifest ilegible", "Manifeste illisible", "Manifest unlesbar"));
    char line[320]; sdc_header_t hdr; bool have_hdr = false;
    int total = 0;
    while (fgets(line, sizeof line, mf)) {
        if (!have_hdr && sdc_parse_header(line, &hdr)) { have_hdr = true;
            if (!sdc_tag_matches(ver3, hdr.tag)) { fclose(mf);
                return fail_run(L5("Aggiorna prima il firmware", "Update the firmware first", "Actualiza el firmware", "Mettez a jour le firmware", "Erst Firmware updaten")); }
            continue;
        }
        sdc_file_t f; bool cmt;
        if (sdc_parse_file(line, &f, &cmt)) { if (pack_wanted(f.pack)) total++; }
        else if (!cmt) { fclose(mf); return fail_run(L5("Manifest non valido", "Invalid manifest", "Manifest invalido", "Manifeste invalide", "Ungueltiges Manifest")); }
    }
    if (!have_hdr) { fclose(mf); return fail_run(L5("Manifest non valido", "Invalid manifest", "Manifest invalido", "Manifeste invalide", "Ungueltiges Manifest")); }
    s_st.files_total = total; s_st.phase = SDC_DOWNLOADING; s_st.pct = 0; progress();

    // 3) install pass.
    rewind(mf);
    while (fgets(line, sizeof line, mf)) {
        sdc_file_t f; bool cmt;
        if (!sdc_parse_file(line, &f, &cmt)) continue;      // header / #pack / comment
        if (!pack_wanted(f.pack)) continue;                 // optional pack, not selected this run
        char dst[260]; snprintf(dst, sizeof dst, "%s/%s", NUCLEO_SD_MOUNT, f.path);
        uint32_t dsz = 0; bool on_disk = file_stat(dst, &dsz);
        char dsha[65]; bool have_sha = false;
        if (on_disk && dsz == f.size) have_sha = file_sha(dst, dsha);   // only hash when size matches
        sdc_plan_t pl = sdc_plan(&f, on_disk, dsz, have_sha ? dsha : NULL);

        const char *slash = strrchr(f.path, '/');
        const char *bn = slash ? slash + 1 : f.path;
        snprintf(s_st.cur, sizeof s_st.cur, "%.39s", bn);   // basename for the progress line (bounded)

        if (pl == SDC_PLAN_SKIP || pl == SDC_PLAN_CREATE_SKIP) {
            s_st.files_done++;
        } else if (pl == SDC_PLAN_MERGE) {
            // apps.json: on a fresh/absent file, write it (first install). If it already exists it is the
            // device's live registry (agent-created apps etc.) — keep it, never blind-overwrite.
            if (!on_disk) { if (fetch_file(base, &f)) s_st.files_written++; else { fclose(mf); return fail_run(L5("Download fallito", "Download failed", "Descarga fallida", "Echec du telechargement", "Download fehlgeschlagen")); } }
            s_st.files_done++;
        } else { // WRITE
            if (!fetch_file(base, &f)) { fclose(mf); return fail_run(L5("Download fallito", "Download failed", "Descarga fallida", "Echec du telechargement", "Download fehlgeschlagen")); }
            s_st.files_written++; s_st.files_done++; s_st.recv_kb += (int)(f.size / 1024);
        }
        s_st.pct = total > 0 ? (s_st.files_done * 100) / total : 100;
        progress();
    }
    fclose(mf);
    if (s_cli) { esp_http_client_cleanup(s_cli); s_cli = NULL; }

    content_write(ver3, true, false);
    s_st.phase = SDC_DONE; s_st.pct = 100; progress();
    ESP_LOGW(TAG, "SD content complete: %d files (%d written) for %s", s_st.files_done, s_st.files_written, ver3);
    return true;
}

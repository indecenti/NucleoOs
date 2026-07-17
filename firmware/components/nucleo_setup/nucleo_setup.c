#include "nucleo_setup.h"
#include "nucleo_board.h"
#include <sys/stat.h>   // mkdir() for the redundant SD credential backup
#include "nucleo_ui.h"
#include "nucleo_storage.h"
#include "nucleo_app.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_mac.h"       // per-device MAC for the unique default hotspot SSID
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_event.h"
#include "cJSON.h"
#include "nvs.h"           // NVS fallback tier: config survives even without /cfg partition or SD
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <time.h>
#include "nucleo_theme.h"
#include "nucleo_i18n.h"   // first-run wizard language step + bilingual (TR) wizard strings
#include "esp_heap_caps.h"

static const char *TAG = "setup";

// Privacy (OPT-IN): join networks with a locally-administered RANDOM MAC instead of the burned-in
// Espressif one, so a network can't fingerprint/track this device.
// DEFAULT OFF, and here is why: the MAC was re-rolled with esp_random() on EVERY boot, so although it
// was stable within one session, each reboot presented a NEW MAC to the router -> DHCP handed out a
// NEW IP every reboot -> the device became unreachable at its known address (e.g. 192.168.0.166) and
// undiscoverable (the MAC no longer matches the factory OUI; mDNS is off). For your OWN home device you
// want the stable factory MAC and a fixed IP. Set to 1 only when joining untrusted networks where
// anti-tracking matters. (To get BOTH privacy and a stable IP, derive a DETERMINISTIC per-device MAC
// from the factory MAC instead of esp_random() — a future enhancement.)
#ifndef NUCLEO_PRIVACY_RANDOM_STA_MAC
#define NUCLEO_PRIVACY_RANDOM_STA_MAC 0
#endif

// Wi-Fi bring-up must NEVER panic the boot. A transient radio/heap error during init or a
// (re)connect — common precisely when no network is in range — would otherwise abort() and,
// since the panic handler is set to reboot, loop the device forever before it ever reaches
// the home screen or the first-run wizard. Log-and-continue instead of ESP_ERROR_CHECK so a
// failed radio degrades to "no network" (recoverable from the on-device Wi-Fi menu) rather
// than bricking the boot.
#define WIFI_TRY(expr) do { \
    esp_err_t _werr = (expr); \
    if (_werr != ESP_OK) ESP_LOGW(TAG, "%s -> %s", #expr, esp_err_to_name(_werr)); \
} while (0)
// Brick-class config now lives on the power-loss-safe LittleFS store (internal flash),
// not the SD's FAT which can corrupt on a power cut mid-write. SETUP_LEGACY is the old
// SD path: read once and migrated to /cfg so existing devices keep their setup.
#define SETUP_JSON   NUCLEO_CFG_MOUNT "/config/setup.json"
#define SETUP_LEGACY NUCLEO_SD_MOUNT  "/system/config/setup.json"
#define AP_SSID_PREFIX  "NucleoOS"     // factory default hotspot name prefix; a per-device suffix is appended
#define AP_PASS_LEGACY  "nucleoos"     // the OLD shared default — auto-upgraded to a per-device random one
#define AP_SSID_LEGACY  "NucleoOS-Setup"   // the OLD shared default SSID — auto-upgraded to a per-device name

static char s_ip[16];   // STA IP once connected (empty if not)

static char s_mode[4] = "ap";          // "sta" | "ap"
static char s_ssid[33];
static char s_name[24] = "nucleo-01";
// "Setup done" flag. It is set true ONLY when the first-run wizard finishes — NOT as a side effect of
// any save_config() (e.g. the AP starts and mints its per-device credentials at boot, before the wizard
// even runs, and persisting THOSE must not mark setup complete or the wizard would be skipped forever).
static bool s_complete = false;
// Hotspot credentials are runtime-editable (persisted in setup.json). Empty = not yet initialised;
// ensure_default_ap_creds() fills a per-device SSID + a RANDOM WPA2 password on first use. An empty
// password after that means an OPEN AP; otherwise WPA2-PSK (the driver requires 8..63 chars).
static char s_ap_ssid[33] = "";
static char s_ap_pass[64] = "";
static bool s_wifi_ready;
static bool s_want_sta = false;        // true while we intend to stay on STA (drives auto-reconnect)
// "Real OS" Wi-Fi: we remember EVERY network joined and auto-pick the best in-range one. s_auto means
// the user intends a client (STA) link, so the background supervisor keeps (re)joining known networks
// even after an AP fallback — and rejoins automatically when the home Wi-Fi reappears. Distinct from
// s_want_sta (which only tracks "keep the CURRENT association alive").
static bool s_auto = false;
static SemaphoreHandle_t s_wifi_op_lock;   // serialize join / best-known selection (one connect attempt at a time)
static void wifi_supervisor(void *arg);    // background (re)join task; defined after the scan helpers
// One scan at a time. The native Wi-Fi app drives nucleo_setup_scan() from a worker task and the web
// scanner (/api/wifi/scan) drives it from the httpd task; two esp_wifi_scan_start() calls in parallel
// destabilize the driver, so all callers serialize on this. Created once in wifi_ensure() (boot,
// single-threaded), so it is non-NULL before any concurrent scan is possible.
static SemaphoreHandle_t s_scan_lock;

// NTP clock sync. The ESP boots with its clock at the 1970 epoch and only counts up from
// power-on, so the on-device Clock app shows wrong time until we sync. We start an SNTP
// client the moment the STA gets an IP (every (re)connect), which is the only point we are
// guaranteed to have internet. Default zone is Europe/Rome; the device serves Italy.
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.cloudflare.com"
#define NTP_SERVER_3  "time.google.com"
#define NTP_SERVER_4  "time.apple.com"
#define DEFAULT_TZ  "CET-1CEST,M3.5.0,M10.5.0/3"   // POSIX TZ for Europe/Rome (with DST)
static bool s_sntp_inited;
static volatile bool s_time_synced;    // flips true once the first NTP reply lands


static void on_time_sync(struct timeval *tv)
{
    s_time_synced = true;
    time_t t = tv ? tv->tv_sec : time(NULL);
    char buf[32];
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ESP_LOGI(TAG, "NTP time synced: %s (local)", buf);
    // Persist the last-known time so an offline boot can show an approximate clock. The SD mounts at
    // NUCLEO_SD_MOUNT ("/sd"), NOT "/sdcard" — the old hardcoded "/sdcard/..." path never existed, so
    // both the write and restore_saved_time() read were permanent no-ops (device stuck at 1970 offline).
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    FILE *f = fopen(NUCLEO_SD_MOUNT "/system/time.json", "w");
    if (f) { fprintf(f, "{\"t\":%lu}", (unsigned long)t); fclose(f); }
}

// Start (or, if already running, restart) the SNTP client. Idempotent and safe to call on
// every STA GOT_IP event — the first call inits, later ones just kick a fresh poll.
static void start_sntp(void)
{
    if (s_sntp_inited) { esp_sntp_restart(); return; }
    setenv("TZ", DEFAULT_TZ, 1);
    tzset();
    // The MULTIPLE macro takes exactly (count, list); the server list MUST be wrapped in
    // ESP_SNTP_SERVER_LIST() so its commas stay inside one macro argument. The array it builds is
    // sized by CONFIG_LWIP_SNTP_MAX_SERVERS (=4, pinned in sdkconfig.defaults so a config regen
    // can't shrink it back to 1 and reintroduce the "excess elements"/arg-count build break).
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(4,
        ESP_SNTP_SERVER_LIST(NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3, NTP_SERVER_4));
    cfg.start = true;
    cfg.sync_cb = on_time_sync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err)); return; }
    s_sntp_inited = true;
    ESP_LOGI(TAG, "SNTP started (4 servers), TZ=%s", DEFAULT_TZ);
}

// Keep the STA link alive and track the live IP. Without auto-reconnect on disconnect a
// single drop (AP reboot, roaming, weak signal, or the very first association attempt)
// would leave the device offline forever — that is the "doesn't keep the connection" bug.
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA disconnected (reason=%d, was ip=%s) — reconnecting",
                 d ? d->reason : -1, s_ip[0] ? s_ip : "-");   // reason code lands in the RAM ring -> /api/logs
        s_ip[0] = '\0';
        if (s_want_sta) esp_wifi_connect();          // reconnect to the saved network
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        uint32_t a = e->ip_info.ip.addr;
        snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u",
                 (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
                 (unsigned)((a >> 16) & 0xff), (unsigned)((a >> 24) & 0xff));
        ESP_LOGI(TAG, "STA got IP %s free=%u largest=%u", s_ip,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        // Refresh the displayed network from the AP we are ACTUALLY associated with. This is the
        // single source of truth for the header SSID: it fires on every (re)connect — including the
        // driver's own auto-reconnect and a supervisor network switch — so the name can never be
        // stale (showing an old network) or blank while connected. Also pins mode to "sta" (e.g.
        // after the supervisor rejoined following an AP fallback).
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.ssid[0]) {
            strncpy(s_ssid, (char *)ap.ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = 0;
            strncpy(s_mode, "sta", sizeof(s_mode) - 1);
        }
        start_sntp();                                // sync the clock now that we have internet
    }
}

// ---- persistence -----------------------------------------------------------

// Read a whole small config file. CAPS the allocation: `n` comes from ftell on a file that may be
// corrupt or attacker-placed (SD legacy path), and a multi-MB malloc on the ~18 KB heap would OOM the
// boot. Config docs here are < 4 KB; 32 KB is a generous ceiling that rejects the pathological case.
#define SLURP_MAX (32 * 1024)
static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > SLURP_MAX) { fclose(f); return NULL; }
    char *b = malloc(n + 1);
    if (b && fread(b, 1, n, f) == (size_t)n) b[n] = '\0'; else { free(b); b = NULL; }
    fclose(f);
    return b;
}

// ---- three-tier persistence ------------------------------------------------
// Config MUST survive a reboot on ANY install, including the worst case a user can hit: a firmware
// loaded through a third-party launcher that lacks our custom partition table (so the /cfg LittleFS
// store never mounts) AND with no SD card inserted. We therefore write to three INDEPENDENT tiers,
// each best-effort so one failure never blocks the others:
//   1. /cfg LittleFS  — primary, power-loss-safe, works on an SD-less device (needs our partition table)
//   2. NVS            — present in every ESP-IDF app (esp_wifi already relies on it); the guaranteed
//                       fallback that persists even when tiers 1 and 3 are both unavailable
//   3. SD mirror      — survives an internal-flash wipe / reflash and is human-visible on the card
// Read order mirrors reliability: /cfg -> NVS -> SD. The first tier that answers wins, and the loader
// re-persists so any missing tier heals on the next save. Result: settings stick no matter how the
// firmware was installed and whether or not an SD is present.
#define CFG_NVS_NS "nucleocfg"   // NVS namespace for the setup/networks documents (<=15 chars)

// Atomic write via temp+rename. Parent dirs must already exist. Returns false — never crashes — if the
// path's filesystem is absent. Works on BOTH stores: LittleFS rename overwrites the destination in place
// (atomic, power-loss-safe), but FATFS (the SD) rename FAILS if the destination already exists — so on a
// rename failure we remove the old file and retry. tmp is only dropped if the retry also fails, so the
// destination is never left both-gone by the common (dest-exists) case.
static bool write_file_atomic(const char *path, const char *text)
{
    char tmp[160];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return false;
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = (fputs(text, f) >= 0);
    fflush(f);
    fclose(f);
    if (!ok) { remove(tmp); return false; }
    if (rename(tmp, path) != 0) {          // LittleFS: overwrites -> done. FATFS: fails if dest exists...
        remove(path);                      // ...so clear the old file and retry (SD mirror path).
        if (rename(tmp, path) != 0) { remove(tmp); return false; }
    }
    return true;
}

// SD backup write: create the /system/config subtree first. Best-effort no-op if no card is mounted.
static bool write_sd_backup(const char *sd_path, const char *text)
{
    mkdir(NUCLEO_SD_MOUNT "/system", 0775);
    mkdir(NUCLEO_SD_MOUNT "/system/config", 0775);
    return write_file_atomic(sd_path, text);
}

static bool nvs_write_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

// Read an NVS string. Returns a malloc'd, NUL-terminated buffer (caller frees) or NULL if absent.
static char *nvs_read_str(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t sz = 0;
    if (nvs_get_str(h, key, NULL, &sz) != ESP_OK || sz == 0) { nvs_close(h); return NULL; }
    char *buf = malloc(sz);
    if (!buf) { nvs_close(h); return NULL; }
    esp_err_t e = nvs_get_str(h, key, buf, &sz);
    nvs_close(h);
    if (e != ESP_OK) { free(buf); return NULL; }
    return buf;
}

// Persistence health of the most recent persist_doc(), surfaced via nucleo_setup_persist_status()
// for /api/diag so "settings not saved" reports are instantly triageable.
static bool s_cfg_ok = false, s_nvs_ok = false, s_sd_ok = false;
static int  s_tiers_ok = -1;   // -1 until the first save

// Fan one config document out to every available tier (independent, best-effort writes). Returns how
// many tiers accepted it, so the caller can shout when that is zero (no persistence at all).
static int persist_doc(const char *cfg_path, const char *sd_path, const char *nvs_key, const char *text)
{
    bool cfg = write_file_atomic(cfg_path, text);     // 1. /cfg LittleFS (power-loss-safe, SD-independent)
    bool nvs = nvs_write_str(nvs_key, text);          // 2. NVS (always present — the guaranteed fallback)
    bool sd  = write_sd_backup(sd_path, text);        // 3. SD mirror (survives a flash wipe)
    s_cfg_ok = cfg; s_nvs_ok = nvs; s_sd_ok = sd;
    s_tiers_ok = (cfg ? 1 : 0) + (nvs ? 1 : 0) + (sd ? 1 : 0);
    return s_tiers_ok;
}

// Load a config document in reliability order. *from_fallback is set when it did NOT come from the
// primary /cfg tier, so the caller can re-persist and heal the others. Malloc'd result (caller frees).
static char *load_doc(const char *cfg_path, const char *sd_path, const char *nvs_key, bool *from_fallback)
{
    if (from_fallback) *from_fallback = false;
    char *txt = slurp(cfg_path);
    if (txt) return txt;
    txt = nvs_read_str(nvs_key);
    if (txt) { if (from_fallback) *from_fallback = true; return txt; }
    txt = slurp(sd_path);
    if (txt) { if (from_fallback) *from_fallback = true; return txt; }
    return NULL;
}

static void save_config(void);   // fwd: load may re-persist when recovering from a fallback tier

static bool load_config(void)
{
    // Try /cfg, then the NVS fallback, then the SD backup (covers the legacy SD-only layout too).
    bool from_fallback = false;
    char *txt = load_doc(SETUP_JSON, SETUP_LEGACY, "setup", &from_fallback);
    if (!txt) return false;
    cJSON *r = cJSON_Parse(txt); free(txt);
    if (!r) return false;
    cJSON *c = cJSON_GetObjectItem(r, "complete");
    cJSON *m = cJSON_GetObjectItem(r, "mode");
    cJSON *s = cJSON_GetObjectItem(r, "ssid");
    cJSON *n = cJSON_GetObjectItem(r, "device_name");
    cJSON *as = cJSON_GetObjectItem(r, "ap_ssid");
    cJSON *ap = cJSON_GetObjectItem(r, "ap_pass");
    if (cJSON_IsString(m)) strncpy(s_mode, m->valuestring, sizeof(s_mode) - 1);
    if (cJSON_IsString(s)) strncpy(s_ssid, s->valuestring, sizeof(s_ssid) - 1);
    if (cJSON_IsString(n)) strncpy(s_name, n->valuestring, sizeof(s_name) - 1);
    if (cJSON_IsString(as) && as->valuestring[0]) strncpy(s_ap_ssid, as->valuestring, sizeof(s_ap_ssid) - 1);
    if (cJSON_IsString(ap)) strncpy(s_ap_pass, ap->valuestring, sizeof(s_ap_pass) - 1);
    bool complete = cJSON_IsTrue(c);
    s_complete = complete;                    // reflect the persisted flag so later save_config()s preserve it
    cJSON_Delete(r);
    // Recovered from NVS or the SD backup (e.g. /cfg absent on a launcher install, or the legacy
    // SD-only layout): rewrite everywhere so every available tier holds a fresh copy next boot.
    if (from_fallback && complete) { save_config(); ESP_LOGI(TAG, "setup recovered from fallback tier — re-persisted to all tiers"); }
    return complete;
}

static void save_config(void)
{
    // Serialize with cJSON — it ESCAPES quotes/backslashes in the user-supplied strings (device name is
    // free-text; SSIDs/passwords can contain " or \). Hand-rolled snprintf JSON would emit a malformed
    // document that persist_doc then fans out IDENTICALLY to all three tiers, so cJSON_Parse fails
    // everywhere on next boot -> load_config() returns false -> the first-run wizard re-runs and the
    // config resets. Then fan out best-effort (no early return: a dead /cfg must not stop NVS + SD).
    cJSON *r = cJSON_CreateObject();
    if (!r) return;
    cJSON_AddBoolToObject(r,   "complete",    s_complete);   // true only after the wizard finishes (see s_complete)
    cJSON_AddStringToObject(r, "mode",        s_mode);
    cJSON_AddStringToObject(r, "ssid",        s_ssid);
    cJSON_AddStringToObject(r, "device_name", s_name);
    cJSON_AddStringToObject(r, "ap_ssid",     s_ap_ssid);
    cJSON_AddStringToObject(r, "ap_pass",     s_ap_pass);
    char *txt = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!txt) return;
    int tiers = persist_doc(SETUP_JSON, SETUP_LEGACY, "setup", txt);
    if (tiers == 0) ESP_LOGE(TAG, "save_config: NO persistence tier available — settings will not survive reboot");
    free(txt);
}

// ---- known-networks store (multi-network) ----------------------------------
// Persisted next to setup.json on the power-loss-safe flash store. Holds every Wi-Fi we have
// successfully joined (SSID + password + manual priority + recency stamp) so the device can scan
// and pick the best in-range one instead of only ever retrying the last network.
#define NETS_JSON  NUCLEO_CFG_MOUNT "/config/networks.json"
#define NETS_SD    NUCLEO_SD_MOUNT  "/system/config/networks.json"   // redundant SD backup of saved networks
#define MAX_NETS   16
typedef struct { char ssid[33]; char pass[64]; uint8_t prio; uint32_t seq; } known_net_t;
static known_net_t s_nets[MAX_NETS];
static int      s_net_n = 0;
static uint32_t s_net_seq = 0;          // monotonic recency stamp (higher = used more recently)
static bool     s_nets_loaded = false;

static void save_networks(void);

static void load_networks(void)
{
    if (s_nets_loaded) return;
    s_nets_loaded = true;
    bool from_fallback = false;
    char *txt = load_doc(NETS_JSON, NETS_SD, "networks", &from_fallback);   // /cfg -> NVS -> SD
    if (!txt) return;
    cJSON *r = cJSON_Parse(txt); free(txt);
    if (!r) return;
    cJSON *seq = cJSON_GetObjectItem(r, "seq");
    if (cJSON_IsNumber(seq)) s_net_seq = (uint32_t)seq->valuedouble;
    cJSON *arr = cJSON_GetObjectItem(r, "nets");
    int k = 0; cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (k >= MAX_NETS) break;
        cJSON *s = cJSON_GetObjectItem(it, "ssid");
        if (!cJSON_IsString(s) || !s->valuestring[0]) continue;
        cJSON *p  = cJSON_GetObjectItem(it, "pass");
        cJSON *pr = cJSON_GetObjectItem(it, "prio");
        cJSON *sq = cJSON_GetObjectItem(it, "seq");
        strncpy(s_nets[k].ssid, s->valuestring, 32); s_nets[k].ssid[32] = 0;
        if (cJSON_IsString(p)) { strncpy(s_nets[k].pass, p->valuestring, 63); s_nets[k].pass[63] = 0; }
        s_nets[k].prio = cJSON_IsNumber(pr) ? (uint8_t)pr->valueint : 0;
        s_nets[k].seq  = cJSON_IsNumber(sq) ? (uint32_t)sq->valuedouble : 0;
        if (s_nets[k].seq > s_net_seq) s_net_seq = s_nets[k].seq;
        k++;
    }
    s_net_n = k;
    cJSON_Delete(r);
    if (from_fallback && s_net_n > 0) save_networks();   // heal /cfg (+ any other tier) from the recovered copy
}

static void save_networks(void)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) return;
    cJSON_AddNumberToObject(r, "seq", s_net_seq);
    cJSON *arr = cJSON_AddArrayToObject(r, "nets");
    for (int i = 0; i < s_net_n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", s_nets[i].ssid);
        cJSON_AddStringToObject(o, "pass", s_nets[i].pass);
        cJSON_AddNumberToObject(o, "prio", s_nets[i].prio);
        cJSON_AddNumberToObject(o, "seq",  s_nets[i].seq);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!txt) return;
    int tiers = persist_doc(NETS_JSON, NETS_SD, "networks", txt);   // /cfg + NVS + SD, best-effort
    if (tiers == 0) ESP_LOGE(TAG, "save_networks: NO persistence tier available");
    free(txt);
}

static int net_find(const char *ssid)
{
    if (!ssid) return -1;
    for (int i = 0; i < s_net_n; i++) if (!strcmp(s_nets[i].ssid, ssid)) return i;
    return -1;
}

// Add or refresh a network and mark it most-recently-used. When the store is full, evict the
// least-preferred entry (lowest manual priority, then oldest). Called on every successful join.
static void net_remember(const char *ssid, const char *pass)
{
    load_networks();
    if (!ssid || !ssid[0]) return;
    int i = net_find(ssid);
    if (i < 0) {
        if (s_net_n < MAX_NETS) { i = s_net_n++; s_nets[i].prio = 0; }
        else {
            int worst = 0;
            for (int j = 1; j < s_net_n; j++)
                if (s_nets[j].prio < s_nets[worst].prio ||
                    (s_nets[j].prio == s_nets[worst].prio && s_nets[j].seq < s_nets[worst].seq)) worst = j;
            i = worst; s_nets[i].prio = 0;
        }
        strncpy(s_nets[i].ssid, ssid, 32); s_nets[i].ssid[32] = 0;
    }
    strncpy(s_nets[i].pass, pass ? pass : "", 63); s_nets[i].pass[63] = 0;
    s_nets[i].seq = ++s_net_seq;
    save_networks();
}


bool nucleo_setup_is_complete(void) { return load_config(); }
const char *nucleo_setup_device_name(void) { return s_name; }

// ---- wifi helpers ----------------------------------------------------------

static void wifi_ensure(void)
{
    if (s_wifi_ready) return;
    WIFI_TRY(esp_netif_init());
    if (esp_event_loop_create_default() == ESP_ERR_INVALID_STATE) { /* already created */ }
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    WIFI_TRY(esp_wifi_init(&cfg));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);      // persist creds in NVS, not on SD
    WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_APSTA));
#if NUCLEO_PRIVACY_RANDOM_STA_MAC
    // set_mac needs the iface stopped (we are, pre-start). Locally-administered, unicast random MAC.
    {
        uint32_t a = esp_random(), b = esp_random();
        uint8_t mac[6] = { (uint8_t)((a & 0xFC) | 0x02), (uint8_t)(a >> 8), (uint8_t)(a >> 16),
                           (uint8_t)(a >> 24), (uint8_t)b, (uint8_t)(b >> 8) };
        esp_wifi_set_mac(WIFI_IF_STA, mac);          // best-effort; ignore if a chip refuses it
    }
#endif
    WIFI_TRY(esp_wifi_start());
    if (!s_scan_lock)    s_scan_lock    = xSemaphoreCreateMutex();   // serialize scans (see s_scan_lock)
    if (!s_wifi_op_lock) s_wifi_op_lock = xSemaphoreCreateMutex();   // serialize join / best-known selection
    static bool sup_started = false;
    if (!sup_started) { sup_started = true;                          // background (re)join supervisor
        xTaskCreate(wifi_supervisor, "wifisup", 6144, NULL, tskIDLE_PRIORITY + 1, NULL);   // +2 KB: room for the net_trace FAT write on a silent-drop
    }
    s_wifi_ready = true;
}

// Scan and copy up to max unique SSIDs. Returns the count.
static int scan_networks(char out[][33], int max)
{
    wifi_ensure();
    // Scanning needs the STA interface active; we may currently be AP-only, so switch to
    // APSTA (keeps the AP up) before scanning.
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_scan_stop();
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi scan failed: %s (0x%x)", esp_err_to_name(err), err);
        return 0;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;
    wifi_ap_record_t *recs = malloc(20 * sizeof(wifi_ap_record_t));
    if (!recs) return 0; // out of memory
    esp_wifi_scan_get_ap_records(&num, recs);
    int count = 0;
    for (int i = 0; i < num && count < max; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < count; j++) if (!strcmp(out[j], ssid)) { dup = true; break; }
        if (!dup) { strncpy(out[count], ssid, 32); out[count][32] = '\0'; count++; }
    }
    free(recs);
    return count;
}

// Wait (up to ~8s) for a DHCP IP on the STA interface. Sets s_ip (empty on failure). Only ever runs
// in the background supervisor task now, so a slow/absent AP costs the retry loop a backoff tick, not
// the boot. 8s comfortably covers normal DHCP while keeping rescan+rejoin snappy when a net is gone.
static void wait_for_ip(void)
{
    s_ip[0] = '\0';
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    for (int t = 0; t < 40; t++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) {
            uint32_t a = ip.ip.addr;
            snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u",
                     (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
                     (unsigned)((a >> 16) & 0xff), (unsigned)((a >> 24) & 0xff));
            ESP_LOGI(TAG, "STA got IP %s", s_ip);
            return;
        }
    }
    ESP_LOGW(TAG, "STA: no IP (connection failed)");
}

// Connect as STA with explicit credentials, then wait for an IP.
static void connect_sta(const char *ssid, const char *pass)
{
    s_want_sta = true;                  // keep this link up (auto-reconnect on any drop)
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    WIFI_TRY(esp_wifi_set_config(WIFI_IF_STA, &wc));
    WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_wifi_connect();
    wait_for_ip();
    if (s_ip[0]) net_remember(ssid, pass);   // every successful join is added to the known list
}

// True iff the hotspot is ACTUALLY secured. WPA2-PSK needs an 8..63 char key; a 1..7 char password is
// invalid and the driver would fall back to OPEN. Every UI must read this, not s_ap_pass[0], so the
// screen never claims a password on an AP that is actually open.
static bool ap_secure(void) { return strlen(s_ap_pass) >= 8; }

// Fill per-device factory defaults for the hotspot on first use, and upgrade the old SHARED defaults.
// Security: the default password is RANDOM (hardware RNG), NOT derived from the MAC — this firmware is
// open source and the soft-AP broadcasts its MAC as the BSSID, so any MAC-based scheme would be publicly
// reproducible. The SSID gets a per-device suffix (not secret, just to avoid two units colliding). The
// freshly-minted default is persisted once (robust 3-tier save) and shown on the device screen, so it is
// stable across reboots and always discoverable by the owner.
static void ensure_default_ap_creds(void)
{
    bool changed = false;
    // Upgrade path: a device still on the old shared "nucleoos" / "NucleoOS-Setup" defaults gets fresh
    // per-device credentials. (A user who set their own values never matches these exact strings.)
    if (!strcmp(s_ap_pass, AP_PASS_LEGACY)) { s_ap_pass[0] = 0; ESP_LOGW(TAG, "upgrading shared default AP password -> per-device random"); }
    if (!strcmp(s_ap_ssid, AP_SSID_LEGACY))   s_ap_ssid[0] = 0;

    if (!s_ap_ssid[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
        changed = true;
    }
    if (!s_ap_pass[0]) {
        static const char AB[] = "abcdefghjkmnpqrstuvwxyz23456789";   // 30 chars, no ambiguous 0/O/1/I/l
        char p[13];
        for (int i = 0; i < 12; i++) p[i] = AB[esp_random() % (sizeof(AB) - 1)];   // ~59 bits entropy
        p[12] = 0;
        snprintf(s_ap_pass, sizeof s_ap_pass, "%s", p);              // 12 chars, WPA2-valid
        changed = true;
    }
    if (changed) { save_config(); ESP_LOGI(TAG, "hotspot default set: SSID '%s' (per-device)", s_ap_ssid); }
}

static void start_ap(void)
{
    s_want_sta = false;                 // AP mode: stop trying to reconnect as a client
    ensure_default_ap_creds();          // per-device SSID + random WPA2 password on first use (persists once)
    wifi_config_t wc = {0};
    strncpy((char *)wc.ap.ssid, s_ap_ssid, sizeof(wc.ap.ssid) - 1);
    bool secure = ap_secure();                                               // WPA2 needs >=8; else OPEN
    if (secure) strncpy((char *)wc.ap.password, s_ap_pass, sizeof(wc.ap.password) - 1);
    else if (s_ap_pass[0]) ESP_LOGW(TAG, "AP password too short (<8) — starting OPEN");
    wc.ap.max_connection = 2;
    wc.ap.authmode = secure ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.ap.pmf_cfg.capable = true;                                            // advertise 802.11w PMF (harden mgmt frames)
    WIFI_TRY(esp_wifi_set_config(WIFI_IF_AP, &wc));
    WIFI_TRY(esp_wifi_set_mode(WIFI_MODE_AP));
    strncpy(s_ssid, s_ap_ssid, sizeof(s_ssid) - 1);
}

// ---- non-blocking Wi-Fi control for the native Wi-Fi app --------------------
// These call into the (blocking) esp_wifi driver, so the app runs them on a worker task and shows a
// spinner. Scan results are cached here (the app reads them through the primitive getters below).
#define WSCAN_MAX 24
typedef struct { char ssid[33]; int rssi; uint8_t ch; uint8_t auth; } wscan_t;
static wscan_t s_wscan[WSCAN_MAX];
static int s_wscan_n = 0;

// Blocking active scan (~1-2 s). De-dupes by SSID (keeps the strongest), sorts by RSSI desc, caches.
int nucleo_setup_scan(void)
{
    wifi_ensure();
    if (s_scan_lock) xSemaphoreTake(s_scan_lock, portMAX_DELAY);   // serialize native-app + web scans
    // Scanning needs STA up, so we flip to APSTA. If we entered in pure-AP mode (intentional hotspot),
    // restore it on every exit: otherwise the STA radio stays powered after a one-off scan (wasted
    // battery — a first-class resource here). Callers that go on to join a net (connect_sta) re-set the
    // mode themselves, so the restore never fights them.
    wifi_mode_t prev = WIFI_MODE_NULL; esp_wifi_get_mode(&prev);
    esp_wifi_set_mode(WIFI_MODE_APSTA);          // need STA up to scan; keep the AP for the web UI
    esp_wifi_scan_stop();
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) { s_wscan_n = 0; if (prev == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP); if (s_scan_lock) xSemaphoreGive(s_scan_lock); return 0; }
    uint16_t num = 0; esp_wifi_scan_get_ap_num(&num);
    if (num > 40) num = 40;
    wifi_ap_record_t *recs = malloc((num ? num : 1) * sizeof(wifi_ap_record_t));
    if (!recs) { s_wscan_n = 0; if (prev == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP); if (s_scan_lock) xSemaphoreGive(s_scan_lock); return 0; }
    esp_wifi_scan_get_ap_records(&num, recs);
    s_wscan_n = 0;
    for (int i = 0; i < num && s_wscan_n < WSCAN_MAX; i++) {
        const char *ss = (const char *)recs[i].ssid;
        if (!ss[0]) continue;
        int dup = -1;
        for (int j = 0; j < s_wscan_n; j++) if (!strcmp(s_wscan[j].ssid, ss)) { dup = j; break; }
        if (dup >= 0) {
            if (recs[i].rssi > s_wscan[dup].rssi) { s_wscan[dup].rssi = recs[i].rssi; s_wscan[dup].ch = recs[i].primary; s_wscan[dup].auth = recs[i].authmode; }
            continue;
        }
        strncpy(s_wscan[s_wscan_n].ssid, ss, 32); s_wscan[s_wscan_n].ssid[32] = 0;
        s_wscan[s_wscan_n].rssi = recs[i].rssi; s_wscan[s_wscan_n].ch = recs[i].primary; s_wscan[s_wscan_n].auth = recs[i].authmode;
        s_wscan_n++;
    }
    free(recs);
    for (int i = 1; i < s_wscan_n; i++) {                 // insertion sort by RSSI desc (n<=24)
        wscan_t t = s_wscan[i]; int j = i;
        while (j > 0 && s_wscan[j - 1].rssi < t.rssi) { s_wscan[j] = s_wscan[j - 1]; j--; }
        s_wscan[j] = t;
    }
    if (prev == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_AP);   // pure-AP entry: don't leave STA powered
    if (s_scan_lock) xSemaphoreGive(s_scan_lock);
    return s_wscan_n;
}
int         nucleo_setup_scan_count(void)        { return s_wscan_n; }
const char *nucleo_setup_scan_ssid(int i)        { return (i >= 0 && i < s_wscan_n) ? s_wscan[i].ssid : ""; }
int         nucleo_setup_scan_rssi(int i)        { return (i >= 0 && i < s_wscan_n) ? s_wscan[i].rssi : 0; }
int         nucleo_setup_scan_channel(int i)     { return (i >= 0 && i < s_wscan_n) ? s_wscan[i].ch : 0; }
int         nucleo_setup_scan_secure(int i)      { return (i >= 0 && i < s_wscan_n) ? (s_wscan[i].auth != WIFI_AUTH_OPEN) : 0; }
// Human-readable security label for the web scanner. Falls through to "WPA2" for any future/unknown
// authmode so a secured network is never mislabelled "Open".
const char *nucleo_setup_scan_auth_label(int i)
{
    if (i < 0 || i >= s_wscan_n) return "";
    switch (s_wscan[i].auth) {
        case WIFI_AUTH_OPEN:            return "Open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI";
        case WIFI_AUTH_OWE:             return "OWE";
        default:                        return "WPA2";
    }
}

// Scan, then connect to the BEST in-range known network: highest manual priority first, then
// strongest RSSI. If the scan saw none of our saved nets (e.g. a hidden SSID), make one blind
// attempt on the most-recently-used saved network. Returns true once an IP is acquired. Serialized
// so a boot bring-up and the background supervisor never drive the radio at the same time.
static bool connect_best_known(void)
{
    load_networks();
    if (s_wifi_op_lock) xSemaphoreTake(s_wifi_op_lock, portMAX_DELAY);

    if (s_net_n == 0) {
        // Legacy device: networks.json not written yet, creds live only in NVS. Make one attempt with
        // the stored NVS config, then migrate the readable creds into the store so future joins use the
        // smart multi-network path below. Runs only in the supervisor task now (off the boot path), so
        // its blocking wait_for_ip() never stalls startup. Migrate ONLY a non-empty password: some IDF
        // builds return an empty one, and storing that would create a useless entry that later fails.
        bool ok = false;
        if (s_ssid[0]) {
            s_want_sta = true;
            esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_connect(); wait_for_ip();
            if (s_ip[0]) {
                wifi_config_t cur = {0};
                if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK && cur.sta.ssid[0] && cur.sta.password[0])
                    net_remember((char *)cur.sta.ssid, (char *)cur.sta.password);
                ok = true;
            }
        }
        if (s_wifi_op_lock) xSemaphoreGive(s_wifi_op_lock);
        return ok;
    }

    nucleo_setup_scan();                       // refresh s_wscan (RSSI-sorted), internally serialized

    int order[MAX_NETS], rssi[MAX_NETS], m = 0;
    for (int i = 0; i < s_net_n; i++) {
        int best = 0; bool seen = false, secure = false;   // strongest RSSI + security for this saved SSID
        for (int j = 0; j < s_wscan_n; j++)
            if (!strcmp(s_wscan[j].ssid, s_nets[i].ssid) && (!seen || s_wscan[j].rssi > best)) {
                best = s_wscan[j].rssi; secure = (s_wscan[j].auth != WIFI_AUTH_OPEN); seen = true; }
        // Never attempt a secured AP with no stored password: it can't succeed and connect_sta would
        // overwrite the NVS creds with an empty one, breaking a network that was working. Skip it so
        // the user can re-enter the password from the Wi-Fi app.
        if (seen && !(secure && !s_nets[i].pass[0])) { order[m] = i; rssi[m] = best; m++; }
    }
    for (int a = 1; a < m; a++) {              // insertion sort: priority desc, then RSSI desc
        int oi = order[a], ri = rssi[a], b = a;
        while (b > 0 && (s_nets[order[b-1]].prio < s_nets[oi].prio ||
               (s_nets[order[b-1]].prio == s_nets[oi].prio && rssi[b-1] < ri))) {
            order[b] = order[b-1]; rssi[b] = rssi[b-1]; b--;
        }
        order[b] = oi; rssi[b] = ri;
    }

    bool ok = false;
    for (int a = 0; a < m && a < 4 && !ok; a++) {     // try the top few in preference order
        int i = order[a];
        ESP_LOGI(TAG, "auto-join '%s' (prio %u, %d dBm)", s_nets[i].ssid, s_nets[i].prio, rssi[a]);
        connect_sta(s_nets[i].ssid, s_nets[i].pass);
        if (s_ip[0]) { strncpy(s_ssid, s_nets[i].ssid, sizeof(s_ssid)-1); s_ssid[sizeof(s_ssid)-1] = 0;
                       strncpy(s_mode, "sta", sizeof(s_mode)-1); save_config(); ok = true; }
    }
    if (!ok && m == 0) {                       // nothing visible matched: blind retry of the most recent
        int r = -1; for (int j = 0; j < s_net_n; j++)   // ...that actually has a stored password
            if (s_nets[j].pass[0] && (r < 0 || s_nets[j].seq > s_nets[r].seq)) r = j;
        if (r >= 0) {
            ESP_LOGI(TAG, "no saved net in range; blind retry '%s'", s_nets[r].ssid);
            connect_sta(s_nets[r].ssid, s_nets[r].pass);
            if (s_ip[0]) { strncpy(s_ssid, s_nets[r].ssid, sizeof(s_ssid)-1); s_ssid[sizeof(s_ssid)-1] = 0;
                           strncpy(s_mode, "sta", sizeof(s_mode)-1); save_config(); ok = true; }
        }
    }
    if (s_wifi_op_lock) xSemaphoreGive(s_wifi_op_lock);
    return ok;
}

// Append a one-line network event to /sd/net_trace.txt (best-effort, low-frequency: only a DETECTED
// silent link loss writes here). Makes "device stuck on a stale IP" diagnosable from the SD on a PC
// even with no serial and the web UI unreachable. No-op if no SD. Called only from the supervisor task
// (6 KB stack) so the FAT write has stack room.
static void net_trace(const char *what, const char *ip)
{
    FILE *f = fopen(NUCLEO_SD_MOUNT "/net_trace.txt", "a");
    if (!f) return;
    fprintf(f, "%ld %s ip=%s ssid=%s\n", (long)time(NULL), what,
            ip && ip[0] ? ip : "-", s_ssid[0] ? s_ssid : "-");
    fclose(f);
}

// True iff a client is currently associated with our soft-AP. A full-radio scan (channel hop) while
// someone is connected deauthenticates them AND aborts their in-flight WPA2 4-way handshake, which the
// client reports as a WRONG PASSWORD ("authentication required" / "incorrect password"). The supervisor
// checks this before scanning so an active hotspot user is never kicked to go hunting for a saved net.
static bool ap_has_clients(void)
{
    wifi_mode_t m;
    if (esp_wifi_get_mode(&m) != ESP_OK || (m != WIFI_MODE_AP && m != WIFI_MODE_APSTA)) return false;
    wifi_sta_list_t list;
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK && list.num > 0;
}

// Background Wi-Fi supervisor. While the user intends a client link (s_auto) but we have no IP,
// periodically rescan and (re)join the best known network — and keep the device reachable on its
// own AP between retries. The driver's on_wifi_event handles brief drops to the SAME AP; this
// recovers when that AP is gone (another saved net in range) and rejoins the home Wi-Fi after a
// fallback. Exponential backoff up to 60 s so we never scan in a tight loop.
static void wifi_supervisor(void *arg)
{
    TickType_t backoff = pdMS_TO_TICKS(8000);
    TickType_t next = xTaskGetTickCount();
    int link_miss = 0;                              // consecutive "association lost" polls while s_ip is set
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!s_auto) { backoff = pdMS_TO_TICKS(8000); continue; }
        // We THINK we have a link (s_ip set): verify it is REALLY alive. The STA can lose the AP without
        // WIFI_EVENT_STA_DISCONNECTED ever reaching us (beacon loss under heap pressure, the event dropped
        // on a starved queue) -> s_ip stays stale, the launcher shows a dead IP, the web server is up but
        // unreachable, and nothing recovers until a manual reboot. Poll the association; on repeated
        // failure, log it (RAM ring -> /api/logs, and /sd/net_trace.txt) and force a reconnect — the
        // disconnect handler reassociates (s_want_sta) and GOT_IP refreshes s_ip.
        if (s_ip[0]) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                link_miss = 0;
            } else if (++link_miss >= 3) {           // ~6 s of "not associated" while we still hold an IP
                ESP_LOGW(TAG, "STA link silently lost (stale ip=%s) — forcing reconnect", s_ip);
                net_trace("silent-drop", s_ip);
                link_miss = 0; s_ip[0] = '\0';
                esp_wifi_disconnect();
            }
            backoff = pdMS_TO_TICKS(8000);
            continue;
        }
        // Someone is actively using our fallback hotspot: do NOT scan (it would deauth them and break
        // their WPA2 handshake -> "incorrect password"). Defer the STA retry until they disconnect; the
        // driver keeps the AP alive in the meantime, and retries resume automatically once they leave.
        if (ap_has_clients()) { next = xTaskGetTickCount() + pdMS_TO_TICKS(8000); continue; }
        if ((int32_t)(xTaskGetTickCount() - next) < 0) continue;
        if (connect_best_known()) {
            backoff = pdMS_TO_TICKS(8000);
        } else {
            if (strcmp(s_mode, "ap")) { start_ap(); strncpy(s_mode, "ap", sizeof(s_mode)-1); }   // stay reachable
            if (backoff < pdMS_TO_TICKS(60000)) backoff *= 2;
        }
        next = xTaskGetTickCount() + backoff;
    }
}

// Join an SSID+password at runtime (blocking ~ up to 12 s for DHCP). Persists into the known-network
// store on success (so the device auto-rejoins it later). Returns true once an IP is acquired.
bool nucleo_setup_join(const char *ssid, const char *pass)
{
    if (s_wifi_op_lock) xSemaphoreTake(s_wifi_op_lock, portMAX_DELAY);
    s_auto = true;
    // Reconnecting to a saved network without re-typing the password: if none was supplied, reuse
    // the stored one (an open network simply has an empty stored password).
    const char *use_pass = pass ? pass : "";
    if (!use_pass[0]) { load_networks(); int idx = net_find(ssid); if (idx >= 0 && s_nets[idx].pass[0]) use_pass = s_nets[idx].pass; }
    esp_wifi_disconnect();
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = 0;
    connect_sta(ssid, use_pass);               // remembers the net on success
    bool ok = s_ip[0] != 0;
    if (ok) { strncpy(s_mode, "sta", sizeof(s_mode) - 1); save_config(); }
    if (s_wifi_op_lock) xSemaphoreGive(s_wifi_op_lock);
    return ok;
}
void nucleo_setup_start_ap(void)  { s_auto = false; strncpy(s_mode, "ap", sizeof(s_mode) - 1); start_ap(); save_config(); }

// Turn the Access Point OFF and return to client (STA) mode — rejoin the best known network. The AP
// toggle in Settings MUST call this, NOT apply_network() directly: apply_network() only brings STA up
// when s_mode is ALREADY "sta" (otherwise it (re)starts the AP), so toggling "off" while s_mode=="ap"
// just restarted the hotspot — "AP won't turn off". Flip the mode to "sta" FIRST, persist it, then
// apply: connect_best_known() joins a known Wi-Fi. If none is in range, apply_network() falls back to
// the AP (it never leaves the device fully offline) — but the persisted mode stays "sta", so the
// supervisor rejoins automatically the moment a known network reappears.
void nucleo_setup_stop_ap(void)
{
    strncpy(s_mode, "sta", sizeof(s_mode) - 1);
    save_config();
    nucleo_setup_apply_network();
}

// Forget ALL saved networks and drop to the hotspot.
void nucleo_setup_forget(void)
{
    load_networks();
    s_net_n = 0; s_net_seq = 0; save_networks();                    // drop the whole known list
    wifi_config_t wc = {0}; esp_wifi_set_config(WIFI_IF_STA, &wc);   // wipe the saved creds from NVS
    s_auto = false; s_want_sta = false; esp_wifi_disconnect(); s_ip[0] = 0; s_ssid[0] = 0;
    strncpy(s_mode, "ap", sizeof(s_mode) - 1); start_ap(); save_config();
}

// ---- known-networks public API (native Wi-Fi app + web manager) ------------
int         nucleo_setup_net_count(void)     { load_networks(); return s_net_n; }
const char *nucleo_setup_net_ssid(int i)     { load_networks(); return (i >= 0 && i < s_net_n) ? s_nets[i].ssid : ""; }
int         nucleo_setup_net_priority(int i)  { load_networks(); return (i >= 0 && i < s_net_n) ? s_nets[i].prio : 0; }
void        nucleo_setup_net_set_priority(const char *ssid, int prio)
{
    load_networks(); int i = net_find(ssid);
    if (i >= 0) { s_nets[i].prio = (uint8_t)(prio < 0 ? 0 : prio > 9 ? 9 : prio); save_networks(); }
}
bool nucleo_setup_net_is_known(const char *ssid) { if (!ssid || !ssid[0]) return false; load_networks(); return net_find(ssid) >= 0; }
bool nucleo_setup_net_has_password(const char *ssid)
{
    if (!ssid || !ssid[0]) return false;
    load_networks(); int i = net_find(ssid);
    return i >= 0 && s_nets[i].pass[0] != 0;
}

// Forget one saved network. If it is the one we are currently on, leave it and auto-join the next
// best known network (or fall back to the hotspot).
void nucleo_setup_forget_ssid(const char *ssid)
{
    if (!ssid || !ssid[0]) return;
    load_networks();
    int i = net_find(ssid);
    if (i < 0) return;
    for (int j = i; j < s_net_n - 1; j++) s_nets[j] = s_nets[j + 1];
    s_net_n--; save_networks();
    if (!strcmp(s_ssid, ssid) && !strcmp(s_mode, "sta")) {
        esp_wifi_disconnect(); s_ip[0] = 0;
        if (!(s_auto && connect_best_known())) { start_ap(); strncpy(s_mode, "ap", sizeof(s_mode) - 1); }
    }
}

// Rescan and (re)join the best known network right now (blocking). Drops to the hotspot if none.
void nucleo_setup_reconnect_best(void)
{
    s_auto = true;
    if (!connect_best_known()) { start_ap(); strncpy(s_mode, "ap", sizeof(s_mode) - 1); }
}
void        nucleo_setup_set_device_name(const char *n) { if (n && n[0]) { strncpy(s_name, n, sizeof(s_name) - 1); s_name[sizeof(s_name) - 1] = 0; save_config(); } }
const char *nucleo_setup_ap_ssid(void)           { return s_ap_ssid; }
const char *nucleo_setup_ap_pass(void)           { return s_ap_pass; }
bool        nucleo_setup_ap_secure(void)         { return ap_secure(); }
// The hotspot is the user's DELIBERATE choice only when we are NOT auto-joining a client link. During a
// STA-intended boot with a saved network out of range, apply_network() runs the AP as a reachable
// fallback and sets s_mode="ap" in RAM (s_auto stays true) — so a raw s_mode=="ap" test makes the
// Settings toggle read "ON" and bounce back to "ON" when tapped off. The toggle must read THIS instead,
// so it honestly shows OFF while the device is really trying to be a client.
bool        nucleo_setup_ap_intended(void)       { return !s_auto && !strcmp(s_mode, "ap"); }
void nucleo_setup_persist_status(nucleo_persist_status_t *out)
{
    if (!out) return;
    out->cfg_ok = s_cfg_ok; out->nvs_ok = s_nvs_ok; out->sd_ok = s_sd_ok;
    out->tiers_ok = s_tiers_ok;   // -1 = no save yet this boot
}

// Edit the hotspot credentials. SSID must be non-empty; password is "" (open) or 8..63 (WPA2).
// Persists immediately; the change takes effect on the next AP (re)start.
void nucleo_setup_set_ap_ssid(const char *s)
{
    if (!s || !s[0]) return;
    strncpy(s_ap_ssid, s, sizeof(s_ap_ssid) - 1); s_ap_ssid[sizeof(s_ap_ssid) - 1] = 0;
    save_config();
    if (!strcmp(s_mode, "ap")) start_ap();           // apply live if the hotspot is up
}
void nucleo_setup_set_ap_pass(const char *p)
{
    if (!p) return;
    // Enforce the WPA2 invariant AT the setter, not only in UI callers: "" = intentional OPEN, but a
    // 1..7 char password is invalid and would make start_ap() silently launch an OPEN hotspot while the
    // UI still shows a password. Reject it here so no future caller (web API, ANIMA action) can ship an
    // accidentally-open AP. (esp. see ap_secure().)
    size_t len = strlen(p);
    if (len > 0 && len < 8) { ESP_LOGW(TAG, "AP password rejected: %u chars (need 0 or 8..63)", (unsigned)len); return; }
    strncpy(s_ap_pass, p, sizeof(s_ap_pass) - 1); s_ap_pass[sizeof(s_ap_pass) - 1] = 0;
    save_config();
    if (!strcmp(s_mode, "ap")) start_ap();
}

// ---- wizard ----------------------------------------------------------------

// Choose AP or join an existing Wi-Fi (scan/pick/password); sets mode and brings the
// radio up. Reusable from the first-run wizard AND the home menu (reconfigure anytime).
void nucleo_setup_choose_network(void)
{
    const char *modes[] = { "Join a Wi-Fi network", "Create an Access Point" };
    int m = nucleo_ui_menu("Network", modes, 2);
    if (m < 0) return;                     // back: keep current network
    if (m == 0) {                          // join an existing network
        if (s_ip[0] && !strcmp(s_mode, "sta")) {
            char title[32];
            snprintf(title, sizeof(title), "Net: %s", s_ssid);
            const char *warn_opts[] = { "Keep current Wi-Fi", "Disconnect & Scan new" };
            int w = nucleo_ui_menu(title, warn_opts, 2);
            if (w != 1) return; // return to launcher
            esp_wifi_disconnect();
            s_ip[0] = '\0';
            vTaskDelay(pdMS_TO_TICKS(100)); // allow stack to process disconnect
        }

        const char *scan_msg[] = { "Scanning for Wi-Fi", "Please wait..." };
        nucleo_ui_home("Network", scan_msg, 2);
        char ssids[20][33];
        int n = scan_networks(ssids, 20);
        if (n > 0) {
            const char *items[20];
            for (int i = 0; i < n; i++) items[i] = ssids[i];
            int pick = nucleo_ui_menu("Choose Wi-Fi", items, n);
            if (pick >= 0) {
                strncpy(s_ssid, ssids[pick], sizeof(s_ssid) - 1);
                char pass[65] = {0};
                nucleo_ui_input("Wi-Fi password", pass, sizeof(pass), 1);
                const char *wait[] = { "Disabling AP...", "Connecting to Wi-Fi:", s_ssid };
                nucleo_ui_home("Network", wait, 3);
                connect_sta(s_ssid, pass);           // remembers the net on success
                if (s_ip[0]) {                       // connected: got an IP
                    s_auto = true;
                    strncpy(s_mode, "sta", sizeof(s_mode) - 1);
                    save_config();
                    char u[32]; snprintf(u, sizeof(u), "http://%s/", s_ip);
                    const char *ok[] = { "Connected!", s_ssid, u };
                    nucleo_ui_message("Network", ok, 3);
                    return;
                }
                const char *fail[] = { "Could not connect.", "Check the password." };
                nucleo_ui_message("Network", fail, 2);
                return; // Return instead of falling through to AP
            } else {
                return; // User aborted picking Wi-Fi
            }
        } else {
            const char *none[] = { "No networks found." };
            nucleo_ui_message("Network", none, 1);
            return; // Return instead of falling through to AP
        }
    }
    // Switch to Access Point: turn the Wi-Fi client off, bring the AP up, and confirm.
    const char *sw[] = { "Disabling Wi-Fi...", "Starting Access Point" };
    nucleo_ui_home("Network", sw, 2);
    s_auto = false;
    strncpy(s_mode, "ap", sizeof(s_mode) - 1);
    start_ap();
    save_config();
    char apline[80]; snprintf(apline, sizeof apline, "%s / %s", s_ap_ssid, ap_secure() ? s_ap_pass : "open");
    const char *apok[] = { "Access Point ready", apline, "http://192.168.4.1/" };
    nucleo_ui_message("Network", apok, 3);
}

static void build_info(char *l1, char *l2, char *l3)
{
    char base[40];
    if (!strcmp(s_mode, "sta") && s_ssid[0]) {
        snprintf(l1, 48, "Wi-Fi: %s", s_ssid);
        // Prefer the real IP (always works); fall back to mDNS name if unknown.
        if (s_ip[0]) snprintf(base, sizeof(base), "http://%s", s_ip);
        else snprintf(base, sizeof(base), "http://%s.local", s_name);
    } else {
        snprintf(l1, 48, "AP: %s (%s)", s_ap_ssid, ap_secure() ? s_ap_pass : "open");
        snprintf(base, sizeof(base), "http://192.168.4.1");
    }
    snprintf(l2, 48, "Open: %s/", base);
    snprintf(l3, 52, "Win app: %s/downloads/", base);
}

// ---- first-run wizard steps ------------------------------------------------

// Step 1 — LANGUAGE, the very first thing on a fresh device. Applied INSTANTLY (nucleo_i18n_set_en),
// so every following wizard screen — and the whole OS — is already painted in the chosen language.
// English is the default (also on back/dismiss), matching the OS-wide English-first default.
static void wizard_language(void)
{
    const char *opts[] = { "English", "Italiano" };
    int m = nucleo_ui_menu("Language / Lingua", opts, 2);
    nucleo_i18n_set_en(m != 1);   // index 1 = Italiano; anything else (incl. back) = English
}

// Step 3 — NETWORK, and it is BYPASSABLE. "Connect to Wi-Fi" runs the scan/join; "Skip" (and any
// join failure / no networks / back) falls through to the Access Point so the device is ALWAYS
// reachable, then tells the user exactly how to connect (SSID + password + URL).
static void wizard_network(void)
{
    const char *opts[] = { TR("Connetti a una rete Wi-Fi", "Connect to a Wi-Fi network"),
                           TR("Salta - usa un Access Point", "Skip - use an Access Point") };
    int m = nucleo_ui_menu(TR("Rete", "Network"), opts, 2);
    if (m == 0) {
        const char *scan_msg[] = { TR("Ricerca reti Wi-Fi", "Scanning for Wi-Fi"), TR("Attendere...", "Please wait...") };
        nucleo_ui_home(TR("Rete", "Network"), scan_msg, 2);
        char ssids[20][33];
        int n = scan_networks(ssids, 20);
        if (n > 0) {
            const char *items[20];
            for (int i = 0; i < n; i++) items[i] = ssids[i];
            int pick = nucleo_ui_menu(TR("Scegli la rete", "Choose Wi-Fi"), items, n);
            if (pick >= 0) {
                strncpy(s_ssid, ssids[pick], sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid) - 1] = 0;
                char pass[65] = {0};
                nucleo_ui_input(TR("Password Wi-Fi", "Wi-Fi password"), pass, sizeof(pass), 1);
                const char *wait[] = { TR("Connessione a:", "Connecting to:"), s_ssid };
                nucleo_ui_home(TR("Rete", "Network"), wait, 2);
                connect_sta(s_ssid, pass);                 // remembers the net on success
                if (s_ip[0]) {
                    s_auto = true;
                    strncpy(s_mode, "sta", sizeof(s_mode) - 1);
                    save_config();
                    char u[40]; snprintf(u, sizeof u, "http://%s/", s_ip);
                    const char *ok[] = { TR("Connesso!", "Connected!"), s_ssid, TR("Apri nel browser:", "Open in your browser:"), u };
                    nucleo_ui_message(TR("Rete", "Network"), ok, 4);
                    return;                                // STA up — network step done
                }
                const char *fail[] = { TR("Connessione fallita.", "Could not connect."), TR("Avvio Access Point.", "Starting Access Point.") };
                nucleo_ui_message(TR("Rete", "Network"), fail, 2);   // -> fall through to AP
            }
            // pick < 0 (back): fall through to AP
        } else {
            const char *none[] = { TR("Nessuna rete trovata.", "No networks found."), TR("Avvio Access Point.", "Starting Access Point.") };
            nucleo_ui_message(TR("Rete", "Network"), none, 2);       // -> fall through to AP
        }
    }
    // Skip, or Wi-Fi not joined: bring the Access Point up and INFORM the user how to reach the device.
    s_auto = false;
    strncpy(s_mode, "ap", sizeof(s_mode) - 1);
    start_ap();                                            // mints the per-device SSID + random password
    save_config();
    char ssidline[56]; snprintf(ssidline, sizeof ssidline, "Wi-Fi: %s", s_ap_ssid);
    char passline[80]; snprintf(passline, sizeof passline, "%s %s", TR("Password:", "Password:"), ap_secure() ? s_ap_pass : TR("(aperta)", "(open)"));
    const char *info[] = { TR("Collega PC/telefono al Wi-Fi:", "Connect your PC/phone to Wi-Fi:"), ssidline, passline, "http://192.168.4.1/" };
    nucleo_ui_message(TR("Access Point attivo", "Access Point ready"), info, 4);
}

void nucleo_setup_run(void)
{
    wizard_language();                                     // step 1: language first, applied live

    const char *welcome[] = { TR("Benvenuto in NucleoOS.", "Welcome to NucleoOS."), "",
                              TR("Configuriamo il dispositivo.", "Let's set up your device."),
                              TR("Premi Invio per iniziare.", "Press Enter to begin.") };
    nucleo_ui_message("NucleoOS - Setup", welcome, 4);     // step 2: welcome, in the chosen language

    wizard_network();                                      // step 3: Wi-Fi (bypassable) / AP fallback

    nucleo_ui_input(TR("Nome dispositivo", "Device name"), s_name, sizeof(s_name), 0);   // step 4: name
    if (!s_name[0]) strncpy(s_name, "nucleo-01", sizeof(s_name) - 1);
    s_complete = true;                                     // the wizard finished -> mark setup complete NOW
    save_config();                                         // persists complete:true so the wizard never runs again

    char l1[48], l2[48], l3[52];
    build_info(l1, l2, l3);
    const char *done[] = { TR("Configurazione completata!", "Setup complete!"), l1, l2, l3 };
    nucleo_ui_message(TR("Tutto pronto", "All set"), done, 4);   // step 5: done
}

// The launcher keeps the app foregrounded until Esc; on_enter just sets the hint and the
// framework calls on_draw (nucleo_setup_show_home) to paint the connection info. Esc exits.
static void info_app_enter(void)
{
    nucleo_app_set_hint("esc back");
}

void nucleo_setup_register_apps(void)
{
    static const nucleo_app_def_t app_info = {
        "device", "Device Info", "System", "Network details and web address",
        'i', 0x2D7F, info_app_enter, NULL, NULL, nucleo_setup_show_home, NULL
    };
    nucleo_app_register(&app_info);
    // The "theme" app now lives in nucleo_app/app_theme.cpp — a proper on_draw-based picker
    // registered via nucleo_register_theme(). The old stub here drove the BLOCKING nucleo_ui_menu()
    // from on_enter with on_draw == NULL, which does not render inside the app run-loop, so the
    // app opened to an empty screen. Removed in favour of the framework app.
    // The Wi-Fi app itself is the modern tabbed app_wifi.cpp (Connect group), registered with the
    // other built-ins — the old blocking "network" wizard app was removed (it starved the app loop).
}

void nucleo_setup_suspend(void)
{
    s_want_sta = false;                  // stop the auto-reconnect loop (restored by apply_network)
    s_auto = false;                      // pause the background (re)join supervisor too
}

// Restore the last NTP-synced timestamp persisted to SD. Called before WiFi starts so the
// device shows a reasonable time even before it connects (or if NTP is unreachable).
static void restore_saved_time(void)
{
    if (s_time_synced) return;
    char *buf = slurp(NUCLEO_SD_MOUNT "/system/time.json");
    if (!buf) return;
    const char *p = strstr(buf, "\"t\":");
    if (p) {
        time_t t = (time_t)atol(p + 4);
        if (t > 1640000000) {   // sanity: must be post-2022
            struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Restored saved time from SD: %lu (approx)", (unsigned long)t);
        }
    }
    free(buf);
}

// Set the device clock from a browser push. Ignored if NTP has already synced.
// Validates the range (2022..2100) to reject obviously wrong values.
void nucleo_setup_set_time(time_t t)
{
    if (s_time_synced) return;
    if (t < 1640000000 || t > 4102444800LL) return;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_time_synced = true;
    ESP_LOGI(TAG, "Clock set from browser push: %lu", (unsigned long)t);
}

esp_err_t nucleo_setup_apply_network(void)
{
    load_config();
    load_networks();
    restore_saved_time();   // best-effort approximate time before WiFi/NTP
    wifi_ensure();
    // Restore the default STA modem-sleep here, the common chokepoint every network bring-up passes
    // through. A Wi-Fi attack app (deauth/beacon flood) sets WIFI_PS_NONE for max injection throughput
    // and exits via this function; without this the radio would stay at the higher PS_NONE idle draw
    // until reboot. Idempotent on a normal boot (MIN_MODEM is already the IDF STA default).
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (!strcmp(s_mode, "sta") && (s_net_n > 0 || s_ssid[0])) {
        // Client link intended — but DO NOT block the boot waiting for it. The old synchronous join
        // stalled the whole UI + web server for up to ~12 s per saved network (≈14 s with nothing in
        // range, far worse with several known nets that fail) before the splash even cleared. Instead:
        // bring the setup AP up NOW (so the device + web UI are reachable instantly) and hand the first
        // scan+join to the background wifi_supervisor, which already runs the exact same retry/backoff
        // path used for every later reconnect. It fires its first attempt within ~2 s; on success the
        // GOT_IP handler flips mode→"sta" and refreshes the SSID, so the header switches from "offline"
        // to the live network on its own. Net result: instant boot AND the same auto-join behaviour.
        s_auto = true;                       // intend a client link: supervisor keeps (re)joining
        ESP_LOGI(TAG, "Wi-Fi STA intended (%d saved network(s)) — joining in background", s_net_n);
        start_ap();                          // reachable immediately; supervisor upgrades us to STA
        strncpy(s_mode, "ap", sizeof(s_mode) - 1);   // RAM-only: header reads "offline" until the join lands
    } else {
        s_auto = false;
        start_ap();
        ESP_LOGI(TAG, "started Access Point '%s'", s_ap_ssid);
    }
    return ESP_OK;
}

// ---- network status getters (consumed by /api/status) ----------------------
const char *nucleo_setup_mode(void) { return s_mode; }   // "sta" | "ap"
const char *nucleo_setup_ssid(void) { return s_ssid; }
const char *nucleo_setup_ip(void)   { return s_ip; }     // STA IP, "" if none
bool nucleo_setup_time_synced(void) { return s_time_synced; }  // true once NTP has set the clock

// Live link quality of the joined AP (the NETWORK watch face). rssi is dBm (negative; 0 = not
// associated); channel is the primary 2.4 GHz channel. Both come straight from the Wi-Fi driver.
int nucleo_setup_rssi(void)    { wifi_ap_record_t ap; return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0; }
int nucleo_setup_channel(void) { wifi_ap_record_t ap; return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.primary : 0; }

// The mini-OS "home" screen shown after setup: how to connect + where to get the app.
void nucleo_setup_show_home(void)
{
    static char l1[48], l2[48], l3[52];
    char base[40];
    if (!strcmp(s_mode, "sta") && s_ssid[0]) {
        snprintf(l1, sizeof(l1), "Wi-Fi: %s", s_ssid);
        // Prefer the real IP (always reachable); fall back to mDNS only if unknown.
        if (s_ip[0]) snprintf(base, sizeof(base), "http://%s", s_ip);
        else         snprintf(base, sizeof(base), "http://%s.local", s_name);
    } else {
        snprintf(l1, sizeof(l1), "AP: %s (%s)", s_ap_ssid, ap_secure() ? s_ap_pass : "open");
        snprintf(base, sizeof(base), "http://192.168.4.1");
    }
    snprintf(l2, sizeof(l2), "Open: %s/", base);
    snprintf(l3, sizeof(l3), "Win app: %s/downloads/", base);
    const char *lines[] = { l1, l2, "", l3 };
    nucleo_ui_home("NucleoOS", lines, 4);
}

// Fast boot network start — returns in < 200 ms with no blocking connect.
// STA mode: lets the background supervisor handle the join (s_auto=true, wakes at t+2s).
// AP mode: identical to apply_network().
// WiFi mode transitions are left entirely to connect_sta()/start_ap() — no mode changes here.
void nucleo_setup_fast_start(void)
{
    load_config();
    load_networks();
    restore_saved_time();
    wifi_ensure();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    if (!strcmp(s_mode, "sta") && (s_net_n > 0 || s_ssid[0])) {
        s_auto = true;
        ESP_LOGI(TAG, "fast start: STA supervisor armed (%d nets known)", s_net_n);
    } else {
        s_auto = false;
        start_ap();
        ESP_LOGI(TAG, "fast start: AP-only '%s'", s_ap_ssid);
    }
}

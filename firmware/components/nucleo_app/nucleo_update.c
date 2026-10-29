// nucleo_update.c — native release-update engine. See nucleo_update.h for the contract and
// update_policy.c (host-gated) for every decision rule. Two one-shot workers, never resident:
//   check task    ~30-byte GET of version.json -> NVS (throttled 24h; silent on any failure)
//   install task  streaming GET of the app-only OTA image -> esp_ota_write, SHA-256 verified
// Both ride the heavy-work arbiter and the same heap gates as every other TLS touch: this chip
// has no PSRAM and the fetch must never be the thing that OOMs it.
#include "nucleo_update.h"
#include "update_policy.h"
#include "nucleo_notify.h"
#include "nucleo_i18n.h"
#include "nucleo_arb.h"
#include "nucleo_guest.h"   // hosted by M5Launcher: the next OTA slot is another app's -> no self-OTA

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/sha256.h"
#include "fwsig.h"           // the image must carry a release signature (docs/release-signing.md)

static const char *TAG = "nucupd";

// Same host the web flasher/updater uses: GitHub Release assets send no CORS headers for the
// browser, and for the DEVICE the win is different but real — version.json is ~30 bytes where
// the GitHub API answer is tens of KB of JSON this chip would have to buffer and parse.
#define UPD_BASE        "https://indecenti.github.io/NucleoOs/"
#define UPD_VERSION_URL UPD_BASE "version.json"
#define UPD_SUMS_URL    UPD_BASE "SHA256SUMS"
#define UPD_BIN_URL     UPD_BASE "nucleoos-latest-ota.bin"
#define UPD_BIN_NAME    "nucleoos-latest-ota.bin"

#define UPD_CHECK_INTERVAL_S (24u * 3600u)

// ── five-language literal pick (Font0 ASCII, flash literals — zero RAM; pomodoro pattern) ──────
static const char *UT(const char *it, const char *en, const char *es, const char *fr, const char *de)
{
    const char *l = nucleo_i18n_lang();
    switch (l[0]) {
        case 'i': return it;
        case 'e': return l[1] == 's' ? es : en;
        case 'f': return fr;
        case 'd': return de;
        default:  return en;
    }
}

// ── state ──────────────────────────────────────────────────────────────────────────────────────
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static nucleo_update_state_t s_st;          // ~100 B .bss — well under the boot-RAM radar
static volatile bool s_task_alive = false;  // one worker at a time (check OR install)
static bool s_notified_this_boot = false;

// Claim the single worker slot ATOMICALLY: the boot check (kicked ~60 s in) and a user-triggered
// check/install could otherwise both read s_task_alive==false and both spawn. The check-and-set
// runs inside the same critical section that guards the state, so exactly one caller wins.
static bool try_claim_task(void)
{
    bool won = false;
    portENTER_CRITICAL(&s_mux);
    if (!s_task_alive) { s_task_alive = true; won = true; }
    portEXIT_CRITICAL(&s_mux);
    return won;
}

static nvs_handle_t s_nvs;
static bool s_nvs_ok = false, s_nvs_tried = false;

static void st_set_phase(upd_phase_t ph, const char *err)
{
    portENTER_CRITICAL(&s_mux);
    s_st.phase = ph;
    if (err) { strncpy(s_st.err, err, sizeof(s_st.err) - 1); s_st.err[sizeof(s_st.err) - 1] = 0; }
    else s_st.err[0] = 0;
    portEXIT_CRITICAL(&s_mux);
}
static void st_progress(int recv, int total)
{
    portENTER_CRITICAL(&s_mux);
    s_st.recv_kb = recv / 1024;
    s_st.total_kb = total > 0 ? total / 1024 : 0;
    s_st.pct = total > 0 ? (int)((int64_t)recv * 100 / total) : -1;
    portEXIT_CRITICAL(&s_mux);
}

// Lazy NVS open. NO lock on purpose: nvs_open does flash I/O, which must never run inside a
// spinlock. Safe by an ordering invariant instead — the FIRST caller is always
// nucleo_update_dialog_pending(), run synchronously on the main task at boot BEFORE the ~60 s
// background check task can exist. Keep it that way: never call an nvs_* path from a new context
// that could race the very first nvs_ready() before the boot dialog check has run.
static bool nvs_ready(void)
{
    if (!s_nvs_tried) {
        s_nvs_tried = true;
        s_nvs_ok = (nvs_open("nucupd", NVS_READWRITE, &s_nvs) == ESP_OK);
        if (s_nvs_ok) {                      // warm the state's latest-tag mirror once
            size_t len = sizeof(s_st.latest);
            if (nvs_get_str(s_nvs, "latest", s_st.latest, &len) != ESP_OK) s_st.latest[0] = 0;
        }
    }
    return s_nvs_ok;
}
static void nvs_get_string(const char *key, char *out, size_t cap)
{
    out[0] = 0;
    if (!nvs_ready()) return;
    size_t len = cap;
    if (nvs_get_str(s_nvs, key, out, &len) != ESP_OK) out[0] = 0;
}

// ── tiny HTTPS GET (weather-get pattern: heap gate + arbiter outside, cleanup always) ─────────
// Fills buf (NUL-terminated), returns body length, -1 (network / HTTP failure) or -2 (the heap can't fit
// a TLS handshake right now). Caller holds the arbiter token.
#define UPD_TLS_MIN_BLOCK (42 * 1024)
// One screen line (Font0, ASCII): the honest reason a check can't run — and where it does work.
#define LOWRAM_IT "Poca RAM: controlla dal browser"
#define LOWRAM_EN "Low RAM: check from the browser"
#define LOWRAM_ES "Poca RAM: comprueba en navegador"
#define LOWRAM_FR "RAM faible: verifiez via navigateur"
#define LOWRAM_DE "Wenig RAM: im Browser pruefen"
static int small_get(const char *url, char *buf, size_t cap)
{
    // HARD heap gate. An outbound TLS handshake to GitHub Pages needs a large contiguous block; below
    // this it does NOT fail cleanly — mbedTLS can OOM mid-handshake and STALL, holding the heap. On
    // this no-PSRAM chip the largest block is ~30 KB even at its best (fresh boot, canvas freed), so
    // in practice this gate refuses the fetch rather than risk a stall. The reliable release check is
    // the browser/web path; the device never gambles its stability on a handshake that may not fit.
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (largest < UPD_TLS_MIN_BLOCK) {
        ESP_LOGW(TAG, "heap too low for TLS (largest=%u, need >=%u) — skipping fetch", (unsigned)largest, (unsigned)UPD_TLS_MIN_BLOCK);
        return -2;   // not a network failure: the caller says "low RAM, use the browser", never "offline?"
    }
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach,
        .max_redirection_count = 3,     // Pages assets don't redirect today; bound it explicitly anyway
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    int n = -1;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        if (esp_http_client_get_status_code(cli) == 200) {
            int total = 0, r;
            while (total < (int)cap - 1 &&
                   (r = esp_http_client_read(cli, buf + total, (int)cap - 1 - total)) > 0) total += r;
            buf[total] = 0;
            n = total;
        }
    }
    esp_http_client_cleanup(cli);
    return n;
}

static bool wifi_sta_connected(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

// The web-to-native bridge: the browser CAN do HTTPS to GitHub, the device can't. So the browser
// writes the latest release tag to SD (/system/config/update.json) and the native side reads it here
// — zero device-side TLS. This is the reliable replacement for the (removed) on-device fetch.
#define UPD_SD_FILE  "/sd/system/config/update.json"
static bool read_sd_latest(char *tag, size_t cap, char *type, size_t tcap)
{
    FILE *f = fopen(UPD_SD_FILE, "rb");
    if (!f) return false;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (type && tcap) upd_extract_type(buf, type, tcap);   // "type" bridged by the browser (default patch)
    return upd_extract_tag(buf, tag, cap);   // parses {"tag":"vX.Y.Z", ...}
}

// ── public queries ─────────────────────────────────────────────────────────────────────────────
// Whichever source has learned a release tag: the browser bridge (SD file — richer, carries notes)
// if a browser has ever visited this device online, else the native boot/background check's own
// NVS-cached result (nvs_ready() warms s_st.latest from it). Either path alone is enough to offer
// an update — this device never NEEDS a browser to have visited first.
// The NEWER of the two wins: the SD file is written by a browser only when it learned a newer release, and
// nothing ever rewrites it after the device updated — preferring it blindly meant a stale v0.7.1 file hid a
// v0.7.2 the device's own check had already learned ("up to date" while an update existed).
static bool best_known_latest_typed(char *out, size_t cap, bool *security)
{
    char sd[24] = "", nv[24] = "", sdtype[16] = "patch", nvtype[16] = "patch";
    bool have_sd = read_sd_latest(sd, sizeof sd, sdtype, sizeof sdtype) && upd_parse_semver(sd, NULL);   // junk loses
    nvs_ready();
    portENTER_CRITICAL(&s_mux);
    strncpy(nv, s_st.latest, sizeof nv - 1); nv[sizeof nv - 1] = 0;
    portEXIT_CRITICAL(&s_mux);
    nvs_get_string("ltype", nvtype, sizeof nvtype);
    bool use_nv = !have_sd || (nv[0] && upd_cmp(nv, sd) > 0);
    strncpy(out, use_nv ? nv : sd, cap - 1); out[cap - 1] = 0;
    if (security) *security = !strcmp(use_nv ? nvtype : sdtype, "security");
    return out[0] != 0;
}
static bool best_known_latest(char *out, size_t cap) { return best_known_latest_typed(out, cap, NULL); }

bool nucleo_update_dialog_pending(void)
{
    char latest[24], dismiss[24];
    bool security = false;
    if (!best_known_latest_typed(latest, sizeof latest, &security)) return false;   // nothing learned yet
    nvs_get_string("dismiss", dismiss, sizeof dismiss);
    uint32_t dismt = 0, now = (uint32_t)time(NULL);
    if (nvs_ready()) {
        nvs_get_u32(s_nvs, "dismt", &dismt);
        // Dismissed while the clock was not set: start the reminder window now that it is.
        if (dismiss[0] && dismt < UPD_CLOCK_VALID_S && now >= UPD_CLOCK_VALID_S) {
            nvs_set_u32(s_nvs, "dismt", now); nvs_commit(s_nvs); dismt = now;
        }
    }
    const esp_app_desc_t *app = esp_app_get_description();
    return upd_should_show_at(app ? app->version : "?", latest, dismiss, dismt, now, security);
}

const char *nucleo_update_latest_tag(void)
{
    static char tag[24];
    if (!best_known_latest(tag, sizeof tag)) tag[0] = 0;
    return tag;
}

void nucleo_update_dismiss_latest(void)
{
    if (!nvs_ready()) return;
    // Dismiss the SAME tag the dialog/app compare against — whichever source it came from
    // (see best_known_latest), not necessarily a fresh one if a background check is mid-flight.
    char tag[24];
    if (!best_known_latest(tag, sizeof tag)) return;
    nvs_set_str(s_nvs, "dismiss", tag);
    uint32_t now = (uint32_t)time(NULL);
    nvs_set_u32(s_nvs, "dismt", now >= UPD_CLOCK_VALID_S ? now : 0);   // a security release returns after 7 days
    nvs_commit(s_nvs);
    ESP_LOGI(TAG, "dismissed %s", tag);
}

void nucleo_update_get_state(nucleo_update_state_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

// "Install at the next (fresh-heap Solo) boot" flag. The outbound TLS + flash need a large
// contiguous block that only a fresh boot has; the Updates app arms this, reboots into Solo, and
// the Solo boot runs the real install on an unfragmented heap. NVS-persisted across the warm reboot.
void nucleo_update_arm_boot_install(void)
{
    if (!nvs_ready()) return;
    nvs_set_u8(s_nvs, "install", 1);
    nvs_commit(s_nvs);
}
bool nucleo_update_boot_install_armed(void)
{
    if (!nvs_ready()) return false;
    uint8_t v = 0;
    return nvs_get_u8(s_nvs, "install", &v) == ESP_OK && v == 1;
}
void nucleo_update_disarm_boot_install(void)
{
    if (!nvs_ready()) return;
    nvs_erase_key(s_nvs, "install");
    nvs_commit(s_nvs);
}

// Wait up to ~12 s for the STA IP — used before the install fetch in a Solo boot, where Wi-Fi has
// only just come up. Returns false if no IP (caller aborts the install cleanly).
static bool wait_sta_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    for (int i = 0; i < 120; i++) {
        esp_netif_ip_info_t ip;
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

bool nucleo_update_busy(void) { return s_task_alive; }

// ── check worker ───────────────────────────────────────────────────────────────────────────────
static void check_task(void *arg)
{
    const bool from_boot = (bool)(uintptr_t)arg;

    if (from_boot) {
        // Wait for the STA link (supervisor may still be joining); give up quietly after ~5 min.
        int i = 0;
        for (; i < 30 && !wifi_sta_connected(); i++) vTaskDelay(pdMS_TO_TICKS(10000));
        if (!wifi_sta_connected()) goto out_silent;
        uint32_t last = 0;
        if (nvs_ready()) nvs_get_u32(s_nvs, "lastck", &last);
        if (!upd_check_due((uint32_t)time(NULL), last, UPD_CHECK_INTERVAL_S)) goto out_silent;
        vTaskDelay(pdMS_TO_TICKS(5000));    // let the post-connect burst (SNTP, mDNS) settle first
    }

    {
        uint32_t tk = nucleo_arb_acquire(ARB_BG, "upd-check", 0);   // try-only: busy -> another day
        if (!tk) { st_set_phase(from_boot ? UPD_IDLE : UPD_CHECK_FAIL,
                                from_boot ? NULL : UT("Dispositivo occupato", "Device busy", "Dispositivo ocupado", "Appareil occupe", "Geraet beschaeftigt"));
                   goto out_silent; }
        char body[192], tag[24];
        int n = small_get(UPD_VERSION_URL, body, sizeof body);
        nucleo_arb_release(tk);

        if (n == -2) {
            st_set_phase(from_boot ? UPD_IDLE : UPD_CHECK_FAIL,
                         from_boot ? NULL : UT(LOWRAM_IT, LOWRAM_EN, LOWRAM_ES, LOWRAM_FR, LOWRAM_DE));
            goto out_silent;
        }
        if (n <= 0 || !upd_extract_tag(body, tag, sizeof tag) || !upd_parse_semver(tag, NULL)) {
            st_set_phase(from_boot ? UPD_IDLE : UPD_CHECK_FAIL,
                         from_boot ? NULL : UT("Controllo non riuscito (offline?)", "Check failed (offline?)", "Comprobacion fallida (sin conexion?)", "Verification echouee (hors ligne ?)", "Pruefung fehlgeschlagen (offline?)"));
            goto out_silent;
        }

        portENTER_CRITICAL(&s_mux);
        strncpy(s_st.latest, tag, sizeof(s_st.latest) - 1); s_st.latest[sizeof(s_st.latest) - 1] = 0;
        portEXIT_CRITICAL(&s_mux);
        char ltype[16];
        upd_extract_type(body, ltype, sizeof ltype);
        if (nvs_ready()) {
            nvs_set_str(s_nvs, "latest", tag);
            nvs_set_str(s_nvs, "ltype", ltype);
            nvs_set_u32(s_nvs, "lastck", (uint32_t)time(NULL));
            nvs_commit(s_nvs);
        }
        ESP_LOGI(TAG, "latest release: %s (%s)", tag, ltype);

        char dismiss[24];
        nvs_get_string("dismiss", dismiss, sizeof dismiss);
        uint32_t dismt = 0;
        if (nvs_ready()) nvs_get_u32(s_nvs, "dismt", &dismt);
        const esp_app_desc_t *app = esp_app_get_description();
        if (upd_should_show_at(app ? app->version : "?", tag, dismiss, dismt, (uint32_t)time(NULL),
                               !strcmp(ltype, "security")) && !s_notified_this_boot) {
            s_notified_this_boot = true;
            char id[40], body_txt[72];
            snprintf(id, sizeof id, "update-%s", tag);
            snprintf(body_txt, sizeof body_txt, "%s %s", tag,
                     UT("disponibile - apri Aggiornamenti", "available - open Updates",
                        "disponible - abre Actualizaciones", "disponible - ouvrez Mises a jour",
                        "verfuegbar - oeffne Updates"));
            // A security release rings louder: WARN (colour + chime) instead of INFO. The type comes
            // from the same version.json the tag did; a missing/unknown type is an ordinary update.
            char utype[16];
            upd_extract_type(body, utype, sizeof utype);
            bool sec = (strcmp(utype, "security") == 0);
            const char *title = sec
                ? UT("Aggiornamento di sicurezza NucleoOS", "NucleoOS security update",
                     "Actualizacion de seguridad NucleoOS", "Mise a jour de securite NucleoOS",
                     "NucleoOS-Sicherheitsupdate")
                : UT("Aggiornamento NucleoOS", "NucleoOS update", "Actualizacion de NucleoOS",
                     "Mise a jour NucleoOS", "NucleoOS-Update");
            // One backbone: SD journal + notify.post -> web Notification Center + native banner/chime.
            nucleo_notify_emit("ota", sec ? NOTIFY_WARN : NOTIFY_INFO, id,
                               title, body_txt, "app:settings@updates");
        }
        st_set_phase(UPD_CHECK_DONE, NULL);
    }

out_silent:
    s_task_alive = false;
    vTaskDelete(NULL);
}

bool nucleo_update_kick_check(bool from_boot)
{
    // The fetch needs UPD_TLS_MIN_BLOCK contiguous for its TLS handshake, which this chip rarely has once
    // the OS is up. Don't spawn an 8 KB-stack task (that may then wait minutes for Wi-Fi) just for small_get
    // to refuse it: decide here, and tell a user who pressed "Check now" where the check does work.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < UPD_TLS_MIN_BLOCK) {
        if (!from_boot) st_set_phase(UPD_CHECK_FAIL, UT(LOWRAM_IT, LOWRAM_EN, LOWRAM_ES, LOWRAM_FR, LOWRAM_DE));
        return true;   // handled: the screen shows the reason (not "a check is already running")
    }
    if (!try_claim_task()) return false;
    if (!from_boot) st_set_phase(UPD_CHECKING, NULL);
    // 8 KB stack: the TLS handshake runs in this task (same budget as the IDF https examples).
    if (xTaskCreate(check_task, "upd_check", 8192, (void *)(uintptr_t)from_boot, 3, NULL) != pdPASS) {
        s_task_alive = false;
        if (!from_boot) st_set_phase(UPD_CHECK_FAIL, UT("Memoria insufficiente", "Out of memory", "Memoria insuficiente", "Memoire insuffisante", "Zu wenig Speicher"));
        return false;
    }
    return true;
}

// ── install worker ─────────────────────────────────────────────────────────────────────────────
// noreturn: every call site is a bare `if (cond) fail_install(...)` with no following return, so a
// future edit that let this fall through would silently continue ~10 places at once. The trailing
// spin makes the "never returns" contract hold even if vTaskDelete's teardown is deferred a tick.
static void fail_install(const char *msg, uint32_t tk) __attribute__((noreturn));
static void fail_install(const char *msg, uint32_t tk)
{
    if (tk) nucleo_arb_release(tk);
    ESP_LOGE(TAG, "install failed: %s (stack HWM %u)", msg, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    st_set_phase(UPD_FAILED, msg);
    s_task_alive = false;
    vTaskDelete(NULL);
    for (;;) vTaskDelay(portMAX_DELAY);   // unreachable; satisfies noreturn
}

static void install_task(void *arg)
{
    (void)arg;
    char want[65];

    // Installed by M5Launcher: esp_ota_get_next_update_partition() would be ANOTHER app's slot (or our
    // own running one). Never write it — the update comes from Launcher's OTA list (docs/m5launcher.md).
    if (!nucleo_guest_self_ota_allowed())
        fail_install(UT("Aggiorna da M5Launcher (OTA)", "Update from M5Launcher (OTA)", "Actualiza desde M5Launcher (OTA)",
                        "Mettez a jour via M5Launcher (OTA)", "Update ueber M5Launcher (OTA)"), 0);

    // Runs in a fresh-heap Solo boot where Wi-Fi has only just come up — wait for the STA IP first.
    if (!wait_sta_ip()) fail_install(UT("Nessuna rete Wi-Fi", "No Wi-Fi network", "Sin red Wi-Fi", "Pas de reseau Wi-Fi", "Kein WLAN"), 0);

    uint32_t tk = nucleo_arb_acquire(ARB_FG, "ota-nat", 0);
    if (!tk) fail_install(UT("Dispositivo occupato, riprova", "Device busy, retry", "Dispositivo ocupado, reintenta", "Appareil occupe, reessayez", "Geraet beschaeftigt, erneut versuchen"), 0);

    {   // Fresh tag + checksums from the SAME deploy as the image we are about to pull.
        char sums[2048], body[192], tag[24];   // headroom: a growing asset list must not truncate the OTA line
        if (small_get(UPD_VERSION_URL, body, sizeof body) <= 0 ||
            !upd_extract_tag(body, tag, sizeof tag))
            fail_install(UT("Sito release non raggiungibile", "Release site unreachable", "Sitio de release inaccesible", "Site de release injoignable", "Release-Seite nicht erreichbar"), tk);
        const esp_app_desc_t *app = esp_app_get_description();
        if (upd_cmp(app ? app->version : "?", tag) >= 0)
            fail_install(UT("Nessuna versione piu' nuova", "No newer version", "No hay version mas nueva", "Pas de version plus recente", "Keine neuere Version"), tk);
        int sn = small_get(UPD_SUMS_URL, sums, sizeof sums);
        if (sn >= (int)sizeof(sums) - 1)   // filled the buffer: truncated -> a real line may be cut off
            fail_install(UT("Elenco checksum troppo grande", "Checksum list too large", "Lista de checksums demasiado grande", "Liste de checksums trop grande", "Checksum-Liste zu gross"), tk);
        if (sn <= 0 || !upd_find_sha256(sums, UPD_BIN_NAME, want))
            fail_install(UT("Release senza immagine OTA: usa il flasher web", "Release has no OTA image: use the web flasher", "Release sin imagen OTA: usa el web flasher", "Release sans image OTA : utilisez le web flasher", "Release ohne OTA-Image: Web-Flasher nutzen"), tk);
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) fail_install("no OTA partition", tk);

    // Erase the OTA slot BEFORE opening the connection. OTA_SIZE_UNKNOWN erases the whole partition
    // (a few seconds); doing that with a socket already open risked the server dropping the idle
    // connection mid-erase (the "stalls after erase" class). With no connection yet, that race is
    // gone — we then stream into an already-erased slot. On any later failure we esp_ota_abort(h).
    st_set_phase(UPD_DOWNLOADING, NULL);
    esp_ota_handle_t h = 0;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) != ESP_OK) fail_install("ota_begin", tk);

    esp_http_client_config_t cfg = {
        .url = UPD_BIN_URL, .timeout_ms = 20000, .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048, .max_redirection_count = 3,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { esp_ota_abort(h); fail_install("http init", tk); }
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        esp_http_client_cleanup(cli); esp_ota_abort(h);
        fail_install(UT("Download non riuscito", "Download failed", "Descarga fallida", "Telechargement echoue", "Download fehlgeschlagen"), tk);
    }
    int64_t clen = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        esp_http_client_cleanup(cli); esp_ota_abort(h);
        fail_install(UT("Immagine OTA non trovata sul sito", "OTA image not found on the site", "Imagen OTA no encontrada", "Image OTA introuvable", "OTA-Image nicht gefunden"), tk);
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    // SHA256SUMS comes from the same site as the image, so it only proves the transfer; the release signature
    // proves who built it. Hashed as it streams (the trailing block held back), checked after esp_ota_end.
    static fwsig_stream_t fsig;          // static: off the 12 KB task stack
    fwsig_stream_init(&fsig);

    char buf[2048];
    int total = 0, r;
    bool ok = true;
    const char *why = NULL;
    const int64_t t_start = esp_timer_get_time();
    while ((r = esp_http_client_read(cli, buf, sizeof buf)) > 0) {
        if (total == 0 && (unsigned char)buf[0] != 0xE9) {     // ESP image magic — junk page guard
            ok = false; why = UT("File non valido", "Invalid file", "Archivo no valido", "Fichier invalide", "Ungueltige Datei");
            break;
        }
        if (esp_ota_write(h, buf, r) != ESP_OK) { ok = false; why = "ota_write"; break; }
        mbedtls_sha256_update(&sha, (const unsigned char *)buf, r);
        fwsig_stream_update(&fsig, (const uint8_t *)buf, (size_t)r);
        total += r;
        st_progress(total, (int)clen);
        // Aggregate hard ceiling: per-op timeout_ms doesn't bound a connection that trickles a few
        // bytes per window forever. Cap the WHOLE transfer so a stalled server can't hold exclusive
        // mode + the arbiter token indefinitely with no way out.
        if (esp_timer_get_time() - t_start > 300LL * 1000000) {   // 5 min
            ok = false; why = UT("Download troppo lento", "Download too slow", "Descarga demasiado lenta", "Telechargement trop lent", "Download zu langsam");
            break;
        }
    }
    if (ok && (r < 0 || total == 0)) { ok = false; why = UT("Download interrotto", "Download interrupted", "Descarga interrumpida", "Telechargement interrompu", "Download abgebrochen"); }
    esp_http_client_cleanup(cli);

    if (ok) {
        st_set_phase(UPD_VERIFYING, NULL);
        unsigned char digest[32]; char hex[65];
        mbedtls_sha256_finish(&sha, digest);
        for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", digest[i]);
        if (strcmp(hex, want) != 0) { ok = false; why = UT("Verifica SHA-256 fallita", "SHA-256 check failed", "Verificacion SHA-256 fallida", "Echec verification SHA-256", "SHA-256-Pruefung fehlgeschlagen"); }
    }
    mbedtls_sha256_free(&sha);
    char sig_sha[65]; uint32_t sig_size = 0;
    bool sig_whole = fwsig_stream_final(&fsig, sig_sha, &sig_size);
    fwsig_stream_free(&fsig);

    if (!ok) { esp_ota_abort(h); fail_install(why ? why : "error", tk); }
    if (esp_ota_end(h) != ESP_OK) fail_install(UT("Immagine incompleta", "Incomplete image", "Imagen incompleta", "Image incomplete", "Unvollstaendiges Image"), tk);
    {   // Never boot an image this firmware cannot prove came from a NucleoOS release key.
        esp_app_desc_t d;
        fwsig_rc_t sr = FWSIG_E_MISSING;
        if (esp_ota_get_partition_description(part, &d) == ESP_OK && !strcmp(d.project_name, "nucleoos") && sig_whole)
            sr = fwsig_verify(fsig.tail, FWSIG_DOMAIN_OTA, d.version, sig_sha, sig_size);
        if (sr != FWSIG_OK) {
            ESP_LOGE(TAG, "native OTA rejected: %s", fwsig_rc_name(sr));
            fail_install(sr == FWSIG_E_MISSING
                ? UT("Release non ancora firmata: riprova piu' tardi", "Release not signed yet: retry later", "Release aun no firmada: reintenta mas tarde", "Release pas encore signee : reessayez plus tard", "Release noch nicht signiert: spaeter erneut")
                : UT("Firma del firmware non valida", "Invalid firmware signature", "Firma del firmware no valida", "Signature du firmware invalide", "Ungueltige Firmware-Signatur"), tk);
        }
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) fail_install("set_boot", tk);

    nucleo_arb_release(tk);
    ESP_LOGI(TAG, "native OTA ok: %d bytes -> %s (stack HWM %u), rebooting",
             total, part->label, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    st_set_phase(UPD_REBOOTING, NULL);
    vTaskDelay(pdMS_TO_TICKS(1200));        // one UI tick to paint the reboot screen
    esp_restart();
}

bool nucleo_update_start(void)
{
    if (!try_claim_task()) return false;   // a background check may be mid-flight; caller handles false
    st_set_phase(UPD_DOWNLOADING, NULL);
    st_progress(0, 0);
    // 12 KB stack: the download loop holds a 2 KB on-stack buffer while mbedTLS + esp_ota_write's
    // flash-driver chain are live. The HWM is logged at the end of every run (success + fail) so the
    // margin is measured on real hardware, not guessed. The posture reclaim (~47 KB) already ran.
    if (xTaskCreate(install_task, "upd_flash", 12288, NULL, 4, NULL) != pdPASS) {
        s_task_alive = false;
        st_set_phase(UPD_FAILED, UT("Memoria insufficiente", "Out of memory", "Memoria insuficiente", "Memoire insuffisante", "Zu wenig Speicher"));
        return false;
    }
    return true;
}

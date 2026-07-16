#include "nucleo_auth.h"
#include "nucleo_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "nvs.h"             // NVS fallback tier for auth.json (survives a /cfg-less launcher install)
#include "lwip/sockets.h"   // getpeername / sockaddr_in — per-source-IP pairing lockout

static const char *TAG = "auth";

#define COOKIE_NAME   "nucleo_session"
#define TOKEN_HEX_LEN 48                    // 24 random bytes -> 48 hex chars
#define MAX_TOKENS    32                    // bounded ring of valid sessions (browsers + CLI tools)
#define COOKIE_MAXAGE 31536000              // session cookie lifetime: 1 year
#define AUTH_JSON     NUCLEO_CFG_MOUNT "/config/auth.json"
#define SETTINGS_JSON NUCLEO_SD_MOUNT  "/system/config/settings.json"
#define MAX_FAILS       5
#define LOCKOUT_BASE_US (30 * 1000000LL)    // base lockout after MAX_FAILS misses; doubles per repeat lock
#define LOCKOUT_MAX_LVL 5                   // cap: 30s << 5 = ~16 min
#define FAILTAB_N       8                   // bounded per-source-IP fail table (LRU)

static char    s_pin[7];                    // 6 digits + NUL — stable across reboots (persisted)
static char    s_pin_override[7];           // optional fixed PIN from settings.security.pin
static bool    s_required = true;
static char    s_tokens[MAX_TOKENS][TOKEN_HEX_LEN + 1];
static int     s_token_count;               // newest at [count-1] wrapping via ring write
static int     s_token_head;                // next write slot (ring)

// Per-source-IP brute-force throttle. A GLOBAL counter let any one hostile client lock out EVERY other
// client from pairing (trivial remote DoS of the whole web OS); keying it per IP contains the attacker
// to their own bucket while a new browser on another IP can still pair. Escalating backoff per bucket
// makes sustained guessing exponentially slower. Table is LRU-evicted; accessed only from the single
// httpd task (pair_post), so no lock is needed.
typedef struct { uint32_t ip; int fails; int level; int64_t lock_until; int64_t last; } fail_ent_t;
static fail_ent_t s_fail[FAILTAB_N];

// ---- persistence: PIN + live session tokens ---------------------------------
// TWO tiers: /cfg LittleFS (primary, power-loss-safe) and NVS (fallback). NVS is present in every
// ESP-IDF app, so the PIN + sessions survive the worst case that motivated the config-persistence work:
// a firmware loaded via a third-party launcher WITHOUT our partition table (no /cfg) — previously the
// PIN regenerated every boot there, forcing a re-pair on every restart. NVS also heals a /cfg that got
// reformatted (mount fault -> format_if_mount_failed). Deliberately NOT mirrored to the SD card: unlike
// Wi-Fi config, auth.json holds the PIN and LIVE session tokens, which must never sit on removable media.
#define AUTH_NVS_NS  "nucleoauth"

static bool auth_nvs_write(const char *txt)
{
    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, "auth", txt);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

static char *auth_nvs_read(void)   // malloc'd (caller frees) or NULL
{
    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t sz = 0;
    if (nvs_get_str(h, "auth", NULL, &sz) != ESP_OK || sz == 0) { nvs_close(h); return NULL; }
    char *b = malloc(sz);
    if (!b) { nvs_close(h); return NULL; }
    esp_err_t e = nvs_get_str(h, "auth", b, &sz);
    nvs_close(h);
    if (e != ESP_OK) { free(b); return NULL; }
    return b;
}

static void save_auth(void)
{
    cJSON *root = cJSON_CreateObject();
    if (s_pin[0]) cJSON_AddStringToObject(root, "pin", s_pin);   // stable PIN survives reboots
    cJSON *arr = cJSON_AddArrayToObject(root, "tokens");
    for (int i = 0; i < MAX_TOKENS; i++)
        if (s_tokens[i][0]) cJSON_AddItemToArray(arr, cJSON_CreateString(s_tokens[i]));
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return;

    // Tier 1: /cfg LittleFS (atomic temp+rename; LittleFS rename overwrites the destination in place, so
    // we don't remove the original first — that old remove-then-rename could lose BOTH copies on failure).
    mkdir(NUCLEO_CFG_MOUNT "/config", 0775);
    char tmp[] = AUTH_JSON ".tmp";
    FILE *f = fopen(tmp, "wb");
    if (f) {
        bool ok = (fputs(out, f) >= 0);
        fclose(f);
        if (!ok || rename(tmp, AUTH_JSON) != 0) remove(tmp);
    }
    // Tier 2: NVS (the guaranteed fallback).
    auth_nvs_write(out);
    cJSON_free(out);
}

static void load_auth(void)
{
    memset(s_tokens, 0, sizeof(s_tokens));
    s_token_count = 0; s_token_head = 0;

    // Read /cfg first, then the NVS fallback.
    bool from_nvs = false;
    char *buf = NULL;
    FILE *f = fopen(AUTH_JSON, "rb");
    if (f) {
        buf = malloc(2048);
        if (buf) { size_t n = fread(buf, 1, 2048 - 1, f); buf[n] = '\0'; }
        fclose(f);
    }
    if (!buf) { buf = auth_nvs_read(); from_nvs = (buf != NULL); }
    if (!buf) return;                         // no tier holds it -> fresh PIN minted by init, safe

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    cJSON *pin = cJSON_GetObjectItem(root, "pin");           // restore the persisted PIN
    if (cJSON_IsString(pin) && strlen(pin->valuestring) == 6) strcpy(s_pin, pin->valuestring);
    cJSON *arr = cJSON_GetObjectItem(root, "tokens");
    int c = cJSON_GetArraySize(arr);
    for (int i = 0; i < c && i < MAX_TOKENS; i++) {
        cJSON *t = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsString(t) && strlen(t->valuestring) == TOKEN_HEX_LEN) {
            strcpy(s_tokens[s_token_head], t->valuestring);
            s_token_head = (s_token_head + 1) % MAX_TOKENS;
            if (s_token_count < MAX_TOKENS) s_token_count++;
        }
    }
    cJSON_Delete(root);
    if (from_nvs) { save_auth(); ESP_LOGW(TAG, "auth recovered from NVS fallback (/cfg missing) — re-persisted"); }
    ESP_LOGI(TAG, "loaded %d session token(s)", s_token_count);
}

// settings.security.require_pairing — best effort; default ON if SD/settings absent.
static void load_required(void)
{
    FILE *f = fopen(SETTINGS_JSON, "rb");
    if (!f) { s_required = true; return; }
    char *buf = malloc(2048);                 // transient: parsed then freed -> not permanent .bss
    if (!buf) { fclose(f); s_required = true; return; }   // OOM (never at boot): default pairing ON
    size_t n = fread(buf, 1, 2048 - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    s_required = true;
    s_pin_override[0] = '\0';
    if (root) {
        cJSON *sec = cJSON_GetObjectItem(root, "security");
        cJSON *rp = sec ? cJSON_GetObjectItem(sec, "require_pairing") : NULL;
        if (cJSON_IsBool(rp)) s_required = cJSON_IsTrue(rp);
        // Optional user-chosen fixed PIN (exactly 6 digits) — takes precedence over the
        // auto-generated one, so you can set a memorable code in settings.json.
        cJSON *pp = sec ? cJSON_GetObjectItem(sec, "pin") : NULL;
        const char *pv = NULL;
        if (cJSON_IsString(pp)) pv = pp->valuestring;
        char num[16]; if (!pv && cJSON_IsNumber(pp)) { snprintf(num, sizeof(num), "%06d", pp->valueint); pv = num; }
        if (pv && strlen(pv) == 6 && strspn(pv, "0123456789") == 6) strcpy(s_pin_override, pv);
        cJSON_Delete(root);
    }
}

void nucleo_auth_init(void)
{
    s_pin[0] = '\0';
    load_required();      // s_required + optional s_pin_override
    load_auth();          // restores the persisted PIN (and session tokens), if any

    // PIN selection, most-authoritative first:
    //   1) a fixed PIN set in settings.security.pin
    //   2) the PIN persisted from a previous boot (stable across restarts)
    //   3) a fresh random one on first ever boot — then persisted so it never changes again
    if (s_pin_override[0]) strcpy(s_pin, s_pin_override);
    if (strlen(s_pin) != 6) snprintf(s_pin, sizeof(s_pin), "%06u", (unsigned)(esp_random() % 1000000u));
    save_auth();          // persist PIN (+ tokens) so the same code is shown next boot

    ESP_LOGI(TAG, "pairing %s; PIN %s", s_required ? "required" : "disabled",
             s_pin_override[0] ? "set in settings" : "stable (persisted)");
}

const char *nucleo_auth_pin(void) { return s_pin; }

// Length-checked constant-time string equality: no early-out on the first differing byte, so a network
// attacker can't time-probe the PIN/token one character at a time. (The length is fixed for both the
// 6-digit PIN and the 48-hex token, so comparing over the max leaks nothing useful.)
static bool ct_equal(const char *a, const char *b)
{
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    size_t n = la > lb ? la : lb;
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < n; i++)
        diff |= (unsigned)((unsigned char)(i < la ? a[i] : 0) ^ (unsigned char)(i < lb ? b[i] : 0));
    return diff == 0;
}

// Source IPv4 of the request (0 if unknown / IPv6 — those share one bucket). Best-effort.
static uint32_t client_ip(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) return 0;
    struct sockaddr_storage ss; socklen_t sl = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return 0;
    if (ss.ss_family == AF_INET) return ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    return 0;
}

// Find (or LRU-allocate) the throttle bucket for this IP.
static fail_ent_t *fail_slot(uint32_t ip)
{
    fail_ent_t *lru = &s_fail[0];
    for (int i = 0; i < FAILTAB_N; i++) {
        if (s_fail[i].last != 0 && s_fail[i].ip == ip) return &s_fail[i];
        if (s_fail[i].last < lru->last) lru = &s_fail[i];
    }
    lru->ip = ip; lru->fails = 0; lru->level = 0; lru->lock_until = 0;
    return lru;
}

// ---- token check ----
static bool token_valid(const char *tok)
{
    if (!tok || !*tok) return false;
    for (int i = 0; i < MAX_TOKENS; i++)
        if (s_tokens[i][0] && ct_equal(s_tokens[i], tok)) return true;
    return false;
}

// Pull the nucleo_session value out of the Cookie header.
static bool cookie_token(httpd_req_t *req, char *out, size_t n)
{
    // No per-request malloc: the auth guard runs on EVERY gated /api/* call (+ the /ws handshake), so a
    // malloc/free of the Cookie header on the hot path churned the fragmented no-PSRAM heap during the
    // cold-load+crawl burst. A function-static buffer is race-free (httpd serves on ONE task) with zero
    // stack growth; an implausibly long Cookie is treated as unpaired (the /pair flow re-issues a token).
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len >= 512) return false;
    static char cookie[512];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) != ESP_OK) return false;
    const char *p = strstr(cookie, COOKIE_NAME "=");
    if (!p) return false;
    p += strlen(COOKIE_NAME "=");
    size_t i = 0;
    while (p[i] && p[i] != ';' && p[i] != ' ' && i < n - 1) { out[i] = p[i]; i++; }
    out[i] = '\0';
    return i > 0;
}

bool nucleo_auth_request_ok(httpd_req_t *req)
{
    if (!s_required) return true;
    char tok[TOKEN_HEX_LEN + 1];
    if (!cookie_token(req, tok, sizeof(tok))) return false;
    return token_valid(tok);
}

esp_err_t nucleo_auth_reject(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"error\":\"unpaired\"}");
    return ESP_FAIL;
}

// ---- handlers ----
static esp_err_t status_get(httpd_req_t *req)
{
    char out[64];
    snprintf(out, sizeof(out), "{\"required\":%s,\"paired\":%s}",
             s_required ? "true" : "false", nucleo_auth_request_ok(req) ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static void mint_token(char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < TOKEN_HEX_LEN; i++) out[i] = hex[esp_random() & 0xF];
    out[TOKEN_HEX_LEN] = '\0';
    strcpy(s_tokens[s_token_head], out);             // bounded ring: oldest session is evicted
    s_token_head = (s_token_head + 1) % MAX_TOKENS;
    if (s_token_count < MAX_TOKENS) s_token_count++;
    save_auth();
}

static esp_err_t pair_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int64_t now = esp_timer_get_time();
    fail_ent_t *fe = fail_slot(client_ip(req));
    fe->last = now;
    if (now < fe->lock_until) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_sendstr(req, "{\"error\":\"locked\"}");
        return ESP_OK;
    }

    char body[128] = {0};
    int total = 0, r;
    while ((r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total)) > 0) {
        total += r;
        if (total >= (int)sizeof(body) - 1) break;
    }
    body[total > 0 ? total : 0] = '\0';

    char pin[16] = {0};
    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *p = cJSON_GetObjectItem(root, "pin");
        if (cJSON_IsString(p)) snprintf(pin, sizeof(pin), "%s", p->valuestring);
        else if (cJSON_IsNumber(p)) snprintf(pin, sizeof(pin), "%06d", p->valueint);
        cJSON_Delete(root);
    }

    if (pin[0] && ct_equal(pin, s_pin)) {
        fe->fails = 0; fe->level = 0; fe->lock_until = 0;   // clear this IP's throttle on success
        char token[TOKEN_HEX_LEN + 1];
        mint_token(token);
        char cookie[160];
        snprintf(cookie, sizeof(cookie),
                 COOKIE_NAME "=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax", token, COOKIE_MAXAGE);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);   // valid until the send below
        httpd_resp_sendstr(req, "{\"ok\":true}");
        ESP_LOGI(TAG, "client paired");
        return ESP_OK;
    }

    // Wrong PIN: after MAX_FAILS misses from THIS IP, lock only this bucket, with a backoff that doubles
    // each repeat lock (30s, 60s, 120s ... capped ~16 min) so sustained guessing gets exponentially slower.
    if (++fe->fails >= MAX_FAILS) {
        fe->lock_until = now + (LOCKOUT_BASE_US << (fe->level < LOCKOUT_MAX_LVL ? fe->level : LOCKOUT_MAX_LVL));
        if (fe->level < LOCKOUT_MAX_LVL) fe->level++;
        fe->fails = 0;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, now < fe->lock_until ? "{\"error\":\"bad pin\",\"locked\":true}"
                                                 : "{\"error\":\"bad pin\",\"locked\":false}");
    return ESP_OK;
}

esp_err_t nucleo_auth_register(httpd_handle_t server)
{
    httpd_uri_t status = { .uri = "/api/auth/status", .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t pair   = { .uri = "/api/pair",        .method = HTTP_POST, .handler = pair_post };
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &pair);
    ESP_LOGI(TAG, "pairing API ready: /api/pair, /api/auth/status");
    return ESP_OK;
}

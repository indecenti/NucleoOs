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

static const char *TAG = "auth";

#define COOKIE_NAME   "nucleo_session"
#define TOKEN_HEX_LEN 48                    // 24 random bytes -> 48 hex chars
#define MAX_TOKENS    32                    // bounded ring of valid sessions (browsers + CLI tools)
#define COOKIE_MAXAGE 31536000              // session cookie lifetime: 1 year
#define AUTH_JSON     NUCLEO_CFG_MOUNT "/config/auth.json"
#define SETTINGS_JSON NUCLEO_SD_MOUNT  "/system/config/settings.json"
#define MAX_FAILS     5
#define LOCKOUT_US    (30 * 1000000LL)      // 30s lockout after MAX_FAILS misses

static char    s_pin[7];                    // 6 digits + NUL — stable across reboots (persisted)
static char    s_pin_override[7];           // optional fixed PIN from settings.security.pin
static bool    s_required = true;
static char    s_tokens[MAX_TOKENS][TOKEN_HEX_LEN + 1];
static int     s_token_count;               // newest at [count-1] wrapping via ring write
static int     s_token_head;                // next write slot (ring)
static int     s_fails;
static int64_t s_lock_until;                // esp_timer time after which pairing is allowed again

// ---- persistence (power-loss-safe /cfg): the PIN + the live session tokens ----
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

    mkdir(NUCLEO_CFG_MOUNT "/config", 0775);
    char tmp[] = AUTH_JSON ".tmp";
    FILE *f = fopen(tmp, "wb");
    if (f) {
        fputs(out, f);
        fclose(f);
        remove(AUTH_JSON);                  // FATFS/LittleFS rename won't overwrite
        if (rename(tmp, AUTH_JSON) != 0) remove(tmp);
    }
    cJSON_free(out);
}

static void load_auth(void)
{
    memset(s_tokens, 0, sizeof(s_tokens));
    s_token_count = 0; s_token_head = 0;
    FILE *f = fopen(AUTH_JSON, "rb");
    if (!f) return;
    char *buf = malloc(2048);                 // transient: parsed then freed -> not permanent .bss
    if (!buf) { fclose(f); return; }          // OOM (never at boot): leave tokens empty -> re-pair, safe
    size_t n = fread(buf, 1, 2048 - 1, f);
    fclose(f);
    buf[n] = '\0';
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

// ---- token check ----
static bool token_valid(const char *tok)
{
    if (!tok || !*tok) return false;
    for (int i = 0; i < MAX_TOKENS; i++)
        if (s_tokens[i][0] && strcmp(s_tokens[i], tok) == 0) return true;
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
    if (now < s_lock_until) {
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

    if (pin[0] && strcmp(pin, s_pin) == 0) {
        s_fails = 0;
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

    if (++s_fails >= MAX_FAILS) { s_lock_until = now + LOCKOUT_US; s_fails = 0; }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_sendstr(req, now < s_lock_until ? "{\"error\":\"bad pin\",\"locked\":true}"
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

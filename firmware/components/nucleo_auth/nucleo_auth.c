#include "nucleo_auth.h"
#include "auth_slots.h"
#include "nucleo_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>           // time() — wall clock for the sliding session idle-TTL
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "nvs.h"             // NVS fallback tier for auth.json (survives a /cfg-less launcher install)
#include "lwip/sockets.h"   // getpeername / sockaddr_in — per-source-IP pairing lockout
#include <stdatomic.h>       // factory-reset seal vs an in-flight save_auth()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"   // vTaskDelay while a racing save drains

static const char *TAG = "auth";

#define COOKIE_NAME   "nucleo_session"
#define TOKEN_HEX_LEN 48                    // 24 random bytes -> 48 hex chars
#define MAX_TOKENS    32                    // bounded table of valid sessions (browsers + CLI tools), LRU-evicted
#define COOKIE_MAXAGE 31536000              // browser cookie lifetime: 1 year (server-side idle-TTL is the real bound)

// Sliding idle-TTL. A session that goes UNUSED for SESSION_TTL_SEC is expired server-side and dropped —
// so a cookie sniffed on an untrusted network but never replayed dies on its own, and abandoned browsers
// don't leave a live token forever. Actively-used sessions never expire: each request re-stamps "last
// seen", but at most once per SESSION_REFRESH_SEC so the hot auth path stays write-free within the day.
// (TTL bounds IDLE tokens; an actively-abused token is killed with nucleo_auth_revoke, not by TTL.)
#define SESSION_TTL_SEC     (90LL * 24 * 3600)   // 90 days idle -> re-pair
#define SESSION_REFRESH_SEC (24LL * 3600)        // re-persist "last seen" at most once/day per token
#define CLOCK_VALID_EPOCH   1672531200LL         // > 2023-01-01: the wall clock is really set (not cold boot)
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
static int64_t s_tok_seen[MAX_TOKENS];      // wall-clock (time()) last seen; 0 = minted before the clock was set
static int     s_token_count;               // live sessions in s_tokens
// RAM-only recency (auth_slots.h): bumped on every authenticated request, never persisted, so the hot
// path stays write-free. A new pairing takes a free slot, else evicts the LEAST recently used session.
static uint32_t s_tok_lru[MAX_TOKENS];
static uint32_t s_lru_tick;

static void count_tokens(void)
{
    s_token_count = 0;
    for (int i = 0; i < MAX_TOKENS; i++) if (s_tokens[i][0]) s_token_count++;
}

static void rank_tokens(void)               // after a load: recency from the persisted "last seen"
{
    bool used[MAX_TOKENS];
    for (int i = 0; i < MAX_TOKENS; i++) used[i] = s_tokens[i][0] != '\0';
    s_lru_tick = auth_rank_seen(used, s_tok_seen, s_tok_lru, MAX_TOKENS);
}

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
#define AUTH_NVS_NS   "nucleoauth"
#define AUTH_NVS_REC  "rec"       // the compact binary record (auth_slots.h): what the NVS tier holds now
#define AUTH_NVS_JSON "auth"      // older firmware: the auth.json text — read once to migrate, then erased
_Static_assert(TOKEN_HEX_LEN == AUTH_TOK_HEX, "the NVS record stores 24-byte tokens");

static bool s_cfg_present;        // /cfg is mounted (probed at init): else no auth.json is built at all
static bool s_cfg_saved;          // the last save reached /cfg
static int  s_nvs_kept = -1;      // sessions the last save left in NVS (-1: none yet / failed)
static bool s_removed;            // a session was revoked / expired / evicted since the last NVS save

// The NVS binding of auth_rec_save (auth_slots.h holds the policy and its reasons; host-tested).
static int nvs_put(void *ctx, const uint8_t *rec, size_t len)
{
    nvs_handle_t h = *(nvs_handle_t *)ctx;
    esp_err_t e = nvs_set_blob(h, AUTH_NVS_REC, rec, len);   // identical content: NVS writes nothing
    if (e == ESP_OK) e = nvs_commit(h);
    if (e != ESP_OK) ESP_LOGW(TAG, "NVS save of %u B: %s", (unsigned)len, esp_err_to_name(e));
    return e == ESP_OK ? AUTH_PUT_OK : e == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? AUTH_PUT_NOSPACE : AUTH_PUT_ERROR;
}

static void nvs_wipe(void *ctx)
{
    nvs_handle_t h = *(nvs_handle_t *)ctx;
    nvs_erase_key(h, AUTH_NVS_REC); nvs_erase_key(h, AUTH_NVS_JSON); nvs_commit(h);
}

static long nvs_room(void *ctx)
{
    (void)ctx;
    nvs_stats_t st;
    return nvs_get_stats(NULL, &st) == ESP_OK ? (long)st.available_entries * 32 : -1;
}

static int auth_nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READWRITE, &h) != ESP_OK) { ESP_LOGE(TAG, "NVS open failed: pairing not saved"); return -1; }
    const auth_store_t st = { nvs_put, nvs_wipe, nvs_room, &h };
    uint8_t *rec = malloc(AUTH_REC_SIZE(s_token_count));      // ~0.1-0.9 KB, freed below (NULL: the policy copes)
    int kept = auth_rec_save(&st, rec, s_pin, (const char (*)[AUTH_TOK_HEX + 1])s_tokens, s_tok_seen, s_tok_lru,
                             MAX_TOKENS, s_token_count, s_removed);
    free(rec);
    if (kept >= 0) {
        s_removed = false;
        if (nvs_erase_key(h, AUTH_NVS_JSON) == ESP_OK) nvs_commit(h);   // migrated: the old text frees its room
        if (kept < s_token_count)
            ESP_LOGW(TAG, "NVS is full: %d of %d session(s) persisted (the least recently used are lost at reboot)",
                     kept, s_token_count);
    } else {
        ESP_LOGE(TAG, "pairing not saved to NVS (the cause is logged above)");
    }
    nvs_close(h);
    return kept;
}

static bool auth_nvs_load(void)   // the compact record -> PIN + sessions; false if absent or malformed
{
    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = 0;
    uint8_t *rec = NULL;
    bool ok = false;
    if (nvs_get_blob(h, AUTH_NVS_REC, NULL, &sz) == ESP_OK && sz >= AUTH_REC_HDR && sz <= AUTH_REC_SIZE(255) &&
        (rec = malloc(sz)) != NULL) {
        char pin[7];
        if (nvs_get_blob(h, AUTH_NVS_REC, rec, &sz) == ESP_OK &&
            auth_rec_decode(rec, sz, pin, s_tokens, s_tok_seen, MAX_TOKENS) >= 0) {
            if (pin[0]) strcpy(s_pin, pin);
            ok = true;
        }
        free(rec);
    }
    nvs_close(h);
    return ok;
}

static char *auth_nvs_read_json(void)   // older firmware's copy; malloc'd (caller frees) or NULL
{
    nvs_handle_t h;
    if (nvs_open(AUTH_NVS_NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t sz = 0;
    if (nvs_get_str(h, AUTH_NVS_JSON, NULL, &sz) != ESP_OK || sz == 0) { nvs_close(h); return NULL; }
    char *b = malloc(sz);
    if (!b) { nvs_close(h); return NULL; }
    esp_err_t e = nvs_get_str(h, AUTH_NVS_JSON, b, &sz);
    nvs_close(h);
    if (e != ESP_OK) { free(b); return NULL; }
    return b;
}

// Factory-reset latch (nucleo_auth_factory_reset). Once sealed, save_auth() never writes again this boot,
// so a pairing or a session refresh racing the reset can't re-create auth.json / the NVS copy behind it.
// A save announces itself (s_saving++) BEFORE it reads the seal; the reset raises the seal BEFORE it reads
// s_saving — with sequentially-consistent atomics one side always sees the other (same as setup_store.c).
static atomic_bool s_sealed;
static atomic_int  s_saving;

static void save_auth_now(void);
static void save_auth(void)
{
    atomic_fetch_add(&s_saving, 1);
    if (!atomic_load(&s_sealed)) save_auth_now();
    atomic_fetch_sub(&s_saving, 1);
}

static void save_auth_now(void)
{
    s_cfg_saved = false;
    if (!s_cfg_present) { s_nvs_kept = auth_nvs_save(); return; }   // no /cfg (Launcher install): NVS only
    cJSON *root = cJSON_CreateObject();
    if (s_pin[0]) cJSON_AddStringToObject(root, "pin", s_pin);   // stable PIN survives reboots
    cJSON *arr = cJSON_AddArrayToObject(root, "tokens");
    for (int i = 0; i < MAX_TOKENS; i++)
        if (s_tokens[i][0]) {
            cJSON *o = cJSON_CreateObject();                     // {t:token, s:last-seen} so the idle-TTL survives reboots
            cJSON_AddStringToObject(o, "t", s_tokens[i]);
            cJSON_AddNumberToObject(o, "s", (double)s_tok_seen[i]);
            cJSON_AddItemToArray(arr, o);
        }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    // Tier 1: /cfg LittleFS (atomic temp+rename; LittleFS rename overwrites the destination in place, so
    // we don't remove the original first — that old remove-then-rename could lose BOTH copies on failure).
    if (out) {
        mkdir(NUCLEO_CFG_MOUNT "/config", 0775);
        char tmp[] = AUTH_JSON ".tmp";
        FILE *f = fopen(tmp, "wb");
        if (f) {
            bool ok = (fputs(out, f) >= 0);
            fclose(f);
            if (!ok || rename(tmp, AUTH_JSON) != 0) remove(tmp);
            else s_cfg_saved = true;
        }
        cJSON_free(out);
    }
    // Tier 2: NVS (the guaranteed fallback) — the compact record, written even when the JSON text could not
    // be built (it needs no cJSON tree).
    s_nvs_kept = auth_nvs_save();
}

// auth.json text -> PIN + sessions. False if it does not parse (the caller tries the next tier).
static bool apply_json(const char *txt)
{
    cJSON *root = cJSON_Parse(txt);
    if (!root) return false;
    cJSON *pin = cJSON_GetObjectItem(root, "pin");           // restore the persisted PIN
    if (cJSON_IsString(pin) && strlen(pin->valuestring) == 6) strcpy(s_pin, pin->valuestring);
    cJSON *arr = cJSON_GetObjectItem(root, "tokens");
    int c = cJSON_GetArraySize(arr), k = 0;
    for (int i = 0; i < c && k < MAX_TOKENS; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        const char *hex = NULL; int64_t seen = 0;
        if (cJSON_IsString(e)) {                                   // legacy format: a bare token string (pre-TTL)
            hex = e->valuestring;
        } else if (cJSON_IsObject(e)) {                            // new format: {t:token, s:last-seen}
            cJSON *t = cJSON_GetObjectItem(e, "t");
            cJSON *s = cJSON_GetObjectItem(e, "s");
            if (cJSON_IsString(t)) hex = t->valuestring;
            if (cJSON_IsNumber(s)) seen = (int64_t)s->valuedouble;
        }
        if (hex && strlen(hex) == TOKEN_HEX_LEN) {
            strcpy(s_tokens[k], hex);
            s_tok_seen[k] = seen;                                  // legacy tokens load with seen=0 -> TTL clock starts on next use
            k++;
        }
    }
    cJSON_Delete(root);
    return true;
}

static void load_auth(void)
{
    memset(s_tokens, 0, sizeof(s_tokens));
    memset(s_tok_seen, 0, sizeof(s_tok_seen));
    memset(s_tok_lru, 0, sizeof(s_tok_lru));
    s_token_count = 0; s_lru_tick = 0;

    // /cfg first, then the NVS record, then the NVS text an older firmware wrote. Size the /cfg buffer from
    // the file: a full 32-token ring is ~2.3 KB, and the old fixed 2 KB read truncated it -> parse failed ->
    // PIN + all sessions wiped.
    const char *from = NULL;
    char *buf = NULL;
    FILE *f = fopen(AUTH_JSON, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 8192 && (buf = malloc(sz + 1)) != NULL) { size_t n = fread(buf, 1, sz, f); buf[n] = '\0'; }
        fclose(f);
    }
    if (buf) { if (apply_json(buf)) from = "/cfg"; free(buf); }
    if (!from && auth_nvs_load()) from = "NVS";
    if (!from && (buf = auth_nvs_read_json()) != NULL) { if (apply_json(buf)) from = "NVS (old format)"; free(buf); }
    if (!from) return;                        // no tier holds it -> fresh PIN minted by init, safe
    count_tokens();
    rank_tokens();
    ESP_LOGI(TAG, "loaded %d session token(s) from %s", s_token_count, from);
    // nucleo_auth_init saves right after this: /cfg is healed from NVS when it went missing, and an
    // old-format NVS copy becomes the compact record.
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
    // /cfg mounted? (a Launcher install has no such partition; then nothing is built for it on every save)
    s_cfg_present = mkdir(NUCLEO_CFG_MOUNT "/config", 0775) == 0 || errno == EEXIST;
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

// Find (or LRU-allocate) the throttle bucket for this IP. NULL = every bucket still holds a guesser's
// history: the caller refuses with 429. Evicting such a bucket used to hand a FRESH budget to whoever
// rotated through FAILTAB_N+1 source addresses (trivial on a LAN), which turned the per-IP lockout into
// no limit at all for a 6-digit PIN. Only a clean bucket (no misses, no backoff) or one idle longer than
// the longest lockout may be reused; under a many-address attack, pairing pauses for everyone (already
// paired browsers keep working — they never call /api/pair).
#define FAIL_STALE_US (3600LL * 1000000)
static fail_ent_t *fail_slot(uint32_t ip, int64_t now)
{
    fail_ent_t *victim = NULL;
    for (int i = 0; i < FAILTAB_N; i++) {
        fail_ent_t *e = &s_fail[i];
        if (e->last != 0 && e->ip == ip) return e;
        bool reusable = e->last == 0 || (e->fails == 0 && e->level == 0 && now >= e->lock_until) ||
                        now - e->last > FAIL_STALE_US;
        if (reusable && (!victim || e->last < victim->last)) victim = e;
    }
    if (!victim) return NULL;
    victim->ip = ip; victim->fails = 0; victim->level = 0; victim->lock_until = 0;
    return victim;
}

// ---- token check ----
static bool token_valid(const char *tok)
{
    if (!tok || !*tok) return false;
    int64_t now = (int64_t)time(NULL);
    bool clk = now > CLOCK_VALID_EPOCH;                     // only enforce/refresh the idle-TTL when the clock is really set
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!s_tokens[i][0] || !ct_equal(s_tokens[i], tok)) continue;
        if (clk) {
            if (s_tok_seen[i] == 0) {                       // first use with a known clock -> start the idle window
                s_tok_seen[i] = now; save_auth();
            } else if (now - s_tok_seen[i] > SESSION_TTL_SEC) {
                s_tokens[i][0] = '\0'; s_tok_seen[i] = 0;   // unused past the TTL -> expire this session (must re-pair)
                s_tok_lru[i] = 0; s_removed = true;
                count_tokens();
                save_auth();
                return false;
            } else if (now - s_tok_seen[i] > SESSION_REFRESH_SEC) {
                s_tok_seen[i] = now; save_auth();           // sliding refresh, at most once/day (keeps the hot path write-free)
            }
        }
        s_tok_lru[i] = ++s_lru_tick;                        // in use right now: the last one a new pairing may evict
        return true;
    }
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

int nucleo_auth_revoke(const char *keep_token)
{
    int revoked = 0;
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!s_tokens[i][0]) continue;
        if (keep_token && *keep_token && ct_equal(s_tokens[i], keep_token)) continue;   // spare the caller's own session
        s_tokens[i][0] = '\0'; s_tok_seen[i] = 0; s_tok_lru[i] = 0; revoked++;
    }
    count_tokens();
    if (revoked) { s_removed = true; save_auth(); }                                     // persist the wipe (both tiers)
    return revoked;
}

int nucleo_auth_session_count(void) { return s_token_count; }

void nucleo_auth_persist_status(nucleo_auth_persist_t *out)
{
    out->cfg = s_cfg_saved; out->nvs_kept = s_nvs_kept; out->sessions = s_token_count;
    nvs_stats_t st;
    bool ok = nvs_get_stats(NULL, &st) == ESP_OK;
    out->nvs_free = ok ? (int)st.available_entries : -1;     // free for data (the GC reserve page excluded)
    out->nvs_used = ok ? (int)st.used_entries : -1;
}

bool nucleo_auth_factory_reset(void)
{
    atomic_store(&s_sealed, true);
    for (int i = 0; i < 200 && atomic_load(&s_saving) > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));   // let a racing save land first
    bool ok = (atomic_load(&s_saving) == 0);
    if (!ok) ESP_LOGE(TAG, "factory reset: a save is still in flight — it may land after the erase");
    // RAM first: every session is dead and pairing is refused (no PIN matches "") from this instant on.
    memset(s_tokens, 0, sizeof(s_tokens));
    memset(s_tok_seen, 0, sizeof(s_tok_seen));
    memset(s_tok_lru, 0, sizeof(s_tok_lru));
    s_token_count = 0; s_lru_tick = 0; s_pin[0] = '\0';
    remove(AUTH_JSON); remove(AUTH_JSON ".tmp");
    struct stat st;                                          // gone only if the FS says so (not EMFILE / I/O error)
    if (!(stat(AUTH_JSON, &st) != 0 && (errno == ENOENT || errno == ENOTDIR))) ok = false;
    nvs_handle_t h;
    esp_err_t e = nvs_open(AUTH_NVS_NS, NVS_READONLY, &h);   // probe: no namespace = nothing stored
    if (e == ESP_OK) {
        nvs_close(h);
        e = nvs_open(AUTH_NVS_NS, NVS_READWRITE, &h);
        if (e == ESP_OK) { e = nvs_erase_all(h); if (e == ESP_OK) e = nvs_commit(h); nvs_close(h); }
    } else if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
    if (e != ESP_OK) ok = false;
    ESP_LOGW(TAG, "factory reset: pairing PIN and all sessions erased%s", ok ? "" : " (INCOMPLETE)");
    return ok;
}

// ---- handlers ----
static esp_err_t status_get(httpd_req_t *req)
{
    char out[80];
    snprintf(out, sizeof(out), "{\"required\":%s,\"paired\":%s,\"sessions\":%d}",
             s_required ? "true" : "false", nucleo_auth_request_ok(req) ? "true" : "false", s_token_count);
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
    bool used[MAX_TOKENS];
    for (int i = 0; i < MAX_TOKENS; i++) used[i] = s_tokens[i][0] != '\0';
    int slot = auth_pick_slot(used, s_tok_lru, MAX_TOKENS);   // free slot, else the least recently used session
    if (used[slot]) s_removed = true;                         // a full table evicted a session
    strcpy(s_tokens[slot], out);
    { int64_t now = (int64_t)time(NULL);             // stamp last-seen so the idle-TTL has a start point
      s_tok_seen[slot] = now > CLOCK_VALID_EPOCH ? now : 0; }   // 0 if clock cold -> starts on first clocked use
    s_tok_lru[slot] = ++s_lru_tick;
    count_tokens();
    save_auth();
}

static esp_err_t pair_post(httpd_req_t *req)
{
    // No Access-Control-Allow-Origin: the shell pairs same-origin and CLI tools ignore CORS, while a wildcard
    // let any web page the operator visits run the PIN guessing from their browser and read each verdict.
    httpd_resp_set_type(req, "application/json");

    int64_t now = esp_timer_get_time();
    fail_ent_t *fe = fail_slot(client_ip(req), now);
    if (fe) fe->last = now;
    if (!fe || now < fe->lock_until) {
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

// POST /api/unpair — revoke sessions. Body {"scope":"others"|"all"} (default "others"). "others" logs
// out every OTHER paired client (this session stays in); "all" wipes every session so everyone re-pairs.
// Gated: only a paired client can revoke — the "a cookie may have leaked on an untrusted network -> log
// the others out" recovery. Same-origin (the Settings/Connection UI); no CORS header, no ACAO wildcard.
static esp_err_t revoke_post(httpd_req_t *req)
{
    NUCLEO_AUTH_GUARD(req);
    httpd_resp_set_type(req, "application/json");

    char body[64] = {0};
    int total = 0, r;
    while ((r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total)) > 0) {
        total += r;
        if (total >= (int)sizeof(body) - 1) break;
    }
    body[total > 0 ? total : 0] = '\0';

    bool all = false;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *s = cJSON_GetObjectItem(root, "scope");
        if (cJSON_IsString(s) && strcmp(s->valuestring, "all") == 0) all = true;
        cJSON_Delete(root);
    }

    char keep[TOKEN_HEX_LEN + 1] = {0};
    if (!all) cookie_token(req, keep, sizeof keep);   // spare the caller's own token on "others"
    int n = nucleo_auth_revoke(all ? NULL : keep);
    ESP_LOGW(TAG, "revoked %d session(s) (scope=%s)", n, all ? "all" : "others");

    char out[48];
    snprintf(out, sizeof(out), "{\"ok\":true,\"revoked\":%d}", n);
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

esp_err_t nucleo_auth_register(httpd_handle_t server)
{
    httpd_uri_t status = { .uri = "/api/auth/status", .method = HTTP_GET,  .handler = status_get };
    httpd_uri_t pair   = { .uri = "/api/pair",        .method = HTTP_POST, .handler = pair_post };
    httpd_uri_t unpair = { .uri = "/api/unpair",      .method = HTTP_POST, .handler = revoke_post };
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &pair);
    httpd_register_uri_handler(server, &unpair);
    ESP_LOGI(TAG, "pairing API ready: /api/pair, /api/auth/status, /api/unpair");
    return ESP_OK;
}

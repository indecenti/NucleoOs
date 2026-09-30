// Three-tier config persistence with a secret-free SD mirror + the factory-reset seal/erase.
// See setup_store.h for the model.
#include "setup_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>      // tolower (member scanner)
#include <strings.h>    // strcasecmp: cJSON_GetObjectItem matches member names case-insensitively, so must we
#include <errno.h>
#include <stdatomic.h>
#include <sys/stat.h>   // mkdir() for the SD mirror's /system/config subtree
#include "esp_log.h"
#include "nvs.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define STORE_WAIT_TICK() vTaskDelay(pdMS_TO_TICKS(10))
#else
#define STORE_WAIT_TICK() ((void)0)   // host: single-threaded, nothing can be in flight
#endif

static const char *TAG = "setup";

#define SLURP_MAX (32 * 1024)   // config docs here are < 4 KB; this only rejects the pathological file

static setup_store_status_t s_status = { false, false, false, -1 };

char *setup_store_slurp(const char *path)
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

// "<path>.tmp" into buf; false if it doesn't fit.
static bool tmp_path(const char *path, char *buf, size_t n)
{
    return (size_t)snprintf(buf, n, "%s.tmp", path) < n;
}

// Atomic write via temp+rename. Parent dirs must already exist. Returns false — never crashes — if the
// path's filesystem is absent. Works on BOTH stores: LittleFS rename overwrites the destination in place
// (atomic, power-loss-safe), but FATFS (the SD) rename FAILS if the destination already exists — so on a
// rename failure we remove the old file and retry. tmp is only dropped if the retry also fails, so the
// destination is never left both-gone by the common (dest-exists) case.
static bool write_file_atomic(const char *path, const char *text)
{
    char tmp[160];
    if (!tmp_path(path, tmp, sizeof tmp)) return false;
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

// Create every directory below the mount root on the way to `path` (e.g. /sd/system and
// /sd/system/config; never the mount itself, so with no card inserted every step fails harmlessly and
// the tier reports false). Its own non-inlined frame: the save chain also runs on the 6 KB Wi-Fi
// supervisor task, so this buffer is popped before write_file_atomic's tmp[] is pushed.
__attribute__((noinline)) static bool mkdir_parents(const char *path)
{
    char dir[96];                          // SD config paths are < 48 chars
    if (!path[0] || (size_t)snprintf(dir, sizeof dir, "%s", path) >= sizeof dir) return false;
    char *first = strchr(dir + 1, '/');    // end of the mount-root component
    for (char *p = first ? strchr(first + 1, '/') : NULL; p; p = strchr(p + 1, '/')) {
        *p = '\0'; mkdir(dir, 0775); *p = '/';
    }
    return true;
}

// SD mirror write: create the parent subtree first. Best-effort no-op if no card is mounted.
static bool write_sd_mirror(const char *sd_path, const char *text)
{
    return mkdir_parents(sd_path) && write_file_atomic(sd_path, text);
}

static bool nvs_write_str(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(SETUP_STORE_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_str(h, key, val);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

// Read an NVS string. Returns a malloc'd, NUL-terminated buffer (caller frees) or NULL if absent.
static char *nvs_read_str(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(SETUP_STORE_NVS_NS, NVS_READONLY, &h) != ESP_OK) return NULL;
    size_t sz = 0;
    if (nvs_get_str(h, key, NULL, &sz) != ESP_OK || sz == 0) { nvs_close(h); return NULL; }
    char *buf = malloc(sz);
    if (!buf) { nvs_close(h); return NULL; }
    esp_err_t e = nvs_get_str(h, key, buf, &sz);
    nvs_close(h);
    if (e != ESP_OK) { free(buf); return NULL; }
    return buf;
}

// Remove every member named `key` from the tree, at any depth (networks.json keeps one "pass" per net).
static void redact(cJSON *node, const char *key)
{
    cJSON *c = node ? node->child : NULL;
    while (c) {
        cJSON *next = c->next;
        if (c->string && !strcasecmp(c->string, key)) cJSON_Delete(cJSON_DetachItemViaPointer(node, c));
        else redact(c, key);
        c = next;
    }
}

// Member-name scanner, fed one byte at a time so the SAME logic serves an in-RAM text and a zero-heap
// stream over the card. It tokenizes string literals (a quote or backslash inside a value can't
// desynchronize it) and matches a literal equal to `key` (case-insensitive, no escapes) only when the
// next non-space byte is ':' — a member name, never a value.
typedef struct { const char *key; size_t kl, pos; int st; bool esc, ok; } mscan_t;
enum { MS_OUT, MS_STR, MS_NAME };   // outside a literal / inside one / after a key-equal literal
static void mscan_init(mscan_t *m, const char *key) { memset(m, 0, sizeof *m); m->key = key; m->kl = strlen(key); }
static bool mscan_feed(mscan_t *m, int c)
{
    if (m->st == MS_STR) {
        if (m->esc) { m->esc = false; return false; }               // escaped byte: never part of a key
        if (c == '\\') { m->esc = true; m->ok = false; return false; }
        if (c == '"') { m->st = (m->ok && m->pos == m->kl) ? MS_NAME : MS_OUT; return false; }
        if (m->ok && m->pos < m->kl && tolower(c) == tolower((unsigned char)m->key[m->pos])) m->pos++;
        else m->ok = false;
        return false;
    }
    if (m->st == MS_NAME) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
        if (c == ':') return true;
        m->st = MS_OUT;                                             // it was a value: rescan c from outside
    }
    if (c == '"') { m->st = MS_STR; m->pos = 0; m->ok = true; m->esc = false; }
    return false;
}

bool setup_store_has_member(const char *txt, const char *key)
{
    if (!txt || !key || !key[0]) return false;
    mscan_t m; mscan_init(&m, key);
    for (const unsigned char *p = (const unsigned char *)txt; *p; p++) if (mscan_feed(&m, *p)) return true;
    return false;
}

// Same test over a file, with ZERO heap: stdio is unbuffered (no malloc'd FILE buffer) and the bytes go
// through a 64 B stack chunk. It runs on every load — including the boot network step, before httpd's
// largest-free-block gate, and every Wi-Fi app exit — so it must not allocate a copy of the document.
// Scans at most SLURP_MAX bytes (a larger file is never loaded as a mirror anyway).
static bool file_has_member(const char *path, const char *key)
{
    if (!key || !key[0]) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    setvbuf(f, NULL, _IONBF, 0);
    mscan_t m; mscan_init(&m, key);
    unsigned char chunk[64];
    size_t n, seen = 0;
    bool hit = false;
    while (!hit && seen < SLURP_MAX && (n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        for (size_t i = 0; i < n && !hit; i++) hit = mscan_feed(&m, chunk[i]);
        seen += n;
    }
    fclose(f);
    return hit;
}

// Factory-reset latch + in-flight counter. A save announces itself (inflight++) BEFORE it reads the
// seal; the seal is raised BEFORE it reads inflight. With sequentially-consistent atomics one of the two
// always sees the other: either the save sees the seal and backs out, or the seal sees the save and
// waits for it. Costs one int + one bool of .bss.
static atomic_bool s_sealed;
static atomic_int  s_inflight;

static int save_unsealed(const setup_doc_t *d, cJSON *doc)
{
    char *full = doc ? cJSON_PrintUnformatted(doc) : NULL;
    char *mirror = NULL;
    if (full) { redact(doc, d->secret); mirror = cJSON_PrintUnformatted(doc); }
    cJSON_Delete(doc);                                     // tree gone before any write (heap peak)
    if (!full) return -1;                                  // nothing printed: no tier touched, status kept

    bool cfg = write_file_atomic(d->cfg_path, full);       // 1. /cfg LittleFS (power-loss-safe, SD-independent)
    bool nvs = nvs_write_str(d->nvs_key, full);            // 2. NVS (always present — the guaranteed fallback)
    cJSON_free(full);
    bool sd = false;
    if (mirror) {
        sd = write_sd_mirror(d->sd_path, mirror);          // 3. SD mirror (survives a flash wipe; no secrets)
        cJSON_free(mirror);
    }
    // Couldn't (re)write the mirror (redacted print OOM, card full, write error): if the copy left on
    // the card is one an older firmware wrote WITH the secret, delete it — a missing mirror is worth
    // more than a leaked password. Only once the secret is safe on an internal tier, though: in a
    // legacy SD-only recovery that card copy may be the last persistent one. A clean stale mirror stays.
    if (!sd && (cfg || nvs) && file_has_member(d->sd_path, d->secret)) remove(d->sd_path);
    s_status.cfg_ok = cfg; s_status.nvs_ok = nvs; s_status.sd_ok = sd;
    s_status.tiers_ok = (cfg ? 1 : 0) + (nvs ? 1 : 0) + (sd ? 1 : 0);
    return s_status.tiers_ok;
}

int setup_store_save(const setup_doc_t *d, cJSON *doc)
{
    atomic_fetch_add(&s_inflight, 1);
    int r;
    if (atomic_load(&s_sealed)) {
        cJSON_Delete(doc);                                 // owned: freed even when refused
        ESP_LOGW(TAG, "save of '%s' refused: config store sealed by a reset (reboot pending)", d->nvs_key);
        r = 0;
    } else {
        r = save_unsealed(d, doc);
    }
    atomic_fetch_sub(&s_inflight, 1);
    return r;
}

char *setup_store_load(const setup_doc_t *d, setup_tier_t *tier, bool *sd_dirty)
{
    setup_tier_t t = SETUP_TIER_CFG;
    char *txt = setup_store_slurp(d->cfg_path);
    if (!txt) { t = SETUP_TIER_NVS; txt = nvs_read_str(d->nvs_key); }
    if (!txt) { t = SETUP_TIER_SD;  txt = setup_store_slurp(d->sd_path); }
    if (!txt) t = SETUP_TIER_NONE;

    // Card hygiene on every load: an SD copy written before secrets were stripped still holds them.
    bool dirty = false;
    if (t == SETUP_TIER_SD)        dirty = setup_store_has_member(txt, d->secret);
    else if (t != SETUP_TIER_NONE) dirty = file_has_member(d->sd_path, d->secret);   // zero-heap probe
    // An older firmware's temp file (power cut between its FATFS remove and rename) can hold the full
    // text; ours only ever holds the redacted one. Drop it only if it carries the secret AND the
    // document is safe internally (/cfg or NVS answered) — otherwise it may be the last copy.
    char tmp[160];
    if ((t == SETUP_TIER_CFG || t == SETUP_TIER_NVS) && tmp_path(d->sd_path, tmp, sizeof tmp) &&
        file_has_member(tmp, d->secret)) remove(tmp);
    if (tier) *tier = t;
    if (sd_dirty) *sd_dirty = dirty;
    return txt;
}

void setup_store_status(setup_store_status_t *out)
{
    if (out) *out = s_status;
}

bool setup_store_seal(void)
{
    atomic_store(&s_sealed, true);
    // A save that got past the seal check before we raised it is still writing: let it land now, so the
    // caller's erase runs strictly after it. Bounded (~2 s) — three small writes take milliseconds.
    for (int i = 0; i < 200 && atomic_load(&s_inflight) > 0; i++) STORE_WAIT_TICK();
    if (atomic_load(&s_inflight) == 0) return true;
    ESP_LOGE(TAG, "config store sealed with a save still in flight: it may land after the erase");
    return false;
}

bool setup_store_sealed(void) { return atomic_load(&s_sealed); }

// Absent only when the filesystem SAYS so (no entry / no such directory — an unmounted store answers the
// same). Any other failure (EMFILE, an I/O error) is not proof of erasure.
static bool file_absent(const char *path)
{
    struct stat st;
    return stat(path, &st) != 0 && (errno == ENOENT || errno == ENOTDIR);
}

// Remove a file and its write_file_atomic() temp. true when neither exists afterwards.
static bool remove_with_tmp(const char *path)
{
    char tmp[160];
    bool tmp_ok = tmp_path(path, tmp, sizeof tmp);
    remove(path);
    if (tmp_ok) remove(tmp);
    return file_absent(path) && (!tmp_ok || file_absent(tmp));
}

bool setup_store_erase(const setup_doc_t *d)
{
    bool cfg = remove_with_tmp(d->cfg_path);
    bool sd  = remove_with_tmp(d->sd_path);
    bool nvs;
    nvs_handle_t h;
    esp_err_t oe = nvs_open(SETUP_STORE_NVS_NS, NVS_READWRITE, &h);
    if (oe == ESP_OK) {
        esp_err_t e = nvs_erase_key(h, d->nvs_key);
        if (e == ESP_OK) e = nvs_commit(h);
        else if (e != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "erase NVS '%s' failed (0x%x)", d->nvs_key, (unsigned)e);
        size_t sz = 0;                                 // verify: the loader must find nothing in NVS either
        nvs = (nvs_get_str(h, d->nvs_key, NULL, &sz) == ESP_ERR_NVS_NOT_FOUND);
        nvs_close(h);
    } else {
        // No namespace (never written / no room to create it) holds nothing. NVS not initialised this boot
        // is the one case where the old copy may still sit in flash: report it, don't claim success.
        nvs = (oe != ESP_ERR_NVS_NOT_INITIALIZED);
    }
    if (!(cfg && sd && nvs))
        ESP_LOGE(TAG, "reset: '%s' still present (cfg=%d nvs=%d sd=%d erased)", d->nvs_key, cfg, nvs, sd);
    return cfg && sd && nvs;
}

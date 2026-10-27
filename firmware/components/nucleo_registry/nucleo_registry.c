#include "nucleo_registry.h"
#include "nucleo_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "registry_scan.h"

static const char *TAG = "registry";
// ONE heap block per load: [entry table | string pool], sized to what is actually installed. It replaced
// a static table of 80 fixed-size slots (7.5 KB of .bss, resident from boot) that held ~1.9 KB of strings.
static nucleo_app_t *s_apps;
static int s_count;

// An entry is pointers + flags: the strings are never inline again (that is where the 7.5 KB went).
_Static_assert(sizeof(nucleo_app_t) <= 5 * sizeof(void *), "registry entry grew: keep strings in the pool");

#define APPS_JSON NUCLEO_SD_MOUNT "/system/registry/apps.json"

static char *read_file(const char *path);

// The block under construction. While it grows (realloc may move it) the entries' string fields hold
// OFFSETS into it; pool_fix() turns them into pointers once the block has its final address.
typedef struct {
    char  *blk;
    size_t cap, used;     // used covers the table too: strings are appended after it
    size_t empty;         // offset of the shared "" every absent string points at
} pool_t;

#define POOL_FAIL ((size_t)-1)
#define OFF(o)  ((const char *)(uintptr_t)(o))
#define AT(p, s) ((p)->blk + (uintptr_t)(s))   // an entry field (still an offset) -> its string

// Append s (cut to max bytes, as the old fixed fields did) and return its offset, or POOL_FAIL.
static size_t pool_add(pool_t *p, const char *s, size_t max)
{
    size_t n = strnlen(s, max);
    if (!n) return p->empty;
    if (p->used + n + 1 > p->cap) {
        size_t cap = p->used + n + 1 + 256;
        char *b = realloc(p->blk, cap);
        if (!b) return POOL_FAIL;
        p->blk = b; p->cap = cap;
    }
    size_t off = p->used;
    memcpy(p->blk + off, s, n);
    p->blk[off + n] = '\0';
    p->used += n + 1;
    return off;
}

// Enrich an app entry with display fields from its manifest (best-effort). False = the pool is out of heap.
static bool load_manifest_fields(pool_t *p, nucleo_app_t *a)
{
    a->name = a->id;   // fallback
    char id[NUCLEO_APP_ID_MAX + 1];   // a copy: the pool may move under every pool_add below
    snprintf(id, sizeof id, "%s", AT(p, a->id));
    char path[80];
    snprintf(path, sizeof(path), NUCLEO_SD_MOUNT "/apps/%s/manifest.json", id);
    char *txt = read_file(path);
    if (!txt) return true;
    cJSON *m = cJSON_Parse(txt);
    free(txt);
    if (!m) return true;
    bool ok = true;
    cJSON *name = cJSON_GetObjectItem(m, "name");
    cJSON *route = cJSON_GetObjectItem(m, "web_route");
    cJSON *icon = cJSON_GetObjectItem(m, "icon");
    if (cJSON_IsString(name) && strncmp(name->valuestring, id, NUCLEO_APP_NAME_MAX + 1) != 0) {
        size_t o = pool_add(p, name->valuestring, NUCLEO_APP_NAME_MAX);
        if (o == POOL_FAIL) ok = false; else a->name = OFF(o);
    }
    if (cJSON_IsString(route)) {
        // webfs only ever serves an app at /apps/<id>/ (nucleo_webfs.c map_uri), so any other route could
        // not have worked: record the standard one and say so.
        char std[NUCLEO_APP_ID_MAX + 1 + 8];
        snprintf(std, sizeof std, "/apps/%s/", id);
        if (strcmp(route->valuestring, std) != 0)
            ESP_LOGW(TAG, "%s: web_route '%s' unsupported, using %s", id, route->valuestring, std);
        a->route = NUCLEO_ROUTE_STD;
    }
    if (cJSON_IsString(icon)) {
        char std[NUCLEO_APP_ID_MAX + 1 + 16];
        snprintf(std, sizeof std, "/apps/%s/icon.svg", id);
        if (!strcmp(icon->valuestring, std)) {
            a->icon = NUCLEO_ICON_STD;
        } else if (strlen(icon->valuestring) <= NUCLEO_APP_ICON_MAX) {
            size_t o = pool_add(p, icon->valuestring, NUCLEO_APP_ICON_MAX);
            if (o == POOL_FAIL) ok = false; else { a->icon = NUCLEO_ICON_RAW; a->icon_raw = OFF(o); }
        } else {
            ESP_LOGW(TAG, "%s: icon '%s' too long (max %u), using the standard icon", id,
                     icon->valuestring, (unsigned)NUCLEO_APP_ICON_MAX);
            a->icon = NUCLEO_ICON_STD;
        }
    }
    cJSON_Delete(m);
    return ok;
}

const char *nucleo_registry_route(const nucleo_app_t *a, char *buf, size_t n)
{
    if (!buf || !n) return "";
    buf[0] = '\0';
    if (a && a->route == NUCLEO_ROUTE_STD) snprintf(buf, n, "/apps/%s/", a->id);
    return buf;
}

const char *nucleo_registry_icon(const nucleo_app_t *a, char *buf, size_t n)
{
    if (!buf || !n) return "";
    buf[0] = '\0';
    if (!a) return buf;
    if (a->icon == NUCLEO_ICON_STD) snprintf(buf, n, "/apps/%s/icon.svg", a->id);
    else if (a->icon == NUCLEO_ICON_RAW) snprintf(buf, n, "%s", a->icon_raw);
    return buf;
}

// Cap the read: `len` comes from ftell on an SD file that may be corrupt/oversized, and an unbounded
// malloc on the ~18 KB heap would OOM the boot. The largest legit registry doc (apps.json) is well
// under this ceiling.
#define REGFILE_MAX (256 * 1024)
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > REGFILE_MAX) { ESP_LOGE(TAG, "%s size %ld out of range", path, len); fclose(f); return NULL; }
    char *buf = malloc(len + 1);
    if (!buf) ESP_LOGE(TAG, "no heap for %s (%ld B in one block)", path, len);   // was silent: same symptom, no clue
    else if (fread(buf, 1, len, f) == (size_t)len) buf[len] = '\0';
    else { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

esp_err_t nucleo_registry_load(void)
{
    // Runtime reloads (after an apps.json write) run on a tight heap: a failed read/parse must keep the
    // previous table, not empty the launcher until reboot. Reset the count only once the doc is parsed.
    char *txt = read_file(APPS_JSON);
    if (!txt) return ESP_FAIL;
    const size_t len = strlen(txt);

    // ONE app object at a time (registry_scan.h): the whole-document parse needed ~25-30 KB of nodes and
    // failed on the web profile's heap right after an install, leaving the new app off /api/apps.
    // Pass 1 proves every object parses (peak = one small object) BEFORE the live table is touched, so a
    // malformed doc or a heap too tight even for that keeps the previous table, as before.
    size_t cur = 0, b, e;
    int r, objs = 0;
    while ((r = registry_scan_installed(txt, len, &cur, &b, &e)) == 1) {
        cJSON *item = cJSON_ParseWithLength(txt + b, e - b);
        if (!item) { ESP_LOGE(TAG, "apps.json: app #%d does not parse (malformed, or no heap)", objs + 1); free(txt); return ESP_FAIL; }
        cJSON_Delete(item);
        objs++;
    }
    if (r < 0) { ESP_LOGE(TAG, "apps.json: missing \"installed\" array, or a malformed entry after app #%d", objs); free(txt); return ESP_FAIL; }

    // Pass 2 builds the new block beside the live one, which stays valid until the swap at the end.
    const int slots = objs < NUCLEO_MAX_APPS ? objs : NUCLEO_MAX_APPS;
    const size_t tbl = (size_t)slots * sizeof(nucleo_app_t);
    pool_t p = { .cap = tbl + 1 + (size_t)slots * 40 };   // ~38 B of strings per shipped app; grows if not
    p.blk = malloc(p.cap);
    if (!p.blk) { ESP_LOGE(TAG, "no heap for the app table (%u B)", (unsigned)p.cap); free(txt); return ESP_ERR_NO_MEM; }
    p.empty = tbl;
    p.blk[tbl] = '\0';
    p.used = tbl + 1;

    int count = 0, skipped = 0;
    cur = 0;
    while (registry_scan_installed(txt, len, &cur, &b, &e) == 1) {
        if (count >= NUCLEO_MAX_APPS) { ESP_LOGW(TAG, "app cap reached"); break; }
        cJSON *item = cJSON_ParseWithLength(txt + b, e - b);
        if (!item) { skipped++; continue; }              // pass 1 parsed it: only a heap grab in between can fail it
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *ver = cJSON_GetObjectItem(item, "version");
        cJSON *en = cJSON_GetObjectItem(item, "enabled");
        if (!cJSON_IsString(id)) { cJSON_Delete(item); continue; }
        const size_t mark = p.used;                      // rolled back if this app does not fit the heap
        nucleo_app_t a = { .id = OFF(p.empty), .version = OFF(p.empty), .icon_raw = OFF(p.empty),
                           .enabled = cJSON_IsTrue(en) };
        size_t oid = pool_add(&p, id->valuestring, NUCLEO_APP_ID_MAX);
        size_t over = cJSON_IsString(ver) ? pool_add(&p, ver->valuestring, NUCLEO_APP_VER_MAX) : p.empty;
        cJSON_Delete(item);                              // free this app's nodes before its manifest is read
        bool ok = oid != POOL_FAIL && over != POOL_FAIL;
        if (ok) { a.id = OFF(oid); a.version = OFF(over); ok = load_manifest_fields(&p, &a); }
        if (!ok) { p.used = mark; skipped++; continue; }
        memcpy(p.blk + (size_t)count++ * sizeof a, &a, sizeof a);
    }
    free(txt);

    char *blk = realloc(p.blk, p.used);                  // give back the growth slack
    if (blk) p.blk = blk;
    nucleo_app_t *apps = (nucleo_app_t *)p.blk;
    for (int i = 0; i < count; i++) {
        apps[i].id       = AT(&p, apps[i].id);
        apps[i].version  = AT(&p, apps[i].version);
        apps[i].name     = AT(&p, apps[i].name);
        apps[i].icon_raw = AT(&p, apps[i].icon_raw);
    }
    nucleo_app_t *old = s_apps;
    s_apps = apps;
    s_count = count;
    free(old);

    if (skipped) ESP_LOGW(TAG, "loaded %d installed apps; %d skipped for lack of heap (back on the next reload)", s_count, skipped);
    else ESP_LOGI(TAG, "loaded %d installed apps (%d in apps.json, %u B)", s_count, objs, (unsigned)p.used);
    return skipped ? ESP_ERR_NO_MEM : ESP_OK;
}

int nucleo_registry_count(void) { return s_count; }
const nucleo_app_t *nucleo_registry_apps(void) { return s_apps; }

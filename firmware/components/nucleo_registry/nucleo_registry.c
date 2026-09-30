#include "nucleo_registry.h"
#include "nucleo_board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "registry";
static nucleo_app_t s_apps[NUCLEO_MAX_APPS];
static int s_count;

// RAM budget: this table is static DRAM on a PSRAM-less chip. Raising the cap must never cost more than
// the old 48 x 157 B layout did — shrink a field instead (the cap went 48 -> 80 at no RAM cost).
_Static_assert(sizeof(s_apps) <= 48 * 157, "registry table grew past its RAM budget");

#define APPS_JSON NUCLEO_SD_MOUNT "/system/registry/apps.json"

static char *read_file(const char *path);

// Enrich an app entry with display fields from its manifest (best-effort).
static void load_manifest_fields(nucleo_app_t *a)
{
    strncpy(a->name, a->id, sizeof(a->name) - 1);   // fallback
    char path[80];
    snprintf(path, sizeof(path), NUCLEO_SD_MOUNT "/apps/%s/manifest.json", a->id);
    char *txt = read_file(path);
    if (!txt) return;
    cJSON *m = cJSON_Parse(txt);
    free(txt);
    if (!m) return;
    cJSON *name = cJSON_GetObjectItem(m, "name");
    cJSON *route = cJSON_GetObjectItem(m, "web_route");
    cJSON *icon = cJSON_GetObjectItem(m, "icon");
    if (cJSON_IsString(name))  strncpy(a->name, name->valuestring, sizeof(a->name) - 1);
    if (cJSON_IsString(route)) {
        // webfs only ever serves an app at /apps/<id>/ (nucleo_webfs.c map_uri), so any other route could
        // not have worked: record the standard one and say so.
        char std[sizeof(a->id) + 8];
        snprintf(std, sizeof std, "/apps/%s/", a->id);
        if (strcmp(route->valuestring, std) != 0)
            ESP_LOGW(TAG, "%s: web_route '%s' unsupported, using %s", a->id, route->valuestring, std);
        a->route = NUCLEO_ROUTE_STD;
    }
    if (cJSON_IsString(icon)) {
        char std[sizeof(a->id) + 16];
        snprintf(std, sizeof std, "/apps/%s/icon.svg", a->id);
        if (!strcmp(icon->valuestring, std)) {
            a->icon = NUCLEO_ICON_STD;
        } else if (strlen(icon->valuestring) < sizeof(a->icon_raw)) {
            a->icon = NUCLEO_ICON_RAW;
            strcpy(a->icon_raw, icon->valuestring);
        } else {
            ESP_LOGW(TAG, "%s: icon '%s' too long (max %u), using the standard icon", a->id,
                     icon->valuestring, (unsigned)(sizeof(a->icon_raw) - 1));
            a->icon = NUCLEO_ICON_STD;
        }
    }
    cJSON_Delete(m);
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
    if (buf && fread(buf, 1, len, f) == (size_t)len) buf[len] = '\0';
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

    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) { ESP_LOGE(TAG, "invalid JSON in apps.json"); return ESP_FAIL; }
    s_count = 0;

    cJSON *installed = cJSON_GetObjectItem(root, "installed");
    cJSON *item;
    cJSON_ArrayForEach(item, installed) {
        if (s_count >= NUCLEO_MAX_APPS) { ESP_LOGW(TAG, "app cap reached"); break; }
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *ver = cJSON_GetObjectItem(item, "version");
        cJSON *en = cJSON_GetObjectItem(item, "enabled");
        if (!cJSON_IsString(id)) continue;
        nucleo_app_t *a = &s_apps[s_count++];
        memset(a, 0, sizeof(*a));
        strncpy(a->id, id->valuestring, sizeof(a->id) - 1);
        if (cJSON_IsString(ver)) strncpy(a->version, ver->valuestring, sizeof(a->version) - 1);
        a->enabled = cJSON_IsTrue(en);
        load_manifest_fields(a);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded %d installed apps", s_count);
    return ESP_OK;
}

int nucleo_registry_count(void) { return s_count; }
const nucleo_app_t *nucleo_registry_apps(void) { return s_apps; }

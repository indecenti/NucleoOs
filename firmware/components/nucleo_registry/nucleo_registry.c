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
    if (cJSON_IsString(route)) strncpy(a->web_route, route->valuestring, sizeof(a->web_route) - 1);
    if (cJSON_IsString(icon))  strncpy(a->icon, icon->valuestring, sizeof(a->icon) - 1);
    cJSON_Delete(m);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "open %s failed", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    if (buf && fread(buf, 1, len, f) == (size_t)len) buf[len] = '\0';
    else { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

esp_err_t nucleo_registry_load(void)
{
    s_count = 0;
    char *txt = read_file(APPS_JSON);
    if (!txt) return ESP_FAIL;

    cJSON *root = cJSON_Parse(txt);
    free(txt);
    if (!root) { ESP_LOGE(TAG, "invalid JSON in apps.json"); return ESP_FAIL; }

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

#include "nucleo_i18n.h"
#include "nucleo_board.h"   // NUCLEO_SD_MOUNT
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>        // strncasecmp

static const char *TAG = "i18n";
#define SETTINGS_JSON NUCLEO_SD_MOUNT "/system/config/settings.json"

// The whole RAM cost of native i18n: one flag + a small counter + one callback pointer. Default
// Italian (the OS's primary language).
static bool s_en = false;
static uint32_t s_gen = 0;                 // bumped on every real change; native UIs poll it to repaint live
static void (*s_on_change)(bool) = NULL;   // set by the HTTP layer to broadcast a change to web clients

bool nucleo_i18n_is_en(void) { return s_en; }
uint32_t nucleo_i18n_gen(void) { return s_gen; }
void nucleo_i18n_set_on_change(void (*cb)(bool en)) { s_on_change = cb; }
const char *nucleo_tr(const char *it, const char *en) { return s_en ? en : it; }

// Load settings.json into a fresh cJSON tree (caller owns it, or NULL). Mirrors the proven sizing in
// nucleo_voice: read the whole file by its real size, never a fixed buffer that could truncate.
static cJSON *settings_load(void)
{
    FILE *f = fopen(SETTINGS_JSON, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16384) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f); fclose(f);
    buf[n] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

void nucleo_i18n_load(void)
{
    cJSON *root = settings_load();
    if (root) {
        cJSON *ui = cJSON_GetObjectItem(root, "ui");
        cJSON *lg = ui ? cJSON_GetObjectItem(ui, "language") : NULL;
        if (cJSON_IsString(lg) && lg->valuestring)
            s_en = !strncasecmp(lg->valuestring, "en", 2);
        cJSON_Delete(root);
    }
    ESP_LOGI(TAG, "system language: %s", s_en ? "en" : "it");
}

void nucleo_i18n_set_en(bool en)
{
    bool changed = (s_en != en);
    s_en = en;   // live: the next paint of any native screen already reflects the new language.

    // Read-modify-write so every other key the web Settings app owns is preserved.
    cJSON *root = settings_load();
    if (!root) root = cJSON_CreateObject();
    cJSON *ui = cJSON_GetObjectItem(root, "ui");
    if (!cJSON_IsObject(ui)) { cJSON_DeleteItemFromObject(root, "ui"); ui = cJSON_AddObjectToObject(root, "ui"); }
    if (ui) {
        cJSON_DeleteItemFromObject(ui, "language");
        cJSON_AddStringToObject(ui, "language", en ? "en" : "it");
    }
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) return;
    char tmp[160]; snprintf(tmp, sizeof tmp, "%s.tmp", SETTINGS_JSON);
    FILE *w = fopen(tmp, "wb");
    if (w) {
        fwrite(out, 1, strlen(out), w); fclose(w);
        remove(SETTINGS_JSON); rename(tmp, SETTINGS_JSON);   // atomic-ish swap
    }
    cJSON_free(out);
    ESP_LOGI(TAG, "language set: %s", en ? "en" : "it");

    // Notify the rest of the system ONLY on a real change: bump the generation so native screens
    // repaint on their next frame, and fire the hook so the HTTP layer can push the change to any
    // connected browser (device -> web live sync). Idempotent sets stay silent (no spurious repaint).
    if (changed) {
        s_gen++;
        if (s_on_change) s_on_change(en);
    }
}

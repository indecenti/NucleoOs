#include "nucleo_prefs.h"
#include "nucleo_board.h"   // NUCLEO_SD_MOUNT
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "prefs";
#define SETTINGS_JSON NUCLEO_SD_MOUNT "/system/config/settings.json"

// Load settings.json into a fresh cJSON tree (caller owns it, or NULL). Same proven sizing as
// nucleo_i18n: read the whole file by its real size, never a fixed buffer that could truncate.
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

static int clamp_pct(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

bool nucleo_prefs_load(int *brightness, int *volume, bool *muted)
{
    cJSON *root = settings_load();
    if (!root) return false;
    cJSON *power = cJSON_GetObjectItem(root, "power");
    if (cJSON_IsObject(power)) {
        cJSON *b = cJSON_GetObjectItem(power, "display_brightness");
        cJSON *v = cJSON_GetObjectItem(power, "volume");
        cJSON *m = cJSON_GetObjectItem(power, "muted");
        if (brightness && cJSON_IsNumber(b)) *brightness = clamp_pct((int)b->valuedouble);
        if (volume     && cJSON_IsNumber(v)) *volume     = clamp_pct((int)v->valuedouble);
        if (muted      && cJSON_IsBool(m))   *muted      = cJSON_IsTrue(m);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded bri=%d vol=%d mute=%d",
             brightness ? *brightness : -1, volume ? *volume : -1, muted ? *muted : -1);
    return true;
}

void nucleo_prefs_save(int brightness, int volume, bool muted)
{
    brightness = clamp_pct(brightness);
    volume     = clamp_pct(volume);

    // Read-modify-write so every other key the web Settings app owns is preserved.
    cJSON *root = settings_load();
    if (!root) root = cJSON_CreateObject();
    cJSON *power = cJSON_GetObjectItem(root, "power");

    // No-op guard: if the file already holds exactly these three values, skip the write entirely.
    // The deliberate save sites fire on every edit-mode EXIT (even an exit that changed nothing — e.g.
    // opening a slider and backing out, or leaving the shared TTS-speed editor), so without this a
    // settings.json rewrite would happen on interactions that touched none of these prefs. Comparing
    // against the on-disk values collapses those to zero SD writes, sparing the card needless wear.
    if (cJSON_IsObject(power)) {
        cJSON *b = cJSON_GetObjectItem(power, "display_brightness");
        cJSON *v = cJSON_GetObjectItem(power, "volume");
        cJSON *m = cJSON_GetObjectItem(power, "muted");
        if (cJSON_IsNumber(b) && cJSON_IsNumber(v) && cJSON_IsBool(m) &&
            (int)b->valuedouble == brightness && (int)v->valuedouble == volume &&
            cJSON_IsTrue(m) == muted) {
            cJSON_Delete(root);
            return;                                  // already persisted; nothing to do
        }
    }
    if (!cJSON_IsObject(power)) { cJSON_DeleteItemFromObject(root, "power"); power = cJSON_AddObjectToObject(root, "power"); }
    if (power) {
        cJSON_DeleteItemFromObject(power, "display_brightness");
        cJSON_AddNumberToObject(power, "display_brightness", brightness);
        cJSON_DeleteItemFromObject(power, "volume");
        cJSON_AddNumberToObject(power, "volume", volume);
        cJSON_DeleteItemFromObject(power, "muted");
        cJSON_AddBoolToObject(power, "muted", muted);
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
    ESP_LOGI(TAG, "saved bri=%d vol=%d mute=%d", brightness, volume, muted);
}

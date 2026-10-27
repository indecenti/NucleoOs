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

// Size of settings.json on disk, or -1 if absent. Lets us tell "file missing" (fine, create it) apart
// from "file present but unparseable" (must NOT clobber — it holds ui.language/theme we can't see).
static long settings_file_size(void)
{
    FILE *f = fopen(SETTINGS_JSON, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
    return sz;
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
    if (!root) {
        // Distinguish "no file yet" (create one) from "file present but unparseable". In the latter
        // case the document still holds ui.language / ui.theme / device.* that we cannot read here —
        // overwriting it with a power-only object would wipe those keys and revert the OS language on
        // the next boot. A brightness/volume tweak must never do that: bail and leave the file intact.
        if (settings_file_size() > 0) { ESP_LOGW(TAG, "settings.json unparseable — skipping prefs write to avoid clobbering ui.*"); return; }
        root = cJSON_CreateObject();
    }
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

bool nucleo_prefs_load_extdisp(char *mode, size_t mode_len, char *ctrl, size_t ctrl_len, int *rotation, int *mhz,
                               bool *mirror, int *invert, int *rgb, bool *diag, int pins[6],
                               bool *touch_input, int touch_cal[7], bool *has_cal, bool *primary, bool *fit,
                               char *layout, size_t layout_len)
{
    cJSON *root = settings_load();
    if (!root) return false;
    cJSON *disp = cJSON_GetObjectItem(root, "display");
    if (cJSON_IsObject(disp)) {
        cJSON *m = cJSON_GetObjectItem(disp, "external");
        cJSON *c = cJSON_GetObjectItem(disp, "external_controller");
        cJSON *r = cJSON_GetObjectItem(disp, "external_rotation");
        cJSON *f = cJSON_GetObjectItem(disp, "external_mhz");
        cJSON *mi = cJSON_GetObjectItem(disp, "external_mirror");
        cJSON *iv = cJSON_GetObjectItem(disp, "external_invert");
        cJSON *rg = cJSON_GetObjectItem(disp, "external_rgb");
        cJSON *dg = cJSON_GetObjectItem(disp, "external_diag");
        cJSON *p = cJSON_GetObjectItem(disp, "external_pins");
        if (mode && mode_len && cJSON_IsString(m)) snprintf(mode, mode_len, "%s", m->valuestring);
        if (ctrl && ctrl_len && cJSON_IsString(c)) snprintf(ctrl, ctrl_len, "%s", c->valuestring);
        if (rotation && cJSON_IsNumber(r)) *rotation = (int)r->valuedouble;
        if (mhz && cJSON_IsNumber(f)) *mhz = (int)f->valuedouble;
        if (mirror && cJSON_IsBool(mi)) *mirror = cJSON_IsTrue(mi);
        if (invert && cJSON_IsBool(iv)) *invert = cJSON_IsTrue(iv) ? 1 : 0;
        if (rgb && cJSON_IsBool(rg)) *rgb = cJSON_IsTrue(rg) ? 1 : 0;
        if (diag && cJSON_IsBool(dg)) *diag = cJSON_IsTrue(dg);
        static const char *const KEYS[6] = { "cs", "dc", "rst", "t_cs", "t_irq", "bl" };
        cJSON *ti = cJSON_GetObjectItem(disp, "external_touch");
        if (touch_input && cJSON_IsBool(ti)) *touch_input = cJSON_IsTrue(ti);
        cJSON *pr = cJSON_GetObjectItem(disp, "external_primary");
        if (primary && cJSON_IsBool(pr)) *primary = cJSON_IsTrue(pr);
        cJSON *ft = cJSON_GetObjectItem(disp, "external_fit");
        if (fit && cJSON_IsBool(ft)) *fit = cJSON_IsTrue(ft);
        cJSON *lo = cJSON_GetObjectItem(disp, "external_layout");
        if (layout && layout_len && cJSON_IsString(lo)) snprintf(layout, layout_len, "%s", lo->valuestring);
        cJSON *tc = cJSON_GetObjectItem(disp, "external_touch_cal");
        if (touch_cal && has_cal && cJSON_IsArray(tc) && cJSON_GetArraySize(tc) == 7) {
            bool ok = true;
            for (int i = 0; i < 7 && ok; i++) {
                cJSON *v = cJSON_GetArrayItem(tc, i);
                if (cJSON_IsNumber(v)) touch_cal[i] = (int)v->valuedouble; else ok = false;
            }
            *has_cal = ok;
        }
        if (pins && cJSON_IsObject(p))
            for (int i = 0; i < 6; i++) {
                cJSON *v = cJSON_GetObjectItem(p, KEYS[i]);
                if (cJSON_IsNumber(v)) pins[i] = (int)v->valuedouble;
            }
    }
    cJSON_Delete(root);
    return true;
}

// Write one "display.<key>" (takes ownership of `val`), preserving every other key.
static bool save_display_item(const char *key, cJSON *val)
{
    // Same read-modify-write and clobber guard as nucleo_prefs_save: every other key survives.
    cJSON *root = settings_load();
    if (!root) {
        if (settings_file_size() > 0) { ESP_LOGW(TAG, "settings.json unparseable — display.%s not saved", key); cJSON_Delete(val); return false; }
        root = cJSON_CreateObject();
    }
    cJSON *disp = cJSON_GetObjectItem(root, "display");
    if (!cJSON_IsObject(disp)) { cJSON_DeleteItemFromObject(root, "display"); disp = cJSON_AddObjectToObject(root, "display"); }
    if (!disp || !val) { cJSON_Delete(val); cJSON_Delete(root); return false; }
    cJSON_DeleteItemFromObject(disp, key);
    cJSON_AddItemToObject(disp, key, val);
    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) return false;
    char tmp[160]; snprintf(tmp, sizeof tmp, "%s.tmp", SETTINGS_JSON);
    bool ok = false;
    FILE *w = fopen(tmp, "wb");
    if (w) {
        ok = fwrite(out, 1, strlen(out), w) == strlen(out);
        fclose(w);
        if (ok) { remove(SETTINGS_JSON); ok = rename(tmp, SETTINGS_JSON) == 0; }
    }
    cJSON_free(out);
    ESP_LOGI(TAG, "display.%s %s", key, ok ? "saved" : "NOT saved");
    return ok;
}

bool nucleo_prefs_save_extdisp_cal(const int cal[7]) { return save_display_item("external_touch_cal", cJSON_CreateIntArray(cal, 7)); }
bool nucleo_prefs_save_display_bool(const char *key, bool v) { return save_display_item(key, cJSON_CreateBool(v)); }
bool nucleo_prefs_save_display_int(const char *key, int v)   { return save_display_item(key, cJSON_CreateNumber(v)); }
bool nucleo_prefs_save_display_str(const char *key, const char *v) { return save_display_item(key, cJSON_CreateString(v ? v : "")); }

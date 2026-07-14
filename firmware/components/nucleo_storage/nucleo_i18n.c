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

// The whole RAM cost of native i18n: the active language code + a counter + one callback pointer.
// The OS ships more display languages than the on-TFT UI paints: settings.json -> ui.language is the
// single OS-wide signal ("it","en","es","fr","de",…), and it is stored VERBATIM here so a language the
// web supports is never lost on a round-trip through the device. The native TFT strings themselves are
// only IT/EN (nucleo_tr picks between two flash literals), so nucleo_i18n_is_en() is true ONLY for
// "en"; every other non-English code falls back to Italian (the base) when painting native screens.
static char s_lang[6] = "it";              // active OS language code (default Italian, the base)
static uint32_t s_gen = 0;                 // bumped on every real change; native UIs poll it to repaint live
static void (*s_on_change)(const char *) = NULL;   // set by the HTTP layer to broadcast a change to web clients

// Normalize any incoming code to a 2-letter lowercase tag; anything invalid becomes the base "it".
static void norm_lang(char *dst, size_t cap, const char *code)
{
    if (code && code[0] && code[1]) {
        dst[0] = (char)(code[0] | 0x20);
        dst[1] = (char)(code[1] | 0x20);
        dst[2] = '\0';
    } else {
        snprintf(dst, cap, "it");
    }
}

bool nucleo_i18n_is_en(void) { return s_lang[0] == 'e' && s_lang[1] == 'n'; }
const char *nucleo_i18n_lang(void) { return s_lang; }
uint32_t nucleo_i18n_gen(void) { return s_gen; }
void nucleo_i18n_set_on_change(void (*cb)(const char *code)) { s_on_change = cb; }
// Native strings are IT/EN only: English literal for "en", the Italian (base) literal otherwise.
const char *nucleo_tr(const char *it, const char *en) { return nucleo_i18n_is_en() ? en : it; }

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
            norm_lang(s_lang, sizeof s_lang, lg->valuestring);
        cJSON_Delete(root);
    }
    ESP_LOGI(TAG, "system language: %s", s_lang);
}

// Set the OS language to an arbitrary code (it/en/es/fr/de…), persisting it verbatim so the web keeps
// the exact choice across a reboot; native painting falls back to IT for any non-"en" code.
void nucleo_i18n_set_lang(const char *code)
{
    char nl[6]; norm_lang(nl, sizeof nl, code);
    bool changed = (strcmp(s_lang, nl) != 0);
    snprintf(s_lang, sizeof s_lang, "%s", nl);   // live: the next paint of any native screen reflects it

    // Read-modify-write so every other key the web Settings app owns is preserved.
    cJSON *root = settings_load();
    if (!root) root = cJSON_CreateObject();
    cJSON *ui = cJSON_GetObjectItem(root, "ui");
    if (!cJSON_IsObject(ui)) { cJSON_DeleteItemFromObject(root, "ui"); ui = cJSON_AddObjectToObject(root, "ui"); }
    if (ui) {
        cJSON_DeleteItemFromObject(ui, "language");
        cJSON_AddStringToObject(ui, "language", s_lang);
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
    ESP_LOGI(TAG, "language set: %s", s_lang);

    // Notify the rest of the system ONLY on a real change: bump the generation so native screens
    // repaint on their next frame, and fire the hook so the HTTP layer can push the change to any
    // connected browser (device -> web live sync). Idempotent sets stay silent (no spurious repaint).
    if (changed) {
        s_gen++;
        if (s_on_change) s_on_change(s_lang);
    }
}

// Convenience for the native IT/EN toggle sites (launcher, WiFi/Link app settings).
void nucleo_i18n_set_en(bool en) { nucleo_i18n_set_lang(en ? "en" : "it"); }

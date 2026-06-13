#include "nucleo_theme.h"
#include "nucleo_board.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <M5GFX.h>

static const char *TAG = "theme";

// Global active colors
uint16_t THEME_BG = 0x0841;
uint16_t THEME_FG = 0xFFFF;
uint16_t THEME_MUTED = 0x8C71;
uint16_t THEME_DIM = 0x4410;
uint16_t THEME_LINE = 0x2945;
uint16_t THEME_INK = 0x0000;
uint16_t THEME_SEL = 0x4D9F;
uint16_t THEME_ACC = 0x4D9F;

static char s_current_theme[24] = "classic";

static const nucleo_theme_t s_themes[] = {
    {
        "classic", "Nucleo Classic",
        0x0841, // bg
        0xFFFF, // fg
        0x8C71, // muted
        0x4410, // dim
        0x2945, // line
        0x0000, // ink
        0x4D9F, // sel
        0x4D9F  // acc
    },
    {
        "nano_banana", "Nano Banana",
        0x2104, // bg (dark industrial grey/brown)
        0xFFE0, // fg (bright yellow)
        0x8400, // muted (darker yellow/brown)
        0x4200, // dim (very dark brown)
        0x8400, // line
        0x0000, // ink
        0xFD20, // sel (orange/yellow)
        0xFFE0  // acc (yellow)
    },
    {
        "hacker", "Hacker Green",
        0x0000, // bg (black)
        0x07E0, // fg (green)
        0x03E0, // muted (dark green)
        0x01E0, // dim
        0x03E0, // line
        0x0000, // ink
        0x07E0, // sel
        0x07E0  // acc
    },
    {
        "amoled", "AMOLED Black",
        0x0000, // bg (black)
        0xFFFF, // fg (white)
        0x8410, // muted (grey)
        0x4208, // dim
        0x2104, // line
        0x0000, // ink
        0x8410, // sel
        0xFFFF  // acc
    }
};
static const int s_theme_count = sizeof(s_themes) / sizeof(s_themes[0]);

static void apply_theme_struct(const nucleo_theme_t *t) {
    THEME_BG = t->bg;
    THEME_FG = t->fg;
    THEME_MUTED = t->muted;
    THEME_DIM = t->dim;
    THEME_LINE = t->line;
    THEME_INK = t->ink;
    THEME_SEL = t->sel;
    THEME_ACC = t->acc;
    strncpy(s_current_theme, t->id, sizeof(s_current_theme) - 1);
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char*)malloc(n + 1);
    if (b && fread(b, 1, n, f) == (size_t)n) b[n] = '\0'; else { free(b); b = NULL; }
    fclose(f);
    return b;
}

// Internal: read from LittleFS or SD
static void load_config_theme(void) {
    const char *paths[] = {
        NUCLEO_CFG_MOUNT "/config/theme.json",
        NUCLEO_SD_MOUNT "/apps/theme.cfg"
    };
    for (int i = 0; i < 2; i++) {
        char *txt = slurp(paths[i]);
        if (txt) {
            cJSON *r = cJSON_Parse(txt);
            free(txt);
            if (r) {
                cJSON *t = cJSON_GetObjectItem(r, "theme");
                if (cJSON_IsString(t)) {
                    for (int j = 0; j < s_theme_count; j++) {
                        if (!strcmp(s_themes[j].id, t->valuestring)) {
                            apply_theme_struct(&s_themes[j]);
                            cJSON_Delete(r);
                            return;
                        }
                    }
                }
                cJSON_Delete(r);
            }
        }
    }
    // Default fallback
    apply_theme_struct(&s_themes[0]);
}

extern "C" void nucleo_theme_init(void) {
    load_config_theme();
}

extern "C" bool nucleo_theme_set(const char *id) {
    const nucleo_theme_t *found = NULL;
    for (int i = 0; i < s_theme_count; i++) {
        if (!strcmp(s_themes[i].id, id)) {
            found = &s_themes[i];
            break;
        }
    }
    if (!found) return false;
    apply_theme_struct(found);

    // Save to LittleFS
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "theme", id);
    char *txt = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);

    if (txt) {
        // Save to LittleFS
        FILE *f = fopen(NUCLEO_CFG_MOUNT "/config/theme.json", "w");
        if (f) {
            fputs(txt, f);
            fclose(f);
        }
        // Save backup to SD
        FILE *fsd = fopen(NUCLEO_SD_MOUNT "/apps/theme.cfg", "w");
        if (fsd) {
            fputs(txt, fsd);
            fclose(fsd);
        }
        free(txt);
    }
    return true;
}

extern "C" const char* nucleo_theme_get_current(void) {
    return s_current_theme;
}

extern "C" const nucleo_theme_t* nucleo_theme_get_all(int *count) {
    if (count) *count = s_theme_count;
    return s_themes;
}

// Background image caching
static bool s_bg_checked = false;
static bool s_has_bg = false;

extern "C" bool nucleo_theme_has_bg_image(void) {
    if (!s_bg_checked) {
        FILE *f = fopen(NUCLEO_SD_MOUNT "/system/bg.jpg", "rb");
        if (f) {
            s_has_bg = true;
            fclose(f);
        }
        s_bg_checked = true;
    }
    return s_has_bg;
}

extern "C" void nucleo_theme_draw_bg_slice(void *canvas_ptr, int x, int y, int w, int h) {
    LovyanGFX *canvas = (LovyanGFX*)canvas_ptr;
    if (!canvas) return;

    if (nucleo_theme_has_bg_image()) {
        // Draw the image with an offset to only paint the requested slice
        // M5GFX handles off-screen clipping natively
        // Path-only overload (no Arduino `SD` object — this is ESP-IDF; see app_photos.cpp:93).
        canvas->drawJpgFile(NUCLEO_SD_MOUNT "/system/bg.jpg", -x, -y, 240, 135, 0, 0, 1.0, 1.0, datum_t::top_left);
    } else {
        canvas->fillRect(x, y, w, h, THEME_BG);
    }
}

extern "C" void nucleo_theme_draw_bg(void *canvas_ptr, int width, int height) {
    nucleo_theme_draw_bg_slice(canvas_ptr, 0, 0, width, height);
}

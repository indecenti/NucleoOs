#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *id;
    const char *name;
    uint16_t bg;
    uint16_t fg;
    uint16_t muted;
    uint16_t dim;
    uint16_t line;
    uint16_t ink;
    uint16_t sel;
    uint16_t acc;
} nucleo_theme_t;

// Global active theme colors
extern uint16_t THEME_BG;
extern uint16_t THEME_FG;
extern uint16_t THEME_MUTED;
extern uint16_t THEME_DIM;
extern uint16_t THEME_LINE;
extern uint16_t THEME_INK;
extern uint16_t THEME_SEL;
extern uint16_t THEME_ACC;

// Text/glyph colour for `want` drawn on a `fill` surface, guaranteed legible in EVERY theme: `want` if it
// contrasts enough with `fill`, else THEME_MUTED, else whichever of THEME_FG/THEME_BG contrasts more.
// Needed because themes may alias roles (Hacker/Nano Banana: LINE == MUTED), so MUTED text on a
// LINE-filled chip vanishes. Classic keeps its exact colours (they already clear the bar).
uint16_t nucleo_theme_ink_on(uint16_t fill, uint16_t want);

// Initialize the theme system (loads from NVS / setup.json)
void nucleo_theme_init(void);

// Apply a theme by ID and save it (LittleFS + SD backup)
bool nucleo_theme_set(const char *id);

// Apply a theme by ID WITHOUT saving: live preview while a picker cycles; call nucleo_theme_set()
// once on commit so a held arrow doesn't write two files per step.
bool nucleo_theme_preview(const char *id);

// Get the current theme ID
const char* nucleo_theme_get_current(void);

// Get available themes
const nucleo_theme_t* nucleo_theme_get_all(int *count);

// Render background. If a background image exists on SD, it draws it to the provided LovyanGFX canvas.
// Otherwise it fills with THEME_BG.
void nucleo_theme_draw_bg(void *canvas, int width, int height);

// Check if a background image exists
bool nucleo_theme_has_bg_image(void);

// Draw a slice of the background image (for partial repaints)
void nucleo_theme_draw_bg_slice(void *canvas, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

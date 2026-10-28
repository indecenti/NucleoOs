// Optional external display (ILI9341 320x240 + XPT2046 touch) on the Cardputer ADV EXT header.
// Phase 1: runtime detection + a 1:1 mirror of the internal 240x135 UI, centred. Zero RAM and zero pin
// activity when absent or disabled; refused outright on the original Cardputer. docs/external-display.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Right after nucleo_ui_init(), BEFORE the SD mount: on the ADV, weak internal pull-ups on the default
// LCD CS and touch CS pins, so a module wired at boot stays deselected and cannot drive the shared MISO
// while the card initialises. A pull-up only ever deselects an SPI device; it does nothing else.
void nucleo_extdisp_early(void);

// After the SD mount (reads settings.json "display"), before networking/httpd: probe and, if found (or
// forced on), bring the panel up and start mirroring. Single-threaded boot context only.
void nucleo_extdisp_boot(void);

bool nucleo_extdisp_active(void);        // the mirror is installed
bool nucleo_extdisp_touch(void);         // an XPT2046 answered the probe
const char *nucleo_extdisp_mode(void);   // "auto" | "on" | "off"
const char *nucleo_extdisp_reason(void); // last decision, one line ("detected (...)", "no display found", ...)
const char *nucleo_extdisp_controller(void);   // "ili9341" / "st7789" (driven), "none" when not attached

// Second-screen settings (the Settings app's page). `mode`: 0 auto, 1 always on, 2 off. Live fields (primary,
// fit) apply at once; the others are read at boot, so they take effect after a restart. False = not an ADV.
// `layout`: 0 duplicate (both screens), 1 Cardputer only (the panel stays black), 2 second screen only (the
// Cardputer's own backlight off). Live.
typedef struct { int mode; int mhz; int rotation; bool primary; bool fit; bool touch; int layout; } nucleo_extdisp_conf_t;
bool nucleo_extdisp_conf(nucleo_extdisp_conf_t *live, nucleo_extdisp_conf_t *saved);
bool nucleo_extdisp_set_layout(int layout);  // live: the caller repaints its screen
bool nucleo_extdisp_set_primary(bool on);     // live: the launcher home uses the panel natively (320x240)
bool nucleo_extdisp_set_fit(bool on);         // live: compact screens scaled 4/3; the caller repaints its screen
bool nucleo_extdisp_set_mode(int mode);       // after a restart
bool nucleo_extdisp_set_mhz(int mhz);         // after a restart (10 / 20 / 27 / 40; a saved 80 boots at 40)
bool nucleo_extdisp_set_rotation(int rot);    // after a restart (1 or 3 = upside down)
bool nucleo_extdisp_set_touch(bool on);       // after a restart
bool nucleo_extdisp_recalibrate(void);        // live: false when there is no touch to calibrate right now
// The system bar on the large surface (a store app in front): a 30 px band at the top with a Back button and the
// app's name; the app's surface becomes 320x210 below it and a tap on the band is Back. NULL removes it. True when
// the surface changed (the caller repaints everything). Leaving the large surface removes it by itself.
bool nucleo_extdisp_set_hud(const char *title, unsigned short color);
// True once after something painted over the external panel (the calibration screen): the run loop repaints all.
bool nucleo_extdisp_take_repaint(void);
// The panel's own backlight level, separate from the Cardputer's (settings "external_brightness", 10..100 %).
// Only with the LED on a GPIO (`bl`): false / no-op when it sits on the 5 V rail or no panel is attached.
bool nucleo_extdisp_brightness_ok(void);
int  nucleo_extdisp_brightness(void);
bool nucleo_extdisp_set_brightness(int pct, bool persist);   // live; persist = also write settings.json

// Screenshots (GET /api/screen). which 0 = the Cardputer's own panel (always, even while the external panel
// hosts the screen), 1 = the external panel rebuilt from it (false in solo: nothing to rebuild from).
// Rows are raw panel pixels (byte-swapped RGB565), like nucleo_ui_read_row. `w` must be the size's width.
bool nucleo_extdisp_screen_size(int which, int *w, int *h);

// While the external panel hosts a large screen (solo), the Cardputer's own panel is free: draw on it with
// fn(gfx), gfx being a LovyanGFX* on that panel. False (fn not called) when not in solo or its backlight is off.
bool nucleo_extdisp_draw_internal(void (*fn)(void *gfx));
// The same panel as a persistent LovyanGFX* (240x135), for a screen that keeps drawing there while it runs
// (a game's info card, its pause menu). NULL when not in solo or the Cardputer's backlight is off.
void *nucleo_extdisp_internal_gfx(void);
// Frame hold on/off (on by default). Turn it OFF while another task reads the SD card on the shared bus
// (a hold belongs to the drawing task and only it can hand the bus back early). Call from the drawing task.
void nucleo_extdisp_frame_hold(bool on);
bool nucleo_extdisp_screen_row(int which, int y, int w, uint16_t *out);

#ifdef __cplusplus
}
#endif

// On-device UI for the Cardputer: M5GFX display + integrated keyboard (nucleo_kbd).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

void nucleo_ui_init(void);

// True on the Cardputer ADV (M5GFX auto-detected it), false on the original Cardputer. Lets
// the keyboard layer pick the right backend (ADV = TCA8418 I2C scanner; original = 74HC138
// GPIO matrix). Valid only after nucleo_ui_init() has run d.init().
bool nucleo_ui_is_adv(void);

// Panel backlight, 0..255. Lets a foreground app (e.g. the video player) dim the screen.
void nucleo_ui_set_brightness(unsigned char b);

// Titled message; waits for Enter.
void nucleo_ui_message(const char *title, const char *const *lines, int n);

// Scrollable menu; returns chosen index, or -1 on back.
int nucleo_ui_menu(const char *title, const char *const *items, int n);

// Text entry into buf (NUL-terminated). masked hides chars with '*'.
void nucleo_ui_input(const char *title, char *buf, int len, int masked);

// Static info/home screen (draws and returns immediately, no input wait).
void nucleo_ui_home(const char *title, const char *const *lines, int n);

// Full-screen animated boot splash: a glowing atomic nucleus (Nucleo = nucleus) with three
// electrons weaving through tilted orbits, the NucleoOS wordmark + a loading bar. Blocks for
// ~NUCLEO_SPLASH_MS; any keypress skips it. Self-contained (own 16bpp canvas, no SD/network).
// Call once right after nucleo_ui_init(), before the heavy boot work.
void nucleo_ui_boot_splash(void);

#ifdef __cplusplus
}
#endif

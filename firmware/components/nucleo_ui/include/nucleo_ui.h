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

// Panel size + direct readback (for the /api/screen screenshot endpoint). nucleo_ui_read_row reads
// one physical scanline as BYTE-SWAPPED RGB565 (the Cardputer panel's readback order); the caller
// un-swaps. Works regardless of whether the off-screen canvas is currently allocated.
#include <stdint.h>
void nucleo_ui_panel_size(int *w, int *h);
bool nucleo_ui_read_row(int y, int w, uint16_t *out);

// Titled message; waits for Enter.
void nucleo_ui_message(const char *title, const char *const *lines, int n);

// Scrollable menu; returns chosen index, or -1 on back.
int nucleo_ui_menu(const char *title, const char *const *items, int n);

// Text entry into buf (NUL-terminated). masked hides chars with '*'.
void nucleo_ui_input(const char *title, char *buf, int len, int masked);

// Static info/home screen (draws and returns immediately, no input wait).
void nucleo_ui_home(const char *title, const char *const *lines, int n);

// The SD-content installer's screens (the dedicated install boot). One view struct, mapped from the engine's
// state by main.c so this component stays free of nucleo_sdcontent.
typedef enum {
    NUI_INST_CONNECTING = 0,   // waiting for the Wi-Fi link
    NUI_INST_CHECKING,         // card probe + manifest
    NUI_INST_DOWNLOADING,      // files
    NUI_INST_DONE,
    NUI_INST_FAILED,
} nucleo_ui_inst_phase_t;
typedef struct {
    nucleo_ui_inst_phase_t phase;
    int pct;                   // 0..100 by bytes, <0 unknown
    unsigned kb_done, kb_total;
    int files_done, files_total;
    int eta_s;                 // <0: not known yet
    const char *file;          // the file in flight (may be NULL)
    const char *err;           // FAILED: the localized reason
} nucleo_ui_install_t;

// The live install screen: big percentage, MB done/total, files, time left, the file in flight, and a
// "do not switch off" hint. INCREMENTAL on the panel: the chrome is painted once per phase, then only the
// fields whose value changed (fixed-width, background-filled glyphs: no clear, no flicker, no back-buffer).
void nucleo_ui_install_screen(const nucleo_ui_install_t *v);

// The install's last screen (DONE or FAILED): what happened and what to do next. Returns on ENTER or after
// timeout_s seconds (an unattended device must not sit awake on its 120 mAh battery).
void nucleo_ui_install_result(const nucleo_ui_install_t *v, int timeout_s);

// Force every blocking modal to draw DIRECT to the panel (no 32 KB back-buffer / sprite). The SD-content
// installer sets this so painting its progress bar can't re-allocate the canvas it freed for the TLS heap.
void nucleo_ui_modal_direct(bool on);

// Full-screen animated boot splash: a glowing atomic nucleus (Nucleo = nucleus) with three
// electrons weaving through tilted orbits, the NucleoOS wordmark + a loading bar. Blocks for
// ~NUCLEO_SPLASH_MS; any keypress skips it. Self-contained (own 16bpp canvas, no SD/network).
// Call once right after nucleo_ui_init(), before the heavy boot work.
void nucleo_ui_boot_splash(void);

#ifdef __cplusplus
}
#endif

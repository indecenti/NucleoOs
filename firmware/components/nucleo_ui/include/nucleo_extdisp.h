// Optional external display (ILI9341 320x240 + XPT2046 touch) on the Cardputer ADV EXT header.
// Phase 1: runtime detection + a 1:1 mirror of the internal 240x135 UI, centred. Zero RAM and zero pin
// activity when absent or disabled; refused outright on the original Cardputer. docs/external-display.md.
#pragma once
#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif

// nucleo_pce — the PC Engine (TurboGrafx-16), natively on the Cardputer.
//
// The CPU is MAME's HuC6280 (BSD-3-Clause, vendor/h6280, made plain C by tools/emu-host/gen_h6280.py); the video
// (HuC6270 VDC + HuC6260 VCE), the sound (the PSG of the HuC6280) and the machine are ours, written against MAME's
// BSD-3 PC Engine devices: same registers, same vertical state machine and interrupt points, but drawn a scanline
// at a time instead of a pixel at a time. HuCards are paged from the SD card like every console here (up to the
// 2.5 MB of Street Fighter II, whose bank mapper is supported). No CD-ROM, no SuperGrafx.
//
// The API follows nucleo_nes.h, so the emulators' front-end drives it through the same descriptor. The picture is
// 256x240, one line at a time in RGB565 already in the panel's byte order (wider modes are resampled to 256).
#pragma once
#include "nucleo_gb.h"          // nucleo_gb_stats_t, the button masks

#ifdef __cplusplus
extern "C" {
#endif

#define NUCLEO_PCE_W 256
#define NUCLEO_PCE_H 240

typedef void (*nucleo_pce_line_fn)(const uint16_t *px, int line, void *user);

// ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_SIZE (not a HuCard image), ESP_ERR_NO_MEM.
esp_err_t nucleo_pce_open(const char *rom_path, nucleo_pce_line_fn on_line, void *user);
esp_err_t nucleo_pce_probe(const char *rom_path);
void nucleo_pce_close(void);
void nucleo_pce_reset(void);
void nucleo_pce_run_frame(void);
void nucleo_pce_set_buttons(uint8_t mask);          // NUCLEO_GB_* masks: A = I, B = II, SELECT, START = RUN, d-pad
const char *nucleo_pce_title(void);
void nucleo_pce_set_frameskip(bool on);
esp_err_t nucleo_pce_state_save(int slot);
esp_err_t nucleo_pce_state_load(int slot);
bool nucleo_pce_state_exists(int slot);
void nucleo_pce_save(void);                         // the PC Engine has no battery RAM on HuCards: nothing to do
bool nucleo_pce_autosave(void);
void nucleo_pce_get_stats(nucleo_gb_stats_t *out);
void nucleo_pce_reset_counters(void);
void nucleo_pce_geometry(int *w, int *h);

#ifdef __cplusplus
}
#endif

// nucleo_nes — the NES (Famicom), natively on the Cardputer.
//
// The core is agnes (vendor/agnes, MIT) with three marked patches: the cartridge is read through a callback
// (paged from the SD card like every other console here, never a whole file in RAM), each scanline is handed
// over as it is finished instead of a 61 KB frame buffer, and the APU registers reach the host — agnes has no
// sound, so the APU is ours (nucleo_nes.c). Mappers: NROM, UxROM, MMC1, MMC3 (most of the library).
//
// The API follows nucleo_gb.h / nucleo_gg.h, so the emulators' front-end drives it through the same descriptor.
// The picture is 256x240, one line at a time in RGB565, already in the panel's byte order.
#pragma once
#include "nucleo_gb.h"          // nucleo_gb_stats_t, the button masks

#ifdef __cplusplus
extern "C" {
#endif

#define NUCLEO_NES_W 256
#define NUCLEO_NES_H 240

typedef void (*nucleo_nes_line_fn)(const uint16_t *px, int line, void *user);

// ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_SIZE (not an iNES file), ESP_ERR_NOT_SUPPORTED (a mapper agnes does not
// have), ESP_ERR_NO_MEM.
esp_err_t nucleo_nes_open(const char *rom_path, nucleo_nes_line_fn on_line, void *user);
esp_err_t nucleo_nes_probe(const char *rom_path, int *mapper);
void nucleo_nes_close(void);
void nucleo_nes_reset(void);
void nucleo_nes_run_frame(void);
void nucleo_nes_set_buttons(uint8_t mask);          // NUCLEO_GB_* masks: A, B, SELECT, START and the d-pad
const char *nucleo_nes_title(void);
void nucleo_nes_set_frameskip(bool on);
esp_err_t nucleo_nes_state_save(int slot);
esp_err_t nucleo_nes_state_load(int slot);
bool nucleo_nes_state_exists(int slot);
void nucleo_nes_save(void);                         // battery RAM to the card, if the cartridge has it and it changed
bool nucleo_nes_autosave(void);
void nucleo_nes_get_stats(nucleo_gb_stats_t *out);
void nucleo_nes_reset_counters(void);
void nucleo_nes_geometry(int *w, int *h);

#ifdef __cplusplus
}
#endif

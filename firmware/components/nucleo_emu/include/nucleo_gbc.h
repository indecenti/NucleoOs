// nucleo_gbc — the Game Boy Color, natively on the Cardputer.
//
// The same host as the Game Boy (nucleo_gb.c: ROM page cache on the SD card, battery RAM in banks, saves,
// states, the APU, the real-time clock), compiled a second time around a CGB core: Walnut-CGB, a Peanut-GB
// fork with double speed, banked work and video RAM, colour palettes and HDMA (vendor/README.md). Both
// live in one module (nucleo_gbc.c renames every global of the second copy), and the front-end picks one
// per cartridge from its header: byte 0x143 with bit 7 set = a Color cartridge (0xC0 Color-only, 0x80 also
// runs on a Game Boy, in colour here).
//
// The API is nucleo_gb.h's, renamed nucleo_gbc_*, with one difference: the scanline arrives in colour,
// 160 RGB565 pixels already in the panel's byte order (as nucleo_gg's).
#pragma once
#include "nucleo_gb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*nucleo_gb_line16_fn)(const uint16_t *px, int line, void *user);

// As nucleo_gb_open; never refuses a cartridge for being Color-only. ESP_ERR_NO_MEM also when the 32 KB
// work RAM and 16 KB video RAM blocks cannot be had (the heap's largest block is 32 KB: open it first).
esp_err_t nucleo_gbc_open(const char *rom_path, nucleo_gb_line16_fn on_line, void *user);
esp_err_t nucleo_gbc_probe(const char *rom_path, nucleo_gb_info_t *out);
const char *nucleo_gbc_cart_name(uint8_t type);
void nucleo_gbc_reset(void);
void nucleo_gbc_close(void);
bool nucleo_gbc_is_open(void);
void nucleo_gbc_run_frame(void);
void nucleo_gbc_set_buttons(uint8_t mask);
const char *nucleo_gbc_title(void);
void nucleo_gbc_set_frameskip(bool on);
void nucleo_gbc_set_interlace(bool on);
esp_err_t nucleo_gbc_state_save(int slot);
esp_err_t nucleo_gbc_state_load(int slot);
bool      nucleo_gbc_state_exists(int slot);
void nucleo_gbc_save(void);
bool nucleo_gbc_autosave(void);
void nucleo_gbc_get_stats(nucleo_gb_stats_t *out);
void nucleo_gbc_get_regs(nucleo_gb_regs_t *out);
void nucleo_gbc_reset_counters(void);

// True for a cartridge header that asks for the Game Boy Color (byte 0x143 has bit 7 set).
static inline bool nucleo_gbc_wants(uint8_t cgb_flag) { return (cgb_flag & 0x80) != 0; }

#ifdef __cplusplus
}
#endif

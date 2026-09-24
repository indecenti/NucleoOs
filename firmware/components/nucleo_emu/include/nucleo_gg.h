// nucleo_gg — Sega Game Gear emulation for the Cardputer: our own VDP/PSG/mapper around the vendored
// z80emu CPU (vendor/z80emu, "free, do whatever you want with it").
//
// WHY OUR OWN MACHINE AND NOT SMS PLUS
// Every SMS/GG core small enough for a microcontroller descends from SMS Plus, and every one of them
// ships MAME's Z80 ("freeware for non-commercial purposes", retroactively revocable) on top of a GPL
// body, keeps 16 KB of VRAM in .bss, reads the ROM through a flat pointer and wants 32-320 KB of tile
// caches or lookup tables. Adapting one meant rewriting nearly every file; the machine itself is a
// few hundred lines. docs/native-emulation.md §7 has the comparison.
//
// SAME SHAPE AS nucleo_gb, ON PURPOSE. The Game Gear screen is 160x144 — exactly the Game Boy's — so
// the gbemu front-end (shelf, menu, scaler, saves, Solo boot) drives both through the same calls.
// The whole console is one ~25 KB heap block (8 KB WRAM + 16 KB VRAM + CPU/VDP/PSG), the picture
// leaves one line at a time, and the cartridge streams from the SD card through 1 KB pages.
//
// EVERYTHING IS HEAP-ON-OPEN: nothing here lives in .bss (docs/memory-budget.md).
#pragma once
#include "esp_err.h"
#include "nucleo_gb.h"      // the shared button bits and the stats struct the front-end reads
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUCLEO_GG_W 160
#define NUCLEO_GG_H 144

// One finished line of RGB565 pixels in PANEL byte order (high byte first), so the front-end copies
// them to the display without a per-pixel swap. Called once per visible line, in order: 144 lines of
// 160 pixels for a Game Gear cartridge, 192 lines of 256 for one running in Master System mode (a .sms
// file, or one of the Game Gear releases that asks for that mode) — see nucleo_gg_geometry().
typedef void (*nucleo_gg_line_fn)(const uint16_t *px, int line, void *user);

// Open a cartridge from the SD card. Errors, each one a thing the player can be told in words:
//   ESP_ERR_NOT_FOUND        no such file
//   ESP_ERR_INVALID_SIZE     smaller than any Game Gear cartridge
//   ESP_ERR_NO_MEM           the heap cannot host console + page cache (open from a Solo boot)
esp_err_t nucleo_gg_open(const char *rom_path, nucleo_gg_line_fn on_line, void *user);

// Header facts without opening a session — no allocation.
typedef struct {
    uint32_t rom_bytes;
    bool     codemasters;    // Codemasters mapper (Micro Machines, Dizzy...) — detected, supported
    bool     sms;            // a Master System cartridge (.sms); SMS-mode .gg ones are known after open
} nucleo_gg_info_t;
esp_err_t nucleo_gg_probe(const char *rom_path, nucleo_gg_info_t *out);

// The open session's picture: 160x144 (Game Gear) or 256x192 (Master System mode).
void nucleo_gg_geometry(int *w, int *h);

void nucleo_gg_reset(void);          // power-cycle the cartridge; battery RAM is flushed and kept
void nucleo_gg_close(void);          // persist battery RAM and release everything; safe when closed
bool nucleo_gg_is_open(void);
void nucleo_gg_run_frame(void);      // one NTSC frame (262 lines, ~59.92 Hz); 144 line callbacks

// Buttons use the Game Boy bits so one front-end drives both consoles, mapped by POSITION:
// NUCLEO_GB_B (left thumb) = button 1, NUCLEO_GB_A (right thumb) = button 2, START = Start (Pause in
// Master System mode).
void nucleo_gg_set_buttons(uint8_t mask);

const char *nucleo_gg_title(void);   // the cartridge's file name, tidied; "" when closed
void nucleo_gg_set_frameskip(bool on);   // render every other frame (the CPU still runs every frame)

// Save states: "<SD>/data/Saves/gg/<rom name>.st<slot>". Pointers are never taken from the file.
esp_err_t nucleo_gg_state_save(int slot);
esp_err_t nucleo_gg_state_load(int slot);
bool      nucleo_gg_state_exists(int slot);

// Battery RAM: "<SD>/data/Saves/gg/<rom name>.sav", a flat 32 KB image like every other SMS/GG
// emulator writes (128 bytes for the 93C46 EEPROM cartridges). Only pages the game touched cost RAM.
void nucleo_gg_save(void);
bool nucleo_gg_autosave(void);       // writes once the RAM has been quiet ~1.5 s; true when it wrote

void nucleo_gg_get_stats(nucleo_gb_stats_t *out);
void nucleo_gg_reset_counters(void);

#ifdef __cplusplus
}
#endif

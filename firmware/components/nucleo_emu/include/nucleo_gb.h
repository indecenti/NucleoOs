// nucleo_gb — Game Boy (DMG) emulation for the Cardputer, on top of the vendored Peanut-GB core.
//
// WHY THIS IS POSSIBLE ON A BOARD WITH NO PSRAM
// The core's whole state is one ~17.2 KB struct (8 KB WRAM + 8 KB VRAM + 160 B OAM + 256 B HRAM/IO
// + CPU/PPU registers) and it has NO framebuffer: the PPU emits one scanline at a time through a
// host callback, so the display costs a 160-byte line buffer instead of the 23-46 KB a frame would.
// The cartridge is reached through another callback, so the ROM need not be resident either.
//
// That matters because RAM here is measured in CONTIGUOUS blocks, not totals: the boot trace shows
// the largest free block collapsing to ~31.7 KB the moment the shared 32,400-byte UI canvas is
// allocated. Only a Solo boot (fresh, unfragmented heap) with Wi-Fi skipped and the canvas released
// gets it back toward ~60 KB — which is the budget this module is designed to live inside.
//
// EVERYTHING IS HEAP-ON-OPEN. Nothing here lives in .bss: an emulator that is closed 99% of the time
// must cost nothing while closed (see docs/memory-budget.md, and the boot-RAM lessons in CLAUDE.md).
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUCLEO_GB_W 160
#define NUCLEO_GB_H 144

// One finished scanline. `pixels` holds NUCLEO_GB_W bytes, each 0..3 (DMG shades, 0 = lightest) in
// the low two bits — the caller maps them to panel colours. Called from inside nucleo_gb_run_frame,
// once per visible line, in order. Keep it cheap: it runs 144 times per frame.
typedef void (*nucleo_gb_line_fn)(const uint8_t *pixels, int line, void *user);

// Open a ROM from the SD card. Allocates the core state and the ROM cache; returns ESP_ERR_NO_MEM if
// the heap cannot host them (the caller should have entered a Solo/exclusive window first).
esp_err_t nucleo_gb_open(const char *rom_path, nucleo_gb_line_fn on_line, void *user);

// Persist cartridge RAM (if the cart has any) and release everything. Safe to call when not open.
void nucleo_gb_close(void);
bool nucleo_gb_is_open(void);

// Run exactly one frame (~59.7 Hz on real hardware). Invokes the line callback 144 times.
void nucleo_gb_run_frame(void);

// Button state, held between calls. Bit set = pressed.
#define NUCLEO_GB_A      0x01
#define NUCLEO_GB_B      0x02
#define NUCLEO_GB_SELECT 0x04
#define NUCLEO_GB_START  0x08
#define NUCLEO_GB_RIGHT  0x10
#define NUCLEO_GB_LEFT   0x20
#define NUCLEO_GB_UP     0x40
#define NUCLEO_GB_DOWN   0x80
void nucleo_gb_set_buttons(uint8_t mask);

// Cartridge title from the ROM header (16 bytes max, NUL-terminated), or "" when closed.
const char *nucleo_gb_title(void);

// Write cartridge RAM to <rom>.sav now. No-op when the cart has no battery RAM or nothing changed.
// Called automatically by nucleo_gb_close(); exposed so a long session can checkpoint.
void nucleo_gb_save(void);

// Diagnostics — what this session actually costs and how the ROM is being served.
typedef struct {
    size_t   heap_bytes;     // total allocated by this module right now
    uint32_t rom_bytes;      // ROM file size
    bool     rom_resident;   // true = whole ROM in RAM (no SD traffic while playing)
    uint32_t bank_misses;    // cache misses since open (each is a 16 KB SD read)
    uint32_t frames;         // frames run since open
} nucleo_gb_stats_t;
void nucleo_gb_get_stats(nucleo_gb_stats_t *out);

#ifdef __cplusplus
}
#endif

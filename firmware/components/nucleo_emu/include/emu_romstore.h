// emu_romstore — the cartridge from internal flash, for the cores (firmware nucleo_app/include/nucleo_romstore.h,
// ABI level 7), declared here: nucleo_emu sits below nucleo_app in the component graph.
//
// A core first tries the whole cartridge in RAM (when it fits with room to spare), then nucleo_rom_map (copied once
// into the spare OTA slot, then mapped and read like RAM), and only then its page cache from the SD card.
// emu_rom_progress is the front-end's box for a first copy (seconds per megabyte), or NULL; the front-end defines it.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const uint8_t *nucleo_rom_map(const char *path, uint32_t *size, void (*progress)(uint32_t done, uint32_t total, void *user),
                              void *user);
void nucleo_rom_unmap(void);
extern void (*emu_rom_progress)(uint32_t done, uint32_t total, void *user);

#ifdef __cplusplus
}
#endif

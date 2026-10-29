// nucleo_romstore — a cartridge read straight from the internal flash, mapped into the address space.
//
// The emulators read their cartridges from the SD card through a small page cache in RAM. The card is slow to seek
// (milliseconds a page on the ADV, whose panel shares its bus) and the cache is what the heap has left: a big HuCard
// that reads a little of many places a frame (Street Fighter II, 2.5 MB) spends most of its time waiting for the card.
// The flash is 8 MB, read through the MMU and the CPU's data cache. While a game runs in a Solo boot nothing needs
// the OTA slot the device is NOT running (it keeps the previous firmware only until the running one is confirmed),
// so the cartridge is copied there once and mapped: the emulator reads it like RAM, the page cache and every card
// read are gone, and the RAM they took goes back to the game. The next OTA simply overwrites the copy; the next
// game that wants it copies it again.
//
//   nucleo_rom_map(path, &size, progress, user)   the file's bytes, mapped (read-only), or NULL: not possible here
//                                                  (no spare slot, the running firmware not yet confirmed, hosted by
//                                                  M5Launcher whose slots hold other apps, the file too big, a read
//                                                  error). The first time for a file it is copied (seconds per MB;
//                                                  progress(done, total, user) is called as it goes); after that,
//                                                  the same file (path, size, date, first 4 KB) maps at once.
//   nucleo_rom_unmap()                             the mapping goes; the copy stays for next time.
//
// One mapping at a time. Exported to store modules at ABI level 7.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const uint8_t *nucleo_rom_map(const char *path, uint32_t *size, void (*progress)(uint32_t done, uint32_t total, void *user),
                              void *user);
void nucleo_rom_unmap(void);

#ifdef __cplusplus
}
#endif

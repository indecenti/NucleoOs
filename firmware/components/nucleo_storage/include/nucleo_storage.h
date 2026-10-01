// SD filesystem: mount, first-boot provisioning, capacity. See docs/storage.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     mounted;
    char     fs_type[8];     // "FAT32" | "exFAT" | "?"
    uint64_t total_bytes;
    uint64_t free_bytes;
    esp_err_t mount_error;
} nucleo_storage_info_t;

// Mount the power-loss-safe config store (LittleFS on internal flash) at
// NUCLEO_CFG_MOUNT. Formats the partition on first boot. Independent of the SD —
// call early so brick-class config is available even when no card is inserted.
esp_err_t nucleo_storage_mount_cfg(void);

// Mount the SD card over SPI at NUCLEO_SD_MOUNT.
esp_err_t nucleo_storage_mount(void);

// Ensure the system directory tree exists and write /system/volume.json on
// first boot. Safe to call every boot (idempotent).
esp_err_t nucleo_storage_provision(void);

// Recompute free/total space (call after mount and on storage.changed).
esp_err_t nucleo_storage_refresh(void);

// Cached capacity snapshot (valid after a successful mount + refresh).
const nucleo_storage_info_t *nucleo_storage_info(void);

// Graceful shutdown: flush + cleanly unmount the SD and the config store — the OS equivalent of
// `sync` on the way down. Registered as an esp_register_shutdown_handler() at boot, so it runs on
// every clean esp_restart() (OTA, /api/reboot, native reboot, Wi-Fi reset, USB-MSC), but NOT on a
// panic. The filesystems are gone afterwards, which is fine: the device reboots immediately.
void nucleo_storage_sync(void);

// The mounted SD handle (NULL if not mounted). Used by the USB Mass Storage mode to expose the
// card's raw blocks to a host PC. Returned as void* so callers needn't pull in the sdmmc headers
// (cast to sdmmc_card_t* on use).
void *nucleo_storage_card(void);

// Whole-card erase (Settings > Reset > Erase SD). Formatting a mounted card while the OS holds files open
// on it (the ANIMA index, logs, audio) would leave dangling handles writing into the fresh FAT, so the erase
// is ARMED here — a marker on the internal /cfg store, carrying the UI language for the boot screen — and
// RUN on the next boot by main.c, right after the mount and before anything opens a file on the card.
bool nucleo_storage_format_arm(const char *lang);
// True when armed; `lang` (>= 3 bytes, may be NULL) receives the language stored with the marker.
bool nucleo_storage_format_pending(char *lang);
// Erase the mounted card (FAT32, the mount's 16 KB allocation unit). Clears the marker FIRST, so a crash
// or a failure can never turn into a format-at-every-boot loop.
esp_err_t nucleo_storage_format_now(void);

#ifdef __cplusplus
}
#endif

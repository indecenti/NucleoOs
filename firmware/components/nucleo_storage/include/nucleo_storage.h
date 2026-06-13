// SD filesystem: mount, first-boot provisioning, capacity. See docs/storage.md.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

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

// The mounted SD handle (NULL if not mounted). Used by the USB Mass Storage mode to expose the
// card's raw blocks to a host PC. Returned as void* so callers needn't pull in the sdmmc headers
// (cast to sdmmc_card_t* on use).
void *nucleo_storage_card(void);

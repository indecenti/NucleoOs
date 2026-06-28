# Partition Table (8 MB flash)

Dual-bank OTA layout (`ota_0` / `ota_1`), no `factory`. Firmware lives in internal
flash; **all content lives on the SD card**, so the app banks only hold the firmware
image. Total = 0x800000 (8 MB).

```csv
# Name,     Type, SubType,  Offset,    Size,      Notes
nvs,        data, nvs,      0x9000,    0x6000,    # config / key-value
otadata,    data, ota,      0xF000,    0x2000,    # active-slot selector + rollback flag
phy_init,   data, phy,      0x11000,   0x1000,    # RF calibration
nvs_keys,   data, nvs_keys, 0x12000,   0x1000,    # NVS encryption keys
ota_0,      app,  ota_0,    0x20000,   0x380000,  # 3.5 MB app bank A
ota_1,      app,  ota_1,    0x3A0000,  0x380000,  # 3.5 MB app bank B
coredump,   data, coredump, 0x720000,  0x40000,   # 256 KB crash dumps
cfg,        data, spiffs,   0x760000,  0xA0000,   # 640 KB LittleFS config store
```

## Rationale

- **No `factory`.** The old layout carried three app banks (factory 2.5 MB + 2×OTA 2.25 MB
  = 7 MB of the 8). But `idf.py flash` only ever writes one bank and OTA is the redundancy
  mechanism, so `factory` was dead weight. Removing it folds 2.5 MB back into the two OTA
  banks: **2.25 → 3.5 MB each**. The ~1.98 MB image now sits at ~55% (≈45% headroom), up from
  the old ~14% against the 2.25 MB slot.
- **Dual OTA (`ota_0`/`ota_1`)** must be equal size → 3.5 MB each. This is the size `idf.py`
  checks the image against, and either bank can hold the next update. A/B rollback gives the
  "never brick" guarantee `factory` used to provide.
- **Serial flash writes `ota_0`.** `idf.py flash` writes the app to the first app bank and
  rewrites `otadata` to its initial state, so a cable reflash boots `ota_0` cleanly. On a
  healthy boot `main.c` calls `esp_ota_mark_app_valid_cancel_rollback()`; on a bad boot the
  bootloader rolls back to the other bank. (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.)
- **Migration is cable-only.** `nvs`/`otadata`/`phy_init`/`nvs_keys`/`coredump`/`cfg` KEEP their
  exact offsets, so a cable reflash preserves Wi-Fi/pairing (nvs) and OS config (cfg) — no full
  erase needed. But a partition-table change itself is **not** OTA-safe: do the one-time switch to
  this layout over USB cable (`tools\flash.ps1 -Port COM3`), not via `POST /api/ota`.
- **`coredump`** captures crashes for the Log Viewer / time-travel debug (ELF, to-flash).
- **`nvs_keys`** enables encrypted NVS for the device keypair (bundle signing, pairing).
- **`cfg` (640 KB, LittleFS)** holds brick-class OS config (`setup.json`) on internal flash.
  LittleFS is copy-on-write with atomic rename, so a power cut mid-write cannot corrupt it the
  way FAT on the SD can. Mounted at `/cfg`; formatted on first boot. The SD stays FAT for bulk
  content (media, ROMs); only power-loss-critical config moves here. Subtype `spiffs` is just a
  valid data subtype — esp_littlefs mounts by label.

## OTA + rollback flow

1. New image downloaded into the inactive bank (`esp_ota_get_next_update_partition`), signature verified.
2. `otadata` set to boot the new bank with `ESP_OTA_IMG_NEW` (pending verify).
3. On boot, run health check; on success call `esp_ota_mark_app_valid_cancel_rollback()`.
4. On crash or missing confirmation, the bootloader rolls back to the previous valid bank.
5. If **both** banks are invalid, recover via USB-cable reflash (`tools\flash.ps1`).

## Future: SD-staged single-bank OTA

If the image ever outgrows ~3.5 MB, the next step is one large app bank (~6.5 MB) plus a small
recovery bank (~0.5 MB), with the staged image and last-known-good copy held on the SD card. That
maximises app space while keeping OTA safe, but it's a custom OTA subsystem (a dedicated recovery
app + flash-from-SD logic + FAT-corruption handling), not a table edit — only worth it past 3.5 MB.

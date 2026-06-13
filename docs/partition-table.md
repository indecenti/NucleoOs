# Partition Table (8 MB flash)

Custom layout with dual OTA slots plus a minimal recovery (`factory`) partition.
Firmware lives in flash; **all content lives on the SD card**, so app slots only hold
the firmware image. Total = 0x800000 (8 MB).

```csv
# Name,     Type, SubType,  Offset,    Size,      Notes
nvs,        data, nvs,      0x9000,    0x6000,    # config / key-value
otadata,    data, ota,      0xF000,    0x2000,    # active-slot selector
phy_init,   data, phy,      0x11000,   0x1000,    # RF calibration
nvs_keys,   data, nvs_keys, 0x12000,   0x1000,    # NVS encryption keys
factory,    app,  factory,  0x20000,   0x280000,  # 2.5 MB primary image (serial-flashed)
ota_0,      app,  ota_0,    0x2A0000,  0x240000,  # 2.25 MB app slot A
ota_1,      app,  ota_1,    0x4E0000,  0x240000,  # 2.25 MB app slot B
coredump,   data, coredump, 0x720000,  0x40000,   # 256 KB crash dumps
cfg,        data, spiffs,   0x760000,  0xA0000,   # 640 KB LittleFS config store
```

## Rationale

- **Dual OTA (`ota_0`/`ota_1`)** must be equal size → 2.25 MB each, ample for the ~1.44 MB image
  (≈37% headroom). This is the *smallest* app partition, so it's the size `idf.py` checks the image
  against.
- **`factory` (2.5 MB)** is the primary image: OTA is unreliable on this board, so serial flash is
  the real path (see `serial-console-routing` memory). It also boots if both OTA slots fail their
  health check — never brick. History: 1.5→2 MB in 2026-06 when TinyUSB MSC (USB Drive) pushed the
  image past 1.5 MB; then 2→2.5 MB on 2026-06-06 for headroom after the Lua interpreter was removed
  (−106 KB), the 0.5 MB coming from the OTA slots (2.5→2.25 MB each). `nvs`/`otadata`/`nvs_keys`/
  `coredump`/`cfg` KEEP their exact offsets so a cable reflash preserves Wi-Fi/pairing (nvs) and OS
  config (cfg) — no full erase needed. **Changing this table needs a full USB-cable flash**
  (`tools\flash.ps1 -Port COM3`) — a partition-table change isn't OTA-safe.
- **`otadata`** tracks the active slot and the rollback flag.
- **`coredump`** captures crashes for the Log Viewer / time-travel debug.
- **`nvs_keys`** enables encrypted NVS for the device keypair (bundle signing, pairing).
- **`cfg` (640 KB, LittleFS)** holds brick-class OS config (`setup.json`) on internal
  flash. LittleFS is copy-on-write with atomic rename, so a power cut mid-write cannot
  corrupt it the way FAT on the SD can. Mounted at `/cfg`; formatted on first boot.
  The SD stays FAT for bulk content (media, ROMs); only power-loss-critical config moves
  here. Subtype `spiffs` is just a valid data subtype — esp_littlefs mounts by label.

## OTA + rollback flow

1. New image downloaded into the inactive slot, signature verified.
2. `otadata` set to boot the new slot with `ESP_OTA_IMG_NEW` (pending verify).
3. On boot, run health check; on success call `esp_ota_mark_app_valid_cancel_rollback()`.
4. On crash or missing confirmation, bootloader rolls back to the previous valid slot.
5. If both OTA slots are invalid, boot `factory` recovery.

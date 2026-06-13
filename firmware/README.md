# NucleoOS firmware

ESP-IDF skeleton for the Cardputer (ESP32-S3). This first slice boots, mounts the SD card,
provisions the system tree, detects capacity, loads the app registry, and serves
`GET /api/status` over a temporary setup Wi-Fi AP.

## Component layout (scalable: one concern per component)

```
main/                 boot orchestration (app_main)
components/
  nucleo_board/       board pin map (verify against your Cardputer)
  nucleo_storage/     SD mount + first-boot provisioning + capacity
  nucleo_registry/    reads /system/registry/apps.json
  nucleo_httpd/       HTTP server + /api/status
```

## Prerequisites

- ESP-IDF v5.x installed and exported (`. $IDF_PATH/export.sh`).
- For a 64 GB **exFAT** card, enable exFAT in
  `idf.py menuconfig` → Component config → FAT Filesystem support.
  Otherwise format the card as **FAT32**.

## Prepare the SD card

Use the incremental deploy tool — it assembles the full card layout (system, apps,
web shell, media, and the downloadable Windows app) and copies **only changed files**
(SHA-256, atomic, verified, with mirror-delete):

```
# Assemble the staging tree (repo -> deploy/sd):
powershell -ExecutionPolicy Bypass -File tools\deploy.ps1
# Then push it to the SD drive (e.g. H:) — only changed files are written:
powershell -ExecutionPolicy Bypass -File tools\deploy.ps1 -To H:\
```

(The firmware also auto-creates the empty system tree on first boot if missing.)

## Build, flash, monitor

```
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

## Verify

1. Watch the serial log: `SD mounted`, `<fs>: <N> MB total/free`, `loaded N installed apps`.
2. Join Wi-Fi `NucleoOS-Setup` (password `nucleoos`).
3. Open `http://192.168.4.1/api/status` — expect device info, SD capacity and app count.

## Next slices (TODO)

- Event bus (event-sourced, `/journal`) + WebSocket delta sync.
- BLE GATT transport (offline / Android).
- Display (ST7789) boot + recovery UI, keyboard input.
- OTA update + rollback, bundle signature verification.

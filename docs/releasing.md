# Releasing & Over-the-Air Updates

NucleoOS ships in **two independent layers**. Knowing which layer a change touches
tells you how to release it. Mixing them up is the most common mistake.

| Layer | Lives in | What it is | Update channel |
|-------|----------|------------|----------------|
| **Firmware** | internal flash (`ota_0` / `ota_1`) | the C application (`nucleoos.bin`) | USB-serial flash **or** network OTA (`POST /api/ota`) |
| **Web layer** | microSD (`/www/shell`, `/apps/*`, `/system/registry`) | shell, apps, registry — served from SD | SD sync (`deploy.ps1 -To`) **or** network push (`push-ota.mjs`) |

> A firmware flash does **not** touch the SD, and a web update does **not** touch
> the firmware. Most day-to-day changes (apps, shell, icons) are **web-layer only**
> and never require reflashing.

> **Versioning:** the firmware version auto-increments on every build and is reported identically
> by `/api/status`, `/proc/version`, mDNS, and the serial boot banner — see
> [versioning.md](versioning.md). `release.ps1` prints `was vX -> now vY (OTA confirmed)` as
> deploy proof. The web layer versions separately (`sw.js` cache tag + per-app manifest).

---

## 1. Firmware OTA (over Wi-Fi, no cable)

### How it works
- The partition table reserves two app slots, `ota_0` and `ota_1`
  (see [partition-table.md](partition-table.md)).
- `POST /api/ota` streams the new `nucleoos.bin` into the **inactive** slot
  (`esp_ota_begin/write/end`), sets it as the boot partition, and reboots.
  The first byte is checked for the ESP image magic (`0xE9`) so junk uploads
  fail fast with HTTP 400.
- **Rollback safety (always-bootable guarantee):** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`.
  After an OTA the new image boots in state `PENDING_VERIFY`. `app_main()` calls
  `ota_confirm_if_pending()` **only after the core services are up** (SD attempt +
  network + HTTP server). That call (`esp_ota_mark_app_valid_cancel_rollback`) marks
  the image valid. If the new firmware crashes or hangs before that point, it is
  never confirmed, and the bootloader **automatically rolls back** to the previous
  working image on the next reset. The device cannot brick itself with a bad OTA.

> ⚠️ **The mark-valid call is mandatory.** If you ever refactor `app_main()`, keep
> `ota_confirm_if_pending()` on the healthy-boot path. Without it, every OTA image
> is rolled back on its second boot — the OS would silently revert after each update.

### Image state, visible at `/api/status`
```jsonc
"ota": {
  "running": "ota_0",      // active partition label
  "next": "ota_1",         // where the next OTA will be written
  "state": "valid",        // valid | pending | new | invalid | aborted | unknown
  "rollback_enabled": true
}
```
- `valid` — confirmed, safe.
- `pending` — booted from OTA, not yet confirmed (normal in the first seconds after
  an update, before the healthy-boot check runs).

### How to release a firmware update
1. **Build:** `tools\flash.ps1 -BuildOnly` (or full `tools\flash.ps1 -Port COM3` for the
   first/USB flash). Confirm exit 0 and the binary fits (`check_sizes` shows free %).
2. **Deliver — pick one:**
   - **Network (preferred):** open the **Updates** app in the shell (any browser
     pointed at the device), choose `firmware/build/nucleoos.bin`, click *Install
     firmware*. Progress → auto-reboot → it polls until the device is back and shows
     the new state. No cable.
   - **USB-serial:** `tools\flash.ps1 -Port COM3` (needed for the very first flash of a
     blank device, or to recover one).
3. **Verify:** `/api/status` → `ota.state` returns to `valid` after the device settles.

> The **first** firmware that contains the OTA endpoint + mark-valid logic must be
> flashed over USB (bootstrap). From then on, every firmware update can be done over
> Wi-Fi from the Updates app.

### ⚠️ Boot-test on hardware BEFORE OTA — the gate does not measure device RAM
The ANIMA host gate (`anima:gate`) runs on the PC and proves the *logic*, **not** that the
firmware boots on a no-PSRAM ESP32-S3. A build can pass every gate, link cleanly, fit the
partition — and still **reboot-loop** because `httpd` can't get a contiguous block at boot
(see [memory-budget.md](memory-budget.md) "boot-time httpd gate"). The failure mode is a black
screen + continuous reboots. So for **any firmware change that could affect RAM** (new static
buffers, a new boot service, a bigger array), flash **one** unit over USB first and read the
serial `BOOTSTEP` log — it must reach `BOOTSTEP httpd` (not `httpd-FAILED` / `abort()`), with
`pre-httpd` `largest` block comfortably above ~10 KB. Only then OTA the rest.

> **Discipline:** never allocate boot RAM for a feature an app uses — allocate it on the app's
> `enter()` and free it on `exit()`. A 30 KB static `.bss` regression is the whole margin
> between "boots" and "loops". See [memory-budget.md](memory-budget.md).

### Serial recovery — un-brick a reboot-looping unit (no fresh build needed)
A bad firmware in `ota_0` does **not** destroy the previous good firmware: USB-serial
`idf.py flash` writes only `ota_0` and resets `otadata`, leaving the prior image intact in
`ota_1`. To recover instantly, point boot back at `ota_1`:
```powershell
. C:\esp\esp-idf\export.ps1
$ot = "$env:IDF_PATH\components\app_update\otatool.py"
python $ot -p COM3 --baud 115200 switch_ota_partition --slot 1   # boot the previous good image
```
Two ESP32-S3 USB-Serial-JTAG gotchas (M5 Cardputer has no real RTS/DTR wired to EN/GPIO0):
- **Stuck in download mode** after a flash (`boot:0x3 (DOWNLOAD)`, "waiting for download",
  black screen): the `--after hard_reset` (RTS) does not release it. Force a clean app boot with
  the RTC watchdog reset: `python -m esptool --chip esp32s3 -p COM3 --after watchdog_reset flash_id`.
- **Reading the panic:** the console is on USB-Serial-JTAG = the same COMx as flashing
  (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`). Open the port at 115200 right after a watchdog reset
  to capture the `ESP-ROM` banner → `BOOTSTEP` lines → the `abort()` backtrace.

The board enumerates as **COM3↔COM4** (one per unit); detect the live port, don't assume.

---

## 2. Web layer release (shell + apps + registry)

The shell and apps are static files served from the SD card, so they version
independently of the firmware. Writes go through the device's atomic file API
(`/api/fs/write` = temp file + rename), so a half-finished transfer never corrupts
a live file.

### Always do these after changing web code
1. `npm run validate` — manifests + registry + associations must be green.
2. If you changed anything under `web/shell/` (shell.js, wm.js, style.css, sw.js, …),
   **bump the service-worker cache version** in `web/shell/sw.js` (`nucleo-shell-vN`).
   Browsers cache the shell; without the bump, clients keep the old assets.
   - App UIs under `apps/*/www` are **not** in the SW cache — they reload fresh, so
     they do **not** need a bump.
3. `tools\deploy.ps1` — assembles the repo into `deploy/sd/` (hash-based, incremental).

### Deliver — pick one
- **Network push (no SD removal):**
  `npm run push-ota -- --host http://<device-ip>`
  Mirrors `deploy/sd/{www, apps, system/registry}` onto the running device over HTTP,
  incrementally (reads each file first, writes only what changed). Use `--dry-run` to
  preview, `--only <subtree>` to scope. It **never** touches `system/config` (user
  state: pins, wallpaper, settings, session) or `data/` (media, ROMs).
- **SD sync (card in the PC):**
  `tools\deploy.ps1 -To H:\` (the `-To` guard refuses non-removable / system drives).
  Then put the card back in the Cardputer.

After either, reload the shell in the browser; the bumped SW pulls the new assets.

> **SD workflow reminder:** the card is either in the PC (for `-To` sync) **or** in
> the Cardputer (running). Not both. If the card is in the device, use `push-ota`.

---

## Release checklist

**Web-layer change (app / shell / icon / registry):**
- [ ] `npm run validate` green
- [ ] SW cache bumped in `web/shell/sw.js` (only if `web/shell/*` changed)
- [ ] `tools\deploy.ps1` re-staged `deploy/sd`
- [ ] delivered via `push-ota` (device) or `deploy.ps1 -To` (card in PC)
- [ ] shell reloaded; change visible

**Firmware change (C):**
- [ ] `flash.ps1 -BuildOnly` exit 0, binary fits the app partition
- [ ] `ota_confirm_if_pending()` still on the healthy-boot path in `main.c`
- [ ] **if RAM-affecting** (new static buffer / boot service / bigger array): boot-test ONE
      unit over USB, serial `BOOTSTEP` log reaches `httpd` (not `abort()`), before OTAing others
- [ ] delivered via Updates app (Wi-Fi) or `flash.ps1 -Port COMx` (USB)
- [ ] `/api/status` → `ota.state: valid` after the device settles

---

## Security note
`/api/ota`, `/api/fs/*`, `/api/rec/*` and `/ws` now require a **paired session** — a
6-digit PIN shown on the Cardputer screen, exchanged for an HttpOnly session cookie. So
the OTA/push tools (`tools/ota.ps1`, `tools/push-ota.mjs`) and any browser must pair first.
See [security.md](security.md) for the full model. Still **HTTP, not HTTPS** — keep the
device on a trusted LAN. Remaining hardening:
- **Signed images:** verify an **Ed25519** signature of `nucleoos.bin` before
  `esp_ota_set_boot_partition`, matching the app-signing model in
  [architecture.md](architecture.md). Until then, rollback (not signing) is what keeps
  the device bootable.

See also: [partition-table.md](partition-table.md), [storage.md](storage.md),
[roadmap.md](roadmap.md).

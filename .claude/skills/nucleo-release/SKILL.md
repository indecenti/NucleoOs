---
name: nucleo-release
description: Build, gate, and ship NucleoOS to the Cardputer — firmware via OTA and the web/SD payload via the device file API. Use when the user asks to release, deploy, flash, OTA, sync the SD, or push a web app to the device. Covers the one-command release, firmware-only and SD-only paths, and the gotchas (PIN, .gz shadowing, never /MIR).
---

# Releasing NucleoOS to the device

**Gate first.** The ANIMA host regression suite (`npm run anima:gate`, 12 gates incl.
0-hallucination + skill-routing) must be GREEN before shipping firmware that carries that
logic. `flash.ps1` and `release.ps1` enforce it; `-SkipGate` overrides — don't, unless the
user says so.

**Only release when the user explicitly asks.** Building, gating and host-testing are free;
sd-sync / deploy / flash / OTA are not — they push to the device and are hard to reverse.

Run all `.ps1` scripts from the **PowerShell tool** as a child process (not via Bash).

## Versioning — automatic, single source of truth
Every build gets a unique, identifiable version. `flash.ps1` calls `tools\version-bump.ps1`
first, which increments the build counter in `firmware/version/BUILD`; `version.cmake` composes
`PROJECT_VER = <semver>+<build>.g<git>[*]` and bakes it into the app descriptor. The SAME string
then shows up in `/api/status` (`version`), `/proc/version`, the mDNS `ver` TXT record, and the
**serial boot banner** — so you can read the running version from the API or from COM and they
agree. `release.ps1` prints `was vX -> now vY (OTA confirmed)` after the reboot as deploy proof.
- **Just building/OTA?** Do nothing — the build counter auto-increments. `0.2.0+5.gabc1234`.
- **Cutting a real release?** Bump semver first: `tools\version-bump.ps1 -Bump patch|minor|major`
  (resets the build counter to 0), then release. Commit the `firmware/version/*` change.
- `*` suffix = built from an uncommitted (dirty) tree. `-NoBump` rebuilds the same version.
See `docs/versioning.md`.

## One-command release (preferred)
```
powershell -ExecutionPolicy Bypass -File tools\release.ps1
```
Does the whole thing over WiFi — no SD removal, no per-step IP/PIN typing:
1. ANIMA gate  2. build firmware  3. assemble `deploy/sd` (incremental, hash-based)
4. sync the SD in one manifest-driven pass (`push-ota.mjs --sync`, reads
`deploy/sd/.deploy-manifest.json`, pushes only the delta)  5. OTA the firmware (reboots).

Flags: `-FirmwareOnly` (OTA the .bin only) · `-SdOnly` (sync SD, no OTA) ·
`-SkipBuild` (reuse current build) · `-IncludeMedia` (also push ROMs/DOS/Music/Videos —
normally skipped, flaky over WiFi; copy media via SD instead) · `-DeviceHost` / `-Pin`.
Writes retry+verify, so re-running after a dropped transfer is safe (resumable).

Defaults (host/PIN) are baked in `release.ps1`; override per-run or via
`tools\release.local.json = { "host": "...", "pin": "......" }`. The device PIN is stable
(persisted in `/cfg`, doesn't rotate).

## Build — idf.py direct (flash.ps1 fails in PS sandbox)
`flash.ps1` and `release.ps1` fail in the Claude PowerShell sandbox due to ESP-IDF activation.
Use this proven pattern:

```powershell
# Bump version (once, before build)
powershell -ExecutionPolicy Bypass -File "G:\Nucleo\tools\version-bump.ps1"

# Build
$cmake  = "C:\Users\indecenti\.espressif\tools\cmake\3.30.2\bin"
$ninja  = (Get-ChildItem "C:\Users\indecenti\.espressif\tools\ninja" -Recurse -Filter "ninja.exe" | Select-Object -First 1).DirectoryName
$xtensa = (Get-ChildItem "C:\Users\indecenti\.espressif\tools\xtensa-esp-elf" -Recurse -Filter "xtensa-esp32s3-elf-gcc.exe" | Select-Object -First 1).DirectoryName
$env:IDF_PATH            = "C:\esp\esp-idf"
$env:IDF_PYTHON_ENV_PATH = "C:\Users\indecenti\.espressif\python_env\idf5.4_py3.12_env"
$env:PATH                = "$cmake;$ninja;$xtensa;" + $env:PATH
& "C:\Users\indecenti\.espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe" `
  "C:\esp\esp-idf\tools\idf.py" -C G:\Nucleo\firmware build 2>&1 |
  Where-Object { $_ -match "error:|Binary|free|Linking CXX" } | Select-Object -Last 8
```

## Firmware only
```powershell
# Serial flash (idf.py direct — flash.ps1 fails in PS sandbox)
$env:IDF_PATH = "C:\esp\esp-idf"; # ... set PATH as above ...
& $py $idfpy -C G:\Nucleo\firmware -p COM3 -b 921600 flash 2>&1 | Select-Object -Last 10

# OTA via ota.ps1 (use -DeviceHost, not -Host — parameter name matters):
powershell -ExecutionPolicy Bypass -File tools\ota.ps1 -DeviceHost 192.168.0.166 -Pin <PIN>
powershell -ExecutionPolicy Bypass -File tools\ota.ps1 -DeviceHost 192.168.0.104 -Pin <PIN>
```

## Ship a single web app (no full tree)
`tools\release.ps1 -SdOnly` pushes only the changed files (manifest delta), which is the
clean way. Avoid `deploy.ps1 -To H:` for one app — it stages the whole repo.

## Firmware RAM: boot-test before OTA (the gate does NOT catch this)
The ANIMA gate runs on the PC and proves logic, **not** that the firmware boots on the no-PSRAM
device. A green-gate, clean-build, fits-the-partition firmware can still **reboot-loop** (black
screen) because `httpd` — which starts LAST in boot — can't get a contiguous block. Binding
constraint = `largest_free_block` at the `pre-httpd` bootmark, not free bytes. **For any
RAM-affecting firmware change, flash ONE unit over USB and read the serial `BOOTSTEP` log first:
it must reach `BOOTSTEP httpd` (not `httpd-FAILED`/`abort()`).** Only then OTA the rest.
Discipline: never hold static `.bss` for a feature an app uses — allocate on app `enter()`, free
on `exit()`. See `docs/memory-budget.md` and `docs/releasing.md`.

**Serial recovery (un-brick a looping unit, no rebuild):** the prior good firmware survives in
`ota_1` (USB flash only writes `ota_0`). Point boot back:
`python "$env:IDF_PATH\components\app_update\otatool.py" -p COM3 --baud 115200 switch_ota_partition --slot 1`.
USB-Serial-JTAG gotchas (M5 has no real RTS): if stuck `boot:0x3 (DOWNLOAD)` after a flash, force
a clean boot with `python -m esptool --chip esp32s3 -p COM3 --after watchdog_reset flash_id`;
the panic console is the **same COMx** as flashing (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`) — open
it at 115200 after a watchdog reset to read `BOOTSTEP`/backtrace.

## Gotchas
- **`.gz` shadowing:** the device serves `foo.js.gz` in preference to `foo.js`. After editing
  a web asset you must regenerate its `.gz` (`npm run gz:check` / the gzip step), or the old
  gzipped version keeps being served. JSON assets aren't auto-gzipped.
- **Shell asset change → bump `sw.js`** version, or the service worker serves stale files and
  boot can hang (SW skew). See memory `shell-boot-resilience`.
- **SD sync never deletes** (`sd-sync.ps1` / push-ota: no `/MIR`/`/PURGE`). It protects user
  state (`teacher.json` API key, `learned/*`, `system/config/*`) even if it leaks into the
  payload; the firmware `nucleo_fs_is_protected` blocks on-device deletion too.
- **robocopy exit code 2 = success** (files copied), not an error.
- **OTA upload drops transiently** on a busy / low-contiguous-heap unit (seen on the ADV): the SAME
  image resends fine seconds later — it's NOT a deterministic heap threshold (a retry at the same
  `largest_free_block` succeeds). `ota.ps1` now auto-retries the POST 3× (re-pairing each round); a
  dropped attempt never bricks (image committed only by `esp_ota_end` checksum + set_boot). If all 3
  fail, reboot the device for a fresh heap and re-run. Do NOT add a reboot-before-OTA — wrong fix.
- Serial console AND flashing share the USB-Serial-JTAG, which enumerates as **COM3/COM4**
  (one per unit; VID 303A PID 1001). So the boot/panic console is readable on the same COMx you
  flash on (open it at 115200). Detect the live port — don't assume COM3.

After releasing, report what shipped (firmware? SD? which apps) and the gate result plainly.

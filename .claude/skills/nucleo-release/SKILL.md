---
name: nucleo-release
description: Build, gate, and ship NucleoOS to the Cardputer — firmware via OTA and the web/SD payload via the device file API. Use when the user asks to release, deploy, flash, OTA, sync the SD, or push a web app to the device. Covers the one-command release, firmware-only and SD-only paths, and the gotchas (PIN, .gz shadowing, never /MIR).
---

# Releasing NucleoOS to the device

**Only release when the user explicitly asks.** Building, gating and host-testing are free;
sd-sync / deploy / flash / OTA are not — they push to the device and are hard to reverse.
A PreToolUse hook asks for confirmation before any of them.

**Gate first.** The ANIMA host regression suite (`npm run anima:gate`) must be GREEN before shipping
firmware. `flash.ps1` and `release.ps1` run it themselves; `-SkipGate` overrides — don't, unless the
user says so.

Run every `.ps1` from the **PowerShell tool** as a child process (`powershell -ExecutionPolicy Bypass
-File tools\<x>.ps1`), from the repo root. All paths below are repo-relative — never hard-code a
machine path (the repo lives in different places on different PCs).

## Device host + PIN — `tools/release.local.json` (gitignored), never ask, never print
```
{ "host": "<default unit ip>", "pin": "<pin>",              # what release.ps1 / ota.ps1 read by default
  "devices": [ { "host": "192.168.0.104", "pin": "...", "board": "ADV" },
               { "host": "192.168.0.166", "pin": "...", "board": "original" } ] }
```
The PIN is the device's pairing secret: keep it ONLY in this file — not in the repo, not in memory,
not in chat, not on a command line. Read it inside the PowerShell call:
```powershell
$j = Get-Content tools\release.local.json -Raw | ConvertFrom-Json
$d = $j.devices | Where-Object { $_.host -eq '192.168.0.166' }
powershell -ExecutionPolicy Bypass -File tools\release.ps1 -DeviceHost $d.host -Pin $d.pin
```
The PIN is **stable across reboots** (persisted in NVS, `nucleo_auth.c`). A 401 on `/api/pair` means it
really changed (factory reset, wiped config, M5Launcher cfg loss): ask the user to read it off the
device (Connection → Pair) and update the file.

## One-command release (preferred)
```
powershell -ExecutionPolicy Bypass -File tools\release.ps1 [-DeviceHost <ip> -Pin <pin>]
```
Steps: 0) ANIMA gate · device check (`/api/status`, SD mounted, M5Launcher guest?) · 1) build
(`flash.ps1 -BuildOnly`) · 2) stage `deploy/sd` (`deploy.ps1`, hash-incremental) · 3) sync the SD in one
manifest-driven pass (`push-ota.mjs --sync`) · 4) OTA the firmware · verify `was vX -> now vY (OTA confirmed)`.

Flags: `-FirmwareOnly` · `-SdOnly` (reboots the unit so L1 reloads the index) · `-SkipBuild` (reuse the
current `firmware/build/nucleoos.bin`) · `-IncludeMedia` (also ROMs/DOS/Music/Videos — flaky over WiFi,
prefer copying media via the SD) · `-SkipGate` (don't).

A unit installed by M5Launcher (`/api/status` → `ota.self_update:false`) refuses `/api/ota`: release.ps1
still syncs its SD; its firmware is updated from the Launcher.

**Default to the FULL release (firmware + SD), not `-FirmwareOnly`.** The firmware pins content on the
SD — e.g. it embeds the SHA-256 of `data/anima/learned/facets.*.jsonl` (`VKL_FACETS_*_SHA256`) and reads
`data/anima/anima-person-ambig.bin` — so a new firmware over an old card is inconsistent.

### Preflight a release (do this before touching the device)
1. `git status` clean, and the image is what you think it is: `/api/status.version` after the OTA is
   `<semver>+<build>.g<commit>[*]` — `*` = built from a dirty tree, never ship that.
2. `node tools/image-size-check.mjs firmware/build/nucleoos.bin` — must fit the M5Launcher slot.
3. Preview the SD delta, read-only:
   `node tools/push-ota.mjs --host http://<ip> --pin <pin> --sync --dry-run`
   (from Bash, PIN read from `release.local.json` with a one-line `node -e`, never echoed).
4. After the release, re-run the same dry run: it must say **`0 create, 0 update`**. The live run can
   report MORE updates than the preview (a same-size file whose content read fails under load counts as
   different and is rewritten identically) — harmless; the second dry run is the proof.

### Logs from long scripts
Redirect to a file in the scratchpad and run in the background:
`... *> "<scratchpad>\release.log"`. Windows PowerShell 5 writes it as UTF-8 **with BOM** plus ANSI colour
codes — decode with Python `decode('utf-8-sig')` and strip `\x1b\[[0-9;]*m` before grepping; a `Monitor`
on `tail -f` of that raw file sees nothing.

## Build only
```
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -BuildOnly [-NoBump] [-SkipGate]
```
`flash.ps1` bumps the build counter (unless `-NoBump`), runs the gate (unless `-SkipGate`), activates
ESP-IDF (`C:\esp\esp-idf`, 5.4.x, python env `idf5.4_py3.12_env`), builds with RAM-aware `-j` and
OOM self-heal retries, then runs the M5Launcher slot check. A `NativeCommandError` line during
"Setting up ESP-IDF environment" is PowerShell wrapping stderr — harmless.
Verified working from the Claude PowerShell tool on 2026-10-06.

If activation ever fails with *"Python virtual environment not found"*, set the tools path first:
```powershell
$env:IDF_TOOLS_PATH = "$HOME\.espressif"; . 'C:\esp\esp-idf\export.ps1' *> $null
Set-Location firmware; idf.py build 2>&1 | Select-Object -Last 6
```
- `ninja -C firmware\build <target.obj>` is the fast compile-check for a few changed files.
- `idf.py build` alone does NOT bump the version; run `tools\version-bump.ps1` first for a new counter.

## Firmware only
```powershell
powershell -ExecutionPolicy Bypass -File tools\release.ps1 -FirmwareOnly -SkipBuild   # gate + OTA + verify
powershell -ExecutionPolicy Bypass -File tools\ota.ps1 -DeviceHost <ip> -Pin <pin>     # raw OTA, no gate
```
`ota.ps1` takes `-DeviceHost` (not `-Host`); always pass the IP — mDNS is unreliable.

Serial flash (USB), after activating ESP-IDF as above: `idf.py -C firmware -p COM3 -b 921600 flash`.
**Port ↔ unit:** usually `COM3` = original, `COM4` = ADV (both USB-Serial-JTAG, VID 303A PID 1001) —
check the live port, don't assume.

## Ship a single web app
`tools\release.ps1 -SdOnly` pushes only the changed files (manifest delta) — the clean way. Avoid
`deploy.ps1 -To H:` for one app — it stages the whole repo.

## Versioning — automatic, single source of truth
`tools\version-bump.ps1` increments `firmware/version/BUILD`; `version.cmake` composes
`PROJECT_VER = <semver>+<build>.g<git>[*]` and bakes it into the app descriptor. The same string shows
in `/api/status` (`version`), `/proc/version`, the mDNS `ver` TXT record and the serial boot banner.
- Just building/OTA? Nothing to do — the counter auto-increments.
- Cutting a real release? `tools\version-bump.ps1 -Bump patch|minor|major` first (resets the counter),
  then release; commit the `firmware/version/*` change together with the `package.json`, `CITATION.cff`
  and `CHANGELOG.md` the script updated (CI fails on version drift). `-NoBump` rebuilds the same version.
See `docs/versioning.md`.

## Firmware RAM: boot-test before OTA (the gate does NOT catch this)
The gate proves logic on the PC, **not** that the firmware boots on the no-PSRAM device. A green,
fitting build can still **reboot-loop** because `httpd` — started LAST — can't get a contiguous block.
Binding constraint = `largest_free_block` at the `pre-httpd` bootmark. **For a RAM-affecting change,
flash ONE unit over USB and read the serial `BOOTSTEP` log first: it must reach `BOOTSTEP httpd`.**
Only then OTA the rest. After any OTA, check `/api/status`: `ota.state` = `valid`,
`min_free_heap` / `largest_free_block` sane. See `docs/memory-budget.md`, `docs/releasing.md`.

**Serial recovery (looping unit, no rebuild):** the prior firmware survives in the other OTA slot:
`python "$env:IDF_PATH\components\app_update\otatool.py" -p COM3 --baud 115200 switch_ota_partition --slot 1`.
If stuck in `boot:0x3 (DOWNLOAD)` after a flash: `python -m esptool --chip esp32s3 -p COM3 --after
watchdog_reset flash_id`. The panic console is the same COMx (115200).

## Gotchas
- **`.gz` shadowing:** the device serves `foo.js.gz` before `foo.js` (under `/www/shell` and
  `/apps/<id>/www`). Writing `foo.js` over the API drops a stale twin (`nucleo_fsapi/fstwin.c`) and the
  SD tools remove twins the payload no longer ships. Writers that ship both write `foo.js` FIRST. After
  editing a web asset regenerate its `.gz` (`npm run gz:check`). JSON assets aren't auto-gzipped.
- **Testing the webfs with curl:** send `Accept-Encoding: gzip`. Without it the device must serve the raw
  file (e.g. ANIMA's 539 KB page) and may answer `503 low memory` — a curl artifact, not a bug.
- **Shell asset change → bump `sw.js`**, or the service worker serves stale files (SW skew).
- **Device state is never pushed or overwritten** — one table for every SD tool:
  `tools/lib/sd-policy.json` (`state` globs; under `data/anima/` everything is state EXCEPT the
  `animaShip` globs — AKB5, `anima-*`, `dict-*`, `commands*`, `learned/facets.*.jsonl`, which ship).
  The sync never deletes (no `/MIR`/`/PURGE`).
- **OTA upload drops transiently** on a busy unit: `ota.ps1` retries 3× (re-pairing each round); a
  dropped attempt never bricks (committed only by `esp_ota_end` checksum + set_boot). If all 3 fail,
  reboot the device and re-run. Do NOT add a reboot-before-OTA.
- **robocopy exit code 2 = success** (files copied).

After releasing, report what shipped (firmware? SD? counts), the gate result and the before/after
`/api/status` (version, ota state, heap) plainly.

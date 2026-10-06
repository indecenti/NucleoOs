---
name: nucleo-release-dual
description: Build and ship NucleoOS to BOTH Cardputer units — the original and the ADV — in one pass. Use when the user wants to release/build/OTA/flash for both boards, the ADV and non-ADV Cardputers, or two devices at once. Encodes the key fact that it's ONE universal binary (runtime board auto-detect), so you build once and fan out the shipping to each unit, then verify per board.
---

# Releasing to both Cardputers (original + ADV)

Everything in the `nucleo-release` skill applies (explicit user ask, gate, PIN handling, preflight,
full release by default, log decoding). This skill only adds the two-unit fan-out.

## The one thing that changes everything
There is **ONE universal firmware binary** (`firmware/build/nucleoos.bin`). The board variant is
**auto-detected at runtime**, not a build flag — there is no ADV sdkconfig/define. `nucleo_kbd`
owns the system I2C bus; the ADV peripherals are probed on it and the original board no-ops them:
- ES8311 audio codec @0x18 → `nucleo_codec_present()` (false on original; audio falls back to PDM mic + I2S DAC).
- BMI270 IMU → `nucleo_imu_present()` (false on original).
- TCA8418 keyboard controller (ADV) vs the original matrix keyboard.

**So: build ONCE, ship the SAME artifact to both units. Never rebuild between devices** — that
wastes a build and risks the two units running different bytes. Bump the version once, before the build.

## Units — `tools/release.local.json` (gitignored), never ask, never print the PIN
```
"devices": [ { "host": "192.168.0.104", "pin": "...", "board": "ADV" },
             { "host": "192.168.0.166", "pin": "...", "board": "original" } ]
```
IPs come from DHCP and can move: if a unit doesn't answer `/api/status`, ask the user whether it is
on and where, then update the file. PINs are stable across reboots (NVS); see `nucleo-release`.

## Sequence
```powershell
# 1. Gate + build ONCE (bumps the build counter)
powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -BuildOnly

# 2. Ship the same image to each unit, ONE AT A TIME (each OTA is ~3.1 MB and needs the unit's heap).
#    Original first: if the new image misbehaves, the ADV is still on the old one.
$j = Get-Content tools\release.local.json -Raw | ConvertFrom-Json
foreach ($h in '192.168.0.166', '192.168.0.104') {
    $d = $j.devices | Where-Object { $_.host -eq $h }
    powershell -ExecutionPolicy Bypass -File tools\release.ps1 -DeviceHost $d.host -Pin $d.pin -SkipBuild
    if ($LASTEXITCODE -ne 0) { Write-Error "release to $h failed - stopping before the next unit"; break }
}
```
`release.ps1 -SkipBuild` re-runs the gate (cheap, already green), stages `deploy/sd` (a no-op the second
time), syncs that unit's SD, OTAs the shared image and prints `was vX -> now vY (OTA confirmed)`.
Run it in the background with output redirected to a scratchpad log (see `nucleo-release` → Logs).

Before the first unit, run the preflight from `nucleo-release` (clean tree, slot check, `--dry-run` per
unit). After each unit, its dry run must report `0 create, 0 update`.

## Verify per board
```
GET http://<ip>/api/status
  version       both units MUST show the same string (same build) — no '*' suffix
  ota.state     "valid"   (rollback stays enabled)
  imu.present   true  => ADV       — BMI270 found; ES8311 audio + TCA8418 keyboard active
                false => original  — PDM mic + I2S DAC + matrix keyboard
  min_free_heap / largest_free_block   compare with the value before the release
```
Then fetch a few pages with `Accept-Encoding: gzip` (`/`, `/apps/anima/`, `/apps/agent/`) — all 200.

## Serial path (both units on USB)
Activate ESP-IDF once (see `nucleo-release` → Build only), then flash the SAME build to each port,
**one unit first**: for a RAM-affecting change read its serial `BOOTSTEP` log (115200) — it must reach
`BOOTSTEP httpd` — before flashing the second. Don't brick both at once.
```powershell
idf.py -C firmware -p COM3 -b 921600 flash   # original (usually) — check the live port
idf.py -C firmware -p COM4 -b 921600 flash   # ADV (usually)
```
A missing port ("port is busy or doesn't exist") means that unit isn't connected — use OTA for it.
Serial recovery (switch back to the previous OTA slot) is in `nucleo-release`.

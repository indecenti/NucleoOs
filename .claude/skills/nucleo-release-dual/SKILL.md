---
name: nucleo-release-dual
description: Build and ship NucleoOS to BOTH Cardputer units — the original and the ADV — in one pass. Use when the user wants to release/build/OTA/flash for both boards, the ADV and non-ADV Cardputers, or two devices at once. Encodes the key fact that it's ONE universal binary (runtime board auto-detect), so you build once and fan out the shipping to each unit, then verify per board.
---

# Releasing to both Cardputers (original + ADV)

## The one thing that changes everything
There is **ONE universal firmware binary** (`firmware/build/nucleoos.bin`). The board variant is
**auto-detected at runtime**, not a build flag — there is no ADV sdkconfig/define. `nucleo_kbd`
owns the system I2C bus; the ADV peripherals are probed on it and the original board no-ops them:
- ES8311 audio codec @0x18 → `nucleo_codec_present()` (false on original; audio falls back to PDM mic + I2S DAC).
- BMI270 IMU → `nucleo_imu_present()` (false on original).
- TCA8418 keyboard controller (ADV) vs the original matrix keyboard.

**So: build ONCE, ship the SAME artifact to both units. Never rebuild between devices** — that
would waste a build and risks the two units running different bytes.

**Version parity:** bump version ONCE before building; both units get the same build number.
Build once, flash/OTA to both with `-SkipBuild` (OTA path) or a separate flash job (serial path).

## Device pairs — read from release.local.json, do NOT ask
```
tools/release.local.json  (gitignored)
  "devices": [
    { "host": "192.168.0.104", "pin": "<PIN>" },   # ADV
    { "host": "192.168.0.166", "pin": "<PIN>" }    # original
  ]
```
**Never ask the user for IP/PIN.** If a pair returns 401, the PIN rotated on reboot — read the new
one off the device screen (Connection → Pair), update `release.local.json` AND memory `device-pin.md`.

## Build — use idf.py directly (flash.ps1 fails in PS sandbox)
`flash.ps1` fails in the Claude PowerShell sandbox due to ESP-IDF activation errors. Use this
pattern instead (proven to work):

```powershell
# Step 1: bump version (run once before building)
powershell -ExecutionPolicy Bypass -File "G:\Nucleo\tools\version-bump.ps1"

# Step 2: build
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

Filter build output as above — the raw stream is huge. Check for `Binary size ... X% free` at the end.

**Gate:** for non-ANIMA changes (games, UI, new apps) use `-SkipGate` — no need to run the 12-gate
suite every time. For ANIMA logic changes run `npm run anima:gate` first (must be green).

## Serial flash — parallel to both ports
```powershell
$py     = "C:\Users\indecenti\.espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe"
$idfpy  = "C:\esp\esp-idf\tools\idf.py"
$cmake  = "C:\Users\indecenti\.espressif\tools\cmake\3.30.2\bin"
$ninja  = (Get-ChildItem "C:\Users\indecenti\.espressif\tools\ninja" -Recurse -Filter "ninja.exe" | Select-Object -First 1).DirectoryName
$xtensa = (Get-ChildItem "C:\Users\indecenti\.espressif\tools\xtensa-esp-elf" -Recurse -Filter "xtensa-esp32s3-elf-gcc.exe" | Select-Object -First 1).DirectoryName
$envmap = @{
    IDF_PATH            = "C:\esp\esp-idf"
    IDF_PYTHON_ENV_PATH = "C:\Users\indecenti\.espressif\python_env\idf5.4_py3.12_env"
    PATH                = "$cmake;$ninja;$xtensa;" + $env:PATH
}

$job3 = Start-Job -ScriptBlock {
    param($py,$idfpy,$env)
    foreach ($k in $env.Keys) { [System.Environment]::SetEnvironmentVariable($k,$env[$k]) }; $env:PATH=$env.PATH
    & $py $idfpy -C G:\Nucleo\firmware -p COM3 -b 921600 flash 2>&1
} -ArgumentList $py,$idfpy,$envmap

$job4 = Start-Job -ScriptBlock {
    param($py,$idfpy,$env)
    foreach ($k in $env.Keys) { [System.Environment]::SetEnvironmentVariable($k,$env[$k]) }; $env:PATH=$env.PATH
    & $py $idfpy -C G:\Nucleo\firmware -p COM4 -b 921600 flash 2>&1
} -ArgumentList $py,$idfpy,$envmap

Write-Host "Flash COM3+COM4 in corso..."
Wait-Job $job3,$job4 | Out-Null
Write-Host "=== COM3 ==="; Receive-Job $job3 | Select-Object -Last 8
Write-Host "=== COM4 ==="; Receive-Job $job4 | Select-Object -Last 8
Remove-Job $job3,$job4
```

Both units enumerate as **COM3/COM4** over USB-Serial-JTAG (VID 303A PID 1001). If a port is
missing ("port is busy or doesn't exist"), that unit isn't connected — fall back to OTA for it.

## OTA — preferred after first serial flash
`ota.ps1` uses `-DeviceHost` (not `-Host`) — always pass the IP directly, mDNS is unreliable.
Sequential OTA is safer than parallel — both uploads are ~2.4 MB and the device needs heap to absorb them.

```powershell
# Sequential OTA to both units:
powershell -ExecutionPolicy Bypass -File tools\ota.ps1 -DeviceHost 192.168.0.166 -Pin <PIN>
powershell -ExecutionPolicy Bypass -File tools\ota.ps1 -DeviceHost 192.168.0.104 -Pin <PIN>
```

If a PIN returns 401, it rotated on reboot — read the new one off the device screen (Connection → Pair),
update `tools/release.local.json` AND `memory/device-pin.md`.

## Recommended full release sequence

```powershell
# 1. Gate (skip for non-ANIMA changes)
# npm run anima:gate  ← only for ANIMA logic changes

# 2. Bump + build
powershell -ExecutionPolicy Bypass -File "G:\Nucleo\tools\version-bump.ps1"
# ... idf.py build (see Build section above) ...

# 3a. If both units connected via USB: parallel serial flash (fastest, ~1 min)
# ... Start-Job COM3+COM4 flash (see Serial flash section above) ...

# 3b. If devices on WiFi only: sequential OTA (see OTA section above)
# Invoke-NucleoOTA "192.168.0.166" "<PIN>"
# Invoke-NucleoOTA "192.168.0.104" "<PIN>"

# 4. Verify
Invoke-WebRequest -Uri "http://192.168.0.166/api/status" -UseBasicParsing | Select-Object -ExpandProperty Content
Invoke-WebRequest -Uri "http://192.168.0.104/api/status" -UseBasicParsing | Select-Object -ExpandProperty Content
# Both must show same version string; .104 must have "imu":{"present":true}
```

## Verify per board
```
GET http://<ip>/api/status
  version    => both units MUST show the same string (proof they run the same build)
  imu.present  true  => ADV (.104)  — BMI270 found; ES8311 audio + TCA8418 keyboard active
               false => original (.166) — PDM mic + I2S DAC + matrix keyboard
```

## Boot-test discipline
The host gate does NOT measure device boot RAM. A green build can reboot-loop if `httpd` (starts
last) can't get a contiguous block. For RAM-affecting changes: flash COM3 first, read serial
`BOOTSTEP` log at 115200 — must reach `BOOTSTEP httpd`. Then flash COM4. Don't brick both at once.

## Serial recovery (un-brick, no rebuild)
USB flash writes `ota_0`; the previous firmware survives in `ota_1`:
```powershell
python "$env:IDF_PATH\components\app_update\otatool.py" -p COM3 --baud 115200 switch_ota_partition --slot 1
python -m esptool --chip esp32s3 -p COM3 --after watchdog_reset flash_id
```

## Gotchas
- **`flash.ps1` fails in PS sandbox** — always use the direct `idf.py` pattern above.
- **`ota.ps1` param name** — use `-DeviceHost <ip>` not `-Host`; always pass IP, not mDNS name.
- **`release.local.json` PINs** — keep these in sync with `memory/device-pin.md`. If a PIN changes, update BOTH files.
- **`.gz` shadowing** — after editing a web asset regenerate its `.gz` or the old version is served.
- **`sw.js` bump** — shell asset changes need a `sw.js` version bump or boot hangs (SW skew).
- **SD sync never deletes** — no `/MIR`/`/PURGE`, protects user state.
- **robocopy exit 2 = OK** (files copied).
- **OTA heap fragmentation** — if OTA times out 3×, reboot the device (fresh heap) and retry. A fresh boot after serial flash always has enough contiguous heap for OTA.
- Only release when the user explicitly asks.

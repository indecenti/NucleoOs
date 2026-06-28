# One-command systematic OTA release for NucleoOS.
#
#   powershell -ExecutionPolicy Bypass -File tools\release.ps1
#
# Does the whole thing over the network — no SD removal, no per-step IP/PIN typing:
#   1) build firmware     2) assemble deploy/sd (incremental, hash-based)
#   3) SYNC the SD (one dynamic, manifest-driven pass)     4) OTA the firmware (reboots)
#
# Step 3 is the whole SD reconciliation in ONE call (tools/push-ota.mjs --sync): it reads
# deploy/sd/.deploy-manifest.json and pushes exactly the delta — no hand-maintained tree
# lists. system/config/* is create-only (user state never clobbered); heavy media (ROMs/
# DOS/Music/Videos) is skipped (flaky over WiFi — copy via SD), pass -IncludeMedia to push
# it too. Writes retry+verify, so it's safe to re-run after a dropped transfer (resumable).
#
# IP + PIN are baked here because the device PIN is STABLE (persisted in /cfg, never
# rotates — see nucleo_auth.c). Override per-run with -DeviceHost / -Pin, or put a
# tools\release.local.json = { "host": "...", "pin": "......" } to change the defaults
# without editing this file. To pin a memorable code, set settings.security.pin on device.
param(
    [string]$DeviceHost = "192.168.0.166",
    [string]$Pin        = "689614",
    [switch]$SkipBuild,          # reuse the current build
    [switch]$FirmwareOnly,       # OTA the .bin only, skip the SD sync
    [switch]$SdOnly,             # sync the SD only, skip the firmware OTA
    [switch]$IncludeMedia,       # fill-missing also covers heavy media (ROMs/DOS/Music/Videos)
    [switch]$SkipGate            # bypass the ANIMA regression gate (not recommended)
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$node = (Get-Command node).Source

# Optional local override file (not committed) so IP/PIN live outside the script.
$cfg = Join-Path $here "release.local.json"
if (Test-Path $cfg) {
    $j = Get-Content $cfg -Raw | ConvertFrom-Json
    # Explicit -DeviceHost/-Pin on the command line WIN over the local file (needed for dual-device
    # releases: -DeviceHost <unit> must target that unit, not be clobbered by the file's top-level host).
    if ($j.host -and -not $PSBoundParameters.ContainsKey('DeviceHost')) { $DeviceHost = $j.host }
    if ($j.pin  -and -not $PSBoundParameters.ContainsKey('Pin'))        { $Pin = $j.pin }
}
$base = "http://$DeviceHost"
function Step($s) { Write-Host "`n=== $s ===" -ForegroundColor Cyan }

# 0) ANIMA regression gate (host, 12 gates: corpus/route/agent/math/ood/reliability/halluc/skill-routing/
#    kge/hdc/combinator/unit). A red gate ABORTS the release before we touch the device — zero regressions
#    ship. Runs offline on the host (compiles its own exe). -SkipGate to override (not recommended).
if (-not $SkipGate) {
    Step "0/4 ANIMA regression gate (host)"
    & $node (Join-Path $here "anima-host/gate.mjs")
    if ($LASTEXITCODE -ne 0) { Write-Error "ANIMA gate FAILED -- release aborted. Fix the regression, or pass -SkipGate to override."; exit 1 }
}

# Reachability + pairing sanity first, so we fail fast with a clear message.
Step "Device check: $base"
try { $st = Invoke-RestMethod "$base/api/status" -TimeoutSec 8 }
catch { Write-Error "Cannot reach $base - is the Cardputer on Wi-Fi and the IP right? ($($_.Exception.Message))"; exit 1 }
$beforeVer = $st.version
Write-Host ("online: v{0}, {1} {2}, SD {3}" -f $beforeVer, $st.network.mode, $st.network.ssid,
            ($(if ($st.storage.mounted) { 'mounted' } else { 'NOT mounted' })))
if (-not $st.storage.mounted) { Write-Error "SD not mounted on device - cannot sync files."; exit 1 }

if (-not $SkipBuild) {
    Step "1/4 Build firmware"
    powershell -ExecutionPolicy Bypass -File (Join-Path $here "flash.ps1") -BuildOnly -SkipGate   # gate already ran in step 0
    if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }
}

if (-not $FirmwareOnly) {
    Step "2/4 Assemble SD staging (deploy/sd)"
    powershell -ExecutionPolicy Bypass -File (Join-Path $here "deploy.ps1")

    Step "3/4 Sync SD (dynamic, manifest-driven; create/update, never clobbers user state)"
    # ONE pass for the whole card — web + ANIMA models + config defaults + data, straight from
    # the staging manifest. Idempotent and resumable: re-running only retouches what's wrong.
    $sync = @((Join-Path $here "push-ota.mjs"), "--host", $base, "--pin", $Pin, "--sync")
    if ($IncludeMedia) { $sync += "--include-media" }
    & $node @sync
    if ($LASTEXITCODE -ne 0) { Write-Error "SD sync failed (re-run release.ps1 to resume)"; exit 1 }
}

if (-not $SdOnly) {
    Step "4/4 OTA firmware (device reboots into the new image)"
    powershell -ExecutionPolicy Bypass -File (Join-Path $here "ota.ps1") -DeviceHost $DeviceHost -Pin $Pin
    if ($LASTEXITCODE -ne 0) { Write-Error "firmware OTA failed"; exit 1 }
} else {
    # The on-device L1 (ANIMA retrieval) reads the AKB2 index header + offsets into RAM ONCE at
    # boot and keeps the file open. Replacing the index on the SD without a reboot leaves those
    # offsets stale -> every knowledge query silently falls below the gate. So an SD-only release
    # that touched data/anima must reboot too. Reusing the firmware OTA is the only reboot path.
    Step "4/4 Reboot device so L1 reloads the new index"
    powershell -ExecutionPolicy Bypass -File (Join-Path $here "ota.ps1") -DeviceHost $DeviceHost -Pin $Pin
    if ($LASTEXITCODE -ne 0) { Write-Error "reboot failed"; exit 1 }
}

# Verify the device actually rebooted into the version we just shipped. /api/status.version is the
# real app-descriptor version (semver+build.git), so a changed string here is proof the OTA took —
# the whole point of the versioning system. Poll through the ~5s reboot window.
Step "Verify running version"
$afterVer = $null
for ($i = 0; $i -lt 20 -and -not $afterVer; $i++) {
    Start-Sleep -Seconds 2
    try { $afterVer = (Invoke-RestMethod "$base/api/status" -TimeoutSec 5).version } catch { }
}
if (-not $afterVer) {
    Write-Warning "Could not read /api/status after reboot — check the device manually."
} elseif ($beforeVer -eq $afterVer) {
    Write-Host ("running v{0} (unchanged — expected if firmware was not rebuilt/OTA'd)." -f $afterVer) -ForegroundColor Yellow
} else {
    Write-Host ("version: was v{0}  ->  now v{1}  (OTA confirmed)." -f $beforeVer, $afterVer) -ForegroundColor Green
}

Write-Host "`nRelease complete -> $DeviceHost  (firmware + full SD synced)." -ForegroundColor Green
Write-Host "The device reboots into the new image (~5s) and reads the fresh SD." -ForegroundColor Green

# Build the NucleoOS firmware with ESP-IDF and flash it to the Cardputer.
# Usage: powershell -ExecutionPolicy Bypass -File tools\flash.ps1 [-Port COM3] [-BuildOnly] [-Jobs N]
#
# Robust against the one thing that makes this build non-deterministic: at -Os the template-heavy
# m5gfx C++ files spike cc1plus RAM, and too many in parallel get OOM-killed with NO error message
# (silent ninja "FAILED"). idf.py 5.4 has no -j option, so we drive ninja directly and (1) cap jobs
# by free RAM up front, (2) self-heal with escalating backoff to -j 1 — ccache makes retries cheap,
# and the OOM-killed objects recompile alone (no contention) so they succeed.
param([string]$Port = "COM3", [switch]$BuildOnly, [int]$Jobs = 0, [switch]$SkipGate)
$ErrorActionPreference = 'Stop'
$idf = "C:\esp\esp-idf"
$fw = Join-Path (Split-Path $PSScriptRoot -Parent) "firmware"

# ANIMA regression gate (host, 12 gates incl. 0-hallucination + skill-routing) must be GREEN before we
# build firmware that ships this logic. Compiles its own host exe; independent of ESP-IDF. -SkipGate to skip.
if (-not $SkipGate) {
    Write-Host "`n=== ANIMA regression gate (host) ===" -ForegroundColor Cyan
    & (Get-Command node).Source (Join-Path $PSScriptRoot "anima-host/gate.mjs")
    if ($LASTEXITCODE -ne 0) { Write-Error "ANIMA gate FAILED -- aborting build. Fix the regression, or pass -SkipGate to override."; exit 1 }
}

Write-Host "Setting up ESP-IDF environment..."
. "$idf\export.ps1"

Set-Location $fw
# set-target triggers a full clean, so only do it once (first build). It also runs the component
# manager and writes build.ninja, which everything below relies on.
if (-not (Test-Path "$fw\build\CMakeCache.txt")) { idf.py set-target esp32s3 }

# Memory-safe job count: budget ~1.5 GB peak per heavy -Os C++ job, floor 2, cap at core count.
if ($Jobs -le 0) {
    $cores = [Environment]::ProcessorCount
    $freeGB = (Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB   # KB -> GB
    $byRam = [math]::Max(2, [math]::Floor($freeGB / 1.5))
    $Jobs = [math]::Min($cores, $byRam)
}
Write-Host ("CPU cores={0}, free RAM={1:N1} GB -> starting with -j {2}" -f `
    [Environment]::ProcessorCount, ((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB), $Jobs)

# First pass through idf.py so dependency resolution / reconfigure / .bin generation all run the
# blessed way. ninja auto-reconfigures on stale build.ninja, so the retries below stay equivalent.
# Escalating backoff: each retry halves parallelism (lower contention = lower peak RAM), ending at
# -j 1 which is contention-free and effectively guaranteed to finish a transient OOM failure.
$built = $false
$j = $Jobs
for ($attempt = 1; $attempt -le 4; $attempt++) {
    if ($attempt -eq 1) {
        Write-Host "Build attempt 1 (idf.py build)..."
        $env:IDF_BUILD_JOBS = "$j"   # honoured by idf.py->ninja in newer IDF; harmless otherwise
        idf.py build
    } else {
        Write-Host "Build attempt $attempt (ninja -j $j, self-heal)..."
        ninja -C "$fw\build" -j $j
    }
    if ($LASTEXITCODE -eq 0) { $built = $true; break }
    Write-Host "Attempt $attempt failed (exit $LASTEXITCODE)."
    $j = [math]::Max(1, [math]::Floor($j / 2))
}
if (-not $built) {
    Write-Error "Build failed after 4 attempts (last -j $j). Not a contention issue -- check the log for a real compile/link error."
    exit 1
}

# Sanity: the image must actually exist before we try to flash it.
if (-not (Test-Path "$fw\build\nucleoos.bin")) { Write-Error "Build reported success but nucleoos.bin is missing"; exit 1 }
$binKB = [math]::Round((Get-Item "$fw\build\nucleoos.bin").Length / 1KB)
Write-Host "Build OK -- nucleoos.bin = $binKB KB"

if (-not $BuildOnly) {
    Write-Host "Flashing to $Port..."
    idf.py -p $Port flash
    if ($LASTEXITCODE -ne 0) { Write-Error "Flash failed"; exit 1 }
}
Write-Host "Done."

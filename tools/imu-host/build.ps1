# Build + run the IMU motion-classifier host verification (mirrors tools/voice-host/build.ps1).
# MinGW GCC, gnu11, -static so the exe runs without MinGW DLLs.
$ErrorActionPreference = 'Stop'
$mingw = 'C:\msys64\mingw64\bin'
if (-not ($env:Path -like "*$mingw*")) { $env:Path = "$mingw;$env:Path" }

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe  = Join-Path $out 'imu_test.exe'

$inc  = Join-Path $here '..\..\firmware\components\nucleo_imu\include'
$src  = @(
    (Join-Path $here 'test_imu.c'),
    (Join-Path $here '..\..\firmware\components\nucleo_imu\nucleo_imu_motion.c')
)

& "$mingw\gcc.exe" -std=gnu11 -O2 -Wall -I $inc -static -o $exe $src -lm
if ($LASTEXITCODE -ne 0) { Write-Host 'BUILD FAILED'; exit 1 }

& $exe
exit $LASTEXITCODE

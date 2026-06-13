# Build + run the voice DSP host verification (mirrors tools/anima-host/build.ps1).
# MinGW GCC, gnu11 (exposes M_PI), -static so the exe runs without MinGW DLLs.
$ErrorActionPreference = 'Stop'
$mingw = 'C:\msys64\mingw64\bin'
if (-not ($env:Path -like "*$mingw*")) { $env:Path = "$mingw;$env:Path" }

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here 'build'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe  = Join-Path $out 'voice_test.exe'

$src = @(
    (Join-Path $here 'test_dsp.c'),
    (Join-Path $here '..\..\firmware\components\nucleo_voice\nucleo_voice_dsp.c')
)

& "$mingw\gcc.exe" -std=gnu11 -O2 -Wall -static -o $exe $src -lm
if ($LASTEXITCODE -ne 0) { Write-Host 'BUILD FAILED'; exit 1 }

& $exe
exit $LASTEXITCODE

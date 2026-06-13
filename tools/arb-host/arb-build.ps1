#requires -version 5
# Builds the heavy-work arbiter HOST test: compiles the REAL device core (nucleo_arb.c) against a
# Win32 platform shim (arb_plat_host.c) + the concurrency test (arb_test.c) with MinGW-w64 GCC and
# -DARB_HOST. No ESP-IDF. Output: build\arb_test.exe (static -> runs standalone). Mirrors the
# anima-host build so a contributor has one toolchain story.
$ErrorActionPreference = 'Stop'

$here  = $PSScriptRoot
$repo  = (Resolve-Path (Join-Path $here '..\..')).Path
$comp  = Join-Path $repo 'firmware\components\nucleo_arb'
$build = Join-Path $here 'build'

New-Item -ItemType Directory -Force -Path $build | Out-Null

# Locate gcc: PATH first, then the usual MSYS2 roots (identical to tools\anima-host\build.ps1).
$gcc = $null
$onPath = Get-Command gcc -ErrorAction SilentlyContinue
if ($onPath) { $gcc = $onPath.Source }
else { foreach ($c in @('C:\msys64\mingw64\bin\gcc.exe','C:\msys64\ucrt64\bin\gcc.exe','C:\msys64\clang64\bin\gcc.exe')) { if (Test-Path $c) { $gcc = $c; break } } }
if (-not $gcc) { throw "gcc non trovato. Installa MinGW-w64 (es. MSYS2) o aggiungilo al PATH." }
$env:Path = (Split-Path $gcc) + ';' + $env:Path

$q = { param($p) '"' + $p + '"' }
$srcs = @((Join-Path $comp 'nucleo_arb.c'),
          (Join-Path $here 'arb_plat_host.c'),
          (Join-Path $here 'arb_test.c'))
$exe = Join-Path $build 'arb_test.exe'

$parts = @('-std=gnu11','-O2','-g','-static','-DARB_HOST','-Wall','-Wextra',
           "-I$(& $q (Join-Path $comp 'include'))",
           "-I$(& $q $comp)")                       # arb_plat.h (private header) lives here
$parts += ($srcs | ForEach-Object { & $q $_ })
$parts += @('-o', (& $q $exe))
$log = Join-Path $build 'compile.log'

Write-Host "Compilo arbiter host test con $gcc ..." -ForegroundColor Cyan
cmd /c ("`"$gcc`" " + ($parts -join ' ') + " > `"$log`" 2>&1")
$code = $LASTEXITCODE
if ($code -eq 0 -and (Test-Path $exe)) {
    Write-Host "OK -> $exe" -ForegroundColor Green
} else {
    if (Test-Path $log) { Get-Content $log }
    throw "Compilazione fallita (exit $code). Vedi $log"
}

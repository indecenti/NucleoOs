#requires -version 5
# Builds the eventbus HOST test: compiles the REAL firmware source (nucleo_eventbus.c) against tiny
# FreeRTOS/esp shims + the boundary test (eventbus_test.c) with MinGW-w64 GCC. No ESP-IDF. Output:
# build\eventbus_test.exe. Mirrors tools\arb-host\arb-build.ps1 so there is one toolchain story.
$ErrorActionPreference = 'Stop'

$here  = $PSScriptRoot
$repo  = (Resolve-Path (Join-Path $here '..\..')).Path
$comp  = Join-Path $repo 'firmware\components\nucleo_eventbus'
$ashim = Join-Path $repo 'tools\anima-host\shim'       # reuse esp_log.h / esp_err.h / nucleo_board.h
$build = Join-Path $here 'build'

New-Item -ItemType Directory -Force -Path $build | Out-Null

# Locate gcc: PATH first, then the usual MSYS2 roots (identical to tools\arb-host\arb-build.ps1).
$gcc = $null
$onPath = Get-Command gcc -ErrorAction SilentlyContinue
if ($onPath) { $gcc = $onPath.Source }
else { foreach ($c in @('C:\msys64\mingw64\bin\gcc.exe','C:\msys64\ucrt64\bin\gcc.exe','C:\msys64\clang64\bin\gcc.exe')) { if (Test-Path $c) { $gcc = $c; break } } }
if (-not $gcc) { throw "gcc non trovato. Installa MinGW-w64 (es. MSYS2) o aggiungilo al PATH." }
$env:Path = (Split-Path $gcc) + ';' + $env:Path

$q = { param($p) '"' + $p + '"' }
$srcs = @((Join-Path $comp 'nucleo_eventbus.c'),
          (Join-Path $here 'eventbus_test.c'))
$exe = Join-Path $build 'eventbus_test.exe'

$parts = @('-std=gnu11','-O2','-g','-static','-Wall','-Wextra',
           "-I$(& $q (Join-Path $here 'shim'))",          # freertos/FreeRTOS.h, freertos/semphr.h
           "-I$(& $q $ashim)",                            # esp_log.h, esp_err.h, nucleo_board.h
           "-I$(& $q (Join-Path $comp 'include'))")       # nucleo_eventbus.h
$parts += ($srcs | ForEach-Object { & $q $_ })
$parts += @('-o', (& $q $exe))
$log = Join-Path $build 'compile.log'

Write-Host "Compilo eventbus host test con $gcc ..." -ForegroundColor Cyan
cmd /c ("`"$gcc`" " + ($parts -join ' ') + " > `"$log`" 2>&1")
$code = $LASTEXITCODE
if ($code -eq 0 -and (Test-Path $exe)) {
    Write-Host "OK -> $exe" -ForegroundColor Green
} else {
    if (Test-Path $log) { Get-Content $log }
    throw "Compilazione fallita (exit $code). Vedi $log"
}

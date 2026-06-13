#requires -version 5
# Builds the ANIMA host harness: compiles the REAL firmware cascade
# (nucleo_anima.c + anima_solve.c + anima_text.c + _l1.c + _hdc.c) against host shims, with MinGW-w64 GCC.
# No ESP-IDF, no hardware. Output: build\anima.exe (static -> runs standalone).
$ErrorActionPreference = 'Stop'

$here  = $PSScriptRoot
$repo  = (Resolve-Path (Join-Path $here '..\..')).Path
$anima = Join-Path $repo 'firmware\components\nucleo_anima'
$build = Join-Path $here 'build'

New-Item -ItemType Directory -Force -Path $build | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $here 'sd\data\anima') | Out-Null

# Locate gcc: PATH first, then the usual MSYS2 roots.
$gcc = $null
$onPath = Get-Command gcc -ErrorAction SilentlyContinue
if ($onPath) { $gcc = $onPath.Source }
else { foreach ($c in @('C:\msys64\mingw64\bin\gcc.exe','C:\msys64\ucrt64\bin\gcc.exe','C:\msys64\clang64\bin\gcc.exe')) { if (Test-Path $c) { $gcc = $c; break } } }
if (-not $gcc) { throw "gcc non trovato. Installa MinGW-w64 (es. MSYS2) o aggiungilo al PATH." }

# gcc needs its own bin dir on PATH to spawn cc1 + load its DLLs.
$env:Path = (Split-Path $gcc) + ';' + $env:Path

# GLOB every firmware .c (don't hand-list) so a NEW nucleo_anima file is compiled automatically and can
# never be silently left out — matching the staleness check in anima.mjs (which globs the same dir). Two
# files are excluded ON PURPOSE: nucleo_anima_online.c (the network tier — replaced on the host by
# anima_online_stub.c, so it must NOT be linked too) and nucleo_anima_bench.c (a standalone benchmark).
$exclude = @('nucleo_anima_online.c','nucleo_anima_bench.c')
$srcs = Get-ChildItem -Path $anima -Filter '*.c' |
        Where-Object { $exclude -notcontains $_.Name } |
        ForEach-Object { $_.FullName }
$srcs += @('esp_timer_host.c','anima_online_stub.c','host_main.c') |
        ForEach-Object { Join-Path $here $_ }
$exe = Join-Path $build 'anima.exe'

# -std=gnu11 (not c11): matches ESP-IDF, and exposes MinGW's strcasecmp / M_PI that the
# firmware relies on (hidden behind __STRICT_ANSI__ under -std=c11 -> hard error on GCC 15).
# -static: a self-contained exe that runs without mingw DLLs on PATH.
$q = { param($p) '"' + $p + '"' }
$parts = @('-std=gnu11','-O0','-g','-static','-DANIMA_HOST',
           "-I$(& $q (Join-Path $here 'shim'))",
           "-I$(& $q (Join-Path $anima 'include'))",
           "-I$(& $q $anima)",
           '-include', (& $q (Join-Path $here 'shim\host_compat.h')))
$parts += ($srcs | ForEach-Object { & $q $_ })
$parts += @('-o', (& $q $exe), '-lm', '-lshell32')   # shell32: CommandLineToArgvW (UTF-8 argv)
$log = Join-Path $build 'compile.log'

Write-Host "Compilo ANIMA host con $gcc ..." -ForegroundColor Cyan
cmd /c ("`"$gcc`" " + ($parts -join ' ') + " > `"$log`" 2>&1")
$code = $LASTEXITCODE
if ($code -eq 0 -and (Test-Path $exe)) {
    Write-Host "OK -> $exe" -ForegroundColor Green
    Write-Host 'Prova:  .\build\anima.exe "che ore sono"' -ForegroundColor DarkGray
} else {
    if (Test-Path $log) { Get-Content $log }
    throw "Compilazione fallita (exit $code). Vedi $log"
}

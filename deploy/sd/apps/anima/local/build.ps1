#requires -version 5
# Builds ANIMA Local: the decoupled, exact-copy offline cascade compiled to WebAssembly
# with Emscripten. Output: apps/anima/www/local/anima-local.{js,wasm} — served to the browser
# AND require()-able in Node for the parity test against the host harness (anima.exe).
#
# src/ is a SNAPSHOT of firmware/components/nucleo_anima (the network tier excluded, replaced by
# stub/anima_online_stub.c) meant to EVOLVE independently of the Cardputer. We DO pass -DANIMA_HOST,
# exactly like the host harness (tools/anima-host): with no env vars set it uses the SAME default
# gates as the device, but (a) selects the FLAT index — full-corpus recall, RAM-resident, better
# than the MCU's shard-routed AKB5 in a browser with memory to spare — and (b) compiles out the
# mbedTLS facet-integrity check (a LAN-served pack from the user's own device needs no SHA root of
# trust). This makes the browser cascade byte-identical to anima.exe, so the parity test certifies it.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$out  = Join-Path $repo 'apps\anima\www\local'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$srcs = @()
$srcs += (Get-ChildItem (Join-Path $here 'src\*.c')).FullName
$srcs += (Get-ChildItem (Join-Path $here 'stub\*.c')).FullName
$srcs += (Join-Path $here 'wasm_main.c')

$q = { param($p) '"' + $p + '"' }
$args = @(
    '-std=gnu11', '-O2', '-D_GNU_SOURCE', '-DANIMA_HOST',
    '-include', (& $q (Join-Path $here 'shim\wasm_prelude.h')),
    "-I$(& $q (Join-Path $here 'shim'))",
    "-I$(& $q (Join-Path $here 'src\include'))",
    "-I$(& $q (Join-Path $here 'src'))"
)
$args += ($srcs | ForEach-Object { & $q $_ })
$args += @(
    '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sEXPORT_NAME=AnimaLocal',
    '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=33554432', '-sSTACK_SIZE=8388608',
    '-sEXIT_RUNTIME=0', '-sENVIRONMENT=web,worker,node', '-sFORCE_FILESYSTEM=1', '-lidbfs.js',
    '-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,IDBFS,UTF8ToString,stringToUTF8,lengthBytesUTF8',
    '-sEXPORTED_FUNCTIONS=_anima_init,_anima_query_json,_anima_reset,_malloc,_free',
    '-o', (& $q (Join-Path $out 'anima-local.mjs'))
)
# Emit as an ES module (.mjs): a single artifact that loads via dynamic import() in BOTH the
# browser (the web app) and Node (the parity test), with the .wasm located via import.meta.url.

$bat = Join-Path $env:TEMP 'anima_local_emcc.bat'
$line = 'emcc ' + ($args -join ' ')
"@echo off`r`ncall C:\emsdk\emsdk_env.bat >nul 2>&1`r`n$line`r`nexit /b %ERRORLEVEL%" |
    Out-File -FilePath $bat -Encoding ascii
Write-Host "emcc -> $(Join-Path $out 'anima-local.js')" -ForegroundColor Cyan
cmd /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw "emcc failed (exit $LASTEXITCODE)" }
Write-Host "OK -> anima-local.js (+ .wasm)" -ForegroundColor Green
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize

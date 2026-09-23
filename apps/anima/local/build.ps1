#requires -version 5
# Builds ANIMA Local: the offline cascade compiled to WebAssembly with Emscripten, DIRECTLY from the
# firmware sources in firmware/components/nucleo_anima — the way tools/anima-host/build.ps1 builds
# anima.exe: the same .c set (every file but the network tier and the benchmark), the same online stub
# (tools/anima-host/anima_online_stub.c) and the same ESP-IDF shims (tools/anima-host/shim). The wasm side
# adds only glue: shim/nucleo_board.h (/sd mount), shim/wasm_prelude.h (musl <strings.h>), wasm_main.c
# (the JS API) and wasm_fs.c (user-taught state -> the IDBFS-persisted rw/ subtree, via -Wl,--wrap).
# There is no copy of the engine to drift: device, anima.exe and the browser run one C source.
#
# RULE: after ANY change to firmware/components/nucleo_anima (or the shared host shims/stub), rerun
#   powershell -NoProfile -ExecutionPolicy Bypass -File apps/anima/local/build.ps1
# The build stamps a fingerprint of its inputs into the module (engine-src.mjs); parity.mjs — an entry
# of `npm run anima:gate` — fails while the stamped id differs from the sources on disk.
#
# Output: apps/anima/www/local/anima-local.{mjs,wasm} + their .gz twins (the device serves .gz first,
# so a stale twin would ship the old brain). The .mjs loads via import() in the browser AND in Node.
#
# -DANIMA_HOST, exactly like the host harness: with no env vars set it uses the SAME default gates as the
# device, but (a) selects the FLAT index unless wasm_main.c opts into AKB5, and (b) compiles out the
# mbedTLS facet-integrity check (a LAN-served pack from the user's own device needs no SHA root of trust).
# wasm_main.c then sets the browser's PC-grade knobs (L1_PFM, ANIMA_AKB5, ANIMA_AKB5_PROBE); parity.mjs
# runs anima.exe with the same knobs, so it certifies the browser engine answer-for-answer.
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$out  = Join-Path $repo 'apps\anima\www\local'
$fw   = Join-Path $repo 'firmware\components\nucleo_anima'
$host_ = Join-Path $repo 'tools\anima-host'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$node = (Get-Command node -ErrorAction SilentlyContinue)
if (-not $node) { throw 'node not found on PATH (needed for engine-src.mjs)' }
$srcTool = Join-Path $here 'engine-src.mjs'

# The translation units come from engine-src.mjs: ONE list shared with the fingerprint, so the set
# that is compiled and the set that is hashed can never disagree.
$srcs = @(& node $srcTool --sources | Where-Object { $_ -and $_.Trim() })
if ($LASTEXITCODE -ne 0 -or $srcs.Count -eq 0) { throw 'engine-src.mjs --sources failed' }
$idHeader = Join-Path $env:TEMP 'anima_local_build_id.h'
& node $srcTool --header $idHeader
if ($LASTEXITCODE -ne 0) { throw 'engine-src.mjs --header failed' }

$q = { param($p) '"' + $p + '"' }
$emccArgs = @(
    '-std=gnu11', '-O2', '-D_GNU_SOURCE', '-DANIMA_HOST',
    '-include', (& $q (Join-Path $here 'shim\wasm_prelude.h')),
    '-include', (& $q $idHeader),
    "-I$(& $q (Join-Path $here 'shim'))",           # wasm overrides first (nucleo_board.h)
    "-I$(& $q (Join-Path $host_ 'shim'))",          # then every shared ESP-IDF shim
    "-I$(& $q (Join-Path $fw 'include'))",
    "-I$(& $q $fw)"
)
$emccArgs += ($srcs | ForEach-Object { & $q $_ })
$emccArgs += @(
    '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sEXPORT_NAME=AnimaLocal',
    '-sALLOW_MEMORY_GROWTH=1', '-sINITIAL_MEMORY=33554432', '-sSTACK_SIZE=8388608',
    '-sEXIT_RUNTIME=0', '-sENVIRONMENT=web,worker,node', '-sFORCE_FILESYSTEM=1', '-lidbfs.js',
    '-Wl,--wrap=fopen', '-Wl,--wrap=remove', '-Wl,--wrap=rename',   # -> wasm_fs.c (per-user state in rw/)
    '-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,IDBFS,UTF8ToString,stringToUTF8,lengthBytesUTF8',
    '-sEXPORTED_FUNCTIONS=_anima_init,_anima_query_json,_anima_reset,_anima_set_env,_anima_knobs,_anima_build_id,_malloc,_free',
    '-o', (& $q (Join-Path $out 'anima-local.mjs'))
)

$bat = Join-Path $env:TEMP 'anima_local_emcc.bat'
$line = 'emcc ' + ($emccArgs -join ' ')
"@echo off`r`ncall C:\emsdk\emsdk_env.bat >nul 2>&1`r`n$line`r`nexit /b %ERRORLEVEL%" |
    Out-File -FilePath $bat -Encoding ascii
Write-Host "emcc ($($srcs.Count) translation units: firmware/components/nucleo_anima + host stubs + wasm glue) -> $(Join-Path $out 'anima-local.mjs')" -ForegroundColor Cyan
cmd /c "`"$bat`""
if ($LASTEXITCODE -ne 0) { throw "emcc failed (exit $LASTEXITCODE)" }

# Regenerate the .gz twins of exactly the two artifacts (gzip level 9, like tools/gzip-assets.mjs;
# check-gz compares content). Scoped on purpose: other files in www/local have their own owners.
$gz = "const z=require('zlib'),f=require('fs');for(const p of process.argv.slice(1)){f.writeFileSync(p+'.gz',z.gzipSync(f.readFileSync(p),{level:9}))}"
& node -e $gz (Join-Path $out 'anima-local.mjs') (Join-Path $out 'anima-local.wasm')
if ($LASTEXITCODE -ne 0) { throw 'gzip of the artifacts failed' }

Write-Host "OK -> anima-local.mjs + .wasm (+ .gz), build id $(& node $srcTool)" -ForegroundColor Green
Get-ChildItem $out -Filter 'anima-local.*' | Select-Object Name, Length | Format-Table -AutoSize

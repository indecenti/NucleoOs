# ANIMA host harness

Compiles and runs the **real** ANIMA cascade (`firmware/components/nucleo_anima/`) on a PC —
**no Cardputer, no flash, no Wi‑Fi**. This is the default loop for iterating on ANIMA's
offline NLU (L0 intents + L1 retrieval + HDC) in a few seconds.

It builds the firmware's own C sources (not a `.mjs` twin), so there is no logic drift.
The only fakes are four ESP‑IDF headers (`shim/`) and the network tier
(`anima_online_stub.c`), which here always answers "offline / honest miss" — i.e. a device
with Wi‑Fi switched off.

> **Full guide, toolchain notes, and the wider debug menu (JTAG / QEMU / Wokwi):**
> [docs/debugging.md](../../docs/debugging.md).

## Requirements
- **MinGW‑w64 GCC** (e.g. via MSYS2). `build.ps1` finds it on `PATH` or under `C:\msys64`.
  (There is no `cl.exe` on this machine; see docs/debugging.md §2.)

## Build & run
```powershell
.\build.ps1                                   # -> build\anima.exe (static)
.\build\anima.exe "che ore sono"              # one‑shot
.\build\anima.exe --en "what time is it"      # English
.\build\anima.exe                             # REPL: one query per line; /en /it /reset
Get-Content queries.txt | .\build\anima.exe   # batch
```
From the repo root: `npm run anima -- "che ore sono"` (auto‑builds if sources changed),
`npm run anima` (REPL), `npm run anima:build` (rebuild only).

## L1 (semantic retrieval) — optional
L0/HDC run immediately. L1 self‑disables (logs `L1 disabled`) until the model is present:
```powershell
Copy-Item ..\..\models\anima-it-encoder.bin, ..\..\models\anima-it-index.bin sd\data\anima\
```
Then you'll see `L1 ready: encoder 16384x192, AKB2 7304 vectors / 93 clusters`. Session and
telemetry are written under `sd\data\anima\`.

## Layout
```
build.ps1            build with MinGW gcc (-std=gnu11 -static)  ->  build\anima.exe
anima.mjs            npm entry: build-if-stale, then run
host_main.c          driver (one-shot / REPL / batch, IT+EN)
anima_online_stub.c  network tier stubbed to offline / honest miss
esp_timer_host.c     esp_timer_get_time() via QueryPerformanceCounter
shim/                host versions of esp_log/esp_timer/esp_partition/esp_err + nucleo_board
queries.txt          sample sweep for regression diffing
```

## Not covered
FreeRTOS, `esp_http_server`, Wi‑Fi, real SD/FATFS, audio, USB, the M5GFX UI, and the
`{value}` substitution (done by the app‑layer executor, not ANIMA core). For those, use the
other rows in [docs/debugging.md](../../docs/debugging.md) §3.

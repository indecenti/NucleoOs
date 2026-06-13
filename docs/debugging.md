# Debugging NucleoOS — the host-first dev loop

Iterating on firmware by **flash → unplug → join Wi‑Fi → poll `/api/logs`** is slow and
breaks concentration. This document defines the **default** workflow: run as much logic as
possible **on the PC**, in a sub‑10‑second build‑and‑run loop, and flash the real device
only to *confirm* — never to *iterate*.

> **Default for assistant‑logic work (ANIMA): the host harness.** Reach for the device,
> JTAG, or an emulator only when the change touches something the harness cannot see
> (the network stack, the SD/audio/USB peripherals, or the M5GFX UI). See §4.

---

## 1. The default: the ANIMA host harness

`tools/anima-host/` compiles the **real** firmware cascade — the very same C the device
runs (`nucleo_anima.c` + `nucleo_anima_l1.c` + `nucleo_anima_hdc.c`) — into a PC executable.
It is **not** a JavaScript re‑implementation, so there is no `.mjs`‑vs‑C drift: what passes
here is the firmware's own L0/L1/HDC code.

```powershell
cd tools\anima-host
.\build.ps1                                   # ~5 s  ->  build\anima.exe (static, standalone)
.\build\anima.exe "che cos'è nucleoos"        # one‑shot
.\build\anima.exe --en "open the calculator"  # English
.\build\anima.exe                             # REPL: one query per line; /en /it /reset
Get-Content queries.txt | .\build\anima.exe   # batch (regression sweep)
```

Or from the repo root, via npm (auto‑builds if sources changed):

```
npm run anima -- "che ore sono"
npm run anima            # interactive REPL
npm run anima:build      # force a rebuild
```

Each query prints the full runtime decision — exactly the state you previously had to fish
out over Wi‑Fi:

```
Q: che cos'è nucleoos
   tier=L1/fact  action=answer  intent=l1  conf=97
   state=idle  budget=1  from_mem=0
   reply: NucleoOS gira sul M5Stack Cardputer (ESP32-S3) e funziona offline.
```

`tier` (L0 command / L1 fact / none) · `action` · `intent` · `conf` (0–100) ·
`budget` (L1 clusters probed) · `state` (FSM turn) · `corrected` (typo‑correction) · `reply`.

### What it exercises
- **L0** — intent matching, app aliases, the agentic slot/clarify FSM, typo correction.
- **L1** — the real semantic retrieval (hashed n‑gram encoder + cosine over the AKB2 index),
  active once the model files are present (see §5).
- **HDC** — the hyperdimensional reasoning core. Its on-boot `nucleo_anima_hdc_selftest` is OPT-IN
  (`CONFIG_NUCLEO_ANIMA_HDC_SELFTEST`, default off — the linker GCs it out of production builds);
  the reasoning gate runs on host via `npm run anima:gate` (hdc-eval), not at device boot.
- **Honest misses** — out‑of‑corpus input returns `tier=none` (the device escalates to the
  online teacher; on host the online tier is stubbed, so you see the miss directly).

### How it works (no firmware source is modified)
- `shim/` provides tiny host versions of the four ESP‑IDF headers ANIMA touches:
  `esp_log.h` → `stderr`, `esp_timer.h` → `QueryPerformanceCounter`, `esp_partition.h` →
  "not found" (so L1 falls back to the file path, exactly as on a device whose encoder lives
  only on the SD), `esp_err.h`. `nucleo_board.h` is shimmed to point `NUCLEO_SD_MOUNT` at a
  local `./sd` tree.
- `anima_online_stub.c` implements the online tier's interface as a permanent
  "offline / honest miss" — i.e. a device with Wi‑Fi switched off. Swap it for a real fetch
  if you ever want to test the network tier on host.
- `host_main.c` is the driver (one‑shot / REPL / batch, IT+EN).

---

## 2. Toolchain (this machine)

The harness builds with **MinGW‑w64 GCC 15.2** at `C:\msys64\mingw64\bin\gcc.exe`.

> There is **no `cl.exe`** here: the Visual Studio Build Tools folder ships only the VC
> redistributables, not the MSVC compiler. The harness shims are written to work under both
> GCC and MSVC, but GCC is what's installed and what `build.ps1` uses.

Three gotchas, all handled by `build.ps1` — keep them in mind if you compile by hand:

1. **PATH** — `mingw64\bin` must be on `PATH` or `gcc` cannot spawn `cc1` / load its DLLs and
   fails silently (exit 1, no diagnostics).
2. **`-std=gnu11`, not `-std=c11`** — under `c11`, `__STRICT_ANSI__` hides MinGW's
   `strcasecmp` / `M_PI`, which GCC 15 then treats as a hard error. `gnu11` also matches what
   ESP‑IDF compiles the firmware with.
3. **`-static`** — produces a self‑contained `anima.exe` that runs without the MinGW DLLs on
   `PATH`.

MSYS bash mangles Windows paths when calling the MinGW `gcc.exe`; build from PowerShell/`cmd`
with Windows‑style paths (as `build.ps1` does).

---

## 3. The wider menu (for what the harness can't reach)

The other approaches need no new installs — `idf.py`, QEMU, OpenOCD and the Xtensa GDB are
already under `C:\Users\indecenti\.espressif\tools`.

| You want to test… | Use | Hardware? | Wi‑Fi? |
|---|---|---|---|
| **ANIMA logic** (NLU / L1 / HDC) | **host harness** (§1) | no | no |
| Real device state, breakpoints, variables | **OpenOCD + `xtensa-esp-elf-gdb`** over USB‑JTAG | yes (one cable) | no |
| The **whole real firmware** (boot, httpd, services) | **`qemu-xtensa`** (`idf.py qemu`) | no | yes (QEMU gateway) |
| Display + Wi‑Fi‑to‑internet, maximum fidelity | **Wokwi** | no | yes |

Notes:
- **JTAG/GDB** is the answer to "inspect live device state without Wi‑Fi": the ESP32‑S3's
  built‑in USB‑JTAG (already the console transport — see [anima-live-debug] in memory and the
  serial‑console routing notes) drives `idf.py openocd` + `idf.py gdb` over the same cable.
  Flash once, then debug live. Avoid breakpointing radio code (watchdogs).
- **QEMU** runs the actual app image but has no Cardputer display/keyboard and no real radio;
  pair it with a serial command interface (below) to drive ANIMA without Wi‑Fi.
- **Wokwi** is the only option that simulates the screen *and* reaches the real internet, but
  it needs a (free) cloud token, a forced LGFX panel config (its board isn't a Cardputer), and
  serial‑injected input (the matrix keyboard is impractical to simulate).
- **Not built yet:** an `esp_console` REPL on USB‑JTAG. Adding one (commands like `logs`,
  `goto <app>`, `key <x>`, `fs ls`) would let `idf.py monitor` replace the Wi‑Fi `/api` calls
  in the daily loop, and would double as the input channel inside QEMU/Wokwi.

### One cable, no replug
On the ESP32‑S3 you can flash **and** monitor over the same USB‑JTAG port
(`idf.py -p <jtag-port> flash monitor`) — no need to juggle the flash‑UART (COM3) and the
console port. OTA is unreliable on this board; prefer serial flashing.

---

## 4. What the host harness cannot see

It is deliberately scoped to ANIMA's logic. It does **not** link FreeRTOS, `esp_http_server`,
Wi‑Fi, the real SD/FATFS, audio (helix‑mp3), USB, or the M5GFX UI. It also does not fill the
`{value}` placeholder in system replies (e.g. `reply: {value}.` for time/battery/storage) —
that substitution is done by the **app‑layer executor** (`app_anima.cpp` / `nucleo_app`) from
live device state, not by ANIMA core. For any of those, use the right row in §3.

---

## 5. Extending the harness

- **Activate L1 (semantic retrieval).** L0/HDC run immediately; L1 self‑disables (logs
  `L1 disabled`) until the encoder + index are present. Copy them next to the exe:
  ```powershell
  Copy-Item models\anima-it-encoder.bin, models\anima-it-index.bin `
            tools\anima-host\sd\data\anima\
  ```
  (the same `ANE2`/`AKB2` artifacts that go on the device SD, produced by `tools/anima/*.py`).
  On load you'll see `L1 ready: encoder 16384x192, AKB2 7304 vectors / 93 clusters`. Session
  and telemetry are written under `tools\anima-host\sd\data\anima\`.
- **Test the online tier on host.** Replace `anima_online_stub.c` with a real implementation
  (HTTP + JSON) and link it instead of the stub.
- **Regression sweeps.** Keep a curated `tools/anima-host/queries.txt` and diff the output
  across changes; a query that should answer but returns `tier=none`, or whose `intent`/`tier`
  shifts, is a regression you can see before flashing.

---

See also: [docs/anima.md](anima.md) (the cascade design), [docs/anima-online.md](anima-online.md)
(the network tier), [docs/anima-agent.md](anima-agent.md) (the agentic controller).

# Memory Budget (512 KB SRAM, no PSRAM)

This is the riskiest bet in the project: making Wi-Fi, BLE, the HTTP/WS server, the
display and the event bus coexist in ~512 KB. The answer is **mode switching** — we do
**not** run a full Wi-Fi IP stack and BLE heavy at the same time.

All numbers are engineering targets to validate with `idf.py size` and `heap_caps_get_free_size()`.

## Fixed reservations (always present)

| Item | Estimate |
|---|---|
| ROM/bootloader reserved + IDF `.data`/`.bss` | ~80 KB |
| FreeRTOS + task stacks (core services) | ~40 KB |
| Event bus ring buffer (fixed) | ~16 KB |
| SD / FATFS buffers | ~16 KB |
| Display: 2 partial framebuffers (~1/10 frame, 16bpp) | ~14 KB |
| App runtime + automation micro-VM | ~20 KB |
| **Subtotal fixed** | **~186 KB** |

Leaves **~326 KB** for the active transport profile below.

## Transport profiles (mutually exclusive heavy stacks)

| Profile | Active stacks | Approx cost | Notes |
|---|---|---|---|
| **A — Network admin** | Wi-Fi STA + LWIP + HTTP + WS | ~150 KB | 2–3 concurrent web sessions, delta only |
| **B — Offline admin** | BLE (NimBLE) GATT | ~45 KB | Wireless, no router; reaches Android browser |
| **C — Swarm** | ESP-NOW (no LWIP/IP) | ~30 KB | Cheaper than profile A; flagship mesh |
| **D — Wired** | USB CDC (WebUSB/Serial) | ~10 KB | No radio; lowest power |

The system picks one profile at a time and can switch at runtime. Profiles B/C/D leave
large headroom; profile A is the tight one and caps concurrent sessions.

## Rules that keep us inside the budget

- Static allocation for **core services only**; no per-request heap growth.
- **No static RAM for a feature that is not in use.** A native app's working buffers
  (transcripts, file lists, edit/scratch buffers, particle/effect pools, saved-item caches)
  must NOT live in file-scope `static` `.bss` — that RAM is then resident at boot and for the
  whole session even though the app is almost always closed. Instead **allocate on app
  `enter()` and free on `on_exit()`** (the app already enters `nucleo_exclusive`), so the cost
  is paid only while that app is open. Pattern: declare a `static T *p = nullptr;`, `calloc`
  it in `enter()`, `free`+null it in `exit()`, and null-guard every dereference (skip cleanly
  if alloc failed). See the conversions in `app_qr.cpp` (saved-QR scratch) and `app_anima.cpp`
  (the `s_msg` transcript ring + `s_row` wrap cache, ~7.4 KB) as the reference. That ANIMA
  conversion, measured on ADV, lifted the `pre-httpd` `largest_free_block` from 23552 → 25600
  and free heap 31968 → 36352 — a boot-gate win for RAM the closed app never needed resident.
  This is enforceable: `idf.py size --archive_details libnucleo_app.a` lists the per-symbol
  `.bss` — a multi-KB `s_*` array in an app is a red flag.
- WebSocket/BLE payloads are **delta events only** (see `event-protocol.md`), never full state.
- Partial display refresh — never a full 240×135×2 = ~63 KB framebuffer.
- Bounded ring buffers for logs/audio; backpressure instead of unbounded queues.
- Wi-Fi and BLE are **not** both kept hot; ESP-NOW replaces full Wi-Fi for swarm.

## The boot-time httpd gate is the real RAM ceiling

`httpd` starts **last** in the boot sequence (`main.c`), after Wi-Fi (~60 KB), the display, and
every service. It needs a contiguous block, not just free bytes. If `largest_free_block` at the
`pre-httpd` bootmark is too small, `nucleo_httpd_start()` fails and the firmware **`abort()`s on
purpose** ("no degraded boot") → reboot loop, black screen. So the binding constraint is not
"free heap" but **`largest_free_block` at `pre-httpd`**. Healthy reference (per `BOOTSTEP` serial
log): boot-start ~150 KB free / ~86 KB largest → pre-httpd ~40 KB / ~31 KB largest → httpd starts.
A build that boots at ~124 KB / ~63 KB largest reaches pre-httpd at ~14 KB / **~7 KB largest** and
fails. ~30 KB of static `.bss` is the entire margin between "boots" and "loops".

**The host ANIMA gate does NOT catch this** — it runs on the PC and never measures device boot
RAM. A green gate + clean build can still reboot-loop on hardware. **Always boot-test a firmware
change on a real device (serial `BOOTSTEP` log must reach `httpd`) before OTAing it to units you
can't easily recover.** See `releasing.md` for the serial recovery procedure.

## Validation gate

Before writing feature code, prove Profile A boots with ≥ 60 KB free heap under load.
If it fails, drop concurrent sessions to 1 or move admin default to Profile B.

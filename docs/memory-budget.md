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

- Static allocation for core services; no per-request heap growth.
- WebSocket/BLE payloads are **delta events only** (see `event-protocol.md`), never full state.
- Partial display refresh — never a full 240×135×2 = ~63 KB framebuffer.
- Bounded ring buffers for logs/audio; backpressure instead of unbounded queues.
- Wi-Fi and BLE are **not** both kept hot; ESP-NOW replaces full Wi-Fi for swarm.

## Validation gate

Before writing feature code, prove Profile A boots with ≥ 60 KB free heap under load.
If it fails, drop concurrent sessions to 1 or move admin default to Profile B.

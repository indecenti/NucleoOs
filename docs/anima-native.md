# ANIMA native app — the on-device baseline (C)

The canonical reference for the **native ANIMA assistant** that runs on the Cardputer screen
(`firmware/components/nucleo_app/app_anima.cpp`). This is the *device-side* counterpart of the web
ANIMA app: same cascade ([`anima.md`](anima.md), [`anima-online.md`](anima-online.md)), but driven
from the firmware with the device's own RAM, watchdog and per-board hardware.

This doc is the **definitive starting point** for any work on the native C app. It records the
lifecycle, the verified RAM budget, the online-transport stability contract, and the per-board
reality — plus the invariants a change must not break, and the gate that enforces them. If you touch
`app_anima.cpp`, `nucleo_exclusive.*`, or the on-device path of `nucleo_anima_online.c`, read this
first and keep it green.

> Hard context (never re-litigate): ESP32-S3, **no PSRAM**, ~14–18 KB free heap once the OS is up,
> the bottleneck is the **largest contiguous block**, and the **Task-WDT is 8 s with PANIC=y**
> (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=8`). See [`memory-budget.md`](memory-budget.md).

---

## 1. Lifecycle — free first, allocate last

The native app is the launcher's most RAM-hungry tenant (a 30 KB worker stack + the L1 index + a TLS
handshake's transient ~34 KB). The whole design is **reclaim everything reclaimable BEFORE allocating
anything**, so the contiguous block the worker/TLS need actually exists on a fragmented heap.

`enter()` (`app_anima.cpp`), in order:

1. `nucleo_screen_release()` + `nucleo_app_set_direct_draw(true)` — hand back the **32 KB** shared UI
   canvas (ANIMA draws direct to the panel). One clean contiguous block freed first.
2. `nucleo_audio_stop()` — release any prior media decoder buffers.
3. `nucleo_exclusive_enter(NX_HTTPD | NX_ANIMA_L1 | NX_DISCOVERY)` — suspend the heavy network
   subsystems **for the whole session** (~47 KB, see §2). **Not** `NX_VOICE` (ANIMA must speak — the
   voice engine is lazy, ~0 KB at rest, so not requesting it costs nothing) and **not** `NX_WIFI`
   (cloud queries need the radio).
4. Only now: `calloc` the small session buffers (`s_hist` ~0.94 KB, `s_ed_buf` 1 KB) — cheap, and
   they hold **zero `.bss`** when the app is closed.
5. The **30 KB worker is NOT spawned here.** It is created on demand in `submit()` on the first query,
   inside the open reclaim window. Spawning it at `enter()` made it live concurrently with the UI + L1
   + TLS for the whole session and panicked on launch after a media app fragmented the heap.

`leave()` (on_exit) restores everything itself, in a safe order: `save_chat` → `stop_worker` (orphans
the worker; it self-reclaims its 30 KB stack and any per-query window) → free the session buffers →
`nucleo_exclusive_exit()` (httpd / L1 / mDNS back). Every real exit path funnels through `leave()`
(Back/Esc → `close_app` → on_exit; app→app → `launch_by_id` → outgoing on_exit). On the LAUNCH-intent
hand-off (`tick()`), the worker is stopped **early** so the idle task reclaims its 30 KB during the
~0.6 s settle, then `launch_by_id` runs `leave()` which closes the window.

> ANIMA manages exclusive **imperatively** (`exclusive_flags = 0`), so the framework's declarative
> `close_app` safety-net (`s_app_excl`) does **not** cover it — `leave()` is the sole restorer. Keep
> `leave()` linear: never add an early `return` before `nucleo_exclusive_exit()`.

## 2. Verified reclaim inventory (the ~47 KB session window)

Measured 2026-06-24 (`BOOTSTEP` heap deltas + the real buffer/stack sizes). On this chip the number
that matters is **largest contiguous block**, not total free.

| Reclaimed at `enter()` | KB (real) | Helps | Source |
|---|---|---|---|
| Shared UI canvas (240×135 @8bpp) | ~32 (one clean block) | **largest** | `nucleo_ui.cpp` (`nucleo_screen_release`) |
| `NX_HTTPD` — HTTP server task stack | **~18** (`config.stack_size=18432`) | **largest** | `nucleo_httpd.c` |
| `NX_HTTPD` — handler table + sockets | ~1.5 | total | `nucleo_httpd.c` |
| `NX_ANIMA_L1` — hot-row cache `s_ec_row` | **~24** (≤128×192, less if fragmented) | **largest** | `nucleo_anima_l1.c` |
| `NX_ANIMA_L1` — directory + SD FILE* caches | ~2 | total | `nucleo_anima_l1.c` |
| `NX_DISCOVERY` — mDNS task + records | **~5** (~4 KB stack + records) | both | mDNS (`CONFIG_MDNS_TASK_STACK_SIZE=4096`) |

Session window total ≈ **47 KB** (httpd 18 + L1 24 + mDNS 5), **plus** the 32 KB canvas on a separate
axis. The stale header comments that advertised "~70 KB / ~30 KB / ~10 KB" were corrected to these.

**Ownership-correct exclusive.** `nucleo_exclusive_enter()` returns whether **this call** performed the
suspend (`return acted`), not global active-ness — so a caller that pairs enter→exit (the per-query
worker windows, Recorder AI, Video) only ever tears down what *it* stopped, and can never collapse the
session window another owner still holds. The per-query `!nucleo_exclusive_active()` guards are
belt-and-suspenders on top.

## 3. Online transport — the anti-reboot stability contract  {#stability}

`nucleo_anima_online.c` is shared by the **native worker** and the **web `/api/anima`** handler. A
single `esp_http_client_perform()` is **one un-pettable blocking call**. The binding rule:

> **Per-attempt TLS timeout MUST stay below the 8 s Task-WDT.** A timeout ≥ TWDT is a guaranteed
> reboot the moment a handshake stalls on the fragmented heap — exactly the .166 crash (timeouts were
> 8 s / 10 s / 20 s vs an 8 s watchdog; 4 retries × 10 s ⇒ up to 40 s hang ⇒ `TASK_WDT` panic).

The fix (2026-06-24), locked by the `online-stability (TWDT)` gate:

- **`HTTP_TIMEOUT = 6000`** — one shared per-attempt socket timeout for all chat paths
  (`http_get` / `http_post_json` / `http_post_anthropic`); 2 s margin under the TWDT, ample for a
  healthy ~1.5 s reply.
- **`TLS_TURN_BUDGET_MS = 10000`** — a wall-clock budget across the POST retry loop: once spent, bail
  to an honest miss. Bounds the cumulative even if every retry stalls.
- **`tls_wdt_pet()`** — resets the WDT (only if the running task is subscribed; no-op otherwise) at
  every retry boundary and per-chunk inside the long `transcribe` upload (which legitimately keeps its
  30 s timeout — audio upload, unwatched task — but must pet).
- **The pre-flight heap gate** (`online_tls_heap_too_low`, ≥10 KB block **and** ≥38 KB free, post
  L1-unload) still refuses a turn whose heap is clearly too tight. It cannot predict the *in-handshake*
  fragmentation collapse, which is why the timeout bound is the real guard.
- **Diagnostic logging.** Every online failure logs the reason **and** heap state immediately
  (`… FAIL status … free=… largest=…`, `… budget …ms spent … bail …`) — visible in `/api/logs`, so
  when ANIMA goes silent online you see *why* (heap-too-low vs transport stall vs budget) at once.

## 4. Per-board reality (normal vs ADV)

One **universal binary** auto-detects the board. The **ADV** carries extra resident drivers (ES8311
codec, TCA8418 keyboard, BMI270 IMU), so its post-boot heap is much tighter — measured floor ≈ **9.4 KB
free / 7.2 KB largest** (vs ~14–18 KB on the original). Consequence: at the boot floor the online TLS
gate can't pass on the ADV → online ANIMA bails clean to offline **unless** a web client is connected
or the screen is off (the stand-down frees ~32 KB; see [`web-focus`/memory]). Not a crash — the gate
refusing honestly because the contiguous RAM genuinely isn't there.

**ADV serial-flash gotcha:** the USB-Serial-JTAG has no real RTS, so after `flash.ps1 -Port COM4` the
ADV can sit in `boot:0x23 (DOWNLOAD) / waiting for download` — the app never starts and `.104` is
unreachable (not a firmware fault). Unstick with
`esptool.py --chip esp32s3 -p COM4 --after watchdog_reset flash_id` → it reboots from flash
(`boot:0x2b SPI_FAST_FLASH_BOOT`). Read the serial console via `[System.IO.Ports.SerialPort] COM4
@115200`. Benign boot noise: I2C nacks = IMU probe (0x68 fail → 0x69 BMI270); `oom alloc fail 64804 B`
= the canvas trying 16 bpp (240×135×2) then falling back to 8 bpp.

## 5. Invariants — do not regress

1. **`enter()` frees the canvas + the exclusive window BEFORE any allocation**, and the 30 KB worker
   stays on-demand (never spawned at `enter`). Big blocks first, small allocs last.
2. **Keep `.bss` as `.bss`.** Do **not** move ANIMA's resident static buffers (`s_msg`/`s_full`/
   `s_row`/`s_res`) to heap-on-enter to "save RAM": `.bss` is a separate region — heap-allocating them
   during a session only **fragments** the heap around the worker block. (It helps other apps when
   ANIMA is closed, never ANIMA's own session.)
3. **Never suspend `NX_VOICE` in ANIMA** — it gates the whole voice engine including TTS output, so it
   would mute ANIMA's speech. It's already ~0 KB at rest.
4. **Online per-attempt timeout < TWDT, always** (§3). Use the `HTTP_TIMEOUT` symbol for chat paths —
   no raw numeric literals that dodge the bound. Keep the wall-clock budget, the WDT pets, and the
   heap-state failure logs.
5. **`nucleo_exclusive_enter` returns per-call ownership** (`return acted`), and `leave()` is the only
   restorer for ANIMA — keep it linear.
6. **Right-sizing the 30 KB worker stack needs DATA, never a guess** (a blind shrink stack-overflow
   panicked before). `stop_worker()` logs the stack high-water at every session close; read it from
   `/api/logs` and only then trim, with margin.

## 6. How to verify (host-first, then one board)

- **Logic + invariants, no device:** `npm run anima:gate` — includes `online-stability (TWDT)`
  (`tools/anima-host/online-stability-check.mjs`), the source-invariant guard that locks §3 and the
  ownership-return. Must be green before any flash.
- **It compiles for the device:** `tools/flash.ps1 -BuildOnly` (no flash).
- **It boots (the gate can't see this):** flash **one** unit over USB and read the serial `BOOTSTEP`
  log — it must reach `BOOTSTEP httpd`, not `httpd-FAILED`/`abort()`. See [`releasing.md`](releasing.md).
- **It's stable on-device:** reproduce a fragmenting flow (Voice Trainer → online query) and confirm
  `/api/logs` shows at most an honest ~6–10 s bail with a `FAIL … free=… largest=…` line, **no reboot**.

## 7. ANIMA Solo & the native chat UX — RAM model and known limits

**Solo boot personality.** Opening ANIMA from the full OS sets an RTC flag and `esp_restart()`s into a
dedicated boot that SKIPS httpd/mDNS/recorder/auth/IR and runs the whole assistant — UI loop + the
(possibly online) query + TTS — on ONE `anima-solo` task (`main.c`). This hands the assistant a large,
unfragmented heap (~50 KB free / ~32 KB largest at query time) so a cloud TLS handshake fits — which
the 18 KB-httpd full-OS layout cannot spare.

**The hard RAM facts (why the UX is shaped the way it is):**
- The `anima-solo` task stack is **26624 B and already at the boot ceiling** — when it is created the
  largest free block is ~27.6 KB, so the carve *just* fits (~1 KB margin). **It cannot be grown** (no
  bigger contiguous block at that point). A long online answer's deep JSON/TLS parse runs near this
  limit (`stack_hw` ≈ 5.6 KB free measured on a 339-char answer). Keep per-call frames small; do **not**
  add large stack locals on the query/present/voice path.
- **The query runs INLINE on that one task** — no off-loop worker in Solo (two 26 KB + 30 KB stacks plus
  the ~35 KB TLS window do not fit a no-PSRAM chip). So during `nucleo_anima_query()` the UI loop is
  **blocked**: `tick()`/`on_key()` do not run, nothing animates, no key is processed until it returns.
- **Display-DMA and SD-DMA must never run concurrently** — they hard-stall (RTCWDT, no backtrace). You
  cannot draw from a side task while the query (or a TTS render) is touching the SD.
- **`.bss` is sacred.** Adding even ~1 KB of static `.bss` (e.g. making `present_result`'s local
  `reply[1024]` `static`) pushed the ADV's full-OS `httpd_start` over its boot-heap edge →
  `httpd-FAILED → abort` reboot loop. Keep query/present buffers on the stack (small) or transient
  heap; never grow `.bss`. The gate cannot see boot RAM — only a serial `BOOTSTEP` read on the **ADV**
  catches this (see `releasing.md`).

**Chat UX implemented (`app_anima.cpp`):** instant user echo + an animated "thinking" dot wave in the
input row; footer "Invio per fermare" during a turn (painted immediately via `launcher_render_hint_bar()`
because the framework footer is frozen during the inline turn); text-before-voice with a **typewriter**
reveal (word-by-word, ~40 frames; `s_reveal` truncates `s_full`; `draw_body` skips the full-body clear
while the text grows without scrolling = anti-flicker); **Invio = stop** at every interruptible point
(the typewriter returns "stopped" → skips the voice; the voice polls `nucleo_audio_playing()` every
40 ms so it stops mid-clip); **voice cap 340 chars** → one "read it on screen" hint, never twice
(`nucleo_tts_say_quiet()` stays mute on an uncovered phrase instead of speaking the hint itself);
**Esc → full-screen confirm modal** (big font); Up/Down scroll the chat, Ctrl+Up/Down = command history.

**TTS-render TWDT fix.** A knowledge answer's TTS render reads many clips from SD (`fseek` per clip);
on the ADV's slow SD the cumulative time exceeds the 8 s task-WDT → `anima-solo` TWDT reboot (real
backtrace: `speak_result → render → tts_index_find → fseek`). Fix: the inline path keeps the task
**unsubscribed from the WDT through the whole voice** (re-adds it only after `speak_result`), and
`render()` feeds the WDT per token.

**ADV httpd boot fix.** The ADV booted full-OS with only ~30 KB free at pre-httpd and `httpd_start`
needs ~26 KB → an **intermittent** OOM; the failed attempt left port 80 bound so the retry hit
`listen()` **EADDRINUSE (errno 112) → abort → reboot loop**. Fix: start mDNS (~12 KB) **after**
`httpd_start` so httpd comes up with ~42 KB free (deterministic); mDNS then starts post-httpd (~6 KB,
fits). Verified 6/6 cold boots on the ADV.

### Known limits / TODO (not yet done)
- **Dots freeze during the pure model-call.** The inline query is one blocking call on the only task,
  and a side task/timer to animate would risk the display+SD hard-stall (and cost RAM). So the dots
  animate during the Wi-Fi-reconnect wait and during the typewriter, **not during the TLS call itself**.
  A safe fix would be an `esp_timer` that redraws ONLY the tiny dot region, gated to online-only turns
  (which touch no SD) — deferred as risky.
- **Long answers still flicker on scroll.** The anti-flicker no-clear only holds while the revealed text
  fits without scrolling; once it overflows and the view scrolls, the body is cleared each frame.
- **A >340-char / long-form answer is voiced as a single "read it" hint, not narrated.** By design (the
  cap), but chunked narration of long answers is not wired on the Solo inline path (`run_longform` is
  worker-only / full-OS).
- **The original board (.166 / COM3) trails the ADV.** The chat-UX + boot/voice fixes were flashed to
  the ADV (.104 / COM4) during iteration; re-flash the original to keep the one universal binary in
  sync (`nucleo-release-dual`).

## See also
[`anima.md`](anima.md) (cascade + offline identity) · [`anima-online.md`](anima-online.md) (the online
tiers) · [`memory-budget.md`](memory-budget.md) (global RAM budget) · [`releasing.md`](releasing.md)
(boot-test before OTA) · [`debugging.md`](debugging.md) (host harness).

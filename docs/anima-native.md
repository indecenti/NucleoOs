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

## 1. Lifecycle — Solo only, one task, heap-on-enter

The native app **only ever runs in its own ANIMA Solo boot** (§7). In the full OS, `enter()` does one
thing: arm the RTC copy of a seeded question (if any) and call `nucleo_anima_solo_request()`, which
`esp_restart()`s and **never returns**. The Solo flag is latched for the whole boot, so everything past
that gate in `app_anima.cpp` runs in Solo, on the one `anima-solo` task. There is **no worker task**
and no per-query exclusive window. Opening another app is a **reboot hand-off** (§8): a LAUNCH answer
offers "Enter = open Music", and Enter stages the app id in RTC and reboots into the full OS, which opens
it. (`open_file` answers still point to Esc + the launcher.) The old full-OS worker path
(`anima_worker`, `run_longform`, `stop_worker`, the deferred launch in `tick()`) was unreachable and has
been removed.

`enter()` (Solo), in order:

1. Consume the RTC preset magic (a question another app staged, see below).
2. `nucleo_screen_release()` + `nucleo_app_set_direct_draw(true)` — hand back the **32 KB** shared UI
   canvas (ANIMA draws direct to the panel); `nucleo_audio_stop()`.
3. Settings + `nucleo_anima_init`, then the session buffers, all `calloc`'d once, early, from the fresh
   Solo heap: **`s_ses`** (one `AnimaSession` block, ~4.5 KB: the query result, the full current answer,
   the input/request/draft lines, the IDEE slots + recent values, the calendar cache), `s_hist`, `s_msg`,
   `s_row`. None of them is `.bss`, so the **normal OS boots no longer reserve them** (~4.9 KB of `.bss`
   given back to every boot); in the Solo boot it is ~neutral (same bytes, taken once, no fragmentation).
   The editor block (`s_ed`, ~1.6 KB: buffer + path + wrap layout) is allocated only while the editor is
   open (`editor_open` / `editor_close`), so it is never held during a query's TLS handshake.
4. If `s_ses` can't be allocated, the app shows an out-of-memory notice and Esc leaves; every framework
   callback (`draw`/`tick`/`on_key`/`on_tab`/`on_back`) bails while `s_ses` is NULL.

**Cross-app ask.** `nucleo_anima_app_ask()` (notify action, ESP-NOW link) runs in the **full OS**, where
`s_ses` doesn't exist: it writes the question straight into RTC no-init RAM (`s_rtc_preset`) and sets a
1-byte "staged in this boot" flag; the full-OS `enter()` arms the magic only when staged, and the Solo
`enter()` auto-submits it. A question staged in a boot where ANIMA never opens is dropped at the next
reboot, as before.

`leave()` (on_exit, only ever reached from `close_app()` right before its `esp_restart()`): `save_chat`
→ free every session block → re-acquire the canvas. Keep it linear.

## 2. Verified reclaim inventory (the ~47 KB session window)

> Historical for the native app: since ANIMA runs only in Solo (§1), it no longer opens this window —
> the Solo boot never starts these subsystems. The inventory still holds for the full-OS
> `nucleo_exclusive` callers.

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

`nucleo_anima_online.c` is shared by the **native app (inline, Solo)** and the **web `/api/anima`** handler. A
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

1. **ANIMA runs only in Solo, inline on one task.** No worker task, no per-query exclusive window: the
   Solo boot never started httpd/L1/mDNS, and `nucleo_exclusive_exit()` there would *start* them.
2. **No sized `.bss` in `app_anima.cpp`.** Every buffer the app owns lives in a heap block allocated in
   the Solo `enter()` (`s_ses`, `s_msg`, `s_row`, `s_hist`) or while the editor is open (`s_ed`) — the
   `.bss` would otherwise be reserved in *every* boot (a +9 KB `.bss` once boot-looped a device). Only
   scalars and a few pointers stay static (~125 B). Anything that must outlive the Solo reboot goes in
   `RTC_NOINIT` (`s_rtc_preset`, a tagged slot: the magic says "question" or "app to open"), never
   `.bss`. New UI state (view mode, menu paint cache, confirm card, launch offer…) lives in `s_ses`.
   No `s_ses->` access outside the session: all callbacks bail on NULL.
3. **Never suspend `NX_VOICE` in ANIMA** — it gates the whole voice engine including TTS output, so it
   would mute ANIMA's speech. It's already ~0 KB at rest.
4. **Online per-attempt timeout < TWDT, always** (§3). Use the `HTTP_TIMEOUT` symbol for chat paths —
   no raw numeric literals that dodge the bound. Keep the wall-clock budget, the WDT pets, and the
   heap-state failure logs.
5. **`nucleo_exclusive_enter` returns per-call ownership** (`return acted`) — for its full-OS callers
   (Recorder AI, Video, …). The native ANIMA app never enters exclusive; keep `leave()` linear.
6. **Nothing big by value on the query path.** The `anima-solo` stack is 26 KB with ~5.6 KB measured
   headroom (§7). The ~1.4 KB `anima_result_t` is built **in place** in `s_ses->res` (placement-new of
   the prvalue = guaranteed copy elision, `submit()` frame 1648 → 256 B) and passed by reference.
7. **One calendar reader.** `cal_load()` is the only parser of `calendar.json`, with one cap
   (`CAL_MAX_BYTES` 32 KB: a bigger file is refused and logged, the writer fails closed). `cal_refresh()`
   caches the OGGI list, agenda readout and next-event glance, and re-parses only when the file's
   size/mtime, the day or the language changed (a menu open costs a `stat()`); `enter()` forces one read.

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
reveal for **online answers only** (offline answers are instant, so they appear at once): at most
`TW_FRAMES` = 10 frames × 35 ms whatever the length (was ~40–80 × 50 ms, +1.5–2.5 s on a mid-length
answer); `s_reveal` truncates `s_ses->full` and each frame re-wraps **only the current answer** (+ the meta
lines after it, `wrap_ring(k, true)`), not the whole transcript; rows paint in place, so no frame blanks
the body (§8). **Keys during a turn** (`turn_key`): **Invio/Esc = stop**
at every interruptible point (the reveal returns "stopped" → skips the voice; the voice polls
`nucleo_audio_playing()` every 40 ms so it stops mid-clip); a **printable key** (no fn/ctrl) also ends the
reveal/voice and is **kept** (`s_ses->carry`) as the first character of the next question — keys typed
while the query ran are drained the same way, the first printable one ends the drain and later keys stay
queued for the normal loop (they used to be silently dropped); **voice cap 340 chars** → one "read it on screen" hint, never twice
(`nucleo_tts_say_quiet()` stays mute on an uncovered phrase instead of speaking the hint itself);
**Esc → full-screen confirm modal** (big font). **Keys (Notes-editor rule):** the driver delivers
`; . , /` as arrows carrying their character — wherever you write (chat, welcome deck, IDEE fill-in
forms, file editor) they **type** (`2.5`, `ciao.`, a decimal comma, `/l1`); `,` (routed to the back
handler as Left) is a comma and never leaves. The arrow meaning needs a modifier: **fn+; / fn+.** page
the chat — a page per press on an empty line, a row while typing (move the deck pick / hop form fields),
**fn+/** accepts the ghost completion (empty line: cycles Offline→Ibrido→Solo online with a footer toast;
right after an offline "I don't know" it retries online instead, §8), **ctrl+; / ctrl+.** = command history, rebuilt
on enter from the user's lines in the restored chat. Menu lists keep plain arrows. Driver bit names are
offset from the printed legends (printed fn → `NK_MOD_CTRL`, printed ctrl → `NK_MOD_FN`; see `key_fn()`).
Editor: Ctrl+S never overwrites (`name-2`, `-3`…; quick notes are per-second), a failed save keeps the
text open, Esc with text asks first. The "-- ripresa --" separator is display-only (never persisted).

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
- **"Enter = open <app>" needs one OS-side line** (§8): until `nucleo_app_run()` asks
  `nucleo_anima_take_launch()`, the reboot lands on the launcher without opening the app.
- **An NX_SOLO target (Music, Video, Radio, games) costs two reboots**: ANIMA Solo → full OS → the app's
  own Solo boot. ANIMA Solo registers no other app, so it cannot read the target's flags (NX_WIFI /
  NX_BLE) and must let the launcher path decide — correct, just slower.
- **A >340-char / long-form answer is voiced as a single "read it" hint, not narrated.** By design (the
  cap). The old segmented long-form narration (`run_longform`) only existed on the removed full-OS worker
  path; `nucleo_anima_online_longform()` is still in the engine if Solo ever wires it inline.
- **The original board (.166 / COM3) trails the ADV.** The chat-UX + boot/voice fixes were flashed to
  the ADV (.104 / COM4) during iteration; re-flash the original to keep the one universal binary in
  sync (`nucleo-release-dual`).

## 8. Reading, opening apps, honest misses, the OS look (Phase 4)

**Reader.** The transcript view is **top-anchored** (`s_ses->vtop` = first visible row) with three modes:
`V_FOLLOW` pins the newest row to the bottom (every new question), `V_ANCHOR` puts the **first row of the
answer** at the top (every answer, so a long one reads from its start — the view no longer lands on its
last rows), `V_MANUAL` is where you paged to. The anchor is a ring slot (`Row.mi`), resolved to a row
index at paint time, because rows are re-wrapped on every push. The view's top row drops its 3 px message
gap, so a page holds one more line (4 big rows in the chat, 7 in the reader).

| Where | Key | Does |
|---|---|---|
| chat, empty line | **fn+; / fn+.** | one **page** up / down (the last visible row stays as context) |
| chat, typing | fn+; / fn+. | one row (as before) |
| chat, empty line | **Enter** | opens the **full-screen reader** (or opens the offered app, below) |
| reader | `;` `.` `/` `,` space (and fn+ …) | page up / down — no text entry, so the plain arrows page |
| reader | Enter / Esc / DEL | close; any letter closes it **and** starts the next question |

The reader goes `nucleo_app_set_fullscreen(true)`: header, input and footer disappear, the text gets
`RD_BODY` = 126 px (~7 FreeSans rows) plus a 9 px status strip (page "2/5", the keys). In the chat the page
indicator replaces the date in the header's right field whenever the current answer overflows the body
(`answer_pos`). ~40 presses to read a 1 KB answer became ~5 (chat) / ~3 (reader).

**Opening apps from an answer.** ANIMA Solo registers no other app, so it cannot open one itself. A LAUNCH
result is mapped to the native id (`nucleo_app_native_id`, plus `settings`→`wifi`) and named in the OS
language (`launcher_app_localized_name`, or a small proper-noun table); the answer reads **"Invio = apri
Musica" / "Enter = open Music"** and the footer "invio apri app". Enter on the empty line (`launch_now`)
writes the id into the **same RTC no-init slot as the preset question** — `s_rtc_preset_magic` says which
of the two it holds (`ANIMA_LAUNCH_MAGIC`), so it costs **0 extra RTC bytes** — notes "Apro Musica..." in
the chat and calls `nucleo_app_exit()` (leave() saves, `close_app()` reboots into the full OS). Web-only
targets (paint, spreadsheet…) answer "only in the web OS". IDEE ▸ *App e file* gained "Apri app" (a form),
"Musica" and "Impostazioni"; GUIDA's "Apri Musica" promise is now real.

The full OS consumes it with **`extern "C" const char *nucleo_anima_take_launch(void)`** (app_anima.cpp):
one-shot, honoured only after our own `esp_restart()` (`ESP_RST_SW`) — never after a crash or a cold
power-on. **OS-side hook (required, not yet in `nucleo_app.cpp`)** — in `nucleo_app_run()`, in the
post-boot chain right after the Recorder reopen:

```c
extern "C" const char *nucleo_anima_take_launch(void);                     // file scope
else if (const char *aid = nucleo_anima_take_launch()) nucleo_app_launch_id(aid);   // after reopen_recorder_pending()
```

`nucleo_app_launch_id()` then takes the normal launcher path, so an NX_SOLO app gets its own fresh-heap
boot with its NX_WIFI/NX_BLE flags. A stale handoff (no hook yet) is cleared by the next ANIMA Solo enter().

**Honest misses + status.** Every answer carries a discreet **source + time tag** at the end of its last
row (or on a small row of its own when it doesn't fit): `calc`, `L0`, `L1`, `L2`, `web`, with the turn time
when ≥ 0.25 s ("L1 · 0.4 s", "web · 2.1 s"). Tags are persisted inside the ACH1 record as a
`\x1f<tag>\x1f` text prefix (old files load unchanged). An abstention (tier NONE, not a follow-up question)
adds **one next step**, display-only (never saved): network up but mode Offline → "fn / : prova online"
(fn+/ then switches to Hybrid and re-asks the same question); otherwise → "TAB > IDEE: cosa so fare".
Before the blocking query the input row says **"cloud: max 12 s"** when an online tier may answer (the
engine's per-question network budget), so the frozen screen is explained.

**Follow the OS.** Language = the OS language: `s_en` mirrors `nucleo_i18n_is_en()` (re-read in
`load_settings`), the engine init gets `nucleo_i18n_lang()` (was a hard-coded "it"), IA ▸ Lingua switches
the **OS** language (`nucleo_i18n_set_en`), and the remaining Italian-only strings (modes, /info, create_file
replies, /help) are bilingual; `/mode` and `/hybrid` join `/modo` `/ibrido`. Theme = the `THEME_*` roles for
all chrome (`BG/FG/MUTED/DIM/LINE/INK`); the banned capsule greys (`CAP 0x1A8B`, `SURF 0x10A2`) are gone —
the focused row / active tab is the kit's ACC pill with INK text, the tab bar is `app_ui_tabs`. Named
content colours stay (the ANIMA violet accent, GRN, the user-echo blue, AMBER). Transcript colours are stored
as **role codes** (1..7) and resolved at paint time, so a restored chat follows the active theme (old files
hold RGB565 values and paint unchanged; an older firmware reading a new file would paint those codes
near-black).

**Anti-flicker (direct draw, no back-buffer).** Nothing on a repeating cadence clears under content:
`box_text()` prints opaque (the font fills its own cells) and fills only the leftover pixels; `pill_band()`
paints a focused row over the old one (only the corner squares and margins go to BG); the scroll rail is
three non-overlapping pieces. Chat: header fields, transcript rows (scroll, page flip, typewriter frame),
input row and deck all repaint in place; a scene change (menu/editor/modal/reader closed, chat cleared)
is the one clear (`s_d_clear`). Menu: a **scene** key (tab, IDEE level, form, language) decides the one
clear + tab strip + static parts; inside a scene each list row repaints only when its signature (row, y,
focus, value) changed (`s_ses->msig`), GUIDA only on a page flip, the IDEE form only its value/label/preview.
Editor: keys repaint the lines in place, the **blink toggles only the caret bar**. `nucleo_app_repaint_gen()`
(an overlay painted over us) forces one full repaint.

**Clear chat** (IA ▸ Pulisci, `/cancella`, `/clear`) asks with the kit's `app_ui_confirm` card, focus on
No (`;` `.` `,` `/` toggle, Enter picks, y/n, Esc = No).

**RAM.** `.bss` 128 → 125 B, `.data` −4 B, RTC unchanged (144 B). Heap-on-enter: `AnimaSession` +88 B
(4576), the message ring +192 B (16 × 12 B tags), the editor block +6 B. Query-path stack frames unchanged
(`submit` 256 B, `present_result` 1632 B). Flash +~9.8 KB (`app_anima.cpp.o` text 61529 -> 71337 B).

**Verify on hardware:** a long online answer lands on its first row; fn+. pages it (header "2/5"), Enter
opens the reader (7 rows, strip "2/5"), `;`/`.` page, a letter drops back and types; "apri musica" →
"Invio = apri Musica" → Enter → reboot → (with the hook) Music opens in its Solo boot; Offline + Wi-Fi +
an unknown question → "fn / : prova online" → fn+/ re-asks in Hybrid; the "cloud: max 12 s" note during
an online turn; tags "L1 · 0.4 s" / "web · 2.1 s"; Settings ▸ Theme = AMOLED / Hacker Green recolours
ANIMA (chrome, rows, restored chat); IA ▸ Lingua changes the OS language; no flicker on page flips,
typewriter, menu focus moves, IA toggles and the editor caret; Pulisci asks first. On **both** boards.

## See also
[`anima.md`](anima.md) (cascade + offline identity) · [`anima-online.md`](anima-online.md) (the online
tiers) · [`memory-budget.md`](memory-budget.md) (global RAM budget) · [`releasing.md`](releasing.md)
(boot-test before OTA) · [`debugging.md`](debugging.md) (host harness).

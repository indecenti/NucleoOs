# Nucleo Code — a small Claude Code on the Cardputer (design, 2026-11-04)

Status: **design, not built**. Owner of the decision: the maintainer. Target: the native store, ABI 9.

Nucleo Code is a coding and writing agent that lives on the Cardputer itself: you ask, it plans, reads and
edits files in a workspace on the SD card, runs and debugs the programs it writes, and keeps going until the
task is done, with you approving what changes. It does on 512 KB of SRAM, without PSRAM, what the known
microcontroller agents need 8 MB of PSRAM for.

## 1. Goals and non-goals

Goals
- Write, run, debug and keep programs on the device (JavaScript, run on the Cardputer; Python, run in
  Anthropic's code-execution sandbox, results back on the SD card).
- Write documents: text, Markdown books by chapter, CSV; real `.xlsx` / `.docx` / `.pdf` built in the
  cloud sandbox and saved to the SD card.
- Work on a **workspace** on the SD card with Claude Code's habits: a project memory file, a plan the user
  can see, every write approved (or an explicit auto mode), undo per turn.
- Show at all times what the agent works with: workspace, model, effort, context used, cost, mode.
- Programs it writes can become **apps in the launcher**.

Non-goals
- No local model: the reasoning is the cloud model's (Claude by default, the fallback chain of
  `ai-models.md`). Offline the app opens, browses, edits and runs files, but does not think.
- No native C compiled on the device (store modules stay built and signed on a PC).
- Not a replacement for Agenti in the web OS: the same tool contract, so a project moves between them.

## 2. The constraints, measured

| Fact | Value | Source |
|---|---|---|
| SRAM / PSRAM | 512 KB / none | `memory-budget.md` |
| Full OS free heap | ~14–18 KB | `memory-budget.md` |
| Solo boot, Wi-Fi STA + one TLS session | ~79 KB free, ~32 KB largest block | `memory-budget.md`, `sd-content-install.md` |
| One TLS session | ~24–32 KB contiguous; records ≤ 8 KB | `memory-budget.md` |
| Native module code | ≤ 256 KB (flash scratch region) | `native-apps.md` |
| Network for modules (ABI 7) | raw TCP only (`nucleo_tcp`), no TLS | `sdk/native/abi/v7.txt` |
| Claude on the device today | streamed, effort-aware, never a billed retry, ~1.5 KB of parse RAM | `ai-models.md` |

Design rule that follows: **one heavy thing at a time**. While a request streams, the script engine has
no arena; while a script runs, there is no TLS session. The UI and the session index stay under ~12 KB.

## 3. Prior art, and what we take

| Project | What it is | Hardware | What we take |
|---|---|---|---|
| Claude Code, opencode | coding agents on a PC | — | tool set (view / edit / run / search / todo), permission modes, project memory file, plan list, environment block |
| OpenClaw | personal agent | PC / server | memory as plain files (`SOUL/USER/MEMORY.md`), skills as Markdown |
| MimiClaw (DFRobot) | OpenClaw-style agent on ESP32-S3 | **16 MB flash + 8 MB PSRAM** | memory files on flash, ReAct loop with Anthropic tool use |
| ESP-Claw (Espressif / M5Stack) | "chat coding" IoT agent, dynamic Lua | ESP32-S3 boards (PSRAM not stated), under active development | self-programming through a script runtime |

None of them runs a coding agent without PSRAM. The difference here is that the device keeps almost
nothing in RAM: the conversation lives on the SD card and on Anthropic's side (message threads), the
heavy document work runs in Anthropic's sandbox, and the engine runs only between requests.

## 4. Architecture

```
 ┌──────────────── native app "Code" (.nap, store, NX_SOLO, perm net) ────────────────┐
 │  UI (240x135 + ADV 320x240)   agent loop   tools   session store (SD)   undo        │
 │        │                         │           │              │                        │
 │        │            ┌────────────┴───┐   ┌───┴──────────────┐                         │
 │        │            │ LLM service    │   │ script runtime   │  mquickjs, 24 KB arena, │
 │        │            │ (firmware ABI9)│   │ (in the module)  │  only while running     │
 └────────┼────────────┴───────┬────────┴───┴──────────────────┴─────────────────────────┘
          │                    │ TLS, key, SSE, tool_use, threads, cache, fallbacks, cost
          ▼                    ▼
     lg:: UI kit        api.anthropic.com  (Messages API, code execution, Files API, web search)
```

### 4.1 LLM service in the firmware (ABI 9: `nucleo_llm_*`)
The firmware already owns TLS, the key vault and a streamed, effort-aware Claude client
(`nucleo_anima_online.c`, `anima_claude.h`). ABI 9 exposes it to modules instead of giving them TLS:
- `nucleo_llm_open(cfg)` — model, effort, system, tools (JSON), thread on/off; the key never leaves the firmware.
- `nucleo_llm_send(h, messages_json)` — the new messages only (a user turn or the `tool_result`s).
- callbacks: `on_text(chunk)`, `on_tool_use(id, name, input_json)`, `on_done(stop_reason, usage)`.
- `nucleo_llm_cancel(h)`; `nucleo_llm_close(h)`.
- The service does: SSE parsing (text, `tool_use` + `input_json_delta`, `server_tool_use`), message threads
  with replay from the app's own copy on `thread_not_found`, prompt-cache breakpoints, the fallback chain,
  effort clamped per model, `usage` for the cost meter. Bounded buffers only; tool input ≤ 8 KB per call.
- Levels only grow: ABI 9 = v8 + `nucleo_llm_*`. Firmware without it: the Store hides the app (`abi` 9).

### 4.2 The agent loop (in the module)
ReAct, like Claude Code: send → stream text to the screen → on `tool_use` run the tool (asking first when
the mode says so) → send the `tool_result` → repeat until `end_turn`. Hard caps per turn: 24 tool calls,
the effort's wall clock, and a cost ceiling the user sets (default €0.20 a turn). Esc cancels at once.

### 4.3 The script runtime: MicroQuickJS
- MIT, ~100 KB of code, runs in as little as ~10 KB of RAM, allocates only inside a buffer the host gives
  it (no `malloc`), tracing compacting GC, clean exception on out-of-memory, an interrupt handler for time
  limits, stdlib tables in ROM (built by `mquickjs_build.c`).
- Dialect: an ES5-like subset in strict mode: no Promise / async / await, modules, generators, Proxy,
  Symbol; ASCII-only case folding; `Date.now()` only. The system prompt states the dialect in ~15 lines, and
  the run → error → fix loop catches the rest.
- Arena: 24 KB by default (configurable up to the free block), allocated when `run` starts, freed when it
  ends. Output captured into a 2 KB ring; the tool result gets stdout tail, stderr, exit status and the
  error with file:line.
- Device API, synchronous (the device has no event loop to hide): `print`, `fs.read/write/append/list/
  exists/remove` (confined to the workspace), `screen.clear/text/rect/line/flush` (the `lg::` kit, tiles),
  `keys.read(timeout)`, `sound.tone(f, ms)`, `sleep(ms)`, `time()`, `http.get(url)` (firmware fetch),
  `anima.ask(q)`. The same names as the web OS `os.*`, so a script moves between the two.

### 4.4 Why MicroQuickJS (and not MicroPython)

| Engine | RAM to run | Code | Fits beside TLS | Model fluency | Notes |
|---|---|---|---|---|---|
| **MicroQuickJS** | ~10 KB min | ~100 KB | yes | very high (JS) | MIT; buffer arena; interrupt; same language as the web OS |
| Lua 5.4 | ~25 KB | ~120 KB | yes | high | MIT; mature; fallback choice |
| Berry | low (ROM-resident constants) | small | yes | low | used by Tasmota on ESP32; little training data |
| PikaPython | ~4 KB | 32–64 KB | yes | Python-like | a subset; weak stdlib |
| MicroPython | ~56–64 KB initial heap on ESP32, ~100 KB default | 300 KB+ | **no** | very high | sized to own the chip; its heap is the TLS session's RAM |
| JerryScript / Duktape / Espruino | tens of KB; Espruino's TLS alone took 80% of its vars | 150–250 KB | marginal | high | older, heavier |

Python still has a place: `run_python` sends the script to Anthropic's code-execution sandbox (pandas,
numpy, openpyxl, xlsxwriter, python-docx, pypdf, matplotlib preinstalled) and brings stdout and any file
back to the SD card. That is also how real `.xlsx` / `.docx` / `.pdf` files are made.

## 5. The workspace on the SD card

```
/work/<project>/            the workspace root (the only place tools may write)
  NUCLEO.md                 project memory: goals, conventions, what was learned (the agent keeps it)
  ...                       the user's files
  .code/
    session-<n>.jsonl       the full conversation (the app's own copy: thread replay, /resume)
    todo.json               the plan shown in the UI
    undo/<turn>/            files as they were before each turn's writes
/system/code/USER.md        who the user is, preferences (shared by every project; the user can edit it)
/apps/scripts/<id>/         programs promoted to launcher apps (main.js + manifest.json)
```

Every path from the model is resolved against the root and rejected if it escapes (`..`, absolute paths
outside, encoded traversal). Size caps: 256 KB a file written by the agent, 64 KB a `view`.

## 6. Tools (the contract)

| Tool | Kind | Asks first (mode Ask) | Result |
|---|---|---|---|
| `str_replace_based_edit_tool` (`text_editor_20250728`: view, create, str_replace, insert) | Anthropic-defined, no schema tokens | create / str_replace / insert | file text (paged, `max_characters`) or error |
| `list` (glob) | ours | no | paths, sizes |
| `search` (text, regex-lite) | ours | no | file:line matches, capped |
| `run` (a `.js` file, args, timeout ≤ 10 s) | ours, MicroQuickJS | yes | stdout tail, stderr, exit, error file:line, ms, peak arena |
| `run_python` | Anthropic code execution | yes (it costs) | stdout, files saved to the workspace |
| `make_document` (xlsx / docx / pdf / csv from a spec) | code execution + Files API | yes | the file in the workspace |
| `web_search`, `web_fetch` | Anthropic server tools | no | handled server-side |
| `todo` (set the plan) | ours | no | shown in the UI |
| `ask_user` (2–4 choices) | ours | — | the choice |
| `device` (status, open app, reminder) | ours | open / reminder | the reading or "done" |
| `promote_app` (script → launcher) | ours | yes | the app id |

Permission modes (Tab cycles, shown in the status bar): **Ask** (writes and runs ask), **Auto-edit**
(writes inside the workspace go, runs ask), **Plan** (read-only: the agent proposes, nothing changes).

## 7. Context and cost

- **Message threads** (`message-threads-2026-08-12`): after the first request only the new messages are
  sent. The app keeps the whole conversation in `session-<n>.jsonl` and replays it on `thread_not_found`.
- **Prompt caching**: system + tools + `NUCLEO.md` + `USER.md` behind one breakpoint (≥ 512 tokens caches
  on the 5.x models), so a long session pays the instructions once.
- **Tool results stay small**: `view` paged, `run` output tailed (2 KB), `search` capped (40 matches).
- **Context editing** (`clear_tool_uses`) for long loops; `/compact` writes a summary (Haiku 5.5, low
  effort) into the session and starts a new thread from it.
- **Meters**: context = `usage.input_tokens` (+ cache reads) of the last request against the model's window;
  cost = tokens × the model's price; both in the status bar, per turn and per session.

## 8. RAM plan (Solo boot, Wi-Fi on)

| Phase | Resident | Transient | Peak (target) |
|---|---|---|---|
| Idle (UI, session index, input) | ~12 KB | — | 12 KB |
| Request streaming | ~12 KB | TLS ~32 KB + SSE line 1.5 KB + tool input ≤ 8 KB | ~54 KB |
| Tool: edit / view | ~12 KB | file window 4 KB (streamed from SD, never whole) | 16 KB |
| Tool: run | ~12 KB | arena 24 KB + output ring 2 KB | 38 KB |

Out of the ~79 KB free, the peak leaves ≥ 20 KB of margin. The engine and TLS are never alive together.
Acceptance gate on the ADV and the original: 50 turns, 200 tool calls, no OOM, largest block never below
the TLS floor.

## 9. UI and UX

Cardputer screen (240x135), always:

```
┌ ~/pong · Sonnet 5.5 · medio · ctx 18% · €0.04 · ASK ┐  status: workspace · model · effort · context · cost · mode
│ > fai un pong per due giocatori                      │
│ ☐ campo e racchette  ☑ palla  ☐ punteggio            │  the plan (todo), live
│ ✎ main.js  +42 −0                         [invio: diff]│  tool cards, one line each
│ ▶ run main.js  ok 14 ms                               │
│ ⚠ main.js:31 TypeError: score is undefined            │
│ Correggo: inizializzo score a [0,0]…                   │  the model's text, streamed
└ ❯ _                                  tab: modo  esc: stop┘
```

- Tool cards are one line; Enter opens the card: a diff for edits (− red / + green), the full output for a
  run, the file for a view. Fn+↑/↓ scroll, Esc stops the stream at once.
- Approval sheet for a write: file, +/− counts, the first changed lines; keys **y** yes, **n** no (with a
  reason the model receives), **a** always for this session.
- Slash commands with completion: `/model` `/effort` `/mode` `/cd` `/files` `/open` `/run` `/diff` `/undo`
  `/compact` `/resume` `/cost` `/new` `/memory` `/promote` `/help`.
- Second screen (ADV, 320x240), never a mirror: the file tree on the left, the open file or diff on the
  right, touch to open a file or scroll; the Cardputer keeps the conversation and the input.
- Files without the agent: a file browser and the native editor, so the app is useful offline.
- Every string in the five OS languages; ASCII-folded on the device fonts.

## 10. Security

- The key stays in the firmware (vault / `teacher.json`); the module and scripts never see it.
- Tools write only inside the workspace; nothing under `/system`, `/apps` except `promote_app` (asks).
- Scripts: arena-bounded, time-limited (interrupt), workspace-confined `fs`, no secrets API, `http` through
  the firmware with the same allow-list rules as `net` apps.
- Tool output, files and web pages are data, never instructions (stated in the system prompt; the approval
  sheet is the user's gate).
- No auto mode for `run_python`, `make_document` or `promote_app`: they always ask.

## 11. Verification

- Host first: MicroQuickJS and the `os.*` stubs compiled with MinGW; the agent loop driven by recorded SSE
  fixtures (text, tool_use, input_json_delta, error, thread_not_found); path confinement fuzzed.
- `npm run ui:shots` scenes for every screen, five languages, both screens.
- Device bench (ADV + original): RAM per phase (§8), a 30-minute session, cancel at every phase.
- Before the Store: installed and run on both Cardputers (store rule).

## 12. Phases

1. **Foundation** — ABI 9 `nucleo_llm_*` (tools, threads, cache, usage); MicroQuickJS in a `.nap` with the
   arena and the interrupt; the RAM bench of §8 on the ADV. Exit: the numbers hold, or the fallback (Lua)
   is chosen with the same API.
2. **The agent** — loop, text editor + list + search + run + todo, permission modes, undo, session store,
   the status bar and tool cards. Exit: "write a program that…" is written, run, fixed and saved on the SD.
3. **Documents and apps** — `run_python`, `make_document`, `promote_app`, the script API for screen / keys /
   sound, the second screen. Exit: an `.xlsx` and a launcher app made from a sentence.
4. **Polish and store** — `/compact`, `/resume`, cost ceiling, five languages, both boards, Store release.

## 13. Risks

| Risk | Mitigation |
|---|---|
| MicroQuickJS is young (no version, ES5 subset) | Phase 1 gate; Lua behind the same API; vendor a pinned commit |
| Xtensa code larger than the ~100 KB Thumb-2 figure | measure in phase 1; the module has 256 KB |
| TLS + Wi-Fi fragment the heap over a long session | one heavy thing at a time; the bench of §8; reboot-free recovery path |
| Message threads are beta | the app's own copy + replay is the normal path, threads only shorten it |
| Cost surprises | per-turn ceiling, meter always visible, Haiku for summaries |

Sources: [MicroQuickJS](https://github.com/bellard/mquickjs) · [MicroQuickJS as a sandbox](https://github.com/simonw/research/blob/main/mquickjs-sandbox/README.md) ·
[MimiClaw](https://github.com/DFRobot/mimiclaw) · [ESP-Claw](https://github.com/m5stack/ESP-Claw) ·
[PikaPython](https://www.hackster.io/news/pikascript-crams-python-onto-microcontrollers-with-just-4kb-of-ram-and-32kb-of-storage-f23957b215c7) ·
[MicroPython heap on ESP32](https://github.com/micropython/micropython/pull/7963) · [Tasmota Berry](https://docs.lvgl.io/8/get-started/platforms/tasmota-berry.html) ·
[JS engines on ESP32](https://esp32.com/viewtopic.php?p=8687)

## 14. Status and measurements (2026-11-05)

Built (store-only, host-tested; the device bench `b` in the Code app is still to run on the ADV):
- `nucleo_code/code_rt.c` — the runtime: MicroQuickJS in a caller arena, time limit, workspace-confined `fs`,
  errors with file:line, memory live after GC. Gate: `npm run code:test`.
- `nucleo_code/code_agent.c` — the agent loop: Anthropic text editor (view / create / str_replace / insert),
  `run_js`, `search`, `todo_write`; thinking signatures passed back; prompt cache on system + tools + a moving
  breakpoint on the last message; usage and cost per turn. `anima_claude.h` parses the full stream (text,
  tool_use with `input_json_delta`, thinking signatures, usage, stop reason). Gate: `npm run claude:test`.
- ABI 9 (unreleased): v8 + libgcc double / 64-bit helpers, setjmp/longjmp, abort, realloc, cJSON builders.
- Module `code` 0.2.0: 255,892 B (145 KB code), 6.3 KB static RAM (the stdlib tables went to flash: `nap.ld`
  keeps `.data.rel.ro` with the constants).

### The dialect, measured (MicroQuickJS 6d4d7eb6)
Not available: `let`/`const`, arrow functions, template literals, `class`, destructuring, spread, default
parameters, `Map`/`Set`/`Symbol`/`Promise`, `Object.assign/values/entries/freeze`, `Array find/includes/from`,
`String includes/startsWith/endsWith/padStart`, `Number.isInteger`. Writing past an array's end throws. A catch
variable name must be unique in its function and differ from every `var` there (SyntaxError otherwise).
Available: ES5 core, `for...of` on arrays, method shorthand, `repeat/replaceAll/trim*`, RegExp, JSON, typed
arrays, `Object.keys`. The system prompt states all of it; a compatibility layer for the missing methods is next.

### Agent bench, real API (`npm run code:bench`, checkers the model never sees, run in the device engine)

| Task | Sonnet 5.5 medium | Sonnet 5.5 low |
|---|---|---|
| calc: tokenizer + recursive-descent parser, 19 checks | pass, 2 calls, $0.039 | pass, $0.033 |
| bugfix: ES6 + 3 logic bugs in a seeded file | pass, 3 calls, $0.015 | pass, $0.015 |
| csv: report from a CSV + markdown file | pass, 3 calls, $0.018 | pass, $0.019 |
| sudoku: backtracking with fewest-candidates | pass, 2 calls, $0.018 | pass, $0.015 |
| ledger: cents arithmetic, overdrafts, malformed lines | pass, 3 calls, $0.027 | pass, $0.025 |
| story: 3 chapters + index, plan first | pass, 6 calls, $0.072 | pass, $0.052 |
| lisp: interpreter with closures, recursion, errors (15 checks) | pass, 3 calls, $0.078 | — |
| port: Python script to JS, identical output | pass, 3 calls, $0.029 | — |

Observed: parallel tool calls (create + run in one message); prompt cache reads from the second call; on the
first sudoku run the model found that the given puzzle had no solution, named the typo in the data and did not
force an answer. It still writes markdown in its final answer despite the instruction: the UI renders it as
plain text (strip `**`, backticks, fences) rather than relying on the prompt.

### On the ADV (2026-11-05, Sonnet 5.5, effort medium, Solo boot with Wi-Fi)
The agent runs inside the Code app; the firmware's LLM service (`nucleo_llm_*`) streams each request file with the
user's key. Self-test: `work/main/.code/selftest.txt` holds a task; Code runs it at its next open, writes
`selftest-report.txt` (stats, transcript, plan) and `trace.txt` (stages with free / largest block), and returns to
the OS (at most 40 s waiting for the network, 10 minutes per task).

| Task | Calls | Tools | Time | Cost | Result |
|---|---|---|---|---|---|
| primes.js up to 200, checked by a second method | 3 | 2 | 13 s | $0.012 | 46 primes, sum 4227, run on the device in 18 ms |
| vendite.csv (10 rows) + report.js -> aligned report.txt with totals | 4 | 4 | ~20 s | $0.021 | correct totals, no fix needed |

RAM: 90 KB free at entry, 70 KB while the request streams (the largest block stays 31.7 KB). What it took:
- Code draws straight on the panel: the launcher's 32 KB canvas is the model's TLS (on two screens the firmware
  now draws a direct-draw app's compact frame on the Cardputer's own panel).
- The loader checks each of its blocks against its own size, not their sum (a 25 KB relocation table was refused).
- The LLM ring is 4 KB of heap while a request runs, not 8 KB of static RAM in every boot.
- Every file read (the editor, `run_js`, the scripts' `fs.read`) takes a block of the file's size, not the cap.

### Next
1. Device bench (RAM per phase, §8) with the ADV and the original Cardputer; harder tasks (multi-file, a long
   session that compacts); both Cardputers before the store.
2. Tool input streamed to the SD card instead of the 16 KB RAM buffer; the diff view.
3. Apps from the agent: a script API for screen / keys / sound and the ADV's 320x240 panel, `promote_app` into
   `/apps/scripts/<id>/` (launcher), and **visual testing**: a test run returns a screenshot of what the app drew
   (both screens) plus scripted keys, so the model checks its own UI.
4. ANIMA offers Code from the Store when asked to write or run a program and Code is not installed (and the
   same for other tools: emulators for a ROM, VNC for a remote desktop).

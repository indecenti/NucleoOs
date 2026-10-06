# ANIMA engines — modes, answer order, web mode, local models, agent guards

The short, current map of **who answers an ANIMA turn and why**. Read this before touching the ladder in
`apps/anima/www/index.html` (search `ONE ladder`), `web/shell/ai-engines.js` or `apps/agent/www/runtime.js`.
Older deep dives: [anima.md](anima.md), [anima-code.md](anima-code.md), [anima-local-first-plan.md](anima-local-first-plan.md).

## Two modes, one ladder
| Mode | Meaning |
|---|---|
| **Auto** | the best engine available, Internet allowed |
| **Private** | the same ladder with every Internet step removed (the PC and the Cardputer on the home network remain) |

Defined once in `web/shell/anima-mode.js` (shared with the copilot). The **engines panel** (the "ANIMA · …"
pill) shows both modes, the live state of every engine, the real order, "cloud first / PC first" (only when
both exist; `ai.local.engines.prefer`) and the PC model pick (`ai.local.engines.models[server][task]`).

**Order of a turn** (`index.html` send path):
1. **Commands first** (`commandFirst`): device actions, live state, app launch (`local/cascade.js commandHint`,
   routing vocabulary in it/en/es/fr/de) go to the Cardputer. If it cannot take them:
   - live values the status carries (SD space, battery, time, date, network, RAM, uptime, version) are answered
     **exactly** from `/api/status` (`liveFromStatus`, in the language the user wrote in — `guessLang`); never a model.
     Several at once ("RAM e Wi-Fi?") and with the device as subject ("quanta RAM ha il Cardputer") count too (`liveKinds`);
   - a plain volume / brightness order is set directly with `POST /api/anima/act` (`settingAct`) — no engine needed;
   - actions / launches go to the **local agent** (PC model + real tools); if it cannot, the turn ends
     "not done" — a tool-less chat must never claim an action.
2. Weather (browser, Open-Meteo) · 🌐 Web (Groq compound, Auto only) · translation ladder.
3. Local agent (a task: files, code, app) → local server chat → cloud (with a key) → local server as fallback →
   browser GPU (WebLLM, HTTPS only) → browser WASM brain + web index → the Cardputer.

Cloud errors are recorded only when a cloud rung could run (a key, not Private) — never "can't reach Claude"
for a local failure. Replies follow the language of the user's message (prompt rule in `contextkit.js`).

## The web client carries the load (http:// has no service worker)
NucleoOS is opened at `http://<cardputer-ip>`: not a secure origin, so `navigator.serviceWorker` does not exist and the
SW's device gate (2 in flight, exclusive writes, app cache) never runs there. Page code must be device-friendly itself:
- `web/shell/seq-import.js`: an app's own module graph fetched ONE file at a time and linked in the browser (blob URLs;
  shared modules keep their real URL). ANIMA loads the agent runtime with it — a parallel `import()` of ~12 modules lost
  one on the 4-socket httpd and the browser kept the failure until a reload. `tools/seq-import.test.mjs`.
- ANIMA's SD copy of the conversations (`sessions.json`, the cross-device sync) is written after 3 quiet minutes or when
  ANIMA is hidden / closed (keepalive), ≤56 KB — not ~100 KB after every turn (37 s, heap down to 1.5 KB).
- If the agent still fails to load, ANIMA says so (`agentLoadFail`) instead of a tool-less chat "applying" changes.

## The Cardputer's web profile
A browser opening `/` on the full native OS gets a flash page (`firmware/components/nucleo_webfs/handoff.html`,
the shell's own boot splash copied in by `tools/gen-handoff.mjs`; `npm run handoff:check`) and the device
warm-reboots into **server Solo** ("web" profile): httpd + auth only, the offline brain / voice / IR / recorder /
calendar service OFF so the desktop gets the RAM. `/api/status` → `"profile": "web" | "full"`.
- Navigation is detected by `Accept: text/html` (browsers send `Sec-Fetch-Mode` only to secure origins).
- The reboot waits 1.5 s after queuing the page (`HANDOFF_FLUSH_MS`), or the page arrives cut.
- The page reloads when `/api/status` says `profile: "web"`. The WebSocket trigger stays as a fallback.
- In web mode the device's `/api/anima` answers 503 by design → ANIMA says so (`devWebMode`), the L1
  setting stands down, and step 1 above takes over.

Pairing sessions survive it: `nucleo_auth` evicts the **least recently used** of its 32 slots
(`auth_slots.c`, `npm run authslot:test`). The dev tools (`push-files`, `push-ota`, `sd-net-sync`) reuse ONE cached
session per device (`tools/lib/device-session.mjs`, cache `tools/.device-sessions.json`, gitignored) — pairing on
every run used to evict the user's browser.

An app install rewrites `apps.json`; the firmware reloads it **one app object at a time**
(`nucleo_registry/registry_scan.c`, `npm run regscan:test`) — the whole-document parse ran out of heap in web mode
and `/api/apps` kept the old list until a reboot.

## Local models on the user's PC (`web/shell/ai-engines.js`)
- Reached browser → `localhost` (never through the Cardputer); Ollama needs `OLLAMA_ORIGINS`
  (set to `http://192.168.0.*` on the dev PC; ANIMA's help wizard explains it).
- Chrome's Local Network Access can block the page → localhost. `navigator.permissions` is **not** reliable (it says
  "denied" in Chrome while the fetches work), so a server is `blocked` only when no loopback server answered and the
  query says denied; the panel then names both causes (stopped, or blocked by the browser). The Claude desktop's
  built-in browser pane does block it: test Ollama in the user's Chrome.
- `pickModel` ranks from what is installed + what was **measured here** (`ai.local.perf`): speed is the truth.
  A >10 GB file is a fallback until measured ≥12 tok/s.
- A model bigger than the GPU: on Ollama's "CUDA out of memory" the load is retried once with
  `num_gpu = gpuLayersFor(size, block_count, VRAM)`; the working split is remembered (`numGpu`).
  Measured (8 GB RTX 5070 laptop): qwen3.6 35B-A3B at 12 layers = 22.4 tok/s; qwen3.5 9B = 20.5 tok/s.
  NOT monotonic: 7 layers fail too (CUDA pins host RAM for the CPU part) — the retry climbs `gpuLayerLadder` (est, +3, +6).
- **First measurement** (`benchmarkCandidates`, ANIMA idle 25 s, once per model per browser): a big model is a fallback
  until measured fast, and was never measured because never picked. Every unmeasured big model is assumed fast at once
  and only the winner is measured. Dev PC, 2026-10-01: 35B-A3B 40.8 tok/s (10 layers), coder:16k 47 — now the agent /
  chat and code picks; it fixed first time the Contatore task the 9B had botched.

## Agent guards (`apps/agent/www`) — from OpenCode (MIT)
- `edit-replace.js` (in `apps/anima/www`, used by `fsclient.edit`): tolerant old→new replacer chain; one
  unique match or an actionable error.
- `tool-guard.js`: tool-name repair + doom-loop stop (3rd identical call in a row not run). Every loop uses it.
- `fsclient` creates the workspace root itself (`/data/agent` did not exist on a fresh card).
- Approvals show the content / old→new / code, not only the path (scaffold_app: name + folder).
- `sh` (`agent-sh.js`): POSIX-like shell over the workspace (`cat -n`, pipes, redirection; `apps` lists id, name,
  category and description from `web/shell/app-catalog.json`, generated by `tools/gen-app-catalog.mjs`, gated).
- `run_js` returns what the code **printed** (stdout, capped) — before, it reached only the UI and the model guessed
  its own numbers; `save_to` writes the printed output to a file so results are never retyped.
- Truth over the model's last word: `guardPlan` forces file/code requests to a task (the cloud triage answered
  them with scripts to run); a turn whose last `publish_app` failed — or whose post-publish smoke failed — ends
  with "NOT installed", whatever the model wrote; `scaffold_app` never overwrites an app already in the workspace.
- Page scripts and files are linted with `checkSyntax(code, { bare: true })` (no sandbox parameters): the sandbox
  wrapper's `os` made every agent app — the starter included — fail its own lint.
- **The plan rides on every tool result** (`planReminder`, appended in `guardedExec` — every loop passes there):
  one line `[plan 1/3 done · now: … · next: …]` while work is open, nothing once it is all done; with nothing
  in progress it asks the model to mark the next step. Small models otherwise forget the checklist and stop halfway.
- **Read paging** (`withLineNumbers`): the read budget (`READ_CAP` 24 000 chars, `LOCAL_READ_CAP` 9 000 for a PC
  model) bounds the line WINDOW, not the file — `read_file` decodes up to 2 MB and pages to any line; the tail says
  `lines A-B of N shown; M more lines — call read_file with offset=K to continue` (the total matters: told only
  "M more lines", qwen3.5:9b crept to the end 5 lines a read); one line over 2 000 chars (minified code) is cut.
  Live, qwen3.5:9b + the real runtime: "code in the LAST line of a 4 000-line file" → 2–3 reads, 3/3 correct.
  `sh` likewise reads files up to 2 MB (`cat`/`sed -n`/`tail`; its reply is capped at 12 KB) while `grep -r`
  skips files over 256 KB, so one recursive search never drains megabytes off the Cardputer.
- **Out of steps → a summary**, in every loop (`budgetSummary`): one last call without tools — `tool_choice:
  "none"` for Anthropic and OpenAI-compatible providers (the history's tool calls require the tools to stay
  declared; Anthropic's request rides in the last user turn so roles alternate), `noTools` for the PC model —
  then "(step budget exhausted — the task may be incomplete)". A provider that refuses costs only the summary.

Tests: `tools/anima-live-status.test.mjs`, `tools/fsclient-root.test.mjs`, `tools/edit-replace.test.mjs`,
`tools/agent-tool-guard.test.mjs`, `tools/shell-ai-engines.test.mjs`, `tools/anima-host/contextkit-check.mjs`,
`tools/agent-sh.test.mjs`, `tools/ai-models.test.mjs` (rate limit vs quota), `tools/device-session.test.mjs`,
`tools/anima-host/app-publish-check.mjs` (real lint), `tools/anima-host/app-ops-check.mjs`,
`tools/anima-host/agent-runtime.test.mjs` (the REAL `runtime.js` on the host — `tools/lib/web-paths-loader.mjs`
maps the device URLs — driven by a scripted model: plan reminder, read paging, budget summary incl. Anthropic).

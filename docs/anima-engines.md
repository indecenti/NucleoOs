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
     **exactly** from `/api/status` (`liveFromStatus`, in the language the user wrote in — `guessLang`); never a model;
   - actions / launches go to the **local agent** (PC model + real tools); if it cannot, the turn ends
     "not done" — a tool-less chat must never claim an action.
2. Weather (browser, Open-Meteo) · 🌐 Web (Groq compound, Auto only) · translation ladder.
3. Local agent (a task: files, code, app) → local server chat → cloud (with a key) → local server as fallback →
   browser GPU (WebLLM, HTTPS only) → browser WASM brain + web index → the Cardputer.

Cloud errors are recorded only when a cloud rung could run (a key, not Private) — never "can't reach Claude"
for a local failure. Replies follow the language of the user's message (prompt rule in `contextkit.js`).

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
(`auth_slots.c`, `npm run authslot:test`).

## Local models on the user's PC (`web/shell/ai-engines.js`)
- Reached browser → `localhost` (never through the Cardputer); Ollama needs `OLLAMA_ORIGINS`
  (set to `http://192.168.0.*` on the dev PC; ANIMA's help wizard explains it).
- `pickModel` ranks from what is installed + what was **measured here** (`ai.local.perf`): speed is the truth.
  A >10 GB file is a fallback until measured ≥12 tok/s.
- A model bigger than the GPU: on Ollama's "CUDA out of memory" the load is retried once with
  `num_gpu = gpuLayersFor(size, block_count, VRAM)`; the working split is remembered (`numGpu`).
  Measured (8 GB RTX 5070 laptop): qwen3.6 35B-A3B at 12 layers = 22.4 tok/s; qwen3.5 9B = 20.5 tok/s.

## Agent guards (`apps/agent/www`) — from OpenCode (MIT)
- `edit-replace.js` (in `apps/anima/www`, used by `fsclient.edit`): tolerant old→new replacer chain; one
  unique match or an actionable error.
- `tool-guard.js`: tool-name repair + doom-loop stop (3rd identical call in a row not run). Every loop uses it.
- `fsclient` creates the workspace root itself (`/data/agent` did not exist on a fresh card).
- Approvals show the content / old→new / code, not only the path.

Tests: `tools/anima-live-status.test.mjs`, `tools/fsclient-root.test.mjs`, `tools/edit-replace.test.mjs`,
`tools/agent-tool-guard.test.mjs`, `tools/shell-ai-engines.test.mjs`, `tools/anima-host/contextkit-check.mjs`.

# ANIMA — local-first agent plan (2026-09)

Status: **proposed**, awaiting approval. Built from a code audit of every inference path, every AI
configuration surface, live measurements on the dev PC (Chrome 154, RTX 5070 8 GB, Ollama 0.34.4) and
current docs. Guiding rules: the Cardputer does as little as possible; everything heavy runs in the
browser or on the user's own PC; every UI string ships in it/en/es/fr/de; every step is tested.

## 1. Diagnosis (measured / verified)

1. **The device origin disables the whole modern stack.** Probed in Chrome on this PC:

   | Capability | `http://localhost` (secure) | `http://<device-ip>` (not secure) |
   |---|---|---|
   | WebGPU (`shader-f16`, 2 GB buffers) | yes | **no** |
   | Chrome built-in AI (Prompt API, Summarizer, Translator) | yes (`downloadable`) | **no** |
   | Cache API, Service Worker, OPFS, `crypto.subtle` | yes | **no** |
   | Ollama `localhost:11434` | 200 | **403 (CORS)** |

   The installers call the Cache API directly; on http the resulting ReferenceError is classed as a
   network drop and **retried forever inside the OS-wide blocking modal** (CPU-model path).
2. **Fragmentation.** ~9 cloud clients (different defaults/base URLs), 6+ local model registries, 3 model
   installers inside ANIMA, weights cached twice, 4 cascades with different orders (ANIMA, copilot, agent,
   spreadsheet), the Groq key in 3 places (one app overwrites another's), presets writing legacy mode
   values ("Auto" can land in Private), the GPU preset unreachable (`localModelReady:false` hard-coded).
3. **No local servers.** Nothing supports Ollama / LM Studio / llama.cpp / Jan / LocalAI / NucleoMind —
   yet this PC runs Ollama with 7 models, and `qwen3.5:9b` did an Italian **tool call** correctly at
   ~42 tok/s. Chrome's Local Network Access does not currently gate private-IP → loopback, so plain
   CORS (`OLLAMA_ORIGINS`) is the only barrier; from a localhost origin it works out of the box.
4. **Local models are not agents.** The agent loop needs a cloud key; local rungs run only after every
   cloud hop failed; the Agent app refuses offline sends; local prompts get no profile facts, no files,
   **no current date** (Ollama booked "tomorrow" in 2024).
5. **The device does browser work.** `mode=on` defaults make the firmware open its own cloud TLS;
   spreadsheet `=ANIMA()` asks the device per cell; `teachDevice` repeats cloud questions to the device;
   the copilot's truth lamp adds up to 3 `/api/anima/verify` per reply; Gemini is relayed through
   `/api/llm`; `web-llm.js` (6.1 MB) and `vosk.js` (5.8 MB) are read from the SD before the CDN.
6. **The WebGPU wizard** detects the cause correctly (insecure context) and its flag steps work, but:
   it doesn't say the same flag also unlocks cache/SW/crypto; the flag breaks if the IP changes; no
   localhost/app alternative; four other surfaces give contradictory reasons; no `shader-f16` check and
   no q4f32 fallback; "VRAM" is really `maxBufferSize`; the "1.5B is on the SD, installs offline" claim is
   false (not staged; `model_lib` always from GitHub; `webllmAppConfig` never used).

## 2. Target architecture

**A. Secure context, three ways (one wizard, auto-detected):**
- **NucleoOS Link** (new, recommended): a small local companion on `http://localhost` that serves the
  shell/app assets from a local copy (the device stops serving static files → big load drop), reverse-
  proxies `/api` + `/ws` to the Cardputer, and exposes same-origin bridges to local AI servers. Grows out
  of `tools/dev-device-proxy.mjs`; also embeddable in NucleoConnect (Windows) and the Android app.
- **NucleoConnect (Windows)**: WebView2 started with the device origin treated as secure.
- **Chrome/Edge flag**: exact steps with copy buttons, live verification, IP-change warning.

**B. One AI engine layer in the shell (`ai-engines.js`)** — the only transport and router:
- Engine kinds: cloud (Claude/Groq/Gemini/xAI), **local server** (Ollama native `/api/chat` with
  `tools`/`think`/`format`; generic OpenAI-compatible for LM Studio, llama.cpp, Jan, LocalAI, NucleoMind),
  **in-browser GPU** (WebLLM), **in-browser CPU** (wllama), **Chrome built-in** (Translator /
  LanguageDetector for Italian; Prompt API only for en/es/fr/de), **device** (M1 cascade, actions).
- Capability probe per engine: tools (native / schema-emulated / none), JSON schema, vision, embeddings,
  context length, languages, measured tok/s. Parallel probes, ~800 ms timeouts, never via the device.
- Per-task routing: chat, agent+tools, code, vision, speech-to-text, TTS, embeddings, translate, image.
- "Answered by" badge on every AI surface + a log of the last calls (engine, model, latency, error kind).
- Every app calls `resolve(task)`; the duplicate clients, resolvers and model tables are deleted.

**C. In-browser models done right:** one catalog built from WebLLM's prebuilt config
(`vram_required_MB`, required features), one installer (quota check, `storage.persist()`, cancel,
download lock, resume), `shader-f16` detection with q4f32 fallback, recommendation from the adapter.
Current generation: Qwen3 0.6B / 1.7B / 4B (WebLLM); EmbeddingGemma-300m for retrieval.

**D. ANIMA as an offline agent:** one agent runtime for ANIMA, copilot and the Agent app, runnable on
local engines (Ollama native tool calling; WebLLM via JSON-schema-constrained output); OS tools (files,
calendar, open app, **create/open HTML pages**, weather, search the SD, run JS); context injected every
turn (date/time/timezone, OS language, profile facts, open app/file); retrieval over the user's SD
documents with embeddings stored in the browser (IndexedDB), never on the SD; confirmation gate for
destructive tools, step budget, cancel.

**E. Settings ▸ "AI & Models"** — the single place to configure everything: Overview (task → engine →
status, "test all"), Engines (+ Add engine wizard: cloud key / local server / in-browser model / device
only), Routing, Privacy (Auto / Local only / Private), On-device (L1, TTS, voice), Browser models
(install/remove, storage), Transparency. ANIMA's panels become deep links; onboarding reuses the wizards.

**F. Device load:** browser-first classification (WASM brain), `mode=off` by default (device only for
actions), no duplicate device calls, CDN-first heavy libraries with SD fallback.

## 3. Phases (each ends green on host + browser E2E, 5 languages)

1. **Foundations** — secure-capabilities probe shared by every surface; installer fatal errors instead of
   endless retry; correct, complete wizard (flag / Link / app); `shader-f16` + fallback; one model
   catalog; context injection (date, language). *Test:* WebGPU E2E on localhost with the real GPU,
   wizard E2E on the insecure origin.
2. **Local servers + engine layer** — `ai-engines.js`, Ollama + OpenAI-compatible adapters,
   auto-detection, `OLLAMA_ORIGINS` wizard per OS, model picker from `/api/tags` + `/api/show`.
   *Test:* E2E against the real Ollama (skipped when not running) + a mock server in CI.
   *Status (2026-09-30):* engine layer done and in ANIMA's ladder (local-first without a key / in Private,
   cloud fallback otherwise); unit-tested with mocks (`tools/shell-ai-engines.test.mjs`) and E2E on the real
   Ollama (`tools/web-e2e/ollama.e2e.mjs`). Copilot on the same layer; model choice learns tokens/s + VRAM
   share on this computer; the per-OS `OLLAMA_ORIGINS` wizard (`/local-ai-help.js`) is live in ANIMA. Still
   open: the Agent app on the layer, the Settings UI for `prefer`/models + the wizard there (phase 3).
3. **Settings ▸ AI & Models + migration** — the center, wizards, apps moved to `resolve()`, duplicate
   clients/keys removed, presets fixed.
4. **Offline agent = a portable "open code"** — unified runtime + tools + retrieval + HTML Viewer app with
   "Create with ANIMA". ANIMA works like a small OpenCode/Claude Code: a workspace (an SD folder mirrored in
   the browser), tools (read, grep, edit with diff + approval, run JS in a sandboxed worker, preview HTML),
   plan → act → verify loop. It ADAPTS to what is available — Ollama coder model (e.g. qwen3.6-coder) →
   cloud → WebGPU (Qwen3 / Qwen2.5-Coder) → CPU — and sizes context/steps to the engine. The Cardputer is
   only file storage: reads are cached in the browser, writes are batched, and no inference, indexing or
   long task ever runs on the device.
   *Test:* scripted agent tasks in 5 languages on Ollama and on a WebLLM model; device request budget.
   *Status (2026-09-30):* agent runtime on local engines done — Ollama/LM Studio native tool calling and the
   WebGPU model (free decoding + strict re-validation; WebLLM grammars are broken for Qwen3) in ANIMA and Agenti,
   E2E green on the real Ollama (5 languages) and the real GPU. See `docs/anima-code.md` F6. Still open: the HTML
   Viewer app, embeddings retrieval over SD documents, a sandboxed "run JS" with the workspace files.
   **ANIMA stays ANIMA:** facts the OS knows (date, time, weekday, battery, Wi-Fi, storage, open apps, files)
   are answered DETERMINISTICALLY in all five languages before any model runs. Measured 2026-09-29:
   Qwen3-1.7B on WebGPU, given the real date in its prompt, answered "Montag" on a Tuesday. Models reason
   and write; they are never the source of truth for what the OS can simply read.
5. **NucleoOS Link** — the localhost companion (static cache + API/WS proxy + AI bridge) and the
   NucleoConnect switch; measure the device-load drop with the simulator's socket model.
6. **Device-load cleanup + docs** — the items in §1.5, then `docs/anima*.md` updated.

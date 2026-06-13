# ANIMA — Context engine & Grok/Claude multi-agent

How the ANIMA online chat builds, budgets and protects the context it sends to a cloud or browser
LLM, and how the multi-agent runtime works on **both** Claude and Grok/Groq.

## The problem it fixes

The online chat "didn't contextualise": the browser-direct cloud call sent a **single** user message
with no history, the system prompt capped replies at ~240 chars (sabotaging code/stories/games), and the
good compaction in `apps/anima/www/context.js` was wired only to localStorage — never to the request.
Groq users additionally got a **toolless** chat (no multi-agent at all).

## Architecture

Two memories, like a modern agent — **episodic** (compacted transcript, `context.js`) + **working**
(a typed ledger) — assembled into a budgeted, injection-safe `messages[]` for any backend.

| File | Role |
|---|---|
| `apps/anima/www/contextkit.js` | The context engine. Pure, DOM-free, host-tested. `estimateTokens`, `MODEL_PROFILES`/`profileFor`, `buildLedger` (goal/entities/files/lastCodeLang/prefs/pending), `buildSystem`, `buildMessages`/`assemble`, `usageTokens`. Reuses `context.js`. |
| `apps/anima/www/index.html` | Wires it into `cloudComplete`(messages[])/`cloudAsk`/`queryLocal`; token-aware meter; **Context Inspector** (click `#ctx-meter`); Settings ▸ "Contesto & Agenti". |
| `apps/anima/www/local-llm.js` | `queryLocal(q,lang,history)` — WebLLM browser model with the small profile. |
| `apps/agent/www/agent-tools.js` | **Node+browser-safe contract layer**: `CLIENT_TOOLS`, `toOpenAITools`, `callOpenAIChat` (JSON mode), `runOpenAIToolLoop` (tool_call↔tool_result threading), `GROQ_MODELS` tiers, `guardPlan`. Host-testable. |
| `apps/agent/www/runtime.js` | The multi-agent runtime (orchestrator→worker). Now provider-neutral: Claude (Haiku→Sonnet/Opus) **or** Groq (8B→70B). |

### Per-mode token budgets (`MODEL_PROFILES`/`groqProfile`)

| Kind | inTokens | reply / code | notes |
|---|---|---|---|
| cloud (Claude) | 24000 | 1024 / 4096 | full |
| groq 70B-versatile | 12000 | 1536 / 4096 | `firm` framing |
| groq 8B-instant | 4000 | 900 / 2048 | tight window, lower temp, `firm` |
| webllm (browser) | 1800 | 512 / 1024 | reduced models — kept lean on purpose |
| offline (device) | 1200 | 256 / 512 | device cascade answers |

The meter and the Inspector show the **active** mode's budget; no key in an online mode ⇒ resolves to offline.

## Grok/Groq specifics

Small open models (Llama-8B) drift, restate the prompt and bolt a preamble before code. The engine adds
a **`firm` "DISCIPLINA OPERATIVA"** block (answer only the last message, no preamble before code, stay on
the conversation), model-aware budgets, and lower temperature. The runtime adds:

- **Real tool-use** via OpenAI function-calling (`toOpenAITools` + `runOpenAIToolLoop`) — same OS tools as
  Claude (file/run_js/open_in_os/device_status/weather/list_apps). No server-side `web_search` on Groq.
- **JSON-mode orchestrator** (`response_format: json_object`) for a reliable typed plan even on 8B.
- **Model tiers**: 8B orchestrator (cheap triage), 70B worker (capable tool-use/code).
- **`guardPlan`** — a *deterministic* guard: a device/tool request can never slip through as a fabricated
  "answer" (e.g. an invented time); it's forced back to a tool `task` by pattern, regardless of the model.

The ANIMA agent gate opens to Groq too (`keyCfg.key && needsAgentRuntime(...)`, no longer Claude-only).

## Anti-injection

Instructions live **only** in the system message. User/conversation/retrieved text is data, never
commands. Any inlined data uses `wrapData()` → `<<<data … data>>>` (mirrors the firmware guard).

## Cross-provider collaboration (designed, not shipped)

When both a Claude and a Groq key are present they could split independent parallel sub-tasks for speed.
**Deferred**: it can't be verified with a single key, and shipping an unverified cross-provider path would
violate the "only certain things" rule. The runtime is structured to accept an `altCfg` when a 2nd key exists.

## Firmware (one remaining flash)

`firmware/components/nucleo_anima/nucleo_anima_online.c` was aligned with the web (chat prompt: 240-cap
removed → length policy, NucleoOS grounding, injection-guard; long prose served via the overflow channel).
This **only** affects the on-device chat path (`exec='device'`). The default online path is **browser-direct**
(already deployed), so this flash is secondary. To apply it:

```
powershell -File tools\flash.ps1        # builds + serial-flashes (COM3); runs anima:gate first
```

Then re-insert the SD into the Cardputer.

## Tests

```
npm run anima:ctxkit          # context engine (48, deterministic)
npm run anima:agentcontract   # tool schema + guard + tool-loop (21, deterministic, no API)
npm run anima:grok            # LIVE Groq: 10 capability + 10 context + 6 contract (uses the device key)
npm run anima:gate            # full 49-gate release pre-flight
```

`anima:grok` reads the Groq key from `GROQ_API_KEY` or `deploy/sd/data/anima/teacher.json`. Last live run:
24/26 (the 2 misses are an 8B language-switch limitation and a test-prompt artifact since fixed by `guardPlan`).

## Deploy status

**Deployed to SD (H:)** on 2026-06-10 via `tools\deploy.ps1` (stage→`deploy/sd` + gzip) then
`tools\sd-sync.ps1 -Target H:\` (additive, device state preserved). The web layer is live; the firmware
flash above is the only optional remaining step.

To redeploy after changes:
```
powershell -File tools\deploy.ps1            # stage repo → deploy/sd + regenerate .gz
powershell -File tools\sd-sync.ps1 -Target H:\   # SAFE additive copy to the SD (protects teacher.json/learned)
```
Do NOT use `deploy.ps1 -To H:\` for the SD: its mirror-delete can wipe device state (the Groq key, learned cards).

## Gotcha — cross-app imports

The webfs maps `/apps/<id>/<rest>` → `apps/<id>/www/<rest>`, so a cross-app **absolute** import must NOT
contain `/www/` (else a 404'ing double `www/www`). Use `/apps/anima/fsclient.js`, not
`/apps/anima/www/fsclient.js`. Same-app imports use `./relative.js`.

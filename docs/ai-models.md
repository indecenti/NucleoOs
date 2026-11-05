# AI models — chosen live, never pinned

Every NucleoOS surface that talks to a cloud LLM (ANIMA, AI Chat, Agents, Dictation, Recorder, Games,
Paint) goes through one rule in `web/shell/ai.js`: **the model is chosen at call time from what the
key can actually use**, and every failure comes back as one plain sentence that says how to fix it.

Why: providers retire model ids on their own schedule. Groq shut down `groq/compound` on 2026-09-21
(ANIMA's 🌐 web mode died with a bare 404) and `llama-3.1-8b-instant` is on its list; a hardcoded
id eventually breaks every feature built on it.

## How a model is chosen

| Step | What happens |
|---|---|
| List | `listModels(cfg)` reads the key's own `/models` (Anthropic `/v1/models`, OpenAI-compatible `/models`, Gemini through the device proxy), cached 12 h per key in `localStorage` (keyed by a hash of the key, never the key). |
| Prefer | A saved model (Settings, AI Chat picker, `teacher.json`) is only a **preference**: used while the provider still serves it. `'auto'` or empty = no preference. |
| Rank | `rankModels(ids, tier)` drops non-chat ids (speech, embeddings, guards, images, agentic systems) and orders the rest for the tier: `max` (strongest), `mid` (0.7 quality / 0.3 speed), `fast` (speed, with a quality floor). Newer versions win ties. |
| Retry | `withAutoModel(cfg, fn)` runs the call; a "model not found / decommissioned" answer (404, `model_not_found`, `model_decommissioned`…) drops the cached list, re-picks and retries — up to three different models. Other failures are never retried here (a bad key stays a bad key). |
| No list | Offline / provider without `/models`: the saved or static `TIERS` id is tried, exactly as before. |

Special needs: `resolveModel(cfg, { need: 'web' })` picks a model with Groq's built-in `browser_search`
tool (the gpt-oss family — the successor of `groq/compound`); `need: 'image'` picks the newest image
model (Paint's Atelier). Code with its own model tables (the agent runtime) uses
`servedModel(cfg, model)`: the same model if still served, else the best of the same kind.

## Effort, and Claude on the Cardputer itself (2026-11-04)

`teacher.json` carries `"effort"` (top level = the active teacher, optional `keys.<provider>.effort`):
`low | medium | high | xhigh | max`, absent = `medium`. The web OS sets it in the ANIMA model picker and
Settings ▸ AI; the native ANIMA sets it in its IA tab (rows **Modello** and **Impegno**: Enter steps to the next
choice and saves the file, keeping every field it does not know). On Anthropic it is sent as
`output_config.effort`, clamped to what the model accepts: Fable/Mythos and every 5.x take all five (and
think, always); Opus 4.7/4.8 all five; Opus/Sonnet 4.6 no `xhigh`; Opus 4.5 only `low..high`; Haiku 4.5,
Sonnet 4.5 and older get no field. No request sends `thinking` (`disabled` is a 400 on the 5.x models).

On the device (`nucleo_anima_online.c` + `anima_claude.h`) the Claude call is a **stream**: thinking makes a
turn last 5 s (low) to minutes (max), which one blocking POST under the 8 s Task-WDT could not survive.
Each read waits at most `ANTH_READ_MS` (5 s) and pets the WDT; the whole turn has an effort-scaled clock
(20 / 30 / 60 / 90 / 120 s); `max_tokens` = the caller's answer budget + a thinking allowance (1k / 3k / 8k /
16k / 32k) while the answer itself is capped client-side; only `text_delta` events are kept (the RAM cost
is a 1.5 KB line buffer); a request that reached the server is never retried (each retry was billed).
The cross-verifier veto always runs `low`. Host gate: `npm run claude:test` (also in `anima:gate`).

## Errors the user can act on

`AiError.kind` → `explainAiError(err, lang, { settings })` (it/en/es/fr/de). `settings` names where
*that* surface keeps its key (`'⚙'` for AI Chat; default: the OS Settings ▸ AI panel).

| kind | Typical cause | The sentence tells the user to… |
|---|---|---|
| `auth` | 401, invalid key | paste a new key |
| `forbidden` | 403, region/permissions | check the account or switch provider |
| `model` | retired model and **no** replacement served | check the key / switch provider |
| `quota` | credit or quota used up | top up / enable billing / switch provider |
| `rate` | 429 | retry shortly (with Retry-After when given) |
| `too_long` | context too long | start a new conversation / shorten |
| `provider_down` | 5xx / overloaded | retry in a few minutes |
| `device_busy` | the Cardputer's `/api/llm` relay is busy | retry in a few seconds |
| `network` | no Internet / blocked | check the connection or an extension |
| `timeout` | no answer in time | retry / pick a faster model |

ANIMA shows an actionable kind (`auth`, `forbidden`, `model`, `quota`) as a warning under an answer
another tier gave, and as the reply itself when nothing answered — so a broken key gets fixed instead
of silently making every answer worse.

## Tests

`nucleo-webos tests/suite/ai-models.test.mjs` (in `npm run test:all` / the gate's unit tests): ranking on Groq's real
list, preference kept while served, a retired model replaced (404 and 400 "decommissioned"), a bad
key not retried and explained, error classification, the tier carried by `routeFor`.

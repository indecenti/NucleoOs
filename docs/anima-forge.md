# ANIMA Forge — design

> One assistant (ANIMA), four cooperating intelligence substrates, all usable from the browser,
> culminating in an offline agentic code editor **served by a PSRAM-less microcontroller** where a
> client-side WebLLM coder writes and runs code that the on-device zero-hallucination reasoner
> verifies before anything is applied.
>
> **Status: foundation built & host-verified; UI/firmware/GPU layers pending.** The adversarial
> review (verdict *plan-needs-revision*) surfaced real must-fixes (see §13); this doc folds them in,
> and the deterministic spine below already implements the host-verifiable ones.

---

## 0. Build status (what's implemented & verified)

The pure, deterministic spine is built under `apps/anima/www/forge/` and gated on the host
(`npm run forge:check`, also auto-included in `anima:gate` via the `tools/**/*.test.mjs` glob —
**44/44 green**, full suite 255/255, no regression):

| Module | What it is | Must-fix addressed | Gate |
|---|---|---|---|
| `forge/envelope.js` | ANIMA Envelope v1 + `normalize`/`validate`/`provenance` | one shape for 4 substrates; M4 never grounded-badged | `forge-envelope.test.mjs` |
| `forge/actions.js` | closed typed-action set + `parseActions` **schema firewall** + path confinement | #1 (firewall) | `forge-actions.test.mjs` (incl. fuzz) |
| `forge/router.js` | Stage-A deterministic pre-router + `isCodeRequest` + **VRAM-gated `modelPlan`** | #5 (single-model on ≤4 GB) | `forge-router.test.mjs` |
| `forge/extract.js` | claim **extraction + coverage** (uncovered ⇒ surfaced) | #1 (extraction-first, coverage-gated) | `forge-extract.test.mjs` (planted claims) |
| `forge/verify.js` | verdict combiner (veto/warn/pass), honest WARN-on-uncovered | #6 (icon+text chips) | `forge-verify.test.mjs` |
| `forge/download.js` | manifest verify + **bounded-Range** planner + adaptive window + FSM | #3 (never whole-file GET; SD back-off) | `forge-download.test.mjs` |
| `forge/engine.js` | `Engine` boundary + `MockEngine` + `assertMock` | CI w/o GPU | (used by loop gate) |
| `forge/loop.js` | agentic state machine, engine-injected | #4 (no write w/o approve, veto never applied, budget) | `forge-loop.test.mjs` |
| `code-runner/www/nucleo-run.js` | added `checkSyntax` + `mode:'check'` (parse-only) + output cap | #4 (sandbox hardening) | `forge-sandbox.test.mjs` |

**Avant-garde layer** (innovative, useful, all pure + host-gated — `npm run forge:check` now **70/70**, suite 281/281):

| Module | Innovation | Why it's useful |
|---|---|---|
| `forge/grammar.js` | compiles `ACTION_SCHEMA` → **GBNF** for XGrammar/WebLLM constrained decoding | one source of truth drives both the decode-time grammar AND the runtime firewall → a 0.5B orchestrator is constrained at generation *and* re-validated at parse (defense in depth) |
| `forge/capguard.js` | **least-privilege capability inference** + static danger scan (eval/`Function`, mutation-in-loop, network-exfil, dynamic-import, obfuscation) | model-generated code is the highest-risk surface; grant the minimum caps and **veto** dangerous patterns *before* the sandbox; folds into the verdict |
| `forge/selfcheck.js` | **grounded best-of-N** (test-time compute): rank candidates by executed signal — passes check, passes own assertions, fewest caps, smallest | quality without trusting model self-report; picks the candidate the sandbox proves runs |
| `forge/scheduler.js` | **MCU-aware coexistence controller**: device telemetry → go/throttle/pause | turns must-fix #3 into a principled scheduler — never starves the verifier, backs off before the device 503s, CDN bypasses the device |
| `forge/provenance.js` | **tamper-evident provenance ledger** (hash-chain + off-chain anchor) for every applied artifact | signed, model+revision-pinned, immutable audit trail of agentic edits; reseal defeated by the anchor — extends the project's Verifiable-Knowledge-Ledger ethos to code |

The loop now does best-of-N synthesis, folds the capguard severity into the verdict (block→veto), and appends a provenance record on apply — all backward-compatible (defaults reproduce the prior single-candidate behaviour).

**Silent learning flywheel + integration layer** (the headline of this iteration — "evolve silently, only certain & useful things"; all pure + host-gated — `npm run forge:check` now **125/125**, suite 336/336):

| Module | What it is | Why it's the innovation |
|---|---|---|
| `forge/learn.js` | **self-distillation pipeline**: `distill(turn, ctx)` turns a completed Forge interaction into a STAGED `code-recipe` card — but only when CERTAIN (verdict `pass` + approved + ran + provenance) and USEFUL (not a near-dup, not a cross-topic collision, real content) | M4's *verified* generative output silently, **auditably** teaches M1's grounded brain offline; every staged card links its provenance hash → reversible (drop the line). warn/veto/rejected/unrun **never** become knowledge — zero-hallucination by construction |
| `forge/client.js` | the single `makeClient({deviceTransport, localProvider, caps}).ask()` call-site; resolves substrate (incl. `auto` via the router) and normalises every reply to Envelope v1 | M4 can never borrow a grounded device label (hard-stamped `M4-local`); one place the UI calls |
| `forge/transcript.js` | pure UI view-model: `turnModel(env)` (substrate+verdict chips, trace) and `dialModel(mode, caps)` (off/on/only/local/auto; `local` greys out w/o WebGPU) | the UX behaviour is **certain & tested** before the DOM glue lands; icon+text, never colour-only |
| `forge/webllm-engine.js` | the real M4 engine adapter: `probeWebGPU`, `chooseModels` (VRAM→model tier), `orchestratorMessages`(GBNF-constrained)/`coderMessages`, `makeWebLLMEngine({createEngine})` with the WebLLM import **dynamic + injectable** | pure shapes host-tested; the GPU run is the only unverifiable part and is isolated behind injection |
| `forge/download-manager.js` | `makeDownloadManager(...)` ties the FSM + scheduler into a runnable controller with injected I/O: bounded-Range fetches, SHA verify, 503 back-off, idempotent resume, MCU-aware coexistence | offline-first delivery, fully host-tested with a mock transport |

The loop now also runs the **learning hook** on a certain applied turn (stages a recipe + a `LEARN`/`LEARN-SKIP` trace step). The web `addMeta` renders the **substrate + verdict chips** (additive; appears only on M4/verified turns → zero change to device replies).

**The learning loop is closed on the host side** by `tools/anima/promote-learned.mjs` (`npm run anima:promote`, gate `forge-promote.test.mjs` 7/7): it reads Forge's staged `learned-forge.jsonl`, applies a CONSERVATIVE second gate against the WHOLE shipped corpus — schema, **provenance required**, code **re-parse** (`checkSyntax`), **duplicate** (frame-stripped residual-topic overlap ≥0.8 same-category), **collision** (≥0.5 overlap with a DIFFERENT-category card → would hijack a knowledge query) — and writes the survivors to `knowledge/learned.jsonl` (idempotent by id). `build_akb2.py` auto-globs it and `regress.py` (in-dist 100% / 0 OOD-FP, already in `anima:gate`) is the final arbiter. End-to-end verified: `learn.js` stages 2 certain recipes + silently skips a `warn` one; the gate promotes both against the real 2013-card corpus, 0 rejected. *Silent, certain, useful, auditable, reversible (drop the line) self-improvement.*

**The device-side grounded verifier is built and HOST-VERIFIED** (the "M1 judges M4" half of cross-substrate verification). `nucleo_anima_verify_claim(kind, key, asserted, lang, …)` lives in the cascade (`nucleo_anima.c`) and is driven through the REAL `anima.exe` by gate `forge-verify-claim.test.mjs` (4/4): a numeric claim is re-derived on the exact math engine (`2+2=5` → **contradicted**), a fact is checked by KGE/L1 (`capitale della francia = Lione` → **contradicted**, `= Parigi` → **confirmed**, IT *and* EN via the live KGE deduction `francia → Parigi`), an unknown domain (Floonkistan) → **unknown** (abstain → caller WARNs). The thin httpd wrapper `GET /api/anima/verify` (`nucleo_httpd.c`) and the `nucleo_webfs` ranged-read breaker fix (must-fix #3 firmware half) are WRITTEN but ESP-IDF-only → confirm on the next flash to the .166. The cascade smoke (`agent-check` 44/44) confirms the additive C function did not regress the cascade.

**Natural-language verification at scale** (`forge-verify-nl.test.mjs`, **160 IT + 150 EN = 310 real NL claims**, all green against the live exe): capitals via KGE (`qual è la capitale di X` / `what is the capital of X`), birth years (`in che anno è nato X`), arithmetic (`+ − × ÷ ^`), and fictional entities (Atlantis/Wakanda/Gondor → must abstain). Expectations are ground-truth, anchored to the corpus's real coverage by `forge-verify-gen.mjs` (`npm run forge:verify-gen` regenerates `fixtures/forge-verify-cases.jsonl`); the mix exercises all three verdict classes (confirmed/contradicted/unknown) so a verifier regression flips cases and fails.

**The loop now behaves like Claude Code** (evolved, opt-in, backward-compatible): besides plan→synth→verify→approve→apply→run, it (1) **writes its own tests** for the code, **runs them**, and **fixes on failure** — catching WRONG output that merely "ran" (`opts.test`); (2) **captures the real console output / return value** and feeds it back into the fix turn (read-the-output-then-decide); (3) on success **proposes follow-ups** (`run` / `improve` / `add-tests` / `explain` / `edit`). Proven by `forge-agent-complex.test.mjs` (**32 complex runs**, real Node execution sandbox): **20 algorithm tasks** (factorial, fibonacci, isPrime, gcd, flattenDeep, fizzbuzz, romanToInt, binarySearch, …) each driven buggy → the agent's self-test **catches the bug at runtime** → it **fixes** → re-tests → runs → done, with only the CORRECT code ever written; + **12 behaviour tests** (syntax-fix, runtime-fix, capguard veto, delete-loop veto, budget abort, best-of-N, reject, multi-step read, learning, follow-ups, output capture, contradicted-fact veto).

**Intelligent no-GPU fallback** (`engine-policy.js` + `wasm-engine.js`, gates `forge-engine-policy` 21 / `forge-wasm-engine` 6) — the user is **never left with nothing**. Chain, best→floor: **local-webgpu** (WebLLM) → **local-wasm** (wllama/CPU, no-GPU) → **online-grok** (cloud) → **device-recipe** (ANIMA on the device via `/api/anima` — always reachable offline because the page is served BY the device; grows as the learning flywheel adds recipes). Offline-first & privacy-first: a user whose target is a LOCAL model is **bridged through the private device floor while it downloads, never silently through the cloud**; messaging is honest (cloud = "not private", WASM = "slower/CPU", offline read from the real network state, no download CTA on a hard decline). Hardened by a 3-lens **adversarial review** (privacy leak in the bridge + dishonest messaging found and fixed); gated by a 128-combo exhaustive matrix + a privacy invariant (local target ⇒ bridge ≠ cloud) + the VRAM-disqualified-WebGPU→WASM fallthrough (a real bug the gate caught).

**First real-browser proof** (`apps/anima/www/forge-demo.html`, self-contained, no device API): the whole agent runs in a real browser on the REAL `nucleo-run.js` Web Worker sandbox, driven by a scripted MockEngine. Verified live via the preview tools: **factorial** (write → self-test catches wrong output `15` in the Worker → FIX → re-synth → test `120` → approve → apply → provenance → run → **learn** → follow-ups) and **dangerous eval** (capguard `VERIFY veto: capability-block:dynamic-eval` → recover to a safe version; the eval is never written). The capability dial renders the honest privacy-coherent fallback ("device-recipe (private); one-time ~1 GB to enable local-webgpu") — note this preview browser HAS WebGPU. Zero console errors. Run it: `preview_start forge-demo` → `/apps/anima/www/forge-demo.html`.

**Capstone integration gate** (`forge-integration.test.mjs`, 3/3): wires the REAL modules (loop + capguard + extract + verify + selfcheck + provenance + learn) with a real parse/exec sandbox + scripted MockEngine, then runs `promote` — proving the 18-module spine composes into one offline session (plan → best-of-N synth → verify → approve → apply → **run** → provenance → learn → promote) and that the invariants hold across the whole pipeline: a dangerous artifact is vetoed/never-applied/never-learned, and an identical recipe across two sessions is not double-learned. Host totals: **forge 135/135, suite 346/346**.

**A real model is staged on SD.** `Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC` (277 MB, 8 shards + config/tokenizer) downloaded from HuggingFace into `deploy/sd-safe/apps/anima/www/forge/models/<id>/` (served by webfs) and mirrored to the Cardputer SD (`H:`) via `tools/sd-sync.ps1` (safe, no mirror-delete). `tools/anima/prep-llm-models.mjs` (`npm run anima:prep-models`) writes a `manifest.json` (pinned HF revision + per-file SHA-256, `download.verifyManifest`-compatible); gate `forge-model-manifest.test.mjs` checks integrity (skips cleanly when absent). H: copy verified (sizes + sha match manifest). *Note:* this is the 0.5B (autocomplete-grade / the WASM-tier model); the 1.5B high-quality WebGPU coder is the same `prep-models` flow (bigger). Weights on SD ≠ model running: that still needs the `webllm-engine`→UI wiring + the WebLLM runtime lib.

**Runs without a GPU + user picks the engine.** A second model, `Qwen2.5-Coder-0.5B-Instruct-GGUF` (491 MB), is staged for the **WASM/CPU** path (the MLC one is WebGPU-only) — so it runs on a no-GPU PC (slower). `engine-loader.js` (`engineOptions`/`defaultEngine`/`loadEngine`, gate 5/5) is the **user-choice** layer: GPU(WebLLM) / CPU(WASM) / Cloud(Grok) / Device-recipes / Demo, each marked available-or-not by capability, real runtimes built behind the choice (WebLLM/wllama from CDN, weights from SD). `forge-demo.html` is now the real Forge surface with the engine picker — **verified live in a browser**: the picker shows correct availability (WASM available with no GPU), and the Demo agentic loop runs end-to-end. **"Can it download from the Cardputer?" proven** by `forge-delivery.test.mjs`: the 491 MB GGUF fetched over real HTTP in 469 bounded Range windows, SHA-verified. Wired into the ANIMA app via a `/forge` command; modules + demo + updated `index.html` propagated to `deploy/sd-safe` (+ regenerated `.gz`) and synced to the Cardputer SD (`H:`) — both models on H: (748 MB). forge gate **207/207**. *Honest:* weights on SD ≠ the model running inside the main app yet — the real GPU/CPU run is the user's to confirm in a browser (needs the WebLLM/wllama lib, currently from CDN on first use).

**Pending (need device / GPU / browser to verify — logic is built & tested, only the hardware glue remains):** the `index.html` wiring
(`animaClient.ask` call-site refactor + 4th dial segment + editor split pane + Download Manager UI),
the **flash-confirm** of the WRITTEN firmware `GET /api/anima/verify` wrapper + `nucleo_webfs`
ranged-read breaker (the cascade verifier they call is already host-gated), the real WebGPU run of `webllm-engine.js` + CodeMirror bundle, the
device→PC sync of `learned-forge.jsonl` (then `npm run anima:promote --write` + `build_akb2.py` +
`anima:gate` to actually ship learned recipes into the L1 index), and the P-1 de-risk spikes (measure
the .166; confirm the WebLLM grammar API; measure 4 GB-iGPU footprint).

---

## 1. Vision & honest innovation thesis

ANIMA stays **one assistant and one soul**. "Forge" is a new browser *surface* (a 4th view inside
`apps/anima/www/index.html`), not a separate app. The pitch in one line:

> *Generative proposes, deterministic disposes, human approves.* A stochastic LLM (in the client
> GPU) writes code and prose; nothing is shown as trusted or committed until the on-device,
> abstain-by-construction ANIMA cascade has audited what it can and the code has dry-run in a
> hermetic sandbox.

### What is genuinely novel (honest)
- **MCU-as-AI-distributor.** A PSRAM-less ESP32 serves ~1–2.5 GB of WebLLM weights to a browser
  that runs the model on its *own* GPU. The chip never runs the LLM — it only streams bytes
  through the existing `nucleo_webfs` Range/206 path. The inversion (tiny dumb server, fat smart
  client, offline after first load) is a real, unusual, defensible system shape — and it is
  *already proven* in this repo (voice.js serves Vosk WASM exactly this way; ffmpeg-core.wasm and
  js-dos too).
- **Cross-substrate grounded verification.** Using a deterministic, zero-hallucination-by-construction
  on-device reasoner (M1) as the *authoritative judge* over a stochastic generative substrate (M4),
  all local. This generalizes the existing LENS-C veto + evidence-coverage abstention from
  intent-detection to generative output.
- **A real client-side `edit→run→observe→verify` loop served offline by a microcontroller.** The
  browser *is* a world-class JS engine (the firmware has none — Lua was removed); the sandbox
  already ships; the loop closes locally with zero network.
- **One canonical envelope** that all four substrates emit byte-identically, so renderer, context
  compaction, message actions, and export never branch on who answered.

### What is best-in-class assembly (NOT claimed as invented)
WebLLM/MLC on WebGPU, XGrammar grammar-constrained decoding, client-side sandboxed execution, and
agentic `plan→edit→run` loops (Cursor/Antigravity/Claude Code) are all established. Forge composes
them; it does not reinvent them.

### Honest limit of the verification claim
"M1 audits M4" is **real today only for**: numeric/math claims (`anima_solve` re-derives them),
grounded facts (`L1`/`KGE` check against cards, abstain-not-fabricate), string/number **literals the
generated code prints** (mechanically extractable), and "it ran hermetically". It is **not** a
code-correctness prover, and auditing *arbitrary free-form prose* requires a claim-extraction step
that **does not exist yet** and is the hard, must-build-first part (§6, §13). Uncovered assertive
spans must degrade to **WARN**, never silent PASS.

---

## 2. One ANIMA, four substrates

| Mode | Runs where | Role | Browser-usable |
|---|---|---|---|
| **M1 — offline-deterministic** | On-device (ESP32), via `GET /api/anima?mode=off` | Zero-hallucination grounded brain **and** the authoritative verifier. L0/L1/KGE/`anima_solve`; abstains rather than fabricates. | Yes (today). Also the verifier, regardless of active mode. |
| **M2 — hybrid** *(default)* | On-device, escalating to live/Grok when warranted (heap-gated) | Grounded device answer first, online enrichment when needed. Zero-regression default. | Yes (today), dial `on`. |
| **M3 — online/Grok** | On-device dispatch to Grok API (key required) | Cloud teacher for what the device can't ground; existing `code_mode`. | Yes (today), dial `only`. M3 output is also M1-verifiable. |
| **M4 — local-LLM** *(NEW)* | Entirely in the client browser on WebGPU; ESP32 only serves bytes | Generative: tiny orchestrator (routing/planning/typed actions) + lazy coder (JS synthesis). Powers the agentic editor. Never trusted raw — M1 audits it. | NEW dial `local` + `auto`. Greys out (honest reason) without WebGPU/secure-context. |

Default stays **hybrid (`on`)** so nothing regresses. The native `app_anima.cpp` (no WebGPU) and the
device wire format are **unchanged**.

---

## 3. The spine — ANIMA Envelope v1

A strict **superset** of today's `/api/anima` JSON (`nucleo_httpd.c:343-609`), adding
`{ substrate, grounding, verdict, actions[], usage }`. M1/M2/M3 wire format stays **byte-identical**;
the new fields are **client-synthesized** for M4.

One client adapter is the single brain-call site:

```
animaClient.ask(q, { mode, lang, history, root, signal }) -> Envelope v1   (async-iterable for streaming)
```

It dispatches to one of four **providers** that all `normalize()` into the identical envelope:
- `deviceProvider` → `fetch('/api/anima?q=&lang=&mode=off|on|only')` (mapping at `nucleo_httpd.c:363` untouched)
- `localProvider` → WebLLM on WebGPU, synthesizing `{ substrate:'M4-local', tier:'remote', domain:'local-llm', … }`

The renderer (`addMeta/addTrace/buildCard/renderMd`) **never branches on who answered**. This is the
refactor of the single inline `fetch` at `index.html:1185`.

---

## 4. Router (two-stage) + the VRAM-aware model decision

**Stage A — deterministic pre-router** (pure, host-testable, free): slash → `handleSlash`; clear file
intent → `fsclient`; explicit in-workspace code edit → coder; live/weather/knowledge → device M1/M2
(grounded first, *never* the LLM); math/skill → device `anima_solve`. Mirrors the device's own
`nucleo_anima_query` dispatch order.

**Stage B — orchestrator LLM** (only when `mode==='auto'` AND Stage A returns null): emits a
**grammar-constrained typed ROUTE/action** from the closed `nlfs.js {op:…}` vocabulary. Handles only
the genuinely ambiguous long tail / multi-step decomposition.

### VRAM reality (must-fix #1 from review)
Two-model residency (orchestrator + coder) lands ~2.6–3 GB+ and **does not fit the dominant client
class** (≤4 GB integrated GPUs). So:
- **Discrete GPU ≥6 GB:** full two-model loop (resident orchestrator + coder).
- **≤4 GB iGPU (default reality):** **single-model degraded path** — drop the orchestrator; Stage-A
  deterministic pre-router + the **grammar-constrained coder** (resident during a code task anyway)
  emits *both* route and code. Avoids the unload/reload thrash that would make the loop
  minutes-per-step.
- **No WebGPU:** `local` greys out; `auto` degrades to M2.

`auto` chooses the path by a **VRAM probe**, not just availability — it auto-*routes*, never
auto-*downloads*.

---

## 5. Models

| Slot | Model | Size | Notes |
|---|---|---|---|
| **Coder** | `Qwen2.5-Coder-1.5B-Instruct-q4f16_1-MLC` | ~940 MB dl / ~1.63 GB VRAM, 4096 ctx | Best ≤2B JS coder; prebuilt in WebLLM (no conversion). Lazy-loaded only on a synthesis action. Temp 0.2, ~1200 max_tokens, stop on closing fence — mirrors the firmware `grok_chat code_mode` so the web ``` renderer is reused. *(No genuine 1B Qwen-Coder exists; 1.5B is the floor for correct JS.)* |
| **Orchestrator** | tier policy (see below) | 0.4–0.9 GB | Emits **only** grammar-constrained typed actions: physically cannot emit an unknown tool, malformed arg, out-of-root path, or stray prose. Reliability comes from the grammar, not the size. |

**Orchestrator tier policy** (the one open decision, gated on a spike confirming the pinned WebLLM
build's JSON-schema/grammar API):
- **Default until spike green:** `Llama-3.2-1B-Instruct-q4f16_1` (~879 MB) — safest free-form JSON adherence as the ungated default.
- **Upgrade once grammar API confirmed:** `Qwen2.5-0.5B-Instruct-q4f16_1` (~944 MB resident config, low VRAM at runtime) — shares the Qwen tokenizer family with the coder; grammar makes 0.5B trustworthy at PICK+FILL.
- **Low-VRAM floor:** `SmolLM2-360M-q4f16_1` (~376 MB), grammar **mandatory**.

All three are prebuilt in WebLLM → zero conversion. The `m4-actions` host gate must be green
(action stream always validates) before the smaller default is adopted.

---

## 6. Cross-substrate grounded verification (the heart — and the hard part)

Two layers, in front of every generated artifact before **Apply**:

1. **Client static gate** — dry-run the candidate in the existing `nucleo-run.js` sandbox with
   capabilities **denied** (`{ fs:false, http:false, anima:false }`), plus a new `mode:'check'`
   parse-only pass returning `syntaxOk/line/col`, output/iteration bounds, wall-clock terminate.
2. **Device grounded gate** — read-only `GET /api/anima/verify`: `anima_solve` re-derives numeric
   claims; `L1`/`KGE` + the evidence-coverage guard check factual claims (no support → WARN,
   contradicted → VETO, never silent pass).

Verdict `{ pass | warn | veto, checks[], evidence[], coverage:{found,checked,uncovered} }` rides in
the same envelope and renders as the familiar ⎿ trace + a **substrate chip** (M1/M2/M3/M4) + a
**verdict chip** (✓ grounded / ⚠ unverified / ⛔ vetoed). Veto shows a red banner with a "show
anyway" affordance — never a silent commit.

### Claim extraction is the unbuilt core (must-fix #2)
Today the verify primitives run **inside** `nucleo_anima_query` against a single typed NL line — not
as "given this paragraph of M4 prose, find and check every assertion." That extraction step is the
genuinely hard part and **must be built and host-gated first**, with **coverage as a hard property**:
- The verdict reports `found / checked / uncovered`; any **uncovered assertive span downgrades
  PASS→WARN**.
- For code, lean on the deterministic side: numeric/string **literals the code prints** are
  mechanically extractable (no NLP) — verify those; treat free prose as WARN-by-default.
- Build it as its own module `m4-extract` with adversarial "planted claim it must NOT miss" cases in
  the gate, *before* wiring the endpoint.

Honest framing: the verifier vouches for **math + grounded facts + "it ran hermetically"**.
Everything else is **⚠ unverified**. That is weaker than "M1 audits everything" — and it is the
truth.

---

## 7. Agentic editor + client-side runtime

**Surface.** Promote the transient Workspace drawer into a persistent, resizable split
`[chat | divider | editor]` (divider persisted; collapses to the drawer on mobile). Editor engine =
**CodeMirror 6**, tree-shaken to ~250–300 KB gz — chosen over Monaco (3–5 MB + AMD loader +
worker-per-language would saturate the single-task webserver and trip the heap breaker). The CM6
bundle ships from SD as split parts via the **same `voice.js` `assembleParts` rig** (force-cache +
retry/backoff), CDN-fallback, cached offline. *(This adds a CodeMirror bundle step — a new toolchain
dependency in a repo that prides itself on no-build web apps; flagged in §12.)*

Reuses `fsclient.js` (root-confined hands, `resolve()` escape-guard), `buildFileView/renderFileView`,
`runScript`, and the ▶ Run already wired to `nucleo-run.js` at `index.html:888`.

**The loop** (`agent-loop.js` explicit state machine):

```
IDLE → PLAN (orchestrator: goal + context.js digest + tool schema + workspace tree)
     → GATHER (read/tree/search/glob via fsclient)
     → PROPOSE (coder: whole-file content OR exact old/new edit)
     → VERIFY (§6 client static gate + device grounded gate)
     → AWAIT_APPROVAL (human, mandatory for writes — like Claude Code's Edit)
     → APPLY (fsclient.write/edit)
     → RUN (sandbox)
     → OBSERVE → FIX (loop back with error+stack+veto reason)
     → DONE (stop action / step budget)  |  ABORT (unparseable / veto-exhausted)
```

The orchestrator emits **only** the closed typed-action set (the same `nlfs.js {op:…}` vocabulary →
`runFileIntent`'s executor switch reused verbatim); the coder is invoked only for synthesis, never
control flow. Every transition appends a ⎿ trace step (streaming) so the local loop reads identically
to a device multi-step answer. Diffs are computed with `fsclient.diffLines` **before** any write and
staged as pending hunks (Approve / Approve-all / Reject; opt-in session auto-approve scoped to the
workspace root).

### Sandbox hardening required BEFORE the loop (must-fix #4)
`nucleo-run.js` is strong (fetch/XHR/WS/importScripts nulled, fresh single-shot worker, wall-clock
terminate) but needs, before P4:
- `mode:'check'` parse-only (no execution),
- output-byte/line caps + a coarse iteration tripwire (today only wall-clock — a tight loop or a
  multi-GB string alloc runs until the timer/tab dies),
- **all file mutations routed through the staged diff-Approve flow** — generated code must **never**
  call `os.fs.write/append/remove` directly during an agentic run (a hallucinated `os.fs.remove`
  loop could damage the workspace inside the timeout window),
- a pre-write snapshot/undo (recycle-bin reuse).
Verify-runs stay hermetic (`fs:false`, already supported by construction).

---

## 8. Model delivery & download

**Internet-first from a PINNED HuggingFace revision (commit SHA); SD-fallback only when offline.**
This is the exact lesson `voice.js` already paid for: the single-task no-PSRAM httpd collapses on
sustained multi-MB reads (Vosk resets ~2–3 MB in; the 503/Retry-After circuit breaker exists *for*
this). So the client fetches big weights **directly from the HF CDN**, bypassing the device; the
Cardputer SD is the **air-gapped fallback**.

Default split: serve the **small orchestrator from SD** (works offline immediately, ~5–8 min) and
pull the **big coder from the CDN** (SD-fallback only when truly air-gapped, ~12–17 min @ ~1 MB/s).

### Corrections from review
- **The 640 MB write cap (not 8 MB).** `NUCLEO_MAX_UPLOAD_BYTES = 640 MB` with a `.tmp`+atomic rename
  (`nucleo_fsapi.c:23`); the "<8 MB" figure was a `voice.js` WiFi-reliability chunk size, *not* a
  hard cap. Don't size the UX off stale memory — **re-measure on the .166** (SD sustained-read reset
  window, largest reliable ranged-read window under ~18 KB runtime heap, real upload behavior) per
  the `anima-debug-default` discipline.
- **Ranged reads bypass the breaker (must-fix #3).** WebLLM shards are 17–32 MB each — far above the
  reset window — so the SD path needs a **Range-window fetcher** (256 KB–1 MB adaptive windows,
  halving toward 256 KB on repeated resets, concurrency=1). But `nucleo_webfs.c:117` gates the 503
  breaker only on `!want_range && !gz`, so a ranged read-storm is invisible to it. **Fix:** extend
  the breaker to throttle large ranged reads under low heap, OR hard-commit CDN-only for weights and
  treat SD-serve as a rare degraded mode that **pauses verifier traffic during a pull**. Never run M1
  verification concurrently with an SD model pull. Make the device-coexistence test a release gate.
- No physical re-split needed: keep WebLLM's filenames; the Cache API keys intentionally collide
  between CDN and SD sources (download once online = a valid verified SD copy).

**Manifest** (client-verifiable, per model): pinned revision + per-shard bytes + SHA-256 +
`model_lib` .wasm name + `minVramMB`. Gates integrity, makes resume idempotent (skip shards whose
cached SHA matches). The `web-llm` library version is pinned alongside the model revision (model_lib
coupling).

**Download Manager UI:** source (internet/SD), total size, live-throughput ETA (not a fixed
estimate), per-shard progress, pause/resume, verify, re-download, "Offline-ready ✓". Cache-hit
progress synthesized from the manifest (WebLLM's `initProgressCallback` is silent on cache hits).

**Provisioning:** `tools/anima/prep-llm-models.mjs` (sibling of `split-vosk-models.mjs`) pulls the
pinned revision, lays files under `deploy/sd-safe/.../llm/<id>/`, computes SHA-256, writes the
manifest; `sd-sync.ps1` mirrors (no `/MIR`). Content types: `.wasm`→`application/wasm`,
`.bin`→`application/octet-stream`.

---

## 9. UI/UX principles

1. **One assistant, one transcript.** Every turn (M1–M4) renders through the identical pipeline.
   Four engines must look like one mind thinking.
2. **Honest provenance, always visible.** A leading **substrate chip** (on-device / grounded / cloud
   / local-GPU) is the first meta tag on every turn, with a one-line "why this engine" in `auto`
   mode. **M4 turns never borrow the on-device/grounded badge color** — a generative answer must
   never masquerade as the deterministic brain.
3. **Grounded-by-construction trust marks.** ✓ grounded vs ⚠ unverified vs ⛔ vetoed — **icon+text,
   never color-only**. This is the cross-substrate USP and the contract that keeps four substrates
   feeling like one honest assistant.
4. **Zero regression.** Default stays hybrid; dial keeps `off/on/only` and ADDS `local` + `auto`;
   native app and wire format untouched.
5. **Human-in-the-loop for mutations.** Every agent write is a staged diff gated behind
   Approve/Reject, byte-identical to the existing manual file-op card.
6. **Honest one-time cost.** Download framed as "one-time setup, then fully offline, free, on your
   device", with real progress + live ETA + a clear greyed-out "unsupported (no WebGPU)" state —
   never a silent 15-minute stall.
7. **Capability-aware degradation.** `auto` routes only among AVAILABLE substrates and never
   auto-downloads. Deep agentic work lives in the ANIMA app tab; the OS copilot stays light and hands
   off via the existing `open-app` postMessage (never hosts WebLLM).

---

## 10. Test strategy

Split into **deterministic plumbing** (hard pass/fail gates) and a **nondeterministic generative
core** (thresholded evals). Keystone seam: **every LLM-driven module takes `engine` by injection**
(`interface Engine { chat(messages, {schema?, temperature?, seed?}) -> {text, usage} }`); a
`MockEngine` drives the whole `plan→edit→run→verify` loop from a scripted queue — so router,
action-validator, sandbox, download FSM, and verifier are all exercised **without a GPU**. Author
every new web module as **pure ESM** (no top-level DOM/WebGPU) so it imports straight into
`node --test` like `nlfs-check.mjs`/`context-check.mjs`.

**New deterministic host gates** wired into `tools/anima-host/gate.mjs` (the `flash.ps1`/`release.ps1`
red=abort pre-flight):
- `route-envelope` — every substrate produces a valid Envelope v1; `normalize()` renderer-safe.
- `m4-router` — 0 misroutes on a labeled `eval_route.jsonl` across the WebGPU/cached/online matrix; golden snapshot.
- `m4-actions` (schema firewall) — for ANY model output incl. adversarial/garbled JSON, `parseActions` never yields an executable action outside the closed set or an out-of-root path; fuzz-tested.
- `m4-extract` (coverage) — planted claims must not be missed; uncovered assertive span ⇒ WARN. *(Build first.)*
- `m4-sandbox` (policy) — `fs:'none'` rejects every `os.fs.*`, `http:false` rejects `os.http.*`, `mode:'check'` never executes, timeout terminates `while(true)`, output bounds enforced.
- `verify-grounding` — drive the **real `anima.exe`** over labeled `(artifact, expected-verdict)` cases: "capital of France is Lyon" must VETO, a valid pure function "ok", an unfounded claim "flag". Folds into the halluc 0/441 discipline (generative traps also 0).
- `m4-loop` — full loop with `MockEngine` + in-memory `fsclient` + real sandbox policy + real-exe verifier: converges, no write without approve, halts on step budget, does NOT auto-apply on reject.
- `m4-deploy.test.mjs` — manifest declared bytes/hashes match shards under `deploy/sd-safe` and `H:`; every M4 module's `.gz` in sync.

**Browser gates** (preview / Claude-in-Chrome): editor UI + diff-approve + run-output render;
end-to-end loop with a **mock** engine (deterministic); a **manual** real-browser **sandbox-escape**
suite (`fetch`→throws, `top.document`→undefined, `while(1)`→terminated) as a named checklisted
release item.

**Model quality** is the one thing **not** auto-gated: a manual `npm run m4:eval:gpu` does pass@k by
**running** generated JS in the real sandbox and asserting observable behavior (not text-match),
tracks a committed floor, with the M1 verifier as the runtime backstop. Enforce mechanically that
**only `MockEngine` ever appears in a hard gate** (`assert engine instanceof MockEngine`).

---

## 11. Phased roadmap (revised — de-risk first)

| Phase | Deliverable | Builds on | Gate |
|---|---|---|---|
| **P-1 — spikes** *(new, de-risk)* | (a) measure real device limits on .166; (b) confirm pinned WebLLM grammar/JSON-schema API; (c) measure real resident footprint on a 4 GB iGPU. | `anima-debug-default` discipline. | Findings recorded; resolves the orchestrator tier + single-vs-two-model default. |
| **P0 — Envelope + adapter** | Envelope v1 (superset); refactor inline `fetch` (`index.html:1185`) → `animaClient.ask` + `deviceProvider` + `normalize()`; add substrate/verdict meta chips. Wire format byte-unchanged. | `GET /api/anima` + renderer (`index.html:988-1028`). | `route-envelope` (anima:gate, red=abort). |
| **P1 — Router + 4th dial** | `router.js` Stage A + dial gains `local`+`auto` (default stays `on`); availability dots. Stage B stubbed (MockEngine). | `nlfs.parseFileIntent` + device dispatch order + `chooseMode/seg2`. | `m4-router` (0 misroutes). |
| **P2 — Verifier + extractor + sandbox** | `m4-extract` module (coverage-gated) **first**; read-only `GET /api/anima/verify` (`anima_solve` + L1/KGE + evidence guard); harden `nucleo-run.js` (`mode:'check'`, caps, mutations-via-diff-only, snapshot/undo). | verify trace convention, `eval_traps.jsonl`, existing sandbox (`nucleo-run.js:81-116`). | `m4-extract` + `m4-sandbox` + `verify-grounding` (real `anima.exe`); manual browser escape suite. |
| **P3 — Delivery + Download Manager** | `download.js` (FSM + manifest + Range-window fetcher + breaker fix) + `webllm-engine.js` (WebGPU probe, resident orchestrator, lazy coder, **VRAM-probe single-vs-two-model**) + `prep-llm-models.mjs` + Download Manager UI. `localProvider` → device-shaped envelope. | `voice.js assembleParts` + webfs Range/breaker + `split-vosk-models.mjs` + `sd-sync.ps1`. | `m4-download` (FSM+manifest+bounded-Range) + `m4-deploy.test.mjs` + **device-coexistence gate** + browser offline-reload check. |
| **P4 — Agentic editor + closed loop** | Editor split pane (CM6 from SD shards) + `agent-loop.js` + `actions.js` validator + diff-approval. Full `plan→edit→run→verify→apply` with M1 verifier; coder only for synthesis. | `fsclient` + `runFileIntent` switch + `buildFileView/runScript/attachRun` + P2 sandbox + P0 envelope. | `m4-actions` (fuzz) + `m4-loop` (MockEngine) + browser editor E2E. |
| **P5 — Auto default + polish + GPU eval** | `auto` recommended default on WebGPU clients (opt-in until P3/P4 green); OS copilot hand-off; `navigator.storage.persist`; manual pass@k floor. | P1 Stage B + P0 provenance chip + `copilot.js` postMessage. | manual `m4:eval:gpu` (tracked floor) + a11y/theming pass; `flash.ps1 anima:gate` green for the verify endpoint. |

---

## 12. Honest caveats & open decisions

- **WebGPU client requirement.** M4 runs only where the browser exposes WebGPU (desktop-mostly,
  secure-context gated). On the device's plain-HTTP LAN origin WebGPU is blocked **exactly like the
  voice mic** — reuse the `anima-voice-chrome.bat` insecure-origin-allowlist precedent. Many
  phones/older GPUs can't run `local`; it must grey out honestly and `auto` must never pick it.
  M1/M2/M3 stay fully functional → graceful, not broken. **Honest positioning: the headline is the
  verification loop + MCU-as-distributor shape, not "everyone gets a local LLM."**
- **The Cardputer download is genuinely slow** (~5–8 min orchestrator, ~12–17 min coder, air-gapped
  from SD @ ~1 MB/s). Mitigated by CDN-first + lazy coder + SD pre-staging + one-time Cache-API cost,
  but the air-gapped first run is a long, honest setup.
- **LLM nondeterminism is uneliminable.** Coder quality cannot be a hard binary gate without flaking
  CI. Only deterministic plumbing is hard-gated (MockEngine); raw quality is a manual pass@k with a
  tracked floor + the M1 runtime backstop. A model/quant swap could silently regress — only the
  manual eval + verifier would catch it.
- **The verifier is a fact/numeric/run oracle, not a correctness prover.** `verdict:ok` means
  "grounded claims hold AND it ran in a hermetic dry-run" — not "this code is correct". Apply still
  requires human approval. Treating ok as correctness is a category error.
- **Absence of evidence ⇒ WARN, never silent PASS.** Unsupported domain knowledge gets ⚠ unverified;
  M4 can produce a plausible, unverified answer the system honestly cannot ground. The UI must make ✓
  vs ⚠ unmissable.
- **The generative-vs-grounded line is the project's identity.** M4 is a deliberate tension, resolved
  *only* by the inversion (generative proposes, deterministic disposes, human approves writes) and by
  never letting M4 borrow the on-device trust badge. If that slips, M4 becomes just another
  hallucinating agent and the differentiator is lost.
- **Untrusted model code in the browser is the highest-severity surface.** Verify-runs are hermetic;
  committed runs are workspace-root-confined by `fsclient.resolve()`; writes need approval; the
  real-browser escape suite is **manual and must not be skipped under deadline pressure**.
- **Open decisions:** orchestrator default (0.5B+grammar vs Llama-1B ungated, gated on P-1 spike);
  whole-file vs unified-diff coder hand-off (diffs need an apply-patch primitive in `fsclient`/`nlfs`);
  whether mid-loop M4→M1 retrieval counts as grounding for the verdict.
- **Deploy discipline is load-bearing.** Every asset change ⇒ regenerate `index.html.gz` + bump
  `sw.js` (the stale-HTML/new-JS boot skew) + add model files+manifest to `deploy/sd-safe` (or
  `sd-sync` won't copy them) + the new CodeMirror bundle step (a build dependency in a no-build repo).

---

## 13. Must-fix-before-build checklist (from the adversarial review)

1. **Build & host-gate the claim-EXTRACTION module first** (`m4-extract`), coverage as a hard
   property (found/checked/uncovered; uncovered assertive span ⇒ WARN; planted-claim adversarial
   cases). Without it the verification loop is aspirational.
2. **Re-measure real device limits on the .166** (SD reset window, largest reliable ranged-read under
   ~18 KB heap, real upload). Confirm the 640 MB write cap finding. Don't size UX from memory.
3. **Extend the webfs breaker to cover large ranged reads under low heap** (or hard-commit CDN-only
   for weights); forbid M1 verification concurrent with an SD model pull; make the device-coexistence
   test release-blocking.
4. **Harden `nucleo-run.js` before the loop**: `mode:'check'`, output/iteration caps, all mutations
   via the staged diff-Approve flow (never direct `os.fs.*` during an agentic run), pre-write
   snapshot/undo. Keep verify-runs hermetic.
5. **Resolve the orchestrator question with a measured spike** AND commit the **single-model degraded
   path** (deterministic pre-router + grammar-constrained coder emits route+code) as the default on
   ≤4 GB GPUs, so `auto` never attempts two-model residency where it can't fit.
6. **Make the provenance/verdict UI load-bearing**: M4 never renders the grounded badge color; ✓/⚠/⛔
   are icon+text, unmissable.
7. **Document honest positioning**: flagship = the verification loop + MCU-as-distributor, runnable
   on a discrete-GPU desktop / localhost — not a universal in-browser LLM. Default hybrid; `auto`
   opt-in until M4 gates are green.

---

*Sources: this doc integrates a 6-dimension design fan-out + adversarial stress-test over the real
codebase (`apps/anima/www/*`, `nucleo_httpd.c`, `nucleo_webfs.c`, `nucleo_fsapi.c`, `nucleo_anima*.c`,
`voice.js`, `nucleo-run.js`, the gate harness) plus current (2026) WebLLM/WebGPU research. File:line
citations live in the design transcript.*

# AI roadmap — where the OS's intelligence goes next

This file records the outcome of a structured design review (2026-08-18): five parallel readers
mapped the real AI stack, four independent lenses generated 24 proposals, and a skeptical judge
ranked them against the hard constraints — ~18 KB device heap, 120 mAh battery, browser does the
compute, offline-first, hobby-scale effort. The top of the ranking **shipped the same night**
(see `docs/shell-cache-log.md`, `v131`); this file keeps the rest of the ranking, with the
judge's reasoning, so future work starts from decisions instead of re-deriving them.

## The strategic through-line

> The survivors all "light up dormant organs": S/M effort, zero firmware bytes, zero device
> heap, riding seams that are already built and idle. The OS's thesis is: **the device is
> transducers plus deterministic truth; the browser is all cognition.** The next wins come from
> wiring existing organs together, not growing new ones.

Two disciplines fell out of the review and are now house rules for AI work:

1. **Measure before building** anything that writes into ANIMA's memory formats or bets on
   unmeasured model quality. The project's history (pack-tree divergence, the 0-hallucination
   contract) says silent data corruption and junk relevance are the real failure modes. The
   truth lamp shipped only after `tools/anima-host/truthlamp-yield.mjs` measured 45% decisive
   verdicts against a 20% bar — the same gate applies to everything below.
2. **Don't reverse deliberate design decisions** and don't build for hardware the household
   doesn't reliably have, without a concrete reason to exist.

## Shipped (v131) — for context

| Feature | What it proved |
|---|---|
| Voice copilot (mic 🎤 in Ctrl+Space, device mic → Vosk in browser, spoken replies via `/api/tts`) | the "device is the body, browser is the brain" inversion is demo-able |
| Broker `ai.ask`/`ai.complete` (manifest permissions `ai.anima`/`ai.cloud`) | intelligence as a permission-gated OS syscall for machine-written apps |
| Proactive ANIMA (`web/shell/ambient.js`, 2 rules, ✨ switch) | the dormant `src:'anima'` notification channel, finally speaking |
| Answering search row (webstore lane, in-place upgrade) | zero-device-traffic instant answers in the Start menu |
| Truth lamp (cloud replies fact-checked by the C brain, decisive-only badges) | the 18 KB microcontroller grading the frontier model |

## The pipeline, in the judge's order

### 1. Third brain: NucleoMind phone as a discovered LLM tier — score 6
`nucleomind/` is a full Android project with `/health`, `/v1/distill`, `/v1/ground` and QR
pairing documented — and zero references in `ai.js` or `cascade.js`. A missing integration of
an existing asset.
- **First slice:** one `PROVIDERS` entry in `ai.js` speaking the already-supported OpenAI wire
  to the QR-paired URL, plus a `/health` probe and a presence dot in the copilot. Grounding and
  distill write-back are later slices.
- **Killer risk:** phone-side reliability (the household's phone LLM server has an open fault:
  garbage past ~50 tokens; phones throttle background servers). The router must health-probe
  aggressively and fail silently to the next tier. Browsers cannot do mDNS: discovery is
  QR/manual URL only, and the phone app needs CORS.

### 2. Night consolidation: verified self-distillation while the device sleeps — score 6
An idle-and-charging browser job that closes ANIMA's declared open loop: distill the day's
learned material, verify every candidate against `GET /api/anima/verify` (CONFIRMED-only
survives), teach the survivors back to the device.
- **First slice:** teach-path-only — queue copilot refusals, research via `webindex.js`,
  verify-gate, then teach through sequential `/api/anima` sentences (the proven miei-fatti
  mechanism, PIN-gated, spine-serialized). **Do not write `mind.jsonl` or recall sidecars from
  the browser** until byte-compat is host-verified: a subtly malformed line ships straight to
  the screen under the verbatim contract.

### 3. Reflex Engine: automation-studio as a browser rules engine — score 5.5
The registered `enabled:true` + `autostart:true` service with no implementation is a shipped
falsehood; a zero-firmware browser-side rules engine is the only honest way to build it.
- **First slice:** define the missing `schemas/rule.schema.json` plus a pure host-tested
  evaluator with two triggers (`fs.changed`, `calendar.reminder`) and two actions (notify,
  open app), wired into the `connectWS` fan-out.
- **Killer risk:** the always-on expectation gap — rules only run while a shell tab is open.
  Either the UI says that plainly, or the feature reads as broken.

### 4. Episodic Journal — the OS remembers what happened — score 5
The shell already sees every event/window/copilot turn; logging is a weekend. The RETRIEVAL
axis that justifies it is a month, and a full behavioral log on a shared SD is a privacy
decision, not a default.
- **First slice:** `journal.js` as a pure host-tested module logging episodes to IndexedDB,
  surfaced ONLY as a reverse-chronological "Timeline" group for the empty search query. No NL
  parsing, no SD writes, until the timeline proves it gets opened.

### 5. One Embedding Space: same-encoder semantic search over the SD — score 4.5
The most-seduced idea (three lenses proposed it); the plumbing verifies (`nucleo_anima_l1_encode`
is one KEEPALIVE export away in `wasm_main.c`) but nobody has measured whether a 3.1 MB int8
n-gram encoder distilled for QA retrieval is any good at ranking filenames and file snippets.
- **First slice (mandatory measurement spike):** host-side, zero shell code — embed ~200 real
  SD filenames+snippets and ~30 hand-written queries with the actual encoder, measure top-3 hit
  rate against the existing lexical ranker. Build the search lane only if it wins.

## Rejected, and why (do not re-propose without new facts)

- **Forge OS-wide / WebLLM as a copilot substrate** — WebLLM needs a WebGPU-class GPU; the
  project's paired machine is a Bay Trail netbook with no Vulkan/WebGPU. Fails on the actual
  client hardware.
- **Chorus/mesh swarm agents** — the only proposal needing new firmware handlers, plus two
  Cardputers and two host browsers before anything demos. Waits for a concrete reason.
- **Operator Mode (hardware tools in the agent)** — reverses a deliberate, documented decision:
  `runtime.js` explicitly declines to wire `toAgentTools()`; hardware skills live inside
  dedicated apps.
- **Momenti (ephemeral AI-composed micro-apps)** — effort L for a marginal delta: the agent's
  scaffold→edit→lint→review→publish pipeline already turns intent into a working installed app,
  with iteration and a safety story.
- **Self-distilling photo memory** — the Cardputer has no camera; a niche corpus within a niche
  device against real multi-provider vision costs.

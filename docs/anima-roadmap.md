# ANIMA — executable roadmap (the master plan)

ANIMA is an **offline bilingual (IT/EN) micro-agent** on the M5Stack Cardputer (ESP32-S3,
~512 KB SRAM, no PSRAM, microSD ≈ 1 MB/s). It routes a query, **uses tools**, answers from
**multiple knowledge domains** via RAM-bounded RAG, and keeps a small **working memory** —
all on a $30 device, no internet. Design details: `anima.md` (engine), `anima-agent.md`
(micro-agent + RAM-managed RAG). This file is the *ordered, verifiable* build plan.

## North star
A semantic brain **distilled offline** from SOTA models, frozen onto the SD, executed in
pure int8 C on the device. Compute is capped; storage is not — so heavy intelligence is
pre-baked, and at runtime the device does cheap routing + one small SD read. The edge over
mobile/server RAG: a **cascade that makes retrieval rare** (energy-first) + **centroids-in-
RAM / clusters-on-SD** (corpus 100× the RAM) + **tools that never invent arguments**.

## Invariants (never violate)
- No generative LLM at runtime (token-per-minute wall). Retrieve + template, never generate.
- RAM cost is O(domains × centroids), never O(corpus). Big data lives on SD.
- Cheapest tier first: L0 keyword/tool → encoder+RAG only on the residual.
- Tools: strict typed schemas + validation; refuse rather than hallucinate args.
- Bilingual from the foundation, not bolted on: one multilingual encoder, language auto-detect.
- Every claim verified with a number or an on-device check before moving on.

## Phases (each has a gate: do not proceed until it passes)

### Step 1 — Bilingual encoder (FOUNDATION) ← building now
Re-distill the on-device encoder from **multilingual-e5** on **IT + EN**, with a
**cross-lingual alignment** term from Tatoeba IT↔EN pairs, so Italian and English land in
one shared space (an EN query can hit IT knowledge and vice-versa). Same `ANE2` int8 table,
same size → zero extra device RAM. Output: `models/anima-it-encoder.bin` (now bilingual).
**Gate:** held-out student-vs-e5 Spearman ≥ ~0.45 on *both* languages, and cross-lingual
recall@1 (EN→IT translation) clearly above chance. Report the numbers.

### Step 2 — Tool layer + `create_file` (agentic)
Typed tool registry `{name, schema, extract, validate, run}`. First tool: `create_file(path)`
→ empty text file on SD (atomic). Slot extraction + path/extension validation; refuse on no
valid name. Reframe `open_app`, `system_info` as tools. **Gate:** "crea un file note.txt" /
"create a file note.txt" → file appears on SD; bad input → safe refusal.

### Step 3 — RAM-managed multi-RAG (`AKB2`, SD-resident clusters)
EcoVector-lite: k-means **centroids + cluster directory in RAM**; vectors + answers grouped
by cluster **on SD**, read one cluster per query. Bounded LRU cache. Offline builder does the
clustering; device search reads only the winning cluster. **Gate:** index of ≥1000 entries
answers in < ~1 s with RAM delta < ~30 KB; RAM flat as the corpus grows 10×.

### Step 4 — Domain packs (bilingual)
Build `general` (facts), `programming` (variabili, tipi, if/else, cicli, funzioni, array,
stringhe, debug — IT+EN, with micro-examples), `nucleo-os` (manifest, intents, runtimes,
automation micro-VM, SD layout — from `docs/*.md`). Each clustered into `AKB2`, answers in
both languages. **Gate:** representative IT and EN questions per domain retrieve the right
answer in the query's language.

### Step 5 — Router / orchestrator
Domain selection by centroid; tool-vs-command-vs-knowledge decision; **language auto-detect**
(cheap n-gram vote) → answer in the query's language; per-tier confidence gate; honest "non
lo so / I don't know" on low confidence. **Gate:** mixed IT/EN session routes correctly,
commands never trigger a needless SD scan (energy check).

### Step 6 — Working memory (extractive, ESP-safe)
Session ring (last ~8 turns) + entity slots (last app/file/topic) + **context-biased
retrieval** for short follow-ups (`emb = α·query + (1-α)·prev`) + extractive template
"summary"; persisted via the existing event journal, reloaded at boot. No generation.
**Gate:** "apri le foto" → "no, la musica"; "che batteria ho?" → "e lo spazio?" resolve.

### Step 7 — Hot-reload + polish
Reload packs without reboot (so new RAGs/knowledge apply live); smooth-scroll/flicker polish;
optional stronger encoder (bigger table / e5-base teacher) if quality gates demand it.

## Adaptive evolution (folded in — "more architecture, not more model")

Principle: the best ESP system isn't the one with the most modules, but the one that
**lights up the fewest per request**. Make the cascade an explicit, budget-aware controller.
Adopt incrementally on top of the working engine — NOT a rewrite ("ANIMA 2").

- **Micro-thought (the keystone).** The engine emits a small typed plan, not just text:
  `{ intent, domain, memory_need, tool_call, retrieval_budget, lang }`. Layers execute the
  plan. Deterministic, cheap — the MCU analog of reasoning. Reframe `anima_result_t` toward this.
- **Adaptive probe controller (governor-lite).** Choose top-1/2/3 clusters from the centroid
  margin + L0/router confidence (we hardcode TOPC=2 today). Cheaper on easy queries, deeper on
  ambiguous ones. The governor MUST stay ~3 cheap heuristics (L0 margin, length/has-verb,
  confidence) — never its own model/subsystem, or it betrays the principle.
- **Utility memory (not similarity).** Store what changes future decisions: last file, last
  app, current goal, last error, user preference, interrupted task — a tiny slot struct, not
  chat embeddings. Enables follow-ups ("aprilo", "cancellalo").
- **Typed tool registry.** Formalize tools (schema, validators, permissions, rollback,
  confirm-on-risky) — create_file is the first; grow the registry.
- **Live knowledge packs (later).** Per-domain hot-swappable packs (core-os, programming,
  files, network, user-notes); router picks the pack before retrieval. Premature at ~50
  labels (one clustered index already gives low I/O); adopt when a pack grows big or becomes
  writable (user-notes).
- **Companion bridge (optional, last resort).** Offload only when expected value > cost/risk,
  with full offline fallback. Keep it opt-in; it dilutes the offline identity if made core.

Each layer must be able to say "I'm not needed" — enforce that, or layering makes it heavier.

## Honest limits (kept visible)
- Single-step agent (intent → tool/answer), not multi-hop reasoning (needs a generator).
- Distilled encoder is approximate; gates stay conservative; router accuracy is the ceiling
  as domains multiply (mitigate with hierarchical routing later).
- On-device summary is extractive/templated, never abstractive.

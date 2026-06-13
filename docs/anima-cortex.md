# ANIMA — Cortex & the real implemented pipeline (ground truth)

`anima.md` / `anima-agent.md` describe the *aspirational* L0→L1→L2→L3 design. This file documents
what `nucleo_anima_query()` actually runs today, plus the two mechanisms added on top of it. Keep
this in sync with the code; it is the map a new reader should trust.

## The real cascade order (firmware/components/nucleo_anima/nucleo_anima.c)

```
ripeti/replay → clarify-L1 resolve → "dimmi di più" → online-only mode →
live-priority (weather/news; frees L1) →
CORTEX: build the typed plan  ← NEW (front-end, classifies once)
try_cascade: L0 keyword/tool/intent  →  L1 evidential retrieval →
spellfix retry (GATED by plan.allow_spellfix)  ← NEW gate →
combinator (neuro-symbolic compose) → HDC/KGE deduction (frees L1) →
online: fact(Wikidata) → entity(Wikipedia) → live → recall → clarify-band → bare-entity → teacher(LLM) →
honest "non lo so"
```

Notes vs the aspirational doc:
- **L2 (MOSAICO span-stitch) is not built** — `ANIMA_TIER_STITCH` exists in the enum only.
- The runtime index magic is **`AKB3`** (not `AKB2`).
- The reasoning tiers (combinator, HDC/KGE) and the 7-stage online tier are not in the L0–L3 model;
  the cascade has ~7 effective tiers, not 4.

## Cortex — the cognitive front-end (the roadmap "micro-thought")

`anima_cortex_plan(raw, en, &plan)` normalizes + tokenizes the query **once** and emits a typed plan
the cascade reads instead of every tier re-deciding:

```c
typedef struct {
    uint16_t       feat;               // F_DIGIT|F_MATHOP|F_DEFWORD|F_QWORD|F_OPENVERB|F_CREATEVB|
                                       // F_TEMPORAL|F_WEATHER|F_NEWS|F_FILENOUN|F_FOLLOWUP
    anima_iclass_t klass;              // COMMAND | TOOL | FACT | LIVE | FOLLOWUP | UNKNOWN
    bool           allow_spellfix;     // command-vocab typo rescue is safe for this query
    bool           reclaim_before_net; // free L1 once before a heavy net/HDC tier
} anima_plan_t;
```

First consumer: **the spellfix is gated by the plan.** The command-vocab autocorrect must never touch a
fact/live question — it once "corrected" the valid word *domani* → *comandi*, hijacking a weather query
to the *capabilities* answer. Now `allow_spellfix` is false for FACT/LIVE classes (and any query with a
definition/weather/news signal), so that whole bug class is gone. `reclaim_before_net` is computed for a
future single-reclaim consolidation (replacing the 3 scattered `nucleo_anima_l1_unload()` calls).

## Evidential gate — CRAG-style cross-evidence rescue (the innovation)

A distilled n-gram encoder (held-out Spearman ~0.45) places an in-scope **paraphrase** below the 0.85
answer gate, so the hard gate refused real questions. Measured on host:

| query | top1 | top2 | margin | verdict |
|---|---|---|---|---|
| come ricavo la corrente da tensione e resistenza | 0.758 | 0.591 | **0.167** | in-scope → recover |
| differenza tra ram e flash | 0.776 | 0.618 | **0.158** | in-scope → recover |
| cos'è la ram | 0.769 | 0.764 | 0.005 | near-tie → clarify/refuse |
| perché il cielo è blu (OOD) | 0.675 | 0.660 | 0.015 | refuse |
| qual è il senso della vita (OOD) | 0.627 | 0.621 | 0.006 | refuse |

The **margin (top1−top2) is the independent evidence the bare absolute lacks**: an in-scope paraphrase
stands out from the pack; an out-of-scope/wrong match scores alike to its neighbours. So the gate in
`nucleo_anima_l1_query()` accepts on **high absolute OR (mid absolute ≥ 0.72 AND margin ≥ 0.12)**. This
recovered the two real questions above with **zero** new false positives — host routing 90.2% → 93.4%.

## How to verify (host-first, no flash — see docs/debugging.md)

```
node tools/anima-host/anima.mjs --build           # compile the real cascade on PC
node tools/anima-host/route-check.mjs             # routing accuracy vs eval labels (currently 57/61)
node tools/anima-host/route-check.mjs --snapshot  # diff routing vs the golden (regression gate)
ANIMA_TRACE=1 ./tools/anima-host/build/anima.exe "…"   # prints the L1 top-2 cosines (how close a miss was)
```

`route-check.mjs` is the anti-regression net for orchestrator changes: capture a golden, change code,
re-run, review the diff. Zero unexpected diffs = no regression.

## Generic entity recognition — answer about ANY person/thing, then persist

ANIMA doesn't store a fixed list of famous people. It recognizes the *shape* of "asking about a named
thing" and pulls the entity out, for **any** name. `nucleo_anima_online_entity()` (in
`nucleo_anima_online.c`) strips the longest matching wrapper from ~130 bilingual phrasings — "chi è X",
"conosci X", "per cosa è famoso X", "eri a conoscenza di X", "di che attore è X", "what is X famous for",
"what did X do", … — plus a leading article and an English tail, leaving the entity. It enumerates
*phrasings*, never *names*, so it generalises (even to invented names like "Pinco Pallino").

Verified two ways (the C grammar is device-only, so it's mirror + standalone-checked):
- `tools/anima/entity.mjs` — host mirror: **380/380** over 38 phrasing patterns × 10 arbitrary names, 0
  out-of-scope false positives. Keep its PREFIX/SUFFIX/ARTICLE lists in sync with the C `TRIG_*` arrays.
- `tools/anima-host/entity-ctest.c` — compiles the verbatim C arrays + function and runs them (Madonna,
  Elvis Presley, Morgan Freeman, Pinco Pallino, Einstein/"famous for", Messi/"do" all extract correctly).

The full loop (the existing self-distilling tier): recognise phrasing → extract entity → slug →
**learned-cache hit (offline, instant)** OR **fetch Wikipedia/Wikidata + learn** → `cache_put` writes a
card to `/sd/data/anima/learned/<lang>.jsonl` (+ a recall-vector sidecar) → answered **offline forever
after**, including paraphrases via the shared encoder. So a question asked once online persists offline.

## Open work (device-side; not host-verifiable, needs hardware)

- **Static L1 arena** (Leva 2): kill load/unload thrashing + fragmentation at the root (RAM benefit is
  only measurable on the PSRAM-less chip).
- **Side-effecting agentic tools** (Leva 6): `write_note(content)`, `set_alarm`, `add_event` — need the
  httpd/app executor and a content channel (`anima_result_t.arg` is 64 B, too small for note bodies).
- **Online router unification** (Leva 4/5): one online-intent classifier instead of 7 sequential
  detectors; teacher LLM should also learn structured triples into `mind.<lang>.jsonl` so the offline
  reasoning tiers grow from use.

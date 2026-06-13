# ANIMA as a micro-agent — multi-RAG, RAM-managed, tool-using

Goal: evolve ANIMA from a single-index assistant into a **micro-agent** on the Cardputer
that (a) routes a query to the right capability, (b) **uses tools** (create a file, open an
app, report state), and (c) answers from **multiple knowledge domains** (general, basic
programming, NucleoOS dev) — all within ~512 KB SRAM, SD ≈ 1 MB/s, no PSRAM, offline.

This is the spec to **analyze → design → build**. It states where we deliberately do
*better* than the mobile/server state of the art, given the MCU constraints.

---

## 1. How the best do it (2024–2026)

| Technique | Source | The idea we borrow |
|---|---|---|
| **EcoVector / MobileRAG** | [arXiv 2507.01079](https://arxiv.org/abs/2507.01079) | Only **k-means centroids + minimal metadata in RAM**; per-cluster vectors on disk, **loaded on demand**. Energy-efficient. Trim retrieved content (SCR). |
| **DiskANN / LM-DiskANN** | [LM-DiskANN](https://cse.unl.edu/~yu/homepage/publications/paper/2023.LM-DiskANN-Low%20Memory%20Footprint%20in%20Disk-Native%20Dynamic%20Graph-Based%20ANN%20Indexing.pdf), [Milvus](https://milvus.io/blog/diskann-explained.md) | Full vectors on disk, **compressed (PQ) routing layer in RAM** (few bytes/vector); load only search-path data. 15–50× less RAM than HNSW at 95% recall. |
| **TinyAgent** | [arXiv 2409.00608](https://arxiv.org/pdf/2409.00608) | Task-specific small agents for **function calling at the edge**. Accuracy hinges on **argument correctness + strict schemas + validators**, not model size. |
| **FunctionGemma 270M / intent+model routers** | [SLM agents survey 2510.03847](https://arxiv.org/pdf/2510.03847) | An **intent router** maps the prompt to a tool/specialist; small models suffice with tight schemas. |
| **Multi-KB routers** (R1-Router, RAGRouter, RIRS) | [RAGRouter 2505.23052](https://arxiv.org/html/2505.23052v2) | Decide **when and where** to retrieve across disjoint corpora; coordinate specialist RAG agents. |

**The consensus pattern:** a cheap *router* decides intent → *tool* or *which knowledge
base* → retrieve from a **disk-resident** index with only a tiny *routing layer in RAM* →
trim → answer. Tools use **strict schemas + validation**.

---

## 2. Where we do BETTER (for a 512 KB MCU)

The mobile papers assume 4–12 GB RAM. We have **~326 KB free**. So we push the ideas to the
extreme and add the one lever they don't have: a **cascade that makes retrieval rare**.

1. **Cascade gating first (our edge).** Most real queries are commands/tools ("apri le
   foto", "crea un file") or live state — caught by L0 in <1 ms with **zero SD I/O and zero
   inference**. RAG (encoder + SD cluster read) runs only on the residual ~10–20%. On a
   120 mAh device, *not running* the heavy path is the biggest win — energy is a first-class
   signal (see `memory-budget.md`). The papers always retrieve; we mostly don't.
2. **Centroids-only RAM, clusters on SD (EcoVector taken to the limit).** Per domain we keep
   only ~16–64 int8 centroids in RAM (a few KB total across all domains). Full vectors +
   answers live on SD, grouped contiguously by cluster. A query loads **one cluster** (tens
   of KB) → corpus size is bounded by the 64 GB card, not by RAM. This removes today's
   ~hundreds-of-vectors RAM cap and lets the programming/knowledge packs hold thousands.
3. **Coarse-to-fine to minimize the slow SD.** Route by centroid (RAM, instant) → read only
   the winning cluster from SD → int8 rerank. One small SD read per answered question.
4. **Deterministic router in pure C.** No 270M model on-device: the router is L0 keywords +
   the distilled encoder’s centroid match. Strict tool schemas + C validators (TinyAgent’s
   real lesson) give reliable function calls without a generative model.

In one line: **a RAG that costs ~nothing until it must run, sized for a corpus 100× the
RAM, with tool-calling that never hallucinates arguments.**

---

## 3. Architecture

```
query
 ↓
ROUTER (orchestrator)
 ├─ L0 keyword/intent  ──► TOOL or COMMAND or SYSTEM        (cheap, exact, no SD)
 │        e.g. "crea un file note.txt" -> tool:create_file(path)
 ↓ (L0 miss)
 ├─ encode query (distilled encoder, flash/SD)             (one forward pass)
 ├─ centroid match -> pick DOMAIN  {general | programming | nucleo-os | commands}
 ├─ load that domain's nearest cluster from SD -> int8 rerank
 ↓
 ├─ confident -> answer / run tool
 └─ low conf -> honest "non lo so" (+ optional cloud if Wi-Fi)
```

### 3a. Tool layer (function calling)
A registry of typed tools. Each: `{name, schema, extract(), validate(), run()}`.
- `create_file(path)` — create an **empty text file** on the SD (atomic temp+rename via the
  fs layer). Slot extraction pulls the name; validator sanitizes the path + forces a safe
  extension/dir (`/data/...`). Refuses if no valid name (never invents one).
- `open_app(id)`, `system_info(key)` — already present, reframed as tools.
- Lesson applied: **bounded enums, typed args, minimal output, strict validation.**

### 3b. RAM-managed multi-RAG (EcoVector-lite)
- **In RAM:** per domain, centroids (k-means) `int8[K][D]` + a tiny cluster directory
  (offset, count per cluster). Few KB total. Plus a bounded **LRU cache** of recently read
  clusters/answers.
- **On SD:** clustered vectors `int8[N][D]` (contiguous by cluster) + an answer blob.
- **Search:** embed → nearest centroid(s) → `fseek`+read that cluster → rerank → answer.
  RAM is O(centroids), not O(corpus).

### 3c. Domains (RAG packs, built offline)
- `general` — capitals, facts, units (have a seed).
- `programming` — Italian: variabili, tipi, if/else, cicli (for/while), funzioni, array,
  stringhe, booleani, commenti, debug basics. Concept → short explanation + tiny example.
- `nucleo-os` — how NucleoOS works for builders: app manifest fields, intents/file
  associations, runtimes (web/vm/service), the automation micro-VM, SD layout, the event
  bus. Sourced from `docs/*.md` (already authored, authoritative).

---

## 4. Index format v2 (`AKB2`, SD-resident clustered)

```
magic "AKB2" | u32 D | u32 nDomains
per domain: u8 nameLen,name | u32 K (centroids) | u32 N (vectors) | u32 clusterDirOff | u32 vecOff | u32 ansOff
centroids:  int8[K][D]                (loaded to RAM at init)
clusterDir: K x { u32 firstVec, u32 count }   (loaded to RAM)
vectors:    int8[N][D]  grouped by cluster     (read per-cluster from SD)
answers:    N x { u8 action, cstr arg, cstr reply }  (read per-hit from SD)
```
The device loads only centroids + clusterDir (RAM); vectors/answers stream from SD.

---

## 5. Build plan

- **P1 — Tool: `create_file`** (small, high value, agentic). Router intent + slot + validate
  + fs write empty. Works on the current RAM index; no format change.
- **P2 — RAM manager: SD-resident clustered search** (`AKB2`). Centroids in RAM, clusters on
  SD. Offline builder does k-means; device search reads one cluster. Removes the RAM cap.
- **P3 — Domain packs**: build `programming` + `nucleo-os` (from `docs/*.md`) + grow
  `general`. Each clustered into `AKB2`.
- **P4 — Router**: domain selection via centroids; tool-vs-knowledge-vs-command decision;
  confidence gating per tier.

## 6. The agent loop — compose THEN act (BUILT)

The leap from a one-shot Q&A box to a **deterministic micro-agent**: a tool may run a few internal
reasoning *steps* before proposing its side effect, and every step is logged to a visible trace.
No generator — the plan is a fixed sequence the Cortex features select, executed in pure C.

```
"crea una nota con 12*8"
  step 1  plan: compose+write           (a create-file request carrying a content clause)
  step 2  compute 12*8=96               (ag_compose: the agent calling its math sub-skill)
  step 3  verify: ok                    (payload non-empty + fits -> deterministic self-check)
  -> ACT_TOOL create_file  arg=/data/Documents/nota.txt   content="96"
```

Pieces (all in `nucleo_anima.c`, host-tested by `tools/anima-host/agent-check.mjs`):
- **Content channel** (`nucleo_anima_tool_content()`): the payload bus that breaks the 64-byte
  `arg` limit. The controller stashes a composed body here; the EXECUTOR (httpd / native app) writes
  it. `""` = no payload -> empty file, the legacy behavior. Reset per query, one static buffer.
- **`a_content_clause()`**: finds the content in a create request — explicit connectors win
  (`con scritto X`, `con il testo X`, `che dice X`, `with the text X`, a literal `:`), bare `con` is
  content only when it isn't a naming clause (`con nome X` stays a filename).
- **`ag_compose()`**: turns a content sub-query into a payload — COMPUTES it if it's a calculation,
  else takes it literally. This is the reusable seam for richer steps (retrieve / live) later.
- **`tool_note`**: the flagship tool — splits filename | content, composes + self-verifies, proposes
  one `create_file` action with the payload. Returns 0 (no clause) so the plain create still runs.
- **Reasoning trace** (`result.trace`, `" > "` joined, ASCII for the device font): multi-step turns
  show the step log; single-tier answers synthesize a one-liner (`L1 knowledge > 2cl > 100%`). Both
  UIs render it — the native app shows it only for multi-step turns; the web JSON carries `trace`.
- **Device-settings tools** (`set_volume` / `set_brightness`): proposed-action tools kept executor-
  side, so `nucleo_anima` needs no `nucleo_app`/`nucleo_audio` dependency (no link cycle). Absolute
  `a 55` -> arg `55`; bare `alza`/`abbassa` -> arg `+10`/`-10` (relative, clamped by the executor).
- **`add_event`** (calendar reminder): `tool_event` parses WHEN (day offset: oggi/domani/dopodomani/
  "tra N giorni") + an optional TIME ("alle HH[:MM]", pm -> +12) + the TEXT, packs them on the content
  channel as `off=<d>;time=<HH:MM|>;text=<...>`, and proposes one action. The executor
  (`anima_apply_event` in `nucleo_httpd.c`, and the JS twin) resolves the offset against the RTC and
  appends to `/sd/system/config/calendar.json` (publishes `calendar.changed`). See `anima-online`.

### NLU widening + zero-false-positive comprehension
- Play/put-on verbs (`metti`/`riproduci`/`suona`/`ascolta`/`play`/`start`) launch the right app
  ("metti la musica/radio"); added a `radio` app alias. `A_MAX_KW` 20 -> 28. Create verbs and content
  connectors widened (`annota`/`appunta`/`segna`...; `dicendo`/`che recita`/`with the text`).
- **Question guard** (`a_has_qword`, mirrored in `a_event_trigger`): a how-to / capability QUESTION
  ("come si crea un file?", "posso aggiungere un evento?") is knowledge, not a command — it never
  misfires `create_file`/`add_event` (guard applies only when no explicit filename). Curated how-to
  cards (`knowledge/self-howto.jsonl`) answer these. ood-check stays at 0 false positives.
- **LENS C (Grok)**: a veto-only cross-verifier on borderline learned-store saves (see `anima-online`)
  — Grok can only block a wrong save, never force one, driving saved false positives toward zero.

## 7. Honest limits
- The distilled encoder is modest (~0.45 vs e5); bigger table / more data / e5-base teacher
  raise quality. Retrieval is approximate — the confidence gate must stay conservative.
- The agent composes **within one turn** (steps that ANIMA can run itself, then ONE side effect).
  True act→observe→act *across* the executor (write, read result back, decide again) is the next
  step: the `observe()` seam exists; what's missing is executor re-entrancy. Multi-hop *reasoning*
  over generated text still needs a generator we don't run on-device — out of scope by design.
- New steps beyond {math | literal} (retrieve / live) wait until those tiers are link-safe to call
  from inside a tool on the PSRAM-less heap.
- Every index/pack change needs a **reboot** (packs load at init) until we add hot-reload.

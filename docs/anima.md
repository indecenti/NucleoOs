# ANIMA — Adaptive Neural Italian (and English) Micro Assistant

On-device, **offline** natural-language assistant for NucleoOS on the M5Stack Cardputer
(ESP32-S3, **no PSRAM**, ~512 KB SRAM, microSD over SPI ≈ 1 MB/s). It understands typed
Italian (and later English) and either **executes an OS command** or **answers a factual
question** — without internet.

> Status: **Phase 0** (foundation + on-device benchmark). The heavy retrieval tiers are
> specified here but gated behind real measurements taken on hardware. Nothing in this
> design streams large model weights at runtime — see §6 for why that is non-negotiable.
>
> **This doc is the aspirational design. For the pipeline as actually implemented today**
> (Cortex front-end, evidential L1 gate, real cascade order, what's built vs deferred), see
> [`anima-cortex.md`](anima-cortex.md).

---

## 1. Where each part runs (read this first)

The single most important thing to understand: **inference runs on the ESP32, offline.
The big models run on a PC, once, as a build tool — like a compiler.**

| Component | Runs on | Needs internet? |
|---|---|---|
| Orchestrator cascade (L0–L3) | **ESP32-S3** | No |
| Tokenizer + query encoder | **ESP32-S3** (weights in flash) | No |
| Binary coarse search + int8 rerank | **ESP32-S3** (index/payload on SD, hot rows cached in SRAM) | No |
| Frozen answers + templates | **ESP32-S3** (text on microSD) | No |
| **→ the complete assistant at runtime** | **ESP32-S3, 100% offline** | **No** |
| Minerva-7B / e5 / BGE teachers, distillation, FAISS index build | **PC, one-time** (the "factory") | only to fetch models |
| L3 remote fallback | **Cloud** | Yes — **optional**, never required |

The factory (PC) produces `.anima` files; you copy them to the SD with `tools/deploy.ps1`.
After that the Cardputer works with no PC, no Wi-Fi, no cloud. The device **executes and
retrieves**; it does **not** train or generate from scratch — the intelligence was baked
offline and **frozen** onto the card.

---

## 2. The orchestrator: a cost-growing cascade (stop as soon as it's enough)

The orchestrator is a **training-free** decision cascade (~hundreds of lines of C, **no
model**). It runs the cheapest path that can answer, and only escalates when needed. This
is what keeps the device fast **and** battery-frugal (energy is a first-class resource in
NucleoOS — see `memory-budget.md`).

```
query
 ↓
L0  string/intent match over the command+FAQ table        ~<1 ms, 0 SD, 0 encoder
 ├─ high-confidence hit → run command / static answer  ─────────► DONE  (70–90% of use)
 ↓ miss
L1  encode (flash) + binary Hamming search + confidence gate
 ├─ margin top1–top2 HIGH → serve Minerva's frozen answer ──────► DONE
 ↓ MEDIUM
L2  int8 rerank from SD + MOSAICO span-stitch (only here is SD really read)
 ↓ LOW
L3  if Wi-Fi → cloud ; else honest "non lo so"  ───────────────► never hallucinate
```

Confidence is one signal set: **Hamming margin (top1 vs top2) + rerank score**
(CRAG-style quality gate). Three numbers → three branches. No reflection tokens, no
trained router.

### Tiering, honestly

| Tier | Value | Risk | Plan |
|---|---|---|---|
| **L0** commands/FAQ | Very high (covers most real use) | ~none | **Ship now** (Phase 0–1) |
| **L1** frozen answers (binary-MRL retrieval) | High | Medium (encoder latency — measured) | The real project |
| **L2** MOSAICO span-stitch (<1M realizer) | Uncertain (may be worse than a clean frozen answer) | Research-grade | **Deferred behind a measurement gate** |
| **L3** cloud | Convenience only | Breaks "offline" identity | Optional fallback |

L0 + L1 deliver ~90% of the perceived value. L2 is the "wow" demo, not the value. Do not
build L2 before L0/L1 prove themselves on hardware.

---

## 3. The retrieval engine (L1) — frozen brain on the SD

Pattern lineage (all peer-reviewed / production, not invented here):
- **kNN-LM** (ICLR): a small model + a large datastore can beat training on all the data.
- **RETRO-LI** (ECAI 2024): small-scale RAG with a frozen embedder, robust to *noisy*
  neighbor search — exactly our case (approximate ANN over SD).
- **Semantic caching** (2025): works, but a naive cache loses correctness → we add the
  confidence gate as a first-class safeguard.
- **Binary + Matryoshka embeddings** (mxbai-style): ~64-byte vectors keep ~96% quality;
  Hamming distance = XOR + popcount = trivial integer math, ideal for an MCU with no FPU
  pressure.

### Two-stage search

1. **Coarse (binary).** Truncated-Matryoshka binary vectors (8–16 B each). Distance =
   `popcount(a XOR b)`. A hot slice is cached in SRAM; the rest streams from SD.
2. **Rerank (int8).** For the few coarse survivors, read full int8 vectors + the answer
   text from SD and re-score. Read budget ≈ tens of KB per query (~30 ms at 1 MB/s).

### Two micro-techniques specific to this device (novel integration)

- **Matryoshka speculative prefetch.** The first 16 dims of an MRL embedding are a valid
  coarse vector. While core 0 finishes the full encode, core 1 already prefetches the
  likely SD clusters from the 16-dim prefix — overlapping compute and the slow SD.
- **Confidence-gated graceful degradation.** High → frozen answer; medium → stitch;
  low → honest refusal (+ optional cloud). The device never fabricates an answer it
  cannot support, and L2 attribution is free (NEST-style span copy).

---

## 4. Memory & flash budget (corrected for the real partition table)

`partition-table.md`: app slots are **2.75 MB**; the firmware image is ~1.06 MB; **all
content lives on the SD**. So the earlier "put the whole index in flash" idea is wrong:

- **Flash (app slot):** room for the firmware + a **small** embedded default (the L0
  command table, a tiny encoder ≤ ~1.5 MB if it fits, or a hashing/linear fallback).
  Do **not** embed a multi-MB vector index — it would bloat both OTA slots and the OTA
  download.
- **SD (64 GB):** the home of the binary index + int8 payload + frozen answers. Binary
  vectors are tiny, so millions fit; per-query reads stay in the tens of KB.
- **SRAM (~326 KB free under transport profile A, see `memory-budget.md`):** holds the
  tokenizer (~10 KB), the encode scratch, and an **LRU cache of hot binary rows**. The
  in-RAM coarse index is capped (e.g. ~30–50k vectors at 8–16 B); beyond that the coarse
  pass streams from SD. **Corpus size is a tunable**, bounded by SRAM + SD bandwidth.

ANIMA must not enlarge the shared SRAM budget beyond its own arena, must run on **core 1**,
and must yield to the UI. It is an app, not the center of the OS.

---

## 5. Bilingual (IT first, EN later), switchable from the app

One engine, two swappable things:

| | Italian | English |
|---|---|---|
| Tokenizer | **syllabic** (Italian is phonetically regular) | **char-trigram / BPE-lite** (syllabic fails for English) |
| Model pack | `it.anima` | `en.anima` |

Setting `assistant.lang` in `registry/settings.json` selects the pack
(`/sd/data/anima/<lang>.*`). Changing it reloads the pack — no reflash. Phase 0 ships IT
only, but the tokenizer is pluggable so EN is "add a file", not "rewrite the engine".

---

## 6. What we will NOT do (anti-patterns)

The failure mode of this whole project is **one token per minute**. It comes from exactly
one thing: autoregressive inference that **re-reads all weights per token** from slow
storage. Minerva-1B int4 ≈ 0.5 GB ÷ 1 MB/s ≈ **8 minutes per token**. So:

1. **Never run / stream a generative LLM at runtime.** We retrieve and copy; we do not
   generate token-by-token from large weights. Worst realistic case is one encoder
   forward-pass (~0.3–1 s), once per query — not per token.
2. **Never run the encoder on every keystroke/query.** Gate it behind L0. (Battery.)
3. **Never let the index grow into the OTA/app flash.** Index lives on SD; bound any
   embedded blob.
4. **Never quantize to 1-bit blindly.** Measure binary-MRL recall on Italian first; fall
   back to int8 coarse if recall drops.
5. **Never serve a cached answer for live data** (time, battery, files) — those always go
   through L0/templates computed on the spot.
6. **Never hand-roll the offline vector DB.** Build the index with FAISS; the runtime side
   (XOR/popcount) is the only hand-written part.
7. **Never block the UI.** Core 1, cooperative, bounded arenas.

---

## 7. The offline factory (PC) — `tools/anima/`

Produces the `.anima` packs. None of this runs on the device.

1. **Intents (L0).** Author intents + example phrasings; expand paraphrases with a large
   Italian teacher (Minerva-7B-instruct). Output: the command table.
2. **Encoder (L1).** Distill a small int8 student (~3–5 M, or a linear/hashing fallback)
   from `multilingual-e5-large` / `BGE-M3` on Italian.
3. **Knowledge + frozen answers.** Minerva-7B writes/validates answers for a large query
   space (the "verifier moved offline" trick); embed passages with the teacher.
4. **Index.** Build FAISS IVF + binary-MRL + int8 payload → `it.anima` (+ `commands.it.json`).
5. **Deploy.** `tools/deploy.ps1 -To H:\` syncs the packs to the SD under `/data/anima/`.

Data sources & licenses (keep the MIT engine separate from CC-BY-SA data packs):
Tatoeba (CC-BY 2.0 FR), Wikipedia IT (CC-BY-SA), Morph-it!/Wiktionary IT (CC-BY-SA).
PAISÀ is CC-BY-**NC** → usable for training, **not** redistributable in a pack. De Mauro
is proprietary → **excluded** (the original idea was wrong to call it public domain).

---

## 8. Phase 0 — foundation + the three numbers that de-risk everything

Phase 0 ships the guaranteed-value core and measures the unknowns **before** we commit to
the heavy tiers.

**Build (this commit):**
- `nucleo_anima` component: the **L0 orchestrator** (normalize → tokenize → intent match →
  confidence gate → action), pure C, no SD/model dependency, with an embedded default IT
  command table. Maps commands to the existing `app.launch` / system intents.
- `nucleo_anima_benchmark()`: an on-device micro-benchmark (opt-in via Kconfig) that
  measures the three numbers and logs derived latency estimates.

**The three numbers (measured on the real Cardputer, via `idf.py -p COM3 monitor`):**
1. **int8 MAC throughput** → estimates the encoder forward-pass time (bounds L1 latency).
2. **Hamming/popcount throughput** → how many binary vectors we can scan in ~50 ms
   (bounds the coarse search corpus size).
3. **SD sequential read MB/s** → time to read a rerank cluster (bounds L1/L2 SD cost).

If those numbers hold (encoder ≤ ~1 s, popcount ≫ corpus, SD cluster ≪ 100 ms), L1 is
confirmed feasible and we proceed to the offline factory. If the encoder is too slow, we
fall back to the linear/hashing embedder (<50 ms, lower recall) — in no case do we hit the
token-per-minute regime.

**To run the benchmark:** enable `CONFIG_NUCLEO_ANIMA_BENCH=y`
(`idf.py menuconfig` → *NucleoOS ANIMA*), flash, then `idf.py -p COM3 monitor`.

---

## 9. Roadmap

- **Phase 0** — L0 orchestrator + benchmark (this commit). Measure the three numbers.
- **Phase 1** — Encoder student in flash; `nucleo_anima` app in the launcher (keyboard UI,
  Wear-OS style); wire L0 actions to `nucleo_app` open-by-id + `/api/status` live values.
- **Phase 2** — Offline factory: distillation + Minerva answer generation + FAISS index.
- **Phase 3** — L1 retrieval on device: binary search + int8 rerank + frozen answers +
  confidence gate + Matryoshka speculative prefetch.
- **Phase 4** — HW boost (verify SDMMC vs SPI for 4–8× SD bandwidth) + EN pack.
- **Phase 5 (optional)** — L2 MOSAICO span-stitch, only if frozen answers feel too rigid.

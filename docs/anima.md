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

### The abstention is an answer too

When the cascade falls through, the result is `tier = NONE` — and for a long time it was also an
**empty reply string**. That looked harmless because the two visible clients each papered over it:
the web shell substituted its own localized "I don't know yet, here's what I *can* do", and the
native bubble had a fallback literal. The clients that had no fallback simply produced nothing —
the **voice** path spoke silence, and the host CLI printed `(vuoto)`. One engine, four behaviours,
and the two broken ones invisible to every gate.

The refusal now belongs to the engine, filled once at the cascade's convergence point. Two
properties make this safe rather than a hallucination risk, and both are pinned by
`npm run anima:decline` (`tools/anima-host/decline-check.mjs`, 485 adversarial turns):

- **The tier does not move.** `NONE` stays `NONE`. Every abstention and hallucination gate in this
  repo keys on the *tier*, never on the wording, so giving the refusal words cannot make it read as
  an answer to anything that measures us.
- **It is narrow.** It fills only an empty reply on a `NONE`/no-action turn that isn't awaiting a
  follow-up — so a real answer is never replaced, and a pending *clarifying question* keeps its own
  text (a question is not an abstention).

The web shell still prefers its own string, matched on the exact template: it is richer (it lists
what ANIMA can do) and it covers es/fr/de, which the it/en engine cannot.

### MOSAICO must not say the same thing twice

The L2 span-stitch builds a fuller answer by appending verbatim card spans. Its duplicate guard used
to compare only the span's first ~40 characters, so a drill-down that *restated* the lead in
different words sailed through — `cos'è nucleoos` answered with a lead saying NucleoOS runs on the
Cardputer and works offline, then stapled on "Inoltre, NucleoOS gira sul M5Stack Cardputer (ESP32-S3)
e funziona offline."

The guard is now sentence-level: a candidate sentence is dropped when ≥80% of its content words are
already present, and kept sentences are folded back in so a span can't duplicate itself either.
Sentences are only ever *omitted*, never rewritten, so MOSAICO's "every sentence is a verbatim corpus
field" invariant is untouched. Note the honest consequence: when the only candidate span was
redundant, the answer stays at **L1** instead of being promoted to L2 — a lower tier for a better
answer. `npm run anima:dedup` holds it, with a unit half (the real helpers, driven with the shapes
that caused the defect) and a sweep half (the invariant over the whole describe fixture), because
whether the defect *fires* end-to-end depends on which two cards a given index happens to pair.

...and it must not answer in two languages at once. `what is force` returned an Italian lead and then
stapled an English runner-up onto it with " Also, " — two individually correct cards, one unreadable
paragraph. A span is now rejected when a cheap function-word vote puts it in a **different** language
from the lead. The vote is deliberately permissive: an undecidable span (too short to classify) is
allowed through, so the guard can only ever remove a genuinely bilingual answer.

That guard treats the symptom, and the cause is worth stating plainly: `what is force` should not
have retrieved an *Italian* lead for an *English* query in the first place. That is a corpus/index
issue — an IT card ranking first inside the EN pack — and fixing it means changing retrieval ranking,
which is not something to do on top of an index that is already stale. Filed, not silently papered
over.

### Seven fabrications, and what they had in common

The 0-hallucination discipline is enforced by ~590 adversarial traps. Seven were getting through, and
they turned out to be four instances of one idea: **a true sentence is not an answer**. Each fix is a
guard that asks whether the card addresses the *question*, not merely whether it scores well.

| Trap | What came back | Why it slipped |
|---|---|---|
| `qual è il lago più grande dell'Africa` (×3 shapes) | *"intendi 1) Il deserto caldo più grande…"* | The **clarify band** checked scores, never scope. Offering two options asserts that one is right, so a nonsense option is a fabrication with extra steps. It now runs candidates through the same scope guard the answer path uses, plus the question's qualifier. |
| `che cos'è il linguaggio di programmazione Floonk` | the generic *"programming language"* card | The proper-noun guard was reading a **lowercased** query, so it couldn't see `Floonk` was a name and fell back to "content word of 7+ chars" — which a 6-letter invented name walks past. `Floonkium` correctly abstained; `Floonk` did not. L1 now gets the query as typed. |
| `qual è la capitale di Tokyo` | *"La capitale di Giappone è Tokyo."* | A **false premise** answered by inverting the relation: Tokyo *is* a capital, it does not have one. The card's subject must be the entity asked about. |
| `when did cristiano ronaldo die` | his (living) biography | The death-question guard matched `died`/`death` but not **`die`**. Both sides of the guard learned the missing forms, so a card genuinely about dying — or about a semiconductor *die* — still passes. |

Two test expectations had been calibrated to the defects and had to move with them: `agent-check`
pinned `cos'e nucleoos` as a MOSAICO stitch whose only span was the duplicate, and `eval_skills5`
expected an *answer* to `when did the Pacific Ocean die` — which was *"The largest ocean is the
Pacific."*, a true sentence about size offered as an answer about death. Every structurally identical
neighbour in that file expects abstain, and so does its own header.

Worth recording honestly: the clarify band fired **zero** times across ~835 ordinary queries both
before and after the guard — the only firings anywhere were the four wrong ones. As calibrated
(`[0.82, 0.85)` with a 0.08 margin) the band is effectively dormant on the current index. The guard
cost nothing because there was nothing good to lose, which is also a reason to revisit whether the
feature earns its keep.

### Which person? — asking from an exact fact instead of a score

That revisit produced a second clarify, and the interesting part is *why the first one could not do
this job*. Ten people in the corpus are called Trump, four Kennedy, four King — 84 shared surnames
over 210 people. Asked a bare surname, retrieval answered with whichever namesake ranked highest and
stated it as fact: `chi è Trump` returned **Frederick Christ Trump Sr.** at 82%, `chi è Kennedy`
returned **Ethel** at 80%. Every card was real. That is what made it worse than a miss — a coin flip
wearing the costume of a verified answer, and invisible to every hallucination gate, which all key on
whether the *card* is genuine.

The dialogic band cannot reach these: a bare surname scores 0.55–0.72, well below its `[0.82, 0.85)`
window. And widening the window would not help, because **cosine margin is the wrong instrument
here** — `chi è Kennedy` beats its runner-up by 0.186, looking confident while being the wrong
Kennedy. The question "how many people carry this surname?" has an *exact* answer, so it is answered
at build time (`tools/anima/build_person_ambig.mjs` → `anima_person_ambig.h`) and never estimated at
runtime.

Design notes that matter more than the feature:

* **Flash, not SD, and no offsets.** The table lives in flash rodata: zero heap on a device whose
  scarce resource is RAM, and — because it stores names rather than answer offsets — it *cannot* fall
  out of sync with a rebuilt index. A pick is resolved by **re-asking with the full name** (which
  retrieves at 100%), so the answer always arrives from a real card through the ordinary cascade. A
  clarify therefore cannot assert anything the corpus does not contain, by construction rather than by
  care. `npm run anima:person:check` gates the table against the corpus hash.
* **Asking less is part of asking well.** A full name still answers, and so does a surname only one
  person carries. The rule is "among the candidates whose name the query spells completely, the most
  specific wins", which is why the generational suffix and the middle initial are kept: `Jr.` and
  `B.` are the entire difference between two people, and discarding them turns an answerable question
  into a pointless one. A surname that is also a non-person card's subject (`curry`, `richardson`) is
  excluded — that ambiguity is across categories and belongs to the normal cascade.
* **The same fact, used the other way.** A surname only one person carries does not need a question,
  it needs the full name. `who was einstein` scored 0.682 against a corpus that only ever phrases that
  card with the full name — just under the 0.72 rescue floor, while Italian squeaked through at 0.722.
  That looked like a language defect and was really an under-specified query. The retry runs *only
  after the cascade has decided to refuse*, so it can convert an "I don't know" into a card but can
  never displace a correct answer. It closed the `decline` and `working memory` gates.

### The premise guard was right, it just wasn't running

`quando ha twittato Cristoforo Colombo` answered *"La scoperta dell'America avvenne nel 1492…"* — a
true sentence about a real person, offered as an answer about tweeting. The guard for exactly this
already existed and even names the case in its own comment: `l1_premise_covered` checks a closed list
of **premise markers** (`twittato`, `followers`, `email`, `valuta`, `lingua`, `popolazione`, …) and
abstains when the query says one that the matched card never mentions. It was gated to the sharded
path, on the assumption that flat's competition gate covered the same ground. It did not: the query
scored 0.770 with a 0.340 margin and cleared every rescue guard on the strength of the **entity
alone** — `l1_rescue_on_topic` declares a match on-topic as soon as *any* content word appears in the
reply, and "cristoforo" did.

The marker check is now split out (`l1_marker_covered`) and runs on both paths. Only that half: the
capital-lookup half reads a typo'd article as the country name (`capitale dela francia` → abstain),
which flat reaches through the spellfix rescue and the sharded path does not — so it stays AKB5-only.
Measured, that split is the difference between fixing one gate and breaking two.

Two bugs the which-person work found in itself, worth keeping in mind for anything that offers
numbered options:
when narrowing removes candidates, **option 2 is not group member 2** (`chi è Arthur Guinness` narrows
five Guinnesses to two, sitting at positions 2 and 3 — deriving the pick from its ordinal answered
about "Guinness family"); and a chosen name that is *itself* shared will re-ask the same question
forever unless the clarify is suppressed for the resolving turn. `npm run anima:whichperson` pins
both, plus the property that every offered name is independently answerable.

### The corpus reaches the screen unedited

ANIMA relays corpus fields verbatim — that *is* the 0-hallucination contract — so a defect in the
corpus is a defect in the product, and no retrieval gate can catch it, because the text is valid.
Two were shipping:

- **Double-encoded UTF-8** — `una percentuale Ã¨ un numero`, where the card means `è`. A source that
  was already UTF-8 got decoded as Latin-1 and re-encoded. It survives every filter because the
  result *is* valid Latin text.
- **Wikipedia math residue** — `{\displaystyle {\frac {x}{100}}}` left by the extractor. It reads as
  noise, and its braces trip the leaked-`{template}` guard the hallucination probes depend on.

Both are repaired in two places, on purpose. `npm run anima:corpustext` fixes them **at the source**
(29 double-encoded fields and 82 TeX blocks across 9 corpus files) and, with `--check`, guards the
corpus in the gate suite. `a_strip_foreign` also repairs both **at output time**, so an index built
before the lint — or a card learned online later — still displays clean without waiting for a rebuild.
The double-encoding repair is exact and reversible (`C3 83` + `C2 yy` → `C3 yy`), not a guess.

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

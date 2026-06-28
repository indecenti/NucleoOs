# ANIMA — Scaling the offline knowledge brain to 50–60 GB (certain, bilingual, categorized)

> Durable plan. Goal: grow ANIMA's offline knowledge from ~2k cards to **50–60 GB of
> CERTAIN, correctly-categorized IT+EN knowledge**, importable largely automatically
> (download + adapter scripts), with a self-learning loop, while preserving the two
> invariants that make ANIMA special: **0 hallucination** and **RAM = O(domains×centroids),
> never O(corpus)**.
>
> This supersedes ad-hoc chat notes. It builds on `anima-roadmap.md` (north star) and
> `anima-knowledge-graph.md` §3 (the SD-index design, already specified but not built).

## 0. Verdict in one line

The hard architectural thinking is **already done** — the O(K)-RAM streaming index, the AKB4
asymmetric prefilter, the VKL v2 provenance ledger, the on-device P279 closure, and a fully
specified SD-resident sharded-index design (`anima-knowledge-graph.md` §3) are world-class for
this hardware. What's missing is **execution at scale**: (a) the bulk certain-content +
automatic import pipeline, (b) breaking the flat-K retrieval wall with hierarchical sharded
indexes, (c) replacing linear JSONL scans for facts/KGE with on-SD keyed indexes, (d) closing
the device→PC learning loop, (e) contradiction detection + calibrated certainty. Plus **two live
deploy bugs** to fix first.

## 1. Hardware truth (every number below is load-bearing)

| Constraint | Value | Source |
|---|---|---|
| SRAM total / runtime free heap / largest free block | 512 KB / ~18 KB / ~31 KB, **no PSRAM** | `memory-budget.md`, `build_akb2.py:88` |
| SD bus | **SPI3, dedicated** (not shared with display SPI2), 1-bit @15 MHz | `nucleo_storage.c:46`, `storage.md` |
| SD throughput | ~1 MB/s sequential; **scattered reads far slower** (16 KB FAT cluster → every 196 B vector read can fault a 16 KB cluster) | `nucleo_storage.c:72` |
| Filesystem | **exFAT supported** (if enabled in IDF) → **no 4 GB/file limit**; FAT32 fallback caps files at 4 GB | `storage.md:12-13` |
| `/api/fs/read` | supports HTTP **ranged 206**; sustained reads reset ~a few MB → big assets pre-split ≤7 MiB | `nucleo_fsapi.c:191`, Vosk split |
| L1 engine SD access | **raw FATFS fopen/fseek/fread**, NOT `/api/fs/read` | `nucleo_anima_l1.c:259` |
| Flash | 8 MB, **full** (2×OTA 3.5 + coredump/cfg); no `anima_enc`/`anima_brain` partition exists yet | `partition-table.md` |

**Per-query budgets to defend:** keep 70–90 % of queries on L0 (0 SD reads); an L1 hit must stay
**< ~1 s** end-to-end; **SD reads per L1 query should stay in the low tens of KB** as a few
*contiguous* ranges — never thousands of scattered 196 B seeks.

**Card sizing:** 50–60 GB is firmly inside the device's intended envelope (the reference volume
record is a 64 GB exFAT card). 128 GB also works if formatted FAT32/exFAT. The SD capacity is
**never** the binding constraint — query latency and scattered reads are.

## 2. What is already excellent — do NOT rebuild

- **O(K)-RAM streaming index** (`build_akb2.py:86`, `nucleo_anima_l1.c:266`): only centroids +
  directory + norms are resident (~18 KB); vectors/answers stream from SD. The right idea.
- **AKB4 asymmetric "holographic" prefilter** (`nucleo_anima_l1.c:706`): int8-query × 1-bit-DB
  sign score, 8× smaller reads, M shrinks 64→16 at *higher* retention (77 %→92 %). Independently
  confirmed correct by sqlite-vec / FAISS binary-quant practice — ANIMA is ahead of the field here.
- **Zero-hallucination, enforced at many independent layers**: 0.85 cosine gate + evidential
  rescue band + scope/named-entity/date/count coverage guards + KGE coherence gate + facet
  SHA-256 integrity + MOSAICO verbatim-span-only.
- **VKL v2 ledger** (`ledger.mjs`): HMAC hash-chain + **off-SD anchor** (defeats forge-then-reseal),
  grammar-validated + wd_cache-resolved provenance, proof-carrying answers.
- **P279 taxonomy closure DEDUCED on-device** (`nucleo_anima_hdc.c:559`): transitive facts are
  computed by hypervector rotation at query time, never stored — taxonomy answers facts never written.
- **Energy-first cascade** (`nucleo_anima.c:2318`) and **aggressive RAM reclaim** (full L1 unload
  before every heavy phase / TLS handshake).
- **Browser models offloaded to the client GPU** (Forge: WebLLM/Qwen, Vosk): the ESP32 is a
  bounded-Range file server, never a runtime inference engine for big weights. Keep it that way.

## 3. The real walls at 50–60 GB (with numbers)

1. **Flat K-capped IVF → latency explodes.** K = `min(256, 18000/D, N/22)` ⇒ K≈70 (D=256) /
   ≤93 (D=192), **bounded by the single contiguous centroid malloc** vs the ~31 KB free block
   (`build_akb2.py:90`). K can't grow with N, so vectors/cluster grows linearly. Pass-1 signature
   scan = `nprobe·(N/K)·(D/8)`: ~0 s today → **~1.2 s at 50k cards → ~24 s at 1 M cards**.
   Interactive use dies around **50–100k cards**, long before the SD fills.
2. **Linear full-file JSONL scans for facts/KGE.** `facet_lookup` scans the whole facets file;
   `kg_load_subgraph` scans the whole `mind` file **4+ times per query**. `KG_FILESCAN=4096`
   (`nucleo_anima_hdc.c:161`) is a **silent truncation**: edges past line 4096 become invisible →
   wrong/missing deductions, not an error. Already at 1522 lines (~2.7× headroom). At GB-scale =
   minutes per fact question.
3. **Single monolithic index, full rebuild per change.** One `anima-it-index.bin`; every promotion
   re-clusters all vectors (sklearn KMeans in host RAM) and reflashes. No incremental/append build.
4. **No automatic categorization.** `category` = the seed list / file a card was hand-authored into
   (`ingest_wiki.mjs:228`). Bulk-importing millions of facts has **no classifier** — the chief
   blocker to scale.
5. **Device→PC learning loop is broken.** `user.tsv`/`user.vec` user-taught facts have **no path
   back** to the corpus; `sd-net-sync.mjs` actively *protects/excludes* them. Device learning never
   compounds (capped at 128, FIFO-evicted).
6. **No contradiction detection; binary (uncalibrated) certainty.** `append()` dedups identical
   `(s,rel,o)` but cannot detect two certain sources asserting conflicting `o` (e.g. two birth
   dates); the device picks the first edge found — non-deterministically wrong. Wikidata facts are
   hardcoded `conf=100`; no per-source reliability, decay, or calibration.
7. **~30 % of cards are fake-bilingual** — 609 wiki-* cards are IT-only with EN mirrored from IT
   (`anima_lib.py:113`). An EN query that hits one gets an Italian answer.
8. **Two live deploy bugs (fix first, Phase 0):**
   - **Dim mismatch:** shipped encoder is **D=192**, shipped index is **D=256** →
     `load_index()` rejects on `d != s_D` (`nucleo_anima_l1.c:265`) → **L1 silently disabled**.
   - **Prefilter inactive:** shipped index footer is `stay`, not `ASIG` → the AKB4 fast path is
     **off** → brute-force exact cosine over all probed vectors (~290 KB scattered/query).

## 4. The architecture (five moves)

### Move 1 — Hierarchical, sharded, PQ-compressed index (`AKB5`)
The flat single-level IVF is the wall. Replace with three nested levels, each sized to a budget:

- **Shard router (RAM, tiny).** Shard the corpus **by category/domain** (Move 4 gives every card a
  category). A small RAM table of *shard centroids* (one or a few per shard, a few hundred total →
  a few KB) routes a query to **1–2 shards**. This is `anima-knowledge-graph.md` §3's
  "shard by type" + the roadmap's promised "hierarchical routing".
- **Coarse index (SD, per shard).** Within a shard, a 2-level IVF whose **coarse centroids live on
  SD**, not RAM — so cluster count grows with shard size (K ≈ √N per shard) without touching the
  18 KB arena. One contiguous read pulls the coarse table for the chosen shard.
- **Vectors: Product Quantization.** Replace the 192–256 B int8 vector with a **PQ code (32
  sub-quantizers × 4-bit = 16 B/vector)** + the existing AKB4 1-bit signature. ~12–16× smaller →
  ~12–16× fewer SD bytes read. Survivors get exact-cosine rerank (the rerank stage already exists).
  Source: FAISS IVF-PQ; confirmed to beat HNSW on memory/latency, and HNSW's pointer-chasing is
  fatal on 1 MB/s SD — **never HNSW**.
- **Result:** per query touches **one shard**, reads a coarse table + a handful of PQ codes + ~16
  full vectors = a few contiguous KB. Bounded reads at any corpus size. Files stay <4 GB by
  sharding (works on FAT32; exFAT removes the limit entirely).

### Move 2 — On-SD keyed fact/KG store (kill the linear scans)
Replace `facet_lookup` / `kg_load_subgraph` full-file `fgets` scans with the **already-specified**
`anima-knowledge-graph.md` §3 structures:
- **Front-coded sorted dictionary** `subject-slug → offset` (16 keys/block + 2-level directory,
  ≤1 KB RAM top-index) → **O(log n) ≈ ~5 SD reads** instead of a whole-file scan.
- **StreamVByte posting lists** (delta + skip-block every 128 ids) for set/filter queries
  ("quali fisici tedeschi") — galloping `seek_ge` is one block read, intersection in ~1–1.5 KB RAM.
- **Dense positional temporal index** for timeline queries ("eventi del '900") — one contiguous read.
- **512 B-aligned sector LRU** (8–16 slots) absorbs directory-probe read storms.
- Removes the `KG_FILESCAN=4096` silent-truncation hazard entirely (lookups are seeks, not scans).
- Integrity: replace whole-file SHA-256 (`facet.c:341`, minutes at GB-scale) with **per-shard
  Merkle roots** anchored in the firmware manifest — verify only the shard you touch.

### Move 3 — Encoder upgrade (quality + streamability)
Distill the existing multilingual-e5 teacher into a **Model2Vec-style static token-embedding table**
(mean-pool of int8 token vectors; no transformer/matmul at runtime). It's strictly friendlier to an
18 KB-heap MCU and fully SD-streamable. Gate the swap against the existing `l1-recall` / `route-check`
suites before adoption (static embeddings drop a few points; verify, don't assume). Optionally carve a
flash `anima_enc` mmap partition for zero-I/O encoding — **requires repartitioning** (flash is full;
shrink factory or drop one OTA slot), so this is a later, hardware-verified step.

### Move 4 — Bulk import pipeline with automatic categorization (the scalability ask)
A repeatable `import_*.mjs` per source: **download → adapt → card/triple with category +
provenance + certainty → gate → shard-build**. Answers are **always copied verbatim** from the
source; an LLM may only generate question paraphrases (the existing zero-hallucination contract,
applied uniformly). See §5 for the datasets and §6 for the scripts.

**Automatic categorization (this is what "correctly categorized" requires):** derive each item's
category from **Wikidata P31/P279** mapped through the existing `refdata/taxonomy.json` controlled
vocabulary. Wikipedia/Wiktionary items inherit the category of their linked QID's P31; the bilingual
label comes from the QID (exact in IT+EN, never local regex). Items with no QID fall back to a small
deterministic classifier (source-section → domain). Categories are **certain by construction**, not
guessed.

### Move 5 — Close + scale the self-learning flywheel
- **Close the device→PC link:** add a `pull` mode to sync that *reads* `user.tsv`/`user.vec` and
  `learned-forge.jsonl` from the device into a host staging area (today they're protected/excluded).
  Then they flow through the existing `promote-learned.mjs` gate into the corpus.
- **Contradiction detection at ingest:** for each `(subject, relation)`, detect conflicting `object`
  values across sources; resolve by **source-reliability weight × recency**, keep the winner, record
  the loser in the ledger as superseded (auditable). Wikidata "truthy" (best-rank) already minimizes
  this for its own facts; the guard matters when *mixing* sources.
- **Calibrated certainty:** per-source reliability priors (CC0-curated > Wikidata truthy >
  ConceptNet-gated), surfaced to the user and used by the gate — replace the binary `conf≥70`.
- **Device on-demand self-distill** (roadmap Phase 6): on a miss when online, fetch one entity's
  P106+P279, gate by certainty, append to the NVS-anchored ledger.

## 5. The 50 GB content plan (certain, IT+EN, license-clean)

Take **curated subsets** (certain by construction), not full dumps. Two partitions by license.

| Dataset | Size (after filter) | License | Role | Tier |
|---|---|---|---|---|
| **Wikidata truthy** (`latest-truthy.nt.bz2`) | ~10–20 GB | **CC0** | Fact/entity spine + taxonomy (P31/P279/P106/P569/P570/P36/P625) + IT/EN labels | **Primary (CC0)** |
| **Wikipedia 'mini' ZIM IT+EN** | ~16 GB | CC-BY-SA | Definition cards (intro+infobox ≈ ANIMA's card shape 1:1) | Share-alike |
| **Simple-English nopic ZIM** | ~1 GB | CC-BY-SA | High-clarity EN tier | Share-alike |
| **Wiktionary** (kaikki IT+EN) | ~3.4 GB | CC-BY-SA | Word definitions + **translator dict** | Share-alike |
| **GeoNames allCountries** | ~1.5 GB | **CC-BY** | Geography (cities/capitals/pop/coords, IT/EN exonyms) | Primary |
| **ConceptNet 5.7** (it/en slice) | ~1 GB | CC-BY-SA | Common-sense triples (gated by edge weight + relation whitelist) | Share-alike |
| **Open Multilingual WordNet + PWN** | ~0.1 GB | BSD-like / permissive | IT↔EN synonyms + hypernym taxonomy | Primary |
| **NIST CODATA constants 2022** | ~50 KB | Public domain | Physics constants (certain by definition) | Primary |
| **(optional) Wikivoyage ZIM IT+EN** | ~1.5 GB | CC-BY-SA | Descriptive place context | Share-alike |
| Vector + lexical indexes (AKB5 + dict/postings) | a few GB | — | derived | — |

**Total ≈ 35–45 GB**, leaving headroom inside 50 (and 60). **Do NOT** ship full Wikipedia *nopic*
(EN nopic alone ≈ 48 GB — eats the whole budget). **Avoid** the OSM planet (86 GB, ODbL,
needs Nominatim+SSD) — GeoNames gives all the geography ANIMA can answer for ~50× less.
**Avoid** raw Project Gutenberg prose as a *fact* source (hallucination risk); reserve a hand-picked
IT+EN reference slice only for a future Sentence-Bank/L2 tier.

**License hygiene = ship-ability:** keep CC0/CC-BY/PD as the **primary grounded layer**; keep
CC-BY-SA sources in a clearly-attributed, share-alike-tagged partition so distribution stays compliant.

**Caveat (verified):** Wikipedia abstract XML dumps were discontinued (Feb 2025) and Enterprise HTML
mirrors stopped (Mar 2025) — do **not** design around `*-latest-abstract.xml.gz`. Use the 'mini' ZIM
intros or DBpedia abstracts instead.

## 6. Scripts to write (all host-side, gated, idempotent)

| Script | Purpose |
|---|---|
| `tools/anima/import_wikidata.mjs` | Stream `latest-truthy.nt.bz2`, filter to it/en labels+descriptions + whitelisted P-properties → `wd.*` cards + `mind/facets` triples + ledger entries (CC0). |
| `tools/anima/import_zim.mjs` | Host `zimdump`/libzim over the IT+EN 'mini' ZIMs → strip to lead paragraph + infobox → definition cards; category from linked QID P31. |
| `tools/anima/import_wiktionary.mjs` | kaikki IT/EN JSONL → definition cards (word→gloss/POS) + feed the `dict-*.tsv` translator. |
| `tools/anima/import_geonames.mjs` | `allCountries.zip` + `alternateNames` → geo cards (capital/located-in/population/coords), IT/EN names. |
| `tools/anima/import_conceptnet.mjs` | it/en high-weight edges on a relation whitelist (IsA/UsedFor/PartOf/AtLocation) → `mind.jsonl` triples. |
| `tools/anima/import_wordnet.mjs` | OMW+PWN synsets → gloss cards + hypernym triples + IT↔EN synonyms via shared synset id. |
| `tools/anima/import_codata.mjs` | `allascii.txt` → constant cards. |
| `tools/anima/categorize.mjs` | QID→P31→`taxonomy.json` bucket (bilingual); deterministic fallback classifier; the single categorization authority for all importers. |
| `tools/anima/build_akb5.py` | Sharded + 2-level-IVF + PQ index builder (supersedes `build_akb2.py` at scale); per-shard files, shard-router table, PQ codebooks, AKB4 signatures, Merkle roots. **Incremental/append** per shard. |
| `tools/anima/build_factstore.mjs` | Front-coded subject-slug dictionary + StreamVByte posting lists + temporal index (`index-*.bin`). |
| `tools/anima/conflict_resolve.mjs` | Per-`(s,rel)` contradiction detection + source-reliability/recency resolution; emits superseded-edge ledger records. |
| `tools/sd-net-sync.mjs` (extend) | Add a **`pull`** mode for `user.tsv`/`user.vec`/`learned-forge.jsonl` device→PC (close the loop). |

New firmware work (host-first, flash only to confirm): `AKB5` loader + shard router + PQ decode in
`nucleo_anima_l1.c`; on-SD keyed fact store reads in `nucleo_anima_facet.c` / `nucleo_anima_hdc.c`
(replace linear scans); per-shard Merkle verify.

## 7. Patterns stolen from the best projects (architecture, not runtime)

- **Kiwix/ZIM cluster-directory read** → group many small cards into a ZSTD cluster + offset
  directory; one seek + one decompress instead of N scattered reads. Directly attacks the
  scattered-read bottleneck. (Don't run libzim on the MCU — reimplement the read pattern in AKB5.)
- **FAISS IVF-PQ** → the PQ compression half ANIMA hasn't used yet (Move 1).
- **Model2Vec** → static-embedding encoder (Move 3).
- **txtai / sqlite-vec** → "compute the index on a real machine, ship a static index to the edge"
  (ANIMA already does this) + hybrid lexical+dense fusion (ANIMA's dual-channel gate, validated at
  scale; consider an on-disk BM25/FTS5-style posting list as the lexical channel beyond ~36k vectors).
- **DBpedia mappingbased facts** → typed facts feed for the KGE/facet store (CC-BY-SA, build-time).

## 8. Execution sequence (each phase host-first, gated; flash only to confirm)

- **Phase 0 — Fix the live pack. ✅ DONE (2026-06-07).** The device trees (`deploy/sd`, `sd-safe`,
  `tools/sd-sim`) were found already coherent (encoder D=192 + index D=192 + `ASIG`) — rebuilt earlier
  that day, so the two audited device bugs were already gone. Remaining: the **host** harness index
  (`tools/anima-host/sd`, D=256) lacked `ASIG`, making the `l1-parity`/`l1-recall` prefilter gates pass
  **vacuously** (exact-vs-exact) → augmented it (`augment_akb4.py`, append-only/answer-preserving).
  Added a durable guard **`tools/anima/check_pack.mjs`** (encoder.dim == index.D across every SD tree +
  valid ASIG) wired as the **first gate** in `npm run anima:gate`. Verified: all **31 gates PASS**
  (pack-coherence, l1-parity 0 diffs, l1-recall 0 drops, halluc 0/441, math 469/469); functional drive
  of the real exe answers IT+EN across L0/L1/HDC-KGE/L2/MOSAICO. (Device on-hardware confirm still
  pending a flash.)
- **Phase 1 — AKB5 sharded+PQ index** (`build_akb5.py` + firmware loader + shard router). Gate:
  `l1-recall` parity vs AKB4 at small N; synthetic 1 M-vector shard answers < ~1 s in the host
  harness with bounded reads.
- **Phase 2 — On-SD keyed fact store** (front-coded dict + postings + temporal). Gate: fact/facet
  queries are seeks not scans; `KG_FILESCAN` truncation gone; set/timeline queries work.
- **Phase 3 — Bulk import, dataset by dataset** (CC0 first: Wikidata → GeoNames → WordNet → CODATA;
  then share-alike: Wiktionary → Wikipedia mini → ConceptNet). Each importer gated for 0 OOD-FP and
  correct categorization before the next.
- **Phase 4 — Categorization + dedup + contradiction at scale** (`categorize.mjs`,
  `conflict_resolve.mjs`, content-hash dedup on SD not RAM). Gate: no `(s,rel)` conflicts ship
  unresolved; IT/EN parity for every emitted relation (kill the fake-bilingual 30 %).
- **Phase 5 — Close the flywheel** (sync `pull` + calibrated certainty). Gate: a fact taught on the
  device reaches the corpus through the existing promote gate.
- **Phase 6 — Device on-demand self-distill** (roadmap Phase 6).

## 8b. Progress log

**2026-06-07 — Import infrastructure built; flat-index bulk-import limit proven.**
- **Built + green:** `tools/anima/build_packs.mjs` (`npm run anima:packs`) — one deterministic command
  that builds BOTH packs (device D=192, host D=256) to separate outputs, augments (ASIG), syncs the
  device trees, runs the guard; `tools/anima/check_pack.mjs` coherence guard (wired as the first gate);
  `build_akb2.py` made output-configurable (`ANIMA_INDEX_OUT`) + its auto-mirror fixed to device-D
  trees only (it was about to copy a D=192 index into the D=256 host tree — the original bug class).
- **First importer written + validated:** `tools/anima/import_codata.mjs` — NIST CODATA 2022 →
  21 grounded bilingual constant cards, VALUES pulled from the downloaded table (authoritative, not
  hand-typed), units/phrasings curated, category `science`, provenance `codata-2022:nist`, with
  automatic cross-corpus ask-dedup. In isolation: 21/21 correct, 20/21 IT + all EN retrieve at 1.0.
- **The finding (validates Move 1):** landing the 21 cards into the **flat K-means index** re-clustered
  everything and caused collateral regressions on UNRELATED existing cases — `regress` in-dist (a
  speed-of-light eval flipped to the new precise card), 2 skill-probe borderline cases
  (`"ciao come stai"`, `"in che squadra gioca Leonardo da Vinci"`), and left `"costante di boltzmann"`
  unreachable (its exact vector exists at cos 1.0 but its cluster ranks outside the top-12 centroids
  at K=70). None were hallucinations; all were clustering noise. **Conclusion: bulk import is unsafe on
  the flat index — AKB5 per-category sharding (Move 1) must come first** so a new `science` shard can't
  perturb the `people`/`smalltalk` clusters. The constants are STAGED at
  `tools/anima/knowledge.staged/sci-constants.jsonl`, ready to land once AKB5 exists.
- Baseline restored to **31/31 gates green**.

**2026-06-07 (later) — AKB5 firmware loader BUILT + WIRED + ACTIVATED; sharded encyclopedia proven safe.**
- **Loader (firmware, `nucleo_anima_l1.c`):** `nucleo_anima_l1_akb5_available()` + `nucleo_anima_l1_akb5_query()`
  stream the manifest (`/data/anima/anima-it-akb5.bin`), rank shards by query·coarse-centroid, probe the
  top `AKB5_PROBE` shards by **reusing the entire proven `nucleo_anima_l1_query` per shard via a path swap**
  (zero duplication of the 250-line search/gate/prefilter), keep the best confidence, and leave the winning
  shard resident so a following stitch/band reads the right shard. Routing is **transparent**: a one-line
  reentrancy-guarded redirect at the top of `nucleo_anima_l1_query` (`if (s_akb5_on && !s_in_akb5) → akb5`)
  means all 12 call sites + MOSAICO + the clarify band get AKB5 with no change. RAM stays flat (one shard's
  centroids ≤18 KB; manifest streamed O(1)).
- **Activation modes:** **device** auto-activates when a valid manifest (dim==encoder) is on SD; **host**
  keeps AKB5 OFF by default (the flat host index is the calibrated gate fixture) and arms it with
  `ANIMA_AKB5=1` — so the SAME gates run both ways. *(Caught + fixed a real bug: the manifest probe ran
  before `s_ready=true`, so AKB5 silently never activated — every "AKB5 active" result was actually flat
  until the init order was fixed.)*
- **Two recall fixes via dedicated shards** (the AKB5 idiom): physics constants and countries got their own
  categories so they don't dilute the `science`/`geography` shards' prefilter (`build_akb5.py` shards by
  `category`).
- **Zero-hallucination under sharding (the hard part):** a narrow shard loses the flat index's GLOBAL
  competition, so a lone topical card clears the gate on a FALSE PREMISE — `capitale di Marte`→"Valletta
  (Malta)" (marte~malta), `capitale della Luna`→the Moon card, `quanti followers ha il Medioevo`,
  `quando ha twittato Colombo`. Fix: **`l1_premise_covered`, an AKB5-ONLY gate** (`s_in_akb5`) requiring
  every salient query word — entity AND relation/property — to be lexically present (damlev-tolerant) in the
  matched card. Stoplist is bilingual framing/filler ONLY (question words, lead-ins, `qual/exactly/many/
  there/...`) **plus identity-personal words** (`chiami/anni/...`) so "come ti chiami"→"Sono ANIMA" still
  answers; factual-lookup relation words (`capitale/followers/twittato/lavoro`) are REQUIRED, which is
  exactly what separates `capitale dell'Egitto` (card has *capitale*) from `capitale della Luna` (Moon card
  has none). **Flat keeps its competition-based gating and is byte-unchanged.**
- **Results:** flat default **41/41 gates green** (incl. new `akb5-content` gate); AKB5 mode (ANIMA_AKB5=1)
  **0 hallucinations** across halluc-stress 0/441, nl-stress 0, halluc-battery 0/505, describe fabrications
  0; describe-stress passes (74%). New gate `tools/anima-host/akb5-content.mjs` (self-forces AKB5, SKIPs if
  no manifest): recall 15/18 on capitals/constants/definitions, false-premise 10/10 abstained, 0 halluc.
- **402+ encyclopedia cards now LIVE & retrievable via AKB5** (countries/constants/wiki-defs), folded in
  with **zero perturbation of existing cases** — the isolation property the flat index could not give.
- **Known follow-up (recall parity, all SAFE abstentions, NOT hallucinations):** the top-N-SHARD router
  misses cards whose shard's *best* centroid ranks low though their *own* cluster ranks high (e.g. EN
  cross-lingual `how many continents`, the `minuti` factoid) → **router v2 = route by global top-K
  CENTROIDS (true 2-level IVF)**. Minor content vocab gaps (Avogadro card says "numero" not "costante").
  Greetings/deflections that lean on the rescue band can over-abstain under AKB5.
- **Device status:** ships FLAT by default (don't deploy the manifest) until router v2 closes recall parity;
  the loader is built, wired, dormant, and safe. Host AKB5 pack lives under `tools/anima-host/sd/data/anima/`.

**2026-06-07 (later still) — measured the AKB5↔flat gap exhaustively; replaced the blunt guard with a
targeted one → recall loss 58 → 10 at 0 hallucinations.**
- **New audit harness `tools/anima-host/akb5-diff.mjs`:** drives the REAL exe twice (flat, then ANIMA_AKB5=1)
  over **7418 queries** (every `eval_*.jsonl` + one ask/card from corpus+staged) and classifies every
  divergence (both_same / akb5_lost / akb5_gained / both_diff) flagging unsafe candidates. The measurement
  that turns guesses into data.
- **Root cause of the recall gap, MEASURED:** the broad "every salient query word must be lexically covered"
  guard was killing **52 of 58** recall regressions — all legit SEMANTIC recalls: describe-WITHOUT-naming
  ("l'energia che fluisce dal caldo al freddo" → calore), synonyms (collego/connettere), scoped superlatives
  ("montagna più alta DEL MONDO" — "mondo" absent from the card), identity ("introduce yourself" → "Sono
  ANIMA"). Lexical coverage is fundamentally **wrong for a semantic retriever**. Meanwhile the actual
  false-premise hallucinations are almost all **capital look-ups** (marte~malta) + a few **anachronisms**
  (twittato/followers/email on historical entities).
- **Fix — narrow `l1_premise_covered` (AKB5-only, `s_in_akb5`) to two measured classes, nothing else:**
  (1) PREMISE-MARKER absence: if the query carries a social/modern marker (twittato, tweet, followers,
  instagram, email, whatsapp…) that the matched card never mentions → abstain (kills "quando ha twittato
  Colombo" → 1492 deflection). (2) CAPITAL look-ups: the card must itself state a capital (kills "capitale
  della Luna" → Moon card) AND the query's country token must be present in the card (kills "capitale di
  Marte" → Malta). Everything else keeps pure cosine recall. NB: don't add "wifi" to the marker set — the
  card says "Wi-Fi" (hyphen tokenizes apart) so it would mute the legit "come mi collego al wifi".
- **Result:** akb5-diff recall loss **58 → 10** (the 10 left are router-depth/cross-lingual, all SAFE
  abstain); akb5-content recall **15→18/18**, false-premise 10/10 abstained; describe-stress passes;
  halluc-stress 0/441, nl-stress 0, halluc-battery 0/505. **Flat default ALL 42 gates green.**
- Adversarial-verify workflow (`tools/anima-host/wf-akb5-verify.mjs`): judges every flagged divergence +
  generates fresh false-premise NL traps to confirm 0 REAL fabrications (heuristic over-flags true-fact
  answers like Egitto→Il Cairo, light-speed, typo→Tokyo).

**2026-06-07 (adversarial pass) — a multi-agent verify workflow found 3 PRE-EXISTING hallucinations the 43
gates missed; all fixed, 0 regression.** The workflow (9 agents, 204 fresh generated traps) judged the
AKB5 retrieval CLEAN (0/32 flagged divergences were real fabrications — the diff heuristic over-flags
true-fact answers), but its freshly-invented traps exposed 3 real confident-fabrication bugs — all in
SHIPPED code (L0 skills + scope guard run in BOTH flat and AKB5, so flat had them too):
- **H1 — IT exclusion-scope leak.** "qual è il pianeta più grande **oltre/escluso/dopo** Giove" → "…è Giove"
  (returns the very planet the query EXCLUDES). The superlative scope guard only knew `tranne`/EN `besides`
  (and those only abstained by cosine luck). Fix: `l1_scope_covered` now has an EXCLUSION pass — for a
  superlative query, if an exclusion marker (`tranne/eccetto/escluso/salvo/oltre/dopo/besides/except/
  beyond/other than`) is followed by an entity the ANSWER names, abstain.
- **H2 — calc parsed a number glued to an entity name.** "protocollo Flixxon-9" → "Fa -9.", "Plimptonium-7"
  → "Fa -7." (generalized: Zorblax-42 → -42). `a_fold_calc` dropped the word and kept "-9" as a valid unary
  expression. Fix: require a **binary** operator (an operator with a digit already emitted before it) — a
  lone unary-signed number is not arithmetic.
- **H3 — spreadsheet parsed a cell-ref glued to an entity name.** "modulo Frobnicator-X12" →
  "=MOD(X12, 2)" (`modulo`=module false-matched MOD, `X12` lifted from the name as a cell). Fix: new
  `a_cell_fused(raw, cell)` rejects a cell token fused to a preceding word/hyphen in the raw; standalone
  cells ("vlookup A1", "round B2") are untouched. (First attempt — requiring ≥2 cells — regressed 13 legit
  single-cell asks; the gluing check is the correct, surgical fix.)
- Shared meta-defect noted: high-confidence L0 skills (calc 95, spreadsheet 96) pattern-match number-like
  substrings of fictional entity names ahead of the abstain path. The two parser fixes + the binary-op rule
  close the observed cases; an `Name-{digit}` / `X{digits}` adversarial fixture is the durable guard.
- Net: flat **ALL 43 gates green**, math-check 469/469, AKB5 halluc-stress 0/441, nl-stress 0, akb5-content
  clean. The lesson: the curated gates missed these — **adversarial generation found them**; keep it in the loop.

**2026-06-07 (adversarial pass 2) — re-ran the verify workflow on the patched build; it generated 191 FRESH
traps and found 18 more confident fabrications (4 new root classes). All fixed; flat ALL 43 green.** Proof
that one pass isn't enough — fresh generation keeps finding the next layer. The retrieval stayed clean
(0 real fabrications among 35 judged divergences); all defects were L0 skills / guards over-firing:
- **Math skills scrape a number glued to an entity name** (15 traps): "Carbon-Floonk-14"→"half of 14=7",
  "Zorbium-88"→44, "sqrt(Glorbium-64)"→8, "50% of Floonkonium-200"→100. Same family as H2 but via
  funcs/scale/percent/cuberoot, not calc. Fix at the SHARED source: `a_norm_solve` (solver) AND `a_fold_calc`
  (calc) now treat a hyphen between a letter and a digit as a JOIN (entity suffix), not a separator — so the
  trailing number is never a free operand. "Carbon-14" → "carbon14" (a word), no compute.
- **"Discord"→SD-storage** (+ `{value}` template leak): three layers — (a) `a_match("disco","discord")` is a
  gap-2 prefix match → made "disco" EXACT in a_storage_ok; (b) `a_match("disk","discord")` likewise → "disk"
  EXACT; (c) the real root: the **spellfix truncated the valid word "discord"→"disco"** (2 trailing deletes)
  and the retry fired storage. Fix in `a_spell_word`: a PREFIX guard — never correct a token to a vocab word
  that is a strict prefix of it (token = vocab+suffix = a longer real word). Also tightened a_storage_ok's
  short-query (`ntok<=2`) allowance to require a REAL storage word, not a fuzzy false friend.
- **Wrong-attribute on a country card**: "valuta/lingua/popolazione del Giappone"→the capital. Added those
  factual-attribute words to the AKB5 premise-marker guard (query has the attribute, card lacks it → abstain).
- **Superlative wrong head-noun**: "lago più grande del Sahara"→"the largest hot desert is the Sahara". Added
  a HEAD-NOUN check to `l1_scope_covered`: the ranked noun (IT before "più", EN after the superlative adj)
  must appear in the card.
- All 11 discovered classes are now permanent fixtures in `akb5-content.mjs` (false-premise must-abstain set).
- **Gate hygiene lesson:** the full suite went flaky (a different stateful gate — teach/profile — failed each
  run) because stray anima.exe processes held the SD state files open, so the hermetic reset couldn't clear
  them. Kill strays + clear state → ALL 43 deterministically green. Not a logic regression (every gate passes
  standalone clean); the fixes write no state.

**2026-06-07 (bulk dictionary) — the WHOLE FreeDict IT↔EN dictionary on the MCU, binary-searched on SD,
0 RAM.** Downloaded ENTIRE dictionaries programmatically (no per-word API): `build_dict.py` fetches FreeDict
`ita-eng`+`eng-ita` (CC-BY-SA/GPL, ~4 MB tar.xz, lzma+TEI parse) → **76,220 IT↔EN pairs** → seed →
`gen_dicts.py` (now reads a 2nd `seed.freedict.it-en.tsv`, curated-first for display priority) normalizes +
sorts + dedups → **dict-it-en.tsv 66,761 keys / dict-en-it.tsv 62,644 keys** (~2 MB each), synced to all
3 SD trees. **Innovation:** `t_lookup` rewritten from a linear scan to **binary search over the sorted SD
file** (`fseek` to midpoint, snap to next line, bisect; final ~8 KB linear window) — ~22 seeks, one ~1 KB
buffer, ZERO resident RAM → the full ~60k-entry bilingual dictionary is queried on the ESP32-S3 in a couple
dozen disk reads. Reuses the entire existing translate skill (gate-compatible). Two bugs found+fixed by NL
testing: (1) the binary-search final scan skipped the window's first line (lo is always a line boundary —
don't re-skip; hid "sole"/"window"); (2) cross-skill: the **weather** skill grabbed "traduci **sole** in
inglese" (sole=sun) → added a translate-request veto to the weather gate (explicit `traduci`/`come si dice`
owns its weather-word OBJECT). 0 hallucination by construction: a hit is a real FreeDict entry, a miss
abstains ("Non ho X nel dizionario"). New gate `dict-stress.mjs` (auto-samples real entries both directions
as ground truth + junk-abstain + weather-veto): recall 59/59, 0 junk, 0 misroute. **Flat ALL 44 gates green.**
Device: dict is SD data (no flash needed) — sync `deploy/sd-safe` to the card to go live.

**2026-06-07 (dictionary anti-hallucination hardening) — adversarial workflow (250 NL queries, 4 lenses)
+ fixes so the 60k-entry dict NEVER asserts something false.** Three vectors found & closed:
- **Quality / wrong primary sense (the real hallucination):** `drink→abbeverare`, `run→amministrare`,
  `eat→alimentarsi` led with a rare/formal sense @conf95. Root cause: gen_dicts cross-aggregated BOTH source
  dicts into each direction, and the reverse contributions were alphabetical, burying the primary. Fix:
  **each direction uses ONLY its FreeDict forward source** (ita-eng→dict-it-en, eng-ita→dict-en-it; build_dict
  writes two directional seeds `seed.freedict.it-en/en-it.tsv`) so FreeDict's native primary-first order is
  kept; PLUS a hand-curated **primary-sense override** (`seed.override.it-en.tsv`, ~80 common verbs, bare EN
  forms) loaded FIRST so high-traffic words lead correctly (run→correre, drink→bere). Now run/drink/eat/walk/
  work/see/give/make all lead right both ways. (Forward-only also shrank the dicts 66k→28k IT-EN / 62k→49k
  EN-IT = less noise.)
- **Cross-skill misroute:** `traduci 10 in binario` was grabbed by the translator (abstained) instead of base
  conversion. Fix: the translator declines a span containing an all-digit token (numbers aren't IT↔EN words)
  → math/base owns it. (Earlier: weather skill grabbed `traduci sole` → translate-request veto on the weather
  gate.)
- **Binary-search exactness:** a deep sweep (`dict-stress --deep`, ~487 sampled keys + first/last/short/long
  edges, both directions) returns the EXACT entry every time — no neighbour-value (wrong-entry) hallucination.
- **Safe by construction:** a hit is a real FreeDict entry, a miss abstains ("Non ho X nel dizionario"); a
  multi-word idiom starting with a function word ("a caso") or a junk word abstains, never fabricates. RAM
  stays ~0 (binary search, 1 KB stack buffer) — fits the Cardputer (decent CPU, tiny RAM).
- Gate `dict-stress.mjs` (auto-samples real entries as ground truth, both directions + junk + weather-veto +
  `--deep`) green; the override + forward-only is the durable guard. Flat ALL 44 gates green.

**2026-06-07 (dictionary round 2 — adversarial re-run found 2 MORE vectors, fixed).** The fresh workflow (252
queries) confirmed round-1 clean but exposed two new confident-false classes:
- **Homograph direction-trust:** with an explicit direction the engine hard-assumed the source language, so a
  word valid in BOTH ("male","sole","estate","fame","camera","come") returned the WRONG-language reading at
  conf95 ("translate male to english" → "evil"). Fix in `nucleo_anima_translate.c`: probe BOTH dicts; if the
  key is a headword in both, return a labelled DUAL reading ("…esiste in entrambe le lingue. IT->EN: …;
  EN->IT: …") at conf 75 — honest, never a confident single wrong answer. (2 extra binary searches/query;
  RAM still ~0.) Auto-direction keeps the IT-first default.
- **Corrupt FreeDict data:** `tre→"three, E"` (lone-letter junk), `come→"…, подобно"` (Cyrillic), `prime→
  "all’apice"` (curly apostrophe, non-Latin1 the device can't render). Fix in `build_dict.py`: `ok_term`
  drops non-Latin-script + lone-char tokens; `norm_term` folds typographic punctuation (’‘“”–—…) to ASCII.
  Dicts are now ASCII+Latin1 only (verified: 0 non-Latin1 bytes).
- `dict-stress.mjs` extended with permanent fixtures: homograph words must flag both languages (never a lone
  confident reading) + a `dirty-data` check (no non-Latin1, no lone-letter junk in any translation). Green.
- Net: 5 dictionary hallucination vectors found & fixed across 2 adversarial rounds (weather-grab, numeric-
  base misroute, wrong-primary-sense, homograph direction-trust, corrupt-data) — all by adversarial NL
  generation the curated gates missed. Flat ALL 44 gates green; 0 hallucination by construction.

**2026-06-07 (dictionary round 3 — 2 more, fixed; convergence).** Re-run after round-2: quality/boundary
clean; found (a) the homograph guard only fired for an EXPLICIT target lang, so the frame/auto path ("how do
you say male", "traduci fine", "translate camera" → lang=0) still emitted one confident wrong reading →
**applied the homograph dual-reading in ALL paths** (removed the explicit-lang condition; both readings
shown whenever the key is a headword in both dicts); (b) a separate L1 (not dictionary) collision — bare
"gatto" margin-rescued the Unix "cat" command card ("Visualizza il contenuto di un file") via a gatto↔cat
encoder collision → **`l1_lone_uncovered`**: in the rescue band, a BARE single content word (≤3 tokens) that
is absent from the card AND not lexically corroborated (so typos keep lex_ok) abstains. Tuned to bare words
only so multi-word false premises ("why is the sun a cube") still deflect. Flat ALL 44 gates green; the L1
guard cost 0 recall (l1-recall / describe-stress / cross-topic unchanged). 7 vectors total across 3 rounds.

**2026-06-07 (dictionary round 4 — 8 vectors fixed; converged).** Re-run (215 q, 99% clean): (a) `traduci è
in inglese`→"and" — accent-fold collapses è(copula "is") to e(conjunction "and") → translator now DECLINES a
single-letter source key (degenerate + accent trap). (b) bare `gatto`→Unix "cat" man-page (an L1 ENCODER
collision, gatto↔cat — NOT the dictionary; the translator never fires): `l1_lone_uncovered` (rescue band,
≤3-token bare query, sole content word absent from the card, not a typo) abstains → bare `gatto`,
`cos'è il gatto`, `cos'è un gatto` now abstain. **Accepted residual:** the exact 5-token `che cos'è il gatto`
still hits the cat card (conf 84, just over the 0.85 gate); every broader L1 guard (absolute-band coverage,
at-least-one-word-covered) demonstrably muted legit SEMANTIC recall (`spiegami come dipingere a olio`→pittura,
identity "come ti chiami"→"Sono ANIMA", `wifi`→"Wi-Fi", `why is the sun a cube`→deflect), so it is left as a
narrow cosine-wobble edge rather than break working recall. **Net: the DICTIONARY is clean across all
realistic vectors; 8 hallucination vectors fixed over 4 adversarial rounds; flat ALL 45 gates green** (linter
added `realistic` + per-gate hermetic reset). Lesson reaffirmed: lexical-coverage guards fight semantic
recall — apply them ONLY where competition is genuinely absent (AKB5 shards) or to bare-word collisions.

**2026-06-07 (DEPLOYED to Cardputer + AKB5 routing made device-fast).** Flashed firmware over COM3 (gate
45/45 → build 1508 KB → hash-verified) and synced the full SD payload (sd-sync.ps1 → H:, add/update only,
device state preserved): the 83k-word **dictionary** + the **AKB5 encyclopedia** (46 shards, 2449 cards
incl. the 402 staged) + flat fallback. The device auto-activates AKB5 (manifest dim 192 == encoder).
- **Routing optimisation for the device (the "must always work / fast" pass):** the AKB5 manifest centroids
  are now stored as **1-bit signs** (ceil(D/8) bytes) instead of int8, scored with the SAME asymmetric
  holographic measure L1's prefilter uses (int8 query × 1-bit sign). Manifest **202 KB → 26 KB (8×)** so the
  per-query routing read drops from ~200 ms to ~25 ms on the ~1 MB/s SD, at no recall cost (akb5-content +
  45 gates green, halluc 0/441). `build_akb5.py` packs the signs; `nucleo_anima_l1_akb5_query` reads them.
- RAM still ~0 (manifest streamed, one shard's centroids resident ≤18 KB; dictionary binary-searched). The
  remaining per-query cost is the 3-shard search itself (inherent ~3-4× a flat L1 query) — acceptable and
  REVERSIBLE: delete `/data/anima/anima-it-akb5.bin` from the SD → instant fall back to flat, no reflash.
- Honest status: AKB5's correctness is host-proven (0 hallucination by construction); its on-device LATENCY
  is the only unvalidated property (no HW feedback available) — hence the 1-bit speedup + the instant revert.

## 9. Invariants (do not regress)

- **0 hallucination:** every shipped fact source-anchored; abstain otherwise. Answers verbatim;
  LLM only writes question paraphrases.
- **RAM = O(domains × centroids):** nothing resident grows with corpus size. Shard router + coarse
  index on SD; per-query touches one shard.
- **Per-query SD reads bounded** to a few contiguous KB ranges; no whole-file scans, no thousands of
  scattered seeks.
- **No generative LLM at runtime** (token-per-minute wall).
- **it/en parity** for every emitted relation/card; categorization certain (QID-derived), not guessed.
- **CC0/CC-BY for the primary grounded layer; CC-BY-SA isolated + attributed.**
- `npm run anima:gate` green is the pre-flight for flash.

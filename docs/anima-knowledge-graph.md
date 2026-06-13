# ANIMA Knowledge Graph — typed entities, faceted relations, indexed retrieval

> Goal: make ANIMA *categorically* smarter — answer by **type** (person/place/work/event),
> by **facet** (occupation, gender, nationality, born/died, director), and by **time**
> (events in a year/century) — while staying inside the Cardputer's hard limits (no PSRAM,
> ~18 KB runtime heap, SD-streamed, 0-hallucination contract, gate-as-pre-flight).
>
> This document is the durable plan. It supersedes ad-hoc notes in chat.

## 0. The three load-bearing facts (measured / verified)

1. **Typed relations are free on the device.** A relation is encoded as a bit-rotation
   `shift(rel) = 1 + hash32('rel:'+rel) % (D-2)` ([kge.mjs:25](../tools/anima/kge.mjs),
   `nucleo_anima_hdc.c`). Adding `occupation`, `gender`, `died`, `role`, `isa`, `director`
   needs **zero firmware change for the facts** — the reasoner absorbs any relation name.
   Firmware changes are only needed for *new question intents* and *type-gating*.

2. **The database we want exists and is license-clean: Wikidata (CC0).** Properties map
   1:1 to what we need: `P31` instance-of, `P279` subclass-of, `P106` occupation, `P21`
   sex/gender, `P569`/`P570` birth/death, `P27` citizenship, `P57` director. CC0 means we
   can ship derived facets on-device with no attribution/share-alike burden. (DBpedia /
   Kensho KDWD are convenient but CC-BY-SA — build-time only.)

3. **Holographic KGE is the wrong tool for categorical facets.** It shines on *sparse
   relational chains* (Parigi→Francia→Europa, born, died). But `occupation`/`gender`/`isa`
   are *many-to-one* (thousands of people → ~11 buckets). A fan-in bundle of M components
   has per-component coherence ~1/√M, so single-hop `infer(person, occupation)` won't clear
   the honesty gate for crowded buckets, and the inverse (set query) is worse. **Categorical
   facets belong in an inverted index + a direct typed lookup, not in the KGE.**

## 1. Data model — two stores, deliberately separate

| Store | File (per lang) | Relations | Consumer | Why |
|---|---|---|---|---|
| **Relational chains** | `mind.{it,en}.jsonl` | `capital`, `located_in`, `born`, `died`, `country` | device KGE (`nucleo_anima_hdc.c`) | sparse, low fan-in, compose into chains |
| **Categorical facets** | `facets.{it,en}.jsonl` | `isa`, `occupation`, `gender` (later: `nationality`, `role`, `director`, `era`) | inverted index + type-gate | high fan-in; needed for set/filter queries, disambiguation, anti-hallucination |

Both keep the existing triple schema `{subject, rel, value, label}` (slug keys, human
labels). Same writer, same fixtures (`deploy/sd/...`, `tools/sd-sim/...`,
`tools/anima-host/sd/...`). The split is the key design decision: it keeps the device KGE
subgraph small (`KG_MAXENT=12`) and gives the index a clean, dedicated feed.

### Controlled vocabulary

`tools/anima/refdata/taxonomy.json` is the single source of truth for the type/facet
vocabulary — kinds, the bilingual occupation **buckets** (surface forms → canonical
it/en label), and gender. The enricher (Phase 2) and the index builder (Phase 3) both read
it. Buckets are coarse on purpose (scientist/artist/politician/ruler/…); precise roles
(emperor vs king) come from Wikidata `P106` in Phase 2 where the QID→label mapping is exact
in both languages — doing it by local regex would be noisy.

## 2. Sourcing pipeline (good-citizen, mostly offline)

```
cards (title+lang) ──pageprops (50/batch, ~600 calls)──▶ QID
QID set ──one pass over the *truthy* dump (~50 GB, 8 predicates only)──▶ raw facets
raw value-QIDs ──one wbgetentities props=labels (it|en)──▶ human labels
P31/P106 QIDs ──collapse via taxonomy.json buckets / P279* membership──▶ facets.jsonl
missing tail ──live SPARQL VALUES (≤50/batch, ≤5 parallel, maxlag)──▶ fill
```

Rules (Wikimedia etiquette, enforced in the fetcher): descriptive `User-Agent` with
contact, **serial** requests, `maxlag=5`, honor `429`/`Retry-After`, prefer dumps for bulk.
`wbgetentities` caps at **50 entities/request** and returns *all* claims (filter
client-side). WDQS: 60 s timeout, ≤5 parallel, 60 s query-time/min. License of shipped
facets = **CC0 (Wikidata only)**.

Grok keeps its current role: **question-master + schema-normalizer**, never a fact source.
It can map a messy free-text occupation to a controlled bucket or adjudicate an ambiguous
type — but the *fact* is always source-anchored (Wikidata/Wikipedia). 0-hallucination intact.

## 3. SD-resident indexes (the "smart" enabler) — embedded design

Design verified against the 18 KB-heap / FAT-scattered-read regime. Everything append-only
and versioned like the existing AKB files (magic+version header, RAM-resident fixed-width
directory, SD-streamed body).

- **Shard by entity type** — *the* biggest lever. Per-type files + a tiny RAM
  `type → [id_lo, id_hi)` range table turn "German physicists" from a 30k-space problem
  into a person-subset problem, and make "restrict to persons" a free range-clamp.
- **Posting lists**: delta + **StreamVByte** (branchless decode, ~1.2 B/id) with a **skip
  block** every 128 ids (`first_id` + byte offset) → galloping `seek_ge` is one block read,
  streaming intersection in ~1–1.5 KB RAM. **No Roaring** (at 30k ids it degenerates to one
  container). For the few *dense* facets (e.g. `isa person`) store a raw bitmap variant
  (~3.75 KB fixed) and AND with the existing popcount kernels.
- **Dictionaries** (facet-value→postings, slug→entity-id): front-coded sorted blocks (16
  keys/block) + a 2-level directory (≤1 KB RAM top-index → ~5 SD reads). **No FST** (overkill
  for exact match on a microcontroller).
- **Temporal index**: dense positional year directory `[i16 year, u32 off, u16 count]`
  (`dir[year-YEAR_MIN]`, O(1)) + contiguous postings → a century range is **one contiguous
  read**.
- **Low-cardinality facets** (continent∈0..6, century∈0..30): bit-packed columns
  (`ceil(log2 card)` bits/entity), masked scan, ANDs trivially with bitmaps.
- **Caching**: a small **512 B-aligned sector LRU** (8–16 slots, generalizes the existing
  48-row encoder LRU) absorbs directory-probe read storms; pin the <1 KB dict/time
  top-indexes. Total at-rest ≈ 8–12 KB, leaving headroom under 18 KB.

New sidecar files (mirror `build_akb2.py` → `augment_akb4.py` trailer discipline):
`index-facets.bin` (sharded), `index-dict.bin`, `index-slugs.bin`, `index-time.bin`,
`index-cols.bin` (optional).

## 4. Device-side use of the facets

1. **Typed direct answer** — "che lavoro faceva X / X è uomo o donna / quando è morto X":
   exact slug lookup in the facet store (like the profile/learn tiers), not the KGE.
   Source-anchored → 0-hallucination by construction.
2. **Set / filter queries** — "quali fisici tedeschi / elenca imperatori romani": new L0
   intent → dictionary lookup(s) → streaming posting-list intersection → bounded, grounded
   answer (MOSAICO-style) with an **honest truncation note** when capped.
3. **Timeline queries** — "cosa è successo nel 1492 / eventi del '900": temporal index
   range read → grounded list.
4. **Type-gated honesty** — store a 4-bit `kind` per entity + relation domain/range
   constraints. "qual è la capitale di Einstein?" → reject (Einstein is not a place). A cheap
   integer check *before* the expensive cleanup; feeds the `traps` gate. This is the
   anti-hallucination dividend of typing.
5. **Disambiguation** — type resolves "Washington" (person vs place).

## 5. Online tier evolution — self-distilling local⇄online

When an online answer resolves a *typed* query the offline KG missed, distill the
**structured facet** (not the prose) back into `facets.jsonl`/`mind.jsonl` with a volatility
flag: stable facets (born, occupation) get a long/no TTL; ephemeral ones never persist
(reuse `is_ephemeral()` / `ttl_days`, `nucleo_anima_online.c`). The KG grows from use — an
edge AI that learns its own structure. Live sources stay as-is (open-meteo, FX, news);
Wikidata SPARQL becomes an on-demand facet source for the long tail.

## 5b. Self-evolution: the Verifiable Knowledge Ledger (VKL) + deductive taxonomy ✅ (host)

The knowledge GROWS BY ITSELF, only from certain sources, and stays immutable — three pieces:

1. **Verifiable Knowledge Ledger v2** (`tools/anima/ledger.mjs`) — *authenticated*, after an adversarial
   audit (26 findings) broke v1 (a plain hash chain only proves self-consistency). v2:
   - **Keyed chain + off-SD anchor (root of trust).** `h = HMAC-SHA256(KEY, prev_h ‖ canonical)` (full
     256-bit). The KEY lives OFF the removable SD (firmware/eFuse on device; `VKL_KEY` on host) and the
     genuine `head` is **pinned off-SD** (firmware flash; `refdata/knowledge.head` on host). So a
     forge-then-`reseal` produces an internally-valid chain with a *different* head → **detected** by the
     anchor; and without the key an attacker can't compute valid links at all. (v1's reseal laundered any lie.)
   - **Provenance that resolves.** `src` is grammar-validated (`wd:Q\d+#P\d+:Q\d+`, `wp:…@oldid`,
     `derived:closure`, `user:`, `curated:`) AND — for `wd:` — **cross-checked against the immutable
     `wd_cache`**, so `wd:lol` / a fabricated QID / bare `wd:` are rejected (not just "starts with wd:").
   - **Injective, typed canonical.** `canonical = JSON.stringify([s,rel,o,src,conf])` — no in-band 0x01
     delimiter (no collision injection), and `100 ≠ "100"` (conf is a strict integer 0..100).
   - **Device-bounded fields.** s/o ≤ device buffers (47/95 B), control chars rejected → the host hash
     commits exactly the bytes the device can hold (no truncation = no semantic forgery; no label bombs).
   - **Streaming + incremental.** `verify`/`reseal` stream line-by-line (O(1) state); `append` keeps an
     in-memory head + dedup set (O(1)/append, was O(n²)). `reseal` **refuses to bless uncertain rows**.
   The result: the graph can only grow with facts whose origin resolves, and tampering with shipped
   knowledge is detected against the firmware-pinned root. Attacks proven defeated by `ledger-attack-check.mjs`.

2. **The taxonomy grows itself = transitive closure of `subclass_of` (Wikidata P279).**
   `tools/anima/enrich_wikidata.mjs` (online, CC0) pulls each entity's occupations (P106) **and the
   P279 subclass chain** of each — `fisico ⊂ scienziato ⊂ studioso ⊂ persona`, `musicista ⊂ artista ⊂
   creatore ⊂ persona` — into an immutable cache + the ledger + `evo/subclass.jsonl`. The type
   hierarchy is then **never stored as facts**: generalizations ("Einstein is a scientist / a person")
   are **DEDUCED on the device** by composing relation-rotations (occupation ∘ subclass_of ∘ …) — the
   KGE algebra it already runs. New types appear automatically as new P279 edges arrive. Reasoning is on
   a **bounded subgraph** (the chain only, ≤ `KG_MAXENT`=12), so RAM stays tiny no matter how big the KB.

3. **Sound + honest + proof-carrying.** (a) *Soundness*: the coherence gate proves a deduction *resolves*,
   not that the premises are *meaningful* — so `closure_policy.json` is an independent guard: a **blocklist**
   of abstract upper-ontology nodes (being/subject/proto-agent/…) is **pruned at ingest** (so "person" is
   the meaningful ceiling and the gate can't certify "Einstein is a being"), cycles/self-loops are broken
   into a DAG, and a generalization is only *answered* if its target is in the **certified-roots allowlist**
   (so a vandalized `scientist ⊂ robot` edge can't make "Einstein is a robot" — robot isn't a certified
   type). (b) *Honest by convergence*: a long/noisy chain below the gate **ABSTAINS** rather than guessing.
   (c) **Proof-carrying**: each generalization cites the exact immutable, source-anchored **ledger entry
   hashes** that justify it — ANIMA can prove *why* it believes "Einstein is a scientist", down to the source.

Gates (offline, deterministic, on **real Wikidata-sourced** data): `evolution-check.mjs` (`npm run anima:evo`)
proves evolution (before/after) + soundness + proof + anchored verify; `ledger-attack-check.mjs`
(`npm run anima:attack`) replays the audit's 26 attacks and proves v2 defeats each. The enricher splits into
an **online fetch** (→ immutable `wd_cache`) and an **offline build** (`--from-cache` → evo + ledger +
anchor, policy-applied) so the gates need no network. Device-side on-demand self-distill (the device fetches
one entity's P106+P279 when online, gates by certainty, appends to its NVS-anchored ledger) is the next step.

## 6. Roadmap (each phase host-first, gated; flash only to confirm)

- **Phase 1 — Foundation. ✅ DONE.** `taxonomy.json`; `extract_triples.py` emits
  `isa`/`occupation`/`gender` (→ `facets.jsonl`) and `died` (→ `mind.jsonl`) from local data
  (bios + gazetteer), conservatively + self-checked; `typed-facets` gate. Measured:
  occupation=826, gender=454, died=38, isa=1324. No network, no device change.
- **Phase 1b — Device answering. ✅ DONE.** `nucleo_anima_facet.c` answers occupation/gender
  by exact source-anchored lookup ("che lavoro faceva X" → "X era scienziato"; "X è uomo o
  donna"), abstaining on unknown/wrong-type (0-hallucination); `died` answered via the
  existing KGE. Wired into the cascade before fuzzy L1/KGE; host-proven by the `typed-nl`
  gate (171 NL cases: occ/gen/died true-positives + adversarial unknown/place/false-premise/
  overlap → 0 fabrications). Flash to confirm on-device.
- **Phase 4 — Type-gated honesty. ✅ DONE (hardened by an adversarial workflow).**
  `nucleo_anima_facet.c::answer_typegate` honestly refuses a place-relation asked about a person
  ("in che continente è Michael Jackson" → "…è una persona, non un luogo"), never blocking a real
  geo question (place match is EXACT in both languages; person match is segment). An adversarial
  workflow (5 generators + 3 code auditors) found real bugs (surname=place bypass, EN trailing
  "in", cross-language) — all fixed at the root. Gate `typed-nl` 295/295.
- **Phase 5 — Self-evolution (VKL + deductive taxonomy). ✅ DONE (host), see §5b.** Hash-chained
  provenance ledger + Wikidata P279 closure → the taxonomy/relations grow by themselves, certain &
  immutable; gate `auto-evolution`. Device-side on-demand self-distill is the remaining step.
- **Phase 2 — Wikidata enrichment.** ◑ Started: `enrich_wikidata.mjs` (live, CC0, provenance +
  P279 closure + ledger) runs on a sample. Remaining: scale to the full corpus (`pageprops`
  title→QID + batched facets) → high-quality `occupation`/`gender`/`nationality`/`died`/
  `role`/`director` at corpus scale, corroborating/replacing the Phase-1 heuristics.
- **Phase 3 — SD indexes + set/timeline queries.** Build `index-*.bin` (§3); add L0 intents
  for set/filter ("quali fisici tedeschi") and timeline ("eventi del '900"); grounded
  composition with honest truncation. New gates.
- **Phase 6 — Device on-demand self-distill.** When online, the device fetches one entity's
  P106+P279 on a miss, gates by certainty, appends to its own ledger (volatility-aware TTL via
  `is_ephemeral`/`ttl_days`) — the VKL grows on-device, immutably. Closes the local⇄online loop.

## 7. Invariants (do not regress)

- 0 hallucination: every shipped fact source-anchored; abstain otherwise.
- `npm run anima:gate` green is the pre-flight for flash (`flash.ps1`/`release.ps1`).
- it/en parity for every emitted relation.
- Nothing large resident: indexes streamed from SD, small LRU caches, KGE subgraph ≤12.
- CC0-only for shipped facets.

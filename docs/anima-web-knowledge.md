# ANIMA — Web Knowledge: client-side indexing + offline-brain expansion

How NucleoOS lets the **browser** pull fresh, certain knowledge from the web (Wikipedia/Wikidata,
**zero LLM**), use it offline immediately, and **pour it into the Cardputer's permanent offline brain**
(AKB5) — bilingual, deduplicated, collision-checked, and **zero-hallucination by construction**.

This is the "WASM expands ANIMA's offline knowledge" path. The heavy retrieval runs where there is
headroom (the browser, 100s of MB) and the MCU's 18 KB heap is never touched.

---

## 1. The three layers

```
        ┌────────────────────────── BROWSER (client, zero-LLM) ───────────────────────────┐
        │                                                                                   │
  ① web indexer ─────────────► ② branch crawler ─────────────► ③ promotion to AKB5         │
   answer "chi è X" live        "download a whole topic"          bake into the offline      │
   (surgical disambiguation)    (bilingual, bounded)              corpus (host build)        │
        │   stores in                 │   stores in                     │ writes              │
        ▼                             ▼                                  ▼                     │
   ┌─────────────────────────── webStore (IndexedDB) ───────────────┐   knowledge/web.jsonl   │
   │  the browser's own learned knowledge — recalled OFFLINE by the │        (host corpus)    │
   │  hybrid web-indexer tier, sitting next to the WASM engine      │        │                │
   └───────────────────────────────────────────────────────────────┘        │ build_akb5.py  │
        └────────────────────────────────────────────────────────────────────┼───────────────┘
                                                                              ▼
                                              /data/anima/akb5/*  +  anima-it-akb5.bin (SD, PROTECTED)
                                              read OFFLINE by BOTH the device firmware AND the WASM engine
```

| File | Role |
|------|------|
| `apps/anima/www/local/webindex.js` | the engine: entity disambiguation, branch crawler, export. Pure + fetch-injectable. |
| `apps/anima/www/local/webstore.js` | `webStore` — IndexedDB learned store (own DB, sense-keyed, alias pointers). |
| `apps/anima/www/local/cascade.js` | offline resolution policy — adds the `webindex` tier (`browser → webindex → device`). |
| `tools/anima/web-promote.mjs` | host tool: dedup + AKB collision-check → `knowledge/web.jsonl`. |
| `apps/anima-knowledge/` | the **Conoscenza ANIMA** app (download / library / transfer). |
| `apps/anima/local/webindex.test.mjs`, `tools/anima/web-promote.test.mjs` | host gates (`npm run anima:webindex`). |

---

## 2. Layer ① — the web indexer (`webIndexAnswer`)

When the offline brains miss a knowledge question, the browser fetches the answer live and **relays it
VERBATIM** (the Wikipedia lead, pronunciation stripped) — it never composes prose, so it cannot
hallucinate. Three precision upgrades over the firmware's lean online tier:

1. **Context-aware disambiguation.** With a disambiguating context word the query uses Wikipedia
   **full-text search** (`list=search`, content-relevance + snippet), not just title autocomplete. So
   `"il dio Mercurio"` → *Mercurio (divinità)* while `"il pianeta Mercurio"` → *Mercurio (astronomia)*.
2. **Multi-candidate fallback.** A disambiguation/empty/incoherent top hit falls through to the next
   ranked candidate instead of giving up.
3. **Coherence gate.** LENS A (orthographic name match) **or** LENS B (lexical grounding in the defining
   sentence). A fuzzy/typo drift is refused, not answered wrong.

```js
import { webIndexAnswer } from '/apps/anima/local/webindex.js';
import { webStore } from '/apps/anima/local/webstore.js';
const r = await webIndexAnswer('chi è il dio Mercurio', 'it', { store: webStore, online: true });
// r = { reply, title:'Mercurio (divinità)', category:'concept', web:true, ... }  (also cached for offline recall)
```

**Recall is sense-keyed:** the store key is `slug(entity) + ':' + sorted(context)` (e.g. `mercurio:dio`
≠ `mercurio:pianeta`) so two senses of one name never collide in cache.

**Wiring.** In the chat app the tier lives in `cascade.js`'s `resolveOffline` order
`browser (WASM) → webindex → device`. It fires live in hybrid (`on`) fallback and `only` last-resort;
`edge` mode is recall-only (privacy). This means crawled knowledge is **already answered offline by the
browser stack** — the same browser that runs the WASM engine — with no copy step.

---

## 3. Layer ② — the branch crawler (`crawlBranch`)

> *"cerco Seconda Guerra Mondiale → scarica il ramo e catalogalo bilingue."*

A **bounded, targeted** crawl: the seed article + its most-relevant linked sub-articles, each catalogued
as a **bilingual, MOSAICO-shaped** card. Both languages come from the **real `it` and `en` Wikipedia
pages** (via `langlinks`) — never machine-translated. Each card carries a `reply` (defining sentence)
**and** a `detail` (the drill-down span MOSAICO stitches).

```js
import { crawlBranch } from '/apps/anima/local/webindex.js';
const res = await crawlBranch('Seconda guerra mondiale', 'it',
  { store: webStore }, { maxPages: 8, byteBudget: 180 * 1024 });
// res = { seed, cards:[{id, category, reply:{it,en}, detail:{it,en}, ask:{it,en}, source}], pages, bytes, skipped }
```

**Never explodes the client or SD:** hard caps on `maxPages` **and** `byteBudget`; **dedup** by slug +
character-trigram near-duplicate; a re-crawl skips everything already catalogued (incremental). A WWII
crawl of 8 pages is ~5 KB.

Card shape (MOSAICO-ready, schema `schemas/anima-card.schema.json`):
```json
{"id":"web.world-war-ii","category":"event","action":"answer",
 "reply":{"it":"La seconda guerra mondiale fu…","en":"World War II was…"},
 "detail":{"it":"Coinvolse le grandi potenze…","en":"It involved the great powers…"},
 "ask":{"it":["Seconda guerra mondiale"],"en":["World War II"]},
 "source":"wikipedia:it:Seconda guerra mondiale|en:World War II","ttl_days":3650}
```

---

## 4. Layer ③ — promotion into the permanent AKB5 brain

The browser can't run the on-host AKB5 build, so promotion is a **deliberate two-step** (never
automatic — honours the "no auto-deploy/flash" rule):

```
# 1) host gate: dedup + AKB collision-check, then write the curated corpus file
node tools/anima/web-promote.mjs <export.json | /data/anima/knowledge/web.jsonl> --write

# 2) bake into AKB5 + run the release gate, then deploy to SD
npm run anima:packs && npm run anima:gate
powershell -File tools\deploy.ps1 ; powershell -File tools\sd-sync.ps1 -Target H:\
```

`web-promote.mjs` is the **pre-save collision guard** (before anything touches the corpus):

- **schema** — id, bilingual `reply{it,en}`, ≥1 `ask` per language, a `wikipedia:` source.
- **duplicate** — same entity already in the corpus (id clash, or ask-overlap ≥ `DUP` 0.82) → skip.
- **collision** — asks too close to a *different* card (recall hazard) → **reject**.
- **bounded** — `MAX_PROMOTE` cap so one crawl can never bloat the shipped index.

`build_akb5.py` then applies a **second** guard (per-shard vector cosine ≥ 0.97 dedup). Two nets, no dupes.

**Why AKB5 is the right target.** Both the device firmware **and** the in-browser WASM engine load the
AKB5 index (`anima-it-akb5.bin` + `akb5/*`). Knowledge baked there is offline-recallable by **both** —
exactly *"funziona col motore offline WASM"*.

**Persistence / "mai cancellata".** The corpus file `tools/anima/knowledge/web.jsonl` is version-tracked
and re-baked on every build; the resulting `/data/anima/akb5/*` shards are **already protected** by the
firmware predicate `nucleo_fs_is_protected` (the `/data/anima` subtree) and shipped via `sd_deploy`
`SOURCE_MAP`. No system update or deploy ever deletes them — permanent by construction, no firmware edit
needed. (The runtime learned-cache, capped at 256 with FIFO eviction, is **not** used for permanence.)

> Encoder note: the device **re-embeds** the `ask[]` phrasings itself (ANE2, **D=192**) — the browser
> never ships vectors, sidestepping the host256/device192 dimension gotcha.

---

## 5. The app — "Conoscenza ANIMA" (`apps/anima-knowledge`)

A first-class, bilingual (IT/EN), theme-aware NucleoOS app. Launch it from the launcher, or by voice:
*"apri enciclopedia / wiki / scarica conoscenza"* (firmware aliases active after the next flash; the
browser-served ANIMA already reads them from `registry/app-aliases.json`).

- **Esplora** — type a topic → **Scarica ramo** (bounded bilingual crawl) or **Cerca entità** (single
  surgical lookup). A `maxPages` slider caps the crawl; results preview shows each bilingual card.
- **Libreria** — everything catalogued in the browser (IndexedDB): stats (cards / bilingual / KB /
  categories), search + category filter, per-card bilingual + detail view, delete, clear. This store is
  already used **offline** by the hybrid web-indexer tier.
- **Trasferisci** —
  - **Permanente (AKB5)**: *Genera e scrivi sul Cardputer* → builds the bilingual pack, writes it to
    `/data/anima/knowledge/web.jsonl` on the device via `/api/fs/write`, offers a `.json` download, and
    shows the exact host bake command. Used offline by device **and** WASM, forever.
  - **Rapido (insegna al dispositivo)**: teaches each card to ANIMA on the device
    (`/api/anima` *"ricorda che X significa…"*). Immediate offline recall, no rebuild — but monolingual
    and capped at the device's ~128 user-taught facts. For quick use, not permanence.

The app **imports the real engine** (`/apps/anima/local/webindex.js`, `webstore.js`) — one source of
truth, shared with the chat app's hybrid tier.

---

## 6. Tests & gates

```
npm run anima:webindex     # webindex.test.mjs (pure + LIVE Wikipedia) + web-promote.test.mjs
```

- `apps/anima/local/webindex.test.mjs` — slug/ortho/grounding/stripPronun, intent + context detection,
  candidate scoring, coherence gate, sense-keyed recall, **multi-candidate fallback**, the **branch
  crawler** (caps, bilingual, dedup, export), plus a LIVE block against real Wikipedia (auto-skips
  offline). 43 pure + ~9 live assertions.
- `tools/anima/web-promote.test.mjs` — normalize, dedup, collision, within-batch dedup, hard cap,
  idempotent merge. 15 assertions.

Zero-hallucination is structural: every field is a frozen Wikipedia span, gated for coherence, bilingual
from real pages. Nothing is generated.

---

## 7. Deploy checklist (new app, NO firmware flash required)

1. `apps/anima-knowledge/{manifest.json, www/index.html, www/assets/icon.svg}` — created.
2. `registry/apps.json` — add the `installed[]` entry (`enabled:true`).
3. `registry/app-aliases.json` — add launch aliases (distinct from `miei-fatti`), then
   `python tools/anima/gen_aliases.py` (keeps the firmware C array in lock-step; voice-launch on device
   activates at the next flash — the launcher works immediately).
4. `powershell -File tools\deploy.ps1` (stage + gzip) then `tools\sd-sync.ps1 -Target H:\` (safe push).
   The shell auto-reloads `registry/apps.json`; the app appears in the launcher.

See also: [[anima-online]] (the firmware online tier this mirrors), [[anima-mosaico-l2]] /
`anima.md` §MOSAICO (why bilingual `detail` matters), [[anima-knowledge-scale]] (the 50–60 GB roadmap),
`docs/app-manifest.md` + `docs/registry.md` (app integration).

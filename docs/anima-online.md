# ANIMA Online — a self-distilling edge AI

How ANIMA behaves **when Wi-Fi is up**, without betraying the offline-first identity in
[`anima.md`](anima.md). The device still **executes and retrieves**; it never streams a
generative LLM at runtime (see [`anima.md`](anima.md) §6 — non-negotiable). What changes
online is that ANIMA can reach **structured knowledge** and a **cloud teacher**, and —
the whole point — **permanently absorbs** what it learns so it answers the same class of
question **offline forever after**.

> One line: *the cloud does not answer for the device — it teaches the device.* Every
> online interaction leaves the Cardputer measurably smarter when powered off.

This is the build-time distillation of [`anima.md`](anima.md) (e5 teacher → int8 student)
made **continuous and online**. It is, as far as we know, new on an ESP32-class MCU.

---

## 1. The extended cascade

[`anima.md`](anima.md) defines L0 (commands/FAQ) and L1 (frozen-answer retrieval), both
offline. Online adds two tiers **and a growth loop** that feeds back into L1.

```
query
 ↓
L0  command / tool / live-state            offline, <1 ms, 0 SD        (unchanged)
 ↓ miss
L1  semantic retrieval over CURATED + LEARNED cards (one int8 space)   (unchanged engine,
 │     ├─ confident → frozen answer                                     bigger corpus)
 │     └─ band → CONFIRM ("intendi 1 o 2?")
 ↓ miss  AND  Wi-Fi up  AND  not a live/private query
L2  STRUCTURED FEDERATION (online, deterministic, zero-LLM)
 │     route the query to the right source and relay its STRUCTURED field:
 │       entity      → Wikipedia REST summary        "chi è X"
 │       fact/date   → Wikidata (SPARQL/REST)        "quando è nato X", "capitale di Y"
 │       definition  → Wiktionary REST               "cosa significa X"
 │       weather     → Open-Meteo                    "che tempo fa a Roma"
 │       units/fx    → on-device solver / fx API     "20 USD in euro"
 │     → relay frozen structured data                ── never hallucinates
 ↓ miss  (open-ended question, no structured source)
L3  TEACHER (online, generative — runs in the CLOUD, not on device)
 │     a cloud worker answers AND returns a clean ODD card
 │     (paraphrases + frozen answer + provenance), schema-validated
 ↓
GROWTH LOOP  (every L2/L3 success)
       embed the new card with the DEVICE encoder → cosine-dedup vs LEARNED →
       merge or insert into /data/anima/learned/  → instantly retrievable by L1, offline
```

Honesty gate is the same one signal philosophy as offline: if no tier clears its
confidence bar, ANIMA says *"non lo so"* — now optionally *"…lo cerco quando sono online"*.

---

## 2. Why this is the right shape (and what it is NOT)

| Tempting but wrong | Why it fails here | What we do instead |
|---|---|---|
| Stream an LLM on-device | 8 min/token from SD ([`anima.md`](anima.md) §6) | Generation stays in the cloud teacher; device only retrieves/relays |
| "Phone home to GPT" for everything | Mute when offline; no growth; cost; latency; hallucination risk | Teacher is the **last** tier and its output is **captured as permanent knowledge** |
| Scrape HTML pages | Brittle, unparseable on an MCU, legally messy | Only **structured JSON APIs** with a ready summary/field |
| Trust the LLM blindly | Hallucination = the one thing we forbid | L2 (structured) is preferred; L3 output is schema-validated + provenance-stamped + human-gated before it enters the *curated* corpus |
| Cache and forget | The device never gets smarter | **Embed + dedup + index** every answer → semantic recall offline |

The intelligence budget is spent where it is cheap (cloud, once per novel question) and
the **result** is moved to where it is free (frozen on SD, retrieved offline).

---

## 3. L2 — structured federation (the deterministic brain)

A tiny **router** (extends the L0 detectors) maps the query to a source and a parser. Each
source is a `{detect, build_url, parse, validate}` quad — the TinyAgent lesson:
*validators, not model size* ([`anima-agent.md`](anima-agent.md) §3a).

| Intent | Source | Endpoint (IT shown; EN swaps the host) | Field relayed |
|---|---|---|---|
| `chi è / cos'è X` | Wikipedia REST | `it.wikipedia.org/api/rest_v1/page/summary/{title}` | `extract` |
| disambiguation | Wikipedia opensearch | `it.wikipedia.org/w/api.php?action=opensearch&search={q}` | → CONFIRM band |
| `quando / dove / capitale / autore di X` | Wikidata | `query.wikidata.org/sparql` or REST `entity/{Q}` | claim value (P569, P36, …) |
| `cosa significa / definizione di X` | Wiktionary REST | `it.wiktionary.org/api/rest_v1/page/definition/{word}` | first sense |
| `che tempo fa a {city}` | Open-Meteo | `api.open-meteo.com/v1/forecast?...` (+ geocoding API) | temp + condition |
| `X in {currency} / unità` | on-device + fx API | local solver; fx daily rate | computed |

**Title resolution is two-step**, not free-text search: `opensearch` (handles "trump" →
"Donald Trump", typos, redirects) → `summary` by canonical title. The "search phrase" is
really a **title lookup**, which is why it's reliable.

**Wikidata is the quiet superpower.** It answers *facted* questions deterministically:
"quando è nato Einstein" → P569 → `1879-03-14`, rendered by an Italian template. This is
QA-over-knowledge-graph with **zero generation** — feels intelligent, can't hallucinate.

Transport: reuse `esp_http_client` + `esp_crt_bundle_attach` already proven by Radio
([`nucleo_audio_http.c`](../firmware/components/nucleo_audio/nucleo_audio_http.c)).
Wikimedia requires a descriptive User-Agent → `NucleoOS-ANIMA/1.0`.

### Validation = provenance, not reasoning
The device cannot reason about truth (no LLM). It doesn't try. It trusts **structured,
human-curated sources** and validates only mechanically:
- HTTP 200 + parseable JSON + non-empty target field
- `type == "standard"` → answer; `type == "disambiguation"` → CONFIRM band
  ([`nucleo_anima_l1.c`](../firmware/components/nucleo_anima/nucleo_anima_l1.c) reused);
  404/empty → honest refusal + log as GAP for `learn.py`
- truncate to the 250-char card limit on a sentence boundary; strip wiki artifacts

---

## 4. L3 — the cloud teacher (where generation lives)

Only for open-ended questions L2 cannot serve ("perché il cielo è blu", "spiegami la
ricorsione"). A **cloud worker** (a small serverless endpoint we control) does the smart
work and returns **not prose, but a card**:

```json
{ "id":"learned.<hash>", "category":"learned", "action":"answer",
  "reply": {"it":"…≤250…"}, "detail": {"it":"…≤250…"},
  "ask":  {"it":["paraphrase 1","paraphrase 2","paraphrase 3"]},
  "source":"teacher:gpt|claude|local-llm @model", "last_updated":"YYYY-MM-DD",
  "confidence": 0.0-1.0, "ttl_days": <volatility-based> }
```

The teacher is instructed to produce the **paraphrases** itself — so the card is
immediately retrievable by L1 for *phrasings the user hasn't typed yet*. The device
validates against [`schemas/anima-card.schema.json`](../schemas/anima-card.schema.json)
and **refuses** to ingest anything malformed (no silent corruption of the brain).

> **Open decision (see §8):** the teacher backend. Option A — a hosted LLM (Claude/GPT)
> behind our worker: best quality, needs a key + endpoint. Option B — structured-only (no
> L3): no infra, fully deterministic, narrower coverage. We can ship B first and add A.

---

## 5. The growth loop — how the device *evolves*

This is the novel core. Every successful L2/L3 answer becomes permanent knowledge, in a
**partition separate from the curated corpus** so trust levels never blur.

1. **Write** the card to `/data/anima/learned/<lang>.jsonl` (bounded, oldest-evicted when
   over budget). Answerable **instantly, offline, even before semantic indexing** — the
   first layer of growth, live on day one.

   **Dedup & cataloguing (impeccable by construction).** Identity is the **canonical
   Wikipedia title**, not the user's phrasing: "trump", "donald trump", "chi è Trump?" all
   resolve to `Donald Trump` → one id `wiki.it.donald-trump` → **one card, never a
   duplicate**. A second phrasing doesn't add a card — it **merges** as an alias into the
   card's `ask[]` (slug-deduped, capped at `MAX_ALIASES`), refreshing the reply/date.
   Offline lookup matches a query against the canonical slug **or any alias**, so every
   phrasing finds the single card. The substring id-needle is quoted (`"wiki.it.roma"`) so
   `roma` never collides with `roman`. Each card is **auto-classified** from the source's
   one-line description into a *kind* (`person | place | organization | work | species |
   event | concept`) → filed under the right category, never a generic "entity" bucket, and
   ready to promote into the matching curated domain. `nucleo_anima_online.c`:
   `classify()`, `card_matches()`, `ask_add()`, `cache_put()` (canonical id + merge).
2. **Embed on device.** Run the *same int8 encoder* over the card's `ask` + `reply` → an
   int8 vector. The teacher's answer is now reachable by **semantic** L1 search, not just
   exact key: "parlami del padre della relatività" → Einstein.
3. **Dedup / merge in embedding space.** Before inserting, cosine vs existing LEARNED
   vectors; if ≥ 0.92, **merge** (add the new phrasing to `ask`, refresh date) instead of
   duplicating. The device performs its own corpus hygiene — on-device, novel.
4. **Gated promotion (build-side).** Periodically `learn.py` ingests `learned/`, and a
   human promotes the good ones into a real domain pack, rebuilt into AKB2. This keeps the
   *curated* corpus clean while the *learned* corpus grows automatically — matching the
   existing "never auto-applied, blocks cheating" policy.

Two corpora, two trust tiers: **curated** (hand-reviewed, indexed) and **learned**
(auto-grown, provenance-stamped, exact + on-device-embedded). L1 searches both; results
carry their source so the UI can mark "appreso online il …".

---

## 6. Freshness, volatility, and not lying when offline

Facts are not equal. A frozen answer for a volatile fact is a lie waiting to happen
([`anima.md`](anima.md) §6.5). Each learned card carries `source` + `last_updated` +
`ttl_days`:

| Class | Examples | TTL | Offline behavior |
|---|---|---|---|
| Immutable | history, dead figures, math, geography | ~∞ (3650d) | serve freely |
| Slow | population, "current tallest", tech specs | ~180d | serve, re-fetch when online + expired |
| Volatile | "presidente USA", prices, weather | hours | **never frozen as fact**; if offline, serve with date stamp ("al …") or refuse |
| Live | time, battery, files | n/a | always L0 computed-on-the-spot, never cached |

When online + TTL expired → silent re-fetch keeps the brain aligned. When offline → serve
the cache **with its date** so staleness is visible, or refuse. Never assert volatile data
as timeless.

**Implemented as a law, not a hope.** `is_ephemeral()` in `nucleo_anima_online.c` flags any
query bound to *now* or a place's current state (temporal markers + the weather/fx/news
intents). Live intents never cache by construction; the entity path also runs the guard, so
"chi è il presidente **oggi**" is answered but **never** written to an ODD. Caching a frozen
"oggi piove" or "1 USD = 0,92 EUR" would become a lie tomorrow — so we don't.

---

## 7. Bilingual by locale (search IT for IT, EN for EN)

`assistant.lang` selects everything:

| `assistant.lang` | Wikipedia | Wiktionary | Wikidata labels | learned partition |
|---|---|---|---|---|
| `it` | `it.wikipedia.org` | `it.wiktionary.org` | `?lang=it` | `learned/it.jsonl` |
| `en` | `en.wikipedia.org` | `en.wiktionary.org` | `?lang=en` | `learned/en.jsonl` |

Cross-lingual fallback (IT query, only EN page exists) only as last resort, and the card
is stored in the language fetched + flagged. **Bonus from the multilingual e5 lineage:**
one shared int8 embedding space means an Italian query can semantically retrieve an
English-distilled learned card — cross-lingual recall for free, no translation step.

---

## 8. What we will NOT do (online anti-patterns)

Extends [`anima.md`](anima.md) §6:

1. **Never fetch without all gates.** Online path fires only on: L0+L1 miss **AND** Wi-Fi
   connected **AND** an answerable intent (entity/fact/definition/weather/…), **never** for
   commands, tools, live state, or private queries.
2. **Never prefetch / poll in background.** Energy is first-class. Fetch on demand only.
3. **Never relay unstructured scraped HTML.** Structured JSON sources only.
4. **Never ingest an unvalidated card.** Schema-validate before it touches the brain.
5. **Never freeze a volatile fact as timeless** (see §6).
6. **Never blur trust tiers.** `learned/` stays separate from curated until human-promoted.
7. **Never block the UI on the network, and never out-block the watchdog.** Fetch off the UI loop,
   show "cerco online…", fall back to honest refusal on timeout. The per-attempt TLS timeout MUST stay
   **below the 8 s Task-WDT** (it's a single un-pettable blocking call) — `HTTP_TIMEOUT=6000` + a
   wall-clock retry budget + WDT pets, locked by the `online-stability (TWDT)` gate. Full contract and
   the .166 reboot post-mortem: [`anima-native.md` §3](anima-native.md#stability).
8. **Never depend on the cloud for identity.** Offline ANIMA must remain fully functional;
   online is pure augmentation.

---

## 9. Hardware levers worth verifying (could unlock much more)

- **PSRAM.** [`anima.md`](anima.md) assumes *no PSRAM* / ~326 KB SRAM. If the board/build
  actually exposes PSRAM (many ESP32-S3 modules ship 2–8 MB), the **LEARNED index can live
  largely in RAM**, removing the SD bottleneck for the growing corpus and enabling a much
  larger hot vector set. **Action: confirm the real PSRAM situation before sizing the
  learned index.** This single fact materially changes how big the evolving brain can get.
- **SDMMC vs SPI** ([`anima.md`](anima.md) Phase 4): 4–8× SD bandwidth → cheaper per-query
  cluster reads as `learned/` grows.

---

## 10. Build plan (incremental, each step shippable)

- **O1 — L2 entity + the growth loop (no external infra). ✅ IMPLEMENTED.** Wikipedia
  opensearch→summary fetch + on-device learned cache (exact key by slug) + the entity
  detector, wired into the cascade between the typo rescue and the clarify band. Answers
  "chi è Trump / Einstein / Colombo / Berlusconi" online and **then offline forever**.
  Files: `nucleo_anima_online.{c,h}`, wired in `nucleo_anima.c`; schema gained `ttl_days`. **Simulator
  parity (was a gap, now closed):** `tools/serve-shell.mjs` mirrors the tier — `entityDetect/entityResolve/
  entityAnswer` (opensearch→summary→relay+learn) plus a **bare-noun fallback** `bareEntity()` for
  "batman?"/"einstein" (a short command-less noun, STRICT title match so junk stays an honest miss) and a
  cross-lingual + typo (`spellfix`) rescue on explicit "chi è X". Firmware gained the matching
  `nucleo_anima_online_entity_bare()`. Verify with `node tools/anima/dialogue.mjs` (talks to ANIMA, checks
  answers + offline memory recall) and `node tools/anima/teacher-smoke.mjs` (proves the teacher escape).
  The provider-agnostic cloud **teacher hook** is present but **disabled** (returns 0)
  until an API key is configured — last cascade tier, ready to enable.
- **O2 — On-device semantic recall of learned cards. ✅ IMPLEMENTED.** The L1 encoder is
  shared (`nucleo_anima_l1_encode/dim`); each learned card's phrasings are embedded and
  written to a bounded vector sidecar `learned/<lang>.vec` kept in lockstep with the JSONL
  (one encode per cache write, streaming O(dim) RAM). `nucleo_anima_online_recall()` encodes
  the query and cosine-matches the sidecar, answering a **paraphrase** of something already
  learned — offline, no network — above a conservative gate (`RECALL_THRESH`, refuse rather
  than misattribute). Wired after the live tier, before the clarify band. Note: the distilled
  encoder is shallow (hashed n-grams), so recall catches lexical/morphological variants more
  than deep conceptual paraphrase; threshold to be tuned with `tools/anima/eval.py`/`band_sim`.
- **O3 — Wikidata facts. ✅ IMPLEMENTED & TESTED (simulator).** A deterministic QA layer (NO
  LLM, NO key — Wikidata is free): `factDetect()` pattern-matches "quando è nato/morto X",
  "capitale di X", "chi ha scritto / autore di X" (IT+EN) → `wikidataFact()` resolves the entity
  (wbsearchentities) and the property (P569/P570/P36/P50) → a crisp, SOURCE-GROUNDED answer
  (e.g. "14 marzo 1879", "Tokyo", "Dante Alighieri"). Runs BEFORE the LLM teacher and, for these
  unambiguous fact patterns, OVERRIDES a broad L0 match (so "chi ha scritto…" isn't eaten by
  whoami). Learnable & deduped by `wd.<lang>.<entity>.<prop>`; recalled offline. `serve-shell.mjs`:
  `factDetect/wikidataFact/factAnswer/fmtWdTime/stripLead`. Hard-won correctness fixes: pick the
  CURRENT capital (preferred rank / no end-date qualifier, not a historical one); request labels in
  `<lang>|en` (some items like Q1067/Dante have no `it` label); match recall only on stored ask
  phrasings (the id's property segment "capital" would falsely match "capitale"). NB Wikidata
  rate-limits anonymous bursts — fine for user-paced device queries, trips on rapid test loops.
  **Pending:** firmware mirror (the C Wikipedia fetch pattern + cJSON make it straightforward).
- **O4 — live ephemeral layer. ✅ WEATHER + FX + NEWS IMPLEMENTED.** `nucleo_anima_online_live()`:
  Open-Meteo (geocode → today/tomorrow forecast, keyless) and Frankfurter/ECB (exchange
  rates, keyless), each **answered fresh and NEVER cached** (volatility law §6). News has no
  reliable keyless structured feed → honest refusal (no fabrication), to be covered by the
  teacher (O7). Detectors guard against stealing definitions ("cos'è il clima" stays L1).
  Still open under O4: Wiktionary definitions, unit conversion via the existing solver.
- **O5 — Build-side seed.** Use a research harness offline to pre-bake the ~300–500 most
  likely IT/EN entities into the curated AKB3 index → the network becomes the long tail,
  not the common case.
- **O6 — `learn.py` ingest + gated promotion** of `learned/` into curated packs.
- **O7 (decision-gated) — L3 cloud teacher** for open-ended questions (§4, §8). The hook
  exists; this phase implements the per-provider request/response (Claude/Gemini/OpenAI),
  reading `/sd/data/anima/teacher.json`, and the app UI to paste an API key.
  - **Teacher tier with SELECTIVE MEMORY — implemented & tested in the simulator.** On an L0/L1
    miss, the cascade first tries an OFFLINE recall of previously-taught cards, then (only if a
    teacher key is configured) asks the LLM. The LLM answers AND **self-classifies**: `knowledge`
    (general, reusable) → distilled into ONE deduped ODD card the device recalls offline forever;
    `ephemeral` (personal / one-off / "my…" / reminders / chit-chat) → answered but **never saved**
    ("nulla di più"). No key / offline → both steps are skipped → an honest miss, so the device
    **cannot hallucinate offline** — it only ever serves frozen, vetted ODD. Only general facts
    with confidence ≥ 0.6 are learned; dedup is by canonical slug (no doubles). TRUTH GATE (supersedes the confidence-only rule): the LLM is never an authority — a knowledge answer is persisted ONLY if it verifies against a trusted source. The LLM extracts the canonical topic, we resolve it on Wikipedia (opensearch->summary) and store WIKIPEDIA'S extract (not the LLM prose); identity = the Wikipedia title slug. Unverifiable / made-up / ambiguous -> answered but NEVER saved. Live test:
    "cos'è la fotosintesi" → learned + recalled offline on repeat; "ricordami di comprare il latte"
    → answered, not saved. `serve-shell.mjs`: `teacherAnswer()`, `learnCard()`, `learnedLookup()`;
    key via env `GROQ_KEY` (never committed). **Firmware mirror DONE** (untested — no IDF here):
    `nucleo_anima_online_teacher()` now reads `/sd/data/anima/teacher.json` (key/base/model), calls
    the LLM via `http_post_json()`, parses the JSON, and runs the SAME truth gate — `resolve_title`
    + `fetch_summary` verify on Wikipedia, then `cache_put` stores the source text (dedup/catalog/
    embed). No teacher.json → returns 0 (honest miss). Wired as the last cascade tier in
    `nucleo_anima.c`. Provisioning the key on-device (a UI to write teacher.json) is the open bit.
  - **Groundwork done:** a standalone web app `apps/groq-chat` ("AI Chat") talks to a Groq
    (OpenAI-compatible) endpoint — bring-your-own key in localStorage, model picker (default
    `llama-3.1-8b-instant`), changeable endpoint. It calls the model DIRECTLY (no ANIMA in the
    loop yet) through a new same-origin proxy **`/api/llm`** (firmware `llm_proxy` in
    `nucleo_httpd.c` + the simulator's `llmApi` in `serve-shell.mjs`) that relays the
    `Authorization` header and streams SSE back — sidestepping browser CORS exactly like
    `/api/proxy`. Wiring this LLM into ANIMA as the teacher tier is the remaining O7 step.

### Implementation notes / honest limits
- **Redirects (bug fixed).** `http_get` uses `esp_http_client_perform()` (not open()+read()),
  so HTTP 30x are followed — open()+read() silently returned the 3xx and failed. The event
  handler resets its buffer on `ON_CONNECTED` so a redirect's body never prepends the final
  one. Bounded to `HTTP_CAP` (12 KB).
- **Stack discipline.** Large buffers in the cache/recall paths are `static` (BSS, not stack)
  to coexist with the mbedTLS handshake's stack use — matching the L1 tier's existing pattern.
  ANIMA is single-threaded (one query at a time), so this is safe.
- **Synchronous fetch.** The online answer runs inside `nucleo_anima_query()` on the
  caller's task (httpd / app), up to two short GETs with a 5 s timeout each. Fine for a
  one-shot; an async "cerco online…" turn is a later refinement.
- **TLS heap.** A one-shot HTTPS GET needs ~30–40 KB transient heap; safe because no audio
  decoder runs during a query (the radio note about TLS-not-fitting is about running TLS
  *beside* the MP3 decoder). Still, fetch only on demand.
- **Not yet built/verified.** Written against the real code but **not compiled here** (no
  ESP-IDF on this box) — build with `idf.py build` and smoke-test "chi è Einstein" online,
  then again offline (airplane mode) to confirm the learned cache answers with no network.

O1 alone already delivers the headline experience and the "it evolves" demo with **no
server and no API key**. L3 (the teacher) is the only piece that needs a backend.

# ANIMA offline factory

These tools run **on a PC, not on the device** (see [docs/anima.md](../../docs/anima.md)).
They compile knowledge into the small frozen artifacts the Cardputer reads from its SD card.
Think of it as a compiler: heavy here, the output runs standalone on the ESP32. The device
**retrieves** answers (RAG), it never generates them — so new knowledge is added here.

## The two artifacts (in `models/`, deployed to the SD `/data/anima/`)

| File | Magic | What it is | When you rebuild it |
|------|-------|-----------|---------------------|
| `anima-it-encoder.bin` | `ANE2` | distilled micro-encoder (sentence → vector) | rarely — only to improve *semantic* quality |
| `anima-it-index.bin`   | `AKB2` | the **RAG knowledge** (clustered vectors + bilingual answers) | every time you add/edit knowledge |

Adding facts = rebuild the **index** only (fast, no GPU). You almost never touch the encoder.

## Knowledge is JSONL cards

Knowledge lives in [`knowledge/*.jsonl`](knowledge/) — one JSON card per line, grouped by
domain into separate files (modular & categorized): `assistant`, `commands`, `geography`,
`science`, `math`, `programming`, `nucleoos`, `electronics`, `esp32`, `c-lang`, plus any you add. Schema:
[`schemas/anima-card.schema.json`](../../schemas/anima-card.schema.json).

```json
{"id":"geo.capital.francia","category":"general","action":"answer","arg":"",
 "reply":{"it":"La capitale di Francia è Parigi.","en":"The capital of France is Paris."},
 "ask":{"it":["qual è la capitale di Francia","capitale Francia"],
        "en":["capital of France","what is the capital of France"]},
 "source":"builtin"}
```

- **`ask`** — the ways a user might ask. The retriever cosine-matches the query against
  these; more varied phrasings (IT **and** EN) = better recall. 2–6 per language is good.
- **`reply`** — the frozen answer the device speaks. Provide both languages when you can; a
  single-language card is allowed (the compiler mirrors it).
- **`action`** — `answer` (just reply) · `launch` (open the app in `arg`) · `system`
  (fill `{value}` with a live reading: `time`, `storage`).
- **`detail`** (optional) — longer drill-down text (a worked example, the gotchas) served when
  the user follows up with "dimmi di più" / "tell me more". Two-level retrieval, no generation;
  omit it and ANIMA honestly says it has nothing more. Stored in the AKB3 record per card.
- **`id`** unique, **`category`** = the domain, **`source`** = optional provenance.
- Optional build-side metadata (NOT shipped to the device): `lang_primary` (it/en/bi),
  `tags`, `last_updated`. Useful for curation, filtering and audits.

## Add knowledge — three ways

**1. By hand** — append a line to the right `knowledge/*.jsonl` (or make a new `<topic>.jsonl`).

**2. From a book/document, offline (deterministic):**
```sh
python tools/anima/ingest.py mybook.md --category general --id-prefix mybook --lang it
```
Splits the text into focused chunks (Markdown headings become section labels) and writes
draft cards to `knowledge/mybook.jsonl`. Raw but a fast start — **review and add phrasings**,
especially in the other language (deterministic drafts only cover the source language well).
`.txt`/`.md` work out of the box; `.pdf` needs `pip install pypdf`. Use `--dry-run` to preview.

**3. From a book, with an LLM (best quality):**
```sh
python tools/anima/ingest.py mybook.txt --category general --llm "ollama run llama3"
```
Each chunk is piped to *your* command (no provider hard-coded); it must read the prompt on
stdin and print `{"reply_it","reply_en","ask_it":[...],"ask_en":[...]}`. Falls back to the
deterministic draft per chunk if the LLM output isn't valid JSON.

## Quality loop: enrich → build → eval

The encoder is surface-based (hashed n-grams, not a transformer), so the dominant quality
lever is **dense, bilingual `ask` phrasings** — not changing the retriever. Three offline
tools, no model required at any step:

```sh
python tools/anima/enrich.py              # report cards with thin/one-language phrasings (the work-list)
python tools/anima/build_akb2.py          # validate all cards → models/anima-it-index.bin
python tools/anima/eval.py                # Recall@1/@3, mean cosine, gate behaviour, misses
```
`enrich.py --apply` adds within-language stem variants for thin definitional cards (it never
fabricates a missing-language translation — it flags those for you). `eval.py` runs a held-ish
set of paraphrased/cross-language queries (`eval_queries.jsonl`) and is the regression gate
(`--min-recall1 0.9 --max-fp 0` — the second flag fails the build if an out-of-scope query is
answered confidently, the "coverage illusion" trap). Add an eval query whenever you add a
knowledge area you care about.

## Routing: not everything is semantic search

Retrieval is one destination, not the only one. The L0 router (in `nucleo_anima.c`, mirrored
in `tools/serve-shell.mjs`) sends each query to the *right mechanism* before the RAG ever runs:

| domain | mechanism | example |
|--------|-----------|---------|
| `calc` | pure arithmetic parser (no model) | "quanto fa 7 per 8" → "Fa 56." |
| `system` | live device reading | "che ore sono" → the clock |
| `app` | launch a registry app | "apri le foto" |
| `tool` | function call (create/open file) | "crea un file note.txt" |
| `faq` | static built-in answer | "chi sei" |
| `knowledge` | L1 AKB2 retrieval | "cos'è il deep sleep" |
| `none` | honest refusal | "raccontami una barzelletta" |

The **calc** tool is deterministic C (folds IT/EN word operators and × ÷, then a recursive-
descent evaluator for `+ - * /` and parens); it fires before retrieval so math gets a real
answer instead of a wrong RAG hit. Division by zero is reported, never guessed.

`eval_routing.py` is the regression gate for routing — it drives the **live** cascade over
HTTP (so it tests the real router, not a copy) and reports accuracy + non-so rate per domain:

```sh
node tools/serve-shell.mjs &                                            # the mock (L0 only)
python tools/anima/eval_routing.py --base-url http://localhost:5599 --no-l1 --min-accuracy 0.95
python tools/anima/eval_routing.py --base-url http://192.168.0.166 --min-accuracy 0.9   # device (full)
```
`--no-l1` skips the `knowledge` queries, which need the AKB2 pack and so only resolve on the
device. Set `tools/release.local.json` host or pass `--base-url` to point at your Cardputer.

## Deploy

```sh
tools/deploy.ps1 -To H:\                   # sync models + packs to the device SD /data/anima/
```
The builder validates every card (duplicate ids, missing reply/ask, bad action). On the device
the L1 tier auto-enables once the packs are on the SD; a match below the cosine gate (0.66,
tuned via `eval.py` sweep) yields an honest "non lo so", never a hallucination. **Runtime is 100% on the ESP**: it embeds
the query (a tiny int8 n-gram sum) and does cosine over the SD index — no model, no network.

To regenerate the **encoder** (optional, improves paraphrase understanding):
`python tools/anima/distill.py` (needs the e5 teacher) — then rebuild the index.

## Licensing

Keep the MIT engine separate from CC-BY-SA data packs. Allowed in redistributed packs:
Tatoeba (CC-BY 2.0 FR), Wikipedia IT (CC-BY-SA), Morph-it!/Wiktionary IT (CC-BY-SA).
Excluded: PAISÀ (CC-BY-**NC** — training only), De Mauro (proprietary). Knowledge you ingest
from copyrighted books is for your own device; mind redistribution rights.

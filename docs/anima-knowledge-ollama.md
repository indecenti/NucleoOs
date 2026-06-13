# ANIMA knowledge pipeline — local ollama as a question-teacher

Grow ANIMA's retrieval recall by adding more *ways to ask* for each knowledge card, using a
**local** ollama model — fully offline, no API key, no cloud. Same principle as the Grok teacher
([`enrich_grok.mjs`](../tools/anima/enrich_grok.mjs)) but local, and with a stronger quality gate.

**The cardinal rule:** the LLM only proposes **questions** (`ask` phrasings), never answers. Answers
stay frozen on their source (`reply`, from Wikipedia/Wikidata/curated). So the corpus cannot be
poisoned by a hallucinated fact — at worst a weak question is rejected by the gate.

## Pieces
- [`tools/anima/ollama.mjs`](../tools/anima/ollama.mjs) — tiny HTTP client for `http://localhost:11434`
  (`OLLAMA_HOST` to override): `available()`, `listModels()`, `genJSON()` (JSON-constrained chat),
  `embed()` + a memoised `makeEmbedder()`. Every call degrades to `null` on failure.
- [`tools/anima/qgen.mjs`](../tools/anima/qgen.mjs) — the **quality gate** (pure, host-tested by
  [`qgen-check.mjs`](../tools/anima-host/qgen-check.mjs), 19/19):
  - **relevance** — a question is kept only if its content words overlap the card's topic
    (title + reply). Anti-hallucination: no off-topic question whose answer the card can't support.
  - **dedup** — drops near-duplicates of the card's own asks and of already-accepted ones
    (trigram-Jaccard).
  - **collision** — drops a question too similar to a **different** card's asks. This is the real
    hazard for a shallow encoder: two cards sharing a phrasing make retrieval return the wrong fact.
    Lexical by default; semantic with `--embed` (ollama embeddings → cosine).
  - **cue** — must read as a question, not an asserted fact (no years/numbers/statements).
- [`tools/anima/enrich_ollama.mjs`](../tools/anima/enrich_ollama.mjs) — the runner. Reads the cards in
  `tools/anima/knowledge/*.jsonl`, builds a cross-card foreign index, asks ollama for bilingual
  paraphrases per card, runs the gate, appends the survivors to `ask.it` / `ask.en` (deduped, capped).

## Usage
```
ollama serve                 # if not already running
ollama pull llama3.1:8b      # generator (qwen2.5-coder:* also work)
ollama pull nomic-embed-text # only needed for --embed (semantic collision)

node tools/anima/enrich_ollama.mjs --dry                 # preview, writes nothing
node tools/anima/enrich_ollama.mjs --file=wiki-electronics.jsonl --cap=10
node tools/anima/enrich_ollama.mjs --embed --collide=0.82   # semantic collision detection
```
Flags: `--file=` one corpus file (else all), `--limit=N`, `--cap=N` (max asks/lang/card),
`--model=`, `--embed`, `--embModel=`, `--collide=` (similarity threshold), `--dry`.

Then rebuild the index (`tools/anima/build_akb2.py`) and run `npm run anima:gate` to confirm no
regression. Without `--embed`, no embedding model is needed; if `nomic-embed-text` is absent the
collision check falls back to lexical automatically.

## PyTorch
The encoder/distillation tools under `tools/anima/*.py` (which use torch) own the *embedding model*
that ships to the device. The host pipeline here is encoder-agnostic: collision detection uses ollama
embeddings (or lexical), so question generation never has to load torch. If you prefer
sentence-transformers for the collision step, swap the `sim` closure in `enrich_ollama.mjs` — `qgen`'s
`acceptVariants` already accepts any `sim(a,b)` function.

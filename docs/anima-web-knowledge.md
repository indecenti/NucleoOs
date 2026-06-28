# ANIMA — Web Knowledge: client-side single-entity lookup (hybrid, zero-LLM)

How NucleoOS lets the **browser** answer a knowledge question from certain web sources
(Wikipedia/Wikidata, **zero LLM**) when the offline brains abstain — relayed **verbatim**, cached for
offline recall, **zero-hallucination by construction**. The heavy retrieval runs where there is headroom
(the browser) and the MCU's 18 KB heap is never touched.

> **Removed 2026-06-21 — the mass-download path.** The branch crawler (`crawlBranch`, the chat
> *"scarica il ramo X"* command), the **Conoscenza ANIMA** app (`apps/anima-knowledge`), and the AKB5
> promotion pipeline (`tools/anima/web-promote.mjs`) were deleted: bulk-cataloguing generated low-quality
> cards (synthetic `ask` phrasings) that polluted ANIMA's recall. What remains is the **surgical,
> per-question online lookup** below — in hybrid mode a user question still searches certain sources.

---

## The web indexer (`webIndexAnswer`)

When the in-browser WASM offline brain (and the device) abstain on a knowledge question, the browser
fetches the answer LIVE from Wikipedia and **relays it VERBATIM** (the lead sentence, pronunciation
stripped) — it never composes prose, so it cannot hallucinate. Three precision upgrades over the
firmware's lean online tier (`nucleo_anima_online.c`):

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
≠ `mercurio:pianeta`) so two senses of one name never collide in cache. Every entity learned once is
offline-recallable in this browser thereafter — the browser's knowledge **grows through use**, no copy
step, no device write.

| File | Role |
|------|------|
| `apps/anima/www/local/webindex.js` | the engine: entity disambiguation + verbatim extraction. Pure + fetch-injectable. |
| `apps/anima/www/local/webstore.js` | `webStore` — IndexedDB learned cache (own DB, sense-keyed, alias pointers). |
| `apps/anima/www/local/cascade.js` | offline resolution policy — adds the `webindex` tier (`browser → webindex → device`). |
| `apps/anima/local/webindex.test.mjs` | host gate (`npm run anima:webindex`). |

**Wiring.** In the chat app the tier lives in `cascade.js`'s `resolveOffline` order
`browser (WASM) → webindex → device`. It fires live in hybrid (`on`) fallback and `only` last-resort;
`edge` mode is recall-only (privacy). The Settings app exposes the cache size + a *clear* button.

---

## Tests & gates

```
npm run anima:webindex     # webindex.test.mjs (pure + LIVE Wikipedia, auto-skips offline)
```

- `apps/anima/local/webindex.test.mjs` — slug/ortho/grounding/stripPronun, intent + context detection,
  candidate scoring, coherence gate, sense-keyed recall, **multi-candidate fallback**, plus a LIVE block
  against real Wikipedia (auto-skips offline).

Zero-hallucination is structural: the reply is a frozen Wikipedia span, gated for coherence. Nothing is
generated.

See also: [[anima-online]] (the firmware online tier this mirrors), [[anima-knowledge-scale]] (the
offline-corpus roadmap — a separate system), `docs/app-manifest.md` + `docs/registry.md`.

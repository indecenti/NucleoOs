// ANIMA — client-side WEB INDEXER (hybrid, ZERO-LLM). Runs entirely in the browser.
//
// This is the "ibrida senza LLM lato client": when the in-browser WASM offline brain abstains on a
// knowledge question, THIS module fetches the answer LIVE from Wikipedia / Wikidata / Wiktionary —
// directly from the browser (their APIs are CORS-open with origin=*), never through the Cardputer.
// The MCU's 18 KB heap is never touched; the heavy, evolving retrieval lives where there's headroom.
//
// It is a SURGICAL upgrade over the firmware's online tier (nucleo_anima_online.c), which the MCU
// keeps lean. Three precision fixes the device can't afford:
//   1. Context-aware disambiguation — opensearch returns up to 8 candidates WITH one-line
//      descriptions; we score each by name-shape AND by how well its description matches the query's
//      residual context ("il PIANETA mercurio" vs "il DIO mercurio"). The firmware scores name only.
//   2. Multi-candidate fallback — if the top pick is a disambiguation page / empty / incoherent, we
//      try the next ranked candidate instead of giving up. Recovers valid info the device drops.
//   3. Answer-bearing extraction — the defining sentence (entity) or a structured Wikidata fact, not
//      a blind 4-sentence intro. Always relayed VERBATIM (0-hallucination), pronunciation stripped.
//
// Pure + injectable: fetch and the learned store are passed in, so the whole thing is host-testable
// against LIVE Wikipedia in Node (apps/anima/local/webindex.test.mjs). DOM-free, import-free.

// ---- text primitives (mirror nucleo_anima_online.c make_slug / coh_* — keep in lock-step) ----------

export function slug(s) {
  return String(s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
    .replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');
}
function toks(s) { return slug(s).split('-').filter(Boolean); }

// char-trigram cosine over two slugs (orthographic similarity)
function triCos(a, b) {
  const tri = (s) => { const g = {}; const t = '  ' + s + '  '; for (let i = 0; i < t.length - 2; i++) { const k = t.slice(i, i + 3); g[k] = (g[k] || 0) + 1; } return g; };
  const A = tri(a), B = tri(b);
  let dot = 0, na = 0, nb = 0;
  for (const k in A) { na += A[k] * A[k]; if (B[k]) dot += A[k] * B[k]; }
  for (const k in B) nb += B[k] * B[k];
  return na && nb ? dot / Math.sqrt(na * nb) : 0;
}
// bounded Levenshtein (short tokens)
function lev(a, b) {
  const la = a.length, lb = b.length;
  if (!la) return lb; if (!lb) return la;
  let prev = Array.from({ length: lb + 1 }, (_, j) => j), cur = new Array(lb + 1);
  for (let i = 1; i <= la; i++) {
    cur[0] = i;
    for (let j = 1; j <= lb; j++) {
      const cost = a[i - 1] === b[j - 1] ? 0 : 1;
      cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost);
    }
    [prev, cur] = [cur, prev];
  }
  return prev[lb];
}
// best token-pair edit similarity between two slugs (tokens len>=3)
function tokEdit(sa, sb) {
  let best = 0;
  for (const a of sa.split('-')) { if (a.length < 3) continue;
    for (const b of sb.split('-')) { if (b.length < 3) continue;
      const L = Math.max(a.length, b.length); const sim = L ? 1 - lev(a, b) / L : 0; if (sim > best) best = sim; } }
  return best;
}
// LENS A — orthographic name match
export function ortho(qslug, tslug) { return Math.max(triCos(qslug, tslug), tokEdit(qslug, tslug)); }

// LENS B — lexical grounding: do the query's content words appear in the article's defining sentence?
// Exact-token (so a typo can't ground itself). Returns the hit count (0 = not grounded).
export function grounding(words, text) {
  const ts = new Set(toks(text));
  let hits = 0, longhit = 0;
  for (const w of words) { if (w.length < 4) continue; if (ts.has(w)) { hits++; if (w.length >= 7) longhit = 1; } }
  return hits >= 2 || longhit ? hits : 0;
}

function firstSentence(ex) {
  const m = String(ex || '').match(/^[\s\S]{1,220}?[.!?](?=\s|$)/);
  return m ? m[0] : String(ex || '').slice(0, 200);
}

// Strip the IPA/AFI phonetic clutter + reference markers from a Wikipedia lead. TWIN of
// tools/anima/clean-extract.mjs (and strip_pronun() in nucleo_anima_online.c) — keep the three in
// lock-step; the shared test cases live in clean-extract.mjs.
const PRON_LBL = ['ipa:', 'afi:', 'pronuncia', 'pronunciation', 'pronounced'];
export function stripPronun(input) {
  let s = String(input || '');
  for (const lbl of PRON_LBL) {
    for (let i = 0; i < s.length; i++) {
      if (s.slice(i, i + lbl.length).toLowerCase() !== lbl) continue;
      let e = i + lbl.length, seen = 0, phon = false;
      while (e < s.length && ![';', ')', ','].includes(s[e]) && seen < 90) { if (s[e] === '[' || s[e] === '/') phon = true; e++; seen++; }
      if (!phon) continue;
      if (![';', ')', ','].includes(s[e])) continue;
      const cut = s[e] === ')' ? e : e + 1;
      s = s.slice(0, i) + s.slice(cut); i--;
    }
  }
  s = s.replace(/\[[^\]]*\]/g, '');
  return s.replace(/\(\s+/g, '(').replace(/\s+([),.;])/g, '$1').replace(/\(\)/g, '').replace(/ {2,}/g, ' ').trim();
}

// Clip a verbatim extract to a sentence/word boundary within `cap` (no mid-word cut).
export function clip(text, cap = 360) {
  const s = String(text || '').trim();
  if (s.length <= cap) return s;
  const head = s.slice(0, cap);
  const dot = Math.max(head.lastIndexOf('. '), head.lastIndexOf('! '), head.lastIndexOf('? '));
  if (dot >= cap * 0.5) return head.slice(0, dot + 1);
  const sp = head.lastIndexOf(' ');
  return (sp > 0 ? head.slice(0, sp) : head).trim() + '…';
}

// ---- intent detection ------------------------------------------------------------------------------

const ENT_TRIG = [
  'chi e ', 'chi era ', "chi e' ", 'cos e ', "cos'e ", 'cosa e ', 'cosa sono ', 'cosa significa ',
  'che cos e ', "che cos'e ", 'che cosa e ', 'parlami di ', 'parla di ', 'dimmi di ', 'conosci ',
  'who is ', 'who was ', 'what is ', 'what was ', 'whats ', 'whos ', 'tell me about ', 'do you know ',
  'what are ', 'definisci ', 'definizione di ',
].sort((a, b) => b.length - a.length);
const EPHEMERAL = /\b(oggi|domani|ieri|adesso|ora|attualmente|stamattina|stasera|ultim(?:o|a|e|i)ora|in questo momento|today|tomorrow|yesterday|now|currently|right now|latest|this (?:week|month|year))\b/i;
// articles + structural/trigger words: never part of an entity name or its disambiguating context
const STRUCT = new Set(('chi che cosa cos come quando dove perche quale quali mi parlami parla dimmi conosci sai '
  + 'significa vuol dire un uno una il lo la i gli le del dello della dei degli delle di da in su con '
  + 'who what is was were are the a an of to tell me about do you know does mean stands for and').split(/\s+/).filter(Boolean));
// TYPE/category nouns ("il PIANETA mercurio", "il DIO mercurio"): a leading/standalone type noun is
// NOT the entity — it is the disambiguating context AND a strong Wikipedia search term. The proper name
// is whatever is left. (Mirrors how the firmware would need a far bigger lexicon; here we have room.)
const TYPE = new Set(('pianeta planet dio dea god goddess divinita deity mitologia citta city paese country nazione '
  + 'regione region fiume river monte monti mount mountain lago lake isola island mare sea cantante singer '
  + 'attore actor attrice cantautore musicista musician compositore band gruppo film movie libro book romanzo '
  + 'novel album canzone song serie series videogioco squadra team azienda company societa elemento element '
  + 'metallo animale animal specie species pianta plant pittore painter scrittore writer poeta poet scienziato '
  + 'scientist fisico physicist matematico mathematician filosofo philosopher imperatore emperor re king regina '
  + 'queen papa pope santo saint santa romano romana romani greca greco antico antica').split(/\s+/).filter(Boolean));

function norm(q) {
  return String(q || '').toLowerCase().replace(/['’`]/g, ' ').normalize('NFD').replace(/[̀-ͯ]/g, '')
    .replace(/\s+/g, ' ').trim();
}

// Detect a knowledge question. Returns { kind, entity, context[], query } or null.
//   entity   -> the proper-name run: the longest stretch of tokens that are NOT type/structural words
//   context  -> the type + descriptor words that DISAMBIGUATE the entity ("pianeta", "dio", "romano")
//   query    -> the residual phrase (articles removed, type words kept) fed to Wikipedia search, which
//               ranks "pianeta mercurio" -> the planet for free
export function detectIntent(q, { bare = false } = {}) {
  let s = norm(q);
  if (!s) return null;
  if (EPHEMERAL.test(s)) return null;                 // volatile — never indexed; leave to live/LLM tiers
  let residual = null;
  for (const t of ENT_TRIG) { if (s.startsWith(t)) { residual = s.slice(t.length).trim(); break; } }
  if (residual == null) { if (!bare) return null; residual = s.replace(/[?.!]+$/, '').trim(); }
  const tokens = toks(residual);
  if (!tokens.length || tokens.length > 6) return null;
  // proper-name = the longest contiguous run of tokens that are neither structural nor a type noun
  const runs = []; let cur = [];
  for (const t of tokens) { if (STRUCT.has(t) || TYPE.has(t)) { if (cur.length) { runs.push(cur); cur = []; } } else cur.push(t); }
  if (cur.length) runs.push(cur);
  if (!runs.length) return null;
  const nameTok = runs.sort((a, b) => b.join('').length - a.join('').length)[0];
  const entity = nameTok.join(' ');
  if (slug(entity).length < 2) return null;
  if (bare) { if (nameTok.length > 3 || nameTok.some(t => t.length < 3 || /^\d+$/.test(t))) return null; }
  const nameSet = new Set(nameTok);
  const context = tokens.filter(w => w.length >= 3 && !nameSet.has(w) && !STRUCT.has(w));   // type + descriptors
  const query = tokens.filter(w => !STRUCT.has(w)).join(' ') || entity;                     // search phrase
  return { kind: 'entity', entity, context, query };
}

// ---- live fetch layer (Wikipedia) ------------------------------------------------------------------
// All endpoints are CORS-open from the browser with origin=*. `f` is the injected fetch.

const UA = { 'Api-User-Agent': 'NucleoOS-ANIMA/1.0 (https://github.com/; webindex)' };

async function getJSON(f, url) {
  try { const r = await f(url, { headers: UA }); if (!r.ok) return null; return await r.json(); }
  catch { return null; }
}

// opensearch -> title-autocomplete candidates, each { title, desc, slug }. Best when the user gave a
// bare NAME: it matches titles, and ships a short gloss (index 2) for free.
async function searchOpen(f, entity, en, limit = 8) {
  const host = (en ? 'en' : 'it') + '.wikipedia.org';
  const url = `https://${host}/w/api.php?action=opensearch&namespace=0&format=json&origin=*`
    + `&limit=${limit}&search=${encodeURIComponent(entity)}`;
  const arr = await getJSON(f, url);
  const titles = Array.isArray(arr?.[1]) ? arr[1] : [];
  const descs = Array.isArray(arr?.[2]) ? arr[2] : [];
  return titles.map((t, i) => ({ title: t, desc: descs[i] || '', slug: slug(t), rank: i }));
}
// full-text search -> content-RELEVANCE candidates with a snippet. This is the disambiguation engine:
// "dio mercurio" ranks the divinity above the planet because the god's article carries the word "dio".
// The snippet (tags stripped) is the grounding text the planet's article won't match.
async function searchFulltext(f, query, en, limit = 8) {
  const host = (en ? 'en' : 'it') + '.wikipedia.org';
  const url = `https://${host}/w/api.php?action=query&list=search&format=json&origin=*`
    + `&srnamespace=0&srlimit=${limit}&srprop=snippet&srsearch=${encodeURIComponent(query)}`;
  const j = await getJSON(f, url);
  const hits = Array.isArray(j?.query?.search) ? j.query.search : [];
  return hits.map((h, i) => ({ title: h.title, desc: String(h.snippet || '').replace(/<[^>]*>/g, ''), slug: slug(h.title), rank: i }));
}
// Pick the right source: with disambiguating context, full-text relevance wins; for a bare name,
// title-autocomplete is sharper. Union both so a strong title match is never lost.
async function searchCandidates(f, query, entity, context, en, limit = 8) {
  const primary = context && context.length ? await searchFulltext(f, query, en, limit) : await searchOpen(f, entity, en, limit);
  const open = context && context.length ? await searchOpen(f, entity, en, 4) : [];
  const seen = new Set(primary.map(c => c.slug));
  return [...primary, ...open.filter(c => !seen.has(c.slug))];
}

// Bounded intro extract via the action API (exintro+exsentences keeps the response small — critical
// parity with the firmware, which fragments on full REST summaries). Returns { extract, desc } or null
// for a disambiguation / missing page.
async function fetchExtract(f, title, en) {
  const host = (en ? 'en' : 'it') + '.wikipedia.org';
  const url = `https://${host}/w/api.php?action=query&format=json&origin=*&redirects=1`
    + `&prop=extracts|description|pageprops&ppprop=disambiguation&exintro=1&explaintext=1&exsentences=4`
    + `&titles=${encodeURIComponent(String(title).replace(/ /g, '_'))}`;
  const j = await getJSON(f, url);
  const pages = j?.query?.pages;
  const page = pages && pages[Object.keys(pages)[0]];
  if (!page || page.missing !== undefined) return null;
  if (page.pageprops && 'disambiguation' in page.pageprops) return null;
  const ex = page.extract;
  if (!ex || /\bmay refer to\b|uo riferirsi|puo' riferirsi/i.test(ex)) return null;
  return { extract: stripPronun(ex), desc: page.description || '', title: page.title || title };
}

// ---- branch crawler: WASM EXPANDS ANIMA's offline knowledge ----------------------------------------
// "cerco Seconda Guerra Mondiale -> scarica il ramo e catalogalo bilingue". A BOUNDED, targeted crawl:
// the seed article + a hard-capped set of its most-linked sub-articles, each catalogued as a VERBATIM,
// bilingual (real it+en pages via langlinks, never machine-translated), MOSAICO-shaped card with a
// reply + a drill-down detail. Hard caps on page count AND total bytes so it never explodes the client
// or SD. Zero hallucination by construction (every field is a frozen Wikipedia span). The cards it
// produces are the offline-knowledge SOURCE that tools/anima/web-promote.mjs bakes into AKB5.

// Fetch one page's lead in `lang` plus its title in the OTHER language (langlinks), and that page's
// lead too. `sentences` controls extract length (reply=short, detail=longer). Returns the bilingual
// pair { it:{title,extract}, en:{title,extract}, desc, category } or null on a disambiguation/miss.
async function fetchPageBoth(f, title, lang, sentences = 5) {
  const otherLang = lang === 'en' ? 'it' : 'en';
  const host = (lang === 'en' ? 'en' : 'it') + '.wikipedia.org';
  const url = `https://${host}/w/api.php?action=query&format=json&origin=*&redirects=1`
    + `&prop=extracts|description|pageprops|langlinks&ppprop=disambiguation&lllimit=1&lllang=${otherLang}`
    + `&exintro=1&explaintext=1&exsentences=${sentences}&titles=${encodeURIComponent(String(title).replace(/ /g, '_'))}`;
  const j = await getJSON(f, url);
  const pages = j?.query?.pages;
  const page = pages && pages[Object.keys(pages)[0]];
  if (!page || page.missing !== undefined) return null;
  if (page.pageprops && 'disambiguation' in page.pageprops) return null;
  const ex = page.extract;
  if (!ex || /\bmay refer to\b|uo riferirsi|puo' riferirsi/i.test(ex)) return null;
  const here = { title: page.title || title, extract: stripPronun(ex) };
  const ll = Array.isArray(page.langlinks) && page.langlinks[0] ? page.langlinks[0] : null;
  let other = null;
  if (ll && ll['*']) { const og = await fetchExtractLang(f, ll['*'], otherLang, sentences); if (og) other = og; }
  const out = { desc: page.description || '', category: classify(page.description) };
  out[lang === 'en' ? 'en' : 'it'] = here;
  if (other) out[otherLang] = { title: ll['*'], extract: other.extract };
  return out;
}
async function fetchExtractLang(f, title, lang, sentences = 5) {
  const host = (lang === 'en' ? 'en' : 'it') + '.wikipedia.org';
  const url = `https://${host}/w/api.php?action=query&format=json&origin=*&redirects=1`
    + `&prop=extracts&exintro=1&explaintext=1&exsentences=${sentences}&titles=${encodeURIComponent(String(title).replace(/ /g, '_'))}`;
  const j = await getJSON(f, url);
  const pages = j?.query?.pages; const page = pages && pages[Object.keys(pages)[0]];
  const ex = page && page.extract;
  if (!ex || /\bmay refer to\b|uo riferirsi|puo' riferirsi/i.test(ex)) return null;
  return { title: page.title || title, extract: stripPronun(ex) };
}

// The most relevant linked sub-articles of `title` (the "branch"), capped. Uses prop=links (namespace 0)
// in the page's own order (lead links first = most salient). Filters list/meta pages.
async function branchLinks(f, title, lang, limit = 24) {
  const host = (lang === 'en' ? 'en' : 'it') + '.wikipedia.org';
  const url = `https://${host}/w/api.php?action=query&format=json&origin=*&redirects=1`
    + `&prop=links&plnamespace=0&pllimit=${Math.min(limit, 60)}&titles=${encodeURIComponent(String(title).replace(/ /g, '_'))}`;
  const j = await getJSON(f, url);
  const pages = j?.query?.pages; const page = pages && pages[Object.keys(pages)[0]];
  const links = Array.isArray(page?.links) ? page.links : [];
  return links.map(l => l.title).filter(t => t && !/[:(]|\b\d{4}\b|elenco|list of|cronologia|timeline/i.test(t));
}

// Build a MOSAICO-shaped bilingual card from a fetched bilingual pair. reply = first ~2 sentences,
// detail = the remainder (the drill-down MOSAICO stitches). Returns null unless BOTH languages present
// (MOSAICO needs bilingual; a monolingual card is useless to it).
function splitReplyDetail(extract) {
  const s = String(extract || '').trim();
  const sents = (s.match(/[^.!?]+[.!?]+(?=\s|$)/g) || [s]).map(x => x.trim()).filter(Boolean);
  // reply = 1 sentence, grown to 2 ONLY when a 3rd remains for the detail (so a 2-sentence article
  // still yields a non-empty detail span for MOSAICO to stitch).
  let nReply = (sents.length >= 3 && sents[0].length < 120) ? 2 : 1;
  const reply = clip(sents.slice(0, nReply).join(' '), 360);
  const detail = clip(sents.slice(nReply).join(' '), 600);
  return { reply, detail };
}
function buildBranchCard(pair) {
  if (!pair || !pair.it || !pair.en || !pair.it.extract || !pair.en.extract) return null;
  const id = 'web.' + slug(pair.en.title || pair.it.title);
  const it = splitReplyDetail(pair.it.extract), en = splitReplyDetail(pair.en.extract);
  return {
    id, category: pair.category || 'concept', action: 'answer',
    reply: { it: it.reply, en: en.reply },
    detail: { it: it.detail, en: en.detail },
    ask: { it: [pair.it.title, slug(pair.it.title).replace(/-/g, ' ')], en: [pair.en.title, slug(pair.en.title).replace(/-/g, ' ')] },
    source: 'wikipedia:it:' + pair.it.title + '|en:' + pair.en.title,
    last_updated: '', ttl_days: 3650, g: 0, web: true,
  };
}

// crawlBranch(topic, lang, deps, opts) -> { topic, seed, cards, pages, bytes, skipped }
//   Bounded: opts.maxPages (default 10) AND opts.byteBudget (default 120 KB). dedup vs store by slug +
//   trigram near-dup. Never throws; returns whatever it gathered. deps.store optional (cards saved there).
export async function crawlBranch(topic, lang, deps = {}, opts = {}) {
  const f = deps.fetch || (typeof fetch !== 'undefined' ? fetch : null);
  if (!f) return null;
  const en = lang === 'en';
  const maxPages = Math.max(1, Math.min(opts.maxPages || 10, 30));
  const byteBudget = opts.byteBudget || 120 * 1024;
  // 1) resolve the seed page (reuse the disambiguation brain)
  const intent = detectIntent('parlami di ' + topic, { bare: true }) || { entity: topic, context: [], query: topic };
  const cands = await searchCandidates(f, intent.query || topic, intent.entity || topic, intent.context || [], en);
  if (!cands.length) return { topic, seed: null, cards: [], pages: 0, bytes: 0, skipped: 0 };
  const seedTitle = cands.sort((a, b) => (a.rank | 0) - (b.rank | 0))[0].title;
  // 2) seed + bounded branch link set
  const titles = [seedTitle, ...(await branchLinks(f, seedTitle, lang, maxPages * 3))];
  const cards = []; let bytes = 0, skipped = 0; const seen = new Set();
  const seenTri = [];
  for (const t of titles) {
    if (cards.length >= maxPages || bytes >= byteBudget) break;
    const sg = slug(t);
    if (!sg || seen.has(sg)) continue; seen.add(sg);
    // dedup vs the persistent store (already catalogued -> skip the fetch entirely)
    if (deps.store) { try { if (await deps.store.get(sg, en ? 'en' : 'it')) { skipped++; continue; } } catch {} }
    const pair = await fetchPageBoth(f, t, lang, 6);
    const card = buildBranchCard(pair);
    if (!card) { skipped++; continue; }
    // near-duplicate guard (trigram on the reply) so two redirects/variants don't both land
    const tri = card.reply.it + ' ' + card.reply.en;
    if (seenTri.some(p => triJaccard(p, tri) >= 0.8)) { skipped++; continue; }
    seenTri.push(tri);
    bytes += (card.reply.it + card.reply.en + card.detail.it + card.detail.en).length;
    cards.push(card);
    if (deps.store) { try { await deps.store.put({ slug: sg, lang: en ? 'en' : 'it', title: card.id.slice(4), reply: card.reply[en ? 'en' : 'it'], desc: card.category, category: card.category, source: card.source, aliases: [t], card, branch: slug(topic), ts: deps.now || 0 }); } catch {} }
  }
  return { topic, seed: seedTitle, cards, pages: cards.length, bytes, skipped };
}

// Collect the CERTAIN, bilingual, MOSAICO-shaped cards the browser has catalogued, deduped by id —
// the payload tools/anima/web-promote.mjs bakes into AKB5. Only branch cards (full bilingual it+en
// reply + detail) qualify; single-sense entity cards are excluded (they lack the bilingual detail
// MOSAICO needs and would weaken the curated corpus).
export async function exportForPromotion(store) {
  if (!store || !store.all) return [];
  const out = new Map();
  for (const lang of ['it', 'en']) {
    let recs = []; try { recs = await store.all(lang) || []; } catch {}
    for (const r of recs) {
      const c = r && r.card;
      if (c && c.id && c.reply && c.reply.it && c.reply.en && !out.has(c.id)) out.set(c.id, c);
    }
  }
  return [...out.values()];
}

// character-trigram Jaccard (near-duplicate guard; mirrors learn.js trigramJaccard)
function triJaccard(a, b) {
  const tri = (s) => { const g = new Set(); const t = '  ' + slug(s).replace(/-/g, ' ') + '  '; for (let i = 0; i < t.length - 2; i++) g.add(t.slice(i, i + 3)); return g; };
  const A = tri(a), B = tri(b); if (!A.size || !B.size) return 0;
  let inter = 0; for (const x of A) if (B.has(x)) inter++;
  return inter / (A.size + B.size - inter);
}

// ---- candidate scoring (the disambiguation brain) --------------------------------------------------

// Score one candidate for "is this the page the user meant?".
//   name    = orthographic match of the candidate title to the asked entity (LENS A)
//   ctx     = how many of the query's residual context words land in the candidate's gloss/desc
//   rank    = opensearch prominence (earlier = more linked) as the tie-breaker when name+ctx tie
// blended so context can promote the right sense of an ambiguous name without ever overriding a clear
// name mismatch.
function scoreCandidate(cand, entitySlug, context) {
  const name = ortho(entitySlug, cand.slug);
  const ctx = context.length ? grounding(context, cand.desc) : 0;
  // search rank is the prominence/relevance prior (earlier = better); it both breaks name ties for a
  // bare name AND carries the full-text relevance signal when context drove the query.
  const rankBonus = 0.07 * Math.max(0, 4 - (cand.rank | 0));
  return { name, ctx, score: name + 0.5 * Math.min(ctx, 2) + rankBonus, cand };
}

// ---- the public answer -----------------------------------------------------------------------------

// webIndexAnswer(q, lang, deps) -> shaped /api/anima-style result | null
//   deps.fetch    required (browser global or injected for tests)
//   deps.store    optional learned-web store: { get(slug,lang), put(card), all(lang) }
//   deps.bare     true -> allow the strict bare-noun fallback (no "chi è" trigger)
//   deps.online   default true; false -> recall-only (no network), serve a learned card if present
export async function webIndexAnswer(q, lang, deps = {}) {
  const f = deps.fetch || (typeof fetch !== 'undefined' ? fetch : null);
  const en = lang === 'en';
  const online = deps.online !== false;
  const intent = detectIntent(q, { bare: !!deps.bare });
  if (!intent) return null;
  const eslug = slug(intent.entity);
  // Recall/storage key is SENSE-SPECIFIC: an ambiguous name keeps one card per disambiguating context
  // ("mercurio:dio" vs "mercurio:pianeta") so the wrong sense can never be served from cache. A bare
  // name (no context) keys on the entity alone — the prominent default sense.
  const rkey = eslug + (intent.context.length ? ':' + [...intent.context].sort().join('-') : '');

  // 1) RECALL-FIRST — a card we already indexed answers instantly, no network (this is the "evolution":
  //    every entity learned once is offline-recallable in this browser thereafter).
  if (deps.store) {
    try { const hit = await deps.store.get(rkey, en ? 'en' : 'it'); if (hit && hit.reply) return shaped(hit.reply, intent.entity, { recalled: true, category: hit.category }); } catch {}
  }
  if (!online || !f) return null;

  // 2) Gather candidates. Full-text relevance drives sense-selection when context is present (the god
  //    vs the planet); opensearch sharpens a bare name. Cross-lingual is the last resort.
  let cands = await searchCandidates(f, intent.query, intent.entity, intent.context, en);
  if (!cands.length && !en) cands = await searchCandidates(f, intent.query, intent.entity, intent.context, true);
  if (!cands.length) return null;
  const ranked = cands.map((c) => scoreCandidate(c, eslug, intent.context))
    .filter(r => r.name >= 0.34 || r.ctx > 0)          // floor: a plausible name OR a context anchor
    .sort((a, b) => b.score - a.score);
  if (!ranked.length) return null;

  // 3) Walk the ranked list: fetch the extract, accept the first that passes the coherence gate.
  //    The fallback is the multi-candidate recovery — a disambiguation/empty top pick no longer dead-ends.
  const words = [...new Set([...toks(intent.entity), ...intent.context])];
  for (let i = 0; i < Math.min(ranked.length, 4); i++) {
    const c = ranked[i].cand;
    const got = await fetchExtract(f, c.title, en);
    if (!got) continue;
    if (!coherent(eslug, intent.context, got)) continue;   // LENS A name OR LENS B grounding
    const category = classify(got.desc);
    const reply = clip(got.extract);
    // 4) Persist — unless ephemeral (already filtered) — so it is recalled offline next time. Primary
    //    key = the asked entity slug (so the recall lookup above hits); the title slug and any context
    //    phrasings become alias pointers for cross-phrasing dedup.
    if (deps.store) {
      const aliasKeys = [...new Set([slug(got.title), slug(intent.query)].filter(a => a && a !== rkey))];
      try { await deps.store.put({ slug: rkey, lang: en ? 'en' : 'it', title: got.title, reply, desc: got.desc, category,
        source: 'wikipedia:' + (en ? 'en' : 'it') + ':' + got.title, aliases: [intent.entity, ...intent.context].filter(Boolean), aliasKeys, ts: deps.now || 0 }); } catch {}
    }
    return shaped(reply, intent.entity, { title: got.title, category, score: ranked[i].score });
  }
  return null;
}

// The coherence gate, client-side: accept if the resolved page's name matches the asked entity (LENS A)
// OR the query's words ground in its defining sentence (LENS B). Verbatim relay either way — this only
// decides whether the page is the right SUBJECT, never invents text.
function coherent(eslug, context, got) {
  if (ortho(eslug, slug(got.title)) >= 0.5) return true;
  const words = [...new Set([...eslug.split('-'), ...context])].filter(Boolean);
  return grounding(words, firstSentence(got.extract)) > 0;
}

function shaped(reply, entity, extra = {}) {
  return { query: entity, tier: 'fact', action: 'answer', intent: 'wikipedia', confidence: extra.recalled ? 88 : 90,
    state: 'idle', domain: 'knowledge', reply, local: true, web: true, ...extra };
}

// Light category from the one-line description (mirrors firmware classify() / serve-shell classifyDesc()).
export function classify(desc) {
  const d = String(desc || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '');
  const has = (...k) => k.some(x => d.includes(x));
  if (has('politic', 'fisic', 'scienziat', 'attore', 'attrice', 'scrittore', 'scrittrice', 'calciator', 'matematic', 'pittore', 'filosof', 'navigator', 'esplorat', 'compositor', 'regist', 'musicist', ' nato', ' nata', 'born ', 'physicist', 'scientist', 'actor', 'writer', 'singer', 'footballer', 'painter', 'philosopher', 'emperor', 'imperator')) return 'person';
  if (has('citta', 'comune', 'capitale', 'capital', 'nazione', 'regione', 'fiume', 'monte', 'montagn', 'lago', 'isola', 'city', 'town', 'country', 'region', 'river', 'mountain', 'lake', 'island', 'village', 'paese', 'continent', 'pianeta', 'planet')) return 'place';
  if (has('azienda', 'societa', 'organizzazione', 'squadra', 'partito', 'banca', 'universit', 'company', 'organization', 'team', 'party', 'bank', 'university')) return 'organization';
  if (has('film', 'movie', 'libro', 'book', 'romanzo', 'novel', 'canzone', 'song', 'album', 'videogioco', 'video game', 'dipinto', 'painting', 'serie', 'series')) return 'work';
  if (has('specie', 'species', 'genere', 'genus', 'animale', 'animal', 'pianta', 'plant', 'uccello', 'bird')) return 'species';
  if (has('guerra', 'war', 'battaglia', 'battle', 'torneo', 'tournament', 'evento', 'event')) return 'event';
  return 'concept';
}

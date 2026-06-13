// web-promote.mjs — bake the WASM web-indexer's CERTAIN knowledge into the offline AKB5 corpus.
//
// The browser's client-side web indexer (apps/anima/www/local/webindex.js) catalogues verified,
// bilingual, MOSAICO-shaped cards (Wikipedia leads, VERBATIM, coherence-gated — zero hallucination).
// exportForPromotion() hands them out as a JSON array. THIS tool is the conservative second gate that
// admits them into the permanent offline corpus (tools/anima/knowledge/web.jsonl, auto-globbed by
// build_akb5.py into the device L1/AKB5 index). It is the "WASM expands ANIMA's offline knowledge"
// path: the browser does the heavy retrieval, the Cardputer gains permanent, protected, offline recall.
//
// Pre-save COLLISION + DEDUP guard (before anything touches the corpus):
//   • schema      — id, bilingual reply{it,en}, ≥1 ask per language, a wikipedia source
//   • duplicate   — same entity already in the corpus (id clash, or ask-overlap ≥ DUP) → skip
//   • collision   — asks too close to a DIFFERENT card (recall hazard) → REJECT
//   • bounded     — a hard MAX_PROMOTE cap so one crawl can never bloat the shipped index
// build_akb5.py then applies a second guard (per-shard vector cosine ≥ 0.97 dedup). Two nets, no dupes.
//
//   node tools/anima/web-promote.mjs export.json            # dry-run report
//   node tools/anima/web-promote.mjs export.json --write     # merge into knowledge/web.jsonl
//   (then: npm run anima:packs && npm run anima:gate to ship; deploy is the user's deliberate step)

import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { trigramJaccard } from '../../apps/anima/www/forge/learn.js';
import { loadCorpus, parseJsonl } from './promote-learned.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const KNOWLEDGE = join(here, 'knowledge');
const OUT = join(KNOWLEDGE, 'web.jsonl');

export const DUP = 0.82;          // ask overlap ⇒ same entity already known
export const COLLIDE = 0.62;      // overlap with a DIFFERENT-id card ⇒ recall hazard (reject)
export const MAX_PROMOTE = 400;   // hard cap per run: never let one crawl bloat the shipped index

const ID_RE = /^[a-z0-9]+([._-][a-z0-9]+)*$/;
const asks = (c) => [...((c.ask && c.ask.it) || []), ...((c.ask && c.ask.en) || [])];
// topic phrase = the entity's own words (knowledge cards are distinctive by name, no frame-stripping).
const topicPhrase = (c) => asks(c).join(' ').toLowerCase().replace(/[^a-z0-9\s]+/g, ' ').replace(/\s+/g, ' ').trim();

// Coerce an exported web card into the curated schema (anima-card.schema.json). Returns null if it
// isn't a usable bilingual answer card. `today` = ISO date stamp for last_updated.
export function normalizeCard(c, today = '') {
  if (!c || typeof c.id !== 'string' || !ID_RE.test(c.id)) return null;
  const rIt = c.reply && c.reply.it, rEn = c.reply && c.reply.en;
  if (!rIt || !rEn) return null;                                 // MOSAICO needs bilingual; monolingual rejected
  const askIt = (c.ask && c.ask.it || []).filter(Boolean), askEn = (c.ask && c.ask.en || []).filter(Boolean);
  if (!askIt.length || !askEn.length) return null;
  if (!/^wikipedia:/.test(String(c.source || ''))) return null; // provenance required (no source -> not certain)
  const card = {
    id: c.id, category: c.category || 'concept', action: 'answer',
    reply: { it: String(rIt).slice(0, 360), en: String(rEn).slice(0, 360) },
    ask: { it: [...new Set(askIt.map(s => String(s)))].slice(0, 8), en: [...new Set(askEn.map(s => String(s)))].slice(0, 8) },
    source: c.source, last_updated: today || c.last_updated || '', ttl_days: c.ttl_days || 3650,
  };
  const dIt = c.detail && c.detail.it, dEn = c.detail && c.detail.en;
  if (dIt || dEn) card.detail = { it: String(dIt || '').slice(0, 600), en: String(dEn || '').slice(0, 600) };
  return card;
}

// Decide promote/skip/reject for normalized cards against the corpus. Pure → host-testable.
export function promoteWeb(staged, corpus, { max = MAX_PROMOTE } = {}) {
  const promoted = [], rejected = [];
  const corpusById = new Map(corpus.map((c) => [c.id, c]));
  const accepted = [];                                          // promoted-so-far, to dedup within the batch too
  for (const s of staged) {
    if (!s) { rejected.push({ id: '?', reason: 'bad-schema' }); continue; }
    if (corpusById.has(s.id)) { rejected.push({ id: s.id, reason: 'duplicate-id' }); continue; }
    if (promoted.length >= max) { rejected.push({ id: s.id, reason: 'over-cap' }); continue; }
    const sTopic = topicPhrase(s);
    let best = null, bestOv = 0;
    for (const c of corpus.concat(accepted)) { const ov = trigramJaccard(sTopic, topicPhrase(c)); if (ov > bestOv) { bestOv = ov; best = c; } }
    if (best && bestOv >= COLLIDE) {
      const same = (s.category || '') === (best.category || '') && bestOv >= DUP;
      // a near-identical same-entity card → duplicate (skip); anything else colliding → recall hazard (reject)
      rejected.push({ id: s.id, reason: same ? 'duplicate' : 'collision', against: best.id, overlap: +bestOv.toFixed(2) });
      continue;
    }
    promoted.push(s); accepted.push(s);
  }
  return { promoted, rejected };
}

// Merge promoted cards into knowledge/web.jsonl idempotently by id. Pure string builder.
export function mergeWeb(existingText, promoted) {
  const { cards } = parseJsonl(existingText || '');
  const byId = new Map(cards.map((c) => [c.id, c]));
  for (const p of promoted) byId.set(p.id, p);
  const header = '// ANIMA web-indexed knowledge — promoted from the browser web indexer by web-promote.mjs.\n'
    + '// VERBATIM bilingual Wikipedia cards (zero-hallucination, coherence-gated). Reversible: delete a line.\n'
    + '// Baked into AKB5 by build_akb5.py; gated by regress.py (anima:gate). PROTECTED on SD (never deleted).\n';
  return header + [...byId.values()].map((c) => JSON.stringify(c)).join('\n') + '\n';
}

function isoToday() {
  // host tool (plain Node) — a real date stamp is fine here.
  const d = new Date(); const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

function main(argv) {
  const args = argv.slice(2);
  const write = args.includes('--write');
  const path = args.find((a) => !a.startsWith('--'));
  if (!path || !existsSync(path)) { console.error('usage: node tools/anima/web-promote.mjs <export.json> [--write]'); process.exit(1); }
  let raw;
  try { raw = JSON.parse(readFileSync(path, 'utf8')); } catch (e) { console.error('bad JSON:', String(e.message || e)); process.exit(1); }
  const arr = Array.isArray(raw) ? raw : (Array.isArray(raw.cards) ? raw.cards : []);
  const today = isoToday();
  const normalized = arr.map((c) => normalizeCard(c, today)).filter(Boolean);
  const dropped = arr.length - normalized.length;
  const corpus = loadCorpus();
  const { promoted, rejected } = promoteWeb(normalized, corpus);
  const byReason = {}; for (const r of rejected) byReason[r.reason] = (byReason[r.reason] || 0) + 1;
  console.log(`[web-promote] input ${arr.length} (corpus ${corpus.length} cards; ${dropped} not usable bilingual)`);
  console.log(`  promoted: ${promoted.length}   rejected: ${rejected.length}  ${JSON.stringify(byReason)}`);
  for (const r of rejected.slice(0, 30)) console.log(`    ✗ ${r.id || '?'} — ${r.reason}${r.against ? ' vs ' + r.against + ' (' + r.overlap + ')' : ''}`);
  if (write && promoted.length) {
    const prev = existsSync(OUT) ? readFileSync(OUT, 'utf8') : '';
    writeFileSync(OUT, mergeWeb(prev, promoted));
    console.log(`  → wrote ${promoted.length} card(s) to knowledge/web.jsonl (run: npm run anima:packs && npm run anima:gate, then deploy)`);
  } else if (!write) {
    console.log('  (dry-run; pass --write to update knowledge/web.jsonl)');
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) main(process.argv);

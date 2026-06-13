// promote-learned.mjs — the BUILD-TIME gatekeeper of the silent learning flywheel.
//
// At runtime, apps/anima/www/forge/learn.js OPTIMISTICALLY stages verified Forge code-recipes to
// /data/anima/learned-forge.jsonl (one card per certain+useful turn, each linked to its provenance
// hash). This tool is the CONSERVATIVE second gate that decides which of those staged recipes are
// allowed to permanently enter the offline corpus (tools/anima/knowledge/learned.jsonl, which
// build_akb2.py auto-globs into the device L1 index). It is STRICTER than the runtime gate because
// it checks every candidate against the WHOLE shipped corpus, not just the session:
//
//   • schema           — valid id, a reply, ≥1 ask phrasing (anima-card.schema.json)
//   • provenance       — REQUIRED: a learned card with no provenance hash is never promoted
//   • code re-parse    — the recipe's code must still parse (checkSyntax, no execution)
//   • duplicate        — already-known same-topic recipe (ask overlap ≥ DUP) → skip
//   • collision        — asks too close to a DIFFERENT-topic corpus card → REJECT (recall hazard)
//
// The FINAL arbiter remains tools/anima/regress.py (in-dist Recall@1 100% + 0 OOD false-positives),
// which already runs in `npm run anima:gate` (red=abort pre-flash). This tool feeds it clean input.
// "Silent self-improvement of only certain & useful things" — deterministic, auditable, reversible.
//
//   node tools/anima/promote-learned.mjs [staging.jsonl]            # dry-run report
//   node tools/anima/promote-learned.mjs [staging.jsonl] --write    # write knowledge/learned.jsonl

import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { trigramJaccard } from '../../apps/anima/www/forge/learn.js';
import { checkSyntax } from '../../apps/code-runner/www/nucleo-run.js';

const here = dirname(fileURLToPath(import.meta.url));
const KNOWLEDGE = join(here, 'knowledge');

export const DUP = 0.8;          // residual-topic overlap ⇒ already known (same recipe)
export const COLLIDE = 0.5;      // residual overlap with a DIFFERENT-category card ⇒ recall hazard

const ID_RE = /^[a-z0-9]+([._-][a-z0-9]+)*$/;
const asks = (c) => [...((c.ask && c.ask.it) || []), ...((c.ask && c.ask.en) || [])];
const askText = (c) => asks(c).join(' ');
const codeOf = (c) => (c.detail && (c.detail.en || c.detail.it)) || '';

// Request-frame boilerplate to strip so the DISTINCTIVE topic remains: "come faccio un X in
// javascript" / "how do i X in javascript" → "X". Without this, code-recipes collide on shared
// scaffolding (throttle ≈ debounce) and every recipe looks like a duplicate.
const FRAME = new Set('come,faccio,fare,si,fa,puoi,scrivi,scrivimi,scriva,genera,generami,crea,creami,implementa,fammi,mostrami,dammi,una,un,uno,la,il,lo,le,i,gli,di,del,della,che,con,per,in,a,e,funzione,funzioni,metodo,programma,programmino,script,snippet,classe,componente,codice,how,do,does,can,i,you,to,an,the,write,generate,create,make,build,implement,function,method,program,class,component,code,snippet,for,of,my,please,using,with,javascript,js,typescript,ts,python,react,node,html,css'.split(','));
function topicPhrase(card) {
  return askText(card).toLowerCase().split(/[^a-z0-9]+/).filter((w) => w.length >= 2 && !FRAME.has(w)).join(' ');
}

// Parse a JSONL knowledge file (skip // comments + blank lines). Returns {cards, errors}.
export function parseJsonl(text) {
  const cards = [], errors = [];
  text.split('\n').forEach((line, i) => {
    const t = line.trim();
    if (!t || t.startsWith('//')) return;
    try { cards.push(JSON.parse(t)); } catch (e) { errors.push({ line: i + 1, error: String(e.message || e) }); }
  });
  return { cards, errors };
}

export function loadCorpus(dir = KNOWLEDGE) {
  const cards = [];
  if (!existsSync(dir)) return cards;
  for (const f of readdirSync(dir)) {
    if (!f.endsWith('.jsonl')) continue;
    if (f === 'learned.jsonl') continue;        // don't collide candidates against the prior promotion output
    const { cards: cc } = parseJsonl(readFileSync(join(dir, f), 'utf8'));
    cards.push(...cc);
  }
  return cards;
}

// Decide promote/reject for each staged card against the corpus. Pure → host-testable.
export function promote(staged, corpus, { requireProvenance = true } = {}) {
  const promoted = [], rejected = [];
  const corpusById = new Map(corpus.map((c) => [c.id, c]));
  for (const s of staged) {
    if (!s || typeof s.id !== 'string' || !ID_RE.test(s.id)) { rejected.push({ id: s && s.id, reason: 'bad-id' }); continue; }
    if (!(s.reply && (s.reply.it || s.reply.en)) || !asks(s).length) { rejected.push({ id: s.id, reason: 'bad-schema' }); continue; }
    if (requireProvenance && !(typeof s.provenance === 'string' && s.provenance)) { rejected.push({ id: s.id, reason: 'no-provenance' }); continue; }
    if (corpusById.has(s.id)) { rejected.push({ id: s.id, reason: 'duplicate-id' }); continue; }
    const code = codeOf(s);
    if (code && !checkSyntax(code).ok) { rejected.push({ id: s.id, reason: 'bad-syntax' }); continue; }

    // strongest DISTINCTIVE-topic overlap against the whole corpus (frame-stripped)
    const sTopic = topicPhrase(s);
    let best = null, bestOv = 0;
    for (const c of corpus) { const ov = trigramJaccard(sTopic, topicPhrase(c)); if (ov > bestOv) { bestOv = ov; best = c; } }
    if (best && bestOv >= COLLIDE) {
      const sameCat = (s.category || '') === (best.category || '');
      // Cross-category overlap is the dangerous case (a recipe would hijack a knowledge query) →
      // collision at ANY overlap ≥ COLLIDE. Same-category high overlap is just a known recipe → dup.
      // Same-category mid overlap (COLLIDE..DUP) is a legitimate SIBLING recipe → allow.
      if (!sameCat) { rejected.push({ id: s.id, reason: 'collision', against: best.id, overlap: +bestOv.toFixed(2) }); continue; }
      if (bestOv >= DUP) { rejected.push({ id: s.id, reason: 'duplicate', against: best.id, overlap: +bestOv.toFixed(2) }); continue; }
    }
    promoted.push(s);
  }
  return { promoted, rejected };
}

// Merge promoted cards into knowledge/learned.jsonl idempotently by id. Pure string builder.
export function mergeLearned(existingText, promoted) {
  const { cards } = parseJsonl(existingText || '');
  const byId = new Map(cards.map((c) => [c.id, c]));
  for (const p of promoted) byId.set(p.id, p);     // replace/add by id (idempotent)
  const header = '// ANIMA learned recipes — promoted from Forge sessions by promote-learned.mjs.\n'
    + '// Each card is provenance-linked (reversible: delete the line to un-learn). Gated by regress.py.\n';
  return header + [...byId.values()].map((c) => JSON.stringify(c)).join('\n') + '\n';
}

function main(argv) {
  const args = argv.slice(2);
  const write = args.includes('--write');
  const stagingPath = args.find((a) => !a.startsWith('--')) || join(KNOWLEDGE, 'learned-forge.staging.jsonl');
  if (!existsSync(stagingPath)) { console.error('no staging file:', stagingPath); process.exit(0); }
  const { cards: staged, errors } = parseJsonl(readFileSync(stagingPath, 'utf8'));
  const corpus = loadCorpus();
  const { promoted, rejected } = promote(staged, corpus);
  const byReason = {};
  for (const r of rejected) byReason[r.reason] = (byReason[r.reason] || 0) + 1;
  console.log(`[promote-learned] staged ${staged.length} (corpus ${corpus.length} cards)`);
  console.log(`  promoted: ${promoted.length}   rejected: ${rejected.length}  ${JSON.stringify(byReason)}`);
  for (const r of rejected.slice(0, 30)) console.log(`    ✗ ${r.id || '?'} — ${r.reason}${r.against ? ' vs ' + r.against + ' (' + r.overlap + ')' : ''}`);
  if (errors.length) console.log(`  ⚠ ${errors.length} unparseable staging line(s)`);
  if (write && promoted.length) {
    const out = join(KNOWLEDGE, 'learned.jsonl');
    const prev = existsSync(out) ? readFileSync(out, 'utf8') : '';
    writeFileSync(out, mergeLearned(prev, promoted));
    console.log(`  → wrote ${promoted.length} card(s) to knowledge/learned.jsonl (run build_akb2.py + anima:gate to ship)`);
  } else if (!write) {
    console.log('  (dry-run; pass --write to update knowledge/learned.jsonl)');
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) main(process.argv);

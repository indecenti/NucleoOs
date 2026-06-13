#!/usr/bin/env node
// ANIMA end-to-end OOD accuracy — drives the REAL compiled cascade (anima.exe) over a set of
// out-of-distribution paraphrases and checks the *answer*, not just the domain. For each query it
// matches the exe's reply text against the EXPECTED card's frozen reply (in the query's language),
// so it measures what the user actually experiences offline: right answer / wrong answer / refused.
//
// This is stricter than tools/anima/eval.py (a Python retrieval CEILING over the encoder, full scan):
// here the encoder, the AKB3 index, the 0.85 gate AND the evidential rescue all run — the real thing.
//
//   node tools/anima-host/ood-check.mjs                         # default: eval_ood.jsonl
//   node tools/anima-host/ood-check.mjs tools/anima/eval_ood_swe.jsonl
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe  = join(here, 'build', 'anima.exe');
const kdir = join(repo, 'tools', 'anima', 'knowledge');
const qfile = process.argv[2] || join(repo, 'tools', 'anima', 'eval_ood.jsonl');

if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }

// Load every knowledge card -> id -> { it, en } reply.
const cards = [];
for (const f of readdirSync(kdir).filter(f => f.endsWith('.jsonl'))) {
  for (const l of readFileSync(join(kdir, f), 'utf8').split(/\r?\n/)) {
    if (!l.trim() || l.startsWith('//')) continue;
    try { cards.push(JSON.parse(l)); } catch {}
  }
}
const byId = new Map(cards.map(c => [c.id, c]));

// eval.py-style matcher: expect can be an id, "prefix*", or "category:<cat>". Returns the SET of
// acceptable reply strings (in `lang`) for that expectation, so a prefix/category accepts any member.
function acceptableReplies(expect, lang) {
  const out = new Set();
  const add = c => { const r = c.reply?.[lang] || c.reply?.it || c.reply?.en; if (r) out.add(r.trim()); };
  for (const e of (Array.isArray(expect) ? expect : [expect])) {
    if (e === 'none') continue;
    if (e.startsWith('category:')) { const cat = e.slice(9); for (const c of cards) if (c.category === cat) add(c); }
    else if (e.endsWith('*'))      { const pre = e.slice(0, -1); for (const c of cards) if (c.id.startsWith(pre)) add(c); }
    else if (byId.has(e))          add(byId.get(e));
  }
  return out;
}

const items = readFileSync(qfile, 'utf8').split(/\r?\n/)
  .filter(l => l.trim() && !l.startsWith('//')).map(l => JSON.parse(l));

// One REPL stream: /reset before each query, /it|/en for language.
let lang = 'it';
const lines = [];
for (const it of items) {
  lines.push('/reset');
  const want = it.lang === 'en' ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(it.q);
}
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 32 * 1024 * 1024 });
const out = r.stdout.toString('utf8');
const blocks = out.split(/^Q: /m).slice(1);
function parse(b) {
  const tier   = (b.match(/tier=(\S+)/) || [])[1] || 'none';
  const intent = (b.match(/intent=(\S*)/) || [])[1] || '';
  const reply  = ((b.match(/reply: (.*)/) || [])[1] || '').trim();   // one line; `.` stops before \r\n
  return { tier, intent, reply };
}
const rows = blocks.map(parse);
const labelOf = s => { const i = s.search(/[.\n]/); return (i > 0 ? s.slice(0, i) : s).trim(); };

let correct = 0, clarifyHit = 0, wrong = 0, refused = 0;
let oosOK = 0, oosFP = 0, oosClarify = 0, inscope = 0, oos = 0;
const fails = [];
for (let i = 0; i < items.length; i++) {
  const it = items[i], p = rows[i] || { tier: '(none)', intent: '', reply: '' };
  const isClarify = p.intent === 'clarify';
  const answered  = !isClarify && (p.tier === 'L1/fact' || p.tier === 'L3/remote') && p.reply;
  if (it.expect === 'none') {
    oos++;
    if (isClarify) oosClarify++;          // a clarify on junk is mild (a question, not a false fact)
    else if (answered) { oosFP++; fails.push(['FALSE+', it.q, '(refuse)', p.reply.slice(0, 40)]); }
    else oosOK++;
    continue;
  }
  inscope++;
  const lang = it.lang === 'en' ? 'en' : 'it';
  const ok = acceptableReplies(it.expect, lang);
  if (isClarify) {
    if ([...ok].some(r => p.reply.includes(labelOf(r)))) clarifyHit++;   // right card surfaced as an option
    else { wrong++; fails.push(['CLARIFY?', it.q, it.expect, p.reply.slice(0, 50)]); }
  } else if (answered && ok.has(p.reply)) correct++;
  else if (answered) { wrong++; fails.push(['WRONG', it.q, it.expect, p.reply.slice(0, 40)]); }
  else { refused++; fails.push(['REFUSED', it.q, it.expect, '(non lo so)']); }
}

const engaged = correct + clarifyHit;
const pct = n => inscope ? (100 * n / inscope).toFixed(1) : '0';
console.log(`[ood] ${qfile.split(/[\\/]/).pop()} — ${items.length} queries over ${cards.length} cards`);
console.log(`  in-scope (${inscope}): CORRECT ${correct} (${pct(correct)}%)  +clarify-hit ${clarifyHit}  = engaged ${engaged} (${pct(engaged)}%)  | wrong ${wrong}  refused ${refused}`);
if (oos) console.log(`  out-of-scope (${oos}): refused(good) ${oosOK}  clarify(meh) ${oosClarify}  FALSE-POSITIVE(bad) ${oosFP}`);
if (process.argv.includes('--fails')) for (const f of fails) console.log(`    [${f[0]}] "${f[1]}" exp=${f[2]} got="${f[3]}"`);

#!/usr/bin/env node
// ANIMA L1 DEVICE-PATH recall gate — measures what l1-parity can't.
//
// l1-parity proves fast==exact on an in-distribution set (100% retention there). But the AKB4
// prefilter's quality shows on HARD, out-of-distribution natural-language phrasings, where a weak
// coarse filter can drop the true cosine-winner before the exact rerank ever sees it. This gate
// drives the REAL compiled cascade (anima.exe) over the OOD + in-dist eval phrasings TWICE — with
// the prefilter (default) and forced onto brute-force exact (ANIMA_L1_EXACT=1) — and checks, against
// the EXPECTED card's own reply text, that the prefilter retrieves the right card AS OFTEN as exact.
//
// HARD property (the regression guard for the holographic/asymmetric prefilter):
//   * fast-path recall >= exact-path recall            (the prefilter loses NO recall vs exhaustive)
//   * 0 "fast-refused-but-exact-answered-correctly"    (the prefilter never drops a gold exact found)
//
//   node tools/anima-host/l1-recall.mjs           # the gate
//   node tools/anima-host/l1-recall.mjs --verbose # print every divergence
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe  = join(here, 'build', 'anima.exe');
const kdir = join(repo, 'tools', 'anima', 'knowledge');
const verbose = process.argv.includes('--verbose');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

const norm = s => (s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();

// id -> {it, en} reply, from the same corpus the index was built from.
const reply = new Map();
for (const fn of readdirSync(kdir).filter(f => f.endsWith('.jsonl'))) {
  for (const line of readFileSync(join(kdir, fn), 'utf8').split(/\r?\n/)) {
    const t = line.trim(); if (!t || t.startsWith('//')) continue;
    let c; try { c = JSON.parse(t); } catch { continue; }
    if (c.id && c.reply) reply.set(c.id, { it: norm(c.reply.it), en: norm(c.reply.en) });
  }
}

// NL eval phrasings with a concrete expected card (skip abstention/category targets).
const evals = ['eval_ood.jsonl', 'eval_queries.jsonl'];
const items = [];
for (const fn of evals) {
  const p = join(repo, 'tools', 'anima', fn); if (!existsSync(p)) continue;
  for (const line of readFileSync(p, 'utf8').split(/\r?\n/)) {
    const t = line.trim(); if (!t || t.startsWith('//')) continue;
    let c; try { c = JSON.parse(t); } catch { continue; }
    const e = String(c.expect || '');
    if (!e || e === 'none' || e.startsWith('category:')) continue;
    if (!reply.has(e.replace(/\*$/, '')) && !e.endsWith('*')) continue;   // need a reply to score against
    items.push({ q: c.q, lang: c.lang === 'en' ? 'en' : 'it', expect: e, set: fn });
  }
}

function drive(extraEnv) {
  let lang = 'it'; const lines = [];
  for (const it of items) {
    lines.push('/reset');
    if (it.lang !== lang) { lines.push('/' + it.lang); lang = it.lang; }
    lines.push(it.q);
  }
  const input = Buffer.from(lines.join('\n') + '\n', 'utf8');
  const r = spawnSync(exe, [], { input, maxBuffer: 64 * 1024 * 1024, env: { ...process.env, ...extraEnv } });
  const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
  return blocks.map(b => {
    const tier = (b.match(/tier=(\S+)/) || [])[1] || 'none';
    let rep = (b.match(/reply:\s*([\s\S]*?)\s*$/) || [])[1] || '';
    rep = rep.replace(/\s+/g, ' ').trim();
    if (rep === '(vuoto)' || rep === '(empty)') rep = '';
    return { tier, reply: rep, answered: tier !== 'none' && rep !== '' };
  });
}

// A retrieval is "correct" if the device reply CONTAINS the expected card's reply (either language —
// the reply record is bilingual and MOSAICO may stitch detail/runner-up after it).
function correct(it, res) {
  if (!res.answered) return false;
  const got = norm(res.reply);
  const key = it.expect.replace(/\*$/, '');
  const r = reply.get(key) || reply.get(it.expect);
  if (it.expect.endsWith('*')) {            // prefix target: accept any card under that id prefix
    for (const [id, rr] of reply) if (id.startsWith(key) && ((rr.it && got.includes(rr.it)) || (rr.en && got.includes(rr.en)))) return true;
    return false;
  }
  if (!r) return false;
  return (r.it && got.includes(r.it)) || (r.en && got.includes(r.en));
}

console.log(`[l1-recall] driving ${items.length} NL phrasings x2 (prefilter vs forced-exact) over the real exe ...`);
const fast  = drive({});
const exact = drive({ ANIMA_L1_EXACT: '1' });
if (fast.length !== items.length || exact.length !== items.length) {
  console.error(`[l1-recall] block mismatch: items=${items.length} fast=${fast.length} exact=${exact.length}`); process.exit(1);
}

let fHit = 0, eHit = 0, drop = 0, diff = 0;
for (let i = 0; i < items.length; i++) {
  const it = items[i], cf = correct(it, fast[i]), ce = correct(it, exact[i]);
  if (cf) fHit++; if (ce) eHit++;
  if (ce && !cf) { drop++; if (verbose) console.log(`  [DROP] (${it.lang}) "${it.q}" — exact found ${it.expect}, fast did not (fast="${fast[i].reply.slice(0,60)}")`); }
  if (fast[i].reply !== exact[i].reply) { diff++; if (verbose) console.log(`  [DIFF] "${it.q}"\n     fast : ${fast[i].reply.slice(0,70)}\n     exact: ${exact[i].reply.slice(0,70)}`); }
}
const pc = x => `${(100*x/items.length).toFixed(1)}%`;
console.log(`[l1-recall] device recall@gate  fast ${fHit}/${items.length}=${pc(fHit)}   exact ${eHit}/${items.length}=${pc(eHit)}`);
console.log(`[l1-recall] prefilter drops (exact-correct but fast-missed): ${drop}   fast-vs-exact reply diffs: ${diff}`);

// HARD gate: prefilter must not lose recall vs exhaustive search, and must drop no gold exact found.
const ok = fHit >= eHit && drop === 0;
console.log(ok ? `[l1-recall] ✓ prefilter retains exact-path recall (no drops)`
               : `[l1-recall] ✗ prefilter LOST recall vs exact (drops=${drop}, fast<exact=${fHit < eHit})`);
process.exit(ok ? 0 : 1);

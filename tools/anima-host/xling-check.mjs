#!/usr/bin/env node
// CROSS-LINGUAL RECALL gate — the guard that was MISSING. Drives the real cascade (anima.exe) with the
// SAME concept asked in EN and IT (tools/anima/eval_xling.jsonl) and measures, for every concept whose
// IT query succeeds (so the card EXISTS), whether the EN query also reaches it. EN failures where IT
// works = the pure cross-lingual gap (the encoder is bilingual, but EN paraphrases/replies may be thin).
//
// A retrieval "succeeds" if the device answered (tier != none) AND the reply contains one of the concept's
// `must` root substrings (case-insensitive) — the wiki cards reply in Italian, so IT roots usually match
// even an EN query (which is exactly the cross-lingual fallback we want to confirm or expose).
//
//   node tools/anima-host/xling-check.mjs            # measure (always exit 0) — baseline mode
//   node tools/anima-host/xling-check.mjs --gate     # gate: exit 1 if EN recall < FLOOR of IT-covered
//   node tools/anima-host/xling-check.mjs --show     # print every concept's EN/IT verdict
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const FLOOR = 0.90;   // EN must reach >= 90% of the concepts IT reaches (set after enrichment)

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const file = join(here, '..', 'anima', 'eval_xling.jsonl');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }

const items = readFileSync(file, 'utf8').split(/\r?\n/)
  .filter((l) => l.trim() && !l.startsWith('//')).map((l) => JSON.parse(l));

let lang = 'it';
const lines = [];
for (const it of items) {
  lines.push('/reset');
  const want = it.lang === 'en' ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(it.q);
}
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
const parse = (b) => {
  const tier = (b.match(/tier=(\S+)/) || [])[1] || 'none';
  const reply = ((b.match(/reply:\s*([\s\S]*?)\s*$/) || [])[1] || '').replace(/\s+/g, ' ').trim();
  return { tier, reply: reply === '(vuoto)' ? '' : reply };
};
const ok = (p, must) => p.tier !== 'none' && p.reply &&
  must.some((m) => p.reply.toLowerCase().includes(String(m).toLowerCase()));

// Group EN/IT results per concept.
const byConcept = new Map();
for (let i = 0; i < items.length; i++) {
  const it = items[i], p = parse(blocks[i] || '');
  const g = byConcept.get(it.concept) || { concept: it.concept };
  g[it.lang] = ok(p, it.must);
  g[it.lang + 'Reply'] = p.reply;
  byConcept.set(it.concept, g);
}

const show = process.argv.includes('--show');
let itCov = 0, enCovOfIt = 0;
const gaps = [];
for (const g of byConcept.values()) {
  if (g.it) {
    itCov++;
    if (g.en) enCovOfIt++; else gaps.push(g.concept);
  }
  if (show) console.log(`  ${g.it ? 'IT✓' : 'IT✗'} ${g.en ? 'EN✓' : 'EN✗'}  ${g.concept}${g.it && !g.en ? `   <-- EN gap (EN reply="${(g.enReply || '').slice(0, 40)}")` : ''}`);
}
const recall = itCov ? enCovOfIt / itCov : 1;
console.log(`[xling-check] concepts ${byConcept.size} | IT-covered ${itCov} | EN-covered(of IT) ${enCovOfIt}/${itCov} = ${(100 * recall).toFixed(1)}%`);
if (gaps.length) console.log(`[xling-check] cross-lingual gaps (IT works, EN misses): ${gaps.join(', ')}`);

if (process.argv.includes('--gate')) {
  const pass = recall >= FLOOR;
  console.log(pass ? `[xling-check] ✓ EN recall ${(100 * recall).toFixed(1)}% >= floor ${(100 * FLOOR).toFixed(0)}%`
                   : `[xling-check] ✗ EN recall ${(100 * recall).toFixed(1)}% < floor ${(100 * FLOOR).toFixed(0)}% (${gaps.length} gaps)`);
  process.exit(pass ? 0 : 1);
}

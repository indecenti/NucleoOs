#!/usr/bin/env node
// ENCODER QUALITY GATE — scores the SHIPPED on-device encoder (ANE2) with real numbers, closing the
// step-1 gate that until now only ever existed as a print inside tools/anima/distill.py.
//
// distill.py prints Spearman(student vs e5) and a cross-lingual recall@1 at DISTILLATION time. Those
// numbers need the teacher model resident, so they vanish the moment training ends: nothing re-checks
// that the weights actually EXPORTED to sd/data/anima/anima-it-encoder.bin still behave. A bad export,
// a truncated file, a dimension change or a stale copy on the SD all pass every other gate, because the
// cascade's recall gates can be carried by the index alone.
//
// What this measures instead, teacher-free, so it can run in the normal gate on any machine:
//   1. SEMANTIC ORDERING — Spearman rank correlation between the encoder's cosine and a gold
//      relatedness label, per language (tools/anima/eval_encoder_sts.jsonl). This is the absolute
//      quality of the embedding space, not its fidelity to one particular teacher.
//   2. CROSS-LINGUAL ALIGNMENT — recall@1 of EN probes against their own IT translations
//      (tools/anima/eval_encoder_xling.jsonl), scored exactly like distill.py does.
//
// Cosines come from anima.exe --cos, i.e. the REAL C encoder compiled from the firmware source, so
// there is no reimplementation here that could drift from what the device runs.
//
//   node tools/anima-host/enc-quality.mjs           # measure + gate (exit 1 below a floor)
//   node tools/anima-host/enc-quality.mjs --show    # also print every pair
//   node tools/anima-host/enc-quality.mjs --measure # report only, always exit 0 (baselining)
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

// Floors. Spearman matches distill.py's own ~0.45 acceptance bar for the bilingual student; recall@1
// is set below the distillation-time figure on purpose — this gate exists to catch a BROKEN encoder,
// not to relitigate the training run every build.
const SPEARMAN_FLOOR = 0.45;
const RECALL1_FLOOR  = 0.70;

const here = dirname(fileURLToPath(import.meta.url));
const exe  = join(here, 'build', 'anima.exe');
const stsFile   = join(here, '..', 'anima', 'eval_encoder_sts.jsonl');
const xlingFile = join(here, '..', 'anima', 'eval_encoder_xling.jsonl');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }

const show    = process.argv.includes('--show');
const measure = process.argv.includes('--measure');

const readJsonl = (f) => readFileSync(f, 'utf8').split(/\r?\n/)
  .filter((l) => l.trim() && !l.startsWith('//')).map((l) => JSON.parse(l));

// Batch every pair through one `anima.exe --cos` run: one line "a<TAB>b" in, one cosine out.
function cosines(pairs) {
  if (!pairs.length) return [];
  const input = pairs.map(([a, b]) => `${a}\t${b}`).join('\n') + '\n';
  const r = spawnSync(exe, ['--cos'], { input: Buffer.from(input, 'utf8'), maxBuffer: 64 * 1024 * 1024 });
  if (r.status !== 0) { console.error('anima.exe --cos failed:', r.stderr?.toString().trim()); process.exit(2); }
  const out = r.stdout.toString('utf8').trim().split(/\r?\n/).map(Number);
  if (out.length !== pairs.length) {
    console.error(`--cos returned ${out.length} values for ${pairs.length} pairs`); process.exit(2);
  }
  return out;
}

// Spearman rho = Pearson on ranks. Ties get their average rank, which matters here: the gold labels
// are a 0-4 scale, so every level is one big tie group.
function ranks(v) {
  const idx = v.map((x, i) => [x, i]).sort((p, q) => p[0] - q[0]);
  const out = new Array(v.length);
  for (let i = 0; i < idx.length; ) {
    let j = i; while (j + 1 < idx.length && idx[j + 1][0] === idx[i][0]) j++;
    const avg = (i + j) / 2 + 1;
    for (let k = i; k <= j; k++) out[idx[k][1]] = avg;
    i = j + 1;
  }
  return out;
}
function spearman(a, b) {
  const ra = ranks(a), rb = ranks(b), n = a.length;
  const ma = ra.reduce((s, x) => s + x, 0) / n, mb = rb.reduce((s, x) => s + x, 0) / n;
  let num = 0, da = 0, db = 0;
  for (let i = 0; i < n; i++) {
    const x = ra[i] - ma, y = rb[i] - mb;
    num += x * y; da += x * x; db += y * y;
  }
  return (da > 0 && db > 0) ? num / Math.sqrt(da * db) : 0;
}

// ---- 1. semantic ordering, per language -------------------------------------------------------
const sts = readJsonl(stsFile);
const cos = cosines(sts.map((p) => [p.a, p.b]));
const fails = [];
const rows = [];
for (const lang of ['it', 'en']) {
  const sel = sts.map((p, i) => ({ ...p, cos: cos[i] })).filter((p) => p.lang === lang);
  if (sel.length < 10) { console.error(`too few ${lang} pairs (${sel.length})`); process.exit(2); }
  const bad = sel.filter((p) => !Number.isFinite(p.cos));
  if (bad.length) { console.error(`encoder could not embed ${bad.length} ${lang} pair(s)`); process.exit(2); }
  const rho = spearman(sel.map((p) => p.cos), sel.map((p) => p.gold));
  rows.push([lang.toUpperCase(), sel.length, rho]);
  if (rho < SPEARMAN_FLOOR) fails.push(`Spearman ${lang.toUpperCase()} ${rho.toFixed(3)} < ${SPEARMAN_FLOOR}`);
  if (show) {
    for (const p of sel.sort((x, y) => y.gold - x.gold || y.cos - x.cos))
      console.log(`  [${lang}] gold=${p.gold} cos=${p.cos.toFixed(3)}  "${p.a}" ~ "${p.b}"`);
  }
}

// ---- 2. cross-lingual alignment ---------------------------------------------------------------
// Full EN x IT cosine matrix in one batch; an EN probe is a hit when its own translation is argmax.
const xl = readJsonl(xlingFile);
const grid = [];
for (const e of xl) for (const i of xl) grid.push([e.en, i.it]);
const gcos = cosines(grid);
let hit = 0, diag = 0;
const misses = [];
for (let e = 0; e < xl.length; e++) {
  let best = -2, bestJ = -1;
  for (let i = 0; i < xl.length; i++) {
    const c = gcos[e * xl.length + i];
    if (c > best) { best = c; bestJ = i; }
  }
  const self = gcos[e * xl.length + e];
  diag += self;
  if (bestJ === e) hit++;
  else misses.push(`"${xl[e].en}" -> "${xl[bestJ].it}" (${best.toFixed(2)}) instead of "${xl[e].it}" (${self.toFixed(2)})`);
}
const recall1 = hit / xl.length;
if (recall1 < RECALL1_FLOOR) fails.push(`cross-lingual recall@1 ${(recall1 * 100).toFixed(0)}% < ${RECALL1_FLOOR * 100}%`);

// ---- report -----------------------------------------------------------------------------------
console.log('[enc-quality] shipped ANE2 encoder — measured, not estimated');
for (const [lang, n, rho] of rows)
  console.log(`  Spearman(cos, gold relatedness) ${lang}: ${rho.toFixed(3)}   (n=${n}, floor ${SPEARMAN_FLOOR})`);
console.log(`  cross-lingual recall@1 EN->IT:      ${(recall1 * 100).toFixed(0)}%   (n=${xl.length}, floor ${RECALL1_FLOOR * 100}%)`);
console.log(`  mean cos(translation pair):         ${(diag / xl.length).toFixed(3)}`);
if (misses.length && (show || misses.length <= 5))
  for (const m of misses) console.log(`    xling miss: ${m}`);

if (measure) { console.log('[enc-quality] --measure: reporting only.'); process.exit(0); }
if (fails.length) {
  console.log(`ENCODER BELOW FLOOR (${fails.length}):`);
  for (const f of fails) console.log(`  ✗ ${f}`);
  process.exit(1);
}
console.log('✓ encoder quality within floors.');

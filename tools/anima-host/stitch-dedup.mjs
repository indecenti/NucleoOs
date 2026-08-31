#!/usr/bin/env node
// MOSAICO SPAN-DEDUP gate — two halves, because one alone would be worthless here.
//
//  1. UNIT (stitch-dedup-ctest.c): drives the real dedup helpers directly with the sentence shapes
//     that caused the defect. Needed because whether the defect FIRES end-to-end depends on which two
//     cards the index pairs — on some indexes no pair triggers it and a sweep proves nothing.
//  2. SWEEP (anima.exe over eval_describe.jsonl): asserts the invariant on real traffic — no stitched
//     answer may contain a sentence that restates one already in it. This is the half that would catch
//     a regression on a FUTURE index where the pairing does occur.
//
// The unit half compiles nucleo_anima_l1.c via #include, so it links against the same host sources as
// anima.exe minus that file and host_main.c (which has its own main).
//
//   node tools/anima-host/stitch-dedup.mjs [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, readdirSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join, basename } from 'node:path';

const here  = dirname(fileURLToPath(import.meta.url));
const repo  = join(here, '..', '..');
const anima = join(repo, 'firmware', 'components', 'nucleo_anima');
const build = join(here, 'build');
const exe   = join(here, 'build', 'anima.exe');
const show  = process.argv.includes('--show');

// ---- 1. unit ----------------------------------------------------------------------------------
// Same source set and flags as build.sh, minus nucleo_anima_l1.c (the test #includes it) and
// host_main.c (its main would clash). Kept in step with build.sh by globbing, not hand-listing.
const skip = new Set(['nucleo_anima_online.c', 'nucleo_anima_bench.c', 'nucleo_anima_l1.c']);
const srcs = readdirSync(anima).filter((f) => f.endsWith('.c') && !skip.has(f)).map((f) => join(anima, f));
srcs.push(join(here, 'esp_timer_host.c'), join(here, 'anima_online_stub.c'), join(here, 'stitch-dedup-ctest.c'));

// Resolve gcc the way the sibling gates do: MSYS2 MinGW root first, PATH fallback — and keep the
// MinGW bin dir on PATH so gcc can spawn cc1 and load its DLLs (bare 'gcc' ENOENTs on this box).
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

mkdirSync(build, { recursive: true });
const ctest = join(build, 'stitch-dedup-ctest');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O0', '-g', '-DANIMA_HOST',
  '-I', join(here, 'shim'), '-I', join(anima, 'include'), '-I', anima,
  '-include', join(here, 'shim', 'host_compat.h'),
  ...srcs, '-o', ctest, '-lm',
], { encoding: 'utf8', env });
if (cc.status !== 0) {
  console.error('[stitch-dedup] unit test failed to COMPILE:\n' + (cc.stderr || cc.stdout || (cc.error && cc.error.message) || ''));
  process.exit(2);
}
const unit = spawnSync(ctest, [], { encoding: 'utf8', env });
const unitOut = (unit.stdout || '') + (unit.stderr || '');
if (show || unit.status !== 0) process.stdout.write(unitOut);
const unitOk = unit.status === 0;
console.log(`[stitch-dedup] unit: ${unitOk ? 'all checks passed' : 'FAILED'}`);

// ---- 2. sweep ---------------------------------------------------------------------------------
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
const cases = readFileSync(join(here, 'eval_describe.jsonl'), 'utf8').split(/\r?\n/)
  .filter((l) => l.trim() && !l.startsWith('//')).map((l) => JSON.parse(l));

let lang = 'it';
const lines = [];
for (const c of cases) {
  lines.push('/reset');
  const want = c.lang === 'en' ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(c.q || c.query || c.question);
}
// L1_STITCH_C2 is lowered so MOSAICO actually fires across the fixture: at the shipped 0.80 floor only
// a handful of cases stitch, and a gate that exercises a handful of cases guards almost nothing.
const run = spawnSync(exe, [], {
  input: Buffer.from(lines.join('\n') + '\n', 'utf8'),
  env: { ...process.env, L1_STITCH_C2: '0.70' },
  maxBuffer: 64 * 1024 * 1024,
});
const blocks = run.stdout.toString('utf8').split(/^Q: /m).slice(1);

// Mirror of l1_sent_redundant: >= 80% of a sentence's >=4-char content words already present.
const norm = (s) => ' ' + s.toLowerCase().replace(/[^\p{L}\p{N}]+/gu, ' ').trim() + ' ';
const sentences = (s) => s.split(/(?<=[.!?])\s+/).map((x) => x.trim()).filter(Boolean);
function repeats(reply) {
  const ss = sentences(reply);
  const out = [];
  for (let i = 1; i < ss.length; i++) {
    const have = norm(ss.slice(0, i).join(' '));
    const words = norm(ss[i]).trim().split(/\s+/).filter((w) => w.length >= 4);
    if (words.length < 3) continue;
    const seen = words.filter((w) => have.includes(' ' + w + ' ')).length;
    if (seen >= 0.8 * words.length) out.push(ss[i]);
  }
  return out;
}

// Mirror of l1_lang_vote: -1 unknown, 0 Italian, 1 English, by distinct function words present.
const IT_FN = ['il','lo','la','le','gli','del','dello','della','dei','degli','delle','che','un','una','uno',
  'per','con','non','sono','essere','nel','nella','alla','come','anche','questo','questa','suo','sua','piu','molto'];
const EN_FN = ['the','of','and','is','are','was','were','to','that','for','with','it','as','on','by','from',
  'which','this','these','has','have','can','its','or','they','their','but'];
function langOf(s) {
  const w = norm(s);
  const it = IT_FN.filter((x) => w.includes(' ' + x + ' ')).length;
  const en = EN_FN.filter((x) => w.includes(' ' + x + ' ')).length;
  if (it >= en + 2) return 0;
  if (en >= it + 2) return 1;
  return -1;
}
// A reply is bilingual when two of its sentences are confidently in DIFFERENT languages.
function mixesLanguages(reply) {
  const seen = new Set();
  for (const s of sentences(reply)) { const l = langOf(s); if (l >= 0) seen.add(l); }
  return seen.size > 1;
}

let stitched = 0;
const offenders = [], bilingual = [];
for (let i = 0; i < cases.length; i++) {
  const b = blocks[i] || '';
  const tier  = (b.match(/tier=(\S+)/) || [])[1] || 'none';
  const reply = ((b.match(/reply: (.*)/) || [])[1] || '').trim();
  if (tier === 'L2/stitch') stitched++;
  const rep = repeats(reply);
  if (rep.length) offenders.push([cases[i].q || cases[i].query, tier, rep[0]]);
  if (reply && mixesLanguages(reply)) bilingual.push([cases[i].q || cases[i].query, tier, reply]);
  if (show && tier === 'L2/stitch') console.log(`  [L2] "${cases[i].q || cases[i].query}" -> ${reply}`);
}
console.log(`[stitch-dedup] sweep: ${cases.length} cases, ${stitched} stitched, ${offenders.length} self-repeating, ${bilingual.length} bilingual`);
for (const [q, t, s] of offenders.slice(0, 8)) console.log(`  ✗ "${q}" [${t}] repeats: "${s.slice(0, 90)}"`);
for (const [q, t, s] of bilingual.slice(0, 8)) console.log(`  ✗ "${q}" [${t}] mixes languages: "${s.slice(0, 110)}"`);

if (!unitOk || offenders.length || bilingual.length) process.exit(1);
console.log('✓ no stitched answer says the same thing twice, or in two languages.');

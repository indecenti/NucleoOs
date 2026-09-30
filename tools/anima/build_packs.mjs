#!/usr/bin/env node
// ANIMA pack builder — ONE deterministic command that builds BOTH index packs from the corpus,
// augments them with the AKB4 prefilter trailer, syncs every device SD tree, and verifies coherence.
// Replaces the error-prone manual dance (build_akb2 → copy → augment → copy → hope) that once shipped
// a D=256 index next to a D=192 encoder (L1 silently disabled). Run: `npm run anima:packs`.
//
//   DEVICE pack  (D=192, the on-Cardputer encoder models/anima-it-encoder.bin)
//      → models/anima-it-index.bin  +  deploy/sd, deploy/sd-safe, tools/sd-sim   (+ ASIG)
//   HOST pack    (D=256, the harness encoder models/anima-it-encoder.d256.bin)
//      → tools/anima-host/sd/data/anima/anima-it-index.bin                        (+ ASIG)
//
// Encoders are NOT touched (they are stable and correctly placed per tree); only the indexes are
// (re)built, augmented and placed. The pack-coherence guard (check_pack.mjs) is the final assertion.
import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdirSync, readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', b: '\x1b[1m', d: '\x1b[2m', x: '\x1b[0m' };
const R = (...p) => join(repo, ...p);

const PY = process.env.PYTHON || 'python';
const ENV = { ...process.env, PYTHONUTF8: '1', PYTHONIOENCODING: 'utf-8' };

function py(script, { env = {}, label } = {}) {
  process.stdout.write(`${C.d}· ${label} ...${C.x}\n`);
  const r = spawnSync(PY, [R('tools', 'anima', script)], { cwd: repo, env: { ...ENV, ...env }, encoding: 'utf8' });
  const out = (r.stdout || '') + (r.stderr || '');
  if (r.status !== 0) { console.error(`${C.r}FAILED ${script}:${C.x}\n${out.slice(-3000)}`); process.exit(1); }
  // surface the one-line build summary
  const m = out.match(/\[anima\] AKB2 bilingual:[^\n]*/);
  if (m) console.log(`  ${C.d}${m[0]}${C.x}`);
  return out;
}
function augment(path, label) {
  process.stdout.write(`${C.d}· augment ${label} ...${C.x}\n`);
  const r = spawnSync(PY, [R('tools', 'anima', 'augment_akb4.py'), path], { cwd: repo, env: ENV, encoding: 'utf8' });
  const out = (r.stdout || '') + (r.stderr || '');
  if (r.status !== 0) { console.error(`${C.r}FAILED augment ${path}:${C.x}\n${out.slice(-2000)}`); process.exit(1); }
  return out;
}
function place(srcAbs, treeDirs) {
  for (const t of treeDirs) {
    const dstDir = R(...t.split('/'));
    if (!existsSync(dstDir)) mkdirSync(dstDir, { recursive: true });
    copyFileSync(srcAbs, join(dstDir, 'anima-it-index.bin'));
    // The provenance sidecar (corpus+encoder hash) must travel with the (augmented) index so the
    // pack-coherence guard can detect a stale fixture. build_akb2 wrote <srcAbs>.prov next to it.
    if (existsSync(srcAbs + '.prov')) copyFileSync(srcAbs + '.prov', join(dstDir, 'anima-it-index.bin.prov'));
    console.log(`  ${C.d}→ ${t}/anima-it-index.bin${C.x}`);
  }
}

console.log(`${C.b}=== ANIMA pack builder ===${C.x}`);

// --- HOST pack (D=256) — the GATE FIXTURE. Rebuilt ONLY with --host, because regenerating it from a
// changed corpus reshuffles the flat k-means index and flips borderline gate cases (the route-golden
// snapshot + skill evals are calibrated to a specific build). So a host rebuild is a DELIBERATE act
// that must be followed by re-validating the goldens. Default builds the device packs only. ----------
const wantHost = process.argv.includes('--host') || process.argv.includes('--host-only');
if (wantHost) {
  const d256 = R('models', 'anima-it-encoder.d256.bin');
  const hostIdx = R('tools', 'anima-host', 'sd', 'data', 'anima', 'anima-it-index.bin');
  if (!existsSync(d256)) { console.error(`${C.r}missing host encoder ${d256}${C.x}`); process.exit(1); }
  console.log(`${C.b}[1/3] HOST pack (D=256) — gate fixture (--host)${C.x}`);
  // Place the ENCODER too, not just the index. Building a 256-dim index into a tree that still holds
  // the 192-dim device encoder produces a pack the C core REJECTS at load (nucleo_anima_l1.c), which
  // switches L1 off SILENTLY — the gate then scores an OS with no offline brain and reports plausible
  // nonsense. That mismatch is exactly how this fixture ended up wrong, so the command that rebuilds
  // it must leave the tree self-consistent by construction, not by the operator remembering a cp.
  const hostEnc = R('tools', 'anima-host', 'sd', 'data', 'anima', 'anima-it-encoder.bin');
  mkdirSync(dirname(hostEnc), { recursive: true });
  copyFileSync(d256, hostEnc);
  console.log(`  ${C.d}→ encoder d256 placed in the gate fixture${C.x}`);
  // ANIMA_KMEANS=det: the fixture is built with the reproducible k-means, and the FINAL (augmented) bytes
  // are fingerprinted into the .prov — so any machine or CI can rebuild it and prove it got the same pack
  // the goldens were calibrated on (check_pack verifies index_sha).
  py('build_akb2.py', { env: { ANIMA_ENC: d256, ANIMA_INDEX_OUT: hostIdx, ANIMA_KMEANS: 'det' }, label: 'build host index (reproducible k-means)' });
  augment(hostIdx, 'host index');
  const provP = hostIdx + '.prov';
  const prov = JSON.parse(readFileSync(provP, 'utf8'));
  prov.index_sha = createHash('sha256').update(readFileSync(hostIdx)).digest('hex');
  writeFileSync(provP, JSON.stringify(prov, Object.keys(prov).sort(), 0).replace(/,"/g, ',\n"').replace(/^\{/, '{\n').replace(/\}$/, '\n}') + '\n');
  console.log(`  ${C.d}→ fixture fingerprint ${prov.index_sha.slice(0, 12)}… recorded in the .prov${C.x}`);
  // The fixture is MORE than the L1 index: the typed-fact encyclopedia (AKB5 shards + manifest) and the
  // learned facets are what "quando è nato Einstein" / "in che continente è il Brasile" / typed-nl read.
  // A fresh clone had neither — the gate then scored an ANIMA with no typed knowledge (it abstained: safe,
  // but every recall golden flipped). Build them here too, reproducibly, from tracked sources only.
  const hostAnima = R('tools', 'anima-host', 'sd', 'data', 'anima');
  const staged = readdirSync(R('tools', 'anima', 'knowledge.staged')).filter((f) => f.endsWith('.jsonl')).map((f) => R('tools', 'anima', 'knowledge.staged', f));
  py('build_akb5.py', { env: { ANIMA_ENC: d256, ANIMA_AKB5_DIR: join(hostAnima, 'akb5'), ANIMA_KMEANS: 'det', ANIMA_EXTRA: staged.join(',') }, label: 'build host AKB5 (typed encyclopedia, reproducible k-means)' });
  const learnedSrc = R('deploy', 'sd', 'data', 'anima', 'learned');          // what ships to the device = canonical
  for (const rel of ['facets.it.jsonl', 'facets.en.jsonl', 'knowledge.ledger.jsonl', 'evo/occ.jsonl', 'evo/subclass.jsonl']) {
    const src = join(learnedSrc, ...rel.split('/'));
    if (!existsSync(src)) continue;
    const dst = join(hostAnima, 'learned', ...rel.split('/'));
    mkdirSync(dirname(dst), { recursive: true });
    copyFileSync(src, dst);
  }
  // The HDC triple store (mind.*: "Albert Einstein · born · 14 marzo 1879", capitals, continents) —
  // written by extract_triples.py; tracked in the simulator tree.
  for (const f of ['mind.it.jsonl', 'mind.en.jsonl']) {
    const src = R('tools', 'sd-sim', 'data', 'anima', 'learned', f);
    if (existsSync(src)) copyFileSync(src, join(hostAnima, 'learned', f));
  }
  // The offline IT<->EN dictionary the translate skill reads (tracked, identical in sd-sim and deploy/sd).
  // A fresh clone had none: translate-check scored 21/67 and dict-stress recalled 0/0.
  for (const f of ['dict-it-en.tsv', 'dict-en-it.tsv']) {
    const src = R('tools', 'sd-sim', 'data', 'anima', f);
    if (existsSync(src)) copyFileSync(src, join(hostAnima, f));
  }
  console.log(`  ${C.d}→ learned facets (deploy/sd) + HDC triples + IT<->EN dictionary (sd-sim) placed in the fixture${C.x}`);
} else {
  console.log(`${C.d}[1/3] HOST pack — skipped (gate fixture; pass --host to regenerate + re-validate goldens)${C.x}`);
}

// --- DEVICE pack (D=192) — default OUT auto-mirrors the AKB3 body to the device trees ---------------
// --host-only: rebuild just the gate fixture (e.g. on a fresh clone) without touching the shipped packs.
if (process.argv.includes('--host-only')) {
  console.log(`${C.d}[2/3] DEVICE pack — skipped (--host-only)${C.x}`);
} else {
  console.log(`${C.b}[2/3] DEVICE pack (D=192)${C.x}`);
  const modelsIdx = R('models', 'anima-it-index.bin');
  py('build_akb2.py', { label: 'build device index (+ auto-mirror to device trees)' }); // default ANIMA_ENC / OUT
  augment(modelsIdx, 'device index');
  // overwrite the (pre-augment) auto-mirrored copies with the augmented ASIG version
  place(modelsIdx, ['deploy/sd/data/anima', 'deploy/sd-safe/data/anima', 'tools/sd-sim/data/anima']);
}

// --- Verify -----------------------------------------------------------------------------------------
console.log(`${C.b}[3/3] coherence guard${C.x}`);
const g = spawnSync('node', [R('tools', 'anima', 'check_pack.mjs')], { cwd: repo, stdio: 'inherit' });
process.exit(g.status ?? 1);

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
import { copyFileSync, existsSync, mkdirSync } from 'node:fs';
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
const wantHost = process.argv.includes('--host');
if (wantHost) {
  const d256 = R('models', 'anima-it-encoder.d256.bin');
  const hostIdx = R('tools', 'anima-host', 'sd', 'data', 'anima', 'anima-it-index.bin');
  if (!existsSync(d256)) { console.error(`${C.r}missing host encoder ${d256}${C.x}`); process.exit(1); }
  console.log(`${C.b}[1/3] HOST pack (D=256) — gate fixture (--host)${C.x}`);
  py('build_akb2.py', { env: { ANIMA_ENC: d256, ANIMA_INDEX_OUT: hostIdx }, label: 'build host index' });
  augment(hostIdx, 'host index');
} else {
  console.log(`${C.d}[1/3] HOST pack — skipped (gate fixture; pass --host to regenerate + re-validate goldens)${C.x}`);
}

// --- DEVICE pack (D=192) — default OUT auto-mirrors the AKB3 body to the device trees ---------------
console.log(`${C.b}[2/3] DEVICE pack (D=192)${C.x}`);
const modelsIdx = R('models', 'anima-it-index.bin');
py('build_akb2.py', { label: 'build device index (+ auto-mirror to device trees)' }); // default ANIMA_ENC / OUT
augment(modelsIdx, 'device index');
// overwrite the (pre-augment) auto-mirrored copies with the augmented ASIG version
place(modelsIdx, ['deploy/sd/data/anima', 'deploy/sd-safe/data/anima', 'tools/sd-sim/data/anima']);

// --- Verify -----------------------------------------------------------------------------------------
console.log(`${C.b}[3/3] coherence guard${C.x}`);
const g = spawnSync('node', [R('tools', 'anima', 'check_pack.mjs')], { cwd: repo, stdio: 'inherit' });
process.exit(g.status ?? 1);

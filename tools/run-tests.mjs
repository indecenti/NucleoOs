#!/usr/bin/env node
// Central CLI test runner — reads the ONE catalog tools/test-registry.json and runs any slice of the test
// universe (all / only-NL / a category / by id substring), printing a green/red summary. Same source of
// truth as the Tkinter cockpit (tools/test-lab/test_lab.py) and the docs, so there is exactly one list.
//
//   node tools/run-tests.mjs                 # every registered test
//   node tools/run-tests.mjs --nl            # only the natural-language tests (the heart of the system)
//   node tools/run-tests.mjs --cat nl-hallucination
//   node tools/run-tests.mjs --grep boundary # tests whose id/label matches
//   node tools/run-tests.mjs --list          # just list, don't run
// NB: `npm run anima:gate` remains the canonical all-green pre-flight; this runner is the wider catalog
// (it also covers the app tests) and is handy for slicing by category.
import { spawnSync } from 'node:child_process';
import { readFileSync, rmSync, appendFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..');

// HERMETIC (mirror of gate.mjs): clear the volatile per-run SD state before EVERY test so a stateful
// runner (realistic/profile/teach write profile.tsv/user.tsv/events) can't pollute a later one
// (e.g. akb5-content). Without this the batch order changes outcomes — not deterministic.
const clearVolatile = () => {
  const sd = join(here, 'anima-host', 'sd', 'data', 'anima');
  for (const f of ['units.txt', 'session.txt', 'telemetry.ndjson', 'user.tsv', 'user.vec',
                   'user.tsv.tmp', 'user.vec.tmp', 'profile.tsv', 'profile.tsv.tmp',
                   'events.tsv', 'events.txt', 'calendar.tsv']) {
    try { rmSync(join(sd, f)); } catch { /* absent is fine */ }
  }
};
const reg = JSON.parse(readFileSync(join(repo, 'tools', 'test-registry.json'), 'utf8'));
const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', x: '\x1b[0m' };
const argv = process.argv.slice(2);
const has = (f) => argv.includes(f);
const val = (f) => { const i = argv.indexOf(f); return i >= 0 ? argv[i + 1] : null; };

let tests = reg.tests;
if (has('--anima')) tests = tests.filter((t) => t.anima);   // only the deterministic ANIMA cascade (no models, no apps)
if (has('--nl')) tests = tests.filter((t) => t.nl);
const cat = val('--cat'); if (cat) tests = tests.filter((t) => t.category === cat);
const grep = val('--grep'); if (grep) tests = tests.filter((t) => (t.id + ' ' + t.label).toLowerCase().includes(grep.toLowerCase()));
const catLabel = (id) => (reg.categories.find((c) => c.id === id) || {}).label || id;

if (has('--list') || !tests.length) {
  console.log(`${C.b}${tests.length} test${C.x}`);
  let last = '';
  for (const t of tests.sort((a, b) => a.category.localeCompare(b.category))) {
    if (t.category !== last) { console.log(`\n${C.b}${catLabel(t.category)}${t.nl ? ' 🗣' : ''}${C.x}`); last = t.category; }
    console.log(`  ${t.label}  ${C.d}${t.cmd} ${t.args.join(' ')}${C.x}`);
  }
  process.exit(0);
}

// Build the exe once up front — most ANIMA runners need tools/anima-host/build/anima.exe.
process.stdout.write(`${C.d}ensuring anima.exe …${C.x}\r`);
spawnSync('node', ['tools/anima-host/anima.mjs', '--ensure'], { cwd: repo, encoding: 'utf8' });

const results = [];
for (const t of tests) {
  process.stdout.write(`  ${C.d}running ${t.label} …${C.x}\r`);
  clearVolatile();                       // each test starts from clean SD state
  const p = spawnSync(t.cmd, t.args, { cwd: repo, encoding: 'utf8',
    env: { ...process.env, PYTHONUTF8: '1', PYTHONIOENCODING: 'utf-8' } });
  const out = (p.stdout || '') + (p.stderr || '');
  const skip = /SKIP/.test(out) && p.status === 0;
  const pass = p.status === 0;
  const last = out.trim().split(/\r?\n/).filter((l) => l.trim()).pop() || '';
  results.push({ t, pass, skip, last, full: out });
  const tag = skip ? `${C.y}SKIP` : pass ? `${C.g}PASS` : `${C.r}FAIL`;
  console.log(`  ${tag}${C.x}  ${t.label.padEnd(34)} ${C.d}${last.replace(/\x1b\[[0-9;]*m/g, '').slice(0, 70)}${C.x}`);
}

const fail = results.filter((r) => !r.pass);
const green = results.filter((r) => r.pass && !r.skip).length;
const skipped = results.filter((r) => r.skip).length;
console.log('\n' + (fail.length
  ? `${C.r}${C.b}${fail.length} FAILED:${C.x} ${fail.map((r) => r.t.label).join(', ')}`
  : `${C.g}${C.b}ALL ${green} PASS${C.x}${skipped ? ` ${C.y}(+${skipped} skipped)${C.x}` : ''}`));

// ---- HEALTH MONITOR: roll the per-test metrics (declared in the registry) up into ANIMA's headline health,
// print it, and append a snapshot to the SAME history the GUI's trends read. Run on a full sweep only.
if (results.length >= 20) {   // a substantial run (--anima / --nl / --all): show health + record a trend point
  const out = new Map(results.map((r) => [r.t.id, (r.full || '')]));
  const per = {};
  for (const t of reg.tests) {
    out.set(t.id, out.get(t.id) ?? '');
    for (const m of (t.metrics || [])) {
      const ms = [...(out.get(t.id) || '').matchAll(new RegExp(m.re, 'ig'))];
      if (!ms.length) continue;
      const v = m.agg === 'sum'
        ? ms.reduce((a, mm) => a + mm.slice(1).reduce((s, g) => s + (g && /^\d+$/.test(g) ? +g : 0), 0), 0)
        : +ms[ms.length - 1][1];
      per[m.k] = (per[m.k] || 0) + v;
    }
  }
  const health = {};
  for (const h of (reg.health || [])) {
    health[h.id] = h.special === 'green' ? [green, results.filter((r) => !r.skip).length] : (per[h.metric] || 0);
  }
  const fmt = (n) => n.toLocaleString('it-IT');
  console.log(`\n${C.b}SALUTE ANIMA${C.x}`);
  for (const h of (reg.health || [])) {
    const v = health[h.id];
    if (h.special === 'green') { console.log(`  ${h.label.padEnd(18)} ${v[0]}/${v[1]}`); continue; }
    const ok = h.goal === undefined ? null : v === h.goal;
    const col = ok === null ? C.b : (ok ? C.g : C.r);
    console.log(`  ${h.label.padEnd(18)} ${col}${fmt(v)}${C.x}${h.goal !== undefined ? (ok ? ' ✓' : ' ⚠') : ''}`);
  }
  try {
    appendFileSync(join(repo, 'tools', 'test-lab', 'history.jsonl'),
      JSON.stringify({ ts: new Date().toISOString().slice(0, 19), health }) + '\n');
  } catch { /* best-effort */ }
}
process.exit(fail.length ? 1 : 0);

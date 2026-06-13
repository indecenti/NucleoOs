#!/usr/bin/env node
// ANIMA routing regression gate — drives the REAL compiled cascade (anima.exe) and checks
// where each query lands. Two modes:
//   node tools/anima-host/route-check.mjs            # --expect: routing accuracy vs eval labels
//   node tools/anima-host/route-check.mjs --snapshot # characterization: write/diff a golden file
//
// Unlike tools/anima/eval.py (a Python retrieval-ceiling probe over the encoder), this exercises
// the C orchestrator end-to-end on host (online tiers stubbed = device with Wi-Fi off). It is the
// anti-regression net for orchestrator refactors: capture a golden snapshot, change code, re-run,
// review the diff. Zero unexpected diffs = no regression; an intended diff (a bug fix) is visible.
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const exe  = join(here, 'build', 'anima.exe');
const golden = join(here, 'route-golden.json');

const argv = process.argv.slice(2);
const snapshot = argv.includes('--snapshot');
const update   = argv.includes('--update');
const qfileArg = argv.find(a => a.endsWith('.jsonl'));
const qfile = qfileArg || join(repo, 'tools', 'anima', 'eval_routing.jsonl');

if (!existsSync(exe)) {
  console.error('anima.exe missing — run `npm run anima:build` first.');
  process.exit(2);
}

// Parse the JSONL query set (skip // comments and blanks).
const items = readFileSync(qfile, 'utf8').split(/\r?\n/)
  .filter(l => l.trim() && !l.startsWith('//'))
  .map(l => JSON.parse(l));

// Build one REPL stream: /reset before each query (clean session), /it|/en to set language.
// Only the query line emits a stdout block; /reset and /it|/en print to stderr.
let lang = 'it';
const lines = [];
for (const it of items) {
  lines.push('/reset');
  const want = (it.lang === 'en') ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(it.q);
}
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 16 * 1024 * 1024 });
const out = r.stdout.toString('utf8');

// Parse the printed result blocks (one per query, in order).
// Q: <q>\n [ (interpretato: "...")\n ] tier=.. action=.. intent=.. conf=N[..]\n [arg=..] [state=..] reply: ..
const blocks = out.split(/^Q: /m).slice(1).map(b => 'Q: ' + b);
function parse(b) {
  const q = (b.match(/^Q: (.*)/) || [])[1] || '';
  const corrected = (b.match(/\(interpretato: "(.*)"\)/) || [])[1] || '';
  const tier   = (b.match(/tier=(\S+)/) || [])[1] || 'none';
  const action = (b.match(/action=(\S+)/) || [])[1] || 'none';
  const intent = (b.match(/intent=(\S*)/) || [])[1] || '';
  const conf   = parseInt((b.match(/conf=(\d+)/) || [])[1] || '0', 10);
  return { q, corrected, tier, action, intent, conf };
}

// Map the cascade's (tier,action,intent) to the user-facing mechanism the routing eval labels.
function domainOf({ tier, action, intent }) {
  if (intent === 'clarify') return 'clarify';
  if (tier === 'L1/fact' || tier === 'L2/stitch' || ['l1', 'hdc', 'combinator', 'mosaico', 'pcg'].includes(intent)) return 'knowledge';
  if (['calc', 'percent', 'convert', 'ohm', 'base', 'prime', 'roman', 'geo', 'phys'].includes(intent)) return 'calc';
  if (['capabilities', 'whoami'].includes(intent)) return 'faq';
  if (action === 'tool')   return 'tool';
  if (action === 'system') return 'system';
  if (action === 'launch') return 'app';
  if (action === 'answer') return 'faq';
  return 'none';
}

const rows = blocks.map(parse).map(p => ({ ...p, domain: domainOf(p) }));

if (snapshot) {
  // Characterization snapshot: q -> stable routing signature (ignore conf jitter -> bucket it).
  const snap = {};
  for (const p of rows) snap[p.q] = `${p.domain}|${p.tier}|${p.action}|${p.intent}|${p.corrected}`;
  if (update || !existsSync(golden)) {
    writeFileSync(golden, JSON.stringify(snap, null, 1));
    console.log(`[snapshot] wrote ${Object.keys(snap).length} rows -> ${golden}`);
    process.exit(0);
  }
  const prev = JSON.parse(readFileSync(golden, 'utf8'));
  let diffs = 0;
  for (const q of Object.keys(snap)) {
    if (prev[q] !== snap[q]) {
      diffs++;
      console.log(`  ~ ${q}\n      was: ${prev[q] ?? '(new)'}\n      now: ${snap[q]}`);
    }
  }
  for (const q of Object.keys(prev)) if (!(q in snap)) { diffs++; console.log(`  - ${q} (removed)`); }
  console.log(diffs ? `[snapshot] ${diffs} routing change(s) vs golden` : '[snapshot] no routing changes vs golden ✓');
  process.exit(diffs ? 1 : 0);
}

// --expect: routing accuracy vs the eval's own domain labels.
let ok = 0, miss = 0;
const fails = [];
for (let i = 0; i < items.length; i++) {
  const want = items[i].domain;
  const got = rows[i] ? rows[i].domain : '(no output)';
  if (want === undefined) continue;
  if (got === want) ok++;
  else { miss++; fails.push({ q: items[i].q, want, got, sig: rows[i] }); }
}
console.log(`[route] ${ok}/${ok + miss} correct (${(100 * ok / (ok + miss)).toFixed(1)}%)`);
for (const f of fails) console.log(`  [MISS] "${f.q}"  expected ${f.want} -> ${f.got}  (${f.sig?.intent}/${f.sig?.action})`);
process.exit(0);

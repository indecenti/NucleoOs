#!/usr/bin/env node
// ANIMA reasoning-layer gate — equation solver, multi-step chains, cross-turn registers, the conscience.
//
// Drives the REAL compiled firmware (anima.exe) over tools/anima-host/reason-cases.jsonl and checks every
// answer. Single-turn cases are isolated by /reset; {turns:[...]} cases run as ONE session (no reset in
// between) so register memory ("chiamalo A" … "A + B") is actually exercised end to end.
//
//   npm run anima:build
//   node tools/anima-host/reason-check.mjs
//
// Exit != 0 on any wrong value or any false-positive (a math/reason frame claiming an ordinary sentence).
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const casesFile = join(here, 'reason-cases.jsonl');
// intents a reason/math frame uses — for "reject" cases, NONE of these may fire.
const MATH_INTENTS = new Set(['base', 'calc', 'prime', 'roman', 'convert', 'percent', 'ohm', 'geo', 'phys', 'math']);

if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

const cases = readFileSync(casesFile, 'utf8').split(/\r?\n/)
  .filter(l => l.trim() && !l.startsWith('//')).map(l => JSON.parse(l));

// Flatten to an ordered list of probes (one stdout "Q:" block each), inserting /reset + /lang stream
// commands. A single-turn case is one probe after a /reset; a multi-turn case is N probes after ONE reset.
const lines = [];
const probes = [];            // {q, expect?, reject?, lang, label}
let lang = 'it';
const ensureLang = (want) => { want = want === 'en' ? 'en' : 'it'; if (want !== lang) { lines.push('/' + want); lang = want; } };
for (const c of cases) {
  lines.push('/reset');
  ensureLang(c.lang);
  if (Array.isArray(c.turns)) {
    c.turns.forEach((t, i) => { lines.push(t.q); probes.push({ q: t.q, expect: t.expect, reject: t.reject, lang: c.lang, label: `${c.turns[0].q} » ${t.q}` }); });
  } else {
    lines.push(c.q); probes.push({ q: c.q, expect: c.expect, reject: c.reject, lang: c.lang, label: c.q });
  }
}

const run = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 32 * 1024 * 1024 });
const blocks = run.stdout.toString('utf8').split(/^Q: /m).slice(1);
const rows = blocks.map(b => ({
  intent: (b.match(/intent=(\S*)/) || [])[1] || '',
  reply: ((b.match(/reply: ([\s\S]*?)\n\s*\n/) || b.match(/reply: (.*)/) || [])[1] || '').replace(/\s+/g, ' ').trim(),
}));

const norm = s => String(s).toLowerCase().replace(/\s+/g, ' ').trim();
let ok = 0; const wrong = [], falsePos = [];
for (let i = 0; i < probes.length; i++) {
  const p = probes[i], row = rows[i] || { intent: '', reply: '' };
  if (p.reject) {
    if (MATH_INTENTS.has(row.intent)) falsePos.push({ p, row }); else ok++;
  } else if (p.expect === undefined) {
    ok++;                                                   // a setup turn with no assertion
  } else if (norm(row.reply).includes(norm(p.expect))) {
    ok++;
  } else {
    wrong.push({ p, got: row.reply, intent: row.intent });
  }
}

console.log(`\n[reason] firmware: ${ok}/${probes.length} correct  (${wrong.length} wrong, ${falsePos.length} false-positive)`);
for (const w of wrong)    console.log(`  [WRONG]  [${w.p.lang}] "${w.p.label}"  expected ~"${w.p.expect}"  got(${w.intent}): ${w.got || '(empty)'}`);
for (const f of falsePos) console.log(`  [FALSE+] [${f.p.lang}] "${f.p.label}"  reason frame "${f.row.intent}" wrongly claimed it: ${f.row.reply}`);

const fatal = wrong.length + falsePos.length;
console.log(fatal ? `\n${fatal} FAILURE(S)` : `\n[reason] ALL GREEN — ${ok}/${probes.length} exact, 0 false-positive.`);
process.exit(fatal ? 1 : 0);

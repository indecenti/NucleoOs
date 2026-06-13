#!/usr/bin/env node
// TRANSLATE CHECK — content + routing + zero-false-positive gate for the offline IT<->EN translator
// (nucleo_anima_translate.c), driven through the REAL cascade (anima.exe). Unlike skill-probe (which only
// checks routing), this asserts the TRANSLATED TEXT, so a wrong/empty dictionary answer fails the gate.
//
// Reads tools/anima/eval_translate.jsonl ({ q, lang?, has?|intent?|not_translate?, nothas? }) and for each:
//   has           -> reply must contain all substrings (ci); also asserts intent === "translate"
//   intent         -> routed intent must equal this (used for decline/ask cases that stay "translate")
//   not_translate  -> routed intent must NOT be "translate" (the zero-false-positive property)
//   nothas         -> reply must contain none of these substrings
// Prints "[translate-check] N/M pass" + every failure. Exit != 0 on any failure.
//
//   node tools/anima-host/translate-check.mjs [path-to.jsonl] [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const file = process.argv.find((a, i) => i >= 2 && !a.startsWith('--'))
  || join(here, '..', '..', 'tools', 'anima', 'eval_translate.jsonl');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
if (!existsSync(file)) { console.error(`cases not found: ${file}`); process.exit(2); }

const items = readFileSync(file, 'utf8').split(/\r?\n/)
  .filter((l) => l.trim() && !l.startsWith('//')).map((l) => JSON.parse(l));

// Drive the exe in one batch: /reset between cases, switch language as needed.
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
const parse = (b) => ({
  intent: (b.match(/intent=(\S*)/) || [])[1] || '',
  reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
});
const arr = (v) => (v == null ? [] : Array.isArray(v) ? v : [v]);
const has = (reply, sub) => reply.toLowerCase().includes(String(sub).toLowerCase());

const show = process.argv.includes('--show');
const fails = [];
for (let i = 0; i < items.length; i++) {
  const it = items[i], p = parse(blocks[i] || '');
  const reasons = [];
  if (it.not_translate) {
    if (p.intent === 'translate') reasons.push('routed to translate (false positive)');
  } else {
    const wantIntent = it.intent || 'translate';
    if (p.intent !== wantIntent) reasons.push(`intent ${p.intent || '(none)'} != ${wantIntent}`);
    for (const sub of arr(it.has)) if (!has(p.reply, sub)) reasons.push(`missing "${sub}"`);
  }
  for (const sub of arr(it.nothas)) if (has(p.reply, sub)) reasons.push(`contains "${sub}"`);
  const ok = reasons.length === 0;
  if (!ok) fails.push([it.q, p.intent || '(none)', p.reply.slice(0, 52), reasons.join('; ')]);
  if (show) console.log(`${ok ? '  ok  ' : 'FAIL  '} "${it.q}" -> ${p.intent || '(none)'} : ${p.reply.slice(0, 60)}`);
}

const pass = items.length - fails.length;
console.log(`[translate-check] ${pass}/${items.length} pass`);
if (fails.length) {
  console.log('FAILURES:');
  for (const f of fails) console.log(`  ✗ "${f[0]}" got=${f[1]} reply="${f[2]}" — ${f[3]}`);
  process.exit(1);
}
console.log('✓ all translations + routing as expected');

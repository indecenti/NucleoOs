#!/usr/bin/env node
// MATH DIALOG — multi-turn CHAINED-calculation dialogues over the real pipeline (anima.exe). Unlike
// skill-probe/halluc-probe (which /reset before EVERY case), here a dialogue is a CONVERSATION: NO reset
// between its turns, so the cross-turn named registers carry (`chiamalo a` / `call it a` -> `a più 10`).
// /reset fires only BETWEEN dialogues. Each turn passes iff its reply CONTAINS the expected number.
//
// Input JSONL, one dialogue per line:
//   {"lang":"it","name":"chain-1","turns":[{"q":"15 per 6","e":"90"},{"q":"chiamalo a","e":"90"},...]}
// `e` = the exact number substring expected in the reply ("Fa 90." / "It's 90." / "Salvato: a = 90.").
//
//   node tools/anima-host/math-dialog.mjs <dialogs.jsonl> [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const file = process.argv[2];
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
if (!file || !existsSync(file)) { console.error('usage: math-dialog.mjs <dialogs.jsonl> [--show]'); process.exit(2); }

const dialogs = readFileSync(file, 'utf8').split(/\r?\n/)
  .filter(l => l.trim() && !l.startsWith('//')).map(l => JSON.parse(l));

// Feed every dialogue in ONE exe session: /reset + language switch before each dialogue, then its turns
// verbatim (no reset between turns -> the register context survives). Only the turn lines produce Q: blocks.
let lang = 'it';
const lines = [];
const flat = [];                       // parallel list of {d, t, turn} for block alignment
for (let di = 0; di < dialogs.length; di++) {
  const d = dialogs[di];
  lines.push('/reset');
  const want = d.lang === 'en' ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  for (let ti = 0; ti < d.turns.length; ti++) { lines.push(d.turns[ti].q); flat.push({ di, ti }); }
}
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
const replyOf = (b) => ((b.match(/reply: (.*)/) || [])[1] || '').trim();

const show = process.argv.includes('--show');
const fails = [];
let turnsTotal = 0, turnsOk = 0;
const dlgBad = new Set();
for (let i = 0; i < flat.length; i++) {
  const { di, ti } = flat[i];
  const turn = dialogs[di].turns[ti];
  const reply = replyOf(blocks[i] || '');
  const want = String(turn.e);
  // pass iff the expected number appears as a token in the reply (word-ish boundaries so "5" != "50")
  const ok = new RegExp('(^|[^0-9.])' + want.replace(/[.]/g, '\\.') + '([^0-9]|$)').test(reply);
  turnsTotal++;
  if (ok) turnsOk++;
  else { fails.push([dialogs[di].name || ('#' + di), ti + 1, turn.q, want, reply.slice(0, 50)]); dlgBad.add(di); }
  if (show) console.log(`${ok ? '  ok  ' : 'FAIL  '} [${dialogs[di].name || di} t${ti + 1}] "${turn.q}" want=${want} got="${reply.slice(0, 40)}"`);
}

const dlgOk = dialogs.length - dlgBad.size;
console.log(`[math-dialog] ${file.split(/[\\/]/).pop()} — ${dlgOk}/${dialogs.length} dialogues clean, ${turnsOk}/${turnsTotal} turns correct`);
if (fails.length) {
  console.log('FAILURES:');
  for (const f of fails) console.log(`  ✗ [${f[0]} turn ${f[1]}] "${f[2]}" want=${f[3]} got="${f[4]}"`);
  process.exit(1);
}
console.log('✓ every chained calculation correct across all dialogues');

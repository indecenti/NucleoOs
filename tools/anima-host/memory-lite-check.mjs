#!/usr/bin/env node
// MEMORY-LITE-CHECK — the lean personal-memory path (nucleo_anima_query_memory) the web server falls back to
// when its heap can't carve the full cascade's 30 KB worker (the Cardputer ADV with the web OS connected:
// largest free block ~13 KB, so /api/anima used to answer "busy" to everything). Pins three things:
//   * SAME ANSWER: for every memory utterance the lean path answers exactly what the full cascade answers
//     (same tiers, same functions — only the deep L1/AKB5/HDC walk is skipped);
//   * it serves the memory tiers: profile set/recall, teach, user-taught recall (IT + EN);
//   * it claims NOTHING else: commands, reminders, launches, knowledge, time stay "not mine" (the caller
//     answers busy and the browser's own engine takes them, device actions via /api/anima/act).
// Stateful: it writes ./sd/data/anima/{profile,user}.* and removes them (and restores any prior store) after.
//
//   node tools/anima-host/memory-lite-check.mjs [--show]
import { spawnSync } from 'node:child_process';
import { existsSync, rmSync, renameSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const dir = join(here, 'sd', 'data', 'anima');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }
const show = process.argv.includes('--show');

// Each scenario runs in a fresh session: lines are REPL lines ("/memory q" = the lean path, plain = full).
const MEMORY = [   // [lang, utterance] — taught/recalled in order, so recalls have something to find
  ['it', 'mi chiamo Marco'],
  ['it', 'come mi chiamo'],
  ['it', 'ricorda che la sala riunioni è al terzo piano'],
  ['it', 'dove si trova la sala riunioni'],
  ['it', 'ricorda che il mio colore preferito e il blu'],
  ['it', 'qual è il mio colore preferito'],
  ['en', 'my name is Anna'],
  ['en', 'what is my name'],
  ['en', 'remember that the server room is in the basement'],
  ['en', 'where is the server room'],
];
const NOT_MINE = [
  ['it', 'che ore sono'], ['it', 'apri le foto'], ['it', 'alza il volume al 30'],
  ['it', 'ricordami il dentista domani alle 16'], ['it', 'crea un file spesa.txt'],
  ['it', 'chi era Albert Einstein'], ['it', 'quanto fa 12 per 8'], ['it', 'capitale della francia'],
  ['it', 'meteo brescia domani'], ['en', 'open the calculator'], ['en', 'who was Isaac Newton'],
];

const STATE = ['profile.tsv', 'profile.tsv.tmp', 'user.tsv', 'user.vec', 'user.tsv.tmp', 'user.vec.tmp', 'session.txt'];
const bak = (f) => join(dir, f + '.memlite.bak');
const wipe = () => { for (const f of STATE) try { rmSync(join(dir, f)); } catch { /* absent */ } };

function run(lines) {
  const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
  return r.stdout.toString('utf8').split(/^Q: /m).slice(1).map((b) => ({
    notMine: /\(memory: not mine\)/.test(b),
    intent: (b.match(/intent=(\S*)/) || [])[1] || '',
    reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
  }));
}
// One session per path: the same utterances in the same order, lean (/memory) vs full cascade.
const seq = (prefix) => { const out = ['/reset']; let lang = 'it';
  for (const [l, q] of MEMORY) { if (l !== lang) { out.push('/' + l); lang = l; } out.push(prefix + q); } return out; };

for (const f of STATE) if (existsSync(join(dir, f))) renameSync(join(dir, f), bak(f));
const fails = [];
let n = 0;
try {
  wipe();
  const lite = run(seq('/memory '));
  wipe();
  const full = run(seq(''));
  MEMORY.forEach(([, q], i) => {
    n++;
    const a = lite[i] || {}, b = full[i] || {};
    let why = '';
    if (a.notMine) why = 'lean path did not take a memory utterance';
    else if (a.reply !== b.reply) why = `lean "${a.reply}" != full "${b.reply}"`;
    else if (!a.reply || /^(non lo so|i don't know)/i.test(a.reply)) why = `no memory answer ("${a.reply}")`;
    if (why) fails.push([q, why]);
    if (show) console.log(`${why ? 'FAIL' : '  ok'}  "${q}" -> ${a.notMine ? '(not mine)' : a.intent + ' "' + a.reply + '"'}`);
  });
  wipe();
  const neg = [];
  let lang = 'it';
  for (const [l, q] of NOT_MINE) { if (l !== lang) { neg.push('/' + l); lang = l; } neg.push('/memory ' + q); }
  run(['/reset', ...neg]).forEach((r, i) => {
    n++;
    const q = NOT_MINE[i][1];
    if (!r.notMine) fails.push([q, `lean path claimed a non-memory utterance: ${r.intent} "${r.reply}"`]);
    if (show) console.log(`${r.notMine ? '  ok' : 'FAIL'}  "${q}" -> ${r.notMine ? '(not mine)' : r.intent}`);
  });
} finally {
  wipe();
  for (const f of STATE) if (existsSync(bak(f))) renameSync(bak(f), join(dir, f));
}

console.log(`[memory-lite] lean personal-memory path — ${n - fails.length}/${n} pass`);
if (fails.length) { console.log('FAILURES:'); for (const [q, w] of fails) console.log(`  ✗ "${q}" — ${w}`); process.exit(1); }
console.log('✓ same answers as the full cascade for memory; nothing else claimed');

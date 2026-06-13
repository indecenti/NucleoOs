#!/usr/bin/env node
// KGE / combinator END-TO-END gate over the REAL compiled cascade (anima.exe), BILINGUAL.
// Exercises the on-device deductive tier on the grown mind.<lang>.jsonl: born-forward, capital-inverse,
// continent-transitive (country 1-hop AND city 2-hop), the neuro-symbolic combinator (older/younger/
// age-diff/continent-membership/same-country) — in BOTH Italian and English — plus the honesty refusals
// (subject absent / not a continent / fabricated). A reply is checked by substring; a refusal must be empty.
//   node tools/anima-host/kge-host-check.mjs
import { spawnSync } from 'node:child_process';
import { existsSync, rmSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const C = { g: '\x1b[32m', r: '\x1b[31m', d: '\x1b[2m', b: '\x1b[1m', x: '\x1b[0m' };
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`'); process.exit(2); }
for (const f of ['session.txt']) { try { rmSync(join(here, 'sd', 'data', 'anima', f)); } catch {} }

// {lang, q, has} -> reply must contain `has` (case-insensitive); {lang, q, refuse:true} -> reply empty.
const CASES = [
  // --- born-forward (IT + EN) ---
  ['it', 'quando e nato Einstein', '1879'], ['it', 'quando e nato Newton', '1643'],
  ['it', 'quando e nato Dante', '1265'], ['it', 'quando e nato Napoleon', '1769'],
  ['it', 'quando e nata Madonna', '1958'],
  ['en', 'when was Einstein born', '1879'], ['en', 'when was Newton born', '1643'],
  // --- capital-inverse (IT + EN), incl. countries with NO card (Groq-extended) ---
  ['it', 'qual e la capitale della Francia', 'Parigi'], ['it', 'capitale del Giappone', 'Tokyo'],
  ['it', 'capitale del Kenya', 'Nairobi'], ['it', 'capitale del Vietnam', 'Hanoi'],
  ['it', 'capitale della Giamaica', 'Kingston'], ['it', 'capitale del Cile', 'Santiago'],
  ['en', 'what is the capital of France', 'Paris'], ['en', 'what is the capital of Kenya', 'Nairobi'],
  ['en', 'what is the capital of Japan', 'Tokyo'],
  // --- continent-transitive: country (1 hop) AND city (2 hops), IT + EN ---
  ['it', 'in che continente e la Francia', 'Europa'], ['it', 'in che continente e il Giappone', 'Asia'],
  ['it', 'in che continente e il Kenya', 'Africa'], ['it', 'in che continente e il Vietnam', 'Asia'],
  ['it', 'in che continente e il Brasile', 'America'], ['it', 'in che continente e Parigi', 'Europa'],
  ['it', 'in che continente e Roma', 'Europa'], ['it', 'dove si trova Tokyo', 'Asia'],
  ['it', 'in che continente e Nairobi', 'Africa'],
  ['en', 'what continent is France in', 'Europe'], ['en', 'what continent is Japan in', 'Asia'],
  ['en', 'what continent is Kenya in', 'Africa'], ['en', 'what continent is Vietnam in', 'Asia'],
  // --- combinator: older / younger / age-diff (IT + EN) ---
  ['it', 'chi e piu vecchio tra Einstein e Dante', 'Dante'], ['it', 'chi e piu vecchio Einstein o Newton', 'Newton'],
  ['it', 'chi e piu giovane tra Newton e Einstein', 'Einstein'],
  ['it', 'quanti anni tra la nascita di Dante e Einstein', '614'],
  ['en', 'who is older Einstein or Dante', 'Dante'], ['en', 'who is younger Newton or Einstein', 'Einstein'],
  ['en', 'how many years between the birth of Dante and Einstein', '614'],
  // --- combinator: continent-membership (IT + EN) ---
  ['it', 'Newton era europeo', 'Si'], ['it', 'Einstein era asiatico', 'No'], ['it', 'Madonna era americana', 'Si'],
  ['en', 'was Newton european', 'Yes'], ['en', 'was Madonna european', 'No'], ['en', 'was Madonna american', 'Yes'],
  // --- combinator: same-country (IT + EN) ---
  ['it', 'Dante e Colombo erano connazionali', 'Si'], ['it', 'Einstein e Dante erano connazionali', 'No'],
  ['en', 'were Dante and Colombo compatriots', 'Yes'], ['en', 'were Einstein and Dante from the same country', 'No'],
  // --- nationality / country-of-origin (FWD edge "country", IT + EN) ---
  ['it', 'di che nazionalita e Einstein', 'Germania'], ['it', 'da dove viene Dante', 'Italia'],
  ['en', 'what nationality was Einstein', 'Germany'],
  // --- HONESTY: must refuse (empty), never fabricate ---
  ['it', 'quando e nato Pinco', null], ['it', 'in che continente e Marte', null],
  ['it', 'capitale dell Europa', null], ['it', 'capitale del Pincopallino', null],
  ['en', 'when was Pinco born', null], ['en', 'what continent is Pincoland in', null],
  ['it', 'di che nazionalita e Pinco', null], ['it', 'da dove viene la pizza', null],   // nationality honesty
  // proactive disambiguation: a bare ambiguous surname (many Trumps) must NAME the candidates and ask,
  // never silently guess one. (>2 homonyms with the relation -> "...sii più preciso".)
  ['it', 'di che nazionalita e Trump', 'preciso'],
];

// Build one REPL stream: /reset + /it|/en before each query so sessions never bleed.
let lang = 'it';
const lines = [];
for (const [l, q] of CASES) {
  lines.push('/reset');
  if (l !== lang) { lines.push('/' + l); lang = l; }
  lines.push(q);
}
const res = spawnSync(exe, [], { input: lines.join('\n') + '\n', encoding: 'utf8', maxBuffer: 1 << 24 });
const out = (res.stdout || '');
// Parse "Q: <q>\n ... reply: <text>" blocks in order.
const blocks = [];
const re = /Q: (.*?)\r?\n([\s\S]*?)reply: (.*?)\r?\n/g;
let m;
while ((m = re.exec(out))) blocks.push({ q: m[1].trim(), reply: m[3].trim() });

let pass = 0, fail = 0; const fails = [];
const byKind = {};
for (const [l, q, want] of CASES) {
  const blk = blocks.find((b) => b.q === q);
  const reply = blk ? blk.reply : '(no block)';
  const empty = !reply || reply === '(vuoto)';
  let ok;
  if (want === null) ok = empty;                              // refusal
  else ok = !empty && reply.toLowerCase().includes(want.toLowerCase());
  const kind = want === null ? 'honesty' : 'answer';
  byKind[kind] = byKind[kind] || [0, 0];
  if (ok) { pass++; byKind[kind][0]++; } else { fail++; byKind[kind][1]++; fails.push(`[${l}] "${q}" -> ${reply}  (want ${want === null ? 'REFUSE' : JSON.stringify(want)})`); }
}

console.log(`\n${C.b}=== KGE / combinator host gate (bilingual, real exe) ===${C.x}`);
console.log(`  answers: ${byKind.answer ? byKind.answer[0] + '/' + (byKind.answer[0] + byKind.answer[1]) : '0'}   honesty-refusals: ${byKind.honesty ? byKind.honesty[0] + '/' + (byKind.honesty[0] + byKind.honesty[1]) : '0'}`);
for (const f of fails) console.log(`   ${C.r}FAIL${C.x} ${f}`);
console.log(fail === 0 ? `${C.g}${C.b}ALL ${pass} BILINGUAL CASES PASS${C.x}` : `${C.r}${C.b}${fail} FAILURE(S)${C.x}`);
process.exit(fail ? 1 : 0);

#!/usr/bin/env node
// SKILL <-> KNOWLEDGE BOUNDARY — proves the deterministic cascade keeps SKILLS and KNOWLEDGE in their
// lanes, in BOTH directions, with NO generative model involved:
//   (A) A DEFINITION question about a word that is ALSO a skill trigger ("cos'è la forza", "what is the
//       average", "spiegami il vettore") must be EXPLAINED (knowledge / honest abstain) and must NEVER be
//       COMPUTED — a fabricated number on a query that carries no operands is a skill stepping on knowledge.
//   (B) A COMPUTE request ("forza con massa 10 e accelerazione 9.8", "radice quadrata di 144") must STILL
//       fire its skill — knowledge must not swallow a real command (anti-over-correction).
// Every homograph + its ground-truth domain is enumerated from the real triggers (anima_solve.c) crossed
// with the knowledge corpus. Deterministic: a fixed spec (no RNG, no clock — the wall-clock `date` skill is
// excluded), batched through the REAL anima.exe, parsed from its structured stdout. Re-runnable + valid.
//
//   node tools/anima-host/boundary.mjs            # the gate
//   node tools/anima-host/boundary.mjs --show     # routing distribution + every case
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }

// Intents that COMPUTE/convert/act (a definition must never reach one of these with a fabricated value).
const SKILL = new Set(['calc', 'geo', 'phys', 'ohm', 'convert', 'percent', 'base', 'roman', 'spreadsheet', 'prime', 'func', 'vector', 'units', 'scale']);
// A reply that contains a COMPUTED numeric result ("= 16", "Fa -7", "is 0.5", "= 78.5398"). Definition
// queries carry ZERO operands, so ANY such number is fabricated. (π·r²/symbolic formulas have no digits.)
const hasComputedValue = (s) => /(=|\bfa\b|\bis\b|\bvale\b)\s*-?\d/i.test(s) || /^-?\d+(\.\d+)?\s*(N|J|W|Pa|Hz|m|kg)?\.?$/.test(s.trim());

// ---- the homograph spec (verified against anima_solve.c triggers + tools/anima/knowledge/*.jsonl) -------
// np_it/np_en: the concept as a NOUN PHRASE (with article) so the framings read naturally and carry NO digit.
// skill: the intent the COMPUTE form must fire (pipe = legitimately-ambiguous dispatch). domain: ground truth.
const H = [
  { np_it: 'la forza',          np_en: 'force',            skill: 'phys',  dom: 'physics',    ci: 'forza con massa 10 e accelerazione 3',            ce: 'force with mass 10 and acceleration 3' },
  { np_it: "l'energia",         np_en: 'energy',           skill: 'phys',  dom: 'physics',    ci: 'energia cinetica con massa 2 e velocità 3',       ce: 'kinetic energy with mass 2 and velocity 3' },
  { np_it: 'la velocità',       np_en: 'velocity',         skill: 'phys',  dom: 'physics',    ci: 'velocità con spazio 100 e tempo 4',               ce: 'speed with distance 100 and time 4' },
  { np_it: "l'accelerazione",   np_en: 'acceleration',     skill: 'phys',  dom: 'physics',    ci: 'accelerazione con velocità 20 e tempo 5',         ce: 'acceleration with velocity 20 and time 5' },
  { np_it: 'la massa',          np_en: 'mass',             skill: 'phys',  dom: 'physics',    ci: 'massa con forza 100 e accelerazione 10',          ce: 'mass with force 100 and acceleration 10' },
  { np_it: 'il lavoro',         np_en: 'work',             skill: 'phys',  dom: 'physics',    ci: 'lavoro con forza 50 e spostamento 4',             ce: 'work with force 50 and displacement 4' },
  { np_it: 'la pressione',      np_en: 'pressure',         skill: 'phys',  dom: 'physics',    ci: 'pressione con forza 500 e superficie 2',          ce: 'pressure with force 500 and area 2' },
  { np_it: 'la densità',        np_en: 'density',          skill: 'phys',  dom: 'physics',    ci: 'densità con massa 200 e volume 100',              ce: 'density with mass 200 and volume 100' },
  { np_it: 'la frequenza',      np_en: 'frequency',        skill: 'phys',  dom: 'physics',    ci: 'frequenza con periodo 0.02',                      ce: 'frequency with period 0.02' },
  { np_it: 'la quantità di moto', np_en: 'momentum',       skill: 'phys',  dom: 'physics',    ci: 'quantità di moto con massa 4 e velocità 5',       ce: 'momentum with mass 4 and velocity 5' },
  { np_it: 'la resistenza',     np_en: 'resistance',       skill: 'ohm',   dom: 'electronics',ci: 'resistenza con tensione 9 volt e corrente 3 ampere', ce: 'resistance with voltage 9 volt and current 3 ampere' },
  { np_it: 'la tensione',       np_en: 'voltage',          skill: 'ohm',   dom: 'electronics',ci: 'tensione con corrente 2 ampere e resistenza 5 ohm',  ce: 'voltage with current 2 ampere and resistance 5 ohm' },
  { np_it: 'la corrente',       np_en: 'current',          skill: 'ohm',   dom: 'electronics',ci: 'corrente con tensione 12 volt e resistenza 4 ohm',   ce: 'current with voltage 12 volt and resistance 4 ohm' },
  { np_it: "l'area",            np_en: 'area',             skill: 'geo',   dom: 'geometry',   ci: 'area del cerchio di raggio 3',                    ce: 'area of a circle of radius 3' },
  { np_it: 'il perimetro',      np_en: 'perimeter',        skill: 'geo',   dom: 'geometry',   ci: 'perimetro del quadrato di lato 5',                ce: 'perimeter of a square of side 5' },
  { np_it: 'il volume',         np_en: 'volume',           skill: 'geo',   dom: 'geometry',   ci: 'volume della sfera di raggio 2',                  ce: 'volume of a sphere of radius 2' },
  { np_it: "l'ipotenusa",       np_en: 'hypotenuse',       skill: 'geo',   dom: 'geometry',   ci: 'ipotenusa con cateti 3 e 4',                      ce: 'hypotenuse with legs 3 and 4' },
  { np_it: 'la media',          np_en: 'average',          skill: 'calc',  dom: 'statistics', ci: 'media di 4 8 12 6',                               ce: 'average of 4 8 12 6' },
  { np_it: 'la mediana',        np_en: 'median',           skill: 'calc|spreadsheet', dom: 'statistics', ci: 'mediana di 3 1 9 5 7',                  ce: 'median of 3 1 9 5 7' },
  { np_it: 'il logaritmo',      np_en: 'logarithm',        skill: 'calc',  dom: 'math',       ci: 'logaritmo di 100',                                ce: 'logarithm of 100' },
  { np_it: 'la radice quadrata',np_en: 'square root',      skill: 'calc',  dom: 'math',       ci: 'radice quadrata di 144',                          ce: 'square root of 144' },
  { np_it: 'la percentuale',    np_en: 'percentage',       skill: 'percent', dom: 'math',     ci: 'il 20 per cento di 350',                          ce: '20 percent of 350' },
  { np_it: 'il fattoriale',     np_en: 'factorial',        skill: 'calc',  dom: 'math',       ci: 'fattoriale di 6',                                 ce: 'factorial of 6' },
  { np_it: 'il seno',           np_en: 'sine',             skill: 'calc',  dom: 'trigonometry', ci: 'seno di 30 gradi',                              ce: 'sine of 30 degrees' },
  { np_it: 'il coseno',         np_en: 'cosine',           skill: 'calc',  dom: 'trigonometry', ci: 'coseno di 60 gradi',                            ce: 'cosine of 60 degrees' },
  { np_it: 'la tangente',       np_en: 'tangent',          skill: 'calc',  dom: 'trigonometry', ci: 'tangente di 45 gradi',                          ce: 'tangent of 45 degrees' },
  { np_it: 'un vettore',        np_en: 'a vector',         skill: 'calc|vector', dom: 'math', ci: 'modulo del vettore 3 4',                          ce: 'magnitude of the vector 3 4' },
  { np_it: 'il modulo',         np_en: 'the modulo',       skill: 'calc',  dom: 'math',       ci: '17 modulo 5',                                     ce: '17 modulo 5' },
  { np_it: 'il valore assoluto',np_en: 'absolute value',   skill: 'calc',  dom: 'math',       ci: 'valore assoluto di -7',                           ce: 'absolute value of -7' },
  { np_it: 'la potenza',        np_en: 'a power',          skill: 'calc',  dom: 'math',       ci: '2 elevato alla 10',                               ce: '2 to the power of 10' },
  { np_it: 'il numero binario', np_en: 'a binary number',  skill: 'base',  dom: 'computer-science', ci: 'converti 42 in binario',                    ce: 'convert 42 to binary' },
  { np_it: 'il numero esadecimale', np_en: 'a hexadecimal number', skill: 'base', dom: 'computer-science', ci: 'converti 255 in esadecimale',       ce: 'convert 255 to hexadecimal' },
  { np_it: 'il numero romano',  np_en: 'a roman numeral',  skill: 'roman', dom: 'history',    ci: 'scrivi 2024 in numeri romani',                    ce: 'write 2024 in roman numerals' },
  { np_it: 'il numero primo',   np_en: 'a prime number',   skill: 'prime', dom: 'math',       ci: '17 è un numero primo',                            ce: 'is 17 a prime number' },
  { np_it: 'il massimo comune divisore', np_en: 'the greatest common divisor', skill: 'calc', dom: 'math', ci: 'mcd di 12 e 18',                       ce: 'gcd of 12 and 18' },
];

// (C) REVERSE bait: genuine KNOWLEDGE questions that carry a skill-trigger word and/or numbers, which must
// stay in the knowledge lane (l1/mosaico) or honestly abstain — a skill must NOT compute on them.
const REV = [
  { q: 'qual è la velocità della luce', lang: 'it' }, { q: 'qual è la massa della terra', lang: 'it' },
  { q: 'quanti pianeti ci sono nel sistema solare', lang: 'it' }, { q: 'quanti lati ha un triangolo', lang: 'it' },
  { q: 'quanti gradi ha un angolo retto', lang: 'it' }, { q: "qual è la formula chimica dell'acqua", lang: 'it' },
  { q: 'quante ossa ha il corpo umano', lang: 'it' }, { q: "qual è l'area dell'italia", lang: 'it' },
  { q: 'quanta forza ha un leone', lang: 'it' }, { q: 'qual è la potenza di un fulmine', lang: 'it' },
  { q: 'quanti anni ha avuto la guerra dei cento anni', lang: 'it' }, { q: 'qual è la frequenza cardiaca normale', lang: 'it' },
  { q: 'what is the speed of light', lang: 'en' }, { q: 'what is the mass of the earth', lang: 'en' },
  { q: 'how many sides does a triangle have', lang: 'en' }, { q: 'how many planets are in the solar system', lang: 'en' },
  { q: 'how many degrees in a right angle', lang: 'en' }, { q: 'what is the area of italy', lang: 'en' },
];

const DEF_IT = ["cos'è {NP}", "che cos'è {NP}", "cosa significa {NP}", "spiegami {NP}", "a cosa serve {NP}", "dammi la definizione di {NP}"];
const DEF_EN = ['what is {NP}', 'explain {NP}', 'what does {NP} mean', 'tell me about {NP}', 'define {NP}'];
const COMPUTE_IT = ['calcola {E}', 'quanto fa {E}', '{E}'];
const COMPUTE_EN = ['compute {E}', 'how much is {E}', '{E}'];

// ---- build the case set (deterministic; definition queries carry NO digit) ------------------------
const defs = [], comps = [];
for (const h of H) {
  for (const f of DEF_IT) defs.push({ q: f.replace('{NP}', h.np_it), lang: 'it', term: h.np_it, dom: h.dom });
  for (const f of DEF_EN) defs.push({ q: f.replace('{NP}', h.np_en), lang: 'en', term: h.np_en, dom: h.dom });
  comps.push({ q: COMPUTE_IT[0].replace('{E}', h.ci), lang: 'it', term: h.np_it, want: h.skill });
  comps.push({ q: COMPUTE_EN[0].replace('{E}', h.ce), lang: 'en', term: h.np_en, want: h.skill });
}

function runBatch(items) {
  let lang = 'it'; const lines = [];
  for (const it of items) { lines.push('/reset'); const w = it.lang === 'en' ? 'en' : 'it'; if (w !== lang) { lines.push('/' + w); lang = w; } lines.push(it.q); }
  const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 128 * 1024 * 1024 });
  return r.stdout.toString('utf8').split(/^Q: /m).slice(1).map((b) => ({
    tier: (b.match(/tier=(\S+)/) || [])[1] || 'none',
    intent: (b.match(/intent=(\S*)/) || [])[1] || '',
    reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
  }));
}
const intentEq = (p) => (p.intent === 'mosaico' ? 'l1' : p.intent);

const dRows = runBatch(defs), cRows = runBatch(comps), rRows = runBatch(REV);
const show = process.argv.includes('--show');

// (A) DEFINITION must not COMPUTE a fabricated value.
const dist = {};
const defFails = [];
for (let i = 0; i < defs.length; i++) {
  const d = defs[i], p = dRows[i] || { tier: 'none', intent: '', reply: '' };
  const it = intentEq(p) || '(abstain)'; dist[it] = (dist[it] || 0) + 1;
  const stepped = SKILL.has(intentEq(p)) && hasComputedValue(p.reply);     // a compute skill fired AND produced a number
  if (stepped) defFails.push([d.q, `${p.intent}/${p.tier}`, p.reply.slice(0, 56)]);
  if (show) console.log(`  [def ${d.lang}] "${d.q}" -> ${p.intent || '(abstain)'}/${p.tier}`);
}
// (B) COMPUTE must still fire its skill.
const compFails = [];
for (let i = 0; i < comps.length; i++) {
  const c = comps[i], p = cRows[i] || { tier: 'none', intent: '', reply: '' };
  const ok = c.want.split('|').includes(intentEq(p));
  if (!ok) compFails.push([c.q, c.want, `${p.intent || '(none)'}/${p.tier}`, p.reply.slice(0, 48)]);
  if (show) console.log(`  [cmp ${c.lang}] "${c.q}" -> ${p.intent || '(none)'}/${p.tier}  want=${c.want}`);
}

// (C) REVERSE: a knowledge question with skill-word/number bait must not route to a compute skill.
const revFails = [];
for (let i = 0; i < REV.length; i++) {
  const c = REV[i], p = rRows[i] || { tier: 'none', intent: '', reply: '' };
  if (SKILL.has(intentEq(p)) && hasComputedValue(p.reply)) revFails.push([c.q, `${p.intent}/${p.tier}`, p.reply.slice(0, 48)]);
  if (show) console.log(`  [rev ${c.lang}] "${c.q}" -> ${p.intent || '(abstain)'}/${p.tier}`);
}

console.log(`\n[boundary] ${H.length} skill∩knowledge homographs · ${defs.length} definition + ${comps.length} compute + ${REV.length} reverse-bait cases over the real exe`);
console.log('  definition routing: ' + Object.entries(dist).sort((a, b) => b[1] - a[1]).map(([k, v]) => `${k}:${v}`).join('  '));
console.log(`  (A) definitions that COMPUTE a fabricated value (skill steps on knowledge): ${defFails.length}`);
for (const f of defFails.slice(0, 30)) console.log(`     ✗ "${f[0]}" -> ${f[1]}  reply="${f[2]}"`);
console.log(`  (B) compute requests that FAIL to fire their skill (knowledge swallows a command): ${compFails.length}`);
for (const f of compFails.slice(0, 30)) console.log(`     ✗ "${f[0]}" want=${f[1]} got=${f[2]}  reply="${f[3]}"`);
console.log(`  (C) knowledge-with-bait questions that a skill WRONGLY computed: ${revFails.length}`);
for (const f of revFails.slice(0, 30)) console.log(`     ✗ "${f[0]}" -> ${f[1]}  reply="${f[2]}"`);

const fail = defFails.length + compFails.length + revFails.length;
console.log(fail ? `\n[boundary] FAIL — ${defFails.length} skill-steps-on-knowledge, ${compFails.length} skill-misfire, ${revFails.length} reverse-collision`
                 : `\n[boundary] ✓ skills never compute on a definition or a knowledge question, and every compute fires its skill.`);
process.exit(fail ? 1 : 0);

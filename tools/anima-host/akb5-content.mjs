#!/usr/bin/env node
// AKB5 CONTENT — natural-language test for the SHARDED encyclopedia (countries / physics-constants /
// grounded definitions) that only the AKB5 loader makes reachable. It forces AKB5 on (ANIMA_AKB5=1) and
// drives the REAL compiled cascade (anima.exe), proving two things at once on the NEW content:
//   (A) RECALL — capitals, physical constants and bilingual definitions answer under varied phrasings
//       (IT+EN, paraphrase, a dropped accent). Reported + soft floor (the sharded router's recall is a
//       tracked property, lifted by the global-centroid router work — see docs/anima-knowledge-scale.md).
//   (B) FALSE-PREMISE must-abstain — "capitale di Marte", "capitale della Luna", anachronisms — the very
//       traps a narrow shard exposes (no global runner-up). ANY confident answer here is a hallucination.
// HARD GATE: 0 hallucinations in (B). Run:  node tools/anima-host/akb5-content.mjs [--verbose]
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
const verbose = process.argv.includes('--verbose');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build` first.'); process.exit(2); }

const manifest = join(here, 'sd', 'data', 'anima', 'anima-it-akb5.bin');
if (!existsSync(manifest)) {
  console.log('[akb5-content] no AKB5 manifest in the host SD tree — build it with:');
  console.log('  ANIMA_ENC=tools/anima-host/sd/data/anima/anima-it-encoder.bin \\');
  console.log('  ANIMA_AKB5_DIR=tools/anima-host/sd/data/anima/akb5 \\');
  console.log('  ANIMA_EXTRA="$(ls tools/anima/knowledge.staged/*.jsonl|paste -sd,)" python tools/anima/build_akb5.py');
  console.log('[akb5-content] SKIP (manifest absent)');
  process.exit(0);
}

// want: a keyword that MUST appear in a correct answer (accent/space-insensitive). lang picks /it or /en.
const RECALL = [
  // capitals (countries shard) — IT + EN, plain + paraphrased + one dropped accent
  { q: 'qual è la capitale della Norvegia', lang: 'it', want: 'oslo' },
  { q: 'capitale del Giappone', lang: 'it', want: 'tokyo' },
  { q: "qual e la capitale dell'Egitto", lang: 'it', want: 'cairo' },
  { q: 'capitale di Malta', lang: 'it', want: 'valletta' },
  { q: 'what is the capital of Brazil', lang: 'en', want: 'brasilia' },
  { q: 'capital of Kenya', lang: 'en', want: 'nairobi' },
  { q: 'capitale della Francia', lang: 'it', want: 'parigi' },
  { q: 'qual è la capitale del Canada', lang: 'it', want: 'ottawa' },
  // physics constants (physics-constants shard)
  { q: 'costante di Planck', lang: 'it', want: '6.626' },
  { q: 'quanto vale la costante di Planck', lang: 'it', want: '6.626' },
  { q: 'costante di Avogadro', lang: 'it', want: '6.022' },
  { q: "carica dell'elettrone", lang: 'it', want: '1.602' },
  { q: 'value of the speed of light', lang: 'en', want: '299792458' },
  // grounded definitions (cs / electronics / science shards)
  { q: "cos'è il DNS", lang: 'it', want: 'dominio' },
  { q: 'what is a transistor', lang: 'en', want: 'transistor' },
  { q: "cos'è la fotosintesi", lang: 'it', want: 'piante' },
  { q: "cos'è un microcontrollore", lang: 'it', want: 'chip' },
  { q: 'what is photosynthesis', lang: 'en', want: 'light' },
  // bare-surname person recall (person.bin low-floor rescue): card carries the FULL name, query is the
  // surname only — must answer (the surname appears exactly in the reply). Pairs with the fake/typo traps.
  { q: 'chi è nixon', lang: 'it', want: 'nixon' },
  { q: 'chi è einstein', lang: 'it', want: 'einstein' },
  { q: 'chi è mandela', lang: 'it', want: 'mandela' },
  { q: 'who is obama', lang: 'en', want: 'obama' },
];

// FALSE PREMISE — there is no such capital / the property is an anachronism. Must ABSTAIN.
const ABSTAIN = [
  { q: 'qual è la capitale di Marte', lang: 'it' },
  { q: 'qual è la capitale della Luna', lang: 'it' },
  { q: 'qual è la capitale del Sole', lang: 'it' },
  { q: 'qual è la capitale di Giove', lang: 'it' },
  { q: 'what is the capital of Mars', lang: 'en' },
  { q: 'what is the capital of the Moon', lang: 'en' },
  { q: 'quando ha twittato Cristoforo Colombo', lang: 'it' },
  { q: 'quanti followers ha il Medioevo', lang: 'it' },
  { q: "qual è l'indirizzo email di Napoleone", lang: 'it' },
  { q: 'qual è la costante di Planck di Marte', lang: 'it' },
  // regression fixtures for the bugs the adversarial verify workflow (wf-akb5-verify.mjs) found — all were
  // confident fabrications the curated gates missed; each class is now guarded:
  { q: 'qual è il pianeta più grande oltre Giove', lang: 'it' },          // exclusion-scope -> named the excluded entity
  { q: 'what is the biggest planet other than Jupiter', lang: 'en' },
  { q: 'che cosa è il protocollo Flixxon-9', lang: 'it' },                // calc scraped the entity-name suffix
  { q: 'what is the half-life of isotope Carbon-Floonk-14', lang: 'en' },
  { q: 'qual è la radice cubica del cristallo Zorblax-27', lang: 'it' },  // funcs/cuberoot on an entity number
  { q: 'what is 50 percent of Floonkonium-200', lang: 'en' },            // percent/calc on an entity number
  { q: 'come si configura il modulo Frobnicator-X12', lang: 'it' },       // spreadsheet cell lifted from a name
  { q: 'qual è il server Discord di Napoleone', lang: 'it' },            // "disco" substring -> SD storage
  { q: 'what is the Discord username of Cleopatra', lang: 'en' },
  { q: 'qual è la valuta del Giappone', lang: 'it' },                    // wrong-attribute on a country card
  { q: 'qual è il lago più grande del deserto del Sahara', lang: 'it' },  // superlative wrong head-noun
  // PERSON-EXISTENCE traps — the class the gate was BLIND to (no fake-person fixtures existed). These pre-arm
  // the gate for the bare-surname recall fix: a fake or typo-neighbour name must ABSTAIN, never borrow a
  // damlev-neighbour's real bio (newtron!=neutron card, washingtom!=Denzel Washington, verde!=Cape Verde).
  // FULLY-FABRICATED names (no real-person neighbour) must abstain — never borrow a card's bio. NB a typo
  // of a REAL famous person (newtron->Newton, neason->Neeson) is acceptable typo-tolerance, NOT fabrication,
  // so it is deliberately NOT trapped here; the danger is an invented name getting a confident answer.
  { q: 'chi è Floonk Zorblax', lang: 'it' },
  { q: 'chi è Mario Inventato Inesistente', lang: 'it' },
  { q: 'who is Glerbon Flonzo', lang: 'en' },
  { q: 'chi è Zibbib Qwronk', lang: 'it' },
  { q: 'come funziona il Frobnicator-X12', lang: 'it' },  // fake mechanism (arms the come-funziona strip)
];

const items = [...RECALL.map(x => ({ ...x, kind: 'recall' })), ...ABSTAIN.map(x => ({ ...x, kind: 'abstain' }))];
const lines = [];
let lang = 'it';
for (const it of items) { lines.push('/reset'); if (it.lang !== lang) { lines.push('/' + it.lang); lang = it.lang; } lines.push(it.q); }

const r = spawnSync(exe, [], {
  input: Buffer.from(lines.join('\n') + '\n', 'utf8'),
  maxBuffer: 64 * 1024 * 1024,
  env: { ...process.env, ANIMA_AKB5: '1' },   // force the sharded router regardless of caller
});
const norm = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/\s+/g, ' ').trim();
// Each block starts with the echoed query text (its first line); match blocks to items by that text so we
// are robust to which control lines (/reset, /it, /en) do or don't echo a Q-block.
const byQuery = new Map();
for (const b of r.stdout.toString('utf8').split(/^Q: /m).slice(1)) {
  const q = norm(b.split('\n')[0]);
  const tier = (b.match(/tier=(\S+)/) || [])[1] || 'none';
  let reply = (b.match(/reply:\s*([\s\S]*?)\s*$/m) || [])[1] || '';
  reply = reply.replace(/\s+/g, ' ').trim();
  if (reply === '(vuoto)' || reply === '(empty)') reply = '';
  byQuery.set(q, { tier, reply, answered: tier !== 'none' && reply !== '' });
}
// A type-gate refusal / clarify is a SAFE abstention, not a fabricated answer ("Mars" -> "Bruno Mars is a
// person, not a place" correctly declines to invent a capital).
const isRefusal = (s) => /not a place|not a person|non (è|e) (un|una) (luogo|persona|posto)|did you mean|non sono sicuro|intendi\b|non ho (informazioni|dettagli)|i (don'?t|do not) (have|know)/i.test(s);
let recallOk = 0, recallMiss = 0, halluc = 0;
const fails = [];
for (let k = 0; k < items.length; k++) {
  const it = items[k];
  const b = byQuery.get(norm(it.q)) || { answered: false, reply: '' };
  if (it.kind === 'abstain') {
    if (b.answered && !isRefusal(b.reply)) { halluc++; fails.push({ k: 'HALLUC', q: it.q, reply: b.reply.slice(0, 70) }); }
  } else {
    if (b.answered && norm(b.reply).includes(norm(it.want))) recallOk++;
    else { recallMiss++; if (verbose) fails.push({ k: b.answered ? 'WRONG' : 'MISS', q: it.q, want: it.want, reply: b.reply.slice(0, 60) }); }
  }
}

console.log(`[akb5-content] recall ${recallOk}/${RECALL.length}  |  false-premise abstained ${ABSTAIN.length - halluc}/${ABSTAIN.length}  |  HALLUCINATIONS ${halluc}`);
if (fails.length) { console.log('details:'); for (const f of fails) console.log('  ' + JSON.stringify(f)); }
const recallFloor = Math.ceil(RECALL.length * 0.8);   // 80% of curated new content must answer
const ok = halluc === 0 && recallOk >= recallFloor;
if (!ok) console.log(`[akb5-content] FAIL — halluc:${halluc} (must be 0)  recall:${recallOk}/${RECALL.length} (floor ${recallFloor})`);
else console.log(`[akb5-content] ✓ 0 hallucinations on false premises AND recalls the new encyclopedia content`);
process.exit(ok ? 0 : 1);

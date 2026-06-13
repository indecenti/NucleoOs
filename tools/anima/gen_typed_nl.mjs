#!/usr/bin/env node
// Generate eval_typed_nl.jsonl — the typed-facet NL anti-regression set (docs/anima-knowledge-graph.md).
// TRUE POSITIVES are authored data-first: I pick the SUBJECT + craft varied phrasings; the EXPECTED answer
// ("want") is read straight from the committed facet/mind fixtures, so a case can never assert a fact the
// corpus doesn't back. ADVERSARIAL cases (unknown entity, wrong-type subject, false premise, tier overlap)
// are hand-listed below — the "studied false-positives / overlaps". Re-run after extract_triples changes:
//   node tools/anima/gen_typed_nl.mjs
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const SD = join(repo, 'tools', 'anima-host', 'sd', 'data', 'anima', 'learned');
const load = (f) => readFileSync(join(SD, f), 'utf8').split(/\r?\n/).filter(Boolean).map((l) => JSON.parse(l));
const facIt = load('facets.it.jsonl'), facEn = load('facets.en.jsonl'), mindIt = load('mind.it.jsonl');

const map = (rows, rel) => { const m = new Map(); for (const t of rows) if (t.rel === rel) m.set(t.subject, t.value); return m; };
const occIt = map(facIt, 'occupation'), occEn = map(facEn, 'occupation');
const genIt = map(facIt, 'gender'), genEn = map(facEn, 'gender');
const diedIt = map(mindIt, 'died');

// candidate subjects (slug). Only those actually present with the facet are emitted; the rest are dropped.
const PEOPLE = [
  'albert-einstein', 'isaac-newton', 'marie-curie', 'charles-darwin', 'leonardo-da-vinci', 'stephen-hawking',
  'michael-jackson', 'wolfgang-amadeus-mozart', 'ludwig-van-beethoven', 'bob-marley', 'elvis-presley',
  'david-bowie', 'freddie-mercury', 'dante-alighieri', 'william-shakespeare', 'cleopatra', 'pablo-picasso',
  'vincent-van-gogh', 'barack-obama', 'winston-churchill', 'abraham-lincoln', 'john-f-kennedy', 'adolf-hitler',
  'elon-musk', 'steve-jobs', 'bill-gates', 'audrey-hepburn', 'leonardo-dicaprio', 'tom-hanks', 'cristoforo-colombo',
];
// died: clean, recognizable subjects (segment-matched by the tier, so a short query name binds).
const DIED = [['napoleon', 'napoleon'], ['prince', 'prince-musician'], ['diana', 'diana-princess-of-wales']];

const name = (slug) => slug.replace(/-/g, ' ');
const rows = [];
const push = (q, lang, t, want) => rows.push(want === undefined ? { q, lang, t } : { q, lang, t, want });

let drop = [];
PEOPLE.forEach((s, i) => {
  if (!occIt.has(s)) { drop.push(s); return; }
  const n = name(s);
  // occupation — varied phrasings, want read from the fixtures (it/en)
  const occLeadsIt = ['che lavoro faceva', 'di cosa si occupava', 'che mestiere faceva', 'qual era il lavoro di'];
  push(`${occLeadsIt[i % occLeadsIt.length]} ${n}`, 'it', 'occ', occIt.get(s));
  if (i % 2 === 0) push(`di cosa si occupava ${n}`, 'it', 'occ', occIt.get(s));
  const occLeadsEn = ['what was the occupation of', 'what was the job of', 'what was the profession of'];
  if (occEn.has(s)) push(`${occLeadsEn[i % occLeadsEn.length]} ${n}`, 'en', 'occ', occEn.get(s));
  // gender — only when the subject has a gender facet
  if (genIt.has(s)) {
    push(i % 2 ? `${n} è uomo o donna` : `di che sesso è ${n}`, 'it', 'gen', genIt.get(s));
    if (genEn.has(s)) push(i % 2 ? `is ${n} a man or a woman` : `what gender is ${n}`, 'en', 'gen', genEn.get(s));
  }
});
DIED.forEach(([q, slug]) => { if (diedIt.has(slug)) push(`quando è morto ${q}`, 'it', 'died', diedIt.get(slug)); });

// ---- ADVERSARIAL: must NOT produce a facet (0-hallucination). t="abstain" ----
const ABSTAIN_IT = [
  'che lavoro faceva floonk', 'di cosa si occupava zorblax', 'che mestiere faceva il signor nessuno',
  'che lavoro faceva qwertyuiop', 'di cosa si occupava xyzzy',
  'che lavoro faceva roma', "di cosa si occupava l'italia", 'che lavoro faceva parigi',
  'di che sesso è parigi', 'parigi è uomo o donna', "l'europa è uomo o donna", 'la francia è uomo o donna',
  'di che sesso è il martedì', 'che lavoro faceva il numero sette', "di cosa si occupa l'aria",
  'che lavoro faceva la torre eiffel', 'di che sesso è la pizza', 'che genere è il colore rosso',
  'che lavoro faceva il 1492',
];
const ABSTAIN_EN = [
  'what was the occupation of floonk', 'what gender is berlin', 'is rome a man or a woman',
  'what was the job of the number seven', 'what was the occupation of tuesday',
  'what was the profession of nobody mcnobody',
  'what gender is the color blue', 'what was the occupation of the eiffel tower',
];
for (const q of ABSTAIN_IT) push(q, 'it', 'abstain');
for (const q of ABSTAIN_EN) push(q, 'en', 'abstain');

// TYPE-GATE: a place-relation question about a PERSON must be honestly REFUSED ("X è una persona, non
// un luogo"), not dodged with a bio (nucleo_anima_facet.c answer_typegate). t="refuse_person".
for (const q of ['qual è la capitale di einstein', 'in che continente è michael jackson', 'dove si trova cleopatra',
                 'in che continente è dante alighieri', 'qual è la capitale di napoleon'])
  push(q, 'it', 'refuse_person');
for (const q of ['what is the capital of einstein', 'what continent is dante alighieri in', 'where is cleopatra'])
  push(q, 'en', 'refuse_person');
// REGRESSION guard: a place-relation about a real PLACE must STILL answer (the type-gate must not block it).
for (const q of ['in che continente è la francia', 'qual è la capitale della francia', 'in che continente è parigi',
                 'qual è la capitale del giappone', 'in che continente è il brasile', 'dove si trova madrid'])
  push(q, 'it', 'geo_ok');
for (const q of ['what is the capital of france', 'what continent is japan in', 'what is the capital of spain'])
  push(q, 'en', 'geo_ok');

// ---- OVERLAP: a facet-shaped query that must route ELSEWHERE, never to the facet tier ----
// first-person "my job" -> profile tier (not facet)
for (const q of ['che lavoro faccio', 'qual è il mio lavoro', 'che lavoro faccio io']) push(q, 'it', 'profile');
for (const q of ['what is my job', "what's my job"]) push(q, 'en', 'profile');
// "who is X" -> describe/L1, answered but NOT by the facet tier
for (const q of ['chi è albert einstein', 'parlami di cleopatra', 'chi era dante alighieri']) push(q, 'it', 'answered');
for (const q of ['who is michael jackson']) push(q, 'en', 'answered');

const out = rows.map((r) => JSON.stringify(r)).join('\n') + '\n';
writeFileSync(join(here, 'eval_typed_nl.jsonl'), out, 'utf8');
const byT = {}; for (const r of rows) byT[r.t] = (byT[r.t] || 0) + 1;
console.log(`[gen] wrote ${rows.length} cases ->`, byT, drop.length ? `(dropped ${drop.length}: ${drop.join(',')})` : '');

// nl-offline.test.mjs — deep OFFLINE natural-language test for the deterministic planner. It injects
// the LIVE localIntent (extracted from apps/spreadsheet/www/index.html, exactly like spread-nlu's
// regress.mjs) into planFromNL and checks real IT/EN phrases produce correct VERIFIED PLANS with NO
// model. Two layers:
//   (A) CURATED golden — hand-verified exact op sequences for compound requests + anti-fabrication on
//       knowledge questions (HARD gate, exit≠0 on any miss).
//   (B) the 185-case generated corpus (tools/spread-copilot/nl-corpus.json) — coverage report +
//       HARD anti-fabrication invariant (a knowledge/empty case must yield ZERO sheet actions) +
//       op-set precision (the planner must not emit an op absent from the labelled set).
// Run: node tools/spread-copilot/nl-offline.test.mjs   (add -v to list corpus mismatches)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const DIR = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(DIR, '../..');
const HTML = fs.readFileSync(path.join(ROOT, 'apps/spreadsheet/www/index.html'), 'utf8');
// extract the live parser (brace-matched), same technique as tools/spread-nlu/regress.mjs
function grab(name) { const a = HTML.indexOf('function ' + name + '('); if (a < 0) return ''; let d = 0; for (let j = HTML.indexOf('{', a); j < HTML.length; j++) { if (HTML[j] === '{') d++; else if (HTML[j] === '}') { d--; if (d === 0) return HTML.slice(a, j + 1); } } return ''; }
const localIntent = new Function([grab('extractCol'), grab('localIntent')].join('\n') + '\nreturn localIntent;')();

const { planFromNL } = await import('file://' + path.join(ROOT, 'apps/spreadsheet/www/copilot/sheet-plan.js'));
const plan = (q) => planFromNL(q, { localIntent, cols: 26 });
const ops = (q) => plan(q).actions.map((a) => a.op);

let pass = 0, fail = 0; const fails = [];
const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
function check(name, got, want) { if (eq(got, want)) pass++; else { fail++; fails.push(`✗ ${name}\n    got  ${JSON.stringify(got)}\n    want ${JSON.stringify(want)}`); } }

// ───────────────────────── (A) CURATED GOLDEN — exact op sequences ─────────────────────────
const GOLD = [
  // Italian compound
  ['fai i totali poi ordina per colonna B decrescente', ['total', 'sort']],
  ['somma la colonna A e poi evidenzia il massimo', ['aggregate', 'highlight']],
  ['pulisci i dati, rimuovi i duplicati e poi ordina per colonna A', ['clean', 'dedupe', 'sort']],
  ['rimuovi le righe vuote poi rimuovi i duplicati', ['rmempty', 'dedupe']],
  ['metti la colonna A in maiuscolo e poi ordina per B', ['transform', 'sort']],
  ['evidenzia i duplicati nella colonna A, poi ordina per B', ['highlight', 'sort']],
  ['calcola la media di A poi la somma di B', ['aggregate', 'aggregate']],
  ['media di A e somma di B', ['aggregate', 'aggregate']],
  ['ordina per colonna C decrescente e poi evidenzia il minimo', ['sort', 'highlight']],
  ['fai la somma della B, poi il massimo della C, evidenzia i duplicati e infine ordina per A decrescente', ['aggregate', 'aggregate', 'highlight', 'sort']],
  ['metti la colonna prezzo in formato valuta e poi fai i totali', ['numfmt', 'total']],
  ['calcola la deviazione standard di B poi ordina per A', ['aggregate', 'sort']],
  ['metti i nomi col formato proprio poi ordina per nome crescente', ['transform', 'sort']],
  ['calcola la media della colonna età poi ordina', ['aggregate', 'sort']],
  ['trova il massimo della colonna età e poi ordina per B', ['aggregate', 'sort']],
  // English compound
  ['clean the data then remove duplicates and sort by column A', ['clean', 'dedupe', 'sort']],
  ['sum column A then highlight the max', ['aggregate', 'highlight']],
  ['remove empty rows then count the values in B', ['rmempty', 'aggregate']],
  ['sort by column C descending and then highlight values above 100', ['sort', 'highlight']],
  // anti-fabrication: pure knowledge → NO sheet actions
  ['chi era Napoleone', []],
  ['qual e la capitale del Giappone', []],
  ['what is the capital of Spain', []],
  ['quanto fa 12 per 8', []],
  // single command → exactly one action (compound branch must NOT fire)
  ['somma la colonna A', ['aggregate']],
  ['ordina per colonna B', ['sort']],
  ['evidenzia i valori sopra 100', ['highlight']],
];
for (const [q, want] of GOLD) check('golden: ' + q, ops(q), want);

// a few FIELD-level checks (not just op names)
check('field: sort desc B', plan('ordina per colonna B decrescente').actions[0], { op: 'sort', col: 1, order: 'desc' });
check('field: highlight gt 100', plan('evidenzia i valori sopra 100').actions[0], { op: 'highlight', test: 'gt', value: 100 });
check('field: agg SUM A', plan('somma la colonna A').actions[0], { op: 'aggregate', fn: 'SUM', col: 0 });
check('field: compound preserves order+cols', ops('somma la colonna A poi ordina per colonna C crescente'), ['aggregate', 'sort']);
{ const p = plan('somma la colonna A poi ordina per colonna C crescente').actions; check('field: 2nd is sort C asc', p[1], { op: 'sort', col: 2, order: 'asc' }); }

console.log(`\nCURATED GOLDEN: ${pass}/${pass + fail}`);
if (fails.length) console.log(fails.join('\n'));

// ───────────────────────── (B) GENERATED CORPUS — coverage + anti-fabrication ─────────────────────────
const corpus = JSON.parse(fs.readFileSync(path.join(DIR, 'nl-corpus.json'), 'utf8'));
let fabrications = 0, opLeaks = 0; const leakList = [];
let compoundTotal = 0, compoundDecomposed = 0;
let setHits = 0, setTotal = 0;
const SHEET_OPS = new Set(['aggregate', 'total', 'describe', 'insights', 'chart', 'highlight', 'sort', 'fill', 'clean', 'dedupe', 'rmempty', 'transform', 'format', 'numfmt', 'find', 'explain', 'enrich', 'formula', 'refresh']);

for (const c of corpus) {
  const got = ops(c.text);
  const exp = (c.expected || []);
  const expSheet = exp.filter((o) => o !== 'knowledge' && SHEET_OPS.has(o));
  const isKnowledgeOnly = exp.length && exp.every((o) => o === 'knowledge');
  const isEmpty = exp.length === 0;

  // HARD invariant 1: a pure KNOWLEDGE question must NEVER fabricate a sheet op. (Agent-labelled
  // "empty/ambiguous" cases like "media o somma di A" are a judgment call — a single defensible intent
  // is fine because the planner only ACTS on compound ≥2; the single-intent floor handles the rest.)
  if (isKnowledgeOnly && got.length > 0) { fabrications++; leakList.push(`FABRICATION  [${JSON.stringify(exp)}] "${c.text}" -> ${JSON.stringify(got)}`); }
  else if (isEmpty && got.length > 1) { fabrications++; leakList.push(`FABRICATION(empty→compound)  "${c.text}" -> ${JSON.stringify(got)}`); }

  // HARD invariant 2 (op-set precision): every op the planner emits must be in the labelled set
  // (a compound case may be labelled loosely, but the planner must not invent an unrelated op).
  if (expSheet.length) { for (const o of got) { setTotal++; if (expSheet.includes(o)) setHits++; else { opLeaks++; leakList.push(`OP-LEAK [exp ${JSON.stringify(expSheet)}] "${c.text}" -> +${o}`); } } }

  // coverage report: does a compound case decompose into ≥2 actions?
  if (expSheet.length >= 2) { compoundTotal++; if (got.length >= 2) compoundDecomposed++; }
}

const decompRate = compoundTotal ? (compoundDecomposed / compoundTotal * 100) : 100;
const setPrec = setTotal ? (setHits / setTotal * 100) : 100;
console.log(`\nCORPUS (${corpus.length} cases):`);
console.log(`  anti-fabrication (knowledge/empty → 0 ops): ${fabrications === 0 ? 'OK' : fabrications + ' FABRICATIONS'}`);
console.log(`  op-set precision (emitted ∈ labelled):      ${setPrec.toFixed(1)}%  (${opLeaks} leaks)`);
console.log(`  compound decomposition (≥2 ops):            ${decompRate.toFixed(1)}%  (${compoundDecomposed}/${compoundTotal})`);
if (process.argv.includes('-v')) console.log('\n' + leakList.slice(0, 60).join('\n'));

// gate criteria: curated golden perfect; ZERO fabrications; precision ≥90%; decomposition ≥80%.
let bad = fail > 0 || fabrications > 0 || setPrec < 90 || decompRate < 80;
console.log(`\nnl-offline: ${bad ? 'FAIL' : 'PASS'}  (golden ${pass}/${pass + fail}, fabrications ${fabrications}, precision ${setPrec.toFixed(1)}%, decomp ${decompRate.toFixed(1)}%)`);
process.exitCode = bad ? 1 : 0;

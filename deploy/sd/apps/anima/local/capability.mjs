// ANIMA Local — capability & abstention coverage.
//
// Drives the in-browser WASM engine in Node (parity.mjs loader) over the device brain at
// tools/anima-host/sd and asserts, tier by tier, that the browser brain ACTUALLY WORKS — app launch,
// facts (IT+EN), KGE forward/transitive/facet, the solver, translation — and, just as important, that
// it ABSTAINS on out-of-domain input instead of fabricating. This is the "does the brain do its job"
// gate that complements parity (which only proves WASM == native, not that either is useful).
//
//   node apps/anima/local/capability.mjs
//
// HARD rows must answer/abstain correctly (red on failure). CONDITIONAL rows depend on what this host
// snapshot happens to contain (the curated DEVICE brain has more) — they may SKIP but never go red, so
// the gate stays honest without coupling to a particular corpus build. Exit 0 = all HARD passed.
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
const sdRoot = join(repo, 'tools', 'anima-host', 'sd');
const wasmMjs = join(here, '..', 'www', 'local', 'anima-local.mjs');

if (!existsSync(wasmMjs)) { console.error('anima-local.mjs not found — run apps/anima/local/build.ps1'); process.exit(2); }
if (!existsSync(sdRoot))  { console.error('brain fixture not found at tools/anima-host/sd'); process.exit(2); }

process.env.L1_PFM = '64';   // PC-grade rerank pool, same as the browser

const AnimaLocal = (await import(pathToFileURL(wasmMjs).href)).default;
const { shape, answered } = await import(pathToFileURL(join(here, '..', 'www', 'local', 'engine.js')).href).then(async (m) => ({
  shape: m.shape,
  answered: (await import(pathToFileURL(join(here, '..', 'www', 'local', 'cascade.js')).href)).answered,
}));
const M = await AnimaLocal();

function mkdirp(vpath) { let cur = ''; for (const p of vpath.split('/').filter(Boolean)) { cur += '/' + p; try { M.FS.mkdir(cur); } catch {} } }
function mount(localDir, vdir) {
  mkdirp(vdir);
  for (const name of readdirSync(localDir)) {
    const lp = join(localDir, name), vp = vdir + '/' + name;
    if (statSync(lp).isDirectory()) mount(lp, vp); else M.FS.writeFile(vp, new Uint8Array(readFileSync(lp)));
  }
}
mount(sdRoot, '/sd');

const init = M.cwrap('anima_init', 'number', ['string']);
const reset = M.cwrap('anima_reset', null, []);
const queryJ = M.cwrap('anima_query_json', 'string', ['string', 'string']);
init('it'); init('en');

const ask = (q, lang) => { reset(); return shape(JSON.parse(queryJ(q, lang || 'it')), q, lang || 'it'); };
const askNoReset = (q, lang) => shape(JSON.parse(queryJ(q, lang || 'it')), q, lang || 'it');
const has = (r, s) => (r.reply || '').toLowerCase().includes(s.toLowerCase());

// HARD rows: [label, lang, query, predicate]. predicate(r) -> true means correct.
const HARD = [
  ['launch app',        'it', 'apri la calcolatrice',            (r) => r.action === 'launch' && /calculator/i.test(r.arg || '')],
  ['fact IT',           'it', 'cos’è un buco nero',    (r) => answered(r) && has(r, 'buco nero')],
  ['fact EN',           'en', 'what is the capital of japan',     (r) => has(r, 'tokyo')],
  ['KGE birth (forward)', 'it', 'quando è nato einstein',   (r) => has(r, '1879')],
  ['KGE capital',       'it', 'capitale della francia',          (r) => has(r, 'parigi')],
  ['KGE continent (transitive)', 'it', 'in che continente è la francia', (r) => has(r, 'europa')],
  ['facet occupation',  'it', 'che lavoro faceva einstein',      (r) => has(r, 'scienziato')],
  ['solver multiply',   'it', 'quanto fa 12 per 8',              (r) => has(r, '96')],
  ['solver sqrt',       'it', 'radice quadrata di 144',          (r) => has(r, '12')],
  ['translate IT->EN',  'it', 'traduci cane in inglese',         (r) => has(r, 'dog')],
  // ABSTENTION — the protected property: gibberish must NOT fabricate (empty reply, nothing "answered").
  ['OOD abstains',      'it', 'asdkfj qwerty zzz',               (r) => !answered(r) && !(r.reply || '').trim()],
];

// CONDITIONAL rows: return 'pass' | 'skip' | 'fail'. Depend on the corpus build; may SKIP, never go red.
const CONDITIONAL = [
  ['KGE inverse (author)', () => {
    const r = ask('chi ha scritto la divina commedia', 'it');
    if (has(r, 'dante')) return ['pass', 'answered: ' + r.reply];
    if (!answered(r)) return ['skip', 'abstains on this host snapshot (the curated device brain answers it)'];
    return ['fail', 'answered without Dante: ' + r.reply];
  }],
  ['learn-then-recall (one session)', () => {
    const teach = ask('la sala riunioni è al terzo piano', 'it');           // copula intake
    if (teach.intent !== 'teach' && teach.intent !== 'learn') {
      return ['skip', 'copula did not teach on this host snapshot (intent=' + JSON.stringify(teach.intent) + '); IDBFS persistence is browser-only'];
    }
    const recall = askNoReset('dove si trova la sala riunioni', 'it');      // same session, no reset
    return has(recall, 'terzo') ? ['pass', 'recalled: ' + recall.reply] : ['fail', 'taught but did not recall: ' + recall.reply];
  }],
];

let fail = 0, pass = 0, skip = 0;
console.log('— HARD —');
for (const [label, lang, q, pred] of HARD) {
  const r = ask(q, lang);
  let ok = false; try { ok = !!pred(r); } catch { ok = false; }
  if (ok) pass++; else fail++;
  console.log(`${ok ? 'OK  ' : 'XX  '}${label.padEnd(28)} [${lang}] ${q}`);
  if (!ok) console.log(`        got: tier=${r.tier} action=${r.action} intent=${r.intent} reply=${JSON.stringify((r.reply || '').slice(0, 90))}`);
}
console.log('\n— CONDITIONAL (skip-allowed) —');
for (const [label, run] of CONDITIONAL) {
  let verdict = 'fail', note = '';
  try { [verdict, note] = run(); } catch (e) { verdict = 'fail'; note = String(e && e.message || e); }
  if (verdict === 'pass') pass++; else if (verdict === 'skip') skip++; else fail++;
  const tag = verdict === 'pass' ? 'OK  ' : verdict === 'skip' ? '··  ' : 'XX  ';
  console.log(`${tag}${label.padEnd(28)} ${verdict.toUpperCase()} — ${note}`);
}

console.log(`\n=== capability: ${pass} pass, ${skip} skip, ${fail} fail (HARD must be 0 fail) ===`);
process.exit(fail === 0 ? 0 : 1);

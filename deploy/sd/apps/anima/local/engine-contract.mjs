// ANIMA Local — web-contract certification.
//
// Loads the in-browser WASM engine in Node (like parity.mjs), mounts the device brain from
// tools/anima-host/sd, and asserts that the JSON the web app consumes — produced by the REAL
// engine.js `shape()` over the REAL anima_query_json output — carries EVERY field index.html reads,
// on representative queries across the tiers. This pins the contract the chat depends on so a future
// shaper change can't quietly drop a field (e.g. `local`, `subject`, `content`).
//
//   node apps/anima/local/engine-contract.mjs
//
// Exit 0 = contract intact. Exit 1 = a field/marker is missing. Exit 2 = build missing (SKIP).
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
const sdRoot = join(repo, 'tools', 'anima-host', 'sd');
const wasmMjs = join(here, '..', 'www', 'local', 'anima-local.mjs');

if (!existsSync(wasmMjs)) { console.error('anima-local.mjs not found — run apps/anima/local/build.ps1'); process.exit(2); }
if (!existsSync(sdRoot))  { console.error('brain fixture not found at tools/anima-host/sd'); process.exit(2); }

// PC-grade rerank pool, same as the browser (set via setenv in wasm_main.c; mirror it here for clarity).
process.env.L1_PFM = '64';

const AnimaLocal = (await import(pathToFileURL(wasmMjs).href)).default;
const { shape } = await import(pathToFileURL(join(here, '..', 'www', 'local', 'engine.js')).href);
const M = await AnimaLocal();

function mkdirp(vpath) { let cur = ''; for (const p of vpath.split('/').filter(Boolean)) { cur += '/' + p; try { M.FS.mkdir(cur); } catch {} } }
function mount(localDir, vdir) {
  mkdirp(vdir);
  for (const name of readdirSync(localDir)) {
    const lp = join(localDir, name), vp = vdir + '/' + name;
    if (statSync(lp).isDirectory()) mount(lp, vp);
    else M.FS.writeFile(vp, new Uint8Array(readFileSync(lp)));
  }
}
mount(sdRoot, '/sd');

const init = M.cwrap('anima_init', 'number', ['string']);
const reset = M.cwrap('anima_reset', null, []);
const queryJ = M.cwrap('anima_query_json', 'string', ['string', 'string']);
init('it'); init('en');

// Re-create what engine.js query() does: raw struct JSON -> shape() -> web contract.
function ask(q, lang) { reset(); return shape(JSON.parse(queryJ(q, lang || 'it')), q, lang || 'it'); }

// Every field index.html reads off an /api/anima-shaped result.
const FIELDS = ['query', 'lang', 'tier', 'action', 'arg', 'reply', 'tool', 'confidence', 'domain',
  'intent', 'budget', 'memory', 'state', 'awaiting', 'corrected', 'trace', 'content', 'subject', 'relation', 'local'];

const QUERIES = [
  ['it', 'apri la calcolatrice'],
  ['it', 'quando è nato einstein'],
  ['it', 'capitale della francia'],
  ['it', 'traduci cane in inglese'],
  ['en', 'what is the capital of japan'],
  ['it', 'quanto fa 12 per 8'],
  ['it', 'asdkfj qwerty zzz'],            // OOD — must still be a well-formed (abstaining) contract object
];

let fail = 0;
for (const [lang, q] of QUERIES) {
  const r = ask(q, lang);
  const missing = FIELDS.filter((k) => !(k in r));
  const okFields = missing.length === 0;
  const okLocal = r.local === true;
  const okEcho = r.query === q && r.lang === lang;
  const ok = okFields && okLocal && okEcho;
  if (!ok) fail++;
  console.log(`${ok ? 'OK ' : 'XX '}[${lang}] ${q}`);
  console.log(`      tier=${r.tier} action=${r.action} domain=${r.domain} intent=${r.intent} local=${r.local} reply=${JSON.stringify((r.reply || '').slice(0, 70))}`);
  if (!okFields) console.log(`      MISSING FIELDS: ${missing.join(', ')}`);
  if (!okLocal) console.log(`      local marker not true: ${r.local}`);
  if (!okEcho) console.log(`      echo mismatch: query=${r.query} lang=${r.lang}`);
}
console.log(`\n=== engine contract: ${QUERIES.length - fail}/${QUERIES.length} well-formed, ${fail} broken ===`);
process.exit(fail === 0 ? 0 : 1);

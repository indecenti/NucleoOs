// UNIFY cert: prove the unified device AKB5 (base + staged knowledge) at /data/anima/ on the REAL device
// runtime (WASM, D=192) (a) never fabricates on adversarial traps, (b) keeps base recall, (c) answers the
// NEW knowledge. PASS = 0 fabrications AND base recall intact AND most new-knowledge queries answer.
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
const dev = join(repo, 'tools', 'sd-sim', 'data', 'anima');
const AnimaLocal = (await import(pathToFileURL(join(here, '..', 'www', 'local', 'anima-local.mjs')).href)).default;
const M = await AnimaLocal();
const mkdirp = (d) => { let c = ''; for (const s of d.split('/').filter(Boolean)) { c += '/' + s; try { M.FS.mkdir(c); } catch {} } };
const put = (f, vp) => { mkdirp(vp.slice(0, vp.lastIndexOf('/'))); M.FS.writeFile(vp, new Uint8Array(readFileSync(f))); };
for (const f of ['anima-it-encoder.bin', 'anima-it-index.bin', 'commands.it.json', 'dict-it-en.tsv', 'dict-en-it.tsv', 'anima-it-akb5.bin'])
  if (existsSync(join(dev, f))) put(join(dev, f), '/sd/data/anima/' + f);
for (const f of ['mind.it.jsonl', 'mind.en.jsonl', 'facets.it.jsonl', 'facets.en.jsonl'])
  if (existsSync(join(dev, 'learned', f))) put(join(dev, 'learned', f), '/sd/data/anima/learned/' + f);
for (const f of readdirSync(join(dev, 'akb5'))) put(join(dev, 'akb5', f), '/sd/data/anima/akb5/' + f);
const init = M.cwrap('anima_init', 'number', ['string']), qj = M.cwrap('anima_query_json', 'string', ['string', 'string']), rst = M.cwrap('anima_reset', null, []);
init('it');
const q = (s) => { rst(); const r = JSON.parse(qj(s, 'it')); return { s, t: r.tier, reply: (r.reply || '').trim() }; };
// (a) adversarial — every one must abstain
const TRAPS = ['capitale di Marte', 'capitale della Luna', 'quando è nato il fiume Po', 'quando è morto Babbo Natale',
  'chi è il presidente dei gatti', 'chi ha inventato l\'acqua', 'quanti anni ha l\'aria', 'chi è il re d\'Italia oggi',
  'chi è Mario Rossi di Bergamo', 'asdkfj qwerty zzz', 'quando è nato il numero 7', 'capitale di Atlantide',
  'colore della felicità', 'peso di un sogno', 'presidente della Francia nel 1200', 'chi è il dottor Inesistente',
  'quando finisce il mondo', 'qual è la capitale del nulla', 'chi ha scritto il vento', 'in che anno è nato il futuro',
  'chi è il capo dei dinosauri', 'quanto pesa internet', 'che giorno è nato il sole', 'capitale dei sogni'];
// (b) base recall — must still answer (no regression vs base brain)
const BASE = ['capitale della francia', 'cos\'è python', 'quanto fa 12 per 8', 'cos\'è un microcontrollore',
  'cos\'è esp32', 'cos\'è la fotosintesi', 'capitale del giappone', 'cos\'è un algoritmo'];
// (c) NEW knowledge — should answer (people / concepts / schemes / countries)
const NEW = ['chi è Galileo Galilei', 'quando è nato Isaac Newton', 'chi era Alan Turing', 'chi è Ada Lovelace',
  'cos\'è una closure', 'cos\'è WebAssembly', 'cos\'è il DOM', 'cos\'è la ricorsione',
  'complessità big o', 'codici http', 'modello osi', 'comandi git',
  'capitale della germania', 'capitale del brasile'];
const tR = TRAPS.map(q), bR = BASE.map(q), nR = NEW.map(q);
const fab = tR.filter((x) => x.reply.length > 0);
const blost = bR.filter((x) => !x.reply.length);
const nans = nR.filter((x) => x.reply.length > 0);
console.log('=== (a) TRAPS — fabrications (must be 0) ===');
fab.forEach((x) => console.log(`   ✗ ${x.s} -> ${x.reply.slice(0, 70)}`));
if (!fab.length) console.log('   ✓ all 24 abstained');
console.log('\n=== (b) BASE recall — lost (must be 0) ===');
blost.forEach((x) => console.log(`   ✗ ${x.s}`));
if (!blost.length) console.log(`   ✓ all ${BASE.length} answered`);
console.log('\n=== (c) NEW knowledge — answered ===');
nR.forEach((x) => console.log(`   ${x.reply.length ? '✓' : '·'} ${x.s}${x.reply.length ? ' -> ' + x.reply.slice(0, 64) : ' (abstain)'}`));
console.log(`\n=== unified AKB5: ${fab.length}/${TRAPS.length} fab · base ${BASE.length - blost.length}/${BASE.length} · new ${nans.length}/${NEW.length} ===`);
process.exit(fab.length === 0 && blost.length === 0 ? 0 : 1);

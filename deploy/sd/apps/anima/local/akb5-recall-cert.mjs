// Certify the extended AKB5 for the Local: mount device base + extended shards into the WASM (AKB5 on),
// then check (a) NEW people are recalled, (b) EXISTING key queries aren't regressed, (c) abstention holds
// (zero fabrications). Decides whether AKB5 is shippable for the Local with the new knowledge.
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..', '..');
const base = join(repo, 'tools', 'sd-sim', 'data', 'anima');
const ext = join(repo, 'deploy', 'sd-safe', 'data', 'anima-local');
const AnimaLocal = (await import(pathToFileURL(join(here, '..', 'www', 'local', 'anima-local.mjs')).href)).default;
const M = await AnimaLocal();
const mkdirp = (d) => { let c = ''; for (const s of d.split('/').filter(Boolean)) { c += '/' + s; try { M.FS.mkdir(c); } catch {} } };
const put = (f, vp) => { mkdirp(vp.slice(0, vp.lastIndexOf('/'))); M.FS.writeFile(vp, new Uint8Array(readFileSync(f))); };
for (const f of ['anima-it-encoder.bin', 'commands.it.json', 'dict-it-en.tsv', 'dict-en-it.tsv'])
  if (existsSync(join(base, f))) put(join(base, f), '/sd/data/anima/' + f);
for (const f of ['mind.it.jsonl', 'mind.en.jsonl', 'facets.it.jsonl', 'facets.en.jsonl'])
  if (existsSync(join(base, 'learned', f))) put(join(base, 'learned', f), '/sd/data/anima/learned/' + f);
if (existsSync(join(base, 'anima-it-index.bin'))) put(join(base, 'anima-it-index.bin'), '/sd/data/anima/anima-it-index.bin');
put(join(ext, 'anima-it-akb5.bin'), '/sd/data/anima/anima-it-akb5.bin');
for (const f of readdirSync(join(ext, 'akb5'))) put(join(ext, 'akb5', f), '/sd/data/anima/akb5/' + f);
const init = M.cwrap('anima_init', 'number', ['string']), qj = M.cwrap('anima_query_json', 'string', ['string', 'string']), rst = M.cwrap('anima_reset', null, []);
init('it');
const q = (s, l) => { rst(); const r = JSON.parse(qj(s, l || 'it')); return { q: s, tier: r.tier, reply: (r.reply || '').trim() }; };
// sample NEW people across the imported file
const people = readFileSync(join(repo, 'tools', 'anima', 'knowledge.staged', 'people-wd.jsonl'), 'utf8').trim().split('\n').map((l) => JSON.parse(l));
const pick = []; for (let i = 0; i < people.length; i += Math.floor(people.length / 10)) pick.push(people[i]);
const names = pick.slice(0, 10).map((c) => c.ask.it[0].replace(/^chi è /, ''));
const NEW = names.map((n) => q('chi è ' + n));
const EXIST = ['capitale della francia', 'cos\'è la fotosintesi', 'quando è nato einstein', 'quanto fa 12 per 8', 'capitale del giappone'].map((s) => q(s));
const ABSTAIN = ['capitale di Marte', 'quando è nato il fiume Po', 'asdkfj qwerty zzz', 'chi è il presidente dei gatti'].map((s) => q(s));
const line = (x) => `   ${x.reply ? '•' : '∅'} ${x.q} -> [${x.tier}] ${x.reply.slice(0, 56) || '(abstains)'}`;
console.log('NEW people (must answer):\n' + NEW.map(line).join('\n'));
console.log('EXISTING (regression check):\n' + EXIST.map(line).join('\n'));
console.log('ABSTENTION (must be empty):\n' + ABSTAIN.map(line).join('\n'));
const newOk = NEW.filter((x) => x.reply).length, regress = EXIST.filter((x) => !x.reply).length, fab = ABSTAIN.filter((x) => x.reply).length;
console.log(`\n=== extended-AKB5: NEW recalled ${newOk}/${NEW.length} · EXISTING regressions ${regress}/${EXIST.length} · fabrications ${fab} (must be 0) ===`);
process.exit(fab === 0 ? 0 : 1);

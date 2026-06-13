// FULL halluc cert on the UNIFIED device AKB5 (D=192, the real device runtime). Runs every adversarial
// trap from the project's 8 eval_halluc*.jsonl files (~669) and asserts ZERO fabrications. Judges by
// CONTENT (device answers correct cases at tier=none via compose/MOSAICO, so a tier check would be wrong):
// a trap FABRICATES iff its reply is a non-empty, non-decline, non-leak substantive answer.
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
// AKB5_DIR env overrides the manifest+shards source (for a base-only baseline); else use sd-sim (unified).
const akbDir = process.env.AKB5_DIR ? process.env.AKB5_DIR : dev;
const manifest = process.env.AKB5_DIR ? join(akbDir, 'anima-it-akb5.bin') : join(dev, 'anima-it-akb5.bin');
for (const f of ['anima-it-encoder.bin', 'anima-it-index.bin', 'commands.it.json', 'dict-it-en.tsv', 'dict-en-it.tsv'])
  if (existsSync(join(dev, f))) put(join(dev, f), '/sd/data/anima/' + f);
if (existsSync(manifest)) put(manifest, '/sd/data/anima/anima-it-akb5.bin');
for (const f of ['mind.it.jsonl', 'mind.en.jsonl', 'facets.it.jsonl', 'facets.en.jsonl'])
  if (existsSync(join(dev, 'learned', f))) put(join(dev, 'learned', f), '/sd/data/anima/learned/' + f);
const shardDir = process.env.AKB5_DIR ? join(akbDir, 'akb5') : join(dev, 'akb5');
for (const f of readdirSync(shardDir)) put(join(shardDir, f), '/sd/data/anima/akb5/' + f);
console.error('[cert] AKB5 source: ' + (process.env.AKB5_DIR || 'sd-sim (unified)'));
const init = M.cwrap('anima_init', 'number', ['string']), qj = M.cwrap('anima_query_json', 'string', ['string', 'string']), rst = M.cwrap('anima_reset', null, []);
init('it');
// canonical decline/leak detectors (mirrored from tools/anima-host/halluc-probe.mjs + device-akb5-probe.mjs)
const DECLINE = /\b(non (lo )?so|non ho|non posso|non sono in grado|non riesco|non trovo|non conosco|non dispongo|non.* disponibile|nessun(a)? (informazione|risultato|dato)|fuori (dalla mia |portata)|non.* offline|non.* certezza|chiarire|puoi precisare|cosa intendi|specifica|non.* sicur|non calcolo|non (è |e )?definit[oa]|interi non negativi|posso aprire|intendi|did you mean)\b/i;
const DECLINE_EN = /\b(i (don't|do not|can't|cannot) |i'm not |i am not |no information|not available|not sure|unable|don't have|do not have|can you clarify|what do you mean|please specify|out of (my )?scope|offline|undefined|(don't|do not) compute|non.negative|not a (place|person))\b/i;
const isDecline = (s) => DECLINE.test(s) || DECLINE_EN.test(s);
const isLeak = (s) => /\{[a-z_]+\}/i.test(s);
const FILES = ['eval_halluc_it', 'eval_halluc_en', 'eval_halluc2_it', 'eval_halluc2_en', 'eval_halluc_math_it', 'eval_halluc_math_en', 'eval_halluc3_it', 'eval_halluc3_en'];
let total = 0, fab = 0; const fails = [];
for (const fn of FILES) {
  const p = join(repo, 'tools', 'anima', fn + '.jsonl');
  if (!existsSync(p)) continue;
  const items = readFileSync(p, 'utf8').split(/\r?\n/).filter((l) => l.trim() && !l.startsWith('//')).map((l) => JSON.parse(l));
  for (const it of items) {
    rst();
    const r = JSON.parse(qj(it.q, it.lang === 'en' ? 'en' : 'it'));
    const reply = (r.reply || '').replace(/\s+/g, ' ').trim();
    total++;
    const abstained = reply === '' || isDecline(reply);
    if (!abstained || isLeak(reply)) { fab++; fails.push({ q: it.q, lang: it.lang, tier: r.tier, reply: reply.slice(0, 80) }); }
  }
}
console.log(`\n=== FULL halluc cert on UNIFIED device AKB5 — ${total} adversarial traps ===`);
if (fails.length) { console.log(`FABRICATIONS (${fab}):`); for (const f of fails) console.log(`  ✗ [${f.lang}] "${f.q}" -> (${f.tier}) ${f.reply}`); }
else console.log('✓ ZERO fabrications — every unanswerable question honestly abstained.');
console.log(`\nRESULT: ${fab}/${total} fabrications`);
process.exit(fab === 0 ? 0 : 1);

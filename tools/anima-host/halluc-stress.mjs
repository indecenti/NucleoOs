#!/usr/bin/env node
// HALLUCINATION STRESS — drives the REAL compiled pipeline (anima.exe) over HUNDREDS of
// combinatorially-generated questions whose precise answer ANIMA provably does NOT hold offline:
//   - scoped superlatives  ("il fiume più lungo DELLA MONGOLIA")   -> only a global default exists
//   - sub-national capitals ("la capitale DEL MAHARASHTRA")        -> ANIMA has countries, not states
//   - false-premise / fictional ("la capitale DI WAKANDA", "quando è morto <living>")
// Every line is expect:none: a confident L1/fact (incl. hdc) or remote answer = a hallucination.
// Measures the evidence-coverage / premise guards AT SCALE. Exit != 0 if ANY hallucination.
//
//   node tools/anima-host/halluc-stress.mjs            # full sweep
//   node tools/anima-host/halluc-stress.mjs --show     # list every generated phrase + verdict
import { spawnSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');
if (!existsSync(exe)) { console.error('anima.exe missing — run `npm run anima:build`.'); process.exit(2); }

const SUP_IT = ['il fiume piu lungo', 'il lago piu grande', 'il lago piu profondo', 'la montagna piu alta',
  'la vetta piu alta', 'il deserto piu grande', "l isola piu grande", 'la cascata piu alta',
  'il vulcano piu alto', 'la citta piu popolosa', 'il grattacielo piu alto', "l edificio piu alto",
  'il ghiacciaio piu grande', 'la foresta piu grande'];
const SCOPE_IT = ['della siberia', 'della mongolia', 'del kazakistan', 'della cina', 'della russia',
  "dell africa", "dell asia", "dell oceania", 'del sudamerica', 'delle ande', "dell himalaya",
  'della scandinavia', 'del borneo', 'del madagascar', 'della patagonia', "dell alaska",
  'della baviera', 'del maharashtra', 'della groenlandia', "dell indonesia",
  // apostrophe-glued (elided) prepositions — these bypassed the scope guard until fixed
  "dell'antartide", "dell'irlanda", "dell'europa orientale", "dell'arabia saudita", "dall'islanda"];
const SUBNAT_IT = ['della california', 'del texas', "dell oregon", 'della baviera', 'della sassonia',
  'del karnataka', 'del maharashtra', 'del gujarat', 'della catalogna', 'del queensland',
  "dell ontario", 'del quebec', 'della galizia', 'del kerala', 'della renania', 'del michigan'];
const FICT_IT = ['della luna', 'di marte', 'di atlantide', 'di narnia', 'di wakanda', 'di gondor',
  'di hogwarts', 'di mordor', 'di asgard'];
const LIVING = ['cristiano ronaldo', 'elon musk', 'taylor swift', 'lionel messi', 'jannik sinner', 'rafael nadal'];

const SUP_EN = ['the longest river', 'the largest lake', 'the highest mountain', 'the largest desert',
  'the most populous city', 'the tallest building'];
const SCOPE_EN = ['in mongolia', 'in siberia', 'in the andes', 'in south america', 'in africa',
  'in kazakhstan', 'in borneo', 'in patagonia', 'in bavaria', 'in maharashtra'];

const traps = [];
for (const s of SUP_IT) for (const sc of SCOPE_IT) traps.push({ q: `qual e ${s} ${sc}`, lang: 'it', cat: 'sup-scope' });
for (const sc of SUBNAT_IT) traps.push({ q: `qual e la capitale ${sc}`, lang: 'it', cat: 'subnat-cap' });
for (const f of FICT_IT) traps.push({ q: `qual e la capitale ${f}`, lang: 'it', cat: 'fiction-cap' });
for (const p of LIVING) traps.push({ q: `quando e morto ${p}`, lang: 'it', cat: 'false-premise' });
for (const s of SUP_EN) for (const sc of SCOPE_EN) traps.push({ q: `what is ${s} ${sc}`, lang: 'en', cat: 'sup-scope-en' });

// One REPL stream: /reset (+ /it|/en) before each.
let lang = 'it';
const lines = [];
for (const t of traps) {
  lines.push('/reset');
  const want = t.lang === 'en' ? 'en' : 'it';
  if (want !== lang) { lines.push('/' + want); lang = want; }
  lines.push(t.q);
}
const r = spawnSync(exe, [], { input: Buffer.from(lines.join('\n') + '\n', 'utf8'), maxBuffer: 64 * 1024 * 1024 });
const blocks = r.stdout.toString('utf8').split(/^Q: /m).slice(1);
const parse = (b) => ({
  tier: (b.match(/tier=(\S+)/) || [])[1] || 'none',
  reply: ((b.match(/reply: (.*)/) || [])[1] || '').trim(),
});

const show = process.argv.includes('--show');
const byCat = {};
const halluc = [];
for (let i = 0; i < traps.length; i++) {
  const t = traps[i], p = parse(blocks[i] || '');
  const answered = (p.tier === 'L1/fact' || p.tier === 'L3/remote') && p.reply;
  byCat[t.cat] = byCat[t.cat] || { n: 0, fp: 0 };
  byCat[t.cat].n++;
  if (answered) { byCat[t.cat].fp++; halluc.push([t.cat, t.q, p.reply.slice(0, 46)]); }
  if (show) console.log(`${answered ? 'HALLUC' : '  ok  '} [${t.cat}] "${t.q}"${answered ? ` -> ${p.reply.slice(0, 50)}` : ''}`);
}

console.log(`\n[halluc-stress] ${traps.length} guaranteed-unanswerable phrases over the real pipeline`);
for (const [cat, s] of Object.entries(byCat)) console.log(`  ${cat.padEnd(16)} ${s.n - s.fp}/${s.n} abstain  ${s.fp ? `(${s.fp} HALLUC)` : ''}`);
console.log(`  TOTAL HALLUCINATIONS: ${halluc.length}/${traps.length}`);
if (halluc.length) {
  console.log('\nHALLUCINATIONS:');
  for (const [cat, q, rep] of halluc.slice(0, 40)) console.log(`  ✗ [${cat}] "${q}" -> "${rep}"`);
  process.exit(1);
}
console.log('✓ zero hallucinations — every unanswerable question was honestly refused.');

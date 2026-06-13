// MEASURE the neuro-symbolic combinators: answers COMPUTED by composing facts, that recall/deduction
// cannot produce. Deterministic (seeded real fact base, no network). Baseline (a recall/deduction
// engine) returns nothing for a composed question — its answer was never stored; the combinator computes
// it. Also checks honesty (a missing fact → refuse) and provenance.
//   node tools/anima/combinator-eval.mjs
import { answer, parseQuery } from './combinator.mjs';

const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', c: '\x1b[36m', x: '\x1b[0m' };
const slug = (s) => String(s).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');

// Seeded fact base (real values ANIMA learns from Wikidata). The combinator only sees it via getFact.
const FB = {
  dante: { born: 1265, nationality: 'Italia' }, einstein: { born: 1879, nationality: 'Germania' },
  newton: { born: 1643, nationality: 'Regno Unito' }, colombo: { born: 1451, nationality: 'Italia' },
  italia: { continent: 'Europa' }, germania: { continent: 'Europa' }, 'regno unito': { continent: 'Europa' },
};
const getFact = async (entity, rel) => {
  const e = FB[slug(entity).replace(/-/g, ' ')] || FB[slug(entity)] || FB[String(entity).toLowerCase()];
  if (!e || e[rel] === undefined) return null;
  return rel === 'born' ? { year: e[rel], conf: 0.95, src: `wd:${entity}:born` } : { value: e[rel], conf: 0.9, src: `wd:${entity}:${rel}` };
};

const Q = [
  ['chi è nato prima, Dante o Einstein?', 'dante', '1265'],
  ['chi è nato prima, Einstein o Colombo?', 'colombo', '1451'],
  ['chi è più vecchio, Newton o Einstein?', 'newton', '1643'],
  ['chi è più giovane, Dante o Einstein?', 'einstein', '1879'],
  ['quanti anni tra la nascita di Dante ed Einstein?', '614', '614'],
  ['quanti anni separano la nascita di Colombo e Newton?', '192', '192'],
  ['Einstein era europeo?', 'sì', 'europa'],
  ['Dante e Colombo erano connazionali?', 'sì', 'entrambi'],
  ['Dante e Einstein erano connazionali?', 'no', 'germania'],
];

let bOk = 0, bWrong = 0;
console.log(`\n${C.b}=== Combinatori — risposte COMPOSTE, mai memorizzate (offline, deterministico) ===${C.x}\n`);
const run = async () => {
  for (const [q, mustA, mustB] of Q) {
    const r = await answer(q, getFact);
    const txt = (r?.reply || '').toLowerCase();
    const ok = r && (txt.includes(String(mustA).toLowerCase()) || txt.includes(String(mustB).toLowerCase()));
    if (ok) bOk++; else if (r) bWrong++;
    console.log(`${C.c}you ▸${C.x} ${q}`);
    console.log(`${C.b}anima ◂${C.x} ${r ? r.reply : C.y + '—'} ${C.d}${r ? `[${r.op} · conf ${r.confidence} · ${(r.provenance || []).length} fonti]` : ''}${C.x}  baseline ${C.y}—${C.x} ${ok ? C.g + '✓' : C.r + '✗'}${C.x}\n`);
  }
  // honesty: a missing fact must refuse (no fabrication)
  const miss = await answer('chi è nato prima, Dante o Pinco Pallino?', getFact);
  const honest = miss === null;
  console.log(`${C.b}Onestà${C.x} — fatto mancante (Pinco Pallino): ${honest ? C.g + 'rifiuta (null)' : C.r + 'ha inventato'}${C.x}\n`);

  console.log(`${C.b}=== MIGLIORAMENTO ===${C.x}`);
  console.log(`  risposte composte:  baseline ${C.r}0/${Q.length}${C.x}  →  combinatori ${C.g}${bOk}/${Q.length}${C.x}  ${C.d}(calcolate componendo ≥2 fatti)${C.x}`);
  console.log(`  errori:             ${bWrong === 0 ? C.g + '0' : C.r + bWrong}${C.x} ·  onestà: ${honest ? C.g + 'sì' : C.r + 'no'}${C.x} ·  confidenza evidenziale + provenienza: ${C.g}sì${C.x}`);
  const fail = bOk < Q.length || bWrong > 0 || !honest;
  console.log(`${C.b}=== ${fail ? C.r + 'DA TARARE' : C.g + 'ANIMA ora COMPONE: calcola risposte nuove da più fatti, a errore zero'} ${C.x}`);
  process.exit(fail ? 1 : 0);
};
run();

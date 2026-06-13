// MEASURE the improvement the HDC reasoning tier brings, OFFLINE and deterministic (no network).
// A/B over the SAME learned facts:
//   A = baseline: exact/substring recall of stored phrasings (what learnedLookup does today, offline).
//   B = HDC reasoning: unbind the relation from the grown "mind" (phrasing-robust) + analogy + multi-hop,
//       gated by intrinsic coherence (refuse rather than misattribute).
// We report OFFLINE coverage (correct answers with no network), wrong answers (must stay 0), and the
// purely additive capabilities (analogy, multi-hop) the exact-match cache simply cannot do.
//   node tools/anima/hdc-eval.mjs
import { Mind } from './hdc.mjs';

const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', c: '\x1b[36m', x: '\x1b[0m' };
const slug = (s) => String(s).toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '');
const GATE = 4.0;

// Facts ANIMA learned (subject --rel--> value), each with the phrasings the exact-match cache stored.
const FACTS = [
  { subj: 'giappone', rel: 'capital', val: 'tokyo', asks: ['capitale di giappone', 'qual è la capitale di giappone'] },
  { subj: 'francia', rel: 'capital', val: 'parigi', asks: ['capitale di francia'] },
  { subj: 'egitto', rel: 'capital', val: 'il cairo', asks: ['capitale di egitto'] },
  { subj: 'italia', rel: 'capital', val: 'roma', asks: ['capitale di italia'] },
  { subj: 'einstein', rel: 'born', val: '1879', asks: ['quando è nato einstein', 'data di nascita di einstein'] },
  { subj: 'giappone', rel: 'valuta', val: 'yen', asks: ['valuta del giappone'] },
  { subj: 'stati uniti', rel: 'valuta', val: 'dollaro', asks: ['valuta degli stati uniti'] },
  { subj: 'giappone', rel: 'lingua', val: 'giapponese', asks: ['lingua del giappone'] },
  { subj: 'francia', rel: 'lingua', val: 'francese', asks: ['lingua della francia'] },
  { subj: 'egitto', rel: 'continente', val: 'africa', asks: ['continente egitto'] },
  { subj: 'giappone', rel: 'continente', val: 'asia', asks: ['continente giappone'] },
];

// B: the HDC mind.
const m = new Mind();
for (const f of FACTS) m.learn(f.subj, f.rel, f.val);

// A: faithful replica of learnedLookup's offline exact/substring match over stored ask slugs.
const store = FACTS.flatMap(f => f.asks.map(a => ({ s: slug(a), val: f.val })));
function baseline(query) {
  const qs = slug(query);
  for (const e of store) if (qs === e.s || (e.s.length >= 6 && qs.includes(e.s)) || (qs.length >= 6 && e.s.includes(qs))) return e.val;
  return null;
}
function hdc(subj, rel) { const r = m.ask(slug(subj), rel); return r.coherence >= GATE ? r.key : null; }

// --- 1) RECALL: varied phrasings NOT stored verbatim (the entity/relation is parsed upstream; here we
//        compare the RECALL mechanism). Each: the query, its (subject,relation), and the truth. ---
const RECALL = [
  ['capitale di giappone', 'giappone', 'capital', 'tokyo'],            // control: baseline SHOULD get this
  ['qual è la capitale del giappone', 'giappone', 'capital', 'tokyo'], // "del" ≠ stored "di"
  ['qual è la capitale della francia', 'francia', 'capital', 'parigi'],
  ["capitale dell'egitto", 'egitto', 'capital', 'il cairo'],
  ["mi dici la capitale d'italia", 'italia', 'capital', 'roma'],
  ['in che anno è nato einstein', 'einstein', 'born', '1879'],
  ['che lingua si parla in francia', 'francia', 'lingua', 'francese'],
  ['che moneta usa il giappone', 'giappone', 'valuta', 'yen'],
];
let aCov = 0, bCov = 0, aWrong = 0, bWrong = 0;
console.log(`\n${C.b}=== HDC eval — miglioramento misurato, OFFLINE ===${C.x}\n`);
console.log(`${C.b}1) RECALL relazionale (frasi non memorizzate verbatim)${C.x}`);
console.log(`   ${C.d}${'query'.padEnd(34)} baseline      HDC${C.x}`);
for (const [q, s, rel, truth] of RECALL) {
  const a = baseline(q), bb = hdc(s, rel);
  if (a === truth) aCov++; else if (a) aWrong++;
  if (bb === truth) bCov++; else if (bb) bWrong++;
  const fmt = (x) => x === truth ? `${C.g}${x}${C.x}` : x ? `${C.r}${x}✗${C.x}` : `${C.y}—${C.x}`;
  console.log(`   ${q.padEnd(34)} ${fmt(a).padEnd(22)} ${fmt(bb)}`);
}
console.log(`   ${C.d}copertura offline:  baseline ${aCov}/${RECALL.length}  →  HDC ${bCov}/${RECALL.length}   (errori: ${aWrong} / ${bWrong})${C.x}\n`);

// --- 2) ANALOGY: impossible for an exact-match cache; pure additive capability. ---
const ANALOGY = [
  ['giappone', 'francia', 'tokyo', 'parigi'],
  ['giappone', 'stati uniti', 'yen', 'dollaro'],
  ['francia', 'giappone', 'francese', 'giapponese'],
];
console.log(`${C.b}2) ANALOGIA${C.x} ${C.d}(baseline: impossibile)${C.x}`);
let anOk = 0; for (const [a, bx, va, truth] of ANALOGY) { const res = m.analogy(a, bx, va); const ok = res.coherence >= GATE && res.key === truth; if (ok) anOk++;
  console.log(`   ${(va + ' : ' + a + '  ::  ? : ' + bx).padEnd(40)} baseline ${C.y}—${C.x}   HDC ${ok ? C.g + res.key : C.r + (res.key || '—')}${C.x} ${C.d}(coh ${res.coherence.toFixed(1)})${C.x}`); }
console.log(`   ${C.d}analogie risolte:  baseline 0/${ANALOGY.length}  →  HDC ${anOk}/${ANALOGY.length}${C.x}\n`);

// --- 3) MULTI-HOP: capitale⁻¹ then continente. Additive capability. ---
console.log(`${C.b}3) MULTI-HOP${C.x} ${C.d}(baseline: impossibile)${C.x}`);
let mhOk = 0; const MH = [['il cairo', 'africa'], ['tokyo', 'asia']];
for (const [cap, truth] of MH) {
  let best = null, bc = -1;
  for (const f of FACTS) if (f.rel === 'capital') { const rr = m.ask(f.subj, 'capital'); if (rr.key === cap && rr.coherence > bc) { bc = rr.coherence; best = f.subj; } }
  const hop2 = best ? m.ask(best, 'continente') : { key: null, coherence: 0 };
  const ok = hop2.coherence >= GATE && hop2.key === truth; if (ok) mhOk++;
  console.log(`   ${('continente del paese con capitale ' + cap).padEnd(40)} baseline ${C.y}—${C.x}   HDC ${ok ? C.g + hop2.key : C.r + (hop2.key || '—')}${C.x} ${C.d}(coh ${hop2.coherence.toFixed(1)})${C.x}`);
}
console.log(`   ${C.d}catene risolte:  baseline 0/${MH.length}  →  HDC ${mhOk}/${MH.length}${C.x}\n`);

// --- 4) HONESTY: questions with no answer in memory — BOTH must refuse (no fabrication). ---
const HON = [['einstein', 'capital'], ['giappone', 'born'], ['dante', 'valuta'], ['francia', 'born']];
console.log(`${C.b}4) ONESTÀ${C.x} ${C.d}(nessuno deve inventare)${C.x}`);
let aRef = 0, bRef = 0;
for (const [s, rel] of HON) { const a = baseline(`${rel} di ${s}`); const bb = hdc(s, rel); if (!a) aRef++; if (!bb) bRef++;
  console.log(`   ${(rel + ' di ' + s).padEnd(40)} baseline ${a ? C.r + a : C.g + 'rifiuta'}${C.x}   HDC ${bb ? C.r + bb : C.g + 'rifiuta'}${C.x}`); }
console.log(`   ${C.d}rifiuti corretti:  baseline ${aRef}/${HON.length}  ·  HDC ${bRef}/${HON.length}${C.x}\n`);

// --- verdict ---
const dCov = bCov - aCov, total = RECALL.length;
const newCaps = anOk + mhOk;
console.log(`${C.b}=== MIGLIORAMENTO ===${C.x}`);
console.log(`  recall offline:   ${C.g}+${dCov}${C.x} domande (${aCov}→${bCov} su ${total}, ${(100 * dCov / total).toFixed(0)}% in più senza rete)`);
console.log(`  capacità nuove:   ${C.g}+${newCaps}${C.x} (${anOk} analogie + ${mhOk} multi-hop, prima impossibili)`);
console.log(`  errori introdotti: ${bWrong === 0 ? C.g + '0' : C.r + bWrong}${C.x} ·  onestà preservata: ${bRef === HON.length ? C.g + 'sì' : C.r + 'no'}${C.x}`);
const fail = (dCov <= 0) || (bWrong > 0) || (anOk < ANALOGY.length) || (mhOk < MH.length) || (bRef < HON.length);
console.log(`${C.b}=== ${fail ? C.r + 'REGRESSIONE' : C.g + 'HDC migliora la copertura offline e aggiunge ragionamento, a errore zero'} ${C.x}`);
process.exit(fail ? 1 : 0);

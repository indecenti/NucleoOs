// MEASURE the leap from RECALL to INFERENCE. We store a knowledge graph, then ask for facts that were
// NEVER stored but are logically ENTAILED (transitive closures, inverses, multi-hop chains). A recall
// engine can only answer what it was told → 0 on these. The permutation-KGE DEDUCES them by composing
// rotations. We also check it stays HONEST (refuses non-entailed) and finds chains AUTONOMOUSLY.
//   node tools/anima/kge-eval.mjs
import { KG, GATE } from './kge.mjs';

const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', c: '\x1b[36m', x: '\x1b[0m' };

// ---- the knowledge ANIMA was TOLD (stored triples) ----
const STORED = [
  ['parigi', 'si_trova_in', 'francia'], ['lione', 'si_trova_in', 'francia'],
  ['francia', 'si_trova_in', 'europa'], ['italia', 'si_trova_in', 'europa'],
  ['roma', 'si_trova_in', 'italia'], ['tokyo', 'si_trova_in', 'giappone'],
  ['giappone', 'si_trova_in', 'asia'], ['europa', 'si_trova_in', 'emisfero_nord'],
  ['parigi', 'capitale_di', 'francia'], ['roma', 'capitale_di', 'italia'], ['tokyo', 'capitale_di', 'giappone'],
];
const kg = new KG(); for (const [h, r, t] of STORED) kg.add(h, r, t); kg.build();

// recall baseline: answers ONLY an exactly-stored (h,r,t).
const storedSet = new Set(STORED.map(([h, r, t]) => `${h}|${r}|${t}`));
const recall = (h, rels, t) => rels.length === 1 && storedSet.has(`${h}|${rels[0]}|${t}`);

let aOk = 0, bOk = 0, bWrong = 0, n = 0;
const row = (q, truth, baselineHit, inf) => {
  n++; if (baselineHit) aOk++;
  const got = inf.coherence >= GATE ? inf.key : null;
  const good = got === truth; if (good) bOk++; else if (got) bWrong++;
  console.log(`   ${q.padEnd(44)} recall ${baselineHit ? C.g + truth : C.y + '—'}${C.x}   KGE ${good ? C.g + got : got ? C.r + got + '✗' : C.y + '—'}${C.x} ${C.d}(coh ${inf.coherence.toFixed(1)})${C.x}`);
};

console.log(`\n${C.b}=== KGE eval — deduzione di fatti MAI memorizzati (offline) ===${C.x}\n`);

console.log(`${C.b}1) TRANSITIVITÀ${C.x} ${C.d}(si_trova_in ∘ si_trova_in — mai memorizzato)${C.x}`);
for (const [h, truth] of [['parigi', 'europa'], ['lione', 'europa'], ['roma', 'europa'], ['tokyo', 'asia']]) {
  row(`${h} si trova in ? (2 salti)`, truth, recall(h, ['si_trova_in', 'si_trova_in'], truth), kg.inferPath(h, ['si_trova_in', 'si_trova_in']));
}

console.log(`\n${C.b}2) CATENA a 3 salti${C.x} ${C.d}(città → paese → continente → emisfero)${C.x}`);
for (const [h, truth] of [['parigi', 'emisfero_nord'], ['roma', 'emisfero_nord']]) {
  row(`${h} → emisfero (3 salti)`, truth, recall(h, ['si_trova_in', 'si_trova_in', 'si_trova_in'], truth), kg.inferPath(h, ['si_trova_in', 'si_trova_in', 'si_trova_in']));
}

console.log(`\n${C.b}3) RELAZIONE INVERSA${C.x} ${C.d}(capitale_di⁻¹ = "ha per capitale" — mai memorizzato)${C.x}`);
for (const [t, truth] of [['francia', 'parigi'], ['italia', 'roma'], ['giappone', 'tokyo']]) {
  row(`qual è la capitale di ${t}? (inverso)`, truth, false, kg.inverse(t, 'capitale_di'));
}

console.log(`\n${C.b}4) ONESTÀ${C.x} ${C.d}(fatti NON entailed → deve rifiutare, non inventare)${C.x}`);
let refOk = 0; const HON = [
  ['einstein', ['si_trova_in'], 'einstein fuori dal grafo'],
  ['europa', ['capitale_di'], "l'Europa non ha una capitale"],
  ['parigi', ['si_trova_in', 'si_trova_in', 'si_trova_in', 'si_trova_in'], '4 salti oltre la catena'],
];
for (const [h, rels, why] of HON) { const inf = kg.inferPath(h, rels); const refused = inf.coherence < GATE; if (refused) refOk++;
  console.log(`   ${why.padEnd(44)} KGE ${refused ? C.g + 'rifiuta' : C.r + ('dice ' + inf.key)}${C.x} ${C.d}(coh ${inf.coherence.toFixed(1)})${C.x}`); }

console.log(`\n${C.b}5) MULTI-HOP AUTONOMO${C.x} ${C.d}(il device SCOPRE la catena da solo, non gliela diamo)${C.x}`);
const reached = kg.reach('parigi', { maxDepth: 3, gate: GATE });
const found = reached.map(x => x.entity);
for (const x of reached.slice(0, 6)) console.log(`   parigi —[${x.path.join(' → ')}]→ ${C.c}${x.entity}${C.x} ${C.d}(coh ${x.coherence.toFixed(1)})${C.x}`);
const autoOk = ['francia', 'europa', 'emisfero_nord'].every(e => found.includes(e));
console.log(`   ${autoOk ? C.g + 'pass' : C.r + 'FAIL'}${C.x} ha raggiunto da solo francia → europa → emisfero_nord\n`);

// ---- verdict ----
console.log(`${C.b}=== MIGLIORAMENTO ===${C.x}`);
console.log(`  fatti entailed dedotti:  recall ${C.r}${aOk}/${n}${C.x}  →  KGE ${C.g}${bOk}/${n}${C.x}   ${C.d}(deduzioni che il recall NON può fare)${C.x}`);
console.log(`  errori introdotti:        ${bWrong === 0 ? C.g + '0' : C.r + bWrong}${C.x}`);
console.log(`  onestà:                   ${refOk === HON.length ? C.g + 'rifiuta tutti i non-entailed' : C.r + refOk + '/' + HON.length}${C.x}`);
console.log(`  ragionamento autonomo:    ${autoOk ? C.g + 'sì (catena scoperta da solo)' : C.r + 'no'}${C.x}`);
const fail = bOk <= aOk || bWrong > 0 || refOk < HON.length || !autoOk;
console.log(`${C.b}=== ${fail ? C.r + 'DA TARARE' : C.g + 'ANIMA ora DEDUCE: risponde a fatti mai memorizzati, a errore zero, restando onesta'} ${C.x}`);
process.exit(fail ? 1 : 0);

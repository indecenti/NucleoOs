// Measure the HDC math empirically — the claims in hdc.mjs are not asserted, they are tested.
//   node tools/anima/hdc-bench.mjs
import { D, STD, randomHV, semanticHV, hamming, sim, bundle, Codebook, Mind, role, bind } from './hdc.mjs';

const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', c: '\x1b[36m', x: '\x1b[0m' };
const pct = (x) => (100 * x).toFixed(1) + '%';
const mean = (a) => a.reduce((s, v) => s + v, 0) / a.length;
const std = (a) => { const m = mean(a); return Math.sqrt(mean(a.map(v => (v - m) ** 2))); };
let fails = 0; const ok = (cond, msg) => { console.log(`  ${cond ? C.g + 'pass' : (fails++, C.r + 'FAIL')}${C.x} ${msg}`); };

console.log(`\n${C.b}=== HDC math bench ===${C.x}  ${C.d}D=${D} bits (${D / 8} B/HV) · random-Hamming std≈${STD.toFixed(0)}${C.x}\n`);

// 1) CONCENTRATION OF MEASURE: two random concepts are quasi-orthogonal (Hamming ≈ D/2, tiny spread).
{
  const ds = []; for (let i = 0; i < 400; i++) ds.push(hamming(randomHV('a' + i), randomHV('b' + i)));
  console.log(`${C.b}1. Quasi-orthogonality${C.x}  random pair Hamming = ${mean(ds).toFixed(0)} ± ${std(ds).toFixed(1)}  (ideale ${D / 2} ± ${STD.toFixed(0)})`);
  ok(Math.abs(mean(ds) - D / 2) < 3 * STD && Math.abs(std(ds) - STD) < STD, 'concetti distinti ≈ ortogonali, spread come da teoria\n');
}

// 2) SEMANTIC ATOMS: lexically/semantically near text stays CORRELATED; unrelated stays orthogonal.
{
  const near = [['gatto', 'il gatto'], ['fotosintesi', 'fotosintesi clorofilliana'], ['einstein', 'albert einstein'], ['giappone', 'il giappone']];
  const far = [['gatto', 'giappone'], ['einstein', 'tokyo'], ['fotosintesi', 'batman']];
  const sn = near.map(([a, b]) => sim(semanticHV(a), semanticHV(b)));
  const sf = far.map(([a, b]) => sim(semanticHV(a), semanticHV(b)));
  console.log(`${C.b}2. Semantic atoms (SimHash)${C.x}  vicini sim=${mean(sn).toFixed(2)}  ·  lontani sim=${mean(sf).toFixed(2)}`);
  ok(mean(sn) > 0.3 && Math.abs(mean(sf)) < 0.15, 'parafrasi restano correlate, concetti diversi restano ortogonali\n');
}

// 3) KEY→VALUE CAPACITY LAW: bundle N pairs (key_i ⊗ value_i) into ONE hypervector, then recover a
//    value by unbinding with its key + cleanup. This is exactly how Mind stores facts (role⊗value), so
//    it's the capacity that matters. Each unbind = target + crosstalk from the other N-1 pairs; recall
//    stays high while N is moderate, then degrades GRACEFULLY. Cleanup is over N values + M distractors.
{
  console.log(`${C.b}3. Capacità chiave→valore${C.x}  (N coppie role⊗value in UN ipervettore; recupero per unbinding, fra N+64 valori)`);
  console.log(`   ${C.d}N(fatti)   Hamming(target)↓   recall(misurato)${C.x}`);
  const Ns = [1, 3, 7, 15, 25, 40, 60], M = 64; let lastGood = 0;
  for (const N of Ns) {
    let hitsumD = 0, correct = 0, trials = 40;
    for (let t = 0; t < trials; t++) {
      const cb = new Codebook(); const keys = [], vals = [], pairs = [];
      for (let i = 0; i < N; i++) { const k = randomHV(`k${t}_${i}`); const vk = `v${t}_${i}`; const v = randomHV(vk); cb.add(vk, v); keys.push(k); vals.push(vk); pairs.push(bind(k, v)); }
      for (let i = 0; i < M; i++) cb.add(`d${t}_${i}`, randomHV(`d${t}_${i}`));   // distractor values
      const mem = bundle(pairs);
      const j = (t * 13) % N; const probe = bind(mem, keys[j]);     // unbind: mem ⊗ key_j ≈ value_j
      hitsumD += hamming(probe, cb.get(vals[j]));
      if (cb.cleanup(probe).key === vals[j]) correct++;
    }
    const rec = correct / trials; if (rec >= 0.95) lastGood = N;
    console.log(`   ${String(N).padEnd(10)} ${(hitsumD / 40 / D).toFixed(3).padEnd(17)} ${rec >= 0.95 ? C.g : rec >= 0.5 ? C.y : C.r}${pct(rec)}${C.x}`);
  }
  console.log(`   ${C.d}→ recupero ≥95% fino a N≈${lastGood} fatti in UN ipervettore da ${D / 8} B; oltre, degrada con grazia (→ "non lo so")${C.x}`);
  ok(lastGood >= 15, `capacità reale alta (N≈${lastGood} fatti/HV); il crollo è graduale, mai un cliff\n`);
}

// 4) ANALOGY (Kanerva): build country records (capital/currency/language) and answer cross-entity by
//    pure binary algebra — "la valuta del Messico" via USA↔MEX transform, no lookup of that fact.
{
  const facts = [
    ['stati uniti', { capitale: 'washington', valuta: 'dollaro', lingua: 'inglese' }],
    ['messico', { capitale: 'citta del messico', valuta: 'peso', lingua: 'spagnolo' }],
    ['giappone', { capitale: 'tokyo', valuta: 'yen', lingua: 'giapponese' }],
    ['francia', { capitale: 'parigi', valuta: 'euro', lingua: 'francese' }],
  ];
  const m = new Mind(); for (const [s, rels] of facts) for (const [r, v] of Object.entries(rels)) m.learn(s, r, v);
  const trials = [
    ['stati uniti', 'messico', 'dollaro', 'peso', 'la valuta del Messico (come il dollaro per gli USA)'],
    ['giappone', 'francia', 'tokyo', 'parigi', 'la capitale della Francia (come Tokyo per il Giappone)'],
    ['francia', 'giappone', 'francese', 'giapponese', 'la lingua del Giappone (come il francese per la Francia)'],
  ];
  console.log(`${C.b}4. Analogia per algebra binaria${C.x}`);
  let an = 0; for (const [a, b, va, exp, label] of trials) { const res = m.analogy(a, b, va); const good = res.key === exp; if (good) an++;
    console.log(`   ${good ? C.g + '✓' : C.r + '✗'}${C.x} ${label} → ${C.c}${res.key}${C.x} ${C.d}(coh ${res.coherence.toFixed(1)})${C.x}`); }
  ok(an === trials.length, `${an}/${trials.length} analogie corrette, zero generazione\n`);
}

// 5) RESONANCE COHERENCE = INTRINSIC HONESTY. Relational recall of KNOWN facts resolves with high
//    coherence; asking a relation the subject DOESN'T have resolves incoherently. The gap is the gate.
{
  const m = new Mind();
  const data = { einstein: { campo: 'fisica', nato: '1879', nazione: 'germania' },
                 dante: { opera: 'divina commedia', nato: '1265', nazione: 'italia' },
                 giappone: { capitale: 'tokyo', valuta: 'yen', continente: 'asia' } };
  for (const [s, rels] of Object.entries(data)) for (const [r, v] of Object.entries(rels)) m.learn(s, r, v);
  const known = [['einstein', 'campo'], ['einstein', 'nato'], ['dante', 'opera'], ['giappone', 'capitale'], ['giappone', 'valuta']];
  const unknown = [['einstein', 'capitale'], ['dante', 'valuta'], ['giappone', 'campo'], ['einstein', 'opera']];
  const kc = known.map(([s, r]) => m.ask(s, r).coherence);
  const uc = unknown.map(([s, r]) => m.ask(s, r).coherence);
  console.log(`${C.b}5. Coerenza di risonanza = onestà intrinseca${C.x}`);
  console.log(`   domande con risposta:  coh = ${mean(kc).toFixed(2)} ± ${std(kc).toFixed(2)}`);
  console.log(`   domande senza risposta: coh = ${mean(uc).toFixed(2)} ± ${std(uc).toFixed(2)}`);
  // A self-set gate halfway between the two populations; report separation (no hand-tuned constant).
  const gate = (mean(kc) + mean(uc)) / 2;
  const tp = kc.filter(c => c >= gate).length, tn = uc.filter(c => c < gate).length;
  console.log(`   ${C.d}gate auto = ${gate.toFixed(2)} → risponde correttamente ${tp}/${kc.length}, rifiuta correttamente ${tn}/${uc.length}${C.x}`);
  ok(mean(kc) - mean(uc) > 1.0 && tp === kc.length && tn === uc.length, 'separazione netta: il calcolo stesso dice quando NON sa\n');
}

console.log(`${C.b}=== ${fails ? C.r + fails + ' FAIL' : C.g + 'tutta la matematica regge'} ${C.x}`);
process.exit(fails);

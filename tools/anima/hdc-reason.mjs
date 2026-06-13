// ANIMA reasons OFFLINE — no network, no model, no generation. Just XOR/popcount over a hyperdimensional
// "mind" built from the kind of facts ANIMA learns (its Wikidata/Wikipedia tier). Shows the four things
// retrieval alone can't do: relational recall by UNBINDING, ANALOGY, MULTI-HOP composition, and ONE-SHOT
// learning — each gated by RESONANCE COHERENCE so the device knows when it doesn't know.
//   node tools/anima/hdc-reason.mjs
import { Mind, resonate, Codebook, bind, semanticHV } from './hdc.mjs';

const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', c: '\x1b[36m', x: '\x1b[0m' };
const GATE = 4.0;   // coherence units (≈ margin/σ). Between the two populations the bench measured (~0.3 vs ~42).
const m = new Mind();

// Facts of the shape ANIMA already learns deterministically (capital/author/born/...). Built once, offline.
const KB = {
  'giappone':    { capitale: 'tokyo', valuta: 'yen', lingua: 'giapponese', continente: 'asia' },
  'francia':     { capitale: 'parigi', valuta: 'euro', lingua: 'francese', continente: 'europa' },
  'stati uniti': { capitale: 'washington', valuta: 'dollaro', lingua: 'inglese', continente: 'america' },
  'egitto':      { capitale: 'il cairo', valuta: 'sterlina egiziana', lingua: 'arabo', continente: 'africa' },
  'einstein':    { campo: 'fisica', nato: '1879', nazione: 'germania' },
  'dante':       { opera: 'divina commedia', nato: '1265', nazione: 'italia' },
};
for (const [s, rels] of Object.entries(KB)) for (const [r, v] of Object.entries(rels)) m.learn(s, r, v);

const say = (q, res, expect) => {
  const known = res.coherence >= GATE;
  const reply = known ? res.key : '— non lo so';
  const good = expect === undefined ? true : (known ? res.key === expect : expect === null);
  console.log(`${C.c}you ▸${C.x} ${q}`);
  console.log(`${C.b}anima ◂${C.x} ${known ? C.x : C.y}${reply}${C.x}  ${C.d}(coerenza ${res.coherence.toFixed(1)}${known ? '' : ' < gate → rifiuto onesto'})${C.x}  ${good ? C.g + '✓' : C.r + '✗'}${C.x}\n`);
  return good;
};
let bad = 0; const T = (g) => { if (!g) bad++; };

console.log(`\n${C.b}=== ANIMA — ragionamento iperdimensionale OFFLINE ===${C.x} ${C.d}(zero rete, zero modello, solo XOR/popcount)${C.x}\n`);

console.log(`${C.b}1) Recall relazionale per UNBINDING${C.x} ${C.d}— la risposta è SBINDATA dal ricordo, non cercata${C.x}`);
T(say('qual è la capitale del Giappone?', m.ask('giappone', 'capitale'), 'tokyo'));
T(say('in che campo lavorava Einstein?', m.ask('einstein', 'campo'), 'fisica'));

console.log(`${C.b}2) ANALOGIA per algebra binaria${C.x} ${C.d}— "X sta ad A come ? sta a B", pura algebra${C.x}`);
T(say('Tokyo sta al Giappone come ? sta alla Francia', m.analogy('giappone', 'francia', 'tokyo'), 'parigi'));
T(say('lo yen sta al Giappone come ? agli Stati Uniti', m.analogy('giappone', 'stati uniti', 'yen'), 'dollaro'));

console.log(`${C.b}3) MULTI-HOP${C.x} ${C.d}— compone due salti: capitale⁻¹ poi continente${C.x}`);
{ // "in che continente è il paese la cui capitale è Il Cairo?"
  // hop1 (reverse lookup): the subject whose 'capitale' unbinds to 'il cairo'. hop2: its 'continente'.
  let bestS = null, bestC = -1;
  for (const s of Object.keys(KB)) { const r = m.ask(s, 'capitale'); if (r.key === 'il cairo' && r.coherence > bestC) { bestC = r.coherence; bestS = s; } }
  const hop2 = bestS ? m.ask(bestS, 'continente') : { coherence: 0, key: null };
  T(say('in che continente è il paese la cui capitale è Il Cairo?', hop2, 'africa'));
}

console.log(`${C.b}4) ONESTÀ INTRINSECA${C.x} ${C.d}— il calcolo non converge → rifiuta, non inventa${C.x}`);
T(say('qual è la valuta di Einstein?', m.ask('einstein', 'valuta'), null));
T(say('chi ha scritto il Giappone?', m.ask('giappone', 'opera'), null));

console.log(`${C.b}5) APPRENDIMENTO ONE-SHOT${C.x} ${C.d}— imparare = UN bundle XOR, istantaneo, poi subito richiamabile${C.x}`);
console.log(`${C.d}   (insegno a runtime: Italia → capitale → Roma)${C.x}`);
m.learn('italia', 'capitale', 'roma');
T(say('qual è la capitale dell\'Italia?', m.ask('italia', 'capitale'), 'roma'));

console.log(`${C.b}6) "PENSARE" PER RISONANZA${C.x} ${C.d}— fattorizza un prodotto legato iterando fino al punto fisso${C.x}`);
{
  const subj = new Codebook(); for (const s of Object.keys(KB)) subj.addText(s, s);
  const vals = m.values;                                   // reuse the value codebook
  const P = bind(semanticHV('giappone'), semanticHV('tokyo'));   // an unknown bound pair to factor
  const res = resonate(P, subj, vals);
  const good = res.a === 'giappone' && res.b === 'tokyo';
  console.log(`${C.c}risolvi ▸${C.x} fattorizzo un legame sconosciuto in (soggetto ⊗ valore)`);
  console.log(`${C.b}anima ◂${C.x} ${C.c}${res.a} ⊗ ${res.b}${C.x}  ${C.d}(${res.iters} iterazioni → ${res.converged ? 'punto fisso' : 'instabile'}, coerenza ${res.coherence.toFixed(1)})${C.x}  ${good ? C.g + '✓' : C.r + '✗'}${C.x}\n`);
  T(good);
}

console.log(`${C.b}=== ${bad ? C.r + bad + ' errori' : C.g + 'ANIMA ha ragionato su tutto, offline'} ${C.x}`);
process.exit(bad);

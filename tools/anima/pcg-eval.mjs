// MEASURE the leap from COMPOSE to GENERATE-WITH-PROOF. NSPCG discovers a derivation chain by itself
// (kge.reach), verifies every hop against the symbol store, and VERBALIZES it into a sentence that exists
// nowhere in the corpus — each one carrying a machine-checkable proof tree. We assert: (1) it generates
// NOVEL grounded sentences a retrieval/deduction engine cannot, (2) every proof independently RE-VERIFIES,
// (3) a TAMPERED proof is caught (the check has teeth), (4) chains are discovered AUTONOMOUSLY, (5) it
// REFUSES when no grounded chain exists (zero hallucination by construction), (6) it works bilingually.
//   node tools/anima/pcg-eval.mjs
import { KG } from './kge.mjs';
import { ProofGen, verifyProof, parseQuery } from './pcg.mjs';

const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', b: '\x1b[1m', c: '\x1b[36m', x: '\x1b[0m' };
const deslug = (s) => String(s).replace(/[_-]+/g, ' ');

// ---- the knowledge ANIMA was TOLD (stored single-hop edges only) ----
const STORED = [
  ['parigi', 'capitale_di', 'francia'], ['lione', 'si_trova_in', 'francia'],
  ['roma', 'capitale_di', 'italia'], ['tokyo', 'capitale_di', 'giappone'],
  ['francia', 'si_trova_in', 'europa'], ['italia', 'si_trova_in', 'europa'],
  ['giappone', 'si_trova_in', 'asia'], ['europa', 'si_trova_in', 'emisfero_nord'],
  // a cross-domain birth fact — so a generated answer can span biography + geography
  ['dante', 'nato_in', 'firenze'], ['firenze', 'si_trova_in', 'italia'],
];
const kg = new KG(); for (const [h, r, t] of STORED) kg.add(h, r, t); kg.build();
const storedSet = new Set(STORED.map(([h, r, t]) => `${h}|${r}|${t}`));

const gen = new ProofGen(kg, { lang: 'it' });
const genEn = new ProofGen(kg, { lang: 'en' });

console.log(`\n${C.b}=== NSPCG — GENERAZIONE proof-carrying: frasi nuove, grounded, con prova controllabile (offline) ===${C.x}\n`);

// ---------- 1) GENERATION: novel grounded sentences a retrieval/deduction engine cannot produce ----------
console.log(`${C.b}1) GENERA${C.x} ${C.d}(catena scoperta da sola → frase mai esistita nel corpus, con prova)${C.x}`);
const GEN = [
  ['tokyo', 'asia', 'asia'],
  ['parigi', 'europa', 'europa'],
  ['roma', 'emisfero_nord', 'emisfero nord'],
  ['dante', 'europa', 'europa'],            // biography ∘ geography — "nato in un luogo che si trova in Europa"
];
let genOk = 0; const proofs = [];
for (const [h, t, mustContain] of GEN) {
  const a = gen.derive(h, t);
  const novel = a && !storedSet.has(`${a.proof.claim.h}|${a.proof.claim.rel}|${a.proof.claim.t}`);
  const right = a && a.reply.toLowerCase().includes(mustContain);
  const multi = a && a.proof.derivation.length >= 2;
  const ok = a && novel && right && multi;
  if (ok) { genOk++; proofs.push(a.proof); }
  console.log(`   ${C.c}${(h + ' → ' + t).padEnd(22)}${C.x} ${a ? C.b + a.reply : C.y + '—'}${C.x}`);
  console.log(`   ${' '.repeat(22)} ${C.d}[hops ${a ? a.proof.derivation.length : 0} · conf ${a ? a.confidence : 0} · ${a ? a.provenance.length : 0} fonti · ${novel ? 'NUOVA' : 'già nota'}]${C.x} ${ok ? C.g + '✓' : C.r + '✗'}${C.x}`);
}

// ---------- 2) PROOF-CARRYING: every proof independently re-verifies against the symbol store ----------
console.log(`\n${C.b}2) PROVA CONTROLLABILE${C.x} ${C.d}(un verificatore indipendente ri-deriva la tesi senza fidarsi del generatore)${C.x}`);
let proofAll = proofs.length === GEN.length;
for (const p of proofs) {
  const v = verifyProof(kg, p);
  if (!v.ok) proofAll = false;
  console.log(`   ${(deslug(p.claim.h) + ' ⊢ ' + deslug(p.claim.t)).padEnd(28)} ${v.ok ? C.g + 'prova valida' : C.r + 'INVALIDA: ' + v.failures.join('; ')}${C.x} ${C.d}(${p.rule})${C.x}`);
}

// ---------- 3) TAMPER: a forged proof must be CAUGHT (the guarantee is real, not decorative) ----------
console.log(`\n${C.b}3) ANTI-FALSIFICAZIONE${C.x} ${C.d}(altero un passo della prova → il verificatore deve smascherarla)${C.x}`);
const forged = JSON.parse(JSON.stringify(proofs[0]));
const before = forged.derivation[forged.derivation.length - 1].t;
forged.derivation[forged.derivation.length - 1].t = 'europa';   // swap the tail to a false entity
const vf = verifyProof(kg, forged);
const tamperCaught = vf.ok === false;
console.log(`   prova manomessa (${before} → europa): ${tamperCaught ? C.g + 'smascherata' : C.r + 'NON rilevata'}${C.x} ${C.d}(${vf.failures.slice(0, 2).join('; ')})${C.x}`);

// ---------- 4) AUTONOMOUS: the chain was DISCOVERED, never supplied ----------
console.log(`\n${C.b}4) RAGIONAMENTO AUTONOMO${C.x} ${C.d}(nessuna catena fornita: il device la trova da solo)${C.x}`);
const dante = gen.derive('dante', 'europa');
const autoOk = !!dante && dante.proof.derivation.length >= 3;
if (dante) console.log(`   dante —[${dante.proof.derivation.map((s) => s.rel).join(' → ')}]→ europa  ${autoOk ? C.g + 'catena a ' + dante.proof.derivation.length + ' salti scoperta' : C.r + 'troppo corta'}${C.x}`);

// ---------- 5) ZERO-HALLUCINATION: refuse when no grounded chain exists ----------
console.log(`\n${C.b}5) ONESTÀ${C.x} ${C.d}(nessuna catena verificata → rifiuta, non inventa un ponte)${C.x}`);
const REFUSE = [
  ['bridge tokyo↔europa (scollegati)', () => gen.bridge('tokyo', 'europa')],
  ['perché tokyo è in europa (falso)', () => gen.explain('perché tokyo è in europa')],
  ['bridge dante↔asia (irraggiungibile)', () => gen.derive('dante', 'asia')],
  ['NL spazzatura ("che ore sono")', () => gen.explain('che ore sono')],
];
let refuseOk = 0;
for (const [why, fn] of REFUSE) {
  const out = fn();
  const refused = out === null;
  if (refused) refuseOk++;
  console.log(`   ${why.padEnd(36)} ${refused ? C.g + 'rifiuta (null)' : C.r + 'ha inventato: ' + out.reply}${C.x}`);
}

// ---------- 6) BILINGUAL ----------
console.log(`\n${C.b}6) BILINGUE${C.x}`);
const en = genEn.derive('tokyo', 'asia');
const enOk = !!en && /located in asia/i.test(en.reply) && verifyProof(kg, en.proof).ok;
console.log(`   ${en ? C.b + en.reply : C.y + '—'}${C.x} ${enOk ? C.g + '✓' : C.r + '✗'}${C.x}`);

// also prove the NL front-end routes (not just the engine API)
const nl = gen.explain('come è collegato parigi a emisfero_nord');
const nlOk = !!nl && verifyProof(kg, nl.proof).ok;
console.log(`   ${C.d}NL:${C.x} "come è collegato parigi a emisfero_nord" → ${nl ? C.b + nl.reply : C.y + '—'}${C.x} ${nlOk ? C.g + '✓' : C.r + '✗'}${C.x}`);

// ---- verdict ----
console.log(`\n${C.b}=== MIGLIORAMENTO ===${C.x}`);
console.log(`  frasi generate (nuove+grounded):  baseline ${C.r}0/${GEN.length}${C.x}  →  NSPCG ${C.g}${genOk}/${GEN.length}${C.x}  ${C.d}(verbalizza una deduzione mai memorizzata)${C.x}`);
console.log(`  prove ri-verificabili:            ${proofAll ? C.g + 'tutte valide' : C.r + 'alcune invalide'}${C.x}`);
console.log(`  falsificazione smascherata:       ${tamperCaught ? C.g + 'sì' : C.r + 'no'}${C.x}`);
console.log(`  ragionamento autonomo:            ${autoOk ? C.g + 'sì (catena scoperta da sola)' : C.r + 'no'}${C.x}`);
console.log(`  onestà (rifiuti corretti):        ${refuseOk === REFUSE.length ? C.g : C.r}${refuseOk}/${REFUSE.length}${C.x}`);
console.log(`  bilingue + front-end NL:          ${enOk && nlOk ? C.g + 'sì' : C.r + 'no'}${C.x}`);
const fail = genOk < GEN.length || !proofAll || !tamperCaught || !autoOk || refuseOk < REFUSE.length || !enOk || !nlOk;
console.log(`${C.b}=== ${fail ? C.r + 'DA TARARE' : C.g + 'ANIMA ora GENERA: frasi nuove e grounded, con prova controllabile, a errore zero, restando onesta'}${C.x}\n`);
process.exit(fail ? 1 : 0);

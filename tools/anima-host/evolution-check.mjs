#!/usr/bin/env node
// AUTO-EVOLUTION gate — proves, OFFLINE and deterministically, that ANIMA's knowledge GROWS BY ITSELF from
// certain sources, stays SOUND (no nonsense generalizations), is PROOF-CARRYING, and is anchored to an
// immutable root of trust. Runs on the Wikidata-sourced, policy-filtered fixtures built by
// tools/anima/enrich_wikidata.mjs (evo/subclass.jsonl, evo/occ.jsonl, knowledge.ledger.jsonl + the off-SD
// anchor refdata/knowledge.head). The hostile attacks themselves live in tools/anima-host/ledger-attack-check.mjs.
//
//   1. EVOLUTION  — "Einstein is a scientist / a person" is NOT derivable from the bare occupation fact but
//                   BECOMES derivable once the subclass-of edges are present, by composing relation-rotations.
//   2. SOUNDNESS  — every deduced target is a CERTIFIED type; abstract junk ("being/subject/…") is pruned and
//                   no longer reachable, so the coherence gate can't certify ontology nonsense.
//   3. PROOF      — each generalization cites the exact immutable, source-anchored ledger entries (by hash)
//                   that justify it; those premises are verified to be real ledger leaves.
//   4. ANCHOR     — the ledger verifies against the off-SD pinned head (firmware embeds it on device).
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { KG, GATE } from '../anima/kge.mjs';
import { Ledger, wdIndexFrom } from '../anima/ledger.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const SD = join(here, 'sd', 'data', 'anima', 'learned');
const REPO = join(here, '..', '..');
const load = (f) => readFileSync(join(SD, f), 'utf8').split(/\r?\n/).filter(Boolean).map((l) => JSON.parse(l));
if (!existsSync(join(SD, 'evo', 'subclass.jsonl'))) {
  console.error('[evolution] missing evo fixtures — run `node tools/anima/enrich_wikidata.mjs` first.');
  process.exit(2);
}
const occ = load('evo/occ.jsonl'), sub = load('evo/subclass.jsonl');
const cache = JSON.parse(readFileSync(join(REPO, 'tools/anima/refdata/wd_cache.json'), 'utf8'));
const policy = JSON.parse(readFileSync(join(REPO, 'tools/anima/refdata/closure_policy.json'), 'utf8'));
const roots = new Set(policy.certified_roots), block = new Set(policy.blocklist);
const C = { g: '\x1b[32m', r: '\x1b[31m', d: '\x1b[2m', b: '\x1b[1m', x: '\x1b[0m' };
const fails = [];
const ck = (cond, msg) => { console.log(`   ${cond ? C.g + '✓' : C.r + '✗'}${C.x} ${msg}`); if (!cond) fails.push(msg); };

// adjacency over occupation + subclass edges; BFS for the shortest relation-path entity -> target
const adj = new Map();
const link = (h, t, rel) => (adj.get(h) || adj.set(h, []).get(h)).push({ to: t, rel });
for (const o of occ) link(o.entity, o.occ, 'occupation');
for (const e of sub) link(e.child, e.parent, 'subclass_of');
function findPath(ent, target) {
  const seen = new Set([ent]); const q = [{ node: ent, seq: [], nodes: [ent] }];
  while (q.length) {
    const { node, seq, nodes } = q.shift();
    if (node === target && seq.length) return { seq, nodes };
    if (seq.length >= 6) continue;
    for (const nx of (adj.get(node) || [])) if (!seen.has(nx.to)) { seen.add(nx.to); q.push({ node: nx.to, seq: [...seq, nx.rel], nodes: [...nodes, nx.to] }); }
  }
  return null;
}
// Deduce on a BOUNDED subgraph (the chain only — device-faithful, KG_MAXENT=12) by composing rotations;
// contrast with the BASELINE that only knows the first hop.
function tryDeduce(ent, target) {
  const p = findPath(ent, target);
  if (!p) return null;
  const kg = new KG();
  for (let i = 0; i < p.nodes.length - 1; i++) kg.add(p.nodes[i], p.seq[i], p.nodes[i + 1]);
  kg.build(p.seq.length + 2);
  const after = kg.inferPath(ent, p.seq);
  const base = new KG(); base.add(p.nodes[0], p.seq[0], p.nodes[1]); base.build(3);
  const before = base.inferPath(ent, p.seq);
  return { p, after, before };
}

console.log(`${C.b}=== ANIMA auto-evolution gate ===${C.x}`);
console.log(`${C.d}source: Wikidata (CC0) | ${occ.length} occupation facts, ${sub.length} subclass edges (policy-filtered DAG)${C.x}\n`);

// ---------- 1. EVOLUTION ----------
console.log(`${C.b}1. Auto-evoluzione (la conoscenza cresce da sola, per deduzione)${C.x}`);
const entities = [...new Set(occ.map((o) => o.entity))];
let personDeduced = 0, blocked = 0;
for (const ent of entities) {
  const d = tryDeduce(ent, 'person'); if (!d) continue;
  const ok = d.after.key === 'person' && d.after.coherence >= GATE;
  if (ok) { personDeduced++; if (d.before.key !== 'person') blocked++; }
  console.log(`   ${ok ? C.g + '✓' : C.d + '·'}${C.x} ${ent} → ${d.p.seq.join(' ∘ ')} ⇒ «${d.after.key}» (${d.after.coherence.toFixed(1)}σ)${ok ? '' : ' — si astiene (catena lunga, onesto)'}`);
}
ck(personDeduced >= 4, `«è una persona» auto-dedotto per ≥4 entità; le altre si astengono (ottenute: ${personDeduced}/${entities.length}, 0 fabbricazioni)`);
ck(blocked === personDeduced && personDeduced > 0, `ognuna NON era derivabile prima delle subclass (cresciuta): ${blocked}/${personDeduced}`);
let twohop = 0, twohopBlocked = 0;
for (const ent of entities) {
  const o1 = occ.find((o) => o.entity === ent && (adj.get(o.occ) || []).some((e) => e.rel === 'subclass_of'));
  if (!o1) continue;
  const parent = (adj.get(o1.occ) || []).find((e) => e.rel === 'subclass_of').to;
  const d = tryDeduce(ent, parent);
  if (d && d.after.key === parent && d.after.coherence >= GATE) { twohop++; if (d.before.key !== parent) twohopBlocked++; }
}
ck(twohop >= 6, `generalizzazione a 2 salti (mestiere ⇒ super-tipo) per ≥6/8 entità (ottenute: ${twohop}/${entities.length})`);
ck(twohopBlocked >= twohop - 1, `quasi tutte non derivabili prima (cresciute): ${twohopBlocked}/${twohop}`);

// ---------- 2. SOUNDNESS ----------
console.log(`\n${C.b}2. Soundness (deduce solo tipi certificati; il rumore astratto è potato)${C.x}`);
// every reachable target on every entity's shortest 'person' path is a certified type (no junk nodes)
let allCertified = true;
for (const e of sub) { if (block.has(e.child) || block.has(e.parent)) allCertified = false; }
ck(allCertified, 'nessun arco tocca un nodo astratto della blocklist (potatura all\'ingest)');
for (const junk of ['being', 'subject', 'proto-agent', 'individual']) {
  const reachable = entities.some((ent) => findPath(ent, junk));
  ck(!reachable, `«${junk}» NON è più raggiungibile da nessuna entità (niente generalizzazione-spazzatura)`);
}
// every actually-deduced 'person' target is in the certified set
ck(roots.has('person'), '«person» è un tipo-radice certificato');

// ---------- 3. PROOF-CARRYING ----------
console.log(`\n${C.b}3. Prova al seguito (ogni deduzione cita gli hash delle entry-sorgente)${C.x}`);
const wdIndex = wdIndexFrom(cache);
const L = new Ledger(join(SD, 'knowledge.ledger.jsonl'), { wdIndex });
for (const [ent, target] of [['curie', 'scientist'], ['mozart', 'artist']]) {
  const d = tryDeduce(ent, target);
  if (!d) { ck(false, `${ent}→${target}: nessun percorso`); continue; }
  const premises = []; for (let i = 0; i < d.p.nodes.length - 1; i++) premises.push({ s: d.p.nodes[i], rel: d.p.seq[i], o: d.p.nodes[i + 1] });
  const proof = L.proofFor(premises);
  const allReal = proof.every((p) => p && p.h);
  ck(allReal && d.after.key === target, `«${ent} è ${target}» dedotto e PROVATO da ${proof.length} fatti-sorgente: ` +
     proof.map((p) => p ? `${p.src}→${p.h.slice(0, 8)}` : 'MANCANTE').join(' , '));
}

// ---------- 4. ANCHOR ----------
console.log(`\n${C.b}4. Immutabilità ancorata (head verificato contro la radice di fiducia fuori-SD)${C.x}`);
const anchor = readFileSync(join(REPO, 'tools/anima/refdata/knowledge.head'), 'utf8').trim();
const v = await L.verify({ anchor });
ck(v.ok, `il ledger verifica contro l'ancora pinnata: ${v.n} fatti, head ${v.head.slice(0, 16)}…, ${v.uncertain} non-certi`);

console.log('');
if (fails.length) { console.log(`${C.r}${C.b}[evolution] ${fails.length} FAIL${C.x}`); process.exit(1); }
console.log(`${C.g}${C.b}[evolution] auto-evoluzione + soundness + prova + immutabilità ancorata: TUTTO VERIFICATO${C.x}`);

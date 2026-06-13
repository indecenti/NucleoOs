#!/usr/bin/env node
// ANIMA online enrichment from WIKIDATA (CC0) — the "certain, immutable" source that lets the taxonomy
// GROW BY ITSELF. For each entity it pulls, from the live Wikidata Query Service:
//   * its occupation(s)  (P106)
//   * the SUBCLASS-OF chain of each occupation (P279, transitive) — physicist ⊂ scientist ⊂ … ⊂ person
// and emits, with full PROVENANCE (wd:Qx#P279:Qy):
//   * tools/anima/refdata/wd_cache.json     the raw fetched facts (immutable cache; the gate runs OFFLINE off this)
//   * <fixtures>/evo/subclass.jsonl         the type-hierarchy EDGES (the KGE composes these into the closure)
//   * <fixtures>/evo/occ.jsonl              entity→occupation facts
//   * <fixtures>/knowledge.ledger.jsonl     every fact, hash-chained & source-anchored (tools/anima/ledger.mjs)
//
// Only Wikidata (CC0) is used, so what ships on the device is license-clean and the provenance is a fixed
// historical reference. Good-citizen: descriptive User-Agent, serial requests, depth/'edge caps so we never
// hammer the endpoint. Run ONLINE:  node tools/anima/enrich_wikidata.mjs
import { writeFileSync, mkdirSync, existsSync, readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { Ledger, wdIndexFrom, VAL_MAX } from './ledger.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const ROOT = join(here, '..', '..');
const FIXTURES = ['deploy/sd/data/anima/learned', 'tools/sd-sim/data/anima/learned', 'tools/anima-host/sd/data/anima/learned']
  .map((d) => join(ROOT, d));
const CACHE = join(here, 'refdata', 'wd_cache.json');
const UA = 'NucleoOS-ANIMA-Enrichment/1.0 (https://github.com/nucleoos; offline edge AI; contact dev)';
const WDQS = 'https://query.wikidata.org/sparql';
const DEPTH = 5, EDGES_PER_OCC = 16, MAX_OCC = 3;

// A small, famous, unambiguous sample (QID hardcoded to avoid title-resolution flakiness). Expandable.
const SAMPLE = [
  ['einstein', 'Q937', 'Albert Einstein'], ['newton', 'Q935', 'Isaac Newton'],
  ['curie', 'Q7186', 'Marie Curie'], ['mozart', 'Q254', 'Wolfgang Amadeus Mozart'],
  ['picasso', 'Q5593', 'Pablo Picasso'], ['shakespeare', 'Q692', 'William Shakespeare'],
  ['cleopatra', 'Q635', 'Cleopatra'], ['napoleon', 'Q517', 'Napoleon'],
];

const slug = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const qid = (uri) => uri.replace(/.*\/(Q\d+)$/, '$1');

async function sparql(query) {
  const u = WDQS + '?format=json&query=' + encodeURIComponent(query);
  for (let attempt = 0; attempt < 3; attempt++) {
    try {
      const r = await fetch(u, { headers: { 'User-Agent': UA, Accept: 'application/sparql-results+json' } });
      if (r.status === 429) { await sleep(2000 * (attempt + 1)); continue; }
      if (!r.ok) throw new Error('HTTP ' + r.status);
      return (await r.json()).results.bindings;
    } catch (e) { if (attempt === 2) throw e; await sleep(1500); }
  }
}

// structure: occupations of QID + every (node -> direct parent) edge in their P279 closure
const Q_STRUCT = (q) => `SELECT ?occ ?node ?parent WHERE {
  wd:${q} wdt:P106 ?occ . ?occ wdt:P279* ?node . ?node wdt:P279 ?parent . } LIMIT 400`;
// labels (it+en) for a batch of QIDs
const Q_LABELS = (qs) => `SELECT ?x ?it ?en WHERE { VALUES ?x { ${qs.map((q) => 'wd:' + q).join(' ')} }
  OPTIONAL { ?x rdfs:label ?it FILTER(lang(?it)="it") } OPTIONAL { ?x rdfs:label ?en FILTER(lang(?en)="en") } }`;

async function labelsFor(qids) {
  const out = {};
  for (let i = 0; i < qids.length; i += 50) {
    const rows = await sparql(Q_LABELS(qids.slice(i, i + 50)));
    for (const r of rows) out[qid(r.x.value)] = { it: r.it?.value || '', en: r.en?.value || '' };
    await sleep(400);
  }
  return out;
}

async function fetchEntity(q) {
  const rows = await sparql(Q_STRUCT(q));
  const occ = [...new Set(rows.map((r) => qid(r.occ.value)))].slice(0, MAX_OCC);
  // edge set node->parent, then bound the upward DAG by BFS from each occupation
  const adj = new Map();
  for (const r of rows) { const c = qid(r.node.value), p = qid(r.parent.value); (adj.get(c) || adj.set(c, []).get(c)).push(p); }
  // bounded upward DAG PER occupation (its own edge budget), so every occupation's chain is captured
  // (physicist⊂scientist isn't starved by a sibling's big subtree like professor⊂academic⊂…).
  const edges = new Map();                                   // "child|parent" -> [child,parent]
  for (const o of occ) {
    let frontier = [o]; const keep = new Set([o]); let budget = EDGES_PER_OCC;
    for (let d = 0; d < DEPTH && budget > 0; d++) {
      const next = [];
      for (const c of frontier) for (const p of (adj.get(c) || [])) {
        if (budget > 0 && !edges.has(c + '|' + p)) { edges.set(c + '|' + p, [c, p]); budget--; }
        if (!keep.has(p)) { keep.add(p); next.push(p); }
      }
      frontier = next; if (!frontier.length) break;
    }
  }
  return { occ, edges: [...edges.values()] };
}

const POLICY = join(here, 'refdata', 'closure_policy.json');
const ANCHOR = join(here, 'refdata', 'knowledge.head');   // the OFF-SD root of trust (firmware embeds this)

// ONLINE: fetch each entity's occupations + P279 closure into the immutable cache (CC0). Network only.
async function fetchToCache() {
  const cache = { _doc: 'Immutable Wikidata (CC0) enrichment cache. Provenance for ANIMA self-evolution.', entities: {} };
  const allQids = new Set();
  for (const [, q] of SAMPLE) allQids.add(q);
  console.log('[enrich] fetching structure for', SAMPLE.length, 'entities ...');
  const struct = {};
  for (const [name, q] of SAMPLE) {
    struct[q] = await fetchEntity(q);
    struct[q].occ.forEach((x) => allQids.add(x));
    struct[q].edges.forEach(([c, p]) => { allQids.add(c); allQids.add(p); });
    console.log(`  ${name} (${q}): ${struct[q].occ.length} occ, ${struct[q].edges.length} subclass edges`);
    await sleep(500);
  }
  console.log('[enrich] resolving', allQids.size, 'labels (it+en) ...');
  const lab = await labelsFor([...allQids]);
  const L = (q) => lab[q] || { it: q, en: q };
  for (const [, q] of SAMPLE) {
    const s = struct[q]; const ent = { qid: q, label_it: L(q).it, label_en: L(q).en, occupations: [], edges: [] };
    for (const o of s.occ) ent.occupations.push({ qid: o, it: L(o).it, en: L(o).en });
    for (const [c, p] of s.edges) ent.edges.push({ child: c, child_it: L(c).it, child_en: L(c).en, parent: p, parent_it: L(p).it, parent_en: L(p).en });
    cache.entities[q] = ent;
  }
  if (!existsSync(dirname(CACHE))) mkdirSync(dirname(CACHE), { recursive: true });
  writeFileSync(CACHE, JSON.stringify(cache, null, 1), 'utf8');
  console.log('[enrich] cache ->', CACHE);
}

const loadPolicy = () => { const p = JSON.parse(readFileSync(POLICY, 'utf8')); return { roots: new Set(p.certified_roots), block: new Set(p.blocklist) }; };

// Keep the subclass graph a DAG: drop self-loops and any edge child->parent where parent already reaches
// child (would close a cycle). Wikidata P279 has cycles (e.g. creator⊂creator) — the audit showed they let
// the coherence gate certify nonsense, so we refuse them at ingest.
function acyclic(edges) {
  const adj = new Map(); const kept = [];
  const reaches = (a, b) => { const seen = new Set(); const st = [a]; while (st.length) { const x = st.pop(); if (x === b) return true; if (seen.has(x)) continue; seen.add(x); for (const y of (adj.get(x) || [])) st.push(y); } return false; };
  for (const e of edges) {
    if (e.child === e.parent) continue;
    if (reaches(e.parent, e.child)) continue;
    (adj.get(e.child) || adj.set(e.child, []).get(e.child)).push(e.parent); kept.push(e);
  }
  return kept;
}

// OFFLINE: build the evo fixtures + the authenticated ledger + the off-SD anchor from the immutable cache,
// applying the SOUNDNESS POLICY (drop edges into abstract upper-ontology nodes, cap field lengths, break
// cycles). Deterministic -> the gate runs against this without a network.
function buildFromCache() {
  const cache = JSON.parse(readFileSync(CACHE, 'utf8'));
  const { block } = loadPolicy();
  const okSlug = (s) => s && s.length <= VAL_MAX && !block.has(s);            // prune abstract/over-long nodes
  const occRows = []; const rawEdges = []; const seenOcc = new Set(), seenEdge = new Set();
  for (const [name, q] of SAMPLE) {
    const ent = cache.entities[q]; if (!ent) continue;
    for (const o of ent.occupations || []) {
      const os = slug(o.en); if (!okSlug(os)) continue;
      const k = name + '|' + os; if (seenOcc.has(k)) continue; seenOcc.add(k);
      occRows.push({ entity: name, entity_label: ent.label_en, occ: os, occ_it: o.it || o.en, occ_en: o.en, src: `wd:${q}#P106:${o.qid}` });
    }
    for (const e of ent.edges || []) {
      const cs = slug(e.child_en), ps = slug(e.parent_en);
      if (!okSlug(cs) || !okSlug(ps)) continue;                              // drop edges touching abstract nodes
      const k = cs + '|' + ps; if (seenEdge.has(k)) continue; seenEdge.add(k);
      rawEdges.push({ child: cs, parent: ps, child_it: e.child_it || e.child_en, child_en: e.child_en,
        parent_it: e.parent_it || e.parent_en, parent_en: e.parent_en, src: `wd:${e.child}#P279:${e.parent}` });
    }
  }
  const subclass = acyclic(rawEdges);                                         // enforce a DAG (no cycles/self-loops)

  const wdIndex = wdIndexFrom(cache);                                         // bind ledger provenance to the cache
  let head = '';
  for (const dir of FIXTURES) {
    mkdirSync(join(dir, 'evo'), { recursive: true });
    writeFileSync(join(dir, 'evo', 'subclass.jsonl'), subclass.map((x) => JSON.stringify(x)).join('\n') + '\n', 'utf8');
    writeFileSync(join(dir, 'evo', 'occ.jsonl'), occRows.map((x) => JSON.stringify(x)).join('\n') + '\n', 'utf8');
    const lp = join(dir, 'knowledge.ledger.jsonl'); writeFileSync(lp, '', 'utf8');
    const led = new Ledger(lp, { wdIndex });
    led.appendMany(occRows.map((o) => ({ s: o.entity, rel: 'occupation', o: o.occ, src: o.src, conf: 100 })));
    led.appendMany(subclass.map((e) => ({ s: e.child, rel: 'subclass_of', o: e.parent, src: e.src, conf: 100 })));
    head = led.head();
  }
  writeFileSync(ANCHOR, head + '\n', 'utf8');                                 // the off-SD pin firmware embeds
  console.log(`[enrich] built ${occRows.length} occupation + ${subclass.length} subclass facts (${rawEdges.length - subclass.length} cyclic dropped)`);
  console.log(`[enrich] ledger head=${head}  -> anchored in ${ANCHOR}`);
  return head;
}

async function main() {
  if (!process.argv.includes('--from-cache')) await fetchToCache();           // refresh the immutable cache (online)
  buildFromCache();                                                            // always (re)build offline + deterministic
}
main().catch((e) => { console.error('[enrich] FAILED:', e.message); process.exit(1); });

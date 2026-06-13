#!/usr/bin/env node
// ANIMA contradiction gate — the certainty backbone the flywheel was missing (docs/anima-knowledge-scale.md:
// "niente contradiction-detection, certezza binaria non calibrata"). Over the accumulated (subject, rel,
// value) triple stores — the OUTPUT of extract_triples.py AND of the online learned cache that grows from
// Wikipedia/Wikidata at runtime — it asserts the knowledge does not CONTRADICT ITSELF on a functional fact.
//
// CERTAIN BY CONSTRUCTION (zero false positives, by design):
//   * Only FUNCTIONAL (single-valued) relations are checked — a subject has exactly ONE of these:
//       born  / died  -> the YEAR (4 digits) is compared, so "14 marzo 1879" == "1879" (no format noise),
//                         and years are language-independent (compared across it+en).
//       gender         -> normalized to M/F via a synonym/cross-language map (uomo/maschio/man -> M, etc.),
//                         so it+en are comparable and synonyms never look like a conflict.
//       capital        -> a city is the capital of ONE country; compared PER-LANGUAGE only, because
//                         "Spagna" (it) and "Spain" (en) are the SAME fact, not a contradiction (the trap
//                         that 40 cross-lingual false positives taught us — see the prototype).
//   * MULTI-VALUED relations are DELIBERATELY EXCLUDED — they are legitimately many, not conflicts:
//       occupation (Einstein is physicist AND scientist), isa, located_in (Rome in Italy AND in Europe),
//       subclass_of (taxonomy DAG). And `country` is excluded too: dual nationality (Einstein = Germany /
//       Switzerland / USA) is real, so two countries for a person is NOT a contradiction.
// So a flag here is a HARD factual conflict (e.g. the live find: "il-cairo capital Sudan" alongside
// "il-cairo capital Egitto" — a world.json typo the extractor's own self-checks could not see). The gate
// only ever makes ANIMA MORE cautious; it can never introduce an answer, so it cannot add a hallucination.
//
// Usage:  node tools/anima-host/contradiction-check.mjs [learnedDir]
//   default learnedDir = tools/anima-host/sd/data/anima/learned (the host fixture).
//   Point it at a device pull (the cards fetched live) to CERTIFY the real on-device knowledge:
//     node tools/anima-host/contradiction-check.mjs path/to/pulled/learned
// Exit 0 = clean (or no store present -> SKIP); 1 = at least one functional contradiction.
import { readFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const C = { g: '\x1b[32m', r: '\x1b[31m', y: '\x1b[33m', d: '\x1b[2m', x: '\x1b[0m' };
const dir = process.argv[2] || join(here, 'sd', 'data', 'anima', 'learned');

// --- normalizers ----------------------------------------------------------
const stripAccents = (s) => s.normalize('NFD').replace(/[̀-ͯ]/g, '');
const norm = (s) => stripAccents(String(s).toLowerCase()).replace(/[^a-z0-9]+/g, ' ').trim();
const year = (v) => { const m = String(v).match(/\b(1[0-9]{3}|20[0-2][0-9])\b/); return m ? m[1] : null; };
const GENDER = {
  uomo: 'M', uomini: 'M', maschio: 'M', maschile: 'M', male: 'M', man: 'M', men: 'M',
  donna: 'F', donne: 'F', femmina: 'F', femminile: 'F', female: 'F', woman: 'F', women: 'F',
};
// rel -> { fn: value-normalizer (null = drop, can't compare), perLang: compare only within one language }
const FUNCTIONAL = {
  born:    { fn: year,                          perLang: false },
  died:    { fn: year,                          perLang: false },
  gender:  { fn: (v) => GENDER[norm(v)] ?? null, perLang: false },
  capital: { fn: (v) => norm(v) || null,         perLang: true  },
};

// --- load the triple stores -----------------------------------------------
const SOURCES = [
  ['mind.it.jsonl', 'it'], ['mind.en.jsonl', 'en'],
  ['facets.it.jsonl', 'it'], ['facets.en.jsonl', 'en'],
];
if (!existsSync(dir) || !SOURCES.some(([f]) => existsSync(join(dir, f)))) {
  console.log(`${C.y}[contradiction] SKIP — no learned triple store at ${dir}${C.x}`);
  process.exit(0);
}

const triples = [];
for (const [file, lang] of SOURCES) {
  const p = join(dir, file);
  if (!existsSync(p)) continue;
  for (const ln of readFileSync(p, 'utf8').split(/\r?\n/)) {
    if (!ln.trim()) continue;
    try { const o = JSON.parse(ln); if (o && o.subject && o.rel) triples.push({ ...o, _lang: lang, _file: file }); } catch { /* skip bad line */ }
  }
}

// --- group by (key) -> map(normValue -> [occurrences]) ---------------------
const groups = new Map();
for (const t of triples) {
  const spec = FUNCTIONAL[t.rel];
  if (!spec) continue;                       // not a functional relation -> never a contradiction
  const nv = spec.fn(t.value);
  if (nv == null) continue;                  // unparseable value -> can't compare, skip (never a false flag)
  const key = `${t.rel}|${t.subject}${spec.perLang ? `|${t._lang}` : ''}`;
  if (!groups.has(key)) groups.set(key, new Map());
  const g = groups.get(key);
  if (!g.has(nv)) g.set(nv, []);
  g.get(nv).push({ raw: t.value, file: t._file, label: t.label });
}

// --- report ----------------------------------------------------------------
const conflicts = [];
for (const [key, vals] of groups) if (vals.size > 1) conflicts.push([key, vals]);
conflicts.sort((a, b) => a[0].localeCompare(b[0]));

for (const [key, vals] of conflicts) {
  console.log(`${C.r}CONTRADICTION${C.x} ${key}`);
  for (const [nv, occ] of vals)
    console.log(`   ${C.d}[${nv}]${C.x} <- ${occ.map((o) => `${o.label ? o.label + ': ' : ''}${o.raw} (${o.file})`).join(', ')}`);
}

const checked = [...groups.keys()].length;
const summary = `[contradiction] ${triples.length} triples, ${checked} functional groups (${Object.keys(FUNCTIONAL).join('/')}), ${conflicts.length} contradiction(s)`;
if (conflicts.length) {
  console.log(`\n${C.r}${summary}${C.x}`);
  process.exit(1);
}
console.log(`${C.g}${summary} — clean${C.x}`);
process.exit(0);

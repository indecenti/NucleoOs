#!/usr/bin/env node
// ANIMA importer #1 — NIST CODATA 2022 fundamental physical constants → grounded bilingual cards.
//
// WHY this is the first importer: it is the cleanest possible "certain knowledge" — internationally
// recommended least-squares values, public domain (US-gov work), a single 40 KB ASCII table. It
// proves the import contract end-to-end (download → adapt → categorize → provenance → cards → build
// → gate) on a source that cannot hallucinate.
//
// DESIGN: a CURATED whitelist of the famous, useful constants with hand-quality IT+EN phrasings, but
// every VALUE is pulled from the downloaded CODATA file (never hand-typed) — so the numbers are
// authoritative and a CODATA rename/format-drift fails the build loudly instead of shipping a stale
// number. Units are curated (the SI unit of a fundamental constant is stable); only the value comes
// from the file. Output: tools/anima/knowledge/sci-constants.jsonl (category "science"), then
// `npm run anima:packs` compiles it into both index packs.
//
// Usage:  node tools/anima/import_codata.mjs            (downloads + caches the table if absent)
//         node tools/anima/import_codata.mjs --file <path-to-allascii.txt>
import { readFileSync, writeFileSync, existsSync, mkdirSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const SRC_URL = 'https://physics.nist.gov/cuu/Constants/Table/allascii.txt';
const CACHE = join(here, '.cache', 'codata-2022.txt');
// Writes to knowledge.staged/ (NOT the live knowledge/ dir that build_akb2 globs): landing bulk
// imports into the current FLAT k-means index re-clusters everything and flips borderline existing
// cases (measured: +21 cards regressed 2 skill probes + left "costante di boltzmann" unreachable).
// Bulk import is gated on the sharded AKB5 index (docs/anima-knowledge-scale.md Move 1). To land,
// move the file into knowledge/ and run `npm run anima:packs`.
const OUT = join(here, 'knowledge.staged', 'sci-constants.jsonl');
const PROV = 'codata-2022:nist';   // provenance tag (public domain, US-gov)

const fileArg = process.argv.indexOf('--file');
const srcPath = fileArg !== -1 ? process.argv[fileArg + 1] : CACHE;

async function ensureSource() {
  if (existsSync(srcPath)) return readFileSync(srcPath, 'utf8');
  process.stdout.write(`[codata] downloading ${SRC_URL} ...\n`);
  const res = await fetch(SRC_URL);
  if (!res.ok) throw new Error(`download failed: HTTP ${res.status}`);
  const text = await res.text();
  mkdirSync(dirname(CACHE), { recursive: true });
  writeFileSync(CACHE, text);
  process.stdout.write(`[codata] cached ${text.length} bytes → ${CACHE}\n`);
  return text;
}

// Parse the NIST table → Map(quantity → {value, unc, unit}). The header LABELS are offset from their
// data columns (Value's label sits ~5 chars right of where values start), so fixed-width slicing
// corrupts rows. Instead split on runs of 2+ spaces: quantity, value, uncertainty and unit each use
// only SINGLE internal spaces (digit groups, "J mol^-1 K^-1"), so the column gaps (≥2 spaces) are the
// only safe delimiters. A dimensionless constant (fine-structure) simply has no 4th field.
function parse(text) {
  const lines = text.split(/\r?\n/);
  const hi = lines.findIndex(l => /\bQuantity\b/.test(l) && /\bValue\b/.test(l));
  if (hi < 0) throw new Error('CODATA header row not found — format changed');
  const map = new Map();
  for (let i = hi + 2; i < lines.length; i++) {       // +2 skips the dashed separator
    const l = lines[i];
    if (!l.trim() || /^-+$/.test(l.trim())) continue;
    const f = l.split(/\s{2,}/).filter(Boolean);
    if (f.length < 2) continue;
    const [q, value, unc = '', unit = ''] = f;
    if (q && value) map.set(q, { value, unc, unit });
  }
  return map;
}

// "1.054 571 817... e-34" → "1.054571817e-34" ; "299 792 458" → "299792458" ; "6.674 30 e-11" → "6.67430e-11"
const cleanValue = (v) => v.replace(/\.\.\./g, '').replace(/\s+/g, '').trim();

// Curated constants: codata = EXACT quantity name in the file; sym/unit curated; asks hand-authored.
const C = [
  { id: 'sci.const.speed-of-light', codata: 'speed of light in vacuum', sym: 'c', unit: 'm/s', exact: true,
    // NB: keep asks VALUE-specific so they don't shadow the existing conceptual card sci.speed-light
    // ("qual è la velocità della luce" must stay with the concept; "valore di c" comes here).
    it: { name: 'velocità della luce nel vuoto', asks: ['valore di c', 'quanto vale c', 'valore esatto della velocità della luce', 'velocità della luce nel vuoto in m/s'] },
    en: { name: 'speed of light in vacuum', asks: ['value of c', 'value of the speed of light', 'exact speed of light', 'speed of light in m/s'] } },
  { id: 'sci.const.planck', codata: 'Planck constant', sym: 'h', unit: 'J·s', exact: true,
    it: { name: 'costante di Planck', asks: ['costante di Planck', 'quanto vale la costante di Planck', 'valore di h', 'costante di planck'] },
    en: { name: 'Planck constant', asks: ['Planck constant', 'value of the Planck constant', 'value of h', 'planck constant'] } },
  { id: 'sci.const.reduced-planck', codata: 'reduced Planck constant', sym: 'ħ', unit: 'J·s', exact: true,
    it: { name: 'costante di Planck ridotta', asks: ['costante di Planck ridotta', 'h tagliato', 'valore di h-bar', 'h ridotta'] },
    en: { name: 'reduced Planck constant', asks: ['reduced Planck constant', 'h-bar', 'value of h-bar', 'reduced planck constant'] } },
  { id: 'sci.const.elementary-charge', codata: 'elementary charge', sym: 'e', unit: 'C', exact: true,
    it: { name: 'carica elementare', asks: ['carica elementare', 'carica dell\'elettrone', 'valore di e', 'quanto vale la carica elementare'] },
    en: { name: 'elementary charge', asks: ['elementary charge', 'charge of the electron', 'value of e', 'electron charge value'] } },
  { id: 'sci.const.gravitation', codata: 'Newtonian constant of gravitation', sym: 'G', unit: 'm³·kg⁻¹·s⁻²',
    it: { name: 'costante di gravitazione universale', asks: ['costante di gravitazione', 'costante di gravitazione universale', 'valore di G', 'costante di Newton'] },
    en: { name: 'gravitational constant', asks: ['gravitational constant', 'Newtonian constant of gravitation', 'value of big G', 'big G'] } },
  { id: 'sci.const.boltzmann', codata: 'Boltzmann constant', sym: 'k', unit: 'J/K', exact: true,
    it: { name: 'costante di Boltzmann', asks: ['costante di Boltzmann', 'valore di k', 'costante di boltzmann'] },
    en: { name: 'Boltzmann constant', asks: ['Boltzmann constant', 'value of k', 'boltzmann constant'] } },
  { id: 'sci.const.avogadro', codata: 'Avogadro constant', sym: 'N_A', unit: 'mol⁻¹', exact: true,
    it: { name: 'numero di Avogadro', asks: ['numero di Avogadro', 'costante di Avogadro', 'valore di N_A', 'quanto vale il numero di avogadro'] },
    en: { name: 'Avogadro constant', asks: ['Avogadro constant', 'Avogadro number', 'value of N_A', 'avogadro number'] } },
  { id: 'sci.const.gas', codata: 'molar gas constant', sym: 'R', unit: 'J·mol⁻¹·K⁻¹', exact: true,
    it: { name: 'costante dei gas', asks: ['costante dei gas', 'costante universale dei gas', 'valore di R', 'costante dei gas perfetti'] },
    en: { name: 'molar gas constant', asks: ['gas constant', 'molar gas constant', 'value of R', 'universal gas constant'] } },
  { id: 'sci.const.electron-mass', codata: 'electron mass', sym: 'mₑ', unit: 'kg',
    it: { name: 'massa dell\'elettrone', asks: ['massa dell\'elettrone', 'quanto pesa un elettrone', 'massa elettrone'] },
    en: { name: 'electron mass', asks: ['electron mass', 'mass of the electron', 'electron mass value'] } },
  { id: 'sci.const.proton-mass', codata: 'proton mass', sym: 'mₚ', unit: 'kg',
    it: { name: 'massa del protone', asks: ['massa del protone', 'quanto pesa un protone', 'massa protone'] },
    en: { name: 'proton mass', asks: ['proton mass', 'mass of the proton', 'proton mass value'] } },
  { id: 'sci.const.neutron-mass', codata: 'neutron mass', sym: 'mₙ', unit: 'kg',
    it: { name: 'massa del neutrone', asks: ['massa del neutrone', 'quanto pesa un neutrone', 'massa neutrone'] },
    en: { name: 'neutron mass', asks: ['neutron mass', 'mass of the neutron', 'neutron mass value'] } },
  { id: 'sci.const.fine-structure', codata: 'fine-structure constant', sym: 'α', unit: { it: '(adimensionale)', en: '(dimensionless)' },
    it: { name: 'costante di struttura fine', asks: ['costante di struttura fine', 'valore di alfa', 'costante di struttura fina'] },
    en: { name: 'fine-structure constant', asks: ['fine-structure constant', 'value of alpha', 'fine structure constant'] } },
  { id: 'sci.const.vacuum-permittivity', codata: 'vacuum electric permittivity', sym: 'ε₀', unit: 'F/m',
    it: { name: 'permittività elettrica del vuoto', asks: ['permittività del vuoto', 'costante dielettrica del vuoto', 'valore di epsilon zero'] },
    en: { name: 'vacuum electric permittivity', asks: ['vacuum permittivity', 'electric constant', 'value of epsilon zero'] } },
  { id: 'sci.const.vacuum-permeability', codata: 'vacuum mag. permeability', sym: 'μ₀', unit: 'N/A²',
    it: { name: 'permeabilità magnetica del vuoto', asks: ['permeabilità magnetica del vuoto', 'permeabilità del vuoto', 'valore di mu zero'] },
    en: { name: 'vacuum magnetic permeability', asks: ['vacuum permeability', 'magnetic constant', 'value of mu zero'] } },
  { id: 'sci.const.stefan-boltzmann', codata: 'Stefan-Boltzmann constant', sym: 'σ', unit: 'W·m⁻²·K⁻⁴', exact: true,
    it: { name: 'costante di Stefan-Boltzmann', asks: ['costante di Stefan-Boltzmann', 'valore di sigma', 'costante di stefan boltzmann'] },
    en: { name: 'Stefan-Boltzmann constant', asks: ['Stefan-Boltzmann constant', 'value of sigma', 'stefan boltzmann constant'] } },
  { id: 'sci.const.electronvolt', codata: 'electron volt', sym: 'eV', unit: 'J', exact: true,
    it: { name: 'elettronvolt', asks: ['quanti joule è un elettronvolt', 'valore di un elettronvolt', 'quanto vale un eV', 'elettronvolt in joule'] },
    en: { name: 'electron volt', asks: ['how many joules is an electron volt', 'value of an electron volt', 'electronvolt in joules', 'one eV in joules'] } },
  { id: 'sci.const.atomic-mass-unit', codata: 'atomic mass constant', sym: 'u', unit: 'kg',
    it: { name: 'unità di massa atomica', asks: ['unità di massa atomica', 'quanto vale una uma', 'massa atomica unitaria', 'dalton in kg'] },
    en: { name: 'atomic mass constant', asks: ['atomic mass unit', 'value of one amu', 'unified atomic mass unit', 'dalton in kg'] } },
  { id: 'sci.const.faraday', codata: 'Faraday constant', sym: 'F', unit: 'C/mol', exact: true,
    it: { name: 'costante di Faraday', asks: ['costante di Faraday', 'valore di F', 'costante di faraday'] },
    en: { name: 'Faraday constant', asks: ['Faraday constant', 'value of the Faraday constant', 'faraday constant'] } },
  { id: 'sci.const.rydberg', codata: 'Rydberg constant', sym: 'R∞', unit: 'm⁻¹',
    it: { name: 'costante di Rydberg', asks: ['costante di Rydberg', 'valore di R infinito', 'costante di rydberg'] },
    en: { name: 'Rydberg constant', asks: ['Rydberg constant', 'value of the Rydberg constant', 'rydberg constant'] } },
  { id: 'sci.const.bohr-radius', codata: 'Bohr radius', sym: 'a₀', unit: 'm',
    it: { name: 'raggio di Bohr', asks: ['raggio di Bohr', 'valore del raggio di Bohr', 'raggio di bohr'] },
    en: { name: 'Bohr radius', asks: ['Bohr radius', 'value of the Bohr radius', 'bohr radius'] } },
  { id: 'sci.const.g0', codata: 'standard acceleration of gravity', sym: 'g₀', unit: 'm/s²', exact: true,
    it: { name: 'accelerazione di gravità standard', asks: ['accelerazione di gravità', 'quanto vale g', 'accelerazione di gravità standard', 'gravità terrestre'] },
    en: { name: 'standard acceleration of gravity', asks: ['standard gravity', 'value of g', 'acceleration of gravity', 'earth gravity'] } },
];

// Cross-corpus ask-dedup: the corpus forbids the SAME ask phrasing on two cards (it contaminates the
// retriever — a shared phrasing can't disambiguate which card to return). Build the set of asks already
// claimed by EXISTING cards (and by earlier-emitted constants), and drop any colliding ask. This is the
// scalable guard every importer needs as we pour in bulk knowledge — concepts that already exist
// (speed of light, gravity) keep their generic phrasings; the constant card owns only the value-specific
// ones. Normalization matches the device tokenizer (lowercase, accent-fold, alnum-only).
const normAsk = (s) => s.toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '')
  .replace(/[^a-z0-9 ]/g, '').replace(/\s+/g, ' ').trim();
function loadClaimedAsks() {
  const dir = join(here, 'knowledge');
  const claimed = new Set();
  for (const f of (existsSync(dir) ? readdirSync(dir) : [])) {
    if (!f.endsWith('.jsonl') || f === 'sci-constants.jsonl') continue;
    for (const l of readFileSync(join(dir, f), 'utf8').split('\n')) {
      if (!l.trim() || l.trim().startsWith('//')) continue;
      let j; try { j = JSON.parse(l); } catch { continue; }
      for (const lang of ['it', 'en']) for (const a of (j.ask?.[lang] || [])) claimed.add(normAsk(a));
    }
  }
  return claimed;
}

const cards = [];
const missing = [];
let dropped = 0;
const main = async () => {
  const map = parse(await ensureSource());
  const claimed = loadClaimedAsks();   // asks owned by pre-existing cards
  const dedup = (arr) => arr.filter((a) => { const n = normAsk(a); if (claimed.has(n)) { dropped++; return false; } claimed.add(n); return true; });
  for (const k of C) {
    const row = map.get(k.codata);
    if (!row) { missing.push(k.codata); continue; }
    k.it.asks = dedup(k.it.asks);
    k.en.asks = dedup(k.en.asks);
    const val = cleanValue(row.value);
    const unitIt = typeof k.unit === 'object' ? k.unit.it : k.unit;
    const unitEn = typeof k.unit === 'object' ? k.unit.en : k.unit;
    const exactIt = k.exact ? ' (valore esatto)' : '';
    const exactEn = k.exact ? ' (exact value)' : '';
    const replyIt = `${k.it.name} (${k.sym}) = ${val} ${unitIt}${exactIt}.`.replace(/\s+\./, '.');
    const replyEn = `${k.en.name} (${k.sym}) = ${val} ${unitEn}${exactEn}.`.replace(/\s+\./, '.');
    const hasUnc = row.unc && row.unc !== '(exact)';
    const detIt = k.exact ? 'Valore esatto fissato dalla ridefinizione SI 2019 (CODATA 2022).'
                          : `Valore raccomandato CODATA 2022${hasUnc ? `, incertezza ${cleanValue(row.unc)} ${unitIt}`.trimEnd() : ''}.`;
    const detEn = k.exact ? 'Exact value fixed by the 2019 SI redefinition (CODATA 2022).'
                          : `CODATA 2022 recommended value${hasUnc ? `, uncertainty ${cleanValue(row.unc)} ${unitEn}`.trimEnd() : ''}.`;
    cards.push({
      id: k.id, category: 'science', action: 'answer', arg: '',
      reply: { it: replyIt.slice(0, 250), en: replyEn.slice(0, 250) },
      ask: { it: k.it.asks, en: k.en.asks },
      detail: { it: detIt.slice(0, 250), en: detEn.slice(0, 250) },
      lang_primary: 'bi', source: PROV, tags: ['constant', 'physics', 'codata'],
    });
  }
  if (missing.length) {
    console.error(`[codata] ✗ ${missing.length} curated name(s) not found in the table (format drift?):`);
    for (const m of missing) console.error(`         - ${m}`);
    process.exit(1);
  }
  const body = cards.map(c => JSON.stringify(c)).join('\n') + '\n';
  writeFileSync(OUT, body);
  console.log(`[codata] wrote ${cards.length} constant cards → ${OUT}  (${dropped} colliding ask(s) dropped vs existing corpus)`);
  console.log(`[codata] sample: ${cards[0].reply.it}`);
};
main().catch(e => { console.error(`[codata] ERROR: ${e.message}`); process.exit(1); });

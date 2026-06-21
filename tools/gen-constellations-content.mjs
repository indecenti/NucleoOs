// gen-constellations-content.mjs — single source of truth for the web "Costellazioni 3D" game.
//
// Parses the firmware's flash-data header (the ONLY place goods/economies/systems/missions are
// authored) and emits an ES module the web Game Center imports, so prices/systems/missions on the
// web match the Cardputer byte-for-byte. Run on deploy (wired in tools/deploy.ps1) or by hand:
//
//     node tools/gen-constellations-content.mjs
//
// Source : firmware/components/nucleo_app/constellations_content.h
// Output : apps/games/www/games/constellations-content.js   (export const CONTENT = {...})
//
// NOTE: this mirrors the DATA tables only. The pricing FORMULA (unit_buy hash variance, rep
// discount, sell 88%, refuel, jump cost) lives in app_constellations.cpp and is re-implemented in
// apps/games/www/games/constellations-econ.js (logic, not data) — keep the two in sync by hand.

import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const HDR = join(ROOT, 'firmware', 'components', 'nucleo_app', 'constellations_content.h');
const OUT = join(ROOT, 'apps', 'games', 'www', 'games', 'constellations-content.js');

// ---- read + strip comments (block comments also remove the /* AGRI */ row labels) --------------
let src = readFileSync(HDR, 'utf8');
src = src.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

// ---- enum symbol -> index map (handles `= N` resets and sentinel members like NGOODS) ----------
const ENUM = {};
for (const m of src.matchAll(/enum\s*\{([^}]*)\}/g)) {
  let idx = 0;
  for (let tok of m[1].split(',')) {
    tok = tok.trim(); if (!tok) continue;
    const eq = tok.indexOf('=');
    let name = tok;
    if (eq >= 0) { name = tok.slice(0, eq).trim(); idx = parseInt(tok.slice(eq + 1).trim(), 10); }
    ENUM[name] = idx; idx++;
  }
}
const resolve = (t) => {
  t = t.trim();
  if (/^-?\d+$/.test(t)) return parseInt(t, 10);
  if (t in ENUM) return ENUM[t];
  throw new Error(`unknown symbol: "${t}"`);
};

// ---- table helpers -----------------------------------------------------------------------------
function body(declRe) {
  const m = src.match(declRe);
  if (!m) throw new Error(`table not found: ${declRe}`);
  let i = src.indexOf('{', m.index + m[0].length), depth = 0, start = i;
  for (; i < src.length; i++) {
    if (src[i] === '{') depth++;
    else if (src[i] === '}') { if (--depth === 0) return src.slice(start + 1, i); }
  }
  throw new Error(`unbalanced: ${declRe}`);
}
function rows(b) {           // top-level { ... } groups inside a table body
  const out = []; let depth = 0, start = -1;
  for (let i = 0; i < b.length; i++) {
    if (b[i] === '{') { if (depth === 0) start = i + 1; depth++; }
    else if (b[i] === '}') { if (--depth === 0) out.push(b.slice(start, i)); }
  }
  return out;
}
const strings = (row) => [...row.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map((m) => m[1]);
function scalars(row) {      // tokens left after removing nested {string pairs} and quoted strings
  return row.replace(/\{[^{}]*\}/g, '').replace(/"(?:[^"\\]|\\.)*"/g, '')
    .split(',').map((t) => t.trim()).filter((t) => t.length).map(resolve);
}

// ---- parse the six data tables -----------------------------------------------------------------
const goods = rows(body(/static const Good GOODS\s*\[[^\]]*\]\s*=/)).map((r) => {
  const s = strings(r), n = scalars(r);   // n = [base, cat]
  return { it: s[0], en: s[1], base: n[0], cat: n[1] };
});
const econ = rows(body(/ECON_NAME\s*\[[^\]]*\]\s*\[2\]\s*=/)).map((r) => {
  const s = strings(r); return { it: s[0], en: s[1] };
});
const econmod = rows(body(/ECONMOD\s*\[[^\]]*\]\s*\[[^\]]*\]\s*=/)).map((r) => scalars(r));
const factions = rows(body(/FAC_NAME\s*\[[^\]]*\]\s*\[2\]\s*=/)).map((r) => {
  const s = strings(r); return { it: s[0], en: s[1] };
});
// NOTE: systems and missions are now PROCEDURAL (constellations-gen.js / firmware pg_*), so they are
// NOT emitted here — only the FIXED archetypes (goods, economies, factions) live in the header.

// ---- assert the shape the firmware/web both assume ---------------------------------------------
const expect = { NGOODS: goods.length, NECON: econ.length, NFAC: factions.length };
for (const [k, v] of Object.entries(expect))
  if (ENUM[k] !== v) throw new Error(`count mismatch: ${k}=${ENUM[k]} but parsed ${v} rows`);
if (econmod.length !== econ.length || econmod.some((r) => r.length !== goods.length))
  throw new Error('ECONMOD is not NECON x NGOODS');

const CONTENT = {
  generatedFrom: 'constellations_content.h',
  goods, econ, econmod, factions,
  enums: {
    cat: pick(/^CAT_/), econ: pick(/^EC_/), faction: pick(/^F_/), good: pick(/^G_/),
    flag: pick(/^FL_/), missionType: pick(/^MT_/), foe: pick(/^FOE_/),
  },
  counts: { NGOODS: goods.length, NECON: econ.length, NFAC: factions.length, NSYS: 10, NMISS_PER_SYS: 4 },
  // systems[] is injected at runtime by the web game (genSector); kept as an empty default so the
  // economy module never dereferences undefined before a sector is built.
  systems: [],
};
function pick(re) { const o = {}; for (const k of Object.keys(ENUM)) if (re.test(k) && !k.startsWith('N')) o[k] = ENUM[k]; return o; }

const out =
  '// AUTO-GENERATED by tools/gen-constellations-content.mjs from constellations_content.h — DO NOT EDIT.\n' +
  '// Regenerate after changing goods/economies/systems/missions in the firmware header.\n' +
  `export const CONTENT = ${JSON.stringify(CONTENT, null, 2)};\n` +
  'export default CONTENT;\n';
writeFileSync(OUT, out);
console.log(`gen-constellations-content: wrote ${OUT}`);
console.log(`  goods=${goods.length} econ=${econ.length} factions=${factions.length} (systems/missions are procedural)`);

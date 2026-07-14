// convert.js — the PURE conversion engine + bilingual (EN/IT) natural-language parser for the
// NucleoOS Unit Converter app. DOM-free and dependency-free, so it runs in the browser AND under
// Node — the whole thing is host-testable (tools/anima-host/unit-converter.test.mjs). The app UI
// (index.html) imports it; so does the test. This is the same "pure core + NL command" pattern
// Paint (nlcommand.js) and the Spreadsheet copilot use.

// ── Unit tables ──────────────────────────────────────────────────────────────────────────────
// Each linear category maps a canonical unit → its factor to the category's BASE unit, so
// convert = value * factor[from] / factor[to]. Temperature is non-linear → special-cased below.
export const UNITS = {
  length: { base: 'm', f: { m: 1, km: 1000, cm: 0.01, mm: 0.001, um: 1e-6, nm: 1e-9,
    mi: 1609.344, yd: 0.9144, ft: 0.3048, in: 0.0254, nmi: 1852 } },
  mass: { base: 'kg', f: { kg: 1, g: 0.001, mg: 1e-6, t: 1000,
    lb: 0.45359237, oz: 0.028349523125, st: 6.35029318 } },
  temperature: { base: 'C', temp: true, f: { C: 1, F: 1, K: 1 } },
  volume: { base: 'l', f: { l: 1, ml: 0.001, m3: 1000, gal: 3.785411784, qt: 0.946352946,
    pt: 0.473176473, cup: 0.2365882365, floz: 0.0295735295625 } },
  data: { base: 'B', f: { B: 1, KB: 1024, MB: 1048576, GB: 1073741824, TB: 1099511627776,
    bit: 0.125, kbit: 128, mbit: 131072, gbit: 134217728 } },
  speed: { base: 'mps', f: { mps: 1, kmh: 1000 / 3600, mph: 0.44704, kn: 1852 / 3600, fps: 0.3048 } },
  time: { base: 's', f: { s: 1, ms: 0.001, min: 60, h: 3600, d: 86400, wk: 604800, yr: 31557600 } },
};

// canonical unit → category (built once from UNITS).
export const CANON_CAT = (() => {
  const m = {};
  for (const [cat, def] of Object.entries(UNITS)) for (const u of Object.keys(def.f)) m[u] = cat;
  return m;
})();

// ── Bilingual aliases: many spellings (EN + IT, symbol + word, sing/plural) → the canonical unit.
// Lower-cased, accents folded (via `fold`), so "miglia", "Miles", "MI" all resolve to `mi`.
const ALIASES = {
  // length
  m: 'm', meter: 'm', meters: 'm', metre: 'm', metres: 'm', metro: 'm', metri: 'm',
  km: 'km', kilometer: 'km', kilometers: 'km', kilometre: 'km', kilometres: 'km', chilometro: 'km', chilometri: 'km',
  cm: 'cm', centimeter: 'cm', centimeters: 'cm', centimetro: 'cm', centimetri: 'cm',
  mm: 'mm', millimeter: 'mm', millimeters: 'mm', millimetro: 'mm', millimetri: 'mm',
  um: 'um', micron: 'um', micrometer: 'um', micrometro: 'um',
  nm: 'nm', nanometer: 'nm', nanometro: 'nm',
  mi: 'mi', mile: 'mi', miles: 'mi', miglio: 'mi', miglia: 'mi',
  yd: 'yd', yard: 'yd', yards: 'yd', iarda: 'yd', iarde: 'yd',
  ft: 'ft', foot: 'ft', feet: 'ft', piede: 'ft', piedi: 'ft',
  in: 'in', inch: 'in', inches: 'in', pollice: 'in', pollici: 'in',
  nmi: 'nmi', 'nautical-mile': 'nmi', 'miglio-nautico': 'nmi',
  // mass
  kg: 'kg', kilogram: 'kg', kilograms: 'kg', kilogramme: 'kg', chilo: 'kg', chili: 'kg', chilogrammo: 'kg', chilogrammi: 'kg',
  g: 'g', gram: 'g', grams: 'g', grammo: 'g', grammi: 'g',
  mg: 'mg', milligram: 'mg', milligrams: 'mg', milligrammo: 'mg', milligrammi: 'mg',
  t: 't', ton: 't', tonne: 't', tonnes: 't', tons: 't', tonnellata: 't', tonnellate: 't',
  lb: 'lb', lbs: 'lb', pound: 'lb', pounds: 'lb', libbra: 'lb', libbre: 'lb',
  oz: 'oz', ounce: 'oz', ounces: 'oz', oncia: 'oz', once: 'oz',
  st: 'st', stone: 'st', stones: 'st',
  // temperature
  c: 'C', celsius: 'C', centigrade: 'C', centigradi: 'C', gradi: 'C', grado: 'C', degree: 'C', degrees: 'C',
  f: 'F', fahrenheit: 'F',
  k: 'K', kelvin: 'K',
  // volume
  l: 'l', liter: 'l', liters: 'l', litre: 'l', litres: 'l', litro: 'l', litri: 'l',
  ml: 'ml', milliliter: 'ml', milliliters: 'ml', millilitro: 'ml', millilitri: 'ml',
  m3: 'm3', 'cubic-meter': 'm3', 'metro-cubo': 'm3',
  gal: 'gal', gallon: 'gal', gallons: 'gal', gallone: 'gal', galloni: 'gal',
  qt: 'qt', quart: 'qt', quarts: 'qt',
  pt: 'pt', pint: 'pt', pints: 'pt', pinta: 'pt', pinte: 'pt',
  cup: 'cup', cups: 'cup', tazza: 'cup', tazze: 'cup',
  floz: 'floz', 'fl-oz': 'floz', 'fluid-ounce': 'floz',
  // data
  b: 'B', byte: 'B', bytes: 'B',
  kb: 'KB', kilobyte: 'KB', kilobytes: 'KB',
  mb: 'MB', megabyte: 'MB', megabytes: 'MB',
  gb: 'GB', gigabyte: 'GB', gigabytes: 'GB',
  tb: 'TB', terabyte: 'TB', terabytes: 'TB',
  bit: 'bit', bits: 'bit',
  kbit: 'kbit', kilobit: 'kbit', mbit: 'mbit', megabit: 'mbit', gbit: 'gbit', gigabit: 'gbit',
  // speed
  mps: 'mps', 'm/s': 'mps', 'meters-per-second': 'mps', 'metri-al-secondo': 'mps',
  kmh: 'kmh', 'km/h': 'kmh', kph: 'kmh', 'chilometri-orari': 'kmh',
  mph: 'mph', 'mi/h': 'mph', 'miglia-orarie': 'mph',
  kn: 'kn', knot: 'kn', knots: 'kn', nodo: 'kn', nodi: 'kn',
  fps: 'fps', 'ft/s': 'fps',
  // time
  s: 's', sec: 's', secs: 's', second: 's', seconds: 's', secondo: 's', secondi: 's',
  ms: 'ms', millisecond: 'ms', milliseconds: 'ms', millisecondo: 'ms', millisecondi: 'ms',
  min: 'min', mins: 'min', minute: 'min', minutes: 'min', minuto: 'min', minuti: 'min',
  h: 'h', hr: 'h', hrs: 'h', hour: 'h', hours: 'h', ora: 'h', ore: 'h',
  d: 'd', day: 'd', days: 'd', giorno: 'd', giorni: 'd',
  wk: 'wk', week: 'wk', weeks: 'wk', settimana: 'wk', settimane: 'wk',
  yr: 'yr', year: 'yr', years: 'yr', anno: 'yr', anni: 'yr',
};

// Fold accents + lower-case so IT accented words and symbols normalise ("°C" → "c", "à" → "a").
export function fold(s) {
  return String(s == null ? '' : s).toLowerCase()
    .normalize('NFD').replace(/[̀-ͯ]/g, '')   // strip combining accents
    .replace(/°/g, '').trim();
}

// Resolve one token (or a small multi-word unit like "fl oz") to a canonical unit, or null.
export function resolveUnit(token) {
  const t = fold(token).replace(/\s+/g, '-');
  if (ALIASES[t]) return ALIASES[t];
  const bare = t.replace(/-/g, '');
  if (ALIASES[bare]) return ALIASES[bare];
  return null;
}

export function categoryOf(unit) { return CANON_CAT[unit] || null; }
export function unitsInCategory(cat) { return UNITS[cat] ? Object.keys(UNITS[cat].f) : []; }

// ── The conversion ───────────────────────────────────────────────────────────────────────────
const toCelsius = (v, u) => (u === 'C' ? v : u === 'F' ? (v - 32) * 5 / 9 : v - 273.15);
const fromCelsius = (c, u) => (u === 'C' ? c : u === 'F' ? c * 9 / 5 + 32 : c + 273.15);

// convert(value, from, to) → { ok, value } or { ok:false, reason }. from/to may be raw tokens or
// canonical units; both must be in the SAME category.
export function convert(value, from, to) {
  const n = typeof value === 'number' ? value : parseFloat(String(value).replace(',', '.'));
  if (!isFinite(n)) return { ok: false, reason: 'bad-number' };
  const uf = CANON_CAT[from] ? from : resolveUnit(from);
  const ut = CANON_CAT[to] ? to : resolveUnit(to);
  if (!uf) return { ok: false, reason: 'unknown-from' };
  if (!ut) return { ok: false, reason: 'unknown-to' };
  const cat = CANON_CAT[uf];
  if (CANON_CAT[ut] !== cat) return { ok: false, reason: 'cross-category', from: uf, to: ut };
  if (UNITS[cat].temp) return { ok: true, value: fromCelsius(toCelsius(n, uf), ut), from: uf, to: ut, category: cat };
  const f = UNITS[cat].f;
  return { ok: true, value: n * f[uf] / f[ut], from: uf, to: ut, category: cat };
}

// Round for display without trailing-zero noise; keeps ~6 significant digits.
export function fmt(x) {
  if (!isFinite(x)) return '—';
  if (x === 0) return '0';
  const abs = Math.abs(x);
  let s;
  if (abs >= 1e12 || abs < 1e-6) s = x.toExponential(4);
  else s = String(Math.round(x * 1e6) / 1e6);
  return s;
}

// ── The natural-language command parser (EN + IT), offline & deterministic ─────────────────────
// Handles the shapes people actually type:
//   "convert 10 km to miles" · "10 km to mi" · "how much is 5 kg in pounds" · "32 F in C"
//   "converti 10 km in miglia" · "quanto fa 5 kg in libbre" · "100 gradi in fahrenheit" · "10km -> mi"
// Returns { ok, value, from, to, category } (ready for convert) or { ok:false, reason }.
// The connective that separates the two units is EN "to/in/into" or IT "in/a", or "->"/"→".
const NUM_RE = /(-?\d+(?:[.,]\d+)?)/;

export function parseCommand(text, lang) {
  const raw = String(text == null ? '' : text).trim();
  if (!raw) return { ok: false, reason: 'empty' };
  const s = fold(raw).replace(/->|→|=>/g, ' to ').replace(/\s+/g, ' ');

  // number
  const nm = s.match(NUM_RE);
  if (!nm) return { ok: false, reason: 'no-number' };
  const value = parseFloat(nm[1].replace(',', '.'));
  // everything after the number, minus a leading "of"/"di"
  let rest = s.slice(nm.index + nm[1].length).trim().replace(/^(of|di)\s+/, '');

  // split on the connective into [fromPart, toPart]. Try the connectives longest-first so
  // "into" wins over "in". IT "a" is only a connective when surrounded by spaces.
  const conns = [' into ', ' to ', ' in ', ' a '];
  let fromPart = '', toPart = '';
  for (const c of conns) {
    const i = rest.indexOf(c);
    if (i >= 0) { fromPart = rest.slice(0, i).trim(); toPart = rest.slice(i + c.length).trim(); break; }
  }
  if (!toPart) return { ok: false, reason: 'no-connective' };

  // the units are the LAST token of each part (drops filler like "gradi"/"degrees", "quanto fa").
  const from = resolveUnit(lastUnitToken(fromPart));
  const to = resolveUnit(lastUnitToken(toPart));
  if (!from) return { ok: false, reason: 'unknown-from', token: fromPart };
  if (!to) return { ok: false, reason: 'unknown-to', token: toPart };
  const cat = CANON_CAT[from];
  if (CANON_CAT[to] !== cat) return { ok: false, reason: 'cross-category', from, to };
  return { ok: true, value, from, to, category: cat };
}

// Pick the unit-bearing token from a phrase: scan tokens right→left and return the first that
// resolves to a known unit (so "gradi celsius" → "celsius", "5 chili" → "chili"). Falls back to
// the whole phrase (handles multi-word units like "fl oz" via resolveUnit's dash join).
function lastUnitToken(phrase) {
  const p = String(phrase).trim();
  if (!p) return '';
  if (resolveUnit(p)) return p;
  const toks = p.split(' ').filter(Boolean);
  for (let i = toks.length - 1; i >= 0; i--) if (resolveUnit(toks[i])) return toks[i];
  // try adjacent pairs (multi-word units) right→left
  for (let i = toks.length - 2; i >= 0; i--) if (resolveUnit(toks[i] + '-' + toks[i + 1])) return toks[i] + '-' + toks[i + 1];
  return toks[toks.length - 1] || '';
}

// One-shot: parse a command and, if valid, convert it. Returns the parse plus { result } on success.
export function runCommand(text, lang) {
  const p = parseCommand(text, lang);
  if (!p.ok) return p;
  const c = convert(p.value, p.from, p.to);
  if (!c.ok) return { ok: false, reason: c.reason, from: p.from, to: p.to };
  return { ok: true, value: p.value, from: p.from, to: p.to, category: p.category, result: c.value };
}

// i18n-check.mjs — gate for the NucleoOS centralized translation system.
//
// Verifies the invariants the runtime (web/shell/nucleo-i18n.js) relies on, so a missing or
// drifted string is caught here instead of showing a raw key on the device:
//   1. Every catalog is valid JSON.
//   2. For each namespace, the language files share the SAME set of keys (no key in `it` missing
//      from `en` or vice versa) — the fallback chain exists for safety, not as a license to drift.
//   3. Plural values are objects with at least an `other` form in every language.
//   4. Interpolation placeholders ({name}, {count}, …) match across languages of the same key.
//   5. Every `data-i18n` / `data-i18n-attr` / `data-i18n-html` key referenced in a UI HTML file
//      exists in that surface's namespace (app id, or `shell`) or in the shared `core` catalog.
//
// Zero dependencies, BOM-safe. Usage: node tools/i18n-check.mjs   (exit 1 on any failure).

import { readdirSync, readFileSync, writeFileSync, existsSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const LANGS = ['it', 'en'];                  // keep in sync with LANGS in nucleo-i18n.js
const BASE = 'it';

const errors = [];
const warnings = [];
let nsCount = 0, keyCount = 0;

function readJSON(path) {
  let txt = readFileSync(path, 'utf8');
  if (txt.charCodeAt(0) === 0xfeff) txt = txt.slice(1);   // strip BOM
  return JSON.parse(txt);
}

// A namespace = a logical surface (core, shell, or an app id) with one catalog file per language.
// Discover them: shell-level catalogs in web/shell/i18n/<ns>.<lang>.json, plus per-app
// apps/<id>/www/i18n.<lang>.json.
function discoverNamespaces() {
  const map = new Map();   // ns -> { lang -> absolute path }
  const add = (ns, lang, path) => { if (!map.has(ns)) map.set(ns, {}); map.get(ns)[lang] = path; };

  const shellI18n = join(ROOT, 'web', 'shell', 'i18n');
  if (existsSync(shellI18n)) {
    for (const f of readdirSync(shellI18n)) {
      const m = f.match(/^([a-z0-9-]+)\.([a-z]{2})\.json$/i);
      if (m && LANGS.includes(m[2])) add(m[1], m[2], join(shellI18n, f));
    }
  }
  const appsDir = join(ROOT, 'apps');
  if (existsSync(appsDir)) {
    for (const id of readdirSync(appsDir)) {
      const www = join(appsDir, id, 'www');
      if (!existsSync(www) || !statSync(www).isDirectory()) continue;
      for (const lang of LANGS) {
        const p = join(www, `i18n.${lang}.json`);
        if (existsSync(p)) add(id, lang, p);
      }
    }
  }
  return map;
}

const placeholders = (v) => {
  const out = new Set();
  const scan = (s) => { const re = /\{(\w+)\}/g; let m; while ((m = re.exec(s))) out.add(m[1]); };
  if (typeof v === 'string') scan(v);
  else if (v && typeof v === 'object') for (const k of Object.keys(v)) if (typeof v[k] === 'string') scan(v[k]);
  return out;
};
const setEq = (a, b) => a.size === b.size && [...a].every((x) => b.has(x));

// Load + validate every catalog; return ns -> { lang -> {key:value} } with metadata keys dropped.
function loadCatalogs(map) {
  const catalogs = new Map();
  for (const [ns, byLang] of map) {
    nsCount++;
    const langs = {};
    for (const lang of LANGS) {
      if (!byLang[lang]) { errors.push(`[${ns}] missing ${lang} catalog (have: ${Object.keys(byLang).join(', ') || 'none'})`); continue; }
      let obj;
      try { obj = readJSON(byLang[lang]); }
      catch (e) { errors.push(`[${ns}.${lang}] invalid JSON: ${e.message}`); continue; }
      const clean = {};
      for (const k of Object.keys(obj)) if (!k.startsWith('_')) clean[k] = obj[k];
      langs[lang] = clean;
    }
    catalogs.set(ns, langs);

    // (2) key-set parity across languages
    if (langs[BASE]) {
      const baseKeys = new Set(Object.keys(langs[BASE]));
      keyCount += baseKeys.size;
      for (const lang of LANGS) {
        if (lang === BASE || !langs[lang]) continue;
        const other = new Set(Object.keys(langs[lang]));
        const missing = [...baseKeys].filter((k) => !other.has(k));
        const extra = [...other].filter((k) => !baseKeys.has(k));
        if (missing.length) errors.push(`[${ns}] keys in ${BASE} missing from ${lang}: ${missing.join(', ')}`);
        if (extra.length) errors.push(`[${ns}] keys in ${lang} not in ${BASE}: ${extra.join(', ')}`);
      }
    }

    // (3)+(4) plural shape + placeholder parity, per key present in the base language
    if (langs[BASE]) for (const key of Object.keys(langs[BASE])) {
      const baseVal = langs[BASE][key];
      const basePh = placeholders(baseVal);
      for (const lang of LANGS) {
        const v = langs[lang] && langs[lang][key];
        if (v === undefined) continue;
        if (typeof baseVal === 'object' || typeof v === 'object') {
          if (typeof v !== 'object' || v.other === undefined) errors.push(`[${ns}.${lang}] "${key}" plural value must be an object with an "other" form`);
        }
        if (!setEq(basePh, placeholders(v))) warnings.push(`[${ns}] "${key}" placeholders differ between ${BASE} {${[...basePh]}} and ${lang} {${[...placeholders(v)]}}`);
      }
    }
  }
  return catalogs;
}

// (5) referenced-key validation: scan the HTML for each surface and confirm every key resolves.
function checkHtmlRefs(catalogs) {
  const surfaces = [];
  const shellIdx = join(ROOT, 'web', 'shell', 'index.html');
  if (existsSync(shellIdx)) surfaces.push({ ns: 'shell', file: shellIdx });
  const appsDir = join(ROOT, 'apps');
  if (existsSync(appsDir)) for (const id of readdirSync(appsDir)) {
    const idx = join(appsDir, id, 'www', 'index.html');
    if (existsSync(idx)) surfaces.push({ ns: id, file: idx });
  }

  const coreKeys = new Set(Object.keys((catalogs.get('core') || {})[BASE] || {}));
  for (const { ns, file } of surfaces) {
    const nsKeys = new Set(Object.keys((catalogs.get(ns) || {})[BASE] || {}));
    if (!nsKeys.size && ns !== 'shell') continue;   // app not migrated yet → nothing to check
    const html = readFileSync(file, 'utf8');
    const refs = new Set();
    let m;
    const reText = /data-i18n(?:-html)?="([^"]+)"/g;
    while ((m = reText.exec(html))) refs.add(m[1]);
    const reAttr = /data-i18n-attr="([^"]+)"/g;
    while ((m = reAttr.exec(html))) for (const pair of m[1].split(';')) { const i = pair.indexOf(':'); if (i >= 0) refs.add(pair.slice(i + 1).trim()); }
    // Only validate STATIC keys. A key built in a JS template literal (e.g. data-i18n="${emptyKey}")
    // is computed at runtime and resolves to a real key then — it cannot be checked statically.
    const isStatic = (k) => /^[\w.-]+$/.test(k);
    const missing = [...refs].filter((k) => k && isStatic(k) && !nsKeys.has(k) && !coreKeys.has(k));
    if (missing.length) errors.push(`[${ns}] index.html references unknown i18n keys: ${missing.join(', ')}`);
  }
}

const map = discoverNamespaces();
const catalogs = loadCatalogs(map);
checkHtmlRefs(catalogs);

for (const w of warnings) console.log('  ⚠ ' + w);
if (errors.length) {
  console.error(`\n✗ i18n gate FAILED — ${errors.length} error(s):`);
  for (const e of errors) console.error('  • ' + e);
  process.exit(1);
}
// Emit a tiny coverage manifest the Settings "Language & Region" panel reads (one small fetch — no
// runtime scanning of every catalog on the device). Counts base-language keys per namespace.
const nsKeyCount = {};
for (const [ns, byLang] of catalogs) nsKeyCount[ns] = Object.keys(byLang[BASE] || {}).length;
const coverage = {
  generated: new Date().toISOString(),
  base: BASE,
  langs: LANGS,
  surfaces: nsCount,
  totalKeys: keyCount,
  namespaces: nsKeyCount,
};
try {
  writeFileSync(join(ROOT, 'web', 'shell', 'i18n', 'coverage.json'), JSON.stringify(coverage, null, 2) + '\n');
} catch (e) { console.log('  (coverage.json not written: ' + e.message + ')'); }

console.log(`✓ i18n gate OK — ${nsCount} namespaces, ${keyCount} keys, ${LANGS.length} languages, ${warnings.length} warning(s)`);

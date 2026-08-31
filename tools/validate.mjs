// NucleoOS registry & manifest validator — zero dependencies (Node ESM).
// Validates every registry file and app manifest against schemas/, then runs
// cross-reference checks. Run: `node tools/validate.mjs` (or `npm run validate`).
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const errs = [];
// Strip a UTF-8 BOM (common on Windows) before parsing.
const read = (p) => JSON.parse(readFileSync(join(ROOT, p), 'utf8').replace(/^﻿/, ''));
// Guarded load: bad JSON is reported as a normal problem, never a stack trace.
const load = (p) => { try { return read(p); } catch (e) { errs.push(`${p}: invalid JSON (${e.message})`); return null; } };
const typeOf = (v) => (Array.isArray(v) ? 'array' : v === null ? 'null' : typeof v);

// Minimal JSON-Schema subset: type, required, properties, additionalProperties,
// enum, items, pattern, minLength/maxLength, minimum/maximum, and local $ref (#/$defs/...).
function check(data, schema, root, path, errs) {
  if (schema.$ref) schema = schema.$ref.replace(/^#\//, '').split('/').reduce((s, k) => s[k], root);
  if (schema.type) {
    const t = typeOf(data);
    const ok = [].concat(schema.type).some((x) => x === t || (x === 'integer' && t === 'number' && Number.isInteger(data)));
    if (!ok) errs.push(`${path}: expected ${schema.type}, got ${t}`);
  }
  if (schema.enum && !schema.enum.includes(data)) errs.push(`${path}: '${data}' not in [${schema.enum.join(', ')}]`);
  if (schema.pattern && typeof data === 'string' && !new RegExp(schema.pattern).test(data)) errs.push(`${path}: '${data}' fails /${schema.pattern}/`);
  if (typeof data === 'string' && schema.minLength !== undefined && data.length < schema.minLength) errs.push(`${path}: length ${data.length} < minLength ${schema.minLength}`);
  if (typeof data === 'string' && schema.maxLength !== undefined && data.length > schema.maxLength) errs.push(`${path}: length ${data.length} > maxLength ${schema.maxLength}`);   // e.g. wifi SSID 32 / passphrase 63 are hard 802.11 buffer limits
  if (typeof data === 'number' && schema.minimum !== undefined && data < schema.minimum) errs.push(`${path}: ${data} < ${schema.minimum}`);
  if (typeof data === 'number' && schema.maximum !== undefined && data > schema.maximum) errs.push(`${path}: ${data} > ${schema.maximum}`);
  if (typeOf(data) === 'object') {
    for (const r of schema.required || []) if (!(r in data)) errs.push(`${path}: missing required '${r}'`);
    for (const [k, v] of Object.entries(data)) {
      const ps = schema.properties && schema.properties[k];
      if (ps) check(v, ps, root, `${path}.${k}`, errs);
      else if (schema.additionalProperties === false) errs.push(`${path}: unexpected key '${k}'`);
      else if (typeOf(schema.additionalProperties) === 'object') check(v, schema.additionalProperties, root, `${path}.${k}`, errs);
    }
  }
  if (typeOf(data) === 'array' && schema.items) data.forEach((it, i) => check(it, schema.items, root, `${path}[${i}]`, errs));
  return errs;
}

const validate = (file, schemaPath, label) => {
  const schema = read(schemaPath);
  const data = load(file);
  if (data !== null) check(data, schema, schema, label, errs);
};

// 1) Schema validation
validate('registry/apps.json', 'schemas/registry-apps.schema.json', 'apps.json');
validate('registry/file-associations.json', 'schemas/file-associations.schema.json', 'file-associations.json');
validate('registry/settings.json', 'schemas/settings.schema.json', 'settings.json');
const manifestSchema = read('schemas/manifest.schema.json');
const appDirs = readdirSync(join(ROOT, 'apps'), { withFileTypes: true }).filter((d) => d.isDirectory()).map((d) => d.name);
const manifests = {};
for (const id of appDirs) {
  const m = load(`apps/${id}/manifest.json`);
  manifests[id] = m;
  if (m !== null) check(m, manifestSchema, manifestSchema, `${id}/manifest`, errs);
}

// 2) Cross-reference checks
const apps = (load('registry/apps.json') || { installed: [] }).installed;
const ids = new Set(apps.map((a) => a.id));
const assoc = load('registry/file-associations.json') || { fallback: '', default_open: {} };
for (const a of apps) {
  if (!existsSync(join(ROOT, `apps/${a.id}/manifest.json`))) { errs.push(`xref: installed app '${a.id}' has no manifest`); continue; }
  if (manifests[a.id] && manifests[a.id].id !== a.id) errs.push(`xref: id mismatch for '${a.id}' (manifest says '${manifests[a.id].id}')`);
}

// A file association must point at something that can actually OPEN a window. 'rule'/'auto'/'flow'
// pointed at automation-studio, which is runtime:'service' and ships no www/ at all: double-clicking
// one of those files produced a blank 404 iframe with no error anywhere. Nothing checked it, so the
// data could stay broken indefinitely.
for (const [ext, id] of Object.entries(assoc.default_open || {})) {
  const m = manifests[id];
  if (!m) { errs.push(`assoc: .${ext} → '${id}', which is not an app in apps/`); continue; }
  if (!apps.some((a) => a.id === id && a.enabled !== false)) errs.push(`assoc: .${ext} → '${id}', which is not installed+enabled`);
  // `runtime` is optional, so absence means "web" by convention — only an EXPLICIT non-web value is a
  // contradiction. The decisive test either way is whether there is a page to load at all.
  if (m.runtime && m.runtime !== 'web') errs.push(`assoc: .${ext} → '${id}' is runtime:'${m.runtime}' — only a web app can open a file in a window`);
  if (!existsSync(join(ROOT, `apps/${id}/www/index.html`))) errs.push(`assoc: .${ext} → '${id}' has no www/index.html — the window would load blank`);
}
if (assoc.fallback && !manifests[assoc.fallback]) errs.push(`assoc: fallback '${assoc.fallback}' is not an app`);

// The shell carries a built-in SEED copy of default_open, used until /api/associations answers (and
// kept if that fetch fails). When the two disagreed, the same photo opened in a different app
// depending on whether a request happened to succeed — a coin flip the user could never explain.
try {
  const shellSrc = readFileSync(join(ROOT, 'web/shell/shell.js'), 'utf8');
  const blk = /default_open: \{([\s\S]*?)\},\s*\n\s*fallback:/.exec(shellSrc);
  if (!blk) errs.push('assoc: could not find the shell seed map (web/shell/shell.js) to compare');
  else {
    for (const mm of blk[1].matchAll(/(\w+):\s*'([^']+)'/g)) {
      const reg = (assoc.default_open || {})[mm[1]];
      if (reg && reg !== mm[2]) errs.push(`assoc: shell seed .${mm[1]} → '${mm[2]}' but the registry says '${reg}'`);
    }
  }
} catch (e) { errs.push('assoc: shell seed check failed: ' + e.message); }
if (!ids.has(assoc.fallback)) errs.push(`xref: fallback '${assoc.fallback}' is not an installed app`);
for (const [ext, app] of Object.entries(assoc.default_open)) {
  if (!ids.has(app)) { errs.push(`xref: .${ext} -> unknown app '${app}'`); continue; }
  const exts = (manifests[app] && manifests[app].handles && manifests[app].handles.extensions) || [];
  if (!exts.includes(ext)) errs.push(`xref: '${app}' is default for .${ext} but does not declare it in handles`);
}

// app-launch aliases (single source shared verbatim with firmware/.../nucleo_anima.c via
// tools/anima/gen_aliases.py). Hard-fail on an exact alias collision (resolution would be
// order-dependent); warn on a target that isn't an installed app (e.g. "radio" is a
// media-player mode, deliberately kept so the device vocabulary stays intact).
const aliasWarns = [];
const aliasPack = load('registry/app-aliases.json');
if (aliasPack && aliasPack.aliases) {
  const claimed = new Map();
  for (const [app, al] of Object.entries(aliasPack.aliases)) {
    if (!ids.has(app)) aliasWarns.push(`app-aliases: target '${app}' is not an installed app`);
    for (const a of al) {
      if (claimed.has(a) && claimed.get(a) !== app) errs.push(`xref: app-alias '${a}' claimed by both '${claimed.get(a)}' and '${app}'`);
      else claimed.set(a, app);
    }
  }
}

// 3) API spec drift: the Help app's Mini Swagger (registry/web-api-spec.json) is generated from
// the real firmware routes by tools/gen-api-spec.mjs. Re-run the build in memory and fail if the
// committed spec, the deploy mirrors, or the docs sidecar have drifted from the C source.
try {
  const { buildSpec, serialize, OUT_PATHS } = await import('./gen-api-spec.mjs');
  const { spec, undocumented, stale } = buildSpec();
  const json = serialize(spec);
  const rawText = (p) => readFileSync(join(ROOT, p), 'utf8').replace(/^﻿/, '');
  for (const p of OUT_PATHS) {
    if (!existsSync(join(ROOT, p))) errs.push(`api-spec: ${p} missing (run: npm run gen:api)`);
    else if (rawText(p) !== json) errs.push(`api-spec: ${p} out of date with firmware (run: npm run gen:api)`);
  }
  for (const k of undocumented) errs.push(`api-spec: undocumented real route '${k}' (add it to registry/api-docs.json)`);
  for (const k of stale) errs.push(`api-spec: stale doc '${k}' (no such firmware route)`);
} catch (e) {
  errs.push(`api-spec: drift check failed (${e.message})`);
}

// 4) .gz freshness: the webfs serves "<file>.gz" with precedence, so a stale/orphan sibling ships old
// or deleted code to the device. Fail the lint if any served-tree .gz is out of sync with its source.
try {
  const { checkGz } = await import('./check-gz.mjs');
  const { stale, orphan } = checkGz();
  for (const s of stale) errs.push(`gz: stale '${s}' (run: node tools/gzip-assets.mjs)`);
  for (const o of orphan) errs.push(`gz: orphan '${o}' (source deleted — remove the .gz)`);
} catch (e) {
  errs.push(`gz: freshness check failed (${e.message})`);
}

// 5) Arcade core reports: EmulatorJS keys its IndexedDB core cache on cores/reports/<core>.json.
// A missing or stale report disables that cache, so the device re-streams a ~1 MB core on every
// launch — straight into the webfs low-heap 503. The report is derived from the core blob itself,
// so swapping a core without regenerating it is drift the gate can see.
try {
  const { checkCoreReports } = await import('./gen-core-reports.mjs');
  for (const d of checkCoreReports()) errs.push(`arcade: core report ${d} (run: node tools/gen-core-reports.mjs)`);
} catch (e) {
  errs.push(`arcade: core report check failed (${e.message})`);
}

if (errs.length) {
  console.error(`✗ ${errs.length} problem(s):\n` + errs.map((e) => '  - ' + e).join('\n'));
  process.exit(1);
}
if (aliasWarns.length) console.warn(aliasWarns.map((w) => '  ! ' + w).join('\n'));
console.log(`✓ valid: ${appDirs.length} manifests, ${apps.length} installed apps, ${Object.keys(assoc.default_open).length} associations`);

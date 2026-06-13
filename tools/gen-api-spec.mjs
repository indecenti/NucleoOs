// NucleoOS — generate the Help app's "Mini Swagger" spec from the REAL firmware.
//
// Single source of truth = the `httpd_register_uri_handler` tables in
// firmware/components/**/*.c. This tool extracts every registered `/api/...`
// route (path + method) straight from the C source, then merges the
// hand-authored it/en prose from registry/api-docs.json. The result,
// registry/web-api-spec.json, therefore ALWAYS matches the compiled firmware —
// it is a build artifact, never edited by hand. Zero runtime cost on the
// Cardputer: the device only ever serves a static JSON file.
//
//   node tools/gen-api-spec.mjs          # regenerate the spec (+ deploy mirrors)
//   node tools/gen-api-spec.mjs --check  # CI/pre-flight: exit 1 on any drift
//
// Drift is reported in both directions: a real route with no docs entry
// (undocumented) and a docs entry whose route no longer exists (stale).
import { readFileSync, writeFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const read = (p) => readFileSync(join(ROOT, p), 'utf8').replace(/^﻿/, '');
const readJSON = (p) => JSON.parse(read(p));

// --- 1) Walk firmware/components for our own C source (never the vendored
//        reference trees: reference/, tmp_esp_claw/, esp-claw, ...).
function firmwareSources() {
  const base = join(ROOT, 'firmware', 'components');
  const out = [];
  const walk = (dir) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      const p = join(dir, e.name);
      if (e.isDirectory()) walk(p);
      else if (/\.(c|cpp|cc)$/.test(e.name)) out.push(p);
    }
  };
  walk(base);
  return out;
}

// --- 2) Extract registered routes. Both registration styles in the codebase
//        place `.uri = "..."` and `.method = HTTP_xxx` on the same line:
//          { .uri = "/api/fs/list", .method = HTTP_GET, .handler = list_get },
//          httpd_uri_t s = { .uri = "/api/status", .method = HTTP_GET, ... };
//        We only keep "/api/..." paths and skip wildcards (e.g. "/api/lua/*").
const ROUTE_RE = /\.uri\s*=\s*"(\/api\/[^"*]+)"\s*,\s*\.method\s*=\s*HTTP_([A-Z]+)/g;

function discoverRoutes() {
  const routes = new Map(); // "METHOD /path" -> { path, method, source }
  for (const file of firmwareSources()) {
    const src = readFileSync(file, 'utf8');
    for (const m of src.matchAll(ROUTE_RE)) {
      const path = m[1];
      const method = m[2];
      const key = `${method} ${path}`;
      if (!routes.has(key)) {
        routes.set(key, { path, method, source: file.slice(ROOT.length + 1).replace(/\\/g, '/') });
      }
    }
  }
  return routes;
}

// id: /api/fs/list -> api-fs-list. Methods on a shared path get a -get/-post suffix.
function idFor(path) {
  return 'api' + path.slice(4).replace(/\//g, '-');
}

function buildSpec() {
  const routes = discoverRoutes();
  const docs = readJSON('registry/api-docs.json');
  const undocumented = [];
  const stale = [];
  const seenIds = new Map();

  // First pass: which paths carry more than one method (need id disambiguation).
  const methodsByPath = new Map();
  for (const { path, method } of routes.values()) {
    if (!methodsByPath.has(path)) methodsByPath.set(path, []);
    methodsByPath.get(path).push(method);
  }

  const spec = [];
  for (const [key, route] of [...routes.entries()].sort()) {
    const doc = docs[key];
    if (!doc) { undocumented.push(key); }
    if (doc && doc.hidden) continue; // documented but deliberately not published

    let id = idFor(route.path);
    if ((methodsByPath.get(route.path) || []).length > 1) id += '-' + route.method.toLowerCase();
    if (seenIds.has(id)) id += '-' + (seenIds.get(id) + 1);
    seenIds.set(id, (seenIds.get(id) || 0) + 1);

    const entry = {
      id,
      path: route.path,
      method: route.method,
      category: 'api',
      it: doc?.it || stub(route, 'it'),
      en: doc?.en || stub(route, 'en'),
      params: doc?.params || [],
    };
    if (!doc) entry._undocumented = true;
    spec.push(entry);
  }

  // Reverse drift: docs entries that no longer map to a real route.
  for (const key of Object.keys(docs)) {
    if (key.startsWith('_')) continue;           // _comment and friends
    if (!routes.has(key)) stale.push(key);
  }

  return { spec, undocumented, stale, routeCount: routes.size };
}

function stub(route, lang) {
  const title = `${route.method} ${route.path}`;
  const desc = lang === 'it'
    ? 'Endpoint reale del firmware non ancora documentato. Aggiungi una voce in registry/api-docs.json.'
    : 'Real firmware endpoint, not documented yet. Add an entry to registry/api-docs.json.';
  return { title, description: desc, synopsis: title, details: '' };
}

// Canonical source of truth — the ONLY path the drift gate checks. The deploy mirrors
// (deploy/sd, deploy/sd-safe) are build outputs assembled from registry/ by tools/deploy.ps1
// (which also re-gzips), so they are never written here.
export const OUT_PATHS = ['registry/web-api-spec.json'];

// Write-only dev convenience: the host simulator (tools/serve-shell.mjs) sandboxes /api/fs/read
// to tools/sd-sim, so the Help app needs the spec there to render under the sim. Kept fresh by
// `npm run gen:api`, but intentionally NOT part of the drift gate (it's a fixture, not a source).
const EXTRA_MIRRORS = ['tools/sd-sim/system/registry/web-api-spec.json'];

export function serialize(spec) {
  return JSON.stringify(spec, null, 2) + '\n';
}

export { buildSpec };

function main() {
  const check = process.argv.includes('--check');
  const { spec, undocumented, stale, routeCount } = buildSpec();
  const json = serialize(spec);

  if (check) {
    const problems = [];
    for (const p of OUT_PATHS) {
      if (!existsSync(join(ROOT, p))) { problems.push(`${p}: missing (run: npm run gen:api)`); continue; }
      if (read(p) !== json) problems.push(`${p}: out of date with firmware (run: npm run gen:api)`);
    }
    for (const k of undocumented) problems.push(`undocumented real route: ${k}`);
    for (const k of stale) problems.push(`stale doc (no such route): ${k}`);
    if (problems.length) {
      console.error(`✗ api-spec drift — ${problems.length} problem(s):\n` + problems.map((p) => '  - ' + p).join('\n'));
      process.exit(1);
    }
    console.log(`✓ api-spec in sync: ${spec.length} published of ${routeCount} real routes`);
    return;
  }

  const written = [...OUT_PATHS, ...EXTRA_MIRRORS].filter((p) => existsSync(join(ROOT, dirname(p))));
  for (const p of written) writeFileSync(join(ROOT, p), json);
  console.log(`✓ wrote ${spec.length} endpoints (${routeCount} real routes) to:\n` +
    written.map((p) => '  - ' + p).join('\n'));
  if (undocumented.length) console.warn(`! ${undocumented.length} undocumented (stub emitted): ${undocumented.join(', ')}`);
  if (stale.length) console.warn(`! ${stale.length} stale doc(s): ${stale.join(', ')}`);
}

// Run as a CLI only when executed directly, not when imported by validate.mjs.
if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) main();

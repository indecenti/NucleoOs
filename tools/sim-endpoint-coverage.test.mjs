// META guard: every endpoint the Control Center app talks to must be (a) mocked by the dev simulator
// so the browser preview can exercise it, and (b) registered by the firmware so the real device serves
// it. Fails loudly if an app endpoint is added without wiring both — keeps app / sim / device in lockstep
// (same spirit as the gen-api-spec --check gate).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { ENDPOINTS } from '../apps/settings/www/selftest.js';
import { SCENE_ENDPOINTS } from '../apps/settings/www/scenes.js';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const sim = readFileSync(join(REPO, 'tools', 'serve-shell.mjs'), 'utf8');

// Routes are registered across several firmware components (httpd, fsapi, auth, app, anima…), so scan
// the whole component tree for the route literals rather than a single file.
function readAllC(dir) {
  let out = '';
  for (const ent of readdirSync(dir, { withFileTypes: true })) {
    const p = join(dir, ent.name);
    if (ent.isDirectory()) out += readAllC(p);
    else if (/\.(c|cpp|h)$/.test(ent.name)) { try { out += readFileSync(p, 'utf8'); } catch {} }
  }
  return out;
}
const fw = readAllC(join(REPO, 'firmware', 'components'));

// Every read-only health endpoint + the live mutators the app/scenes drive.
const APP_ENDPOINTS = [
  ...new Set([
    ...ENDPOINTS.map((e) => e.path),
    ...SCENE_ENDPOINTS,
    '/api/reboot', '/api/fs/read', '/api/fs/write', '/api/fs/list',
  ]),
];

// The simulator matches routes with `path === '/x'` or `path.startsWith('/x')`. A path is considered
// mocked if either form (or the family prefix for /api/fs/*) appears in the source.
function simMocks(path) {
  if (sim.includes(`'${path}'`)) return true;
  if (path.startsWith('/api/fs/') && sim.includes(`path.startsWith('/api/fs/')`)) return true;
  return false;
}

test('the simulator mocks every endpoint the Control Center app uses', () => {
  const missing = APP_ENDPOINTS.filter((p) => !simMocks(p));
  assert.deepEqual(missing, [], 'unmocked in tools/serve-shell.mjs: ' + missing.join(', '));
});

test('the firmware registers every endpoint the Control Center app uses', () => {
  // Each route is registered in nucleo_httpd.c as `.uri = "/api/..."`.
  const missing = APP_ENDPOINTS.filter((p) => !fw.includes(`"${p}"`));
  assert.deepEqual(missing, [], 'unregistered in firmware nucleo_httpd.c: ' + missing.join(', '));
});

test('the self-test health set and scene endpoints are disjoint-or-known (no typos)', () => {
  for (const p of [...ENDPOINTS.map((e) => e.path), ...SCENE_ENDPOINTS]) {
    assert.match(p, /^\/api\//, 'looks like an API path: ' + p);
  }
});

// ── the WHOLE firmware surface ────────────────────────────────────────────────────────────────
// registry/web-api-spec.json is regenerated from the firmware's real route table (gen-api-spec --check
// keeps it honest). Every one of those routes must exist in the simulator too, or the browser E2E suite
// and the preview silently test a device that does not exist: an app calling an unmocked route gets a
// 404 in the simulator and a working answer on the Cardputer (or the reverse), and nobody finds out.
const spec = JSON.parse(readFileSync(join(REPO, 'registry', 'web-api-spec.json'), 'utf8'));
// Routes the simulator deliberately does NOT reproduce — each with the reason. Keep this list short.
const DEVICE_ONLY = {
  '/api/game/costellazioni/gentest': 'debug dump of the native C procedural generator; no JS twin to answer with',
};
function simHas(path) {
  if (sim.includes(`'${path}'`)) return true;
  const parts = path.split('/');
  for (let n = parts.length - 1; n >= 3; n--) if (sim.includes(`path.startsWith('${parts.slice(0, n).join('/')}/')`)) return true;
  return false;
}

test('the simulator mocks EVERY route the firmware registers (web-api-spec.json)', () => {
  const routes = [...new Set(spec.map((r) => r.path))].filter((p) => !/[*{]/.test(p));
  assert.ok(routes.length >= 60, `spec lists only ${routes.length} routes`);
  const missing = routes.filter((p) => !DEVICE_ONLY[p] && !simHas(p));
  assert.deepEqual(missing, [], 'firmware routes the simulator does not mock (add them to tools/serve-shell.mjs): ' + missing.join(', '));
});

test('the device-only exception list only names real firmware routes', () => {
  const routes = new Set(spec.map((r) => r.path));
  for (const p of Object.keys(DEVICE_ONLY)) assert.ok(routes.has(p), `stale DEVICE_ONLY entry: ${p}`);
});

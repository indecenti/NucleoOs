// anima-code-f5.test.mjs — F5 (docs/anima-code.md §8/§10): live install-and-smoke + app-recipe
// learning, both pure over injected deps. What is pinned:
//
//   SMOKE  A published app is proven to actually serve on the device: the route responds, the index
//          is real HTML, the installed manifest matches what we published. A launcher-accepted app
//          that 404s FAILS the smoke — a silent publish failure is caught, not celebrated. Every
//          check is GET-only and never throws out.
//   LEARN  A verified-and-smoke-PASSED app stages a recipe through the SAME distill gate the WebGPU
//          loop uses (certain+useful, provenance-linked, reversible). A FAILED smoke stages nothing.
//          Staging is idempotent (deterministic slug) and never auto-enters the shipped corpus.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { smokeApp, smokeSummary, appSpecText, stageAppRecipe } from '../../apps/agent/www/app-recipe.js';
import { distill } from '../../apps/anima/www/forge/learn.js';
import { canonical, sha256hex } from '../../apps/anima/www/forge/provenance.js';
const DEPS = { distill, canonical, sha256hex };

const HTML = '<!doctype html><html><body><main>Timer</main><script>1</script></body></html>';
const MANIFEST = { id: 'my-timer', name: 'Timer', version: '0.1.0', category: 'tools', permissions: [], entry_service: 'none', web_route: '/apps/my-timer/', handles: {}, power: {} };

// A fetch double serving a fake device: a map of url → {status, body}.
function fakeFetch(routes) {
  return async (url) => {
    const hit = routes[url];
    if (!hit) return { ok: false, status: 404, text: async () => 'no file' };
    return { ok: hit.status < 400, status: hit.status, text: async () => hit.body };
  };
}
// The device fs reader: /api/fs/read?path=<abs> → the file at that abs path. `files` maps abs→body.
function fakeFsFetch(files, apps = null) {
  return async (url) => {
    if (url.startsWith('/api/apps')) return { ok: true, status: 200, text: async () => JSON.stringify({ apps: apps || [] }) };
    const m = /[?&]path=([^&]+)/.exec(url); const abs = m ? decodeURIComponent(m[1]) : '';
    const body = files[abs];
    return body != null ? { ok: true, status: 200, text: async () => body } : { ok: false, status: 404, text: async () => 'no file' };
  };
}
const REG = (id) => [{ id, enabled: true }];

// ---- SMOKE ------------------------------------------------------------------------------------
test('a healthy app passes every smoke check', async () => {
  const fetchFn = fakeFetch({ '/apps/my-timer/': { status: 200, body: HTML } });
  const fsFetch = fakeFsFetch({ '/apps/my-timer/manifest.json': JSON.stringify(MANIFEST) }, REG('my-timer'));
  const r = await smokeApp('my-timer', { fetchFn, fsFetch });
  assert.equal(r.ok, true);
  assert.deepEqual(r.checks.map((c) => c.name).sort(), ['index', 'manifest', 'registered', 'route']);
});

test('a launcher-accepted app whose route 404s fails the smoke (silent-failure catch)', async () => {
  const r = await smokeApp('ghost', { fetchFn: fakeFetch({}) });
  assert.equal(r.ok, false);
  assert.equal(r.checks.find((c) => c.name === 'route').ok, false);
});

test('an index that is empty or an error page fails the index check', async () => {
  const fetchFn = fakeFetch({ '/apps/x/': { status: 200, body: 'oops' } });
  const fsFetch = fakeFsFetch({ '/apps/x/manifest.json': JSON.stringify({ id: 'x' }) }, REG('x'));
  const r = await smokeApp('x', { fetchFn, fsFetch });
  assert.equal(r.checks.find((c) => c.name === 'index').ok, false, 'a 4-byte body is not a real app');
});

test('a manifest whose id does not match the app fails the manifest check', async () => {
  const fetchFn = fakeFetch({ '/apps/y/': { status: 200, body: HTML } });
  const fsFetch = fakeFsFetch({ '/apps/y/manifest.json': JSON.stringify({ id: 'somethingelse' }) }, REG('y'));
  const r = await smokeApp('y', { fetchFn, fsFetch });
  assert.equal(r.checks.find((c) => c.name === 'manifest').ok, false);
});

test('expectManifest deep-compares: a tampered installed manifest is caught', async () => {
  const tampered = { ...MANIFEST, permissions: ['net.wifi'] };
  const fetchFn = fakeFetch({ '/apps/my-timer/': { status: 200, body: HTML } });
  const fsFetch = fakeFsFetch({ '/apps/my-timer/manifest.json': JSON.stringify(tampered) }, REG('my-timer'));
  const r = await smokeApp('my-timer', { fetchFn, fsFetch, expectManifest: MANIFEST, canonical, sha256hex });
  assert.equal(r.checks.find((c) => c.name === 'manifest').ok, false, 'installed differs from published');
});

test('a bad app id is rejected before any fetch', async () => {
  let touched = 0;
  const r = await smokeApp('../etc', { fetchFn: async () => { touched++; return { ok: true, status: 200, text: async () => '' }; } });
  assert.equal(r.ok, false);
  assert.equal(touched, 0);
});

test('an app the device registry does not list fails the registered check', async () => {
  const fetchFn = fakeFetch({ '/apps/z/': { status: 200, body: HTML } });
  const fsFetch = fakeFsFetch({ '/apps/z/manifest.json': JSON.stringify({ id: 'z' }) }, []);   // empty registry
  const r = await smokeApp('z', { fetchFn, fsFetch });
  assert.equal(r.checks.find((c) => c.name === 'registered').ok, false);
  assert.equal(r.ok, false);
});

test('smokeSummary reads clean in both outcomes', () => {
  assert.match(smokeSummary({ ok: true, checks: [1, 2, 3] }, (k, v) => k + ':' + JSON.stringify(v)), /smoke_ok/);
  assert.match(smokeSummary({ ok: false, checks: [{ name: 'route', ok: false }] }, (k, v) => k + ':' + JSON.stringify(v)), /smoke_fail/);
});

// ---- LEARN ------------------------------------------------------------------------------------
test('appSpecText describes an app by what it does, not its code', () => {
  assert.equal(appSpecText({ name: 'Timer', description: 'a countdown' }, 'timer'), 'Timer (timer) — a countdown');
  assert.equal(appSpecText({ name: 'Blank' }, ''), 'Blank');
});

test('a smoke-passed, approved app stages a recipe with provenance', async () => {
  const { staged, reason } = await stageAppRecipe(
    { manifest: MANIFEST, kind: 'timer', html: HTML, smoke: { ok: true }, approved: true, lang: 'it' },
    DEPS, { existingCards: [], stagedCards: [] });
  assert.ok(staged, 'expected a staged card, got reason=' + reason);
  assert.equal(staged.category, 'code-recipe');
  assert.ok(staged.provenance, 'must carry a provenance hash');
  assert.ok(staged.ask && (staged.ask.it.length || staged.ask.en.length), 'must carry recall phrasings');
});

test('a FAILED smoke stages nothing — the floor only learns what works', async () => {
  const { staged, reason } = await stageAppRecipe(
    { manifest: MANIFEST, kind: 'timer', html: HTML, smoke: { ok: false }, approved: true, lang: 'it' }, DEPS, {});
  assert.equal(staged, null);
  assert.match(reason, /not-certain:not-run/);
});

test('an un-approved app stages nothing (publish is human-gated)', async () => {
  const { staged } = await stageAppRecipe(
    { manifest: MANIFEST, kind: 'timer', html: HTML, smoke: { ok: true }, approved: false, lang: 'it' }, DEPS, {});
  assert.equal(staged, null);
});

test('re-publishing the same app yields the same recipe id (idempotent staging)', async () => {
  const turn = { manifest: MANIFEST, kind: 'timer', html: HTML, smoke: { ok: true }, approved: true, lang: 'it' };
  const a = await stageAppRecipe(turn, DEPS, {});
  const b = await stageAppRecipe(turn, DEPS, {});
  assert.equal(a.staged.id, b.staged.id, 'deterministic slug → no duplicate cards on re-publish');
});

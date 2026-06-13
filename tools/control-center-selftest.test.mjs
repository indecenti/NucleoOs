// Unit tests for the device self-test sweep (apps/settings/www/selftest.js).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ENDPOINTS, runSelfTest, formatReport } from '../apps/settings/www/selftest.js';

// A fake fetch driven by a {path -> {status, json?|text?}} map.
function fakeFetch(map) {
  const seen = [];
  const fn = async (path) => {
    seen.push(path);
    const e = map[path];
    if (!e) return { ok: false, status: 404, json: async () => ({}), text: async () => '' };
    return { ok: e.status >= 200 && e.status < 300, status: e.status,
      json: async () => e.json ?? {}, text: async () => e.text ?? '' };
  };
  fn.seen = seen;
  return fn;
}

// Healthy responses for every endpoint (used as the baseline; individual tests break one at a time).
function healthyMap() {
  return {
    '/api/status': { status: 200, json: { os: 'NucleoOS', network: { ip: '1.2.3.4' } } },
    '/api/heap': { status: 200, json: { internal: { free_bytes: 70000, frag_pct: 20 } } },
    '/api/cpu': { status: 200, json: { load: [10, 12], load_avg: 11 } },
    '/api/wifi/scan': { status: 200, json: { networks: [] } },
    '/api/logs': { status: 200, text: 'I (1) boot\n' },
    '/api/anima/caps': { status: 200, json: { l1Mode: 'auto', l1Serving: false } },
    '/api/apps': { status: 200, json: { apps: [{ id: 'x' }] } },
    '/api/auth/status': { status: 200, json: { paired: true } },
  };
}

test('ENDPOINTS covers exactly the expected read-only health set (drift guard)', () => {
  const expected = ['/api/status', '/api/heap', '/api/cpu', '/api/wifi/scan', '/api/logs', '/api/anima/caps', '/api/apps', '/api/auth/status'];
  const got = ENDPOINTS.map((e) => e.path).sort();
  assert.deepEqual(got, expected.slice().sort());
  for (const e of ENDPOINTS) { assert.ok(e.it && e.en, 'bilingual label: ' + e.path); assert.equal(typeof e.check, 'function'); }
});

test('all healthy → every check ok, none failed', async () => {
  const res = await runSelfTest(fakeFetch(healthyMap()));
  assert.equal(res.length, ENDPOINTS.length);
  assert.ok(res.every((r) => r.ok), 'all ok');
  assert.ok(res.every((r) => !r.warn), 'no warnings on healthy heap');
});

test('a 404 marks that endpoint failed but does not abort the sweep', async () => {
  const map = healthyMap(); map['/api/cpu'] = { status: 404 };
  const res = await runSelfTest(fakeFetch(map));
  const cpu = res.find((r) => r.key === 'cpu');
  assert.equal(cpu.ok, false);
  assert.equal(cpu.status, 404);
  assert.equal(res.length, ENDPOINTS.length, 'all endpoints still probed');
});

test('valid HTTP 200 but wrong shape fails the check (cpu without load[])', async () => {
  const map = healthyMap(); map['/api/cpu'] = { status: 200, json: { cores: 2 } };   // no load[]
  const res = await runSelfTest(fakeFetch(map));
  const cpu = res.find((r) => r.key === 'cpu');
  assert.equal(cpu.ok, false);
  assert.equal(cpu.shapeOk, false);
});

test('heap shape requires internal.free_bytes (number)', async () => {
  const map = healthyMap(); map['/api/heap'] = { status: 200, json: { internal: {} } };
  const res = await runSelfTest(fakeFetch(map));
  assert.equal(res.find((r) => r.key === 'heap').ok, false);
});

test('logs are validated as text, not JSON', async () => {
  const f = fakeFetch(healthyMap());
  await runSelfTest(f);
  // the logs endpoint must have been read; selftest declares it kind:text → .text() path
  const logsEp = ENDPOINTS.find((e) => e.key === 'logs');
  assert.equal(logsEp.kind, 'text');
  assert.equal(logsEp.check('plain string'), true);
  assert.equal(logsEp.check({ not: 'text' }), false);
});

test('heap warns (amber) when free internal SRAM is tight', async () => {
  const map = healthyMap(); map['/api/heap'] = { status: 200, json: { internal: { free_bytes: 9000, frag_pct: 60 } } };
  const res = await runSelfTest(fakeFetch(map));
  const heap = res.find((r) => r.key === 'heap');
  assert.equal(heap.ok, true);
  assert.equal(heap.warn, true);
});

test('the sweep is sequential (one request at a time, in ENDPOINTS order)', async () => {
  const f = fakeFetch(healthyMap());
  await runSelfTest(f);
  assert.deepEqual(f.seen, ENDPOINTS.map((e) => e.path));
});

test('formatReport produces a copyable verdict with a per-line mark', () => {
  const results = [
    { key: 'status', path: '/api/status', ok: true, warn: false, status: 200, label: 'Stato' },
    { key: 'cpu', path: '/api/cpu', ok: false, warn: false, status: 404, label: 'CPU' },
  ];
  const rep = formatReport(results, 'it');
  assert.ok(rep.includes('1/2'));
  assert.ok(rep.includes('✓'));
  assert.ok(rep.includes('✗'));
});

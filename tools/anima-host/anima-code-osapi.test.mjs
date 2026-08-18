// anima-code-osapi.test.mjs — the F1 host gate (docs/anima-code.md §10 F1): the agent stops
// GUESSING the NucleoOS contract. What is pinned:
//
//   1. get_os_api's slicers are BOUNDED — the 112 KB spec can never reach a model whole — and they
//      find real routes by path or keyword, in both languages.
//   2. The manifest digest carries the three things a generated app gets wrong when guessed:
//      required fields, the category enum, the permission enum.
//   3. The deploy rules name the silent footguns (www/ invisible, .gz shadowing, enabled:true).
//   4. The device starter kind is a full citizen: valid manifest WITH the system.events permission
//      the broker demands, i18n in both languages, and a body that reads os.sys.status —
//      the proof that a generated app integrates the Cardputer instead of a generic web page.
//   5. Broker sys.status returns the REDUCED shape (no IP, no partitions) and stays serialised.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { osApiIndex, osApiRoute, osApiManifest, OSAPI_RULES, CLIENT_TOOLS } from '../../apps/agent/www/agent-tools.js';
import { buildManifest, validateManifest, starterHtml, starterI18n, APP_KINDS } from '../../apps/agent/www/app-publish.js';
import { createBroker } from '../../web/shell/appbroker.js';

const spec = JSON.parse(readFileSync(new URL('../../registry/web-api-spec.json', import.meta.url), 'utf8'));
const schema = JSON.parse(readFileSync(new URL('../../schemas/manifest.schema.json', import.meta.url), 'utf8'));

// ---- 1. bounded, and it finds things ----------------------------------------------------------
test('the route index covers the API surface and stays bounded', () => {
  const idx = osApiIndex(spec, 'en');
  assert.ok(idx.length <= 4000, 'index must be capped: ' + idx.length);
  for (const must of ['/api/status', '/api/fs/read', '/api/anima']) assert.match(idx, new RegExp(must));
});

test('a route is found by exact path, by fragment, and by keyword — in either language', () => {
  for (const q of ['/api/status', 'stato dispositivo']) {
    const doc = osApiRoute(spec, q, q === '/api/status' ? 'en' : 'it');
    assert.ok(doc, 'no doc for ' + q);
    assert.match(doc, /GET \/api\/status\b/);
    assert.ok(doc.length <= 4000, 'route doc capped');
  }
  assert.equal(osApiRoute(spec, 'a-route-that-does-not-exist'), null);
});

// ---- 2+3. manifest digest and deploy rules ----------------------------------------------------
test('the manifest digest names required fields, categories and permissions', () => {
  const dig = osApiManifest(schema);
  assert.ok(dig);
  for (const must of ['required fields', 'category', 'permissions', 'system.events', 'REJECTED']) {
    assert.ok(dig.includes(must), 'digest missing: ' + must);
  }
});

test('the deploy rules name the silent footguns', () => {
  for (const must of ['www/', '.gz', 'enabled:true', 'postMessage']) assert.ok(OSAPI_RULES.includes(must), 'rules missing: ' + must);
});

test('get_os_api is on the ONE shared tool surface', () => {
  const tool = CLIENT_TOOLS.find((t) => t.name === 'get_os_api');
  assert.ok(tool, 'tool missing');
  assert.deepEqual(tool.input_schema.properties.topic.enum, ['routes', 'route', 'manifest', 'rules']);
});

// ---- 4. the device starter kind ----------------------------------------------------------------
test('the device kind ships with the permission its broker call requires', () => {
  assert.ok(APP_KINDS.includes('device'));
  const m = buildManifest({ name: 'Stato', kind: 'device', category: 'system' });
  assert.deepEqual(m.permissions, ['system.events']);
  assert.equal(validateManifest(m).ok, true);
  // other kinds must NOT silently gain it
  assert.deepEqual(buildManifest({ name: 'X', kind: 'list' }).permissions, []);
});

test('the device starter reads os.sys.status and is bilingual', () => {
  const html = starterHtml({ id: 'stato', name: 'Stato', kind: 'device' });
  assert.match(html, /os\.sys\.status/);
  assert.match(html, /data-i18n="uptime"/);
  for (const en of [false, true]) {
    const cat = JSON.parse(starterI18n({ name: 'Stato', kind: 'device' }, en));
    for (const k of ['uptime', 'sd', 'net', 'refresh', 'offline']) assert.ok(cat[k], (en ? 'en' : 'it') + ' missing ' + k);
  }
});

// ---- 5. broker sys.status: gated, reduced, honest ---------------------------------------------
function brokerWith(fetchImpl, perms = ['system.events']) {
  const app = { id: 'x', permissions: perms, name: 'X' };
  const onMessage = createBroker({ fetchFn: fetchImpl, findApp: () => app });
  return (method) => new Promise((resolve) => {
    onMessage({ data: { type: 'nucleo.broker', method, id: 1 },
      source: { postMessage: (m) => resolve(m) } });
  });
}

test('sys.status returns the reduced shape — clock, uptime, SD, network name; never the IP', async () => {
  const full = { os: 'NucleoOS', version: '1', time: '2026-08-18T10:00:00', uptime_s: 3600,
    storage: { mounted: true, free_bytes: 5, total_bytes: 10, mount: '/sd' },
    wifi: { mode: 'sta', ssid: 'casa', ip: '192.168.0.9', rssi: -60 }, ota: { slot: 'a' }, heap: 12345 };
  const call = brokerWith(async () => ({ ok: true, json: async () => full }));
  const r = await call('sys.status');
  assert.equal(r.ok, true);
  assert.equal(r.status.wifi.ssid, 'casa');
  assert.equal(r.status.wifi.ip, undefined, 'the IP must not cross the sandbox');
  assert.equal(r.status.ota, undefined, 'partitions must not cross');
  assert.equal(r.status.heap, undefined);
});

test('sys.status without the permission is denied before any fetch', async () => {
  let fetched = 0;
  const call = brokerWith(async () => { fetched++; return { ok: true, json: async () => ({}) }; }, ['storage.app']);
  const r = await call('sys.status');
  assert.equal(r.ok, false);
  assert.match(r.error, /denied/);
  assert.equal(fetched, 0);
});

test('a device error surfaces honestly, not as fabricated status', async () => {
  const call = brokerWith(async () => ({ ok: false, status: 503 }));
  const r = await call('sys.status');
  assert.equal(r.ok, false);
  assert.equal(r.error, 'http-503');
});

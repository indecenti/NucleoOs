// Unit test for the pure offline-resolution policy (apps/anima/www/local/cascade.js).
//
// No WASM, no network, no browser — the policy is exercised with mocked tier runners so the
// guarantee the user asked for ("the in-browser WASM brain is tried BEFORE we scale onto the
// Cardputer") is pinned and can't regress. Run:  node --test apps/anima/local/cascade.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const mod = join(here, '..', 'www', 'local', 'cascade.js');
const { answered, resolveOffline } = await import(pathToFileURL(mod).href);

// A runner factory that records call order into `log` and returns a fixed result.
const rec = (log, name, result) => async () => { log.push(name); return result; };
const ans = (extra = {}) => ({ reply: 'x', tier: 'fact', ...extra });   // a real answer

test('answered(): tier-agnostic predicate', () => {
  assert.equal(answered(null), false);
  assert.equal(answered({ reply: '' }), false, 'empty reply -> abstention');
  assert.equal(answered({ reply: 'x', tier: 'none', action: 'none', domain: 'none' }), false, 'no signal -> abstention');
  assert.equal(answered({ reply: 'x', tier: 'fact' }), true);
  assert.equal(answered({ reply: 'x', tier: 'none', action: 'launch' }), true, 'action counts');
  // STITCH/L2: shaper drops tier to 'none' but the reply + domain are real -> must still be "answered".
  assert.equal(answered({ reply: 'la fotosintesi è…', tier: 'none', domain: 'knowledge' }), true, 'STITCH-as-answer');
});

test('ordering: browser is tried before the device', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', ans({ local: true })),
    device: rec(log, 'device', ans()),
  });
  assert.deepEqual(log, ['browser'], 'device must not be called once the browser answered');
  assert.equal(r.local, true, 'the browser result is returned');
});

test('abstain passthrough: browser abstains -> device answers', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', { reply: '' }),            // abstention
    device: rec(log, 'device', ans({ source: 'device' })),
  });
  assert.deepEqual(log, ['browser', 'device']);
  assert.equal(r.source, 'device');
});

test('STITCH-as-answer is returned, not skipped', async () => {
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: async () => ({ reply: 'la fotosintesi è…', tier: 'none', domain: 'knowledge', local: true }),
    device: async () => ans(),
  });
  assert.equal(r.local, true);
  assert.match(r.reply, /fotosintesi/);
});

test('silent skip: an unprovisioned browser brain is never called', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),     // would answer, but...
    device: rec(log, 'device', ans({ source: 'device' })),
    browserProvisioned: async () => false,                  // ...not provisioned
  });
  assert.deepEqual(log, ['device'], 'browser runner must not run when unprovisioned + silent');
  assert.equal(r.source, 'device');
});

test('silent + provisioned: the browser IS used', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),
    device: rec(log, 'device', ans({ source: 'device' })),
    browserProvisioned: async () => true,
  });
  assert.deepEqual(log, ['browser']);
  assert.equal(r.local, true);
});

test('device-first: prefer device, browser as fallback', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'device', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),
    device: rec(log, 'device', { reply: '' }),              // device abstains
    browserProvisioned: async () => true,
  });
  assert.deepEqual(log, ['device', 'browser'], 'device tried first, then browser');
  assert.equal(r.local, true);
});

test('null when nothing answers', async () => {
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: async () => ({ reply: '' }),
    device: async () => null,
  });
  assert.equal(r, null);
});

test('a throwing runner is treated as an abstention (never throws)', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: async () => { log.push('browser'); throw new Error('boom'); },
    device: rec(log, 'device', ans({ source: 'device' })),
  });
  assert.deepEqual(log, ['browser', 'device']);
  assert.equal(r.source, 'device');
});

test('a throwing browserProvisioned() is treated as not-provisioned', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),
    device: rec(log, 'device', ans({ source: 'device' })),
    browserProvisioned: async () => { throw new Error('boom'); },
  });
  assert.deepEqual(log, ['device']);
  assert.equal(r.source, 'device');
});

// ---- web-index tier: browser WASM brain -> web index -> device ------------------------------------
test('webindex sits between the browser brain and the device', async () => {
  const log = [];
  const r = await resolveOffline('chi è X', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', { reply: '' }),                 // WASM brain abstains
    webindex: rec(log, 'webindex', ans({ web: true, local: true, intent: 'wikipedia' })),
    device: rec(log, 'device', ans({ source: 'device' })),
  });
  assert.deepEqual(log, ['browser', 'webindex'], 'web index tried after the brain, before the device');
  assert.equal(r.web, true);
});

test('webindex is NOT silent-gated (runs even when the WASM brain is unprovisioned)', async () => {
  const log = [];
  const r = await resolveOffline('chi è X', 'it', { prefer: 'browser', silent: true }, {
    browser: rec(log, 'browser', ans({ local: true })),         // would answer, but unprovisioned -> skipped
    webindex: rec(log, 'webindex', ans({ web: true, local: true })),
    device: rec(log, 'device', ans({ source: 'device' })),
    browserProvisioned: async () => false,
  });
  assert.deepEqual(log, ['webindex'], 'unprovisioned brain skipped; web index still runs (lightweight)');
  assert.equal(r.web, true);
});

test('webindex abstains -> device answers', async () => {
  const log = [];
  const r = await resolveOffline('chi è X', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', { reply: '' }),
    webindex: rec(log, 'webindex', null),                        // no web answer (offline / no match)
    device: rec(log, 'device', ans({ source: 'device' })),
  });
  assert.deepEqual(log, ['browser', 'webindex', 'device']);
  assert.equal(r.source, 'device');
});

test('device-first order: device, then browser, then webindex', async () => {
  const log = [];
  const r = await resolveOffline('chi è X', 'it', { prefer: 'device', silent: true }, {
    browser: rec(log, 'browser', { reply: '' }),
    webindex: rec(log, 'webindex', ans({ web: true })),
    device: rec(log, 'device', { reply: '' }),                  // device abstains
    browserProvisioned: async () => true,
  });
  assert.deepEqual(log, ['device', 'browser', 'webindex']);
  assert.equal(r.web, true);
});

test('missing webindex runner is simply skipped (back-compat)', async () => {
  const log = [];
  const r = await resolveOffline('q', 'it', { prefer: 'browser', silent: false }, {
    browser: rec(log, 'browser', { reply: '' }),
    device: rec(log, 'device', ans({ source: 'device' })),       // no webindex runner provided
  });
  assert.deepEqual(log, ['browser', 'device']);
  assert.equal(r.source, 'device');
});

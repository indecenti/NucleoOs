// The capability broker's policy (web/shell/appbroker.js) — the /api/* half of the app boundary.
//
// Context: every app runs in a same-origin iframe, so the pairing cookie rides along on its fetches
// and it can call ANY /api/* route. An app declaring `storage.app` could read /data/anima/teacher.json
// and take the user's Claude, Groq and xAI keys. A service worker cannot police this: on the device
// the OS is served over plain http, which is not a secure context, so no service worker exists there.
// Apps written by the agent therefore lose their origin and reach the OS only through this broker.
//
// These tests are the fence. They are written adversarially on purpose: the app on the other side of
// this boundary was written by a model.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { normPath, resolveAppPath, methodAllowed, METHODS, createBroker } from '../web/shell/appbroker.js';

const APP = ['storage.app'];
const SHARED = ['storage.shared'];

test('normPath resolves .. without ever climbing past the root', () => {
  assert.equal(normPath('/data/apps/x/../y'), '/data/apps/y');
  assert.equal(normPath('/../../../etc/passwd'), '/etc/passwd');   // clamped, not escaped
  assert.equal(normPath('/data//x/./y'), '/data/x/y');
  assert.equal(normPath(''), '/');
});

test('storage.app reaches its OWN folder and nothing else', () => {
  assert.equal(resolveAppPath('todo', APP, '/data/apps/todo/items.json'), '/data/apps/todo/items.json');
  assert.equal(resolveAppPath('todo', APP, '/data/apps/other/items.json'), null, 'another app is off limits');
  assert.equal(resolveAppPath('todo', APP, '/data/Documents/a.txt'), null, 'shared area needs storage.shared');
});

test('traversal cannot buy reach the manifest did not grant', () => {
  for (const p of ['/data/apps/todo/../other/x', '/data/apps/todo/../../anima/teacher.json',
                   '/data/apps/todo/../../../system/config/settings.json', '..%2f..%2fetc']) {
    assert.equal(resolveAppPath('todo', APP, p), null, 'must refuse: ' + p);
  }
});

test('THE ONE THAT MATTERS: the key vault is unreachable, with either permission', () => {
  for (const perms of [APP, SHARED, ['storage.app', 'storage.shared']]) {
    assert.equal(resolveAppPath('x', perms, '/data/anima/teacher.json'), null);
    assert.equal(resolveAppPath('x', perms, '/data/anima/'), null);
    assert.equal(resolveAppPath('x', perms, '/data/agent/workspace/notes.md'), null);
  }
});

test('storage.shared reaches the user area but never the system', () => {
  assert.equal(resolveAppPath('x', SHARED, '/data/Documents/a.txt'), '/data/Documents/a.txt');
  for (const p of ['/system/config/settings.json', '/apps/anima/www/index.html', '/www/shell/shell.js', '/']) {
    assert.equal(resolveAppPath('x', SHARED, p), null, 'must refuse: ' + p);
  }
});

test('an app with no storage permission reaches nothing at all', () => {
  for (const p of ['/data/apps/x/a', '/data/b', '/system/c']) {
    assert.equal(resolveAppPath('x', ['system.notify'], p), null);
  }
});

test('a forged app id cannot widen the root', () => {
  assert.equal(resolveAppPath('../anima', APP, '/data/anima/teacher.json'), null);
  assert.equal(resolveAppPath('', APP, '/data/apps//x'), null);
});

test('the method vocabulary is a closed set, gated by declaration', () => {
  assert.deepEqual(METHODS, ['fs.read', 'fs.write', 'fs.list', 'notify', 'sys.info']);
  assert.equal(methodAllowed('fs.read', APP), true);
  assert.equal(methodAllowed('notify', APP), false, 'notify needs system.notify');
  assert.equal(methodAllowed('notify', ['system.notify']), true);
  for (const m of ['fs.remove', 'http.get', 'os.hw.ir.send', 'eval', '__proto__', '']) {
    assert.equal(methodAllowed(m, ['storage.app', 'storage.shared', 'system.notify']), false, 'must refuse: ' + m);
  }
});

// ── the message surface ───────────────────────────────────────────────────────────────────────
function harness(app, fetchFn) {
  const sent = [];
  const source = { postMessage: (m) => sent.push(m) };
  const handler = createBroker({ fetchFn, findApp: (s) => (s === source && app ? app : null) });
  const call = async (method, args) => {
    sent.length = 0;
    await handler({ data: { type: 'nucleo.broker', id: 1, method, args }, source, origin: 'null' });
    return sent[0];
  };
  return { call, sent };
}

test('an unknown caller is refused — identity is the source, never the origin', async () => {
  const h = harness(null, async () => { throw new Error('must not fetch'); });
  assert.deepEqual(await h.call('fs.read', { path: '/data/apps/x/a' }), { type: 'nucleo.broker.reply', id: 1, ok: false, error: 'unknown caller' });
});

test('a denied path never reaches the network', async () => {
  let touched = false;
  const h = harness({ id: 'todo', permissions: APP }, async () => { touched = true; return { ok: true, text: async () => '' }; });
  const r = await h.call('fs.read', { path: '/data/anima/teacher.json' });
  assert.equal(r.ok, false);
  assert.equal(touched, false, 'the device must not even be asked');
});

test('an allowed read is served, and only the declared path is fetched', async () => {
  let url = null;
  const h = harness({ id: 'todo', permissions: APP }, async (u) => { url = u; return { ok: true, text: async () => '[1,2]' }; });
  const r = await h.call('fs.read', { path: '/data/apps/todo/items.json' });
  assert.equal(r.ok, true);
  assert.equal(r.content, '[1,2]');
  assert.ok(url.includes(encodeURIComponent('/data/apps/todo/items.json')), url);
});

test('writes are bounded — an app cannot hand the device megabytes', async () => {
  const h = harness({ id: 'todo', permissions: APP }, async () => ({ ok: true, text: async () => '' }));
  const r = await h.call('fs.write', { path: '/data/apps/todo/big', content: 'x'.repeat(300 * 1024) });
  assert.equal(r.ok, false);
  assert.match(r.error, /too large/);
});

test('brokered calls are SERIALISED — the device never sees two at once', async () => {
  let live = 0, peak = 0;
  const fetchFn = async () => {
    live++; peak = Math.max(peak, live);
    await new Promise((r) => setTimeout(r, 5));
    live--; return { ok: true, text: async () => 'x' };
  };
  const source = { postMessage() {} };
  const handler = createBroker({ fetchFn, findApp: () => ({ id: 'todo', permissions: APP }) });
  await Promise.all([1, 2, 3, 4].map((id) =>
    handler({ data: { type: 'nucleo.broker', id, method: 'fs.read', args: { path: '/data/apps/todo/a' } }, source })));
  assert.equal(peak, 1, 'the whole point: N sandboxed apps cannot burst the 4-6 sockets');
});

test('a message that is not ours is ignored entirely', async () => {
  const h = harness({ id: 'todo', permissions: APP }, async () => ({ ok: true, text: async () => '' }));
  h.sent.length = 0;
  const handler = createBroker({ findApp: () => ({ id: 'todo', permissions: APP }) });
  await handler({ data: { type: 'open-app', id: 'calculator' }, source: { postMessage: () => { throw new Error('replied to a foreign message'); } } });
});

test('sys.info is the one method that needs no declaration — and discloses only the language', async () => {
  // A sandboxed app cannot import /nucleo-i18n.js, so without this it would ship monolingual into a
  // five-language OS. It returns the display language the user already chose and can see.
  assert.equal(methodAllowed('sys.info', []), true);
  const source = { postMessage() {} };
  const sent = [];
  const handler = createBroker({ findApp: () => ({ id: 'x', permissions: [] }), getLang: () => 'de' });
  const src = { postMessage: (m) => sent.push(m) };
  const h2 = createBroker({ findApp: (s) => (s === src ? { id: 'x', permissions: [] } : null), getLang: () => 'de' });
  await h2({ data: { type: 'nucleo.broker', id: 9, method: 'sys.info', args: {} }, source: src });
  assert.deepEqual(sent[0], { type: 'nucleo.broker.reply', id: 9, ok: true, lang: 'de' });
});

test('sys.info does not become a hole: it exposes nothing else', async () => {
  const sent = [];
  const src = { postMessage: (m) => sent.push(m) };
  const h = createBroker({ findApp: (s) => (s === src ? { id: 'x', permissions: ['storage.shared'] } : null), getLang: () => 'it' });
  await h({ data: { type: 'nucleo.broker', id: 1, method: 'sys.info', args: {} }, source: src });
  assert.deepEqual(Object.keys(sent[0]).sort(), ['id', 'lang', 'ok', 'type']);
});

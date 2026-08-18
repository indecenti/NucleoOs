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
  assert.deepEqual(METHODS, ['fs.read', 'fs.write', 'fs.list', 'notify', 'sys.info', 'ai.ask', 'ai.complete']);
  assert.equal(methodAllowed('fs.read', APP), true);
  assert.equal(methodAllowed('notify', APP), false, 'notify needs system.notify');
  assert.equal(methodAllowed('notify', ['system.notify']), true);
  for (const m of ['fs.remove', 'http.get', 'os.hw.ir.send', 'eval', '__proto__', '']) {
    assert.equal(methodAllowed(m, ['storage.app', 'storage.shared', 'system.notify', 'ai.anima', 'ai.cloud']), false, 'must refuse: ' + m);
  }
});

test('ai.* is gated by its OWN permission — and buys no other reach', () => {
  assert.equal(methodAllowed('ai.ask', ['ai.anima']), true);
  assert.equal(methodAllowed('ai.ask', ['ai.cloud']), false, 'the two tiers do not imply each other');
  assert.equal(methodAllowed('ai.complete', ['ai.cloud']), true);
  assert.equal(methodAllowed('ai.complete', ['ai.anima']), false);
  for (const m of ['ai.ask', 'ai.complete']) {
    assert.equal(methodAllowed(m, ['storage.app', 'storage.shared', 'system.notify']), false, 'storage/notify must not buy ' + m);
  }
  // and the other way round: an ai permission opens no filesystem door
  assert.equal(methodAllowed('fs.read', ['ai.anima', 'ai.cloud']), false);
  assert.equal(resolveAppPath('x', ['ai.anima', 'ai.cloud'], '/data/apps/x/a'), null);
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

// ── ai.* — intelligence as a syscall. The app on the other side is machine-written; these tests
// are the fence around the two things it must never buy: OS effects, and the user's money. ─────
function aiHarness(perms, { fetchFn, aiComplete, aiCooldownMs } = {}) {
  const sent = [];
  const source = { postMessage: (m) => sent.push(m) };
  const handler = createBroker({
    fetchFn, aiComplete, aiCooldownMs,
    findApp: (s) => (s === source ? { id: 'todo', permissions: perms } : null),
    getLang: () => 'it',
  });
  const call = async (method, args) => {
    sent.length = 0;
    await handler({ data: { type: 'nucleo.broker', id: 1, method, args }, source, origin: 'null' });
    return sent[0];
  };
  return { call, sent };
}

test('ai.ask without ai.anima never reaches the device', async () => {
  let touched = false;
  const h = aiHarness(['storage.app', 'storage.shared', 'system.notify'], { fetchFn: async () => { touched = true; } });
  const r = await h.call('ai.ask', { q: 'che ore sono' });
  assert.equal(r.ok, false);
  assert.match(r.error, /denied/);
  assert.equal(touched, false);
});

test('ai.ask asks the OFFLINE engine, with the query bounded and encoded', async () => {
  let url = null;
  const h = aiHarness(['ai.anima'], { fetchFn: async (u) => { url = u; return { ok: true, json: async () => ({ reply: 'Roma', intent: 'kb' }) }; } });
  const r = await h.call('ai.ask', { q: 'capitale d\'Italia?' });
  assert.deepEqual(r, { type: 'nucleo.broker.reply', id: 1, ok: true, reply: 'Roma', intent: 'kb' });
  assert.ok(url.includes('/api/anima?q=' + encodeURIComponent('capitale d\'Italia?')), url);
  assert.ok(url.includes('mode=off'), 'a sandboxed app must not make the device network: ' + url);
  assert.ok(url.includes('lang=it'), url);
  // bounds
  assert.equal((await h.call('ai.ask', { q: '' })).ok, false);
  assert.equal((await h.call('ai.ask', { q: 'x'.repeat(300) })).ok, false, 'query over 256 chars must be refused');
});

test('THE ONE THAT MATTERS for ai.ask: OS effects are stripped before re-entering the sandbox', async () => {
  // /api/anima replies can carry {action,tool,arg} — copilot dispatch payloads. Handing those to
  // machine-written code would let a generated app drive the OS. Only text may cross back.
  const h = aiHarness(['ai.anima'], { fetchFn: async () => ({ ok: true, json: async () => ({ reply: 'ok', intent: 'cmd', action: 'open-app', tool: 'fs_write', arg: '/system/x' }) }) });
  const r = await h.call('ai.ask', { q: 'apri le impostazioni' });
  assert.equal(r.ok, true);
  assert.deepEqual(Object.keys(r).sort(), ['id', 'intent', 'ok', 'reply', 'type'], 'nothing beyond text may cross: ' + JSON.stringify(r));
});

test('ai.ask rides the SAME device chain as fs — never two device calls at once', async () => {
  let live = 0, peak = 0;
  const fetchFn = async () => {
    live++; peak = Math.max(peak, live);
    await new Promise((r) => setTimeout(r, 5));
    live--; return { ok: true, text: async () => 'x', json: async () => ({ reply: 'x' }) };
  };
  const source = { postMessage() {} };
  const handler = createBroker({ fetchFn, findApp: () => ({ id: 'todo', permissions: ['storage.app', 'ai.anima'] }) });
  await Promise.all([
    handler({ data: { type: 'nucleo.broker', id: 1, method: 'fs.read', args: { path: '/data/apps/todo/a' } }, source }),
    handler({ data: { type: 'nucleo.broker', id: 2, method: 'ai.ask', args: { q: 'ciao' } }, source }),
    handler({ data: { type: 'nucleo.broker', id: 3, method: 'fs.read', args: { path: '/data/apps/todo/b' } }, source }),
  ]);
  assert.equal(peak, 1, 'ai.ask is a device call and must respect the one-door discipline');
});

test('ai.complete needs ai.cloud, a configured brain, and returns only text', async () => {
  const denied = aiHarness(['ai.anima'], { aiComplete: async () => 'must not run' });
  assert.match((await denied.call('ai.complete', { prompt: 'hi' })).error, /denied/);
  const noBrain = aiHarness(['ai.cloud'], {});          // no key configured → honest refusal
  assert.equal((await noBrain.call('ai.complete', { prompt: 'hi' })).error, 'no-ai');
  const h = aiHarness(['ai.cloud'], { aiComplete: async (p) => 'echo:' + p, aiCooldownMs: 0 });
  const r = await h.call('ai.complete', { prompt: 'hello' });
  assert.deepEqual(r, { type: 'nucleo.broker.reply', id: 1, ok: true, text: 'echo:hello' });
  assert.equal((await h.call('ai.complete', { prompt: '' })).ok, false);
  assert.equal((await h.call('ai.complete', { prompt: 'x'.repeat(5000) })).ok, false, 'prompt over 4 KB must be refused');
});

test('ai.complete spends the user\'s key: single-flight AND cooled down, refusals immediate', async () => {
  let calls = 0;
  const slow = async () => { calls++; await new Promise((r) => setTimeout(r, 20)); return 'ok'; };
  const sent = [];
  const src2 = { postMessage: (m) => sent.push(m) };
  const h2 = createBroker({ aiComplete: slow, aiCooldownMs: 0, findApp: (s) => (s === src2 ? { id: 'x', permissions: ['ai.cloud'] } : null) });
  await Promise.all([
    h2({ data: { type: 'nucleo.broker', id: 1, method: 'ai.complete', args: { prompt: 'a' } }, source: src2 }),
    h2({ data: { type: 'nucleo.broker', id: 2, method: 'ai.complete', args: { prompt: 'b' } }, source: src2 }),
  ]);
  const errors = sent.filter((m) => !m.ok);
  assert.equal(errors.length, 1, 'exactly one of two concurrent calls must be refused: ' + JSON.stringify(sent));
  assert.equal(errors[0].error, 'busy');
  assert.equal(calls, 1, 'the refused call must not have spent anything');
  // cooldown: with a real cooldown, an immediate sequential retry is refused without spending
  const cool = aiHarness(['ai.cloud'], { aiComplete: async () => 'ok', aiCooldownMs: 60000 });
  assert.equal((await cool.call('ai.complete', { prompt: 'a' })).ok, true);
  const again = await cool.call('ai.complete', { prompt: 'b' });
  assert.equal(again.ok, false);
  assert.equal(again.error, 'rate-limited');
});

test('a huge completion is truncated before it re-enters the sandbox', async () => {
  const h = aiHarness(['ai.cloud'], { aiComplete: async () => 'y'.repeat(50000), aiCooldownMs: 0 });
  const r = await h.call('ai.complete', { prompt: 'go' });
  assert.equal(r.ok, true);
  assert.equal(r.text.length, 4000);
});

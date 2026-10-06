// web/shell/micgate.js — the OS-wide microphone gate, run for real inside a node:vm with a scripted Web Locks
// API and a scripted device (/api/rec/*). Every refusal path ("the mic is busy", a 409 from the firmware,
// a stream that gives up) names the current holder through activeMicLabel(); v149 deleted that helper as a
// "dead export" while four call sites still used it, so every busy/error path threw a ReferenceError: the
// Recorder never left "recording…", Dictation / ANIMA voice / Metronome stayed "listening", and
// onMicStatus() subscribers were never told anything.
//
//   node --test tools/micgate.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const SRC = readFileSync(join(REPO, 'web', 'shell', 'micgate.js'), 'utf8');

// Load the module as a strict script: drop the `export` keywords so its functions land on the context.
// `locks`: a Web Locks stand-in; `held` = the mic is taken elsewhere (ifAvailable gets a null lock).
// `net(path, init)`: plays the device.
function loadMicgate({ held = false, net = async () => new Response('{}') } = {}) {
  const fetches = [];
  const locks = {
    async request(name, opts, cb) { return cb(held && opts && opts.ifAvailable ? null : { name }); },
    async query() { return { held: held ? [{ name: 'nucleo-mic' }] : [] }; },
  };
  const ctx = vm.createContext({
    navigator: { locks }, BroadcastChannel: undefined,
    fetch: async (url, init) => { fetches.push(String(url)); return net(String(url), init); },
    setTimeout, clearTimeout, setInterval, clearInterval, Promise, Response, Uint8Array, Int16Array, DataView, Math, Date,
    AbortController, console,
  });
  vm.runInContext('"use strict";\n' + SRC.replace(/^export\s+/gm, ''), ctx, { filename: 'micgate.js' });
  return { ctx, fetches };
}

// Run `fn` and fail the test on ANY unhandled rejection it causes (a ReferenceError inside an async path
// surfaces exactly like that: the caller's promise rejects, nobody hears onerror/onend).
async function noThrow(fn) {
  try { return await fn(); }
  catch (e) { assert.fail('micgate threw instead of reporting through onerror: ' + (e && e.stack || e)); }
}

test('streamMic on a busy mic reports onerror("busy") and ends — it does not throw', async () => {
  const { ctx } = loadMicgate({ held: true });
  const seen = [];
  await noThrow(() => ctx.streamMic({ label: 'Dettatura', onerror: (k, who) => seen.push(['err', k, who]), onend: () => seen.push(['end']) }));
  assert.deepEqual(seen.map((s) => s.slice(0, 2)), [['err', 'busy'], ['end']]);
});

test('recordToSd on a busy mic reports onerror("busy") and resolves done → null', async () => {
  const { ctx } = loadMicgate({ held: true });
  const seen = [];
  const rec = ctx.recordToSd({ label: 'Registrazione', onerror: (k) => seen.push(k) });
  const r = await noThrow(() => rec.done);
  assert.equal(r, null);
  assert.deepEqual(seen, ['busy']);
});

test('recordToSd: a firmware 409 / 401 / 500 on /api/rec/start becomes busy / auth / audio', async () => {
  for (const [status, kind] of [[409, 'busy'], [401, 'auth'], [500, 'audio']]) {
    const { ctx } = loadMicgate({ net: async (u) => (u.startsWith('/api/rec/start') ? new Response('{}', { status }) : new Response('{}')) });
    const seen = [];
    const rec = ctx.recordToSd({ onerror: (k) => seen.push(k) });
    assert.equal(await noThrow(() => rec.done), null);
    assert.deepEqual(seen, [kind], 'HTTP ' + status);
  }
});

test('streamMic: a stream the device keeps refusing ends with onerror, not a ReferenceError', async () => {
  // 401 → 'auth' immediately (no retry loop), the hard-error path at the end of _runStream.
  const { ctx } = loadMicgate({ net: async (u) => (u.startsWith('/api/rec/stream') ? new Response('', { status: 401 }) : new Response('{"recording":false}')) });
  const seen = [];
  await noThrow(() => ctx.streamMic({ onerror: (k) => seen.push(k), onend: () => seen.push('end') }));
  assert.deepEqual(seen, ['auth', 'end']);
});

test('onMicStatus tells a new subscriber the current holder at once', () => {
  const { ctx } = loadMicgate();
  const got = [];
  const off = ctx.onMicStatus((who) => got.push(who));
  assert.deepEqual(got, [null], 'the subscriber was never called (its initial call threw and was swallowed)');
  off();
});

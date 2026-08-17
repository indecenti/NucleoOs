// Unit test for the desktop shell's resilient boot fetch (web/shell/boot-fetch.js).
//
// This is the policy that fixed "the web client doesn't even attach": the PSRAM-less, single-task,
// 6-socket device floods at boot, so /api/apps would 503 / time out, boot() hung awaiting it, and the
// live /ws socket was never attached. The fix RETRIES 503/502/504 + timeouts + network errors with
// backoff (spacing the load), honours Retry-After, fails fast on 4xx, and gives up after N tries.
// We inject fetch + sleep so the whole policy is exercised deterministically with no real timers/net.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeFetchJSON, makeLoadState } from '../web/shell/boot-fetch.js';

// A fake Response. ok mirrors the fetch spec (2xx). headers.get() returns the map value or null.
const res = (status, body = {}, headers = {}) => ({
  status,
  ok: status >= 200 && status < 300,
  headers: { get: (k) => (k in headers ? String(headers[k]) : null) },
  json: async () => body,
});

// Build a fetchJSON whose fetch replays `steps` (a Response, or an Error to throw), recording calls
// and the sleeps requested. AbortSignal is stubbed so the timeout path is taken without real timers.
function harness(steps) {
  const calls = [];
  const sleeps = [];
  const fetch = async (path, opts) => {
    calls.push({ path, opts });
    const step = steps[calls.length - 1];
    if (step instanceof Error) throw step;
    return step;
  };
  const sleep = async (ms) => { sleeps.push(ms); };
  const fetchJSON = makeFetchJSON({ fetch, sleep, AbortSignal: { timeout: () => ({}) } });
  return { fetchJSON, calls, sleeps };
}

test('success on the first try — no retry, no sleep', async () => {
  const h = harness([res(200, { apps: [1, 2] })]);
  const out = await h.fetchJSON('/api/apps');
  assert.deepEqual(out, { apps: [1, 2] });
  assert.equal(h.calls.length, 1);
  assert.equal(h.sleeps.length, 0);
});

test('passes a timeout AbortSignal on every attempt', async () => {
  const h = harness([res(200, {})]);
  await h.fetchJSON('/api/apps');
  assert.ok(h.calls[0].opts.signal, 'a signal must be attached so a hung request fails fast');
  assert.equal(h.calls[0].opts.cache, 'no-store');
});

test('retries 503 (busy/low-heap) then succeeds', async () => {
  const h = harness([res(503), res(503), res(200, { ok: true })]);
  const out = await h.fetchJSON('/api/apps');
  assert.deepEqual(out, { ok: true });
  assert.equal(h.calls.length, 3);
  assert.equal(h.sleeps.length, 2);          // backoff between the 3 attempts
});

test('retries on network error / timeout then succeeds', async () => {
  const h = harness([new Error('timeout'), new Error('network'), res(200, { v: 1 })]);
  const out = await h.fetchJSON('/api/apps');
  assert.deepEqual(out, { v: 1 });
  assert.equal(h.calls.length, 3);
  assert.equal(h.sleeps.length, 2);
});

test('honours Retry-After (seconds) on a 503', async () => {
  const h = harness([res(503, {}, { 'Retry-After': '2' }), res(200, {})]);
  await h.fetchJSON('/api/apps');
  assert.equal(h.sleeps[0], 2000);           // 2s, not the default backoff
});

test('502 and 504 are retryable too', async () => {
  const h = harness([res(502), res(504), res(200, { done: true })]);
  const out = await h.fetchJSON('/api/x');
  assert.deepEqual(out, { done: true });
  assert.equal(h.calls.length, 3);
});

test('gives up after `tries` and throws', async () => {
  const h = harness([res(503), res(503), res(503)]);
  await assert.rejects(() => h.fetchJSON('/api/apps', { tries: 3 }));
  assert.equal(h.calls.length, 3);
  assert.equal(h.sleeps.length, 2);          // no pointless sleep after the final attempt
});

test('4xx fails fast — deterministic, never retried', async () => {
  const h = harness([res(404)]);
  await assert.rejects(() => h.fetchJSON('/api/fs/read?path=/missing'), /HTTP 404/);
  assert.equal(h.calls.length, 1);           // exactly one attempt
  assert.equal(h.sleeps.length, 0);
});

test('mixed transient failures then success', async () => {
  const h = harness([new Error('reset'), res(503), res(200, { mix: 1 })]);
  const out = await h.fetchJSON('/api/apps');
  assert.deepEqual(out, { mix: 1 });
  assert.equal(h.calls.length, 3);
});

test('missing dependencies throw at construction', () => {
  assert.throws(() => makeFetchJSON({ sleep: async () => {} }), /fetch dependency required/);
  assert.throws(() => makeFetchJSON({ fetch: async () => {} }), /sleep dependency required/);
});

// ── makeLoadState: the data-loss guard ────────────────────────────────────────────────────────
// The shell used to read each user-state store with a bare un-retried fetch, take ANY failure as
// "first run", seed the factory defaults — and write them back over the real file. These tests pin
// the three outcomes apart, because conflating "nothing saved" with "cannot reach the device" is
// what destroyed a desktop layout on a single 503.

test('loadState: real saved state comes back as found', async () => {
  const loadState = makeLoadState(async () => ({ pins: ['a'], iconSize: 'lg' }));
  const r = await loadState('/system/config/ui-state.json');
  assert.equal(r.state, 'found');
  assert.deepEqual(r.data, { pins: ['a'], iconSize: 'lg' });
});

test('loadState: the firmware "{}" for a fresh SD is ABSENT, not found', async () => {
  // nucleo_fsapi.c read_get answers a missing /system/config/*.json with 200 "{}" on purpose.
  const loadState = makeLoadState(async () => ({}));
  assert.equal((await loadState('/system/config/ui-state.json')).state, 'absent');
});

test('loadState: junk that is not a state object is ABSENT, never merged', async () => {
  for (const junk of [null, 'nope', 42, ['a']]) {
    const loadState = makeLoadState(async () => junk);
    assert.equal((await loadState('/x.json')).state, 'absent', 'junk: ' + JSON.stringify(junk));
  }
});

test('loadState: a device that cannot be reached is UNREACHABLE, never absent', async () => {
  const loadState = makeLoadState(async () => { throw new Error('network unreachable'); });
  const r = await loadState('/system/config/ui-state.json');
  assert.equal(r.state, 'unreachable', 'this is the case that used to be read as "first run"');
  assert.match(r.error, /network/);
});

test('loadState: a 4xx after retries is UNREACHABLE too — we cannot conclude the file is missing', async () => {
  const loadState = makeLoadState(async () => { throw new Error('HTTP 401'); });
  assert.equal((await loadState('/system/config/session.json')).state, 'unreachable');
});

test('loadState passes the retry budget through to fetchJSON', async () => {
  let seen = null;
  const loadState = makeLoadState(async (url, opts) => { seen = { url, opts }; return { a: 1 }; });
  await loadState('/system/config/clipboard.json', { tries: 7, timeout: 1234 });
  assert.equal(seen.opts.tries, 7);
  assert.equal(seen.opts.timeout, 1234);
  assert.match(seen.url, /^\/api\/fs\/read\?path=/);
  assert.ok(seen.url.includes(encodeURIComponent('/system/config/clipboard.json')));
});

// A corrupt file is not an unreachable device. This was a REGRESSION introduced with makeLoadState:
// the pre-existing code caught JSON.parse and re-seeded, so a truncated ui-state.json healed itself.
// Routing it to 'unreachable' put the store read-only — and since writes were then blocked, the bad
// file could never be replaced, on every boot, forever. The device serves a 0-byte file as 200 with an
// EMPTY body (nucleo_fsapi.c), and Response.json() always rejects on that.
test('loadState: an unparsable body is ABSENT (re-seedable), not unreachable', async () => {
  const parseErr = Object.assign(new Error('Unexpected end of JSON input'), { parseFailure: true });
  const loadState = makeLoadState(async () => { throw parseErr; });
  const r = await loadState('/system/config/ui-state.json');
  assert.equal(r.state, 'absent', 'a corrupt file must be re-seeded, never freeze the store');
  assert.equal(r.corrupt, true);
});

test('loadState: a transport failure stays unreachable even when it also looks like bad JSON', async () => {
  const loadState = makeLoadState(async () => { throw new Error('Unexpected end of JSON input'); });
  assert.equal((await loadState('/x.json')).state, 'unreachable', 'no parseFailure tag → transport');
});

test('fetchJSON tags a parse failure so the caller can tell the two apart', async () => {
  const fetchJSON = makeFetchJSON({
    fetch: async () => ({ ok: true, status: 200, headers: { get: () => null }, json: async () => { throw new SyntaxError('bad'); } }),
    sleep: async () => {},
  });
  await assert.rejects(fetchJSON('/x'), (e) => e.parseFailure === true);
});

test('fetchJSON does NOT retry a parse failure — it is deterministic', async () => {
  let calls = 0;
  const fetchJSON = makeFetchJSON({
    fetch: async () => { calls++; return { ok: true, status: 200, headers: { get: () => null }, json: async () => { throw new SyntaxError('bad'); } }; },
    sleep: async () => {},
  });
  await fetchJSON('/x', { tries: 5 }).catch(() => {});
  assert.equal(calls, 1, 'retrying a corrupt file just burns the device sockets');
});

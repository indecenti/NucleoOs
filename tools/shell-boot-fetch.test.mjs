// Unit test for the desktop shell's resilient boot fetch (web/shell/boot-fetch.js).
//
// This is the policy that fixed "the web client doesn't even attach": the PSRAM-less, single-task,
// 6-socket device floods at boot, so /api/apps would 503 / time out, boot() hung awaiting it, and the
// live /ws socket was never attached. The fix RETRIES 503/502/504 + timeouts + network errors with
// backoff (spacing the load), honours Retry-After, fails fast on 4xx, and gives up after N tries.
// We inject fetch + sleep so the whole policy is exercised deterministically with no real timers/net.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeFetchJSON } from '../web/shell/boot-fetch.js';

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

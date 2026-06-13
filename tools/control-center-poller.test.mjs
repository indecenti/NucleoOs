// Unit tests for the single adaptive live poller (apps/settings/www/poller.js).
// The poller is the whole "never burden the device" contract in code, so it gets thorough coverage.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createPoller } from '../apps/settings/www/poller.js';

test('at most one request in flight — an overlapping tick is dropped', async () => {
  let calls = 0, release;
  const gate = new Promise((r) => { release = r; });
  const p = createPoller({ fetchJson: async () => { calls++; await gate; return { v: 1 }; }, sources: ['heap'] });
  const first = p.tick();                      // starts the fetch, holds it open
  const second = await p.tick();               // must be dropped while the first is in flight
  assert.equal(calls, 1, 'only one fetch issued');
  assert.equal(second.skipped, 'inflight');
  release({}); await first;
});

test('interval backs off geometrically while readings are calm', async () => {
  const p = createPoller({ fetchJson: async () => ({ v: 1 }), sources: ['heap'],
    idleMs: 3000, backoff: 1.5, maxMs: 10000, changed: () => false, crossedThreshold: () => false });
  await p.tick(); assert.equal(p.interval, 4500);
  await p.tick(); assert.equal(p.interval, 6750);
  await p.tick(); assert.equal(p.interval, 10000);   // capped at maxMs
  await p.tick(); assert.equal(p.interval, 10000);
});

test('interval tightens to minMs when a value crosses a threshold band', async () => {
  let cross = false;
  const p = createPoller({ fetchJson: async () => ({ v: 1 }), sources: ['heap'],
    idleMs: 3000, minMs: 2000, changed: () => false, crossedThreshold: () => cross });
  await p.tick(); assert.equal(p.interval, 4500);    // calm → widened
  cross = true;
  await p.tick(); assert.equal(p.interval, 2000);    // crossed → tightened
});

test('moving-but-fine readings hold the baseline idle interval', async () => {
  const p = createPoller({ fetchJson: async () => ({ v: 1 }), sources: ['heap'],
    idleMs: 3000, changed: () => true, crossedThreshold: () => false });
  await p.tick(); assert.equal(p.interval, 3000);
});

test('hidden() true ⇒ no fetch is issued', async () => {
  let calls = 0;
  const p = createPoller({ fetchJson: async () => { calls++; return {}; }, hidden: () => true, sources: ['heap'] });
  const r = await p.tick();
  assert.equal(calls, 0);
  assert.equal(r.skipped, 'hidden');
});

test('freeze stops all ticks', async () => {
  let calls = 0;
  const p = createPoller({ fetchJson: async () => { calls++; return {}; }, sources: ['heap'] });
  p.setFrozen(true);
  const r = await p.tick();
  assert.equal(calls, 0);
  assert.equal(r.skipped, 'frozen');
  assert.equal(p.frozen, true);
});

test('on a 503 it backs off N ticks instead of retry-storming', async () => {
  let calls = 0;
  const p = createPoller({ fetchJson: async () => { calls++; return { __status: 503 }; }, sources: ['heap'], skipOnBusy: 3 });
  const r0 = await p.tick();                    // hits the device, sees busy
  assert.equal(r0.busy, true);
  assert.equal(calls, 1);
  await p.tick(); await p.tick(); await p.tick();   // three skipped ticks (no fetch)
  assert.equal(calls, 1, 'no extra fetches during back-off');
  await p.tick();                               // back-off elapsed → fetches again
  assert.equal(calls, 2);
});

test('arbiter.busy in the payload also triggers the back-off', async () => {
  let calls = 0;
  const p = createPoller({ fetchJson: async () => { calls++; return { arbiter: { busy: true } }; }, sources: ['status'], skipOnBusy: 2 });
  await p.tick(); assert.equal(calls, 1);
  await p.tick(); await p.tick();
  assert.equal(calls, 1, 'skipped while busy');
});

test('round-robins its sources so two reads never overlap', async () => {
  const seen = [];
  const p = createPoller({ fetchJson: async (s) => { seen.push(s); return {}; }, sources: ['status', 'heap', 'cpu'], changed: () => true });
  await p.tick(); await p.tick(); await p.tick(); await p.tick();
  assert.deepEqual(seen, ['status', 'heap', 'cpu', 'status']);
});

test('a fetch error eases off (widens interval) and reports via onError', async () => {
  let errs = 0;
  const p = createPoller({ fetchJson: async () => { throw new Error('net'); }, sources: ['heap'], idleMs: 3000, onError: () => errs++ });
  await p.tick();
  assert.equal(errs, 1);
  assert.equal(p.interval, 4500);
});

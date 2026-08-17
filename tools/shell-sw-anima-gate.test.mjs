// The /api/anima admission policy, extracted as pure logic so the two properties that matter can be
// asserted without a browser or a service worker.
//
// Why it exists: TWELVE surfaces call /api/anima — the copilot, the shell's search bridge, onboarding,
// ai.js, and the anima/agent/settings/spreadsheet/games/miei-fatti/recorder/code-runner apps — and none
// of them knows about the others. On a PSRAM-less chip with 4-6 sockets shared by every open iframe,
// "never make concurrent calls" was enforced only by the good manners of twelve files.

import { test } from 'node:test';
import assert from 'node:assert/strict';

// Mirrors the sw.js branch: a shared admission slot plus coalescing of identical in-flight GETs.
function makeAnimaGate(fetchImpl, { slots = 1 } = {}) {
  const inflight = new Map();
  let active = 0; const waiting = [];
  const acquire = () => (active < slots ? (active++, Promise.resolve()) : new Promise((r) => waiting.push(r)));
  const release = () => { active--; const n = waiting.shift(); if (n) { active++; n(); } };
  return function request(url) {
    const hit = inflight.get(url);
    if (hit) return hit;
    const job = acquire()
      .then(() => fetchImpl(url))
      .finally(() => { release(); inflight.delete(url); });
    inflight.set(url, job);
    return job;
  };
}

const deferred = () => { let res; const p = new Promise((r) => { res = r; }); return { p, res }; };

test('identical questions asked together become ONE request', async () => {
  let calls = 0;
  const d = deferred();
  const gate = makeAnimaGate(() => { calls++; return d.p; });
  const a = gate('/api/anima?q=che+ore+sono');
  const b = gate('/api/anima?q=che+ore+sono');
  const c = gate('/api/anima?q=che+ore+sono');
  d.res('ok');
  assert.deepEqual(await Promise.all([a, b, c]), ['ok', 'ok', 'ok']);
  assert.equal(calls, 1, 'three surfaces, one device request');
});

test('different questions are NOT merged', async () => {
  let calls = 0;
  const gate = makeAnimaGate(async () => { calls++; return 'r'; });
  await Promise.all([gate('/api/anima?q=a'), gate('/api/anima?q=b')]);
  assert.equal(calls, 2);
});

test('concurrent DIFFERENT questions are serialised, never simultaneous', async () => {
  let peak = 0, live = 0;
  const gate = makeAnimaGate(async () => {
    live++; peak = Math.max(peak, live);
    await new Promise((r) => setTimeout(r, 5));
    live--; return 'r';
  });
  await Promise.all(['a', 'b', 'c', 'd'].map((q) => gate('/api/anima?q=' + q)));
  assert.equal(peak, 1, 'the device must never see two ANIMA queries at once');
});

test('a coalesced entry is released, so the next identical question really runs', async () => {
  let calls = 0;
  const gate = makeAnimaGate(async () => { calls++; return 'r'; });
  await gate('/api/anima?q=x');
  await gate('/api/anima?q=x');
  assert.equal(calls, 2, 'coalescing is per in-flight burst, never a cache');
});

test('a failure does not wedge the slot or poison the key', async () => {
  let calls = 0;
  const gate = makeAnimaGate(async () => { calls++; if (calls === 1) throw new Error('device busy'); return 'ok'; });
  await assert.rejects(gate('/api/anima?q=y'));
  assert.equal(await gate('/api/anima?q=y'), 'ok', 'the next caller must still get through');
});

test('everyone waiting on one burst sees the same answer', async () => {
  const d = deferred();
  const gate = makeAnimaGate(() => d.p);
  const all = [gate('/api/anima?q=z'), gate('/api/anima?q=z')];
  d.res({ reply: 'sono le 10' });
  const [r1, r2] = await Promise.all(all);
  assert.equal(r1, r2);
});

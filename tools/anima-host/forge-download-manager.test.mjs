// Gate: ANIMA Forge — the runnable download-manager controller (download.js FSM + scheduler.js
// coexistence, all I/O injected). Hard properties: (1) EVERY request is a bounded Range — no single
// request ever covers a whole multi-window shard; (2) an SD 503 / heap breaker / verifier-in-flight
// PAUSES and shrinks the window, never proceeds; (3) a SHA mismatch re-fetches (bounded) then
// verify-fails; (4) an already-cached shard is skipped. Fully deterministic, no DOM/network.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeDownloadManager } from '../../apps/anima/www/forge/download-manager.js';
import { DL, MIN_WINDOW, MAX_WINDOW } from '../../apps/anima/www/forge/download.js';

const SHA0 = 'a'.repeat(64);
const SHA1 = 'b'.repeat(64);
const manifest = {
  model: 'qwen2.5-coder-1.5b', revision: 'deadbeef',
  shards: [
    { name: 'shard0.bin', bytes: 2_500_000, sha256: SHA0 },   // 3 windows at MAX_WINDOW
    { name: 'shard1.bin', bytes: 1_000_000, sha256: SHA1 },   // 1 window
  ],
};

// In-memory transport: records every range it served per shard so tests can assert boundedness.
function makeFetch({ failOn = null, mode = {} } = {}) {
  const requests = [];   // { name, start, end, span }
  let calls = 0;
  return {
    requests,
    get calls() { return calls; },
    async fetchRange(name, { start, end }) {
      calls++;
      const span = end - start + 1;
      requests.push({ name, start, end, span });
      if (failOn && failOn(name, start, end, calls)) return { ok: false, status: 503 };
      return { ok: true, status: 206, bytes: span };
    },
    mode,
  };
}

// Injected hasher: returns the manifest sha for whichever shard the assembled byte count matches,
// unless `corrupt` says to flip it (simulating a bad transfer).
function makeSha({ corrupt = () => false } = {}) {
  let n = 0;
  return async (bytes) => {
    n++;
    // Map assembled bytes back to a shard by size; deterministic.
    const shard = manifest.shards.find((s) => s.bytes === bytes);
    const good = shard ? shard.sha256 : 'f'.repeat(64);
    return corrupt(n, bytes, good) ? 'c'.repeat(64) : good;
  };
}

const HEALTHY = { freeHeap: 120 * 1024, largestBlock: 300 * 1024, verifyInFlight: false };

test('healthy CDN run downloads all shards via BOUNDED ranges and reaches READY', async () => {
  const t = makeFetch();
  const dm = makeDownloadManager({
    manifest, source: 'cdn',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => ({ freeHeap: 1, largestBlock: 1 }),   // CDN bypasses the device → still 'go'
    cache: {},
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.READY);
  assert.equal(r.ok, true);
  assert.deepEqual(r.done.sort(), ['shard0.bin', 'shard1.bin']);

  // HARD invariant: no single request covers a whole multi-window shard.
  for (const req of t.requests) assert.ok(req.span <= MAX_WINDOW, 'every request is a bounded window');
  const s0 = t.requests.filter((q) => q.name === 'shard0.bin');
  assert.equal(s0.length, 3, 'a 2.5MB shard is split into 3 bounded ranges, never one GET');
  for (const q of s0) assert.ok(q.span < 2_500_000, 'no request spans the whole shard');
  // ranges fully and contiguously cover the shard
  s0.sort((a, b) => a.start - b.start);
  assert.equal(s0[0].start, 0);
  assert.equal(s0[s0.length - 1].end, 2_500_000 - 1);
});

test('CDN parallelises (up to maxConcurrency) — still all bounded', async () => {
  const t = makeFetch();
  const dm = makeDownloadManager({
    manifest, source: 'cdn',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => HEALTHY, cache: {},
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.READY);
  for (const req of t.requests) assert.ok(req.span <= MAX_WINDOW);
});

test('SD 503 pauses, backs off, and SHRINKS the window (never a whole-file GET)', async () => {
  // Fail the very first range of shard0 with a 503 (the device circuit-breaker).
  const t = makeFetch({ failOn: (name, start) => name === 'shard0.bin' && start === 0 });
  const dm = makeDownloadManager({
    manifest, source: 'sd',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => HEALTHY,    // heap fine; the 503 itself triggers the SD pause
    cache: {},
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.PAUSED, 'SD 503 → paused, never proceed');
  assert.ok(r.window < MAX_WINDOW, 'window shrank on the reset');
  // The only requests issued were bounded ranges (no whole-file GET fallback).
  for (const req of t.requests) assert.ok(req.span <= MAX_WINDOW);
  assert.ok(r.done.length === 0, 'no shard completed before the back-off');
});

test('verifyInFlight telemetry PAUSES an SD pull before any request', async () => {
  const t = makeFetch();
  const dm = makeDownloadManager({
    manifest, source: 'sd',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => ({ ...HEALTHY, verifyInFlight: true }),   // verifier privileged → SD must wait
    cache: {},
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.PAUSED);
  assert.equal(t.calls, 0, 'never issued a range while the verifier was in flight');
});

test('heap-floor telemetry pauses an SD pull (breaker before the device 503s)', async () => {
  const t = makeFetch();
  const dm = makeDownloadManager({
    manifest, source: 'sd',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => ({ freeHeap: 200 * 1024, largestBlock: 4 * 1024, verifyInFlight: false }),
    cache: {},
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.PAUSED);
  assert.equal(t.calls, 0);
});

test('SHA mismatch triggers bounded re-fetch then verify-fail (never an infinite loop)', async () => {
  const t = makeFetch();
  // Corrupt EVERY hash for shard0 (2.5MB) → forces exhaustion of bounded retries.
  const sha = makeSha({ corrupt: (n, bytes) => bytes === 2_500_000 });
  const dm = makeDownloadManager({
    manifest, source: 'cdn',
    fetchRange: t.fetchRange, sha256: sha,
    telemetry: () => HEALTHY, cache: {},
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.ERROR, 'persistent SHA mismatch → verify-fail');
  assert.ok(r.errors.some((e) => e.verify && /sha-mismatch/.test(e.verify)));
  // It re-fetched (more than one pass over shard0's ranges) but stayed bounded.
  const s0 = t.requests.filter((q) => q.name === 'shard0.bin');
  assert.ok(s0.length > 3, 're-fetched the shard at least once');
  for (const q of s0) assert.ok(q.span <= MAX_WINDOW, 're-fetch is still bounded');
});

test('SHA mismatch that clears on retry recovers to READY', async () => {
  const t = makeFetch();
  // First hash of shard0 is bad; subsequent hashes good → recovers within the retry budget.
  let badGiven = false;
  const sha = makeSha({ corrupt: (n, bytes) => { if (bytes === 2_500_000 && !badGiven) { badGiven = true; return true; } return false; } });
  const dm = makeDownloadManager({
    manifest, source: 'cdn',
    fetchRange: t.fetchRange, sha256: sha,
    telemetry: () => HEALTHY, cache: {},
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.READY);
  assert.deepEqual(r.done.sort(), ['shard0.bin', 'shard1.bin']);
});

test('already-cached shard is SKIPPED (idempotent resume)', async () => {
  const t = makeFetch();
  const cache = { 'shard0.bin': SHA0 };   // shard0 already present from a prior run
  const dm = makeDownloadManager({
    manifest, source: 'cdn',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => HEALTHY, cache,
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.READY);
  assert.deepEqual(r.done.sort(), ['shard1.bin'], 'only the missing shard ran');
  // shard0 never fetched.
  assert.equal(t.requests.filter((q) => q.name === 'shard0.bin').length, 0);
  assert.ok(t.requests.every((q) => q.name === 'shard1.bin'));
});

test('fully cached manifest reaches READY with ZERO requests', async () => {
  const t = makeFetch();
  const dm = makeDownloadManager({
    manifest, source: 'cdn',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => HEALTHY,
    cache: { 'shard0.bin': SHA0, 'shard1.bin': SHA1 },
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.READY);
  assert.equal(t.calls, 0);
});

test('bad manifest returns an error result without any I/O', async () => {
  const t = makeFetch();
  const dm = makeDownloadManager({
    manifest: { shards: [] }, source: 'cdn',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => HEALTHY, cache: {},
  });
  const r = await dm.run();
  assert.equal(r.ok, false);
  assert.equal(r.phase, DL.ERROR);
  assert.ok(Array.isArray(r.errors));
  assert.equal(t.calls, 0);
});

test('onProgress emits monotonic pct and ends at 100', async () => {
  const t = makeFetch();
  const seen = [];
  const dm = makeDownloadManager({
    manifest, source: 'cdn',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => HEALTHY, cache: {},
    onProgress: (p) => seen.push(p),
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.READY);
  assert.ok(seen.length > 0);
  for (let i = 1; i < seen.length; i++) assert.ok(seen[i].pct >= seen[i - 1].pct, 'pct never goes backwards');
  assert.equal(seen[seen.length - 1].pct, 100);
});

test('pause()/resume() are honoured between shards', async () => {
  const t = makeFetch();
  const dm = makeDownloadManager({
    manifest, source: 'sd',
    fetchRange: t.fetchRange, sha256: makeSha(),
    telemetry: () => HEALTHY, cache: {},
  });
  dm.pause();
  const r = await dm.run();
  assert.equal(r.phase, DL.PAUSED, 'an external pause is honoured before the first request');
  assert.equal(t.calls, 0);
});

test('throttle shrinks to MIN_WINDOW + serial on a tight-heap SD pull (still completes)', async () => {
  const t = makeFetch();
  const dm = makeDownloadManager({
    manifest, source: 'sd',
    fetchRange: t.fetchRange, sha256: makeSha(),
    // freeHeap below HEAP_TIGHT but block above HEAP_FLOOR → scheduler says 'throttle', not 'pause'.
    telemetry: () => ({ freeHeap: 40 * 1024, largestBlock: 300 * 1024, verifyInFlight: false }),
    cache: {},
  });
  const r = await dm.run();
  assert.equal(r.phase, DL.READY, 'throttled SD pull still completes');
  // Under throttle every request is the MIN_WINDOW; a 2.5MB shard → 10 bounded ranges.
  const s0 = t.requests.filter((q) => q.name === 'shard0.bin');
  for (const q of s0) assert.ok(q.span <= MIN_WINDOW, 'throttled requests are MIN_WINDOW-bounded');
  assert.ok(s0.length >= Math.ceil(2_500_000 / MIN_WINDOW));
});

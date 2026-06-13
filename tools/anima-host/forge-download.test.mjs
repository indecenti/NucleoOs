// Gate: ANIMA Forge — download FSM + manifest + bounded Range planning. Hard property: EVERY shard
// request is a bounded Range (never a whole-file GET — the voice.js read-storm), resume is idempotent.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { verifyManifest, planRanges, rangeHeader, adaptiveWindow, pendingShards, initState, reduce, maxConcurrency, DL, MIN_WINDOW, MAX_WINDOW } from '../../apps/anima/www/forge/download.js';

const SHA = 'a'.repeat(64);
const manifest = { model: 'qwen2.5-coder-1.5b', revision: 'deadbeef', shards: [
  { name: 'shard0.bin', bytes: 2_500_000, sha256: SHA },
  { name: 'shard1.bin', bytes: 1_000_000, sha256: 'b'.repeat(64) },
] };

test('verifyManifest accepts a good manifest and rejects bad ones', () => {
  assert.equal(verifyManifest(manifest).ok, true);
  assert.equal(verifyManifest({ shards: [] }).ok, false);
  assert.equal(verifyManifest({ model: 'm', revision: 'r', shards: [{ name: 'x', bytes: 0, sha256: 'short' }] }).ok, false);
});

test('planRanges produces ONLY bounded windows that fully cover the shard', () => {
  const ranges = planRanges(2_500_000, MAX_WINDOW);
  assert.equal(ranges.length, 3);                                  // 1MB + 1MB + 0.5MB
  for (const r of ranges) assert.ok((r.end - r.start + 1) <= MAX_WINDOW, 'window must be bounded');
  assert.equal(ranges[0].start, 0);
  assert.equal(ranges[ranges.length - 1].end, 2_500_000 - 1);      // covers to the last byte
  // contiguous, non-overlapping
  for (let i = 1; i < ranges.length; i++) assert.equal(ranges[i].start, ranges[i - 1].end + 1);
  assert.equal(rangeHeader(ranges[0]), 'bytes=0-1048575');
});

test('adaptiveWindow halves toward the floor on reset', () => {
  assert.equal(adaptiveWindow(MAX_WINDOW, true), MAX_WINDOW / 2);
  assert.equal(adaptiveWindow(MIN_WINDOW, true), MIN_WINDOW);       // never below floor
  assert.equal(adaptiveWindow(MAX_WINDOW, false), MAX_WINDOW);
});

test('idempotent resume: shards whose cached SHA matches are skipped', () => {
  const pending = pendingShards(manifest, { 'shard0.bin': SHA });
  assert.deepEqual(pending.map((s) => s.name), ['shard1.bin']);
  const all = initState(manifest, { cached: { 'shard0.bin': SHA, 'shard1.bin': 'b'.repeat(64) } });
  assert.equal(all.phase, DL.READY);                               // everything cached → ready, no fetch
});

test('FSM: idle → download each shard → verify → ready', () => {
  let s = initState(manifest, { source: 'cdn' });
  assert.equal(s.phase, DL.IDLE);
  s = reduce(s, { type: 'start' });
  assert.equal(s.phase, DL.DOWNLOADING);
  s = reduce(s, { type: 'shard-ok', name: 'shard0.bin' });
  assert.equal(s.phase, DL.DOWNLOADING);
  s = reduce(s, { type: 'shard-ok', name: 'shard1.bin' });
  assert.equal(s.phase, DL.VERIFYING);
  s = reduce(s, { type: 'verify-ok' });
  assert.equal(s.phase, DL.READY);
});

test('SD source backs off on a 503 (Retry-After) and shrinks the window', () => {
  let s = initState(manifest, { source: 'sd' });
  s = reduce(s, { type: 'start' });
  const w0 = s.window;
  s = reduce(s, { type: 'shard-fail', name: 'shard0.bin', status: 503, error: 'circuit-breaker' });
  assert.equal(s.phase, DL.PAUSED);
  assert.ok(s.window < w0, 'window shrinks on reset');
  assert.equal(maxConcurrency('sd'), 1);                            // serial on the device
  assert.equal(maxConcurrency('cdn'), 4);
});

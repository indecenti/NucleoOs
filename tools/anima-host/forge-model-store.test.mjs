// Gate: ANIMA Forge — model download manager rules. Enforces: never auto-download, one at a time,
// online (CDN) first → device SD fallback, integrity-verified, safe bounded SD reads.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeModelStore, REGISTRY, modelById } from '../../apps/anima/www/forge/model-store.js';

const ID = REGISTRY[0].id;
const GOOD = 'a'.repeat(64), BAD = 'b'.repeat(64);
const manifest = { model: ID, revision: 'deadbeef', shards: [{ name: 's0.bin', bytes: 3, sha256: GOOD }], aux: [] };

function mkDeps(over = {}) {
  const cache = new Map();
  const calls = { whole: 0, range: 0, ranges: [] };
  return Object.assign({
    cache, calls,
    getManifest: async () => manifest,
    sha256: async (bytes) => (bytes && bytes[0] === 1 ? GOOD : BAD),     // "good" bytes start with 1
    cacheHas: async (id, name) => cache.has(id + '/' + name),
    cachePut: async (id, name, bytes) => cache.set(id + '/' + name, bytes.length),
    isOnline: () => true,
    telemetry: () => ({ freeHeap: 200 * 1024, largestBlock: 300 * 1024, verifyInFlight: false }),
    fetchWhole: async () => { calls.whole++; return { ok: true, status: 200, bytes: new Uint8Array([1, 2, 3]) }; },
    fetchRange: async (url, r) => { calls.range++; calls.ranges.push(r); return { ok: true, status: 206, bytes: new Uint8Array([1]) }; },
  }, over);
}

test('NEVER auto-downloads: constructing the store starts nothing', () => {
  const s = makeModelStore(mkDeps());
  assert.equal(s.active, null);
});

test('SOURCE ORDER: online → CDN first (no SD hit)', async () => {
  const d = mkDeps({ isOnline: () => true });
  const s = makeModelStore(d);
  const r = await s.download(ID);
  assert.equal(r.source, 'cdn');
  assert.equal(d.calls.whole, 1);
  assert.equal(d.calls.range, 0);
  assert.equal(await s.status(ID), 'cached');
});

test('SOURCE ORDER: offline → device SD, with BOUNDED Range reads only', async () => {
  const d = mkDeps({ isOnline: () => false });
  const s = makeModelStore(d);
  const r = await s.download(ID);
  assert.equal(r.source, 'sd');
  assert.equal(d.calls.whole, 0);
  assert.ok(d.calls.range >= 1);
  for (const rg of d.calls.ranges) assert.ok(Number.isInteger(rg.start) && Number.isInteger(rg.end) && rg.end >= rg.start, 'every SD read is a bounded range');
});

test('CDN failure → falls back to the device SD', async () => {
  const d = mkDeps({ isOnline: () => true, fetchWhole: async () => ({ ok: false, status: 503 }) });
  const s = makeModelStore(d);
  const r = await s.download(ID);
  assert.equal(r.source, 'sd');
  assert.ok(d.calls.range >= 1);
});

test('ONE AT A TIME: a second download while one runs is rejected (never parallel)', async () => {
  let release; const gate = new Promise((res) => { release = res; });
  const d = mkDeps({ fetchWhole: async () => { await gate; return { ok: true, status: 200, bytes: new Uint8Array([1]) }; } });
  const s = makeModelStore(d);
  const first = s.download(ID);
  await new Promise((r) => setTimeout(r, 5));                 // let the first acquire the lock
  assert.equal(s.active, ID);
  await assert.rejects(() => s.download(REGISTRY[1].id), /one at a time|already running/i);
  release(); await first;
  assert.equal(s.active, null);
});

test('INTEGRITY: a SHA mismatch fails the download and does NOT cache', async () => {
  const d = mkDeps({ fetchWhole: async () => ({ ok: true, status: 200, bytes: new Uint8Array([0, 0, 0]) }) });  // [0..] → wrong sha
  const s = makeModelStore(d);
  await assert.rejects(() => s.download(ID, { source: 'cdn' }), /SHA mismatch/i);   // isolate the CDN source
  assert.equal(await s.status(ID), 'absent');
});

test('IDEMPOTENT: already-cached shards are skipped (no re-fetch)', async () => {
  const d = mkDeps();
  d.cache.set(ID + '/s0.bin', 3);                            // pre-cached
  const s = makeModelStore(d);
  const r = await s.download(ID);
  assert.equal(d.calls.whole, 0);
  assert.equal(d.calls.range, 0);
  assert.equal(r.ok, true);
});

test('registry exposes both engines and modelById resolves', () => {
  assert.ok(REGISTRY.length >= 2);
  assert.equal(modelById(ID).kind, 'webgpu');
  assert.ok(REGISTRY.some((m) => m.kind === 'wasm'));
});

test('every registry entry embeds a VALID manifest (no "manifest not found" possible)', () => {
  for (const m of REGISTRY) {
    assert.ok(m.manifest, m.id + ' has an embedded manifest');
    const ok = m.manifest.model && m.manifest.revision && Array.isArray(m.manifest.shards) && m.manifest.shards.length;
    assert.ok(ok, m.id + ' embedded manifest is well-formed');
    for (const s of m.manifest.shards) assert.ok(/^[0-9a-f]{64}$/i.test(s.sha256), m.id + ' shard sha is a sha-256');
  }
});

test('RESILIENT: a missing SD manifest falls back to the embed — status() resolves, never throws', async () => {
  // The exact failure behind "(manifest non trovato)": the device has no manifest.json on the SD.
  const d = mkDeps({ getManifest: async () => { throw new Error('404 not found'); } });
  const s = makeModelStore(d);
  assert.equal(await s.status(ID), 'absent');             // resolves via the embedded manifest, no throw
});

test('RESILIENT: with no SD manifest + online, weights still download from the CDN', async () => {
  // Embedded MLC manifest shas won't match the test sha stub, so target the GGUF (single shard) and
  // make sha256 always return its embedded sha → a clean CDN download proves the path works.
  const gguf = REGISTRY.find((m) => m.kind === 'wasm');
  const sha = gguf.manifest.shards[0].sha256;
  const d = mkDeps({ getManifest: async () => { throw new Error('404'); }, isOnline: () => true, sha256: async () => sha });
  const s = makeModelStore(d);
  const r = await s.download(gguf.id);
  assert.equal(r.source, 'cdn');
  assert.equal(d.calls.whole, 1);                          // pulled the single GGUF shard from the CDN
  assert.equal(await s.status(gguf.id), 'cached');
});

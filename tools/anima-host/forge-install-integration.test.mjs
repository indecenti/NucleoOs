// Gate: ANIMA offline-model installer — REAL-functioning integration. Unlike forge-model-store/
// forge-install-flow (which inject tiny stubs), this wires the ACTUAL production pieces together against
// realistic I/O: the real Web-Crypto SHA-256 adapter (makeSha256 from model-io.js — the exact hasher the
// browser uses), multi-megabyte REAL byte payloads, a real AbortController, and the real model-store +
// install-flow controllers. It proves the whole "scarica dal Cardputer → byte verificati → gira offline"
// path actually works: bytes are reassembled correctly from bounded SD ranges, corruption is caught by a
// genuine checksum, an interrupted download RESUMES from the verified shards, cancel stops it, and the
// full install pipeline survives repeated disconnects and still lands a byte-correct model.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeModelStore, REGISTRY } from '../../apps/anima/www/forge/model-store.js';
import { makeSha256 } from '../../apps/anima/www/forge/model-io.js';
import { installModel } from '../../apps/anima/www/forge/install-flow.js';
import { MAX_WINDOW } from '../../apps/anima/www/forge/download.js';

const sha256 = makeSha256();                 // REAL SHA-256 via globalThis.crypto.subtle (Node ≥18)
const MODEL = REGISTRY[0].id;                 // reuse a real registry id; getManifest overrides the spec

// Deterministic pseudo-random bytes (reproducible — no Math.random, so a failure is debuggable).
function makeBytes(seed, len) {
  const a = new Uint8Array(len); let x = (seed * 2654435761) >>> 0;
  for (let i = 0; i < len; i++) { x = (x * 1664525 + 1013904223) >>> 0; a[i] = (x >>> 24) & 0xff; }
  return a;
}

// A small but realistic model: shards that span MULTIPLE 1 MB SD windows so range-assembly is exercised.
const SHARDS = [
  { name: 'params_shard_0.bin', data: makeBytes(1, 2_500_000) },   // 3 SD ranges
  { name: 'params_shard_1.bin', data: makeBytes(2, 1_200_000) },   // 2 SD ranges
  { name: 'params_shard_2.bin', data: makeBytes(3,   700_000) },   // 1 SD range
];
const AUX = [{ name: 'mlc-chat-config.json', data: makeBytes(9, 2048) }];
const ALL = [...SHARDS, ...AUX];
for (const f of ALL) f.sha256 = await sha256(f.data);             // top-level await: real shas for the manifest
const BY = Object.fromEntries(ALL.map((f) => [f.name, f.data]));
const manifest = {
  model: MODEL, revision: 'itest', format: 'mlc',
  totalBytes: ALL.reduce((n, f) => n + f.data.length, 0),
  shards: SHARDS.map((f) => ({ name: f.name, bytes: f.data.length, sha256: f.sha256 })),
  aux: AUX.map((f) => ({ name: f.name, bytes: f.data.length, sha256: f.sha256 })),
};

// A virtual CDN+SD serving the REAL bytes, with injectable faults (disconnect, corruption, abort hook).
// cache is a real Map<name, Uint8Array> standing in for the browser Cache API.
function makeWorld(opts = {}) {
  const cache = new Map();
  const calls = { whole: 0, range: 0, ranges: [], rangeByName: {} };
  const nameOf = (url) => String(url).split('/').pop();
  const maybeCorrupt = (name, bytes) => {
    if (opts.corrupt === name) { const b = bytes.slice(); b[0] ^= 0xff; return b; }   // flip one byte on the wire
    return bytes;
  };
  const store = makeModelStore({
    getManifest: async () => manifest,
    sha256,                                                       // REAL hasher
    cacheHas: async (_id, name) => cache.has(name),
    cachePut: async (_id, name, bytes) => { cache.set(name, bytes); },
    isOnline: () => opts.online !== false,
    telemetry: () => ({ freeHeap: Infinity, largestBlock: Infinity, verifyInFlight: false }),
    fetchWhole: async (url, o = {}) => {
      calls.whole++;
      if (o.signal && o.signal.aborted) return { ok: false, status: 0 };
      const name = nameOf(url);
      const bytes = maybeCorrupt(name, BY[name]);
      if (o.onChunk) o.onChunk(bytes.length);
      return { ok: true, status: 200, bytes };
    },
    fetchRange: async (url, rg, o = {}) => {
      const name = nameOf(url);
      calls.range++; calls.ranges.push(rg); calls.rangeByName[name] = (calls.rangeByName[name] || 0) + 1;
      if (opts.onRange) opts.onRange(url, rg, o);                 // lets a test abort mid-flight
      if (o.signal && o.signal.aborted) return { ok: false, status: 0 };
      if (opts.failRange && opts.failRange(name, rg)) return { ok: false, status: 0 };   // simulate a disconnect
      const bytes = maybeCorrupt(name, BY[name]);
      return { ok: true, status: 206, bytes: bytes.slice(rg.start, rg.end + 1) };
    },
  });
  return { store, cache, calls };
}

// Re-hash the cached bytes and compare to the known sha — proves byte-identity with REAL crypto (and that
// SD range-reassembly produced the exact original), without spreading megabytes into JS arrays.
async function assertCacheByteCorrect(cache) {
  for (const f of ALL) {
    assert.ok(cache.has(f.name), 'cached: ' + f.name);
    assert.equal(await sha256(cache.get(f.name)), f.sha256, 'byte-identical: ' + f.name);
  }
}

function mkUi() {
  const calls = { phase: [], reconnect: [], error: null, cancelled: 0, done: null, progress: 0 };
  let cancelCb = null;
  return {
    label: 'Model', onCancel(cb) { cancelCb = cb; }, fireCancel() { cancelCb && cancelCb(); },
    setPhase(n) { calls.phase.push(n); }, onProgress() { calls.progress++; },
    setReconnecting(x) { calls.reconnect.push(x); }, setError(e) { calls.error = e; },
    setCancelled() { calls.cancelled++; }, setDone(r) { calls.done = r; }, calls,
  };
}
const instant = () => Promise.resolve();

test('REAL CDN end-to-end: every file pulled whole, bytes land byte-identical (real SHA-256)', async () => {
  const { store, cache, calls } = makeWorld({ online: true });
  const r = await store.download(MODEL);
  assert.equal(r.source, 'cdn');
  assert.equal(calls.whole, ALL.length);                         // one whole GET per shard+aux
  assert.equal(calls.range, 0);                                  // CDN never used bounded ranges
  await assertCacheByteCorrect(cache);
  assert.equal(await store.status(MODEL), 'cached');
});

test('REAL SD download: bounded ≤1MB ranges reassemble into the EXACT original bytes', async () => {
  const { store, cache, calls } = makeWorld({ online: false });
  const r = await store.download(MODEL);
  assert.equal(r.source, 'sd');
  assert.equal(calls.whole, 0);                                  // offline: never a whole-file GET
  for (const rg of calls.ranges) assert.ok((rg.end - rg.start + 1) <= MAX_WINDOW, 'every SD read is bounded');
  assert.ok(calls.rangeByName['params_shard_0.bin'] >= 3, '2.5MB shard spanned ≥3 windows');
  await assertCacheByteCorrect(cache);                           // reassembly from ranges is byte-perfect
});

test('REAL integrity: a single flipped byte is caught by the genuine checksum and never cached', async () => {
  const { store, cache } = makeWorld({ online: true, corrupt: 'params_shard_1.bin' });
  await assert.rejects(() => store.download(MODEL, { source: 'cdn' }), (e) => e.kind === 'integrity');
  assert.ok(cache.has('params_shard_0.bin'), 'the good shard before it was cached');
  assert.ok(!cache.has('params_shard_1.bin'), 'the corrupt shard was rejected, not stored');
  assert.equal(await store.status(MODEL), 'absent');
});

test('REAL resume: an SD disconnect mid-download loses nothing — re-run skips verified shards and finishes', async () => {
  let dropped = false;
  const world = makeWorld({ online: false, failRange: (name, rg) => {
    if (name === 'params_shard_2.bin' && rg.start === 0 && !dropped) { dropped = true; return true; }   // drop once, on shard 2
    return false;
  } });
  // 1st attempt: shards 0 & 1 fully verified+cached, shard 2's first range "disconnects" → transient throw.
  await assert.rejects(() => world.store.download(MODEL), (e) => e.kind === 'transient');
  assert.ok(world.cache.has('params_shard_0.bin') && world.cache.has('params_shard_1.bin'));
  assert.ok(!world.cache.has('params_shard_2.bin'));
  const r0 = world.calls.rangeByName['params_shard_0.bin'];      // snapshot fetches of the already-done shards
  const r1 = world.calls.rangeByName['params_shard_1.bin'];
  // 2nd attempt (auto-resume): the controller re-runs; cached shards are skipped, only shard 2 is fetched.
  const res = await world.store.download(MODEL);
  assert.equal(res.ok, true);
  assert.equal(world.calls.rangeByName['params_shard_0.bin'], r0, 'shard 0 not re-fetched on resume');
  assert.equal(world.calls.rangeByName['params_shard_1.bin'], r1, 'shard 1 not re-fetched on resume');
  await assertCacheByteCorrect(world.cache);
});

test('REAL cancel via AbortController: stops mid-download; a fresh run resumes from the cached shard', async () => {
  const ac = new AbortController();
  let armed = ac;
  const world = makeWorld({ online: false, onRange: (url) => {
    if (armed && url.endsWith('params_shard_1.bin')) { armed.abort(); armed = null; }   // user hits Cancel as shard 1 starts
  } });
  await assert.rejects(() => world.store.download(MODEL, { signal: ac.signal }), (e) => e.kind === 'cancelled');
  assert.ok(world.cache.has('params_shard_0.bin'), 'shard finished before cancel is kept');
  assert.ok(!world.cache.has('params_shard_1.bin'));
  // resume with a fresh signal → completes, byte-correct.
  const res = await world.store.download(MODEL, { signal: new AbortController().signal });
  assert.equal(res.ok, true);
  await assertCacheByteCorrect(world.cache);
});

test('REAL byte progress: bytesDone is monotonic and ends EXACTLY at the model total', async () => {
  const { store } = makeWorld({ online: false });
  const seen = [];
  await store.download(MODEL, { onProgress: (p) => { if (p.phase === 'progress' && Number.isFinite(p.bytesDone)) seen.push(p.bytesDone); } });
  assert.ok(seen.length > SHARDS.length, 'progress fired per range, not just per shard');
  for (let i = 1; i < seen.length; i++) assert.ok(seen[i] >= seen[i - 1], 'bytesDone never goes backwards');
  assert.equal(seen[seen.length - 1], manifest.totalBytes, 'final bytesDone == model total');
});

test('REAL pipeline: install-flow AUTO-RESUMES the real store over two disconnects and lands a byte-correct model', async () => {
  let s2drops = 0;
  const world = makeWorld({ online: false, failRange: (name, rg) => {
    if (name === 'params_shard_2.bin' && rg.start === 0 && s2drops < 2) { s2drops++; return true; }   // drop twice
    return false;
  } });
  const ui = mkUi();
  const res = await installModel({ store: world.store, modelId: MODEL, kind: 'wasm', caps: { wasm: true }, ui, sleep: instant });
  assert.equal(res.ok, true);
  assert.equal(res.source, 'sd');
  assert.equal(s2drops, 2);
  assert.equal(ui.calls.reconnect.length, 2, 'reconnected exactly twice, then succeeded');
  assert.equal(ui.calls.done.ok, true);
  await assertCacheByteCorrect(world.cache);                     // the offline model is fully, correctly installed
});

test('BULLETPROOF: never two Cardputer (SD) pulls at once — a second is refused while one holds the lock', async () => {
  let release; const gate = new Promise((r) => { release = r; });
  let firstInFlight = false;
  const store = makeModelStore({
    getManifest: async () => manifest,
    sha256,
    cacheHas: async () => false,
    cachePut: async () => {},
    isOnline: () => false,                                  // force the device-SD path
    telemetry: () => ({ freeHeap: Infinity, largestBlock: Infinity, verifyInFlight: false }),
    fetchWhole: async () => ({ ok: false, status: 404 }),
    fetchRange: async (url, rg) => { firstInFlight = true; await gate; const b = BY[String(url).split('/').pop()]; return { ok: true, status: 206, bytes: b.slice(rg.start, rg.end + 1) }; },
  });
  const first = store.download(MODEL);                      // begins an SD pull, then stalls mid-transfer on the gate
  await new Promise((r) => setTimeout(r, 10));
  assert.equal(firstInFlight, true);
  assert.equal(store.active, MODEL, 'the one-at-a-time lock is held for the whole transfer');
  // a second SD pull (even a different model) is REFUSED, never run concurrently against the single-task device.
  await assert.rejects(() => store.download(REGISTRY[1].id), /one at a time|already running/i);
  release();                                                // let the first finish
  const r = await first;
  assert.equal(r.source, 'sd');
  assert.equal(store.active, null);
});

test('REAL pipeline: install-flow cancel mid-download, then a second install completes byte-correct', async () => {
  const ac = new AbortController();
  let armed = ac;
  const world = makeWorld({ online: false, onRange: (url) => {
    if (armed && url.endsWith('params_shard_1.bin')) { armed.abort(); armed = null; }
  } });
  const ui = mkUi();
  const res = await installModel({ store: world.store, modelId: MODEL, kind: 'wasm', caps: { wasm: true }, ui, controller: ac, sleep: instant });
  assert.equal(res.reason, 'cancelled');
  assert.equal(ui.calls.cancelled, 1);
  assert.ok(world.cache.has('params_shard_0.bin') && !world.cache.has('params_shard_1.bin'));
  // second install (fresh controller) resumes from the cached shard and finishes.
  const ui2 = mkUi();
  const res2 = await installModel({ store: world.store, modelId: MODEL, kind: 'wasm', caps: { wasm: true }, ui: ui2, sleep: instant });
  assert.equal(res2.ok, true);
  await assertCacheByteCorrect(world.cache);
});

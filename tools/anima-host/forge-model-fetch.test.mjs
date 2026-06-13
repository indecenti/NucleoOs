// forge-model-fetch.test.mjs — deterministic host gate for the REAL resumable model downloader. Zero
// network, zero disk: an in-memory CDN + SD + telemetry are injected. Asserts the five load-bearing rules
// of model-fetch.js: single-download lock, idempotent SHA verify, bounded-window-only fetches, cross-reload
// resume (never re-fetch a verified shard), and scheduler-driven pause on a tight device.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { webcrypto } from 'node:crypto';
import { makeModelDownload, activeDownload, isBusy } from '../../apps/anima/www/forge/model-fetch.js';
import { makeSha256 } from '../../apps/anima/www/forge/model-io.js';
import { MAX_WINDOW } from '../../apps/anima/www/forge/download.js';

const sha256 = makeSha256({ crypto: webcrypto });

// Deterministic byte generator so every run produces identical shards (no Math.random).
function bytesOf(seed, n) { const u = new Uint8Array(n); let x = (seed * 2654435761) >>> 0; for (let i = 0; i < n; i++) { x = (x * 1664525 + 1013904223) >>> 0; u[i] = x & 0xff; } return u; }

// Build a manifest + a backing { name -> Uint8Array } and the matching SHAs.
async function makeModel(model, sizes) {
  const shards = []; const blob = {};
  for (let i = 0; i < sizes.length; i++) {
    const name = `unet.onnx.${String(i).padStart(3, '0')}`;
    const bytes = bytesOf(i + 1, sizes[i]); blob[name] = bytes;
    shards.push({ name, bytes: sizes[i], sha256: await sha256(bytes) });
  }
  return { manifest: { model, revision: 'r1', shards }, blob };
}

// In-memory CDN: serves bounded ranges, recording every (name,start,end) for invariant checks.
function makeCdn(blob, calls = []) {
  return async function fetchRange(name, { start, end }) {
    calls.push({ name, start, end, width: end - start + 1 });
    const b = blob[name]; if (!b) return { ok: false, status: 404 };
    return { ok: true, status: 206, chunk: b.subarray(start, end + 1) };
  };
}
// In-memory SD sink.
function makeSink(disk = {}) { return async (name, bytes) => { disk[name] = Uint8Array.from(bytes); return { ok: true }; }; }
// In-memory resume store.
function makeStore(seed = {}) { let c = { ...seed }; return { state: () => c, loadCache: () => ({ ...c }), saveCache: (x) => { c = { ...x }; } }; }
const okTel = async () => ({ freeHeap: Infinity, largestBlock: Infinity, verifyInFlight: false });

test('happy path: every shard fetched, verified, persisted; phase READY', async () => {
  const { manifest, blob } = await makeModel('m-happy', [500, 600, 700]);
  const disk = {}, store = makeStore();
  const dl = makeModelDownload({ manifest, source: 'cdn', fetchRange: makeCdn(blob), sha256, writeShard: makeSink(disk), telemetry: okTel, store });
  const r = await dl.run();
  assert.equal(r.phase, 'ready');
  assert.equal(r.done.length, 3);
  assert.deepEqual(Object.keys(disk).sort(), manifest.shards.map((s) => s.name).sort());
  // persisted cache equals the manifest SHAs
  assert.deepEqual(store.state(), Object.fromEntries(manifest.shards.map((s) => [s.name, s.sha256])));
  assert.equal(isBusy(), false, 'lock released after run');
});

test('single-download lock: a second model is refused while one is active', async () => {
  const A = await makeModel('m-A', [400, 400]);
  const B = await makeModel('m-B', [400]);
  let release; const gate = new Promise((r) => { release = r; });
  // A blocks on the first range until we release it.
  let first = true;
  const slowCdn = async (name, { start, end }) => { if (first) { first = false; await gate; } return { ok: true, status: 206, chunk: A.blob[name].subarray(start, end + 1) }; };
  const dlA = makeModelDownload({ manifest: A.manifest, source: 'cdn', fetchRange: slowCdn, sha256, writeShard: makeSink(), telemetry: okTel, store: makeStore() });
  const dlB = makeModelDownload({ manifest: B.manifest, source: 'cdn', fetchRange: makeCdn(B.blob), sha256, writeShard: makeSink(), telemetry: okTel, store: makeStore() });
  const pA = dlA.run();
  await Promise.resolve();                                   // let A acquire the lock + enter the blocked fetch
  assert.deepEqual(activeDownload(), { model: 'm-A', revision: 'r1' });
  const rB = await dlB.run();                                // B must be refused, not interleaved
  assert.equal(rB.error, 'busy');
  assert.deepEqual(rB.active, { model: 'm-A', revision: 'r1' });
  release(); await pA;
  assert.equal(isBusy(), false);
});

test('bounded windows only: a shard bigger than MAX_WINDOW is fetched in multiple ranges, none whole-file', async () => {
  const big = MAX_WINDOW + 12345;
  const { manifest, blob } = await makeModel('m-big', [big]);
  const calls = [];
  const dl = makeModelDownload({ manifest, source: 'cdn', fetchRange: makeCdn(blob, calls), sha256, writeShard: makeSink(), telemetry: okTel, store: makeStore() });
  const r = await dl.run();
  assert.equal(r.phase, 'ready');
  assert.ok(calls.length >= 2, 'shard split across >=2 ranges');
  for (const c of calls) assert.ok(c.width <= MAX_WINDOW, `range ${c.width} must be <= MAX_WINDOW ${MAX_WINDOW}`);
});

test('cross-reload resume: a fresh controller with the persisted cache re-fetches only the missing shard', async () => {
  const { manifest, blob } = await makeModel('m-resume', [300, 300, 300]);
  const store = makeStore();
  // Run 1: corrupt the 3rd shard so the run ERRORS after persisting shards 0 and 1.
  const badBlob = { ...blob, [manifest.shards[2].name]: bytesOf(99, 300) };
  const r1 = await makeModelDownload({ manifest, source: 'cdn', fetchRange: makeCdn(badBlob), sha256, writeShard: makeSink(), telemetry: okTel, store }).run();
  assert.equal(r1.phase, 'error');
  assert.equal(Object.keys(store.state()).length, 2, 'two good shards persisted before the failure');

  // Run 2 ("reload"): a brand-new controller sharing the store, now with the CORRECT 3rd shard.
  const calls = [];
  const r2 = await makeModelDownload({ manifest, source: 'cdn', fetchRange: makeCdn(blob, calls), sha256, writeShard: makeSink(), telemetry: okTel, store }).run();
  assert.equal(r2.phase, 'ready');
  const fetched = new Set(calls.map((c) => c.name));
  assert.deepEqual([...fetched], [manifest.shards[2].name], 'only the missing shard was re-fetched');
});

test('SHA mismatch never lands: a corrupt shard retries then declares verify-fail (ERROR)', async () => {
  const { manifest, blob } = await makeModel('m-corrupt', [300]);
  const disk = {};
  const corrupt = async (name, { start, end }) => ({ ok: true, status: 206, chunk: bytesOf(7, end - start + 1) });
  const r = await makeModelDownload({ manifest, source: 'cdn', fetchRange: corrupt, sha256, writeShard: makeSink(disk), telemetry: okTel, store: makeStore() }).run();
  assert.equal(r.phase, 'error');
  assert.equal(Object.keys(disk).length, 0, 'no corrupt bytes written to SD');
  assert.ok(r.errors.some((e) => e.fail === 'verify'));
});

test('scheduler pause: an SD pull on a tight device pauses, then completes when heap recovers', async () => {
  const { manifest, blob } = await makeModel('m-sd', [300, 300]);
  let tight = true;
  const tel = async () => tight ? { freeHeap: 10 * 1024, largestBlock: 10 * 1024, verifyInFlight: false } : { freeHeap: Infinity, largestBlock: Infinity, verifyInFlight: false };
  const store = makeStore(); const disk = {};
  const dl = makeModelDownload({ manifest, source: 'sd', fetchRange: makeCdn(blob), sha256, writeShard: makeSink(disk), telemetry: tel, store });
  const r1 = await dl.run();
  assert.equal(r1.phase, 'paused', 'paused on the heap-floor breaker');
  assert.equal(Object.keys(disk).length, 0, 'nothing pulled while paused');
  tight = false;                                              // device recovers
  const r2 = await dl.resume();
  assert.equal(r2.phase, 'ready');
  assert.equal(Object.keys(disk).length, 2);
});

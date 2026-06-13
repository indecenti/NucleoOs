// Gate: ANIMA Forge — deep-SHA lib verification. The "checksum-verified" upgrade over the size-level
// air-gap probe: reads the whole SD file via bounded, scheduler-gated Range windows and SHA-256s it.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { deepVerifyLib, deepVerifyAll } from '../../apps/anima/www/forge/lib-verify.js';

const sha256 = async (u8) => createHash('sha256').update(Buffer.from(u8)).digest('hex');
// a byte array of length n with deterministic content
const make = (n) => { const a = new Uint8Array(n); for (let i = 0; i < n; i++) a[i] = (i * 7 + 3) & 0xff; return a; };

// a fake fetchRange that serves slices of `data`; optionally 503s the first K calls (transient)
function serve(data, opts = {}) {
  let calls = 0, busy = opts.busy503 || 0;
  return async (_url, { start, end }) => {
    calls++;
    if (busy > 0) { busy--; return { ok: false, status: 503 }; }
    if (start >= data.length) return { ok: false, status: 416 };
    return { ok: true, status: 206, bytes: data.slice(start, end + 1) };
  };
}

test('deepVerifyLib: full read + matching sha → ok', async () => {
  const data = make(3000);
  const file = { url: '/x', bytes: data.length, sha256: await sha256(data) };
  const r = await deepVerifyLib(file, { fetchRange: serve(data), sha256 });
  assert.equal(r.ok, true); assert.equal(r.reason, null);
});

test('deepVerifyLib: a server that IGNORES Range (HTTP 200, full file each time) → still verifies once, no duplicate-copy mismatch', async () => {
  const data = make(4000);
  const file = { url: '/x', bytes: data.length, sha256: await sha256(data) };
  // every ranged request returns 200 + the WHOLE file (python http.server behaviour)
  const fetchFull = async () => ({ ok: true, status: 200, bytes: data });
  const r = await deepVerifyLib(file, { fetchRange: fetchFull, sha256 });
  assert.equal(r.ok, true); assert.equal(r.reason, null);
});

test('deepVerifyLib: same size but WRONG sha (bit-rot / wrong revision) → sha-mismatch', async () => {
  const data = make(3000);
  const file = { url: '/x', bytes: data.length, sha256: 'deadbeef'.repeat(8) };
  const r = await deepVerifyLib(file, { fetchRange: serve(data), sha256 });
  assert.equal(r.ok, false); assert.equal(r.reason, 'sha-mismatch');
});

test('deepVerifyLib: a transient 503 is retried (smaller window, same offset) and still completes', async () => {
  const data = make(5000);
  const file = { url: '/x', bytes: data.length, sha256: await sha256(data) };
  const r = await deepVerifyLib(file, { fetchRange: serve(data, { busy503: 2 }), sha256 });
  assert.equal(r.ok, true);
});

test('deepVerifyLib: a hard read error → read-error (never a false ok)', async () => {
  const file = { url: '/x', bytes: 1000, sha256: 'a'.repeat(64) };
  const r = await deepVerifyLib(file, { fetchRange: async () => ({ ok: false, status: 500 }), sha256 });
  assert.equal(r.ok, false); assert.equal(r.reason, 'read-error');
});

test('deepVerifyLib: heap-floor telemetry → paused (backs off, no crash)', async () => {
  const data = make(2000);
  const file = { url: '/x', bytes: data.length, sha256: await sha256(data) };
  const r = await deepVerifyLib(file, { fetchRange: serve(data), sha256, telemetry: () => ({ freeHeap: 1000, largestBlock: 1000 }) });
  assert.equal(r.ok, false); assert.equal(r.reason, 'paused');
});

test('deepVerifyLib: no manifest size/sha → no-manifest (can\'t claim verified)', async () => {
  const r = await deepVerifyLib({ url: '/x' }, { fetchRange: async () => ({ ok: true, bytes: new Uint8Array(0) }), sha256 });
  assert.equal(r.reason, 'no-manifest');
});

test('deepVerifyAll: per-lib results with names; one bad file flagged among good ones', async () => {
  const good = make(1500), bad = make(1500);
  const man = { libs: [
    { name: 'a', path: 'a.js', bytes: good.length, sha256: await sha256(good) },
    { name: 'b', path: 'b.wasm', bytes: bad.length, sha256: 'f'.repeat(64) },
  ] };
  const byPath = { '/v/a.js': good, '/v/b.wasm': bad };
  const fetchRange = async (url, { start, end }) => { const d = byPath[url]; return d ? { ok: true, status: 206, bytes: d.slice(start, end + 1) } : { ok: false, status: 404 }; };
  const res = await deepVerifyAll(man, '/v/', { fetchRange, sha256 });
  assert.deepEqual(res.map((r) => [r.name, r.ok]), [['a', true], ['b', false]]);
  assert.equal(res[1].reason, 'sha-mismatch');
});

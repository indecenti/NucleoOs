// forge-sd-model-loader.test.mjs — deterministic host gate for the chunked-SD reassembler (the generalised
// Vosk rig that feeds ONNX components to onnxruntime-web). In-memory SD parts injected; no disk, no timers.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { webcrypto } from 'node:crypto';
import { loadComponent, partName } from '../../apps/anima/www/forge/sd-model-loader.js';
import { makeSha256 } from '../../apps/anima/www/forge/model-io.js';

const sha256 = makeSha256({ crypto: webcrypto });
const noSleep = async () => {};
const U = (...a) => Uint8Array.from(a);

// In-memory SD: { path -> Uint8Array }. Missing path → 404.
function reader(parts, opts = {}) {
  const { failOnce = new Set() } = opts; const failed = new Set();
  return async (path) => {
    if (failOnce.has(path) && !failed.has(path)) { failed.add(path); return { ok: false, status: 503 }; }
    const b = parts[path]; return b ? { ok: true, status: 200, bytes: b } : { ok: false, status: 404 };
  };
}

test('reassembles parts in order into one contiguous buffer', async () => {
  const base = '/m/unet.onnx';
  const parts = { [partName(base, 0)]: U(1, 2, 3), [partName(base, 1)]: U(4, 5), [partName(base, 2)]: U(6, 7, 8, 9) };
  const r = await loadComponent({ base, parts: 3, readPart: reader(parts), sleep: noSleep });
  assert.equal(r.ok, true);
  assert.deepEqual([...new Uint8Array(r.buffer)], [1, 2, 3, 4, 5, 6, 7, 8, 9]);
  assert.equal(r.bytes, 9);
});

test('manifest mode: a declared part that is missing is a hard error, not a silent short read', async () => {
  const base = '/m/vae.onnx';
  const parts = { [partName(base, 0)]: U(1, 2), [partName(base, 2)]: U(5, 6) };  // part 1 missing
  const r = await loadComponent({ base, parts: 3, readPart: reader(parts), sleep: noSleep });
  assert.equal(r.ok, false);
  assert.equal(r.error, 'missing-part');
  assert.equal(r.part, 1);
});

test('discovery mode (no part count): stops cleanly at the first 404', async () => {
  const base = '/m/te.onnx';
  const parts = { [partName(base, 0)]: U(9, 9), [partName(base, 1)]: U(8) };     // .002 absent → end
  const r = await loadComponent({ base, readPart: reader(parts), sleep: noSleep });
  assert.equal(r.ok, true);
  assert.deepEqual([...new Uint8Array(r.buffer)], [9, 9, 8]);
});

test('a transient read failure is retried with backoff, then succeeds', async () => {
  const base = '/m/cn.onnx';
  const parts = { [partName(base, 0)]: U(1), [partName(base, 1)]: U(2, 3) };
  const r = await loadComponent({ base, parts: 2, readPart: reader(parts, { failOnce: new Set([partName(base, 1)]) }), sleep: noSleep });
  assert.equal(r.ok, true);
  assert.deepEqual([...new Uint8Array(r.buffer)], [1, 2, 3]);
});

test('per-part SHA check rejects a corrupt part instead of feeding bad bytes to WebGPU', async () => {
  const base = '/m/unet.onnx';
  const good0 = U(1, 2, 3), good1 = U(4, 5);
  const parts = { [partName(base, 0)]: good0, [partName(base, 1)]: U(9, 9) };    // part 1 corrupted
  const partSha = [await sha256(good0), await sha256(good1)];
  const r = await loadComponent({ base, parts: 2, partSha, sha256, readPart: reader(parts), sleep: noSleep });
  assert.equal(r.ok, false);
  assert.equal(r.error, 'sha-mismatch');
  assert.equal(r.part, 1);
});

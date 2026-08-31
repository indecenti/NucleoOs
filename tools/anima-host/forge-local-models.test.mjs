// Gate: ANIMA in-browser GPU model catalog. The chat's "Local · GPU" tier downloads one of these from the
// CDN on first use (automatic, transparent) and caches it offline. This pins: the default is the recommended
// model that actually runs for most people AND ships on the device SD (since 6dd9f49 that is the 1.5B, not
// the 7B — the 7B default OOM'd integrated GPUs after a 4.7 GB download), every id is a real MLC q4f16 id
// (so WebLLM's prebuilt config knows it — no model_lib to vendor), an explicit user choice always wins,
// no-WebGPU is the only HARD block (a per-buffer proxy never blocks a deliberate choice), and
// OOM/device-lost is recognised so the UI can say "pick smaller".
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  LOCAL_MODELS, DEFAULT_LOCAL_MODEL, localModelById, resolveLocalModel, localModelCompat, isOutOfMemoryError,
} from '../../apps/anima/www/forge/local-models.js';

test('catalog: every id is a real MLC q4f16 id, ordered top-quality → smallest, exactly one recommended', () => {
  assert.ok(LOCAL_MODELS.length >= 4);
  for (const m of LOCAL_MODELS) {
    assert.match(m.id, /-q4f16_1-MLC$/, m.id + ' is an MLC q4f16 id');
    assert.ok(m.sizeGB > 0 && m.needGB > 0, m.id + ' has sizes');
  }
  assert.equal(LOCAL_MODELS.filter((m) => m.best).length, 1, 'exactly one best/recommended');
  // Since 6dd9f49 the recommendation is decoupled from the list order: the catalog still reads
  // top-quality first, but the recommended pick is the SD-staged model that installs offline.
  assert.ok(LOCAL_MODELS.find((m) => m.best).onDevice, 'the recommended model is staged on the device SD');
  // the tail is the smallest, gira-quasi-ovunque fallback (last entry has the minimum size).
  const minSize = Math.min(...LOCAL_MODELS.map((m) => m.sizeGB));
  assert.equal(LOCAL_MODELS[LOCAL_MODELS.length - 1].sizeGB, minSize, 'smallest model is last');
});

test('default is the recommended model — the one that actually runs for most people, from the SD', () => {
  assert.equal(DEFAULT_LOCAL_MODEL, LOCAL_MODELS.find((m) => m.best).id);
  assert.ok(localModelById(DEFAULT_LOCAL_MODEL));
  // 6dd9f49: the 7B default (4.7 GB, ~6 GB VRAM) OOM'd integrated GPUs; the 1.5B ships on the SD,
  // installs with no internet, and a strong GPU still picks bigger explicitly (resolveLocalModel).
  assert.match(DEFAULT_LOCAL_MODEL, /1\.5B/);
  assert.equal(localModelById(DEFAULT_LOCAL_MODEL).onDevice, true, 'the default installs offline from the device');
});

test('resolveLocalModel: a valid explicit choice ALWAYS wins; unset/invalid → the best default', () => {
  assert.equal(resolveLocalModel('Qwen2.5-3B-Instruct-q4f16_1-MLC', 0), 'Qwen2.5-3B-Instruct-q4f16_1-MLC');
  assert.equal(resolveLocalModel(null, 99999), DEFAULT_LOCAL_MODEL);          // huge proxy still → default, never bigger than catalog
  assert.equal(resolveLocalModel('not-a-real-model', 0), DEFAULT_LOCAL_MODEL); // invalid → default
});

test('compat: no WebGPU is the ONLY hard block; a deliberate choice is never blocked by the buffer proxy', () => {
  const best = DEFAULT_LOCAL_MODEL;
  assert.equal(localModelCompat(best, { webgpu: false }).ok, false);          // genuinely can't run
  assert.equal(localModelCompat(best, { webgpu: false }).level, 'no-webgpu');
  assert.equal(localModelCompat(best, { webgpu: true, vramMB: 4096 }).ok, true);
  // a tiny per-buffer proxy → advisory 'tight', but still ok:true (the user knows their GPU)
  const tight = localModelCompat(best, { webgpu: true, vramMB: 256 });
  assert.equal(tight.ok, true);
  assert.equal(tight.level, 'tight');
  assert.equal(localModelCompat('nope', { webgpu: true }).ok, false);         // unknown id
});

test('isOutOfMemoryError recognises GPU OOM / device-lost, not ordinary errors', () => {
  for (const s of ['Out of memory', 'buffer exceeds the limit', 'maxStorageBufferBindingSize', 'WebGPU device lost', 'out of device memory'])
    assert.equal(isOutOfMemoryError(new Error(s)), true, s);
  for (const s of ['network error', '404 not found', 'fetch failed'])
    assert.equal(isOutOfMemoryError(new Error(s)), false, s);
});

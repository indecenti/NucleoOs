// Gate: ANIMA Forge — runtime-lib resolver. Offline-first (device SD) with online (CDN) fallback,
// the same policy the weights use. So a fully-provisioned SD runs with no network; otherwise CDN.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolveLib, makeHeadProbe, webllmAppConfig, makeIntegrityProbe, makeRangeProbe, expectedBytesFromManifest } from '../../apps/anima/www/forge/lib-resolver.js';

// a fake fetch whose response carries an optional content-length header (no status → ok-based path)
const fakeFetch = (ok, len) => async () => ({ ok, headers: { get: (k) => (k.toLowerCase() === 'content-length' && len != null ? String(len) : null) } });
// a richer fake modeling the device: a ranged GET → status + Content-Range "bytes 0-0/<total>"
const rangeFetch = (status, total) => async () => ({
  ok: status >= 200 && status < 300, status,
  headers: { get: (k) => (k.toLowerCase() === 'content-range' && total != null ? `bytes 0-0/${total}` : null) },
});

const LOCAL = '/apps/anima/forge/vendor/lib.wasm', CDN = 'https://cdn/lib.wasm';

test('resolveLib prefers the LOCAL (SD) url when present, else the CDN', async () => {
  assert.deepEqual(await resolveLib(LOCAL, CDN, async () => true), { url: LOCAL, source: 'sd' });
  assert.deepEqual(await resolveLib(LOCAL, CDN, async () => false), { url: CDN, source: 'cdn' });
});

test('resolveLib falls back to CDN if the probe throws (offline-safe)', async () => {
  assert.equal((await resolveLib(LOCAL, CDN, async () => { throw new Error('net'); })).source, 'cdn');
});

test('makeHeadProbe: ok→true, non-ok/throw→false', async () => {
  assert.equal(await makeHeadProbe(async () => ({ ok: true }))('u'), true);
  assert.equal(await makeHeadProbe(async () => ({ ok: false }))('u'), false);
  assert.equal(await makeHeadProbe(async () => { throw new Error('x'); })('u'), false);
});

const prebuilt = { useIndexedDBCache: false, model_list: [
  { model_id: 'OTHER', model: 'https://hf/other', model_lib: 'https://cdn/other.wasm' },
  { model_id: 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC', model: 'https://hf/qwen', model_lib: 'https://cdn/qwen.wasm' },
] };
const opts = {
  sdModelBase: '/apps/anima/forge/models/Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC/',
  hfModel: 'https://huggingface.co/mlc-ai/Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC',
  localModelLib: '/apps/anima/forge/vendor/Qwen2-0.5B-Instruct-q4f16_1_cs1k-webgpu.wasm',
  cdnModelLib: 'https://raw.githubusercontent.com/mlc-ai/binary-mlc-llm-libs/main/web-llm-models/v0_2_84/base/Qwen2-0.5B-Instruct-q4f16_1_cs1k-webgpu.wasm',
};

test('webllmAppConfig: SD present → weights + model_lib both from the device (fully offline)', async () => {
  const r = await webllmAppConfig(prebuilt, 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC', opts, async () => true);
  assert.equal(r.weightsSource, 'sd');
  assert.equal(r.libSource, 'sd');
  const e = r.appConfig.model_list.find((m) => m.model_id === 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC');
  assert.equal(e.model, opts.sdModelBase);
  assert.equal(e.model_lib, opts.localModelLib);
});

test('webllmAppConfig: SD absent → falls back to CDN/HF (still works online)', async () => {
  const r = await webllmAppConfig(prebuilt, 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC', opts, async () => false);
  assert.equal(r.weightsSource, 'cdn');
  assert.equal(r.libSource, 'cdn');
  const e = r.appConfig.model_list.find((m) => m.model_id === 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC');
  assert.equal(e.model, opts.hfModel);
  assert.equal(e.model_lib, opts.cdnModelLib);
});

test('webllmAppConfig only overrides the target model, others untouched', async () => {
  const r = await webllmAppConfig(prebuilt, 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC', opts, async () => true);
  const other = r.appConfig.model_list.find((m) => m.model_id === 'OTHER');
  assert.equal(other.model, 'https://hf/other');
  assert.equal(other.model_lib, 'https://cdn/other.wasm');
});

// ---- integrity probe: a TRUNCATED SD lib must NOT be used (fall back to CDN) ----
const sizeOf = (n) => () => n;

test('integrity probe: present + correct size → true', async () => {
  assert.equal(await makeIntegrityProbe(fakeFetch(true, 100), sizeOf(100))('u'), true);
});

test('integrity probe: present but TRUNCATED (size mismatch) → false (treated absent)', async () => {
  assert.equal(await makeIntegrityProbe(fakeFetch(true, 40), sizeOf(100))('u'), false);
});

test('integrity probe: 404 → false; throw → false', async () => {
  assert.equal(await makeIntegrityProbe(fakeFetch(false, 100), sizeOf(100))('u'), false);
  assert.equal(await makeIntegrityProbe(async () => { throw new Error('net'); }, sizeOf(100))('u'), false);
});

test('integrity probe: no expected size → presence-only true; expected-but-unmeasurable → fail-closed false', async () => {
  assert.equal(await makeIntegrityProbe(fakeFetch(true, 100), () => null)('u'), true);    // no ground truth → trust presence
  assert.equal(await makeIntegrityProbe(fakeFetch(true, null), sizeOf(100))('u'), false); // expected size but none reported → fail-closed (SD)
});

test('resolveLib with an integrity probe falls back to CDN on a truncated SD file', async () => {
  const probe = makeIntegrityProbe(fakeFetch(true, 40), sizeOf(100));   // SD file is short
  assert.equal((await resolveLib('/sd/lib.wasm', 'https://cdn/lib.wasm', probe)).source, 'cdn');
});

// ---- three-state ranged probe (device transport): 206/416/503/404 distinguished ----
test('makeRangeProbe: 206 + Content-Range total === expected → present, ok, no reason', async () => {
  const r = await makeRangeProbe(rangeFetch(206, 1000), sizeOf(1000))('u');
  assert.deepEqual({ present: r.present, ok: r.ok, reason: r.reason, size: r.size }, { present: true, ok: true, reason: null, size: 1000 });
});

test('makeRangeProbe: 206 but total < expected → truncated (present, not ok)', async () => {
  const r = await makeRangeProbe(rangeFetch(206, 400), sizeOf(1000))('u');
  assert.equal(r.present, true); assert.equal(r.ok, false); assert.equal(r.reason, 'truncated');
});

test('makeRangeProbe: 416 (0-byte placeholder) → present + empty; 404 → absent; 503 → retry (NOT absent)', async () => {
  const empty = await makeRangeProbe(rangeFetch(416, null), sizeOf(1000))('u');
  assert.equal(empty.present, true); assert.equal(empty.reason, 'empty');
  assert.equal((await makeRangeProbe(rangeFetch(404, null), sizeOf(1000))('u')).reason, 'absent');
  const retry = await makeRangeProbe(rangeFetch(503, null), sizeOf(1000))('u');
  assert.equal(retry.present, false); assert.equal(retry.ok, false); assert.equal(retry.reason, 'retry');
});

test('makeRangeProbe: a throwing fetch → absent (offline-safe), never throws out', async () => {
  const r = await makeRangeProbe(async () => { throw new Error('net'); }, sizeOf(1000))('u');
  assert.equal(r.reason, 'absent');
});

test('expectedBytesFromManifest maps base+path → bytes', () => {
  const man = { libs: [{ path: 'web-llm.js', bytes: 6086300 }, { path: 'wllama.mjs', bytes: 31416 }] };
  const f = expectedBytesFromManifest(man, '/v/');
  assert.equal(f('/v/web-llm.js'), 6086300);
  assert.equal(f('/v/wllama.mjs'), 31416);
  assert.equal(f('/v/unknown'), null);
});

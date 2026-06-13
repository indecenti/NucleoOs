// Gate: ANIMA Forge — Air-Gap Readiness auditor. The exhaustive (engine × caps × probe × weights)
// decision table: an engine is only ever called "air-gapped" when it truly runs with zero network.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { engineReadiness, readinessMatrix, readinessSummary, classBadge, offlineCount, ENGINE_DEPS } from '../../apps/anima/www/forge/readiness.js';

// A probe over a Set of present SD urls; weights-ready over a Set of ready model ids.
const probeOf = (present) => async (url) => present.has(url);
// a three-state probeDetail: present+whole for urls in `ok`, present-but-broken for urls in `bad` (reason), retry for `retry`
const detailOf = (ok, bad = {}, retry = new Set()) => async (url) => {
  if (ok.has(url)) return { present: true, ok: true, reason: null };
  if (retry.has(url)) return { present: false, ok: false, reason: 'retry' };
  if (bad[url]) return { present: true, ok: false, reason: bad[url] };
  return { present: false, ok: false, reason: 'absent' };
};
const cacheOf = (ids) => async (id) => ids.has(id);
const ON = { online: true };   // a reachable source (so missing artifacts → network-once, not unavailable)
// Device URL space: webfs maps /apps/<id>/<rest> onto www/, so vendor URLs have NO /www/ segment.
const B = '/apps/anima/forge/vendor/';
const WEBGPU_LIBS = [B + 'web-llm.js', B + 'Qwen2-0.5B-Instruct-q4f16_1_cs1k-webgpu.wasm'];
const WASM_REQ = [B + 'wllama.mjs', B + 'wllama/single-thread/wllama.wasm'];
const WASM_MT = B + 'wllama/multi-thread/wllama.wasm';
const MLC = 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC';
const GGUF = 'Qwen2.5-Coder-0.5B-Instruct-GGUF';

test('webgpu: all libs on SD + weights ready + WebGPU → air-gapped, nothing missing', async () => {
  const r = await engineReadiness('webgpu', { probe: probeOf(new Set(WEBGPU_LIBS)), weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true } });
  assert.equal(r.klass, 'air-gapped');
  assert.equal(r.missing.length, 0);
});

test('webgpu: no WebGPU → unavailable (capability), never "air-gapped"', async () => {
  const r = await engineReadiness('webgpu', { probe: probeOf(new Set(WEBGPU_LIBS)), weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: false } });
  assert.equal(r.klass, 'unavailable');
  assert.equal(r.missing[0].kind, 'capability');
  assert.equal(r.missing[0].provisionable, false);
});

test('webgpu: a lib missing + online → network-once, lists the missing lib as provisionable', async () => {
  const r = await engineReadiness('webgpu', { probe: probeOf(new Set([WEBGPU_LIBS[0]])), weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true, ...ON } });
  assert.equal(r.klass, 'network-once');
  const m = r.missing.find((x) => x.kind === 'lib');
  assert.equal(m.path, 'Qwen2-0.5B-Instruct-q4f16_1_cs1k-webgpu.wasm');
  assert.equal(m.provisionable, true);
});

test('webgpu: a required lib missing + OFFLINE → unavailable (no source to fetch it now)', async () => {
  const r = await engineReadiness('webgpu', { probe: probeOf(new Set([WEBGPU_LIBS[0]])), weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true, online: false } });
  assert.equal(r.klass, 'unavailable');
  assert.equal(r.missing.find((x) => x.kind === 'lib').provisionable, false);
});

test('webgpu: libs whole but weights absent + online → network-once (weights listed)', async () => {
  const r = await engineReadiness('webgpu', { probe: probeOf(new Set(WEBGPU_LIBS)), weightsReady: cacheOf(new Set()), caps: { webgpu: true, ...ON } });
  assert.equal(r.klass, 'network-once');
  assert.equal(r.missing.find((x) => x.kind === 'weights').modelId, MLC);
});

test('webgpu: libs whole but weights absent + OFFLINE → no-model (runtime ready, just needs weights)', async () => {
  const r = await engineReadiness('webgpu', { probe: probeOf(new Set(WEBGPU_LIBS)), weightsReady: cacheOf(new Set()), caps: { webgpu: true, online: false } });
  assert.equal(r.klass, 'no-model');
});

test('wasm: esm+single-thread present + weights ready, NOT cross-origin isolated → air-gapped (mt optional)', async () => {
  const r = await engineReadiness('wasm', { probe: probeOf(new Set(WASM_REQ)), weightsReady: cacheOf(new Set([GGUF])), caps: { wasm: true, crossOriginIsolated: false } });
  assert.equal(r.klass, 'air-gapped');
  assert.equal(r.missing.length, 0);
  assert.ok(r.notes.some((n) => /single-thread/i.test(n)));
});

test('wasm: cross-origin isolated but multi-thread lib missing + online → network-once (mt becomes required)', async () => {
  const r = await engineReadiness('wasm', { probe: probeOf(new Set(WASM_REQ)), weightsReady: cacheOf(new Set([GGUF])), caps: { wasm: true, crossOriginIsolated: true, ...ON } });
  assert.equal(r.klass, 'network-once');
  assert.ok(r.missing.some((m) => m.path === 'wllama/multi-thread/wllama.wasm'));
});

test('wasm: cross-origin isolated WITH multi-thread present → air-gapped', async () => {
  const r = await engineReadiness('wasm', { probe: probeOf(new Set([...WASM_REQ, WASM_MT])), weightsReady: cacheOf(new Set([GGUF])), caps: { wasm: true, crossOriginIsolated: true } });
  assert.equal(r.klass, 'air-gapped');
});

test('wasm: no WebAssembly → unavailable', async () => {
  const r = await engineReadiness('wasm', { probe: probeOf(new Set(WASM_REQ)), weightsReady: cacheOf(new Set([GGUF])), caps: { wasm: false } });
  assert.equal(r.klass, 'unavailable');
});

test('cloud: online + key → cloud (network every run); else unavailable', async () => {
  assert.equal((await engineReadiness('cloud', { caps: { online: true, hasKey: true } })).klass, 'cloud');
  assert.equal((await engineReadiness('cloud', { caps: { online: true, hasKey: false } })).klass, 'unavailable');
  assert.equal((await engineReadiness('cloud', { caps: { online: false, hasKey: true } })).klass, 'unavailable');
});

test('device floor and demo are always air-gapped', async () => {
  assert.equal((await engineReadiness('device', { caps: {} })).klass, 'air-gapped');
  assert.equal((await engineReadiness('demo', { caps: {} })).klass, 'air-gapped');
});

test('a throwing probe is treated as absent, never crashes (online → network-once, not air-gapped)', async () => {
  const r = await engineReadiness('webgpu', { probe: async () => { throw new Error('offline'); }, weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true, ...ON } });
  assert.equal(r.klass, 'network-once');
  assert.equal(r.missing.filter((m) => m.kind === 'lib').length, 2);
});

// ---- the corrupt class: a present-but-TRUNCATED SD lib must NOT be called air-gapped ----
test('webgpu: a required lib present but TRUNCATED → corrupt (re-sync), never air-gapped', async () => {
  const r = await engineReadiness('webgpu', {
    probeDetail: detailOf(new Set([WEBGPU_LIBS[0]]), { [WEBGPU_LIBS[1]]: 'truncated' }),
    weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true, ...ON },
  });
  assert.equal(r.klass, 'corrupt');
  const m = r.missing.find((x) => x.kind === 'lib');
  assert.equal(m.reason, 'truncated');
});

test('corrupt outranks a missing-weights/network-once verdict (most-severe wins)', async () => {
  const r = await engineReadiness('webgpu', {
    probeDetail: detailOf(new Set([WEBGPU_LIBS[0]]), { [WEBGPU_LIBS[1]]: 'truncated' }),
    weightsReady: cacheOf(new Set()), caps: { webgpu: true, ...ON },   // weights also absent
  });
  assert.equal(r.klass, 'corrupt');
});

test('a 503 (device busy) on a lib is "retry", not absent → network-once with a checking note, not corrupt/air-gapped', async () => {
  const r = await engineReadiness('webgpu', {
    probeDetail: detailOf(new Set([WEBGPU_LIBS[0]]), {}, new Set([WEBGPU_LIBS[1]])),
    weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true, ...ON },
  });
  assert.equal(r.klass, 'network-once');
  assert.ok(r.notes.some((n) => /busy/i.test(n)));
});

test('a 503 (retry) on an OPTIONAL lib variant does NOT demote a truly air-gapped engine (regression)', async () => {
  // wasm, NOT cross-origin isolated → single-thread is required, multi-thread is OPTIONAL. The optional
  // multi-thread binary returns a transient 'retry'. Required libs + weights are whole → must stay air-gapped.
  const r = await engineReadiness('wasm', {
    probeDetail: detailOf(new Set(WASM_REQ), {}, new Set([WASM_MT])),
    weightsReady: cacheOf(new Set([GGUF])), caps: { wasm: true, crossOriginIsolated: false, online: false },
  });
  assert.equal(r.klass, 'air-gapped');
});

test('readinessSummary overall ranks corrupt ABOVE no-model (a corrupt SD is not hidden behind "No model")', async () => {
  const m = [
    await engineReadiness('webgpu', { probeDetail: detailOf(new Set([WEBGPU_LIBS[0]]), { [WEBGPU_LIBS[1]]: 'truncated' }), weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true, online: true } }),
    await engineReadiness('wasm', { probeDetail: detailOf(new Set(WASM_REQ)), weightsReady: cacheOf(new Set()), caps: { wasm: true, online: false } }),  // no-model
  ];
  assert.equal(m[0].klass, 'corrupt');
  assert.equal(m[1].klass, 'no-model');
  const s = readinessSummary(m);
  assert.equal(s.overall, 'corrupt');             // not 'no-model'
  assert.equal(classBadge(s.overall).tone, 'bad');
});

test('OFFLINE + a REQUIRED lib returning 503 (retry) → unavailable, NOT network-once (no network can fix a busy device)', async () => {
  const r = await engineReadiness('webgpu', {
    probeDetail: detailOf(new Set([WEBGPU_LIBS[0]]), {}, new Set([WEBGPU_LIBS[1]])),  // required model_lib is busy
    weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true, online: false },
  });
  assert.equal(r.klass, 'unavailable');
  assert.ok(r.notes.some((n) => /busy/i.test(n)));
});

test('classBadge maps every class; air-gapped is the only "ok" tone, corrupt is "bad"', async () => {
  assert.equal(classBadge('air-gapped').tone, 'ok');
  assert.equal(classBadge('network-once').tone, 'warn');
  assert.equal(classBadge('no-model').tone, 'warn');
  assert.equal(classBadge('corrupt').tone, 'bad');
  assert.equal(classBadge('cloud').tone, 'net');
  assert.equal(classBadge('unavailable').tone, 'off');
  assert.equal(classBadge('bogus').tone, 'off');   // unknown → safe default
});

test('readinessMatrix returns one verdict per engine in order', async () => {
  const ids = Object.keys(ENGINE_DEPS);
  const m = await readinessMatrix(ids, { probe: probeOf(new Set()), weightsReady: cacheOf(new Set()), caps: { webgpu: true, wasm: true, online: true, hasKey: true } });
  assert.deepEqual(m.map((x) => x.id), ids);
});

test('readinessSummary: bestEngine + honest one-liner; offlineCount counts only air-gapped', async () => {
  // webgpu fully provisioned → air-gapped; everything else absent
  const ctx = { probe: probeOf(new Set(WEBGPU_LIBS)), weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true, online: true } };
  const m = await readinessMatrix(['webgpu', 'wasm', 'device', 'demo'], ctx);
  const s = readinessSummary(m);
  assert.equal(s.bestEngine, 'webgpu');
  assert.ok(/air-gap ready/i.test(s.line));
  assert.ok(offlineCount(m) >= 1);
});

test('readinessSummary flags a corrupt SD when no engine is air-gapped', async () => {
  const m = await readinessMatrix(['webgpu'], {
    probeDetail: detailOf(new Set([WEBGPU_LIBS[0]]), { [WEBGPU_LIBS[1]]: 'truncated' }),
    weightsReady: cacheOf(new Set([MLC])), caps: { webgpu: true, online: true },
  });
  const s = readinessSummary(m);
  assert.equal(s.overall, 'corrupt');
  assert.ok(/re-sync/i.test(s.line));
});

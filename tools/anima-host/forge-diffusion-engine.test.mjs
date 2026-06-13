// forge-diffusion-engine.test.mjs — deterministic host gate for Paint's Atelier engine stack: the CLIP BPE
// tokenizer, the WebGPU/WASM probe, the MockEngine (procedural fallback) and the REAL onnxruntime-web
// pipeline orchestration (with mock sessions). No GPU, no weights, no network — proves the logic and, above
// all, SEED-DETERMINISM ("quasi deterministico"): identical inputs ⇒ byte-identical image.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeClipTokenizer } from '../../apps/paint/www/diffusion/clip-tokenizer.js';
import { probeDiffusion } from '../../apps/paint/www/diffusion/webgpu-probe.js';
import { makeDiffusionEngine, makeMockEngine, tensor } from '../../apps/paint/www/diffusion/diffusion-engine.js';

const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

// ---- CLIP tokenizer ----
test('tokenizer: BOS/EOS framing, padding, and a forced BPE merge are deterministic', () => {
  const tok = makeClipTokenizer({ vocab: { 'ab</w>': 5 }, merges: [['a', 'b</w>']], bos: 100, eos: 101, maxLen: 6 });
  const a = tok.encode('ab'), b = tok.encode('ab');
  assert.ok(eq([...a], [...b]), 'deterministic');
  assert.equal(a[0], 100); assert.equal(a[1], 5); assert.equal(a[2], 101);
  assert.equal(a.length, 6);
  assert.equal(tok.encode('')[0], 100);                     // empty → [bos, eos, pad…]
  assert.equal(tok.encode('')[1], 101);
});

// ---- WebGPU probe ----
test('probe: picks webgpu with VRAM, falls back to wasm, can refuse', async () => {
  const navGpu = { gpu: { requestAdapter: async () => ({ limits: { maxBufferSize: 2 * 1024 * 1024 * 1024 }, info: { vendor: 'test' } }) } };
  const p = await probeDiffusion({ navigator: navGpu });
  assert.equal(p.ep, 'webgpu'); assert.equal(p.vramMB, 2048); assert.equal(p.ok, true);
  const w = await probeDiffusion({ navigator: {} });
  assert.equal(w.ep, 'wasm'); assert.equal(w.ok, true);
  const none = await probeDiffusion({ navigator: {}, allowWasm: false });
  assert.equal(none.ok, false);
});

// ---- MockEngine: deterministic + sketch conditioning ----
test('mock engine: same prompt+seed ⇒ identical bytes; different seed ⇒ different', async () => {
  const e = makeMockEngine({ resolution: 32 }); await e.load();
  const a = await e.generate({ prompt: 'un gatto', seed: 7 });
  const b = await e.generate({ prompt: 'un gatto', seed: 7 });
  const c = await e.generate({ prompt: 'un gatto', seed: 8 });
  assert.equal(a.image.width, 32); assert.equal(a.image.data.length, 32 * 32 * 4);
  assert.ok(eq([...a.image.data], [...b.image.data]), 'same seed → identical');
  assert.ok(!eq([...a.image.data], [...c.image.data]), 'different seed → different');
  assert.equal(a.image.data[3], 255, 'opaque alpha');
});

test('mock engine: a dark sketch stroke darkens those pixels (sketch→image conditioning)', async () => {
  const e = makeMockEngine({ resolution: 16 }); await e.load();
  const W = 16, H = 16, sk = { width: W, height: H, data: new Uint8ClampedArray(W * H * 4).fill(255) };
  // draw a black pixel at (8,8)
  const p = (8 * W + 8) * 4; sk.data[p] = 0; sk.data[p + 1] = 0; sk.data[p + 2] = 0;
  const plain = await e.generate({ prompt: 'x', seed: 3 });
  const drawn = await e.generate({ prompt: 'x', seed: 3, sketch: sk });
  const sum = (img, i) => img.data[i] + img.data[i + 1] + img.data[i + 2];
  assert.ok(sum(drawn.image, p) < sum(plain.image, p), 'stroke pixel is darker with the sketch');
});

// ---- Real ONNX pipeline orchestration (mock sessions) ----
function mockHash(feeds) { let h = 2166136261 >>> 0; for (const k of Object.keys(feeds).sort()) { const d = feeds[k].data; for (let i = 0; i < d.length; i++) { h ^= (d[i] | 0); h = Math.imul(h, 16777619); } } return h >>> 0; }
function fillFrom(seed, n) { let s = seed >>> 0; const d = new Float32Array(n); for (let i = 0; i < n; i++) { s = (s * 1664525 + 1013904223) >>> 0; d[i] = (s / 4294967296) * 2 - 1; } return d; }

function makeMockSessions(order) {
  const res = 8;
  return {
    'text-encoder': { run: async (f) => { order.push('text'); return { last_hidden_state: tensor(fillFrom(mockHash(f), 77 * 4), [1, 77, 4]) }; } },
    'controlnet': { run: async (f) => { order.push('control'); assert.ok(f.controlnet_cond, 'sketch hint passed to controlnet'); const s = mockHash(f); return { down_block_res_samples: tensor(fillFrom(s, 8), [8]), mid_block_res_sample: tensor(fillFrom(s ^ 1, 4), [4]) }; } },
    'unet': { run: async (f) => { order.push('unet'); assert.ok(f.down_block_additional_residuals && f.mid_block_additional_residual, 'control residuals reach unet'); return { out_sample: tensor(fillFrom(mockHash(f), 4 * 1 * 1), [1, 4, 1, 1]) }; } },
    'vae-decoder': { run: async (f) => { order.push('vae'); return { sample: tensor(fillFrom(mockHash(f), 3 * res * res), [1, 3, res, res]) }; } },
  };
}

function realEngine(order, ep = 'webgpu') {
  const sessions = makeMockSessions(order);
  const createSession = async (role) => sessions[role];
  const loadComponent = async () => ({ ok: true, buffer: new ArrayBuffer(8) });
  const tokenizer = { encode: (s) => Int32Array.from({ length: 77 }, (_, i) => (s.charCodeAt(i % Math.max(1, s.length)) || 0) + i) };
  const config = { resolution: 8, ep, components: [
    { role: 'text-encoder' }, { role: 'controlnet' }, { role: 'unet' }, { role: 'vae-decoder' },
  ] };
  return makeDiffusionEngine({ createSession, loadComponent, tokenizer, config });
}

test('real engine: loads, runs text→control→unet→vae in order, returns an RGBA image', async () => {
  const order = [];
  const e = realEngine(order);
  const ld = await e.load(); assert.equal(ld.ok, true);
  const W = 8, sk = { width: W, height: W, data: new Uint8ClampedArray(W * W * 4).fill(255) };
  const r = await e.generate({ prompt: 'a red dragon', seed: 5, sketch: sk });
  assert.deepEqual(order, ['text', 'control', 'unet', 'vae']);
  assert.equal(r.image.width, 8); assert.equal(r.image.data.length, 8 * 8 * 4);
  assert.equal(r.meta.steps, 1); assert.equal(r.image.data[3], 255);
});

test('real engine: seed-deterministic (same seed ⇒ identical, different ⇒ different)', async () => {
  const e = realEngine([]); await e.load();
  const r1 = await e.generate({ prompt: 'castle', seed: 11 });
  const r2 = await e.generate({ prompt: 'castle', seed: 11 });
  const r3 = await e.generate({ prompt: 'castle', seed: 12 });
  assert.ok(eq([...r1.image.data], [...r2.image.data]), 'same seed → identical');
  assert.ok(!eq([...r1.image.data], [...r3.image.data]), 'different seed → different');
});

test('model controls: negative prompt and controlScale change the result deterministically', async () => {
  const e = makeMockEngine({ resolution: 16 }); await e.load();
  const a = await e.generate({ prompt: 'cat', seed: 5 });
  const b = await e.generate({ prompt: 'cat', negativePrompt: 'blurry, deformed', seed: 5 });
  assert.ok(!eq([...a.image.data], [...b.image.data]), 'negative prompt alters output');
  const W = 16, sk = { width: W, height: W, data: new Uint8ClampedArray(W * W * 4).fill(255) };
  const p = (8 * W + 8) * 4; sk.data[p] = sk.data[p + 1] = sk.data[p + 2] = 0;     // a black stroke
  const lo = await e.generate({ prompt: 'x', seed: 3, sketch: sk, controlScale: 0.2 });
  const hi = await e.generate({ prompt: 'x', seed: 3, sketch: sk, controlScale: 1.0 });
  const sum = (im, i) => im.data[i] + im.data[i + 1] + im.data[i + 2];
  assert.ok(sum(hi.image, p) < sum(lo.image, p), 'higher controlScale → stronger sketch adherence');
});

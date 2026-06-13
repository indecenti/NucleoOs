// paint-diffusion-engine.test.mjs — host gate for Paint's OFFLINE in-browser image engines
// (apps/paint/www/diffusion/diffusion-engine.js): the procedural preview AND the real ONNX pipelines
// (sketch-ControlNet split-graph + lsb fused), proven via injected mock ORT sessions + tokenizer so the
// orchestration (tokenize → control → unet → vae, seeded latent, tensor wiring, RGBA decode) is exercised
// on REAL prompts without weights or WebGPU. Determinism ("same seed ⇒ same image") is asserted directly.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeMockEngine, makeDiffusionEngine, makeFusedEngine } from '../../apps/paint/www/diffusion/diffusion-engine.js';
import { PROMPTS, NEGATIVES } from './paint-prompt-corpus.mjs';

const RES = 16;
const bytes = (im) => [...im.data];
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const lumAt = (im, x, y) => { const i = (y * im.width + x) * 4; return 0.299 * im.data[i] + 0.587 * im.data[i + 1] + 0.114 * im.data[i + 2]; };

// ───────────────────────── procedural PREVIEW (the honest non-AI fallback) ─────────────────────────
test('mock: dimensions + determinism (same seed+prompt ⇒ identical, different seed ⇒ different)', async () => {
  const eng = makeMockEngine({ resolution: RES }); await eng.load();
  const a = await eng.generate({ prompt: 'un gatto rosso', seed: 42 });
  const b = await eng.generate({ prompt: 'un gatto rosso', seed: 42 });
  const c = await eng.generate({ prompt: 'un gatto rosso', seed: 43 });
  assert.equal(a.image.width, RES); assert.equal(a.image.height, RES);
  assert.ok(eq(bytes(a.image), bytes(b.image)), 'deterministic per seed');
  assert.ok(!eq(bytes(a.image), bytes(c.image)), 'different seed ⇒ different image');
});

test('mock: a different negative prompt changes the output', async () => {
  const eng = makeMockEngine({ resolution: RES }); await eng.load();
  const a = await eng.generate({ prompt: 'un drago', negativePrompt: NEGATIVES[0], seed: 7 });
  const b = await eng.generate({ prompt: 'un drago', negativePrompt: NEGATIVES[1], seed: 7 });
  assert.ok(!eq(bytes(a.image), bytes(b.image)));
});

test('mock: sketch conditioning DARKENS strokes, stronger with higher controlScale', async () => {
  const eng = makeMockEngine({ resolution: RES }); await eng.load();
  const sketch = { width: RES, height: RES, data: new Uint8ClampedArray(RES * RES * 4) };
  for (let i = 0; i < RES * RES; i++) { sketch.data[i * 4 + 3] = 255; sketch.data[i * 4] = sketch.data[i * 4 + 1] = sketch.data[i * 4 + 2] = 255; } // white
  const sx = 4, sy = 4, p = (sy * RES + sx) * 4; sketch.data[p] = sketch.data[p + 1] = sketch.data[p + 2] = 0; // one black stroke pixel
  const plain = (await eng.generate({ prompt: 'x', seed: 5 })).image;
  const soft  = (await eng.generate({ prompt: 'x', seed: 5, sketch, controlScale: 0.5 })).image;
  const hard  = (await eng.generate({ prompt: 'x', seed: 5, sketch, controlScale: 1 })).image;
  assert.ok(lumAt(soft, sx, sy) < lumAt(plain, sx, sy), 'stroke darkened vs no-sketch');
  assert.ok(lumAt(hard, sx, sy) < lumAt(soft, sx, sy), 'higher controlScale ⇒ darker');
});

test('mock: every corpus prompt produces a full opaque RGBA buffer', async () => {
  const eng = makeMockEngine({ resolution: RES }); await eng.load();
  for (const pr of PROMPTS) {
    const { image, meta } = await eng.generate({ prompt: pr.text, seed: 1 });
    assert.equal(image.data.length, RES * RES * 4);
    assert.equal(image.data[3], 255, 'alpha opaque');
    assert.equal(meta.note, 'procedural-fallback');
  }
});

// ───────────────────────── real SPLIT-GRAPH engine (text→control→unet→vae) ─────────────────────────
function recordingTokenizer() { const seen = []; return { seen, encode: (s) => { seen.push(s); return Array.from(String(s)).slice(0, 8).map((ch) => ch.charCodeAt(0)); } }; }

function splitSessions(res) {
  const calls = { text: [], control: [], unet: [], vae: [] };
  const T = (data, dims) => ({ data, dims, type: 'float32' });
  const createSession = async (role) => ({
    async run(feeds) {
      if (role === 'text-encoder') { calls.text.push(Object.keys(feeds)); return { last_hidden_state: T(new Float32Array(77 * 8), [1, 77, 8]) }; }
      if (role === 'controlnet') { calls.control.push(Object.keys(feeds)); return { down_block_res_samples: T(new Float32Array(8), [8]), mid_block_res_sample: T(new Float32Array(8), [8]) }; }
      if (role === 'unet') { calls.unet.push(Object.keys(feeds)); return { out_sample: feeds.sample }; }              // echo the seeded latent
      if (role === 'vae-decoder') {
        calls.vae.push(Object.keys(feeds)); const lat = feeds.latent_sample.data, n = 3 * res * res, out = new Float32Array(n);
        for (let i = 0; i < n; i++) out[i] = Math.sin(lat[i % lat.length] * 3 + i * 0.013);                          // [-1,1], deterministic from the latent
        return { sample: T(out, [1, 3, res, res]) };
      }
      throw new Error('unexpected role ' + role);
    },
  });
  return { createSession, calls };
}
const splitCfg = (res) => ({ resolution: res, ep: 'wasm', components: [{ role: 'text-encoder' }, { role: 'controlnet' }, { role: 'unet' }, { role: 'vae-decoder' }, { role: 'tokenizer' }] });
const okLoad = async () => ({ ok: true, buffer: new ArrayBuffer(8) });

test('split engine: load wires sessions; generate tokenizes + runs text→control→unet→vae', async () => {
  const tok = recordingTokenizer(); const { createSession, calls } = splitSessions(RES);
  const eng = makeDiffusionEngine({ createSession, loadComponent: okLoad, tokenizer: tok, config: splitCfg(RES) });
  assert.equal((await eng.load()).ok, true);
  const out = await eng.generate({ prompt: 'un gatto rosso seduto', seed: 9 });
  assert.equal(tok.seen[0], 'un gatto rosso seduto', 'tokenizer received the prompt');
  assert.ok(calls.text.length && calls.text[0].includes('input_ids'), 'text encoder fed input_ids');
  assert.ok(calls.control.length && calls.control[0].includes('controlnet_cond'), 'controlnet fed the sketch hint');
  assert.ok(calls.unet.length, 'unet ran');
  assert.ok(calls.vae.length && calls.vae[0].includes('latent_sample'), 'vae decoded the latent');
  assert.equal(out.image.width, RES); assert.equal(out.image.data.length, RES * RES * 4);
  assert.equal(out.image.data[3], 255, 'alpha opaque');
});

test('split engine: deterministic per seed (same seed ⇒ identical, different ⇒ different)', async () => {
  const mk = () => makeDiffusionEngine({ createSession: splitSessions(RES).createSession, loadComponent: okLoad, tokenizer: recordingTokenizer(), config: splitCfg(RES) });
  const e1 = mk(); await e1.load(); const e2 = mk(); await e2.load();
  const a = await e1.generate({ prompt: 'un drago', seed: 11 });
  const b = await e2.generate({ prompt: 'un drago', seed: 11 });
  const c = await e1.generate({ prompt: 'un drago', seed: 12 });
  assert.ok(eq(bytes(a.image), bytes(b.image)), 'same seed across instances ⇒ identical');
  assert.ok(!eq(bytes(a.image), bytes(c.image)), 'different seed ⇒ different');
});

test('split engine: runs the whole real-prompt corpus', async () => {
  const eng = makeDiffusionEngine({ createSession: splitSessions(RES).createSession, loadComponent: okLoad, tokenizer: recordingTokenizer(), config: splitCfg(RES) });
  await eng.load();
  for (const pr of PROMPTS) { const out = await eng.generate({ prompt: pr.text, negativePrompt: NEGATIVES[0], seed: 3 }); assert.equal(out.image.data.length, RES * RES * 4); }
});

// ───────────────────────── real FUSED engine (lsb single-graph) ─────────────────────────
function fusedSessions(res) {
  const calls = { text: [], fused: [] };
  const createSession = async (role) => ({
    async run(feeds) {
      if (role === 'text-encoder') { calls.text.push(Object.keys(feeds)); return { output_embeddings: { data: new Float32Array(77 * 8), dims: [1, 77, 8], type: 'float32' } }; }
      if (role === 'fused') {
        calls.fused.push({ keys: Object.keys(feeds), scale: feeds.conditioning_scale.data[0] });
        const lat = feeds.latents.data, n = res * res * 3, out = new Uint8Array(n);
        for (let i = 0; i < n; i++) out[i] = Math.floor((Math.sin(lat[i % lat.length] * 5 + i * 0.017) * 0.5 + 0.5) * 255);
        return { output_image: { data: out, dims: [res, res, 3], type: 'uint8' } };
      }
      throw new Error('unexpected role ' + role);
    },
  });
  return { createSession, calls };
}
const fusedCfg = (res) => ({ format: 'lsb-fused', resolution: res, ep: 'wasm', components: [{ role: 'fused' }, { role: 'text-encoder' }] });

test('fused engine: generate feeds image/prompt_embeds/conditioning_scale/latents and decodes HWC→RGBA', async () => {
  const tok = recordingTokenizer(); const { createSession, calls } = fusedSessions(RES);
  const eng = makeFusedEngine({ createSession, loadComponent: okLoad, tokenizer: tok, config: fusedCfg(RES) });
  assert.equal((await eng.load()).ok, true);
  const out = await eng.generate({ prompt: 'logo nucleo', seed: 21, controlScale: 0.8 });
  assert.equal(tok.seen[0], 'logo nucleo');
  assert.deepEqual(calls.fused[0].keys.sort(), ['conditioning_scale', 'image', 'latents', 'prompt_embeds'].sort());
  assert.ok(Math.abs(calls.fused[0].scale - 0.8) < 1e-6, 'controlScale → conditioning_scale');
  assert.equal(out.image.width, RES); assert.equal(out.image.data.length, RES * RES * 4);
  assert.equal(out.image.data[3], 255, 'alpha forced opaque');
});

test('fused engine: deterministic per seed + runs the real corpus', async () => {
  const mk = () => makeFusedEngine({ createSession: fusedSessions(RES).createSession, loadComponent: okLoad, tokenizer: recordingTokenizer(), config: fusedCfg(RES) });
  const e1 = mk(); await e1.load();
  const a = await e1.generate({ prompt: 'una bussola', seed: 4 });
  const b = await e1.generate({ prompt: 'una bussola', seed: 4 });
  const c = await e1.generate({ prompt: 'una bussola', seed: 5 });
  assert.ok(eq(bytes(a.image), bytes(b.image)) && !eq(bytes(a.image), bytes(c.image)));
  for (const pr of PROMPTS) { const o = await e1.generate({ prompt: pr.text, seed: 2 }); assert.equal(o.image.data.length, RES * RES * 4); }
});

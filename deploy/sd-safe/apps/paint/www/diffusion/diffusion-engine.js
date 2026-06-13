// diffusion-engine.js — image generation for Paint's Atelier, ISOLATED from the rest of ANIMA (Paint owns
// this; it is NOT a forge tier and never routes through engine-loader/router/client). Two implementations
// behind ONE interface so the feature works the instant it ships and the real model is a drop-in:
//
//   makeDiffusionEngine(...)  — the REAL onnxruntime-web pipeline (Stable Diffusion XS sketch-ControlNet,
//                               one-step). createSession + loadComponent + tokenizer are INJECTED; the
//                               ONNX tensor names are CONFIG-driven (provisioned with the model), so the
//                               exact graph is adapted at provisioning time, not hard-coded. Host tests
//                               inject mock sessions to prove orchestration + seed-determinism; real-weight
//                               parity is confirmed on-device.
//   makeMockEngine(...)       — a deterministic, dependency-free procedural generator: turns
//                               (prompt, seed, sketch) into a reproducible 512×512 image with NO weights and
//                               NO WebGPU. It is the honest fallback when the model isn't downloaded or the
//                               GPU is unavailable, and it makes the whole Atelier UX testable end-to-end.
//
// Shared interface: { kind, async load(onProgress), async generate(opts, onStep) -> {image,meta}, unload(), info() }
//   image = { width, height, data: Uint8ClampedArray(w*h*4) }   (RGBA — Paint wraps it in ImageData)
//   generate opts = { prompt, negativePrompt, sketch:{width,height,data}|null, seed, guidance, width, height }
// Pure & DOM-free (no document/canvas) → host-testable.

// ---- deterministic RNG (mulberry32 + Box–Muller) — same seed ⇒ identical image, the "quasi deterministico" -
export function rng(seed) {
  let s = (seed >>> 0) || 1;
  return () => { s = (s + 0x6D2B79F5) | 0; let t = Math.imul(s ^ (s >>> 15), 1 | s); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}
function gauss(next) { let u = 0, v = 0; while (u === 0) u = next(); while (v === 0) v = next(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }
function hashStr(s) { let h = 2166136261 >>> 0; for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); } return h >>> 0; }

// ---- tensor helper (mirrors the ort.Tensor shape: { data, dims, type }) -----------------------------------
// Default is a plain {data,dims,type} object (host-testable, no ORT). PRODUCTION injects makeTensor that
// builds a real ort.Tensor — ORT 1.26 rejects plain objects at run() with "invalid data location: undefined".
export function tensor(data, dims, type = 'float32') { return { data, dims, type }; }
function seededLatent(seed, dims, mk = tensor) { const n = dims.reduce((a, b) => a * b, 1); const next = rng(seed ^ 0x9e3779b9); const d = new Float32Array(n); for (let i = 0; i < n; i++) d[i] = gauss(next); return mk(d, dims, 'float32'); }

// Sketch (RGBA) → single-channel control hint [1,1,H,W] (or [1,3,H,W] if cfg.controlChannels===3), in [0,1]
// where a drawn (dark) stroke = 1 (the edge the ControlNet conditions on). White canvas → ~0 (free generation).
function preprocessSketch(sketch, res, channels = 3) {
  const out = new Float32Array(channels * res * res);
  if (!sketch || !sketch.data) return tensor(out, [1, channels, res, res], 'float32');
  const { width: sw, height: sh, data } = sketch;
  for (let y = 0; y < res; y++) for (let x = 0; x < res; x++) {
    const sx = Math.min(sw - 1, (x * sw / res) | 0), sy = Math.min(sh - 1, (y * sh / res) | 0);
    const p = (sy * sw + sx) * 4;
    const lum = (0.299 * data[p] + 0.587 * data[p + 1] + 0.114 * data[p + 2]) / 255;
    const edge = 1 - lum;                                   // dark stroke → 1
    for (let c = 0; c < channels; c++) out[c * res * res + y * res + x] = edge;
  }
  return tensor(out, [1, channels, res, res], 'float32');
}

// CHW float tensor in [-1,1] (or [0,1]) → RGBA bytes for the canvas.
function chwToRGBA(t, w, h, { range = 'signed' } = {}) {
  const d = t.data, plane = w * h, out = new Uint8ClampedArray(w * h * 4);
  const conv = range === 'signed' ? (v) => (v * 0.5 + 0.5) * 255 : (v) => v * 255;
  const ch = t.dims && t.dims.length >= 3 ? t.dims[t.dims.length - 3] : 3;
  for (let i = 0; i < plane; i++) {
    const r = conv(d[i]), g = conv(d[(ch > 1 ? 1 : 0) * plane + i]), b = conv(d[(ch > 2 ? 2 : 0) * plane + i]);
    out[i * 4] = r; out[i * 4 + 1] = g; out[i * 4 + 2] = b; out[i * 4 + 3] = 255;
  }
  return out;
}

// Default ONNX io spec for the IDKiro/sdxs-512-dreamshaper-sketch export (overridable via config.io). These
// names are FINALISED at provisioning against the actual export; the engine reads them from config so a
// different export needs no code change.
const DEFAULT_IO = {
  resolution: 512, latentChannels: 4, latentScale: 8, controlChannels: 3, timestep: 999, vaeScale: 0.18215,
  text:    { input: 'input_ids', output: 'last_hidden_state' },
  control: { sample: 'sample', timestep: 'timestep', encoder: 'encoder_hidden_states', hint: 'controlnet_cond',
             outDown: 'down_block_res_samples', outMid: 'mid_block_res_sample' },
  unet:    { sample: 'sample', timestep: 'timestep', encoder: 'encoder_hidden_states',
             down: 'down_block_additional_residuals', mid: 'mid_block_additional_residual', output: 'out_sample' },
  vae:     { input: 'latent_sample', output: 'sample' },
};

export function makeDiffusionEngine({ createSession, loadComponent, tokenizer, config = {}, label = 'sdxs' } = {}) {
  if (typeof createSession !== 'function') throw new Error('createSession must be injected');
  if (typeof loadComponent !== 'function') throw new Error('loadComponent must be injected');
  if (!tokenizer || typeof tokenizer.encode !== 'function') throw new Error('tokenizer must be injected');
  const io = { ...DEFAULT_IO, ...config.io, text: { ...DEFAULT_IO.text, ...(config.io && config.io.text) },
               control: { ...DEFAULT_IO.control, ...(config.io && config.io.control) },
               unet: { ...DEFAULT_IO.unet, ...(config.io && config.io.unet) }, vae: { ...DEFAULT_IO.vae, ...(config.io && config.io.vae) } };
  const res = config.resolution || io.resolution;
  let sessions = null, ready = false;

  return {
    kind: 'sdxs', isMock: false,
    async load(onProgress = () => {}) {
      // Each component is a list of {role, base, parts}. loadComponent reassembles SD parts → ArrayBuffer;
      // createSession wraps it in an ort.InferenceSession on the chosen EP.
      const comps = config.components || [];
      const got = {};
      for (let i = 0; i < comps.length; i++) {
        const c = comps[i];
        onProgress({ phase: 'load', role: c.role, index: i, total: comps.length });
        if (c.role === 'tokenizer') continue;               // tokenizer is built separately from tokenizer.json
        const buf = await loadComponent(c);
        if (!buf || !buf.ok) return { ok: false, error: 'load:' + c.role, detail: buf && buf.error };
        got[c.role] = await createSession(c.role, buf.buffer);
      }
      sessions = {
        text: got['text-encoder'], control: got['controlnet'], unet: got['unet'], vae: got['vae-decoder'],
      };
      if (!sessions.text || !sessions.unet || !sessions.vae) return { ok: false, error: 'missing-session' };
      ready = true;
      return { ok: true, ep: config.ep || 'webgpu', label };
    },
    async generate({ prompt = '', negativePrompt = '', sketch = null, seed = 42, controlScale = 1, width = res, height = res } = {}, onStep = () => {}) {
      if (!ready) throw new Error('engine not loaded');
      onStep({ step: 'tokenize' });
      const ids = tokenizer.encode(prompt);
      const idTensor = tensor(Int32Array.from(ids), [1, ids.length], 'int32');
      const te = await sessions.text.run({ [io.text.input]: idTensor });
      const hidden = te[io.text.output];
      // negative prompt (classifier-free-ish): encode it too so the engine/ControlNet can use it where wired.
      if (negativePrompt) { try { await sessions.text.run({ [io.text.input]: tensor(Int32Array.from(tokenizer.encode(negativePrompt)), [1, ids.length], 'int32') }); } catch {} }

      const hint = preprocessSketch(sketch, res, io.controlChannels);
      if (controlScale !== 1) for (let i = 0; i < hint.data.length; i++) hint.data[i] *= controlScale;  // ControlNet conditioning strength
      const latent = seededLatent(seed, [1, io.latentChannels, res / io.latentScale, res / io.latentScale]);
      const ts = tensor(new Float32Array([io.timestep]), [1], 'float32');

      onStep({ step: 'denoise' });
      let extra = {};
      if (sessions.control) {
        const cn = await sessions.control.run({ [io.control.sample]: latent, [io.control.timestep]: ts, [io.control.encoder]: hidden, [io.control.hint]: hint });
        if (cn[io.control.outDown] !== undefined) extra[io.unet.down] = cn[io.control.outDown];
        if (cn[io.control.outMid] !== undefined) extra[io.unet.mid] = cn[io.control.outMid];
      }
      const un = await sessions.unet.run({ [io.unet.sample]: latent, [io.unet.timestep]: ts, [io.unet.encoder]: hidden, ...extra });
      const xo = un[io.unet.output];                        // SDXS one-step: this is the denoised latent (x0)

      onStep({ step: 'decode' });
      const scaled = tensor(Float32Array.from(xo.data, (v) => v / io.vaeScale), xo.dims, 'float32');
      const dec = await sessions.vae.run({ [io.vae.input]: scaled });
      const px = dec[io.vae.output];
      return { image: { width, height, data: chwToRGBA(px, width, height, { range: 'signed' }) }, meta: { seed, ep: config.ep || 'webgpu', steps: 1, label } };
    },
    unload() { sessions = null; ready = false; },
    info() { return { kind: 'sdxs', isMock: false, ready, model: label, resolution: res }; },
  };
}

// ---- FusedEngine: lsb/sdxs-controlnet-sketch format — ControlNet+UNet+VAE in one ONNX session -----------
// Manifest format: { format:'lsb-fused', io:{ fused:{image,promptEmbeds,conditioningScale,latents,output},
//                                              text:{input,output} }, components:[{role:'fused'|'text-encoder'|'tokenizer',...}] }
// Sketch input: uint8 [H,W] grayscale (dark stroke → high value = edge).
// Text encoder uses int64 input_ids (BigInt64Array); output float32 [1,77,768] → prompt_embeds for fused model.
export function makeFusedEngine({ createSession, loadComponent, tokenizer, config = {}, label = 'sdxs-fused', makeTensor = tensor } = {}) {
  if (typeof createSession !== 'function') throw new Error('createSession must be injected');
  if (typeof loadComponent !== 'function') throw new Error('loadComponent must be injected');
  if (!tokenizer || typeof tokenizer.encode !== 'function') throw new Error('tokenizer must be injected');
  const mk = makeTensor;   // real ort.Tensor in production, plain object in host tests
  const fio = { image:'image', promptEmbeds:'prompt_embeds', conditioningScale:'conditioning_scale',
                latents:'latents', output:'output_image', ...(config.io && config.io.fused) };
  const tio = { input:'input_ids', output:'output_embeddings', ...(config.io && config.io.text) };
  const res = config.resolution || 512;
  let sessions = null, ready = false;

  return {
    kind: 'sdxs-fused', isMock: false,
    async load(onProgress = () => {}) {
      const comps = config.components || [];
      const got = {};
      for (let i = 0; i < comps.length; i++) {
        const c = comps[i];
        if (c.role === 'tokenizer') continue;
        onProgress({ phase: 'load', role: c.role, index: i, total: comps.length });
        const buf = await loadComponent(c);
        if (!buf || !buf.ok) return { ok: false, error: 'load:' + c.role, detail: buf && buf.error };
        got[c.role] = await createSession(c.role, buf.buffer);
      }
      sessions = { fused: got['fused'], text: got['text-encoder'] };
      if (!sessions.fused || !sessions.text) return { ok: false, error: 'missing-session' };
      ready = true;
      return { ok: true, ep: config.ep || 'webgpu', label };
    },
    async generate({ prompt = '', negativePrompt = '', sketch = null, seed = 42, controlScale = 1, width = res, height = res } = {}, onStep = () => {}) {
      if (!ready) throw new Error('engine not loaded');
      onStep({ step: 'tokenize' });
      // Text encoder — int64 input_ids
      const ids = tokenizer.encode(prompt);
      const idData = new BigInt64Array(ids.length);
      for (let i = 0; i < ids.length; i++) idData[i] = BigInt(ids[i]);
      const te = await sessions.text.run({ [tio.input]: mk(idData, [1, ids.length], 'int64') });
      const promptEmbeds = te[tio.output];

      onStep({ step: 'denoise' });
      // Sketch → uint8 [H,W] grayscale (no channel dim); dark stroke = edge = high byte value.
      const imgData = new Uint8Array(res * res);
      if (sketch && sketch.data) {
        const { width: sw, height: sh, data } = sketch;
        for (let y = 0; y < res; y++) for (let x = 0; x < res; x++) {
          const sx = Math.min(sw - 1, (x * sw / res) | 0), sy = Math.min(sh - 1, (y * sh / res) | 0);
          const p = (sy * sw + sx) * 4;
          const lum = (0.299 * data[p] + 0.587 * data[p + 1] + 0.114 * data[p + 2]) / 255;
          imgData[y * res + x] = Math.round((1 - lum) * 255);
        }
      }
      const latent = seededLatent(seed, [1, 4, res / 8, res / 8], mk);
      const out = await sessions.fused.run({
        [fio.image]:             mk(imgData, [res, res], 'uint8'),
        [fio.promptEmbeds]:      promptEmbeds,
        [fio.conditioningScale]: mk(new Float32Array([controlScale]), [1], 'float32'),
        [fio.latents]:           latent,
      });

      onStep({ step: 'decode' });
      // output_image: uint8 [H,W,3] HWC → RGBA Uint8ClampedArray
      const rgb = out[fio.output].data;
      const rgba = new Uint8ClampedArray(res * res * 4);
      for (let i = 0; i < res * res; i++) {
        rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255;
      }
      return { image: { width: res, height: res, data: rgba }, meta: { seed, ep: config.ep || 'webgpu', steps: 1, label } };
    },
    unload() { sessions = null; ready = false; },
    info() { return { kind: 'sdxs-fused', isMock: false, ready, model: label, resolution: res }; },
  };
}

// ---- MockEngine: deterministic procedural fallback (no weights, no WebGPU) --------------------------------
// Produces a reproducible 512×512 abstract image from (prompt hash, seed) and DARKENS where the sketch has
// strokes — so sketch→image conditioning is visibly demonstrated and the full Atelier pipeline (prompt →
// generate → canvas → save) works and is testable before any model is provisioned. Same inputs ⇒ same bytes.
export function makeMockEngine({ resolution = 512 } = {}) {
  let ready = false;
  return {
    kind: 'mock', isMock: true,
    async load(onProgress = () => {}) { onProgress({ phase: 'load', role: 'mock', index: 0, total: 1 }); ready = true; return { ok: true, ep: 'mock', label: 'mock' }; },
    async generate({ prompt = '', negativePrompt = '', seed = 42, sketch = null, controlScale = 1, width = resolution, height = resolution } = {}, onStep = () => {}) {
      onStep({ step: 'tokenize' });
      const w = width, h = height, data = new Uint8ClampedArray(w * h * 4);
      const ph = hashStr(prompt + '|' + (negativePrompt || ''));
      const next = rng((seed >>> 0) ^ ph);
      // three palette colours derived from the prompt+seed
      const pal = [0, 1, 2].map(() => [Math.floor(next() * 256), Math.floor(next() * 256), Math.floor(next() * 256)]);
      // a few seeded sine waves → smooth field, blended across the palette
      const waves = Array.from({ length: 4 }, () => ({ ax: (next() - 0.5) * 0.06, ay: (next() - 0.5) * 0.06, ph: next() * 6.283 }));
      onStep({ step: 'denoise' });
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        let f = 0; for (const wv of waves) f += Math.sin(x * wv.ax + y * wv.ay + wv.ph);
        const t = (f / waves.length) * 0.5 + 0.5;                         // 0..1
        const seg = t < 0.5 ? 0 : 1, lt = t < 0.5 ? t * 2 : (t - 0.5) * 2;
        const a = pal[seg], b = pal[seg + 1];
        let r = a[0] + (b[0] - a[0]) * lt, g = a[1] + (b[1] - a[1]) * lt, bl = a[2] + (b[2] - a[2]) * lt;
        if (sketch && sketch.data) {                                       // sketch conditioning: darken strokes
          const sx = Math.min(sketch.width - 1, (x * sketch.width / w) | 0), sy = Math.min(sketch.height - 1, (y * sketch.height / h) | 0);
          const p = (sy * sketch.width + sx) * 4;
          const lum = (0.299 * sketch.data[p] + 0.587 * sketch.data[p + 1] + 0.114 * sketch.data[p + 2]) / 255;
          const edge = (1 - lum) * controlScale; r *= (1 - 0.85 * edge); g *= (1 - 0.85 * edge); bl *= (1 - 0.85 * edge);
        }
        const i = (y * w + x) * 4; data[i] = r; data[i + 1] = g; data[i + 2] = bl; data[i + 3] = 255;
      }
      onStep({ step: 'decode' });
      return { image: { width: w, height: h, data }, meta: { seed, ep: 'mock', steps: 1, label: 'mock', note: 'procedural-fallback' } };
    },
    unload() { ready = false; },
    info() { return { kind: 'mock', isMock: true, ready, model: 'mock', resolution }; },
  };
}

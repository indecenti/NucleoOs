// paint-pipeline.test.mjs — host gate for the Atelier orchestrator (apps/paint/www/diffusion/pipeline.js).
// Proves the LLM-driven flow end to end on REAL prompts: optional enhance → resolve the chosen engine →
// generate N variants, with the enhanced prompt actually reaching the engine, distinct re-seeded variants,
// best-effort error handling (partial success surfaced; throw only when nothing succeeds), and sketch /
// controlScale pass-through. Engines + enhancer are injected → pure & deterministic.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeImagePipeline } from '../../apps/paint/www/diffusion/pipeline.js';
import { PROMPTS } from './paint-prompt-corpus.mjs';

function hash(s) { let h = 2166136261 >>> 0; for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); } return h >>> 0; }

// A deterministic recording engine: same (prompt, seed) ⇒ identical 4×4 image; records every call.
function makeRecordingEngine({ brand = 'mock', isOnline = false, failSeeds = [] } = {}) {
  const calls = [];
  return {
    brand, isOnline, calls,
    async generate(opts, onStep) {
      calls.push(opts);
      onStep && onStep({ step: 'tokenize' }); onStep && onStep({ step: 'decode' });
      if (failSeeds.includes(opts.seed)) { const e = new Error('boom@' + opts.seed); e.status = 500; throw e; }
      const w = 4, h = 4, data = new Uint8ClampedArray(w * h * 4);
      const base = hash(opts.prompt + '|' + opts.seed);
      for (let i = 0; i < w * h; i++) { data[i * 4] = (base >> (i % 24)) & 255; data[i * 4 + 1] = (base >> 3) & 255; data[i * 4 + 2] = (base >> 5) & 255; data[i * 4 + 3] = 255; }
      return { image: { width: w, height: h, data }, meta: { seed: opts.seed, label: brand, online: isOnline } };
    },
  };
}
const eq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

test('enhancement is wired: the engine receives the ENHANCED prompt, not the raw idea', async () => {
  const engine = makeRecordingEngine();
  const enhancer = { available: true, brand: 'Claude', enhance: async (raw) => ({ prompt: 'PRO(' + raw + ')', negative: 'lowres', source: 'llm', brand: 'Claude' }) };
  const pipe = makeImagePipeline({ resolveEngine: async () => engine, enhancer });
  const r = await pipe.run({ provider: 'online', prompt: 'un gatto rosso', enhance: true, style: 'photo' });
  assert.equal(engine.calls[0].prompt, 'PRO(un gatto rosso)');
  assert.equal(engine.calls[0].negativePrompt, 'lowres', 'enhancer negative folded in');
  assert.equal(r.enhanced.from, 'un gatto rosso');
  assert.equal(r.enhanced.to, 'PRO(un gatto rosso)');
  assert.equal(r.prompt, 'PRO(un gatto rosso)');
});

test('enhance requested but prompt empty (img mode) → enhancement skipped, engine still runs', async () => {
  const engine = makeRecordingEngine();
  let enhanceCalled = false;
  const enhancer = { available: true, brand: 'Claude', enhance: async () => { enhanceCalled = true; return { prompt: 'X', source: 'llm', brand: 'Claude' }; } };
  const pipe = makeImagePipeline({ resolveEngine: async () => engine, enhancer });
  const r = await pipe.run({ provider: 'local', prompt: '', enhance: true, seed: 1 });
  assert.equal(enhanceCalled, false, 'cannot enhance an empty idea');
  assert.equal(r.enhanced, null);
  assert.equal(engine.calls[0].prompt, '', 'engine still runs (img mode allows empty prompt)');
  assert.equal(r.results.length, 1);
});

test('enhance=false leaves the prompt untouched (raw idea reaches the engine)', async () => {
  const engine = makeRecordingEngine();
  const enhancer = { available: true, brand: 'Claude', enhance: async () => { throw new Error('should not be called'); } };
  const pipe = makeImagePipeline({ resolveEngine: async () => engine, enhancer });
  const r = await pipe.run({ provider: 'local', prompt: 'a serene lake', enhance: false });
  assert.equal(engine.calls[0].prompt, 'a serene lake');
  assert.equal(r.enhanced, null);
});

test('resolveEngine is called with the SELECTED provider id', async () => {
  let asked = null;
  const pipe = makeImagePipeline({ resolveEngine: async (id) => { asked = id; return makeRecordingEngine(); } });
  await pipe.run({ provider: 'local', prompt: 'x' });
  assert.equal(asked, 'local');
});

test('N variants: distinct consecutive seeds, deterministic, all returned', async () => {
  const engine = makeRecordingEngine();
  const pipe = makeImagePipeline({ resolveEngine: async () => engine });
  const r = await pipe.run({ provider: 'local', prompt: 'un drago', n: 4, seed: 100 });
  assert.equal(r.results.length, 4);
  assert.deepEqual(r.results.map((x) => x.seed), [100, 101, 102, 103]);
  // determinism: re-running the same seed yields identical bytes
  const again = await pipe.run({ provider: 'local', prompt: 'un drago', n: 1, seed: 100 });
  assert.ok(eq([...r.results[0].image.data], [...again.results[0].image.data]), 'same seed ⇒ identical image');
  // different seeds differ
  assert.ok(!eq([...r.results[0].image.data], [...r.results[1].image.data]), 'different seed ⇒ different image');
});

test('best-effort: a failing variant is surfaced but the others still return', async () => {
  const engine = makeRecordingEngine({ failSeeds: [51] });
  const pipe = makeImagePipeline({ resolveEngine: async () => engine });
  const r = await pipe.run({ provider: 'online', prompt: 'x', n: 3, seed: 50 });
  assert.equal(r.results.length, 2, 'two succeed');
  assert.equal(r.errors.length, 1);
  assert.equal(r.errors[0].seed, 51);
  assert.equal(r.errors[0].status, 500);
});

test('throws only when EVERY variant fails (with the mapped status)', async () => {
  const engine = makeRecordingEngine({ failSeeds: [0, 1, 2] });
  const pipe = makeImagePipeline({ resolveEngine: async () => engine });
  await assert.rejects(() => pipe.run({ provider: 'online', prompt: 'x', n: 3, seed: 0 }), (e) => e.status === 500);
});

test('sketch + controlScale pass through to the engine unchanged', async () => {
  const engine = makeRecordingEngine();
  const pipe = makeImagePipeline({ resolveEngine: async () => engine });
  const sketch = { width: 2, height: 2, data: new Uint8ClampedArray(2 * 2 * 4) };
  await pipe.run({ provider: 'local', prompt: 'x', sketch, controlScale: 0.7 });
  assert.equal(engine.calls[0].sketch, sketch);
  assert.equal(engine.calls[0].controlScale, 0.7);
});

test('stages are emitted in order: enhance → engine → generate', async () => {
  const phases = [];
  const enhancer = { available: true, brand: 'Grok', enhance: async (raw) => ({ prompt: raw + '!', source: 'llm', brand: 'Grok' }) };
  const pipe = makeImagePipeline({ resolveEngine: async () => makeRecordingEngine(), enhancer });
  await pipe.run({ provider: 'online', prompt: 'x', enhance: true, n: 1, onStage: (s) => phases.push(s.phase + ':' + (s.state || '')) });
  const order = phases.filter((p) => /^(enhance|engine|generate):/.test(p));
  assert.equal(order[0], 'enhance:active');
  assert.ok(order.includes('engine:active'));
  assert.ok(order.some((p) => p.startsWith('generate:')));
});

test('real corpus runs through all three provider kinds with enhancement', async () => {
  const enhancer = { available: true, brand: 'Claude', enhance: async (raw, o) => ({ prompt: `[${o.style}] ${raw}`, source: 'llm', brand: 'Claude' }) };
  for (const kind of ['online', 'local', 'preview']) {
    const engine = makeRecordingEngine({ isOnline: kind === 'online', brand: kind });
    const pipe = makeImagePipeline({ resolveEngine: async () => engine, enhancer });
    for (const p of PROMPTS) {
      const r = await pipe.run({ provider: kind, prompt: p.text, style: p.style, lang: p.lang, enhance: true });
      assert.equal(r.results.length, 1, `${kind}: ${p.text}`);
      assert.equal(r.online, kind === 'online');
      assert.equal(r.prompt, `[${p.style}] ${p.text}`);
      assert.equal(r.results[0].image.width, 4);
    }
  }
});

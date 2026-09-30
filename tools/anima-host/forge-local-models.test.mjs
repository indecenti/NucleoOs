// Gate: ANIMA in-browser GPU model catalog (apps/anima/www/forge/local-models.js). Pins the 2026-09 truths:
//   • current generation (Qwen3) with REAL download sizes and WebLLM's own VRAM needs;
//   • no false promise: nothing claims "on the SD / installs offline" (it was never staged — audit 2026-09);
//   • the recommendation follows the ADAPTER CLASS (discrete → 4B, integrated → 1.7B, weak → 0.6B), not a
//     per-buffer limit dressed up as VRAM; an explicit user choice always wins;
//   • a GPU without shader-f16 loads the q4f32 sibling instead of failing;
//   • legacy Qwen2.5 choices keep working (not silently switched), but aren't offered to new users.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  LOCAL_MODELS, DEFAULT_LOCAL_MODEL, localModelById, offeredModels, resolveLocalModel, recommendModel, buildFor,
  localModelCompat, isOutOfMemoryError, isShaderF16Error, stripThinking, webgpuCause,
} from '../../apps/anima/www/forge/local-models.js';

const vendored = readFileSync(new URL('../../apps/anima/www/forge/vendor/web-llm.js', import.meta.url), 'utf8');

test('catalog: every id (and its q4f32 sibling) exists in the vendored WebLLM prebuilt config', () => {
  for (const m of LOCAL_MODELS) {
    assert.match(m.id, /-q4f16_1-MLC$/, m.id);
    assert.ok(vendored.includes(`"${m.id}"`), m.id + ' missing from vendored web-llm.js');
    const f32 = m.id.replace('-q4f16_1-', '-q4f32_1-');
    assert.ok(vendored.includes(`"${f32}"`), f32 + ' (no-f16 fallback) missing from vendored web-llm.js');
    assert.ok(m.sizeGB > 0 && m.needGB > 0 && m.needF32GB >= m.needGB, m.id + ' sizes');
  }
  assert.equal(LOCAL_MODELS.filter((m) => m.best).length, 1, 'exactly one default');
});

test('no model claims an offline install from the SD any more (it was never staged)', () => {
  for (const m of LOCAL_MODELS) assert.ok(!m.onDevice, m.id + ' must not claim onDevice');
});

test('the current generation is offered; legacy only when it is the user\'s existing choice', () => {
  const ids = offeredModels(null).map((m) => m.id);
  assert.ok(ids.every((id) => id.startsWith('Qwen3-')), ids.join());
  assert.ok(offeredModels('Qwen2.5-3B-Instruct-q4f16_1-MLC').some((m) => m.id === 'Qwen2.5-3B-Instruct-q4f16_1-MLC'));
  assert.equal(DEFAULT_LOCAL_MODEL, 'Qwen3-1.7B-q4f16_1-MLC');
});

test('recommendModel follows the adapter class', () => {
  assert.equal(recommendModel({ webgpu: false }), null);
  assert.equal(recommendModel({ webgpu: true, adapterClass: 'discrete' }), 'Qwen3-4B-q4f16_1-MLC');
  assert.equal(recommendModel({ webgpu: true, adapterClass: 'integrated', deviceMemoryGB: 8 }), 'Qwen3-1.7B-q4f16_1-MLC');
  assert.equal(recommendModel({ webgpu: true, adapterClass: 'integrated', deviceMemoryGB: 4 }), 'Qwen3-0.6B-q4f16_1-MLC');
  assert.equal(recommendModel({ webgpu: true, adapterClass: 'fallback' }), 'Qwen3-0.6B-q4f16_1-MLC');
});

test('resolveLocalModel: an explicit valid choice ALWAYS wins; unset → the recommendation for this GPU', () => {
  assert.equal(resolveLocalModel('Qwen2.5-3B-Instruct-q4f16_1-MLC', { webgpu: true, adapterClass: 'discrete' }), 'Qwen2.5-3B-Instruct-q4f16_1-MLC');
  assert.equal(resolveLocalModel(null, { webgpu: true, adapterClass: 'discrete' }), 'Qwen3-4B-q4f16_1-MLC');
  assert.equal(resolveLocalModel('not-a-model', {}), DEFAULT_LOCAL_MODEL);
  assert.equal(resolveLocalModel('Qwen3-4B-q4f32_1-MLC', {}), 'Qwen3-4B-q4f16_1-MLC', 'a stored f32 build maps back to its catalog entry');
});

test('buildFor: q4f32 sibling when the GPU lacks shader-f16', () => {
  assert.equal(buildFor('Qwen3-1.7B-q4f16_1-MLC', { f16: false }), 'Qwen3-1.7B-q4f32_1-MLC');
  assert.equal(buildFor('Qwen3-1.7B-q4f16_1-MLC', { f16: true }), 'Qwen3-1.7B-q4f16_1-MLC');
  assert.equal(buildFor('Qwen3-1.7B-q4f16_1-MLC', {}), 'Qwen3-1.7B-q4f16_1-MLC', 'unknown → the f16 build');
});

test('compat: no WebGPU is the ONLY hard block; the f32 VRAM need is used without f16', () => {
  const d = DEFAULT_LOCAL_MODEL;
  assert.equal(localModelCompat(d, { webgpu: false }).level, 'no-webgpu');
  assert.equal(localModelCompat(d, { webgpu: true, adapterClass: 'discrete' }).level, 'ok');
  assert.equal(localModelCompat('Qwen3-8B-q4f16_1-MLC', { webgpu: true, adapterClass: 'integrated' }).level, 'tight', 'an 8B on an iGPU is flagged, not blocked');
  assert.equal(localModelCompat('Qwen3-8B-q4f16_1-MLC', { webgpu: true, adapterClass: 'integrated' }).ok, true);
  assert.equal(localModelCompat(d, { webgpu: true, f16: false, adapterClass: 'discrete' }).need, localModelById(d).needF32GB);
  assert.equal(localModelCompat('nope', { webgpu: true }).ok, false);
});

test('webgpuCause still says WHY (insecure page / browser / blocked adapter)', () => {
  assert.equal(webgpuCause({ webgpu: false, reason: 'no-webgpu' }, { secure: false }), 'insecure');
  assert.equal(webgpuCause({ webgpu: false, reason: 'no-webgpu' }, { secure: true }), 'browser');
  assert.equal(webgpuCause({ webgpu: false, reason: 'no-adapter' }, { secure: true }), 'blocked');
  assert.equal(webgpuCause({ webgpu: true }), null);
});

test('error classifiers + thinking residue', () => {
  for (const s of ['Out of memory', 'buffer exceeds the limit', 'WebGPU device lost']) assert.equal(isOutOfMemoryError(new Error(s)), true, s);
  assert.equal(isOutOfMemoryError(new Error('fetch failed')), false);
  assert.equal(isShaderF16Error(new Error('ShaderF16SupportError: this model requires shader-f16')), true);
  assert.equal(stripThinking('<think>\nhmm\n</think>\n\nCiao!'), 'Ciao!');
  assert.equal(stripThinking('Ciao!'), 'Ciao!');
});

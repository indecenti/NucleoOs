// paint-prompt-enhance.test.mjs — host gate for the LLM prompt enhancer (apps/paint/www/diffusion/online-image.js).
// Proves: style-aware bilingual prompt construction, robust cleanup of the LLM reply, and the SAFETY rule that
// enhancement is additive — any failure (throw / empty / no key) falls back to the original prompt, never blank.
// `ask` is injected → no network. Exercised over the real prompt corpus.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildEnhancePrompt, cleanEnhanced, makePromptEnhancer } from '../../apps/paint/www/diffusion/online-image.js';
import { PROMPTS } from './paint-prompt-corpus.mjs';

test('buildEnhancePrompt: style-aware system + bilingual note + negative', () => {
  const icon = buildEnhancePrompt('una bussola', { style: 'icon', lang: 'it' });
  assert.match(icon.system, /prompt engineer/i);
  assert.match(icon.system, /ICON/);
  assert.match(icon.user, /una bussola/);
  assert.match(icon.user, /Italian/, 'IT idea → instruct English output');

  const photo = buildEnhancePrompt('a plate of pasta', { style: 'photo', lang: 'en', negative: 'cartoon' });
  assert.match(photo.system, /PHOTOREALISTIC/);
  assert.match(photo.user, /Avoid: cartoon/);
  assert.doesNotMatch(photo.user, /Italian/, 'EN idea → no IT note');

  // unknown / missing style falls back to the generic 'image' hint (never crashes)
  assert.match(buildEnhancePrompt('x', { style: 'no-such-style' }).system, /high-quality image/);
  assert.match(buildEnhancePrompt('x', {}).system, /high-quality image/);
});

test('cleanEnhanced: strip quotes, prefixes, take first line, never crash', () => {
  assert.equal(cleanEnhanced('"a vivid red cat"'), 'a vivid red cat');
  assert.equal(cleanEnhanced('Prompt: a serene lake at dawn'), 'a serene lake at dawn');
  assert.equal(cleanEnhanced('a dragon over neon city\n\n(this is my suggestion)'), 'a dragon over neon city');
  assert.equal(cleanEnhanced('   '), '');
  assert.equal(cleanEnhanced(null), '');
});

test('makePromptEnhancer.enhance: LLM path returns a cleaned professional prompt', async () => {
  const ask = async (system, user) => {
    assert.match(system, /prompt engineer/i);
    return '"a fluffy red cat lounging on a warm sunlit sofa, cozy, watercolor style"';
  };
  const enh = makePromptEnhancer({ cfg: { provider: 'anthropic', key: 'k' }, ask, label: 'Claude' });
  assert.equal(enh.available, true);
  const r = await enh.enhance('un gatto rosso sul divano', { style: 'painting', lang: 'it' });
  assert.equal(r.source, 'llm');
  assert.equal(r.brand, 'Claude');
  assert.equal(r.prompt, 'a fluffy red cat lounging on a warm sunlit sofa, cozy, watercolor style');
});

test('makePromptEnhancer.enhance: FALLS BACK to the original on throw / empty / too-short', async () => {
  const original = 'un drago al neon';
  const onThrow = makePromptEnhancer({ cfg: { key: 'k' }, ask: async () => { throw new Error('429'); } });
  assert.equal((await onThrow.enhance(original)).prompt, original);
  assert.equal((await onThrow.enhance(original)).source, 'fallback');

  const onEmpty = makePromptEnhancer({ cfg: { key: 'k' }, ask: async () => '   ' });
  assert.equal((await onEmpty.enhance(original)).prompt, original);

  const onShort = makePromptEnhancer({ cfg: { key: 'k' }, ask: async () => 'ok' });
  assert.equal((await onShort.enhance(original)).prompt, original, 'a 2-char reply is rejected, not used');
});

test('makePromptEnhancer: no cfg / no ask → unavailable, returns the original unchanged', async () => {
  const none = makePromptEnhancer({});
  assert.equal(none.available, false);
  const r = await none.enhance('un gatto');
  assert.equal(r.prompt, 'un gatto');
  assert.equal(r.source, 'fallback');
});

test('makePromptEnhancer.enhance: real corpus — always yields a non-empty prompt', async () => {
  const ask = async (_s, user) => 'refined: ' + user.replace(/^Idea:\s*/, '').split('\n')[0];
  const enh = makePromptEnhancer({ cfg: { key: 'k' }, ask, label: 'Grok' });
  for (const p of PROMPTS) {
    const r = await enh.enhance(p.text, { style: p.style, lang: p.lang });
    assert.ok(r.prompt && r.prompt.length >= 3, `non-empty for "${p.text}"`);
    assert.equal(r.source, 'llm');
  }
});

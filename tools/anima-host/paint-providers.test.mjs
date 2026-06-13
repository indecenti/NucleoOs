// paint-providers.test.mjs — host gate for Atelier provider resolution (apps/paint/www/diffusion/providers.js).
// Proves: teacher.json (every real shape) → correct chat + online-image configs; availability gating from
// live caps; the online→local→preview default cascade; honest brand labels. Pure, deterministic, no I/O.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolveConfigs, listProviders, defaultProvider, brandOf, enhancerLabel } from '../../apps/paint/www/diffusion/providers.js';

test('brandOf: label never lies (Grok ≠ Groq ≠ Claude)', () => {
  assert.equal(brandOf({ provider: 'anthropic' }), 'Claude');
  assert.equal(brandOf({ provider: 'openai', base: 'https://api.x.ai/v1' }), 'Grok');
  assert.equal(brandOf({ provider: 'openai', base: 'https://api.groq.com/openai/v1' }), 'Groq');
  assert.equal(brandOf({ provider: 'openai', base: 'https://api.openai.com/v1' }), 'OpenAI');
  assert.equal(brandOf(null), null);
});

test('resolveConfigs: Claude-only → chat=Claude, NO online image (Anthropic has no image API)', () => {
  const { chat, image } = resolveConfigs({ provider: 'anthropic', key: 'sk-ant-xxx', model: 'claude-sonnet-4-6' });
  assert.equal(chat.provider, 'anthropic');
  assert.equal(brandOf(chat), 'Claude');
  assert.equal(image, null, 'Claude cannot generate images');
});

test('resolveConfigs: xAI Grok key → chat=Grok AND image=grok-2-image', () => {
  const { chat, image } = resolveConfigs({ keys: { xai: { key: 'xai-abc', base: 'https://api.x.ai/v1' } } });
  assert.equal(brandOf(chat), 'Grok');
  assert.ok(image && image.key === 'xai-abc');
  assert.equal(image.base, 'https://api.x.ai/v1');
  assert.equal(image.model, 'grok-2-image');
});

test('resolveConfigs: Groq-only → chat=Groq, NO online image (Groq has no image API)', () => {
  const { chat, image } = resolveConfigs({ key: 'gsk_xxx', base: 'https://api.groq.com/openai/v1' });
  assert.equal(brandOf(chat), 'Groq');
  assert.equal(image, null);
});

test('resolveConfigs: Claude chat + Grok image together (two-key vault)', () => {
  const { chat, image } = resolveConfigs({ provider: 'anthropic', key: 'sk-ant-x', keys: { anthropic: { key: 'sk-ant-x' }, xai: { key: 'xai-y' } } });
  assert.equal(brandOf(chat), 'Claude', 'Claude preferred for enhancement');
  assert.equal(brandOf(image), 'Grok', 'Grok provides the pixels');
});

test('resolveConfigs: explicit image slot (any OpenAI-images endpoint) wins', () => {
  const { image } = resolveConfigs({ keys: { image: { key: 'k', base: 'https://api.openai.com/v1', model: 'gpt-image-1' } } });
  assert.equal(image.base, 'https://api.openai.com/v1');
  assert.equal(image.model, 'gpt-image-1');
});

test('resolveConfigs: grok flat-config alias is recognised', () => {
  const { chat, image } = resolveConfigs({ provider: 'grok', key: 'xai-z', base: 'https://api.x.ai/v1' });
  assert.equal(brandOf(chat), 'Grok');
  assert.equal(brandOf(image), 'Grok');
});

test('resolveConfigs: empty / garbage → both null (no fabrication)', () => {
  for (const j of [null, {}, { keys: {} }, 'nonsense', 42]) {
    const r = resolveConfigs(j);
    assert.equal(r.chat, null);
    assert.equal(r.image, null);
  }
});

test('listProviders: online available only when image key + connection', () => {
  const img = { provider: 'openai', base: 'https://api.x.ai/v1', model: 'grok-2-image', key: 'xai' };
  const on = listProviders(img, { online: true, webgpu: true, modelCached: false, runtimeOk: true });
  assert.equal(on.find((p) => p.id === 'online').available, true);
  const off = listProviders(img, { online: false });
  assert.equal(off.find((p) => p.id === 'online').available, false, 'no connection → online unavailable');
  const noKey = listProviders(null, { online: true });
  assert.equal(noKey.find((p) => p.id === 'online').available, false, 'no key → online unavailable');
});

test('listProviders: local available only with a model present + runtime ok', () => {
  const base = { online: false };
  assert.equal(listProviders(null, { ...base, modelCached: true, runtimeOk: true }).find((p) => p.id === 'local').available, true);
  assert.equal(listProviders(null, { ...base, modelOnSD: true, runtimeOk: true }).find((p) => p.id === 'local').available, true);
  assert.equal(listProviders(null, { ...base, modelCached: false, runtimeOk: true }).find((p) => p.id === 'local').available, false);
  assert.equal(listProviders(null, { ...base, modelCached: true, runtimeOk: false }).find((p) => p.id === 'local').available, false);
});

test('listProviders: preview is always available and honestly non-AI', () => {
  const list = listProviders(null, { online: false, runtimeOk: false });
  const prev = list.find((p) => p.id === 'preview');
  assert.equal(prev.available, true);
  assert.match(prev.reason, /non IA|not AI/);
});

test('defaultProvider: cascade online → local → preview, honouring an available preference', () => {
  const img = { key: 'xai', base: 'https://api.x.ai/v1', model: 'grok-2-image' };
  const all = listProviders(img, { online: true, modelCached: true, runtimeOk: true, webgpu: true });
  assert.equal(defaultProvider(all), 'online', 'best = online when everything is up');
  const noNet = listProviders(img, { online: false, modelCached: true, runtimeOk: true });
  assert.equal(defaultProvider(noNet), 'local', 'falls to local in-browser model when offline');
  const bare = listProviders(null, { online: false, modelCached: false, runtimeOk: false });
  assert.equal(defaultProvider(bare), 'preview', 'falls to preview only as last resort');
  // an available preference overrides the cascade; an unavailable one is ignored
  assert.equal(defaultProvider(all, 'local'), 'local');
  assert.equal(defaultProvider(noNet, 'online'), 'local', 'unavailable preference ignored');
});

test('enhancerLabel: present only with a chat LLM, brand-accurate + bilingual', () => {
  const { chat } = resolveConfigs({ provider: 'anthropic', key: 'sk-ant-x' });
  assert.match(enhancerLabel(chat, 'it'), /Claude/);
  assert.match(enhancerLabel(chat, 'en'), /enhance with Claude/);
  assert.equal(enhancerLabel(null), null);
});

test('brandOf: Gemini labelled by provider OR base', () => {
  assert.equal(brandOf({ provider: 'google' }), 'Gemini');
  assert.equal(brandOf({ provider: 'openai', base: 'https://generativelanguage.googleapis.com/v1beta/openai' }), 'Gemini');
});

test('resolveConfigs: Gemini active → chat=Gemini via proxy, NO online image (no image API)', () => {
  const { chat, image } = resolveConfigs({ provider: 'google', key: 'AIzaX', model: 'gemini-2.5-flash' });
  assert.equal(brandOf(chat), 'Gemini');
  assert.equal(chat.proxy, true, 'CORS-blocked → routed via the device /api/llm proxy');
  assert.equal(image, null, 'Gemini has no image API → pixels fall back to local/preview');
});

test('resolveConfigs: a saved Gemini key does NOT override an active browser-direct brain (Claude)', () => {
  const { chat } = resolveConfigs({ provider: 'anthropic', key: 'sk-ant-x', keys: { anthropic: { key: 'sk-ant-x' }, google: { key: 'AIzaY' } } });
  assert.equal(brandOf(chat), 'Claude', 'browser-direct preferred; Gemini (proxy) only when it is the active/only key');
});

test('resolveConfigs: Gemini-only saved key (not active) is still usable as last resort', () => {
  const { chat } = resolveConfigs({ keys: { google: { key: 'AIzaZ' } } });
  assert.equal(brandOf(chat), 'Gemini');
  assert.equal(chat.proxy, true);
});

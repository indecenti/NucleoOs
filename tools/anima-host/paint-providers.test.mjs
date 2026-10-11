// paint-providers.test.mjs — host gate for Atelier provider resolution (apps/paint/www/diffusion/providers.js).
// Proves: teacher.json (every real shape) → the right ONLINE image config; availability gating from live caps
// (connection, Private mode, local model, runtime); the online→local→preview default cascade; honest brand labels;
// every reason in the five OS languages. The prompt-enhancing LLM is chosen by the OS-wide decision
// (web/shell/ai-ask.js, tools/shell-ai-ask.test.mjs), not here. Pure, deterministic, no I/O.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolveConfigs, listProviders, defaultProvider, brandOf, enhancerLabel, providerText } from '../../apps/paint/www/diffusion/providers.js';

test('brandOf: label never lies (Grok ≠ Groq ≠ Claude)', () => {
  assert.equal(brandOf({ provider: 'anthropic' }), 'Claude');
  assert.equal(brandOf({ provider: 'openai', base: 'https://api.x.ai/v1' }), 'Grok');
  assert.equal(brandOf({ provider: 'openai', base: 'https://api.groq.com/openai/v1' }), 'Groq');
  assert.equal(brandOf({ provider: 'openai', base: 'https://api.openai.com/v1' }), 'OpenAI');
  assert.equal(brandOf({ provider: 'google' }), 'Gemini');
  assert.equal(brandOf({ provider: 'openai', base: 'https://generativelanguage.googleapis.com/v1beta/openai' }), 'Gemini');
  assert.equal(brandOf(null), null);
});

test('resolveConfigs: no image API → no online image (Claude, Groq, Gemini)', () => {
  assert.equal(resolveConfigs({ provider: 'anthropic', key: 'sk-ant-xxx' }).image, null, 'Claude cannot generate images');
  assert.equal(resolveConfigs({ key: 'gsk_xxx', base: 'https://api.groq.com/openai/v1' }).image, null, 'Groq has no image API');
  assert.equal(resolveConfigs({ provider: 'google', key: 'AIzaX' }).image, null, 'Gemini has no image API here');
});

test('resolveConfigs: an xAI key gives the online image engine — as its own slot, the grok alias, or next to Claude', () => {
  const { image } = resolveConfigs({ keys: { xai: { key: 'xai-abc', base: 'https://api.x.ai/v1' } } });
  assert.ok(image && image.key === 'xai-abc');
  assert.equal(image.base, 'https://api.x.ai/v1');
  assert.equal(image.model, 'grok-2-image', 'a first guess; the live list re-picks at generation time');
  assert.equal(brandOf(resolveConfigs({ provider: 'grok', key: 'xai-z', base: 'https://api.x.ai/v1' }).image), 'Grok');
  const two = resolveConfigs({ provider: 'anthropic', key: 'sk-ant-x', keys: { anthropic: { key: 'sk-ant-x' }, xai: { key: 'xai-y' } } });
  assert.equal(brandOf(two.image), 'Grok', 'Grok provides the pixels');
  assert.ok(!('chat' in two), 'the chat LLM is the OS-wide decision now (ai-ask.js), not a copy here');
});

test('resolveConfigs: explicit image slot (any OpenAI-images endpoint) wins', () => {
  const { image } = resolveConfigs({ keys: { image: { key: 'k', base: 'https://api.openai.com/v1', model: 'gpt-image-1' } } });
  assert.equal(image.base, 'https://api.openai.com/v1');
  assert.equal(image.model, 'gpt-image-1');
});

test('resolveConfigs: empty / garbage → null (no fabrication)', () => {
  for (const j of [null, {}, { keys: {} }, 'nonsense', 42]) assert.equal(resolveConfigs(j).image, null);
});

test('listProviders: online available only when image key + connection + not Private', () => {
  const img = { provider: 'openai', base: 'https://api.x.ai/v1', model: 'grok-2-image', key: 'xai' };
  const on = listProviders(img, { online: true, webgpu: true, modelCached: false, runtimeOk: true });
  assert.equal(on.find((p) => p.id === 'online').available, true);
  const off = listProviders(img, { online: false });
  assert.equal(off.find((p) => p.id === 'online').available, false, 'no connection → online unavailable');
  const noKey = listProviders(null, { online: true });
  assert.equal(noKey.find((p) => p.id === 'online').available, false, 'no key → online unavailable');
  const priv = listProviders(img, { online: true, private: true, lang: 'en' });
  const o = priv.find((p) => p.id === 'online');
  assert.equal(o.available, false, 'Private mode: the prompt must not leave this computer');
  assert.match(o.reason, /Private mode/);
});

test('listProviders: local available only with a model present + runtime ok', () => {
  const base = { online: false };
  assert.equal(listProviders(null, { ...base, modelCached: true, runtimeOk: true }).find((p) => p.id === 'local').available, true);
  assert.equal(listProviders(null, { ...base, modelOnSD: true, runtimeOk: true }).find((p) => p.id === 'local').available, true);
  assert.equal(listProviders(null, { ...base, modelCached: false, runtimeOk: true }).find((p) => p.id === 'local').available, false);
  assert.equal(listProviders(null, { ...base, modelCached: true, runtimeOk: false }).find((p) => p.id === 'local').available, false);
});

test('listProviders: preview is always available and honestly non-AI', () => {
  const list = listProviders(null, { online: false, runtimeOk: false, lang: 'it' });
  const prev = list.find((p) => p.id === 'preview');
  assert.equal(prev.available, true);
  assert.match(prev.reason, /non IA/);
  assert.match(listProviders(null, { lang: 'en' }).find((p) => p.id === 'preview').reason, /not AI/);
});

test('every reason reads in the OS language — es/fr/de used to show Italian', () => {
  const img = { provider: 'openai', base: 'https://api.x.ai/v1', key: 'xai' };
  const caps = [{ online: true, runtimeOk: true, modelCached: true, webgpu: true }, { online: false, runtimeOk: false }, { online: true, private: true, runtimeOk: true }];
  for (const lang of ['es', 'fr', 'de']) for (const c of caps) for (const key of [img, null]) {
    for (const p of listProviders(key, { ...c, lang })) {
      for (const it of ['nessun', 'nessuna', 'modalità', 'non disponibile', 'scaricato', 'nel browser', 'procedurale', 'Anteprima'])
        assert.ok(!p.reason.includes(it) && !p.brand.includes(it), `${lang}: Italian "${it}" in ${p.id}: ${p.reason}`);
    }
  }
  assert.equal(providerText('preview', 'xx'), 'Preview', 'an unknown language reads English');
});

test('defaultProvider: cascade online → local → preview, honouring an available preference', () => {
  const img = { key: 'xai', base: 'https://api.x.ai/v1', model: 'grok-2-image' };
  const all = listProviders(img, { online: true, modelCached: true, runtimeOk: true, webgpu: true });
  assert.equal(defaultProvider(all), 'online', 'best = online when everything is up');
  const noNet = listProviders(img, { online: false, modelCached: true, runtimeOk: true });
  assert.equal(defaultProvider(noNet), 'local', 'falls to local in-browser model when offline');
  const priv = listProviders(img, { online: true, private: true, modelCached: true, runtimeOk: true });
  assert.equal(defaultProvider(priv, 'online'), 'local', 'Private: a saved "online" preference is ignored');
  const bare = listProviders(null, { online: false, modelCached: false, runtimeOk: false });
  assert.equal(defaultProvider(bare), 'preview', 'falls to preview only as last resort');
  assert.equal(defaultProvider(all, 'local'), 'local');
  assert.equal(defaultProvider(noNet, 'online'), 'local', 'unavailable preference ignored');
});

test('enhancerLabel: names who would rewrite the prompt, in five languages; none without an engine', () => {
  assert.equal(enhancerLabel('Ollama', 'en'), 'enhance with Ollama');
  assert.equal(enhancerLabel('Claude', 'it'), 'migliora con Claude');
  assert.equal(enhancerLabel('Claude', 'de'), 'verbessern mit Claude');
  assert.equal(enhancerLabel(null), null);
});

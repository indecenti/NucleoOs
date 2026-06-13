// gemini-provider.test.mjs — host gate for the Google Gemini provider wiring.
// Proves: (1) the provider registry shape (OpenAI-compat base, AIza prefix, proxy flag); (2) viaProxy()
// relays ONLY Gemini through the device same-origin /api/llm (others stay browser-direct); (3) the
// teacher.json doc for Gemini (provider:'google', no anthropic-version, key in the keys map); (4)
// readTeacher() infers 'google' from a Gemini base URL; (5) contextkit routes google → the bounded
// 'gemini' profile (within the firmware proxy's 32 KB body cap), no Llama-style "firm" framing.
// Pure + deterministic; the one fetch in readTeacher() is mocked. No device, no network.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { PROVIDERS, providerOf, viaProxy, buildTeacherDoc, readTeacher,
  geminiListModels, geminiProbeTier, calibrateGemini, geminiTierLabel } from '../../web/shell/ai.js';
import { resolveKind, profileFor, MODEL_PROFILES, estimateTokens } from '../../apps/anima/www/contextkit.js';

// Mock the device /api/llm proxy: a GET (models list) and a POST (Pro-model tier probe). Inspects the
// decoded ?url= + method so one stub serves both calibration calls. Lets us pin the logic with no real key.
function mockGemini({ modelsBody = null, modelsOk = true, proStatus = 200, proThrow = false } = {}) {
  return async (url, opts = {}) => {
    const isPost = String((opts && opts.method) || 'GET').toUpperCase() === 'POST';
    if (!isPost) return { ok: modelsOk, status: modelsOk ? 200 : 404, json: async () => modelsBody, text: async () => JSON.stringify(modelsBody) };
    if (proThrow) throw new Error('network down');
    return { ok: proStatus >= 200 && proStatus < 300, status: proStatus, json: async () => ({}), text: async () => '' };
  };
}
const withFetch = async (stub, fn) => { const s = globalThis.fetch; globalThis.fetch = stub; try { return await fn(); } finally { globalThis.fetch = s; } };
const CFG = { provider: 'google', base: PROVIDERS.google.base, key: 'AIzaTEST' };

test('registry: Gemini is OpenAI-compatible, AIza-keyed, proxy-routed', () => {
  const g = PROVIDERS.google;
  assert.ok(g, 'google provider present');
  assert.equal(g.proxy, true, 'must be proxy-routed (no browser CORS)');
  assert.match(g.base, /generativelanguage\.googleapis\.com\/v1beta\/openai$/);
  assert.ok(g.prefix.test('AIzaSyD-xxxxx'), 'AIza key matches');
  assert.ok(!g.prefix.test('gsk_xxx') && !g.prefix.test('sk-ant-xxx'), 'does not swallow other brands');
  assert.equal(g.version, '', 'OpenAI-compat → no anthropic-version');
  assert.equal(g.def, 'gemini-2.5-flash', 'default = a REAL free-tier Flash (gemini-3.5-flash 404s on the API)');
  assert.ok(g.models.some((m) => m[0] === 'gemini-2.5-pro'), 'paid Pro tier offered');
  assert.ok(g.models.some((m) => m[0] === 'gemini-flash-latest'), 'always-current Flash offered');
  assert.ok(!g.models.some((m) => /gemini-3\.5-flash|gemini-3\.1-pro|2\.0-flash/.test(m[0])), 'non-existent (404) or shut-down model IDs are NOT offered');
});

test('viaProxy: ONLY Gemini is relayed through /api/llm; others go direct', () => {
  const url = 'https://generativelanguage.googleapis.com/v1beta/openai/chat/completions';
  assert.equal(viaProxy('google', url), '/api/llm?url=' + encodeURIComponent(url), 'Gemini relayed');
  const groq = 'https://api.groq.com/openai/v1/chat/completions';
  assert.equal(viaProxy('openai', groq), groq, 'Groq stays browser-direct');
  assert.equal(viaProxy('anthropic', 'https://api.anthropic.com/v1/messages'), 'https://api.anthropic.com/v1/messages', 'Claude direct');
  assert.equal(viaProxy('xai', 'https://api.x.ai/v1/chat/completions'), 'https://api.x.ai/v1/chat/completions', 'xAI direct');
  assert.equal(viaProxy('nonsense', groq), groq, 'unknown provider → no accidental proxy');
});

test('buildTeacherDoc(google): provider:google, key in keys map, NO anthropic-version', () => {
  const doc = buildTeacherDoc({ provider: 'google', key: 'AIzaSyTEST', exec: 'browser' });
  assert.equal(doc.provider, 'google');
  assert.equal(doc.exec, 'browser');
  assert.match(doc.base, /generativelanguage/);
  assert.equal(doc.model, 'gemini-2.5-flash');
  assert.equal(doc.version, undefined, 'Gemini must NOT carry an anthropic-version');
  assert.equal(doc.keys.google.key, 'AIzaSyTEST', 'key remembered in the multi-provider map');
});

test('buildTeacherDoc keeps a Groq key alongside Gemini (voice/Whisper survives)', () => {
  // The audio path needs a Groq/OpenAI key even when Gemini is the active CHAT provider.
  const doc = buildTeacherDoc({ provider: 'google', key: 'AIzaX', keys: { openai: { key: 'gsk_keep', base: 'https://api.groq.com/openai/v1' } } });
  assert.equal(doc.provider, 'google');
  assert.equal(doc.keys.openai.key, 'gsk_keep', 'Groq key preserved for Whisper');
  assert.equal(doc.keys.google.key, 'AIzaX');
});

test('readTeacher: infers provider=google from a Gemini base URL (legacy file w/o provider field)', async () => {
  const saved = globalThis.fetch;
  globalThis.fetch = async () => ({ ok: true, status: 200,
    text: async () => JSON.stringify({ base: 'https://generativelanguage.googleapis.com/v1beta/openai', key: 'AIzaLegacy', model: 'gemini-2.5-flash' }) });
  try {
    const cfg = await readTeacher();
    assert.equal(cfg.provider, 'google', 'Gemini base → google');
    assert.equal(cfg.key, 'AIzaLegacy');
  } finally { globalThis.fetch = saved; }
});

test('contextkit: google → bounded "gemini" profile (not the cramped groq one, not firm)', () => {
  assert.equal(resolveKind('only', 'google'), 'gemini');
  assert.equal(resolveKind('on', 'google'), 'gemini');
  assert.equal(resolveKind('only', 'openai'), 'groq', 'Groq path unchanged');
  assert.equal(resolveKind('only', 'anthropic'), 'cloud', 'Claude path unchanged');
  const p = profileFor('gemini', 'gemini-2.5-flash');
  assert.ok(p && p.inTokens >= 4000, 'richer than the small Groq window');
  assert.ok(!p.firm, 'Gemini is strong — no Llama-style strict framing');
});

test('contextkit: gemini gets a cloud-class window (proxy streams the body — 32 KB cap removed)', () => {
  // The firmware /api/llm proxy now STREAMS the request body (no malloc of the whole thing), so the context
  // is no longer heap-bounded. Gemini should get a full cloud-class window like Claude, while staying under
  // the proxy's 1 MB sanity bound (a latency guard on pathological uploads, NOT a RAM limit).
  const p = MODEL_PROFILES.gemini;
  assert.ok(p.inTokens >= 12000, 'streamed proxy → no 32 KB cap → cloud-class context, not the old cramped 5k');
  assert.equal(p.firm, undefined, 'Gemini is strong — no Llama-style strict framing');
  const worstChars = (p.inTokens + 2200) * 3.6 + 900;   // (transcript + system) × ~3.6 chars/token + JSON overhead
  assert.ok(worstChars < (1 << 20), 'still well within the proxy 1 MB sanity bound');
  // sanity: estimateTokens is the same yardstick contextkit uses to enforce the budget
  assert.ok(estimateTokens('x'.repeat(360)) >= 90);
});

// ── plan/tier calibration ──────────────────────────────────────────────────────────────────────────
test('geminiListModels: parses OpenAI-compat + native shapes, filters non-chat, null on failure', async () => {
  await withFetch(mockGemini({ modelsBody: { data: [{ id: 'models/gemini-3.5-flash' }, { id: 'models/gemini-3.1-pro' }, { id: 'models/gemini-embedding-001' }] } }), async () => {
    const ids = await geminiListModels(CFG);
    assert.deepEqual(ids, ['gemini-3.5-flash', 'gemini-3.1-pro'], 'strips models/ prefix, drops embedding');
  });
  await withFetch(mockGemini({ modelsBody: { models: [{ name: 'models/gemini-3.5-flash' }] } }), async () => {
    assert.deepEqual(await geminiListModels(CFG), ['gemini-3.5-flash'], 'native {models:[{name}]} shape');
  });
  await withFetch(mockGemini({ modelsOk: false }), async () => assert.equal(await geminiListModels(CFG), null, '!ok → null (caller uses static)'));
  await withFetch(mockGemini({ modelsBody: 'garbage' }), async () => assert.equal(await geminiListModels(CFG), null, 'bad json → null'));
});

test('geminiProbeTier: 200⟹paid, 4xx⟹free, 5xx/throw⟹unknown', async () => {
  await withFetch(mockGemini({ proStatus: 200 }), async () => assert.equal(await geminiProbeTier(CFG), 'paid'));
  await withFetch(mockGemini({ proStatus: 429 }), async () => assert.equal(await geminiProbeTier(CFG), 'free', 'quota → free'));
  await withFetch(mockGemini({ proStatus: 403 }), async () => assert.equal(await geminiProbeTier(CFG), 'free', 'permission → free'));
  await withFetch(mockGemini({ proStatus: 404 }), async () => assert.equal(await geminiProbeTier(CFG), 'free', 'pro not found → free'));
  await withFetch(mockGemini({ proStatus: 503 }), async () => assert.equal(await geminiProbeTier(CFG), 'unknown', 'proxy busy → unknown'));
  await withFetch(mockGemini({ proThrow: true }), async () => assert.equal(await geminiProbeTier(CFG), 'unknown', 'offline → unknown'));
});

test('calibrateGemini: paid (Pro callable) → recommends Pro', async () => {
  await withFetch(mockGemini({ modelsBody: { data: [{ id: 'gemini-2.5-flash' }, { id: 'gemini-2.5-pro' }] }, proStatus: 200 }), async () => {
    const c = await calibrateGemini(CFG);
    assert.equal(c.tier, 'paid'); assert.equal(c.hasPro, true);
    assert.equal(c.recommended, 'gemini-2.5-pro', 'paid → best Pro recommended');
    assert.ok(c.models.includes('gemini-2.5-flash'));
  });
});

test('calibrateGemini: free (only Flash listed) → no probe needed, recommends Flash', async () => {
  await withFetch(mockGemini({ modelsBody: { data: [{ id: 'gemini-2.5-flash' }, { id: 'gemini-2.5-flash-lite' }] }, proStatus: 200 /* would say paid, but must NOT be reached */ }), async () => {
    const c = await calibrateGemini(CFG);
    assert.equal(c.tier, 'free', 'list has no Pro → free without a wasted probe');
    assert.equal(c.recommended, 'gemini-2.5-flash', 'free → strong Flash, never the weak -lite');
    assert.equal(c.hasPro, false);
  });
});

test('calibrateGemini: free (Pro listed but call refused) → recommends Flash', async () => {
  await withFetch(mockGemini({ modelsBody: { data: [{ id: 'gemini-2.5-flash' }, { id: 'gemini-2.5-pro' }] }, proStatus: 429 }), async () => {
    const c = await calibrateGemini(CFG);
    assert.equal(c.tier, 'free', 'Pro listed but 429 → free');
    assert.equal(c.recommended, 'gemini-2.5-flash', 'free → free-safe Flash, never the unusable Pro');
  });
});

test('calibrateGemini: no model list (probe-only) still classifies + falls back to static models', async () => {
  await withFetch(mockGemini({ modelsOk: false, proStatus: 200 }), async () => {
    const c = await calibrateGemini(CFG);
    assert.equal(c.tier, 'paid', 'no list → probe a real Pro, 200 → paid');
    assert.ok(c.models.includes('gemini-2.5-flash'), 'falls back to the static registry list');
    assert.equal(c.recommended, 'gemini-2.5-pro');
  });
});

test('calibrateGemini: fully offline → tier unknown, static models, Flash default, never throws', async () => {
  await withFetch(mockGemini({ modelsOk: false, proThrow: true }), async () => {
    const c = await calibrateGemini(CFG);
    assert.equal(c.tier, 'unknown');
    assert.equal(c.recommended, 'gemini-2.5-flash', 'safe default when nothing is known');
    assert.ok(Array.isArray(c.models) && c.models.length);
  });
});

test('geminiTierLabel: honest + bilingual', () => {
  assert.match(geminiTierLabel('paid', false), /pagamento/);
  assert.match(geminiTierLabel('paid', true), /Paid/);
  assert.match(geminiTierLabel('free', false), /Flash/);
  assert.match(geminiTierLabel('unknown', false), /non rilevato/);
});

// Automatic model selection + actionable errors (web/shell/ai.js). A provider retiring a model must never
// break a feature again (Groq shut down groq/compound on 2026-09-21 and ANIMA's web mode died with a bare 404),
// and every failure must tell the user what to do. Network is mocked; the /models fixtures are the real shapes.
import { test } from 'node:test';
import assert from 'node:assert/strict';

const AI = await import('../web/shell/ai.js');

// Groq's /models today (shape: {data:[{id, active}]}) — incl. non-chat models that must never be picked.
const GROQ_IDS = ['llama-3.1-8b-instant', 'llama-3.3-70b-versatile', 'openai/gpt-oss-120b', 'openai/gpt-oss-20b',
  'whisper-large-v3', 'whisper-large-v3-turbo', 'canopylabs/orpheus-v1-english', 'meta-llama/llama-prompt-guard-2-86m',
  'openai/gpt-oss-safeguard-20b', 'qwen/qwen3.8-27b', 'minimaxai/minimax-m2.7'];
const groq = (over = {}) => ({ provider: 'openai', base: 'https://api.groq.com/openai/v1', key: 'gsk_test_' + Math.random(), model: '', ...over });

// fetch mock: /models answers the list; chat calls go to `chat(model, body)` -> {status, json}
function mockFetch(ids, chat, seen = []) {
  globalThis.fetch = async (url, opts = {}) => {
    const u = String(url);
    if (u.endsWith('/models') || u.includes('/v1/models')) {
      seen.push('models');
      return new Response(JSON.stringify({ data: ids.map((id) => ({ id, active: true })) }), { status: 200 });
    }
    const body = JSON.parse(opts.body || '{}');
    seen.push(body.model);
    const r = chat(body.model, body);
    return new Response(JSON.stringify(r.json), { status: r.status, headers: r.headers || {} });
  };
  return seen;
}
const ok = (text) => ({ status: 200, json: { choices: [{ message: { content: text } }] } });

test('rankModels: a strong chat model for max, a quick one for fast, never speech/guard/agentic ids', () => {
  assert.equal(AI.rankModels(GROQ_IDS, 'max')[0], 'openai/gpt-oss-120b');
  assert.equal(AI.rankModels(GROQ_IDS, 'fast')[0], 'llama-3.1-8b-instant');
  const all = AI.rankModels(GROQ_IDS, 'mid');
  for (const bad of ['whisper-large-v3', 'canopylabs/orpheus-v1-english', 'meta-llama/llama-prompt-guard-2-86m', 'openai/gpt-oss-safeguard-20b'])
    assert.ok(!all.includes(bad), bad + ' is not a chat model');
  assert.ok(!AI.rankModels(['groq/compound', 'groq/compound-mini', 'llama-3.3-70b-versatile']).includes('groq/compound'));
  // newer beats older within a family
  assert.equal(AI.rankModels(['claude-sonnet-4-5', 'claude-sonnet-4-6', 'claude-haiku-4-5'], 'mid')[0], 'claude-sonnet-4-6');
  assert.equal(AI.rankModels(['claude-sonnet-4-6', 'claude-opus-4-8', 'claude-haiku-4-5'], 'max')[0], 'claude-opus-4-8');
});

test('resolveModel: the saved model while served, the best served one when it was retired', async () => {
  mockFetch(GROQ_IDS, () => ok('x'));
  assert.equal(await AI.resolveModel(groq({ model: 'llama-3.3-70b-versatile' })), 'llama-3.3-70b-versatile', 'preference honoured');
  const auto = await AI.resolveModel(groq({ model: 'llama3-8b-8192' }), { tier: 'max' });   // long gone
  assert.equal(auto, 'openai/gpt-oss-120b');
  assert.equal(await AI.resolveModel(groq({ model: 'auto' }), { tier: 'fast' }), 'llama-3.1-8b-instant');
  assert.equal(await AI.resolveModel(groq(), { need: 'web' }), 'openai/gpt-oss-120b', 'web search -> the gpt-oss family');
  assert.equal(await AI.resolveModel(groq(), { need: 'web', exclude: ['openai/gpt-oss-120b'] }), 'openai/gpt-oss-20b');
});

test('resolveModel without a model list: the saved/static guess, as before', async () => {
  globalThis.fetch = async () => { throw new TypeError('Failed to fetch'); };
  assert.equal(await AI.resolveModel(groq({ model: 'llama-3.3-70b-versatile' })), 'llama-3.3-70b-versatile');
  assert.equal(await AI.resolveModel(groq({ model: '' }), { tier: 'max' }), AI.TIERS.openai.max);
  assert.equal(await AI.resolveModel(groq(), { need: 'web' }), null, 'no list: no guessing a web-search model');
});

test('cloudComplete: a retired model is replaced and the call succeeds (404 and 400 "decommissioned")', async () => {
  const seen = mockFetch(GROQ_IDS.filter((id) => id !== 'llama-3.3-70b-versatile'), (model) =>
    model === 'llama-3.3-70b-versatile'
      ? { status: 404, json: { error: { message: 'The model `llama-3.3-70b-versatile` does not exist or you do not have access to it.', type: 'invalid_request_error', code: 'model_not_found' } } }
      : ok('ciao da ' + model));
  // the saved model is still in the stale cached list the first time: seed the cache with it, then retire it
  const cfg = groq({ model: 'llama-3.3-70b-versatile', key: 'gsk_retire' });
  mockFetch(GROQ_IDS, () => ok('x'));
  await AI.listModels(cfg, { fresh: true });
  mockFetch(GROQ_IDS.filter((id) => id !== 'llama-3.3-70b-versatile'), (model) =>
    model === 'llama-3.3-70b-versatile'
      ? { status: 404, json: { error: { message: 'model not found', code: 'model_not_found' } } }
      : ok('ciao da ' + model), seen);
  const out = await AI.cloudComplete(cfg, null, 'ciao', 32);
  assert.match(out, /^ciao da /, 'answered by a replacement model');
  assert.ok(!/llama-3\.3-70b/.test(out));

  const cfg2 = groq({ model: 'llama3-70b-8192', key: 'gsk_decom' });
  mockFetch(['llama3-70b-8192', ...GROQ_IDS], (model) => model === 'llama3-70b-8192'
    ? { status: 400, json: { error: { message: 'The model `llama3-70b-8192` has been decommissioned and is no longer supported.', type: 'invalid_request_error', code: 'model_decommissioned' } } }
    : ok('ok ' + model));
  assert.match(await AI.cloudComplete(cfg2, null, 'hi', 16), /^ok /);
});

test('cloudComplete: a bad key is NOT retried and explains itself', async () => {
  const seen = mockFetch(GROQ_IDS, () => ({ status: 401, json: { error: { message: 'Invalid API Key', type: 'invalid_request_error', code: 'invalid_api_key' } } }));
  await assert.rejects(AI.cloudComplete(groq({ model: 'llama-3.3-70b-versatile', key: 'gsk_bad' }), null, 'hi', 16), (e) => {
    assert.equal(e.kind, 'auth');
    const it = AI.explainAiError(e, 'it');
    assert.match(it, /chiave Groq non è valida/); assert.match(it, /Impostazioni ▸ AI/);
    assert.match(AI.explainAiError(e, 'en'), /Groq key is invalid/);
    return true;
  });
  assert.equal(seen.filter((s) => s !== 'models').length, 1, 'one chat call, no retry storm');
});

test('aiErrorKind / toAiError: every failure gets a kind the user can act on', () => {
  assert.equal(AI.aiErrorKind(429, 'Rate limit reached for model'), 'rate');
  assert.equal(AI.aiErrorKind(429, 'You exceeded your current quota, please check your plan and billing details'), 'quota');
  assert.equal(AI.aiErrorKind(400, 'prompt is too long: 250000 tokens > 200000 maximum'), 'too_long');
  assert.equal(AI.aiErrorKind(529, 'overloaded_error · Overloaded'), 'provider_down');
  assert.equal(AI.aiErrorKind(403, 'permission_error'), 'forbidden');
  assert.equal(AI.aiErrorKind(404, ''), 'model');
  assert.equal(AI.toAiError(new TypeError('Failed to fetch'), { provider: 'openai' }).kind, 'network');
  const ab = new Error('x'); ab.name = 'AbortError';
  assert.equal(AI.toAiError(ab).kind, 'stopped');
  assert.equal(AI.explainAiError(AI.toAiError(ab)), '', 'a user Stop is not an error to explain');
  const rate = new AI.AiError('rate', 'slow down', { provider: 'openai', retryAfter: 7 });
  assert.match(AI.explainAiError(rate, 'it'), /\(7 s\)/);
  assert.ok(AI.actionableAiError(new AI.AiError('quota', 'x')) && !AI.actionableAiError(rate));
});

test('routeFor carries the tier, so a retired TIERS id is re-picked for the same tier', () => {
  const r = AI.routeFor({ difficulty: 'hard' }, { openai: { key: 'gsk_x' } }, null);
  assert.equal(r.tier, 'max');
  assert.equal(AI.routeFor({ difficulty: 'fast' }, { openai: { key: 'gsk_x' } }, null).tier, 'fast');
});

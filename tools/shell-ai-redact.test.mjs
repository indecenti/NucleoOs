// A provider error can echo the request's credentials ("Invalid API key: gsk_…", "Bearer sk-…"). The shared
// explainer (web/shell/ai.js explainAiError) puts up to 160 chars of that text into EVERY app's UI and logs.
// Keys must never reach the screen: only a masked form may.
import test from 'node:test';
import assert from 'node:assert/strict';

const AI = await import('../web/shell/ai.js');
const KEY = 'gsk_SECRETabcdefghijklmnop1234567890';
const resp = (status, body) => ({ status, headers: { get: () => null }, text: async () => body });

test('an error body echoing the configured key never shows the key', async () => {
  const e = await AI.aiErrorFromResponse(resp(401, JSON.stringify({ error: { message: 'Invalid API key ' + KEY } })), { provider: 'openai', key: KEY });
  const shown = AI.explainAiError(e, 'en');
  assert.ok(!shown.includes(KEY) && !String(e.message).includes(KEY), shown);
});

test('key-shaped tokens and bearer headers are masked even when the key is not known', async () => {
  for (const secret of ['sk-ant-api03-ABCDEFGHIJKLMNOPQRSTUV', 'xai-ABCDEFGHIJKLMNOPQRSTUV123', 'AIzaSyABCDEFGHIJKLMNOPQRSTUVW', 'gsk_ABCDEFGHIJKLMNOPQRSTUV']) {
    const e = AI.toAiError(Object.assign(new Error('rejected header Authorization: Bearer ' + secret), { status: 401 }), { provider: 'openai' });
    const shown = AI.explainAiError(e, 'it');
    assert.ok(!shown.includes(secret), shown);
  }
});

test('ordinary error text is kept (the user still learns what went wrong)', async () => {
  const e = await AI.aiErrorFromResponse(resp(400, JSON.stringify({ error: { message: 'messages.0.content: field required' } })), { provider: 'openai', key: KEY });
  assert.match(AI.explainAiError(e, 'en'), /messages.0.content: field required/);
});

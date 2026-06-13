// paint-online-image.test.mjs — host gate for the ONLINE image engine (apps/paint/www/diffusion/online-image.js)
// covering xAI Grok specifically. Proves: the exact OpenAI-images wire shape, b64/url/revised-prompt parsing,
// negative-prompt folding, honest error mapping, and that generate() decodes inline bytes (no cross-origin
// fetch). fetch + decode are injected → no network, no canvas. Runs over the real prompt corpus.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  composeImagePrompt, buildImageRequest, mimeFromB64, parseImageResponse, explainImageError, makeOnlineImageEngine,
  buildChatRequest, parseChatResponse,
} from '../../apps/paint/www/diffusion/online-image.js';
import { PROMPTS, NEGATIVES } from './paint-prompt-corpus.mjs';

const GROK = { provider: 'openai', base: 'https://api.x.ai/v1', model: 'grok-2-image', key: 'xai-test' };

test('composeImagePrompt folds the negative (the image API has no negative field)', () => {
  assert.equal(composeImagePrompt('a cat', ''), 'a cat');
  assert.equal(composeImagePrompt('a cat', 'blurry, text'), 'a cat. Avoid: blurry, text');
  assert.equal(composeImagePrompt('  spaced  ', '  '), 'spaced');
  assert.equal(composeImagePrompt('', 'blurry'), 'Avoid: blurry', 'empty positive ⇒ no stray leading ". "');
  assert.equal(composeImagePrompt('', ''), '');
});

test('buildImageRequest: exact xAI /images/generations shape (b64_json, n clamp, no double slash)', () => {
  const r = buildImageRequest({ ...GROK, base: 'https://api.x.ai/v1/' }, { prompt: 'a red cat', n: 3 });
  assert.equal(r.url, 'https://api.x.ai/v1/images/generations');
  assert.equal(r.headers.authorization, 'Bearer xai-test');
  assert.equal(r.headers['content-type'], 'application/json');
  assert.equal(r.body.model, 'grok-2-image');
  assert.equal(r.body.prompt, 'a red cat');
  assert.equal(r.body.n, 3);
  assert.equal(r.body.response_format, 'b64_json');
  // n is clamped to 1..10, never a junk value the API would reject
  assert.equal(buildImageRequest(GROK, { prompt: 'x', n: 0 }).body.n, 1);
  assert.equal(buildImageRequest(GROK, { prompt: 'x', n: 99 }).body.n, 10);
  // aspect_ratio is included only when provided
  assert.equal(buildImageRequest(GROK, { prompt: 'x' }).body.aspect_ratio, undefined);
  assert.equal(buildImageRequest(GROK, { prompt: 'x', aspectRatio: '16:9' }).body.aspect_ratio, '16:9');
});

test('mimeFromB64: sniff jpeg/png/webp/gif from the leading base64', () => {
  assert.equal(mimeFromB64('/9j/4AAQ'), 'image/jpeg');
  assert.equal(mimeFromB64('iVBORw0KGgo'), 'image/png');
  assert.equal(mimeFromB64('UklGRiQ'), 'image/webp');
  assert.equal(mimeFromB64('R0lGODlh'), 'image/gif');
  assert.equal(mimeFromB64(''), 'image/jpeg');
});

test('parseImageResponse: b64_json, url, revised_prompt, error + empty shapes', () => {
  const ok = parseImageResponse({ data: [{ b64_json: '/9j/AAA', revised_prompt: 'a vivid red cat on a sofa' }] });
  assert.equal(ok.ok, true);
  assert.equal(ok.images[0].b64, '/9j/AAA');
  assert.equal(ok.images[0].mime, 'image/jpeg');
  assert.equal(ok.images[0].revisedPrompt, 'a vivid red cat on a sofa');

  const urlForm = parseImageResponse({ data: [{ url: 'https://img/abc.jpg' }] });
  assert.equal(urlForm.images[0].url, 'https://img/abc.jpg');

  assert.equal(parseImageResponse({ error: { message: 'bad key' } }).ok, false);
  assert.equal(parseImageResponse({ error: 'flat error' }).ok, false);
  assert.equal(parseImageResponse({ data: [] }).ok, false);                       // empty array branch
  const noData = parseImageResponse({ data: [{ revised_prompt: 'x' }] });          // entries present but no b64/url
  assert.equal(noData.ok, false); assert.match(noData.error, /senza dati/);
  assert.equal(parseImageResponse(null).ok, false);
});

test('explainImageError: honest, actionable Italian messages incl. CORS/network', () => {
  assert.match(explainImageError(401), /chiave/);
  assert.match(explainImageError(429), /limite/);
  assert.match(explainImageError(0), /CORS|connessione/);
  assert.match(explainImageError(500), /provider/);
  assert.equal(explainImageError(418, 'custom'), 'custom');
});

test('makeOnlineImageEngine.generate: posts the right body and decodes inline bytes (no cross-origin fetch)', async () => {
  const calls = [];
  const decodedFrom = [];
  const fetchImpl = async (url, opts) => {
    calls.push({ url, body: JSON.parse(opts.body), headers: opts.headers });
    return { ok: true, status: 200, json: async () => ({ data: [{ b64_json: '/9j/REAL', revised_prompt: 'pro prompt' }] }) };
  };
  const decode = async (src) => { decodedFrom.push(src); return { width: 512, height: 512, data: new Uint8ClampedArray(512 * 512 * 4) }; };
  const eng = makeOnlineImageEngine({ cfg: GROK, fetchImpl, decode, label: 'Grok' });
  assert.equal((await eng.load()).ok, true);

  const out = await eng.generate({ prompt: 'un gatto rosso', negativePrompt: NEGATIVES[0], seed: 7 });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, 'https://api.x.ai/v1/images/generations');
  assert.equal(calls[0].body.prompt, 'un gatto rosso. Avoid: ' + NEGATIVES[0]);
  assert.equal(calls[0].headers.authorization, 'Bearer xai-test');
  // decode received a same-origin data: URL (inline bytes) — never a remote https image (which would taint)
  assert.match(decodedFrom[0], /^data:image\/jpeg;base64,\/9j\/REAL$/);
  assert.equal(out.image.width, 512);
  assert.equal(out.meta.online, true);
  assert.equal(out.meta.revisedPrompt, 'pro prompt');
  assert.equal(out.meta.seed, 7);
});

test('makeOnlineImageEngine.generate: runs the whole real-prompt corpus, one request each', async () => {
  let posted = 0;
  const fetchImpl = async () => { posted++; return { ok: true, status: 200, json: async () => ({ data: [{ b64_json: 'iVBORw0KGgoREAL' }] }) }; };
  const decode = async (src) => { assert.match(src, /^data:image\/png;base64,/); return { width: 256, height: 256, data: new Uint8ClampedArray(256 * 256 * 4) }; };
  const eng = makeOnlineImageEngine({ cfg: GROK, fetchImpl, decode });
  await eng.load();
  for (const p of PROMPTS) {
    const out = await eng.generate({ prompt: p.text });
    assert.equal(out.image.width, 256);
    assert.equal(out.meta.online, true);
  }
  assert.equal(posted, PROMPTS.length);
});

test('makeOnlineImageEngine.generate: HTTP + network errors throw with a mapped status', async () => {
  const decode = async () => ({ width: 1, height: 1, data: new Uint8ClampedArray(4) });
  // 401 invalid key
  let eng = makeOnlineImageEngine({ cfg: GROK, fetchImpl: async () => ({ ok: false, status: 401, json: async () => ({ error: { message: 'no' } }) }), decode });
  await eng.load();
  await assert.rejects(() => eng.generate({ prompt: 'x' }), (e) => e.status === 401 && /chiave/.test(e.message));
  // network / CORS rejection → status 0
  eng = makeOnlineImageEngine({ cfg: GROK, fetchImpl: async () => { throw new Error('Failed to fetch'); }, decode });
  await eng.load();
  await assert.rejects(() => eng.generate({ prompt: 'x' }), (e) => e.status === 0 && /(CORS|connessione)/.test(e.message));
});

test('makeOnlineImageEngine: constructor guards missing deps', () => {
  assert.throws(() => makeOnlineImageEngine({ cfg: { key: '' }, fetchImpl: () => {}, decode: () => {} }), /key/);
  assert.throws(() => makeOnlineImageEngine({ cfg: GROK, decode: () => {} }), /fetchImpl/);
  assert.throws(() => makeOnlineImageEngine({ cfg: GROK, fetchImpl: () => {} }), /decode/);
});

test('makeOnlineImageEngine.generate: ENFORCES inline b64 — a url-only response is declined (no taint, no hang)', async () => {
  let decoded = false;
  const fetchImpl = async () => ({ ok: true, status: 200, json: async () => ({ data: [{ url: 'https://cdn/x.jpg' }] }) });
  const decode = async () => { decoded = true; return { width: 1, height: 1, data: new Uint8ClampedArray(4) }; };
  const eng = makeOnlineImageEngine({ cfg: GROK, fetchImpl, decode });
  await eng.load();
  await assert.rejects(() => eng.generate({ prompt: 'x' }), (e) => e.status === 0 && /URL|b64|inline/.test(e.message));
  assert.equal(decoded, false, 'a remote URL is NEVER handed to the canvas decoder');
});

test('makeOnlineImageEngine.generate: not-loaded guard throws before any request', async () => {
  let posted = false;
  const eng = makeOnlineImageEngine({ cfg: GROK, fetchImpl: async () => { posted = true; return { ok: true, status: 200, json: async () => ({ data: [{ b64_json: '/9j/x' }] }) }; }, decode: async () => ({ width: 1, height: 1, data: new Uint8ClampedArray(4) }) });
  await assert.rejects(() => eng.generate({ prompt: 'x' }), /not loaded/);
  assert.equal(posted, false);
});

test('makeOnlineImageEngine.generate: aspectRatio is forwarded end-to-end into the request body', async () => {
  let body = null;
  const eng = makeOnlineImageEngine({ cfg: GROK, fetchImpl: async (u, o) => { body = JSON.parse(o.body); return { ok: true, status: 200, json: async () => ({ data: [{ b64_json: 'iVBORw0KGgo' }] }) }; }, decode: async () => ({ width: 1, height: 1, data: new Uint8ClampedArray(4) }) });
  await eng.load();
  await eng.generate({ prompt: 'a cat', aspectRatio: '16:9' });
  assert.equal(body.aspect_ratio, '16:9');
});

test('buildChatRequest: Anthropic vs OpenAI shapes (endpoint, auth, system placement, base convention)', () => {
  const a = buildChatRequest({ provider: 'anthropic', base: 'https://api.anthropic.com', model: 'claude-sonnet-4-6', key: 'sk-ant', version: '2023-06-01' }, 'SYS', 'USR', 200);
  assert.equal(a.url, 'https://api.anthropic.com/v1/messages');
  assert.equal(a.headers['x-api-key'], 'sk-ant');
  assert.equal(a.headers['anthropic-version'], '2023-06-01');
  assert.equal(a.headers['anthropic-dangerous-direct-browser-access'], 'true');
  assert.equal(a.body.system, 'SYS', 'system is top-level for Anthropic');
  assert.equal(a.body.messages[0].content, 'USR');
  assert.equal(a.body.max_tokens, 200);

  const o = buildChatRequest({ provider: 'openai', base: 'https://api.x.ai/v1', model: 'grok-3-mini', key: 'xai' }, 'SYS', 'USR');
  assert.equal(o.url, 'https://api.x.ai/v1/chat/completions');
  assert.equal(o.headers.authorization, 'Bearer xai');
  assert.equal(o.body.messages[0].role, 'system', 'system is a message for OpenAI-compatible');
  assert.equal(o.body.messages[1].content, 'USR');
});

test('parseChatResponse: Anthropic content[] join vs OpenAI choices[].message.content', () => {
  assert.equal(parseChatResponse('anthropic', { content: [{ type: 'text', text: 'a vivid ' }, { type: 'text', text: 'red cat' }, { type: 'thinking', text: 'IGNORE' }] }), 'a vivid red cat');
  assert.equal(parseChatResponse('openai', { choices: [{ message: { content: 'a neon dragon' } }] }), 'a neon dragon');
  assert.equal(parseChatResponse('anthropic', null), '');
  assert.equal(parseChatResponse('openai', { choices: [] }), '');
});

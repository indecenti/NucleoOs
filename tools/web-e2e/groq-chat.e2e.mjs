// Browser E2E — AI Chat (groq-chat). The provider is STUBBED in the page (window.fetch for api.groq.com) and
// every other external host resolves to nothing, so no real API is ever called. Covers: the key typed in ⚙ is
// stored and sent only as the Bearer header; the request shape (POST /chat/completions, stream, the running
// conversation, a model the key actually serves); a streamed reply rendered as safe Markdown; Clear starts a
// fresh context; provider errors in plain words with the failed turn dropped; the key never reaches the page
// text, title, console or any URL — even when a provider echoes it back in an error.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1, MAP * ~NOTFOUND, EXCLUDE localhost, EXCLUDE 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const KEY = 'gsk_e2eSECRETkey_0123456789abcdef';
const BASE = 'https://api.groq.com/openai/v1';

// The Groq stand-in. window.__plans: per chat call, 'ok' (streamed reply) | 'auth' (401) | 'echo' (a 400 whose
// message repeats the Authorization header, as some OpenAI-compatible gateways do).
function STUB() {
  window.__reqs = []; window.__plans = [];
  const real = window.fetch.bind(window);
  window.fetch = async (input, opts = {}) => {
    const url = typeof input === 'string' ? input : input.url;
    if (!/^https:\/\/[^/]*groq\.com\//.test(url)) return real(input, opts);
    const h = new Headers(opts.headers || {});
    window.__reqs.push({ url, method: opts.method || 'GET', auth: h.get('authorization'), body: opts.body ? JSON.parse(opts.body) : null });
    const json = (status, o) => new Response(JSON.stringify(o), { status, headers: { 'content-type': 'application/json' } });
    if (url.endsWith('/models')) return json(200, { data: [{ id: 'llama-3.3-70b-versatile' }, { id: 'openai/gpt-oss-120b' }, { id: 'whisper-large-v3' }] });
    const plan = window.__plans.shift() || 'ok';
    if (plan === 'auth') return json(401, { error: { message: 'Invalid API Key', type: 'invalid_request_error', code: 'invalid_api_key' } });
    if (plan === 'echo') return json(400, { error: { message: 'Malformed header: Authorization: ' + h.get('authorization') } });
    const n = window.__reqs.filter((r) => r.method === 'POST').length;
    const parts = ['Reply ' + n + ': ', '**bold** ', '<img src=x onerror="window.__pwn=1">'];
    const enc = new TextEncoder();
    const body = new ReadableStream({ start(c) {
      for (const p of parts) c.enqueue(enc.encode('data: ' + JSON.stringify({ choices: [{ delta: { content: p } }] }) + '\n\n'));
      c.enqueue(enc.encode('data: [DONE]\n\n')); c.close();
    } });
    return new Response(body, { status: 200, headers: { 'content-type': 'text/event-stream' } });
  };
}

async function open(browser, sim) {
  const page = await browser.newPage();
  await page.initScript(`(${STUB.toString()})();`);
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/groq-chat/`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#model option').length > 1 && document.getElementById('sub').textContent !== '—'`, { timeout: 45000 }), 'AI Chat up');
  return page;
}
const setKey = (k) => `(() => { const i = document.getElementById('key'); i.value = ${JSON.stringify(k)}; i.dispatchEvent(new Event('change')); return true; })()`;
const send = (q) => `(() => { document.getElementById('q').value = ${JSON.stringify(q)}; document.getElementById('f').requestSubmit(); return true; })()`;
const CHATS = `window.__reqs.filter((r) => r.method === 'POST')`;
const idle = (page) => page.waitFor(`!document.getElementById('send').disabled && document.getElementById('stop').hidden`, { timeout: 15000 });

test('groq-chat: request shape, a safely rendered streamed reply, the conversation carried and Clear resetting it', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  await page.eval(send('hello'));
  assert.equal(await page.eval(`${CHATS}.length`), 0, 'no key: nothing is sent');
  assert.ok(await page.eval(`document.getElementById('settings').classList.contains('open')`), 'the ⚙ panel opens to ask for the key');

  await page.eval(setKey(KEY));
  assert.equal(await page.eval(`localStorage.getItem('groq.key')`), KEY, 'the key is kept in this browser');
  await page.eval(send('hello'));
  assert.ok(await page.waitFor(`document.querySelector('.turn.bot .meta')`, { timeout: 15000 }), 'the reply completed');
  const [r1] = await page.eval(CHATS);
  assert.equal(r1.url, BASE + '/chat/completions');
  assert.equal(r1.auth, 'Bearer ' + KEY, 'the key travels only as the Bearer header');
  assert.equal(r1.body.stream, true);
  assert.deepEqual(r1.body.messages, [{ role: 'user', content: 'hello' }]);
  assert.ok(['llama-3.3-70b-versatile', 'openai/gpt-oss-120b'].includes(r1.body.model), 'a chat model the key serves (never whisper): ' + r1.body.model);

  const bot = `document.querySelector('.turn.bot .body')`;
  assert.ok((await page.eval(`${bot}.textContent`)).startsWith('Reply 1: bold <img src=x onerror="window.__pwn=1">'), 'the reply is on screen, markup as text');
  assert.equal(await page.eval(`${bot}.querySelectorAll('strong').length`), 1, 'Markdown bold rendered');
  assert.equal(await page.eval(`${bot}.querySelectorAll('img').length`), 0, 'model output never becomes an element');
  assert.equal(await page.eval(`window.__pwn`), undefined);

  await idle(page);
  await page.eval(send('and then?'));
  assert.ok(await page.waitFor(`document.querySelectorAll('.turn.bot .meta').length === 2`, { timeout: 15000 }));
  const r2 = (await page.eval(CHATS))[1];
  assert.deepEqual(r2.body.messages.map((m) => m.role), ['user', 'assistant', 'user'], 'the conversation goes with the next turn');
  assert.equal(r2.body.messages[1].content, 'Reply 1: **bold** <img src=x onerror="window.__pwn=1">', 'the assistant turn is the raw reply');

  await idle(page);
  await page.eval(`document.getElementById('clear').click(), true`);
  await page.eval(send('fresh start'));
  assert.ok(await page.waitFor(`document.querySelectorAll('.turn.bot .meta').length === 1`, { timeout: 15000 }));
  assert.deepEqual((await page.eval(CHATS))[2].body.messages, [{ role: 'user', content: 'fresh start' }], 'Clear drops the old context');
});

test('groq-chat: provider errors in plain words, the failed turn dropped, and the key never shown', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  await page.eval(setKey(KEY));

  await page.eval(`window.__plans.push('auth'), true`);
  await page.eval(send('first'));
  const err = await page.waitFor(`(() => { const e = document.querySelector('.turn.err .body'); return e && e.textContent; })()`, { timeout: 15000 });
  assert.match(err, /Groq key is invalid/, 'a 401 is explained: ' + err);
  assert.equal(await page.eval(`document.querySelectorAll('.turn.bot').length`), 0, 'no empty bot bubble left behind');
  await idle(page);

  await page.eval(`window.__plans.push('echo'), true`);
  await page.eval(send('second'));
  assert.ok(await page.waitFor(`document.querySelectorAll('.turn.err').length === 2`, { timeout: 15000 }), 'the 400 is reported');
  await idle(page);

  await page.eval(send('third'));
  assert.ok(await page.waitFor(`document.querySelector('.turn.bot .meta')`, { timeout: 15000 }));
  assert.deepEqual((await page.eval(CHATS)).at(-1).body.messages, [{ role: 'user', content: 'third' }], 'failed turns are not replayed to the model');

  // The key: only ever in the password field's value and the Authorization header.
  assert.equal(await page.eval(`document.documentElement.outerHTML.includes(${JSON.stringify(KEY)})`), false, 'the key is in the page markup');
  assert.equal(await page.eval(`document.body.innerText.includes(${JSON.stringify(KEY)}) || document.title.includes(${JSON.stringify(KEY)})`), false, 'the key is visible on screen (an error echoed it)');
  assert.equal(await page.eval(`window.__reqs.some((r) => r.url.includes(${JSON.stringify(KEY)}))`), false, 'the key is in a URL');
  const logged = [...page.log, ...page.consoleInfo].map((e) => String(e.text || '') + ' ' + String(e.url || ''));
  assert.deepEqual(logged.filter((s) => s.includes(KEY)), [], 'the key reached the console or a request URL');

  await page.eval(`document.getElementById('forget').click(), true`);
  assert.equal(await page.eval(`localStorage.getItem('groq.key')`), null, '"Forget the key" removes it');
  assert.equal(await page.eval(`document.getElementById('key').value`), '');
});

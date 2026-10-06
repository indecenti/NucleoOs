// The shell service worker (web/shell/sw.js), run for real inside a node:vm with a fake CacheStorage and a
// scripted network. It only ever runs on a secure context (localhost / https), so the browser E2E suite —
// which loads the shell from a plain-http origin on purpose, like the device — never exercises it.
//
//   node --test tools/shell-sw.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import vm from 'node:vm';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const SRC = readFileSync(join(REPO, 'web', 'shell', 'sw.js'), 'utf8');
const ORIGIN = 'http://localhost:5599';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Minimal Cache / CacheStorage keyed by URL (what the SW uses: GET requests, no Vary).
class FakeCache {
  constructor(failPut) { this.m = new Map(); this.failPut = failPut; }
  key(r) { return typeof r === 'string' ? new URL(r, ORIGIN).href : r.url; }
  async match(r) { const v = this.m.get(this.key(r)); return v ? v.clone() : undefined; }
  async put(r, res) { if (this.failPut()) throw new Error('QuotaExceededError'); this.m.set(this.key(r), res.clone()); }
  async delete(r) { return this.m.delete(this.key(r)); }
  async keys() { return [...this.m.keys()].map((u) => new Request(u)); }
  async add() {}
}

// Load sw.js into a fresh sandbox. `net(req, init)` plays the device/the internet. `timeScale` shrinks the
// SW's own long timers (15 s write budget) so a "slow" request can be simulated in milliseconds.
function loadSw({ net, timeScale = 1, failPut = () => false } = {}) {
  const listeners = {};
  const stores = new Map();
  const open = async (n) => { if (!stores.has(n)) stores.set(n, new FakeCache(failPut)); return stores.get(n); };
  const caches = {
    open,
    async match(r) { for (const c of stores.values()) { const h = await c.match(r); if (h) return h; } return undefined; },
    async delete(n) { return stores.delete(n); },
    async keys() { return [...stores.keys()]; },
  };
  const calls = [];
  const fetchImpl = async (req, init) => {
    const r = req instanceof Request ? req : new Request(req);
    calls.push(r.url);
    const signal = (init && init.signal) || null;
    if (signal && signal.aborted) throw new DOMException('aborted', 'AbortError');
    const p = net(r, init);
    if (!signal) return p;
    return Promise.race([p, new Promise((_, rej) => signal.addEventListener('abort', () => rej(new DOMException('aborted', 'AbortError')), { once: true }))]);
  };
  const scaled = (ms) => (ms >= 1000 ? Math.round(ms * timeScale) : ms);
  const AS = { timeout: (ms) => AbortSignal.timeout(scaled(ms)), abort: (r) => AbortSignal.abort(r), any: (s) => AbortSignal.any(s) };
  const self = {
    location: new URL(ORIGIN + '/sw.js'),
    addEventListener: (t, fn) => { listeners[t] = fn; },
    skipWaiting: async () => {},
    clients: { claim: async () => {} },
  };
  const ctx = vm.createContext({
    self, caches, fetch: fetchImpl, Request, Response, Headers, URL, URLSearchParams, AbortSignal: AS, DOMException,
    setTimeout: (fn, ms, ...a) => setTimeout(fn, scaled(ms || 0), ...a), clearTimeout, console, Promise, Map, Set,
  });
  vm.runInContext(SRC, ctx, { filename: 'sw.js' });
  // Dispatch a FetchEvent. Resolves to the Response the SW answered with, or null if it let the request
  // go to the network itself (no respondWith).
  const dispatch = async (input, init) => {
    const request = new Request(new URL(input, ORIGIN).href, init);
    let answered = null; const waits = [];
    listeners.fetch({ request, respondWith(p) { answered = Promise.resolve(p); }, waitUntil(p) { waits.push(p); } });
    if (!answered) return { res: null, waits };
    return { res: await answered, waits };
  };
  return { dispatch, calls, stores, open };
}

const readUrl = (p) => '/api/fs/read?path=' + encodeURIComponent(p);

test('cross-origin requests are left alone: no device permit, no fake 504 (bug 7)', async () => {
  let seen = 0;
  const sw = loadSw({ net: async () => { seen++; return new Response('{}'); } });
  for (const u of ['https://api.anthropic.com/v1/messages', 'https://api.groq.com/openai/v1/chat/completions',
    'https://api.github.com/repos/x/y/releases/latest', 'https://example.com/lib.js']) {
    const { res } = await sw.dispatch(u, u.includes('messages') ? { method: 'POST', body: '{}' } : undefined);
    assert.equal(res, null, 'the SW must not answer ' + u);
  }
  assert.equal(seen, 0, 'the SW itself fetched a cross-origin URL');
});

test('Forge model weights on the HF CDN are still served from the install cache', async () => {
  const sw = loadSw({ net: async () => new Response('net') });
  const c = await sw.open('anima-forge-models');
  await c.put('/fc/m1/w.bin', new Response('cached-weights'));
  const { res } = await sw.dispatch('https://huggingface.co/org/m1/resolve/main/w.bin');
  assert.equal(await res.text(), 'cached-weights');
});

test('a failing cache.put never turns a good image read into a 504 (bug 8)', async () => {
  const sw = loadSw({ net: async () => new Response('PIXELS', { status: 200 }), failPut: () => true });
  const { res } = await sw.dispatch(readUrl('/data/Pictures/a.png'));
  assert.equal(res.status, 200);
  assert.equal(await res.text(), 'PIXELS');
});

test('writing / moving / deleting an image drops its cached copy (bug 6)', async () => {
  const files = new Map([['/data/Pictures/a.png', 'OLD'], ['/data/Pictures/b.png', 'B1']]);
  const net = async (r) => {
    const u = new URL(r.url);
    if (u.pathname === '/api/fs/read') {
      const p = u.searchParams.get('path');
      return files.has(p) ? new Response(files.get(p)) : new Response('{"error":"not found"}', { status: 404 });
    }
    if (u.pathname === '/api/fs/write') { files.set(u.searchParams.get('path'), await r.text()); return new Response('{"ok":true}'); }
    if (u.pathname === '/api/fs/delete') { files.delete(u.searchParams.get('path')); return new Response('{"ok":true}'); }
    if (u.pathname === '/api/fs/move') {
      const f = u.searchParams.get('from'), t = u.searchParams.get('to');
      files.set(t, files.get(f)); files.delete(f); return new Response('{"ok":true}');
    }
    return new Response('', { status: 404 });
  };
  const sw = loadSw({ net });
  const read = async (p) => { const { res, waits } = await sw.dispatch(readUrl(p)); await Promise.all(waits); return { status: res.status, body: await res.text() }; };

  assert.equal((await read('/data/Pictures/a.png')).body, 'OLD');          // cached now
  await sw.dispatch('/api/fs/write?path=' + encodeURIComponent('/data/Pictures/a.png'), { method: 'POST', body: 'NEW' });
  assert.equal((await read('/data/Pictures/a.png')).body, 'NEW', 'Paint saved, reopen showed the old pixels');

  assert.equal((await read('/data/Pictures/b.png')).body, 'B1');
  const { res: del } = await sw.dispatch('/api/fs/delete?path=' + encodeURIComponent('/data/Pictures/b.png'), { method: 'POST' });
  assert.equal(del.status, 200);
  assert.equal((await read('/data/Pictures/b.png')).status, 404, 'a deleted image was still served from the cache');

  assert.equal((await read('/data/Pictures/a.png')).body, 'NEW');
  await sw.dispatch('/api/fs/move?from=' + encodeURIComponent('/data/Pictures') + '&to=' + encodeURIComponent('/data/Old'), { method: 'POST' });
  files.set('/data/Pictures/a.png', 'REPLACED');   // another client puts a new file at the old path
  assert.equal((await read('/data/Pictures/a.png')).body, 'REPLACED', 'a moved folder left its images cached at the old path');
});

test('a long upload is never aborted by the write gate, and does not freeze other reads (bug 5)', async () => {
  // timeScale 0.002: the SW's 15 s write budget becomes 30 ms. The upload takes 250 ms.
  let readServedDuringUpload = false, uploading = false;
  const net = async (r) => {
    const u = new URL(r.url);
    if (u.pathname === '/api/fs/write') { uploading = true; await sleep(250); uploading = false; return new Response('{"ok":true}'); }
    if (uploading) readServedDuringUpload = true;
    return new Response('[]');
  };
  const sw = loadSw({ net, timeScale: 0.002 });
  const big = new Uint8Array(4 * 1024 * 1024);
  const up = sw.dispatch('/api/fs/write?path=' + encodeURIComponent('/data/uploads/big.nfv'),
    { method: 'POST', body: big, headers: { 'content-type': 'application/octet-stream' } });
  await sleep(80);
  const { res: list } = await sw.dispatch('/api/fs/list?path=%2Fdata');
  assert.equal(list.status, 200);
  const { res } = await up;
  assert.equal(res.status, 200, 'a multi-MB upload was aborted by the gate timeout (→ 504 → retried)');
  assert.ok(readServedDuringUpload, 'a long write held the whole device gate and froze every other read');
  // An untyped XHR body (File Commander sends the File as-is) gets the same treatment.
  const { res: r2 } = await sw.dispatch('/api/fs/write?path=' + encodeURIComponent('/data/x.bin'), { method: 'POST', body: big });
  assert.equal(r2.status, 200);
});

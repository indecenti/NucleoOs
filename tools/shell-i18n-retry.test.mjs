// web/shell/nucleo-i18n.js — a catalog the LINK failed to deliver is not an empty catalog. The Cardputer
// answers "503 low memory" under load and its httpd now and then resets a socket; caching that miss as {} left
// the whole session in English. Now: retried, never cached as empty, English meanwhile (never raw keys), and a
// background repair repaints in the right language once the catalog arrives. A 404 stays "no catalog".
// Run: node --test tools/shell-i18n-retry.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';

const CATALOGS = {
  '/i18n/core.en.json': { ok: 'OK' },
  '/i18n/core.fr.json': { ok: 'D’accord' },
  '/i18n/shell.en.json': { link_retry: 'Retry now' },
  '/i18n/shell.fr.json': { link_retry: 'Réessayer' },
};

// A fresh engine per case (module state is per instance) on a minimal browser surface. `script(url, n)`
// answers the n-th request for url: 'ok' | 'reset' (network error) | a status code.
async function engine(script) {
  const store = { 'anima.lang': 'fr' };
  globalThis.localStorage = { getItem: (k) => (k in store ? store[k] : null), setItem: (k, v) => { store[k] = String(v); } };
  globalThis.document = { nodeType: 9, documentElement: { setAttribute() {} }, querySelectorAll: () => [] };
  globalThis.window = { addEventListener() {}, parent: null, postMessage() {} };
  globalThis.location = { pathname: '/', origin: 'http://device' };
  const calls = {};
  globalThis.fetch = async (url) => {
    const n = (calls[url] = (calls[url] || 0) + 1);
    const how = script(url, n);
    if (how === 'reset') throw new TypeError('Failed to fetch');
    if (how !== 'ok') return { ok: false, status: how, json: async () => ({}) };
    if (!(url in CATALOGS)) return { ok: false, status: 404, json: async () => ({}) };
    return { ok: true, status: 200, json: async () => CATALOGS[url] };
  };
  const { default: I18N } = await import(`../web/shell/nucleo-i18n.js?case=${Math.random()}`);
  return { I18N, calls };
}

test('a 503 or a reset socket is retried: the shell still comes up in French', async () => {
  const { I18N, calls } = await engine((url, n) => (url === '/i18n/shell.fr.json' && n === 1 ? 503
    : url === '/i18n/core.fr.json' && n === 1 ? 'reset' : 'ok'));
  const t = await I18N.init('shell');
  assert.equal(t('link_retry'), 'Réessayer');
  assert.equal(t('ok'), 'D’accord');
  assert.equal(calls['/i18n/shell.fr.json'], 2);
});

test('a catalog that stays unreachable: English meanwhile, then the repair repaints in French', async () => {
  let down = true;
  const { I18N, calls } = await engine((url) => (url === '/i18n/shell.fr.json' && down ? 503 : 'ok'));
  const t = await I18N.init('shell');
  assert.equal(t('link_retry'), 'Retry now', 'the English floor, never a raw key');
  assert.equal(calls['/i18n/shell.fr.json'], 3, 'three tries, then handed out as failed');
  const repainted = new Promise((res) => I18N.onChange(res));
  down = false;
  assert.equal(await repainted, 'fr', 'subscribers hear the repair');
  assert.equal(t('link_retry'), 'Réessayer');
});

test('a 404 is a real "no catalog": fetched once, never retried', async () => {
  const { I18N, calls } = await engine(() => 'ok');
  const t = await I18N.init('notes');
  assert.equal(t('ok'), 'D’accord', 'the app namespace inherits core');
  assert.equal(calls['/apps/notes/i18n.fr.json'], 1);
  assert.equal(calls['/apps/notes/i18n.en.json'], 1);
});

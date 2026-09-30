// web/shell/capabilities.js — the ONE diagnosis of what the browser can do for local AI, and the exact
// fix per browser. Five surfaces used to explain a missing WebGPU five different ways (and some wrongly:
// "use a computer" while Android Chrome has WebGPU; nothing said the same flag also unlocks the Cache API
// the model installer needs). These tests pin the pure parts; the browser probe is covered by the E2E suite.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { detectBrowser, secureFix, diagnose, adapterClass, isIpHost } from '../web/shell/capabilities.js';

const UA = {
  chrome: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/154.0.0.0 Safari/537.36',
  edge: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/154.0.0.0 Safari/537.36 Edg/154.0.0.0',
  firefox: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:143.0) Gecko/20100101 Firefox/143.0',
  safari: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/19.0 Safari/605.1.15',
  androidChrome: 'Mozilla/5.0 (Linux; Android 15; Pixel 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/154.0.0.0 Mobile Safari/537.36',
};

test('detectBrowser names the browser, its major version and whether it is Chromium', () => {
  assert.deepEqual(detectBrowser(UA.chrome), { name: 'chrome', version: 154, mobile: false, chromium: true });
  assert.equal(detectBrowser(UA.edge).name, 'edge');
  assert.equal(detectBrowser(UA.firefox).name, 'firefox');
  assert.equal(detectBrowser(UA.firefox).version, 143);
  assert.equal(detectBrowser(UA.safari).name, 'safari');
  assert.equal(detectBrowser(UA.androidChrome).mobile, true, 'Android Chrome is a phone browser — and it HAS WebGPU');
  assert.equal(detectBrowser(UA.chrome, [{ brand: 'Brave' }]).name, 'brave');
});

test('secureFix points each Chromium browser at ITS OWN flags page, with the exact origin to add', () => {
  const o = 'http://192.168.0.166';
  assert.equal(secureFix(detectBrowser(UA.chrome), o).flagUrl, 'chrome://flags/#unsafely-treat-insecure-origin-as-secure');
  assert.equal(secureFix(detectBrowser(UA.edge), o).flagUrl, 'edge://flags/#unsafely-treat-insecure-origin-as-secure');
  assert.equal(secureFix(detectBrowser(UA.chrome), o).origin, o);
  const ff = secureFix(detectBrowser(UA.firefox), o);
  assert.equal(ff.kind, 'firefox-pref');
  assert.equal(ff.pref, 'dom.securecontext.allowlist');
  assert.equal(ff.host, '192.168.0.166', 'Firefox wants the bare host, not the origin');
  assert.equal(secureFix(detectBrowser(UA.safari), o).kind, 'use-chromium', 'Safari has no per-origin switch');
});

test('diagnose tells the real reason, in priority order', () => {
  assert.deepEqual(diagnose({ secure: false, webgpu: { api: false, available: false } }), { gpu: 'insecure', storage: 'insecure', ready: false });
  assert.equal(diagnose({ secure: true, cacheApi: true, webgpu: { api: false } }).gpu, 'browser');
  assert.equal(diagnose({ secure: true, cacheApi: true, webgpu: { api: true, available: false } }).gpu, 'blocked', 'API present, no adapter: acceleration off / blocklisted');
  const nof16 = diagnose({ secure: true, cacheApi: true, webgpu: { api: true, available: true, f16: false } });
  assert.equal(nof16.gpu, 'no-f16');
  assert.equal(nof16.ready, true, 'no shader-f16 is NOT a dead end: the q4f32 builds run');
  assert.deepEqual(diagnose({ secure: true, cacheApi: true, webgpu: { api: true, available: true, f16: true } }), { gpu: 'ok', storage: 'ok', ready: true });
  assert.equal(diagnose({ secure: true, cacheApi: true, webgpu: { available: true, f16: true }, storage: { quotaMB: 2000, usageMB: 1000 } }).storage, 'low');
});

test('adapterClass separates discrete GPUs, integrated ones and software fallbacks', () => {
  assert.equal(adapterClass({ vendor: 'nvidia', architecture: 'blackwell', description: 'NVIDIA GeForce RTX 5070 Laptop GPU' }), 'discrete');
  assert.equal(adapterClass({ vendor: 'intel', architecture: 'xe-lpg' }), 'integrated');
  assert.equal(adapterClass({ vendor: 'amd', description: 'AMD Radeon(TM) Graphics' }), 'integrated', 'an AMD APU is not a discrete card');
  assert.equal(adapterClass({ vendor: 'google', description: 'SwiftShader' }), 'fallback');
  assert.equal(adapterClass({ isFallback: true }), 'fallback');
  assert.equal(adapterClass(null), 'none');
});

test('isIpHost flags origins whose fix breaks when DHCP hands out a new address', () => {
  assert.equal(isIpHost('http://192.168.0.166'), true);
  assert.equal(isIpHost('http://nucleo-01.local'), false);
});

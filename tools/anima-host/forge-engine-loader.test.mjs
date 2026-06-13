// Gate: ANIMA Forge — user-choice engine selection + loader. The user always picks; the policy only
// marks availability. No-GPU clients must still have a real local option (WASM).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { engineOptions, defaultEngine, loadEngine } from '../../apps/anima/www/forge/engine-loader.js';

test('engineOptions reflects capability, and ALL engines are always listed (user can see every choice)', () => {
  const o = engineOptions({ webgpu: false, wasm: true, online: false, deviceBrain: true });
  assert.deepEqual(o.map((e) => e.id), ['webgpu', 'wasm', 'cloud', 'device', 'demo']);   // every option shown
  const by = Object.fromEntries(o.map((e) => [e.id, e.available]));
  assert.equal(by.webgpu, false);   // no GPU
  assert.equal(by.wasm, true);      // no-GPU local path IS available
  assert.equal(by.cloud, false);    // offline / no key
  assert.equal(by.device, true);    // device floor
  assert.equal(by.demo, true);      // always
});

test('NO-GPU client gets a real LOCAL engine (WASM) as the default, not the cloud', () => {
  assert.equal(defaultEngine({ webgpu: false, wasm: true, online: true, hasKey: true, deviceBrain: true }), 'wasm');
});

test('default prefers GPU when present, falls to device when nothing local/cloud', () => {
  assert.equal(defaultEngine({ webgpu: true, wasm: true, deviceBrain: true }), 'webgpu');
  assert.equal(defaultEngine({ webgpu: false, wasm: false, online: false, deviceBrain: true }), 'device');
  assert.equal(defaultEngine({ webgpu: false, wasm: false, online: false, deviceBrain: false }), 'demo');
});

test('loadEngine builds the chosen engine via injected factories; refuses unavailable ones honestly', async () => {
  const caps = { webgpu: true, wasm: true, online: true, hasKey: true, deviceBrain: true };
  const fakeWebLLM = async () => ({ chat: { completions: { create: async () => ({ choices: [{ message: { content: '```js\n1\n```' } }], usage: { tokens: 3 } }) } } });
  const fakeWasm = async () => ({ createChatCompletion: async () => '```js\n2\n```' });
  const deviceTransport = async (q, { mode }) => ({ reply: 'dev(' + mode + '):' + q });
  const mockEngine = { isMock: true, async chat() { return { text: 'mock' }; } };

  const gpu = await loadEngine('webgpu', { caps, createWebLLM: fakeWebLLM, mlcModelId: 'X' });
  assert.equal((await gpu.chat([], {})).text.includes('```js'), true);
  assert.equal(gpu.isMock, false);

  const wasm = await loadEngine('wasm', { caps, createWasm: fakeWasm, ggufModelId: 'Y' });
  assert.match((await wasm.chat([], {})).text, /```js/);

  const cloud = await loadEngine('cloud', { caps, deviceTransport });
  assert.match((await cloud.chat([{ role: 'user', content: 'hi' }])).text, /dev\(only\):hi/);

  const dev = await loadEngine('device', { caps, deviceTransport });
  assert.match((await dev.chat([{ role: 'user', content: 'q' }])).text, /dev\(off\):q/);

  const demo = await loadEngine('demo', { caps, mockEngine });
  assert.equal(demo.isMock, true);
});

test('loadEngine refuses an unavailable engine with an honest reason', async () => {
  await assert.rejects(() => loadEngine('webgpu', { caps: { webgpu: false, wasm: true, deviceBrain: true } }), /unavailable|WebGPU/i);
  await assert.rejects(() => loadEngine('cloud', { caps: { webgpu: true, online: false, deviceBrain: true } }), /unavailable|key/i);
});

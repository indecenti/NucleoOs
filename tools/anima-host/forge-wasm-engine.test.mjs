// Gate: ANIMA Forge — the no-GPU WASM engine adapter (wllama). Pure parts host-tested; the wllama
// import is dynamic + injectable (a real WASM run is browser-only, can't be host-verified).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { probeWasm, chooseWasmModel, coderMessages, testMessages, makeWasmEngine, WASM_CODER, WASM_CODER_BIG } from '../../apps/anima/www/forge/wasm-engine.js';

test('probeWasm detects WebAssembly (present in Node) and reports a RAM budget', () => {
  const p = probeWasm();
  assert.equal(p.supported, true);
  assert.ok(p.ramGB >= 1);
  assert.equal(probeWasm({}).supported, false);   // no WebAssembly in the fake global
});

test('chooseWasmModel picks the 0.5B on small RAM, 1.5B when there is room', () => {
  assert.equal(chooseWasmModel({ ramGB: 4 }).model, WASM_CODER);
  assert.equal(chooseWasmModel({ ramGB: 8 }).model, WASM_CODER_BIG);
  assert.equal(chooseWasmModel({ ramGB: 4 }).params, '0.5B');
});

test('coder/test prompt builders are well-formed and low-temperature', () => {
  const c = coderMessages('reverse a string');
  assert.equal(c.messages[0].role, 'system');
  assert.equal(c.messages[1].content, 'reverse a string');
  assert.ok(c.options.temperature <= 0.2);
  const t = testMessages('reverse a string', 'const f=s=>s');
  assert.match(t.messages[1].content, /CODE:/);
});

test('makeWasmEngine requires an injected createEngine', () => {
  assert.throws(() => makeWasmEngine({}), /createEngine/);
});

test('makeWasmEngine drives an injected (fake) wllama and is NOT a mock', async () => {
  let loaded = 0;
  const fake = async () => { loaded++; return { createChatCompletion: async () => '```js\nconsole.log(1)\n```', exit: async () => {} }; };
  const eng = makeWasmEngine({ createEngine: fake, modelId: WASM_CODER });
  assert.equal(eng.isMock, false);
  const r = await eng.chat([{ role: 'user', content: 'x' }], {});
  assert.match(r.text, /console\.log/);
  assert.ok(r.usage.tokens > 0);
  await eng.unload();
  assert.equal(loaded, 1);
});

test('makeWasmEngine also accepts an OpenAI-style choices response', async () => {
  const fake = async () => ({ createChatCompletion: async () => ({ choices: [{ message: { content: 'ok' } }], usage: { tokens: 5 } }) });
  const eng = makeWasmEngine({ createEngine: fake });
  const r = await eng.chat([], {});
  assert.equal(r.text, 'ok');
  assert.equal(r.usage.tokens, 5);
});

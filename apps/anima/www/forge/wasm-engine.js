// wasm-engine.js — the NO-GPU fallback engine: an Engine over wllama (llama.cpp compiled to
// WebAssembly), so code generation still works on clients without WebGPU — slower (CPU) and a
// smaller model, but local, offline and private. Structured so the PURE parts are host-tested and
// the browser-only wllama import is dynamic + injectable (a fake engine is passed in tests).
// Mirrors webllm-engine.js. NEVER import wllama at module load (keeps Node import clean).

export const WASM_CODER = 'Qwen2.5-Coder-0.5B-Instruct-Q4_K_M.gguf';  // small enough for CPU/WASM
export const WASM_CODER_BIG = 'Qwen2.5-Coder-1.5B-Instruct-Q4_K_M.gguf';

// WebAssembly is essentially universal; this just reports whether the CPU fallback is usable and a
// rough RAM budget. supported:false only in a locked-down env without WebAssembly.
export function probeWasm(glob = (typeof globalThis !== 'undefined' ? globalThis : {})) {
  const supported = typeof glob.WebAssembly !== 'undefined';
  // deviceMemory is GB (Chrome); default to a conservative 4 GB when unknown.
  const ramGB = (glob.navigator && glob.navigator.deviceMemory) || 4;
  return { supported, ramGB, reason: supported ? 'WebAssembly available' : 'no WebAssembly' };
}

// Choose the GGUF by available RAM: 1.5B needs ~2 GB working set on CPU; below that use 0.5B.
export function chooseWasmModel(caps = {}) {
  const ramGB = caps.ramGB || 4;
  return ramGB >= 6 ? { model: WASM_CODER_BIG, params: '1.5B' } : { model: WASM_CODER, params: '0.5B' };
}

// Prompt builders (pure) — the coder asks for ONE fenced ```js block, low temperature; mirrors the
// firmware grok_chat code_mode so the same ``` renderer is reused.
export function coderMessages(spec) {
  return {
    messages: [
      { role: 'system', content: 'You are a precise JavaScript coding assistant. Reply with ONE complete, idiomatic snippet inside a single ```js block. At most one short sentence before it; nothing after.' },
      { role: 'user', content: String(spec || '') },
    ],
    options: { temperature: 0.2, n_predict: 768 },   // smaller budget than WebGPU: CPU is slow
  };
}
export function testMessages(spec, code) {
  return {
    messages: [
      { role: 'system', content: 'Write runnable JavaScript assertions that throw on failure for the given code. Reply with ONE ```js block of assertions only.' },
      { role: 'user', content: `Spec: ${spec}\n\nCODE:\n${code}` },
    ],
    options: { temperature: 0.1, n_predict: 384 },
  };
}

// makeWasmEngine({ createEngine, modelId }) → Engine { chat(messages,opts)->{text,usage}, load, unload, isMock:false }.
// createEngine is INJECTED — the real one dynamically imports '@wllama/wllama'; tests pass a fake.
export function makeWasmEngine({ createEngine, modelId = WASM_CODER, onProgress } = {}) {
  if (typeof createEngine !== 'function') throw new Error('wasm-engine: createEngine must be injected');
  let inst = null;
  return {
    get isMock() { return false; },
    async load() { if (!inst) inst = await createEngine(modelId, { onProgress }); return true; },
    async unload() { if (inst && inst.exit) { try { await inst.exit(); } catch { /* best effort */ } } inst = null; },
    async chat(messages, opts = {}) {
      if (!inst) inst = await createEngine(modelId, { onProgress });
      const r = await inst.createChatCompletion(messages, opts);
      const text = typeof r === 'string' ? r : (r && (r.text || (r.choices && r.choices[0] && r.choices[0].message && r.choices[0].message.content))) || '';
      const usage = (r && r.usage) || { tokens: text.length };
      return { text, usage };
    },
  };
}

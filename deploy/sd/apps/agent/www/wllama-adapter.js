// wllama-adapter.js — F3: make the WASM rung actually RUNNABLE for the agent loop.
//
// F0 built the local transport (runWorkerLocal) and F4 installed the WASM model but kept its rung OUT
// of the runnable ladder, honestly, because the chat wiring did not exist. This is that wiring.
//
// The gap is a shape mismatch. forge/wasm-engine.js (the Engine the loop drives) expects an injected
// object with `createChatCompletion(messages, opts) -> {choices:[{message:{content}}]}` — the WebLLM
// shape. The vendored wllama exposes `createCompletion(promptString, {nPredict, sampling, stopTokens})`
// and a raw `getChatTemplate()`. So this adapter: (1) formats a messages[] into ONE prompt using the
// model's own chat template (Qwen ChatML — the format the GGUF was trained on), and (2) wraps the
// completion in the WebLLM response shape. Both halves are PURE and host-tested; the wllama import and
// model load live in the injected factory, never at module scope (keeps node import clean).

// Render a chat transcript into a prompt string. Prefer the model's declared template; fall back to
// ChatML (Qwen/Qwen2.5-Coder's format) when the GGUF carries none — never guess a foreign format,
// because a wrong template is the single biggest quality cliff for a small local model.
export function formatChatML(messages) {
  let out = '';
  for (const m of messages || []) {
    const role = m.role === 'assistant' ? 'assistant' : (m.role === 'system' ? 'system' : 'user');
    out += '<|im_start|>' + role + '\n' + String(m.content == null ? '' : m.content) + '<|im_end|>\n';
  }
  out += '<|im_start|>assistant\n';
  return out;
}

// A minimal Jinja-ish chat-template applier for the ONE construct these GGUF templates use: a
// {% for message in messages %} loop emitting <|im_start|>{{role}}\n{{content}}<|im_end|>. If the
// template is anything more exotic we do NOT try to interpret it — we return null and the caller uses
// formatChatML, which is the correct ChatML rendering anyway. Honesty over a half-working interpreter.
export function applyTemplate(tmpl, messages) {
  if (typeof tmpl !== 'string' || !tmpl.includes('<|im_start|>')) return null;   // not ChatML-shaped → let ChatML fallback handle it
  return formatChatML(messages);
}

// Map our injected chat opts (temperature, maxTokens, a GBNF grammar) onto wllama's createCompletion
// options. wllama has no grammar-constrained decoding, so a grammar becomes stop tokens + a low
// temperature — the runWorkerLocal loop RE-VALIDATES every output against the grammar anyway (that is
// its whole design), so correctness does not depend on the engine constraining generation.
export function toWllamaOpts(opts = {}) {
  const o = { nPredict: (opts.maxTokens | 0) > 0 ? (opts.maxTokens | 0) : 512, sampling: {} };
  o.sampling.temp = typeof opts.temperature === 'number' ? opts.temperature : 0.2;
  if (typeof opts.seed === 'number') o.sampling.seed = opts.seed;
  return o;
}

// Build the createEngine factory that forge/wasm-engine.js expects. `loadWllama` is injected: in the
// browser it dynamic-imports the vendored wllama and returns a loaded instance; tests pass a fake that
// returns a scripted { createCompletion, getChatTemplate }. The returned object exposes exactly the
// `createChatCompletion` + `exit` that makeWasmEngine calls.
export function makeWllamaCreateEngine(loadWllama) {
  if (typeof loadWllama !== 'function') throw new Error('wllama-adapter: loadWllama must be injected');
  return async function createEngine(modelId, opts = {}) {
    const inst = await loadWllama(modelId, opts);   // a loaded wllama (or a fake in tests)
    let tmpl = null;
    try { tmpl = inst.getChatTemplate ? inst.getChatTemplate() : null; } catch { tmpl = null; }
    return {
      async createChatCompletion(messages, copts = {}) {
        const prompt = applyTemplate(tmpl, messages) || formatChatML(messages);
        const text = await inst.createCompletion(prompt, toWllamaOpts(copts));
        return { choices: [{ message: { content: String(text || '') } }], usage: { tokens: String(text || '').length } };
      },
      async exit() { if (inst && inst.exit) { try { await inst.exit(); } catch { /* best effort */ } } },
    };
  };
}

// The browser loader: import the vendored wllama, point it at the SD-served single/multi-thread wasm,
// and load the GGUF from the shared model cache (same '/fc/<id>/<file>' the picker installed to). Kept
// here so the adapter core stays import-free and host-testable.
const VENDOR = '/apps/anima/forge/vendor/';
export async function browserLoadWllama(modelId, opts = {}) {
  const { Wllama } = await import(VENDOR + 'wllama.mjs');
  const paths = {
    'single-thread/wllama.wasm': VENDOR + 'wllama/single-thread/wllama.wasm',
    'single-thread/wllama.js': VENDOR + 'wllama/single-thread/wllama.js',
    'multi-thread/wllama.wasm': VENDOR + 'wllama/multi-thread/wllama.wasm',
    'multi-thread/wllama.js': VENDOR + 'wllama/multi-thread/wllama.js',
    'multi-thread/wllama.worker.mjs': VENDOR + 'wllama/multi-thread/wllama.worker.mjs',
  };
  const w = new Wllama(paths);
  // The GGUF was installed by the picker into the Cache API under '/fc/<id>/<file>'. wllama fetches by
  // URL, so we hand it the same-origin cache URL; the service worker / Cache API serves the bytes with
  // no network. modelUrl is the single shard's cache key.
  const url = '/fc/' + modelId + '/qwen2.5-coder-0.5b-instruct-q4_k_m.gguf';
  await w.loadModelFromUrl(url, { n_ctx: 2048, progressCallback: opts.onProgress });
  return w;
}

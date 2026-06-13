// webllm-engine.js — the real M4 inference adapter (WebLLM in the browser, on WebGPU). The PURE
// parts — GPU probe shape, VRAM-aware model choice, the orchestrator/coder chat-message + grammar
// option payloads — are host-testable; the only browser-only dependency (WebLLM's CreateMLCEngine)
// is DYNAMICALLY imported INSIDE makeWebLLMEngine and INJECTED via `createEngine` so a fake can be
// passed in tests. ANIMA's contract holds: the orchestrator decodes UNDER the closed-action GBNF
// grammar (grammar.toGBNF) and the coder is re-validated downstream — generative output is never
// trusted raw. Pure & DOM-free → host-testable (WebLLM is never imported at module load).

import { modelPlan } from './router.js';
import { toGBNF } from './grammar.js';

// WebLLM/MLC model ids (q4f16_1 builds — the only ones that fit a no-discrete iGPU).
export const CODER_MODEL = 'Qwen2.5-Coder-1.5B-Instruct-q4f16_1-MLC';
export const ORCH_DEFAULT = 'Llama-3.2-1B-Instruct-q4f16_1-MLC';      // default-safe orchestrator
export const ORCH_SMALL = 'Qwen2.5-0.5B-Instruct-q4f16_1-MLC';       // roomy plan → cheaper planner

const VRAM_HINT = { [CODER_MODEL]: 1200, [ORCH_DEFAULT]: 900, [ORCH_SMALL]: 450 };

// probeWebGPU(nav) → { supported, vramMB?, reason }. Cleanly returns {supported:false} when there is
// no navigator / no navigator.gpu (i.e. under Node). Uses adapter.limits.maxBufferSize as a VRAM
// proxy (the largest single allocation a WebGPU adapter will admit ≈ usable budget).
export async function probeWebGPU(nav = (typeof navigator !== 'undefined' ? navigator : null)) {
  if (!nav || !nav.gpu || typeof nav.gpu.requestAdapter !== 'function') {
    return { supported: false, reason: 'no-webgpu' };
  }
  let adapter;
  try { adapter = await nav.gpu.requestAdapter(); }
  catch (e) { return { supported: false, reason: 'adapter-error:' + (e && e.message || e) }; }
  if (!adapter) return { supported: false, reason: 'no-adapter' };
  const maxBuf = adapter.limits && adapter.limits.maxBufferSize;
  const vramMB = typeof maxBuf === 'number' && maxBuf > 0 ? Math.round(maxBuf / (1024 * 1024)) : undefined;
  return { supported: true, vramMB, reason: 'webgpu-ok' };
}

// chooseModels(caps) → { orchestrator:{id,vramMB}|null, coder:{id,vramMB}, plan }. Pure: defers the
// residency decision to router.modelPlan so `auto` never picks a plan it cannot fit.
//   - single-model plan  → orchestrator=null (deterministic pre-router + grammar-constrained coder)
//   - roomy plan (two-model) → cheaper ORCH_SMALL; otherwise the default-safe ORCH_DEFAULT.
export function chooseModels(caps = {}) {
  const plan = modelPlan(caps);
  const coder = { id: CODER_MODEL, vramMB: VRAM_HINT[CODER_MODEL] };
  if (!plan.orchestrator) return { orchestrator: null, coder, plan };
  const id = plan.mode === 'two-model' ? ORCH_SMALL : ORCH_DEFAULT;
  return { orchestrator: { id, vramMB: VRAM_HINT[id] }, coder, plan };
}

// orchestratorMessages(goal, toolGBNF) → { messages, options }. The orchestrator is a ROUTER, not a
// writer: it emits ONLY a JSON array of closed actions, constrained at decode time by the GBNF
// grammar (defaulting to the live grammar.toGBNF()), then re-validated downstream by parseActions.
export function orchestratorMessages(goal, toolGBNF = toGBNF()) {
  const sys =
    'You are ANIMA\'s action router. Output ONLY a JSON array of actions from the allowed grammar. ' +
    'No prose, no code, no explanation. Plan the minimal steps to satisfy the user goal.';
  const messages = [
    { role: 'system', content: sys },
    { role: 'user', content: String(goal || '') },
  ];
  const options = {
    grammar: toolGBNF,
    response_format: { type: 'grammar', grammar: toolGBNF },
    temperature: 0,
    phase: 'orchestrate',
  };
  return { messages, options };
}

// coderMessages(spec) → { messages, options }. The coder writes EXACTLY one fenced js block at a low
// temperature; its output is parsed for the code claim and run/verified downstream (never trusted).
export function coderMessages(spec) {
  const sys =
    'You are a precise JavaScript engineer. Return EXACTLY ONE fenced code block ```js ... ``` and ' +
    'nothing else — no prose before or after. The code must be self-contained and runnable.';
  const messages = [
    { role: 'system', content: sys },
    { role: 'user', content: String(spec || '') },
  ];
  const options = { temperature: 0.2, phase: 'code' };
  return { messages, options };
}

// makeWebLLMEngine({ createEngine, modelId, initProgress }) → an Engine
//   { async chat(messages, opts)->{text,usage}, async load(), async unload(), get isMock() // false }
// `createEngine` is INJECTED. The real caller passes a thin wrapper that does
//   const { CreateMLCEngine } = await import('@mlc-ai/web-llm');
//   return CreateMLCEngine(modelId, { initProgressCallback });
// — that dynamic import lives in the CALLER, never at this module's top level. Tests inject a fake
// createEngine that returns an object with a scripted `chat.completions.create`.
export function makeWebLLMEngine({ createEngine, modelId = CODER_MODEL, initProgress } = {}) {
  if (typeof createEngine !== 'function') throw new Error('makeWebLLMEngine requires an injected createEngine(modelId, opts)');
  let mlc = null;

  async function ensure() {
    if (mlc) return mlc;
    mlc = await createEngine(modelId, { initProgressCallback: initProgress });
    return mlc;
  }

  // Map our injectable opts onto a WebLLM completion request. opts.schema/grammar (when present)
  // flow into response_format so decoding is constrained.
  function toRequest(messages, opts = {}) {
    const req = { messages };
    if (typeof opts.temperature === 'number') req.temperature = opts.temperature;
    if (typeof opts.seed === 'number') req.seed = opts.seed;
    const grammar = opts.grammar || opts.schema;
    if (grammar) {
      req.response_format = (opts.response_format && opts.response_format.type)
        ? opts.response_format
        : { type: 'grammar', grammar };
    } else if (opts.response_format) {
      req.response_format = opts.response_format;
    }
    return req;
  }

  return {
    async load() { await ensure(); },
    async unload() {
      if (mlc && typeof mlc.unload === 'function') { try { await mlc.unload(); } catch { /* best-effort */ } }
      mlc = null;
    },
    async chat(messages, opts = {}) {
      const eng = await ensure();
      const res = await eng.chat.completions.create(toRequest(messages, opts));
      const choice = res && res.choices && res.choices[0];
      const text = (choice && choice.message && choice.message.content) || '';
      const usage = (res && res.usage) || { tokens: text.length };
      return { text, usage };
    },
    get isMock() { return false; },
  };
}

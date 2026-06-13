// sheet-engine.js — the LLM engine boundary for the spreadsheet copilot's browser-local substrate
// (M4: WebLLM on WebGPU, the weights served by the MCU or a CDN). Everything is INJECTED so the
// whole orchestration runs deterministically in CI behind a MockEngine (no GPU). The real engine is
// created by a thin caller that does `const {CreateMLCEngine} = await import('@mlc-ai/web-llm')` —
// that dynamic import lives in the CALLER, never at this module's top level, so importing this file
// in Node is safe and host-testable.
//
//   interface Engine { chat(messages, {schema?,grammar?,temperature?,seed?,phase?}) -> Promise<{text,usage}> }
//
// ANIMA's contract holds: the ORCHESTRATOR decodes UNDER the closed-action GBNF grammar (so a tiny
// 0.5B–1B model can only emit a valid PLAN), and the CODER's output is dry-run + recompute-verified
// downstream — generative output is never trusted raw. Pure & DOM-free (WebLLM never imported here).

import { toGBNF } from './sheet-grammar.js';

// MLC model ids (q4f16_1 builds — the only ones that fit a no-discrete iGPU). Same as forge.
export const CODER_MODEL = 'Qwen2.5-Coder-1.5B-Instruct-q4f16_1-MLC';
export const ORCH_DEFAULT = 'Llama-3.2-1B-Instruct-q4f16_1-MLC';
export const ORCH_SMALL = 'Qwen2.5-0.5B-Instruct-q4f16_1-MLC';
export const VRAM_HINT = { [CODER_MODEL]: 1200, [ORCH_DEFAULT]: 900, [ORCH_SMALL]: 450 };

// ---- MockEngine (host tests) — replays a scripted queue; never touches a GPU. ----
export class MockEngine {
  constructor(script = []) {
    this.script = typeof script === 'function' ? script : (Array.isArray(script) ? script.slice() : [script]);
    this.calls = []; this.i = 0;
  }
  async chat(messages, opts = {}) {
    this.calls.push({ messages, opts });
    let r;
    if (typeof this.script === 'function') r = this.script(messages, opts, this.i);
    else r = this.i < this.script.length ? this.script[this.i] : '';
    this.i++;
    const text = typeof r === 'string' ? r : ((r && r.text) || '');
    const usage = (r && typeof r === 'object' && r.usage) || { tokens: text.length };
    return { text, usage };
  }
  get isMock() { return true; }
}
export function assertMock(engine) {
  if (!engine || engine.isMock !== true) throw new Error('hard gate requires a MockEngine (no real model in CI)');
}

// ---- WebGPU probe ---- → { supported, vramMB?, reason }. Safe under Node (no navigator).
export async function probeWebGPU(nav = (typeof navigator !== 'undefined' ? navigator : null)) {
  if (!nav || !nav.gpu || typeof nav.gpu.requestAdapter !== 'function') return { supported: false, reason: 'no-webgpu' };
  let adapter;
  try { adapter = await nav.gpu.requestAdapter(); } catch (e) { return { supported: false, reason: 'adapter-error:' + (e && e.message || e) }; }
  if (!adapter) return { supported: false, reason: 'no-adapter' };
  const maxBuf = adapter.limits && adapter.limits.maxBufferSize;
  const vramMB = typeof maxBuf === 'number' && maxBuf > 0 ? Math.round(maxBuf / (1024 * 1024)) : undefined;
  return { supported: true, vramMB, reason: 'webgpu-ok' };
}

// ---- prompt builders (PURE — host-testable) ----

// The orchestrator is a ROUTER, not a writer: it emits ONLY a JSON array of closed sheet actions,
// constrained at decode time by the GBNF grammar, then re-validated by parseSheetActions. `ctx`
// carries the live sheet shape so the plan references real columns.
export function orchestratorMessages(goal, ctx = {}, toolGBNF = toGBNF()) {
  const sys =
    'You are the action planner inside ANIMA Spreadsheet, an OFFLINE Excel clone. Output ONLY a JSON ' +
    'array of actions from the allowed grammar — no prose, no markdown, no explanation. Plan the ' +
    'minimal ordered steps that satisfy the user goal. Prefer the typed ops (aggregate, total, ' +
    'highlight, sort, formula, enrich, transform, dedupe, rmempty, chart). Use "formula" with a ' +
    'natural-language description for anything computed; the app recomputes it. Use "transform_code" ' +
    'ONLY for custom row/group transforms no single op covers. End with {"op":"done"}.\n' +
    `Sheet: ${ctx.rows || 0} data rows, columns ${ctx.headers || 'A..'} . Current cell ${ctx.cell || 'A1'}.` +
    (ctx.md ? `\nSelection:\n${ctx.md}` : '');
  const messages = [{ role: 'system', content: sys }, { role: 'user', content: String(goal || '') }];
  const options = { grammar: toolGBNF, response_format: { type: 'grammar', grammar: toolGBNF }, temperature: 0, phase: 'orchestrate' };
  return { messages, options };
}

// The coder writes EXACTLY one fenced js block that EXPORTS a pure transform: given the selected
// block as a 2D array `rows` (and `headers`), it RETURNS a 2D array of the SAME shape (the new
// values). No I/O — it runs in the sandbox with fs/http/anima denied; its result is staged for
// approval, never auto-applied.
export function coderMessages(spec, ctx = {}) {
  const sys =
    'You are a precise JavaScript engineer for ANIMA Spreadsheet. Return EXACTLY ONE fenced code ' +
    'block ```js ... ``` and nothing else. The code is given two variables in scope: `rows` (a 2D ' +
    'array of the selected block, each inner array a row of cell VALUES) and `headers` (array of ' +
    'header strings, may be empty). It MUST end by RETURNING a 2D array of the same number of rows ' +
    'and columns with the transformed values. Pure compute only — NO fetch, NO imports, NO I/O. ' +
    'Numbers in, numbers out where sensible.';
  const user = `Transform: ${String(spec || '')}\n` +
    `Block is ${ctx.nrows || 0} rows x ${ctx.ncols || 0} cols. Headers: ${JSON.stringify(ctx.headers || [])}.`;
  const messages = [{ role: 'system', content: sys }, { role: 'user', content: user }];
  const options = { temperature: 0.2, phase: 'code' };
  return { messages, options };
}

// makeWebLLMEngine({ createEngine, modelId, initProgress }) → an Engine. `createEngine` is INJECTED.
// The real caller passes: async (id,opts)=>{ const {CreateMLCEngine}=await import('@mlc-ai/web-llm');
// return CreateMLCEngine(id,{initProgressCallback:opts.initProgressCallback}); }
export function makeWebLLMEngine({ createEngine, modelId = ORCH_DEFAULT, initProgress } = {}) {
  if (typeof createEngine !== 'function') throw new Error('makeWebLLMEngine requires an injected createEngine(modelId, opts)');
  let mlc = null;
  async function ensure() { if (!mlc) mlc = await createEngine(modelId, { initProgressCallback: initProgress }); return mlc; }
  function toRequest(messages, opts = {}) {
    const req = { messages };
    if (typeof opts.temperature === 'number') req.temperature = opts.temperature;
    if (typeof opts.seed === 'number') req.seed = opts.seed;
    const grammar = opts.grammar || opts.schema;
    if (grammar) req.response_format = (opts.response_format && opts.response_format.type) ? opts.response_format : { type: 'grammar', grammar };
    else if (opts.response_format) req.response_format = opts.response_format;
    return req;
  }
  return {
    async load() { await ensure(); },
    async unload() { if (mlc && typeof mlc.unload === 'function') { try { await mlc.unload(); } catch { /* */ } } mlc = null; },
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

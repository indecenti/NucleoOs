// engine-loader.js — USER-CHOICE engine selection + construction. The user ALWAYS picks; the policy
// only marks which options are available given the client's capabilities (and explains why, honestly).
// The real runtimes are built behind the choice via INJECTED factories (the UI supplies factories that
// dynamically import WebLLM / wllama from a CDN or the device SD; tests inject fakes). Pure selection
// logic → host-testable; no heavy import at module load.
//
// Engines:
//   webgpu : WebLLM (MLC) on the GPU            — fastest, on-device, private        (needs WebGPU + ~290 MB model)
//   wasm   : wllama (GGUF) on the CPU           — NO GPU needed, slower, on-device, private (~490 MB model)
//   cloud  : the Grok teacher via /api/anima    — capable, but network + key, NOT private
//   device : ANIMA recipes via /api/anima       — offline floor, grounded, grows as it learns (answer-only)
//   demo   : a scripted MockEngine              — no model, to try the flow anywhere

export const ENGINES = [
  { id: 'webgpu', label: 'Local · GPU (WebLLM)', kind: 'local', priv: true, agentic: true, available: (c) => !!c.webgpu, need: 'WebGPU + ~290 MB model', note: 'fastest · on-device · private' },
  { id: 'wasm', label: 'Local · CPU (WASM)', kind: 'local', priv: true, agentic: true, available: (c) => c.wasm !== false, need: '~490 MB model', note: 'no GPU needed · slower · on-device · private' },
  { id: 'cloud', label: 'Cloud (Grok)', kind: 'cloud', priv: false, agentic: true, available: (c) => !!(c.online && c.hasKey), need: 'network + API key', note: 'capable · NOT private' },
  { id: 'device', label: 'Device recipes', kind: 'device', priv: true, agentic: false, available: (c) => c.deviceBrain !== false, need: '—', note: 'offline · grounded · grows as it learns (answer-only)' },
  { id: 'demo', label: 'Demo (scripted)', kind: 'demo', priv: true, agentic: true, available: () => true, need: '—', note: 'no model · scripted · try the flow anywhere' },
];

export function engineOptions(caps = {}) {
  return ENGINES.map((e) => ({ id: e.id, label: e.label, kind: e.kind, priv: e.priv, agentic: e.agentic, need: e.need, note: e.note, available: !!e.available(caps) }));
}

// Best AVAILABLE engine, offline-/privacy-first (local → cloud → device → demo). The UI seeds the
// picker with this but the user can always override to any AVAILABLE option.
export function defaultEngine(caps = {}) {
  const opt = engineOptions(caps);
  for (const id of ['webgpu', 'wasm', 'cloud', 'device', 'demo']) {
    const e = opt.find((x) => x.id === id && x.available);
    if (e) return id;
  }
  return 'demo';
}

// Wrap the device /api/anima endpoint as an Engine (cloud/device "answer" engines). deviceTransport(q,
// {mode}) -> raw {reply,...}. These are NOT full agentic planners; they answer a single turn.
function deviceEngine(deviceTransport, mode) {
  return {
    get isMock() { return false; },
    async chat(messages) {
      const q = (messages && messages.length ? messages[messages.length - 1].content : '') || '';
      const r = await deviceTransport(q, { mode });
      return { text: (r && (r.reply || r.text)) || '', usage: { tokens: 0 } };
    },
  };
}

// Build the chosen engine. deps: { caps, createWebLLM, createWasm, deviceTransport, mockEngine }.
// createWebLLM(modelId,opts)->engine and createWasm(modelId,opts)->engine are injected by the UI
// (they dynamically import the runtime) or by tests (fakes). Throws a clear error if a needed
// factory/capability is missing, so the UI can surface an honest message.
export async function loadEngine(id, deps = {}) {
  const caps = deps.caps || {};
  const avail = engineOptions(caps).find((e) => e.id === id);
  if (!avail) throw new Error('unknown engine: ' + id);
  if (!avail.available) throw new Error(`engine "${id}" unavailable: needs ${avail.need}`);
  switch (id) {
    case 'webgpu': {
      if (typeof deps.createWebLLM !== 'function') throw new Error('webgpu engine: createWebLLM factory not provided');
      const { makeWebLLMEngine } = await import('./webllm-engine.js');
      return makeWebLLMEngine({ createEngine: deps.createWebLLM, modelId: deps.mlcModelId, initProgress: deps.onProgress });
    }
    case 'wasm': {
      if (typeof deps.createWasm !== 'function') throw new Error('wasm engine: createWasm factory not provided');
      const { makeWasmEngine } = await import('./wasm-engine.js');
      return makeWasmEngine({ createEngine: deps.createWasm, modelId: deps.ggufModelId, onProgress: deps.onProgress });
    }
    case 'cloud': return deviceEngine(deps.deviceTransport, 'only');
    case 'device': return deviceEngine(deps.deviceTransport, 'off');
    case 'demo': default: {
      if (!deps.mockEngine) throw new Error('demo engine: mockEngine not provided');
      return deps.mockEngine;
    }
  }
}

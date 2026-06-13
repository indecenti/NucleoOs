// local-models.js — catalog of in-browser chat models (WebLLM/MLC on WebGPU), ordered best → smallest with
// the VRAM each needs and a recommended default. WebLLM downloads the chosen model from the CDN on first use
// (AUTOMATIC + TRANSPARENT) and caches it for offline reuse — so "use the best" just works on a strong GPU.
// Every id below EXISTS in the vendored web-llm.js prebuiltAppConfig (verified), so no model_lib must be
// vendored: WebLLM already knows each model's library + weight URLs. Pure & DOM-free → host-testable.

export const LOCAL_MODELS = [
  { id: 'Qwen2.5-7B-Instruct-q4f16_1-MLC',   label: 'Qwen2.5 7B',   sizeGB: 4.7, needGB: 6,   best: true, note: 'massima qualità' },
  { id: 'Llama-3.1-8B-Instruct-q4f16_1-MLC', label: 'Llama 3.1 8B', sizeGB: 5.0, needGB: 7,               note: 'ottimo, un filo più pesante' },
  { id: 'Qwen2.5-3B-Instruct-q4f16_1-MLC',   label: 'Qwen2.5 3B',   sizeGB: 2.0, needGB: 3,               note: 'equilibrio' },
  { id: 'Qwen2.5-1.5B-Instruct-q4f16_1-MLC', label: 'Qwen2.5 1.5B', sizeGB: 1.1, needGB: 2,               note: 'leggero' },
  { id: 'Qwen2.5-0.5B-Instruct-q4f16_1-MLC', label: 'Qwen2.5 0.5B', sizeGB: 0.3, needGB: 1,               note: 'minimo, gira quasi ovunque' },
];

// "Il migliore in assoluto utilizzabile" on a strong discrete GPU (e.g. RTX 3070 Ti, 8 GB).
export const DEFAULT_LOCAL_MODEL = 'Qwen2.5-7B-Instruct-q4f16_1-MLC';

export function localModelById(id) { return LOCAL_MODELS.find((m) => m.id === id) || null; }

// The model to actually load: an explicit, valid user choice ALWAYS wins (the user knows their GPU); only
// when unset do we fall back to the recommended best. WebGPU never exposes TOTAL VRAM, so we never silently
// downgrade a stored choice on a buffer-size guess — the load path degrades gracefully instead (see compat).
export function resolveLocalModel(stored, _vramMB) {
  return (stored && localModelById(stored)) ? stored : DEFAULT_LOCAL_MODEL;
}

// Compatibility verdict for a model on this client. caps: { webgpu, vramMB }. Returns
//   { ok, level: 'ok'|'tight'|'no-webgpu'|'unknown', msg }.
// IMPORTANT: vramMB here is WebGPU's maxBufferSize (per-buffer cap), a POOR proxy for total VRAM, so it only
// ever raises a soft 'tight' hint — it NEVER blocks a model the user deliberately chose. Only the hard fact
// "no WebGPU at all" blocks (then the local-GPU tier genuinely cannot run; the chat uses device/cloud).
export function localModelCompat(id, caps = {}) {
  const m = localModelById(id);
  if (!m) return { ok: false, level: 'unknown', msg: 'Modello sconosciuto.' };
  if (!caps.webgpu) return { ok: false, level: 'no-webgpu', msg: 'Richiede WebGPU (Chrome/Edge con accelerazione hardware).' };
  // maxBufferSize on a real discrete GPU is often capped (~1–4 GB) far below total VRAM, so flag only a
  // CLEAR shortfall — a per-buffer budget below ~40% of the model's need — and even then as advisory.
  const proxyMB = caps.vramMB || 0;
  if (proxyMB > 0 && proxyMB < m.needGB * 1024 * 0.4) {
    return { ok: true, level: 'tight', msg: `Potrebbe non entrare in GPU: ~${m.needGB} GB di VRAM consigliati. Se va in errore, scegli un modello più piccolo.` };
  }
  return { ok: true, level: 'ok', msg: `~${m.sizeGB} GB, scaricati una volta dal web e poi offline.` };
}

// Is an engine/load error a GPU out-of-memory / device-lost (→ "pick a smaller model"), vs a transient
// network/load failure (→ "retry")? Used to give an honest, actionable message instead of a raw stack.
export function isOutOfMemoryError(err) {
  const s = String((err && (err.message || err.name)) || err || '').toLowerCase();
  return /out of memory|oom|exceeds the limit|maxbuffer|maxstoragebuffer|device lost|allocation failed|out of device memory/.test(s);
}

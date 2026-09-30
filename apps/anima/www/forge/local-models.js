// local-models.js — catalog of in-browser chat models (WebLLM/MLC on WebGPU), ordered best → smallest.
// Every id EXISTS in the vendored web-llm.js prebuiltAppConfig (v0_2_84 libs), so WebLLM already knows each
// model's library and weight URLs. Sizes are the REAL download (sum of the weight shards in each model's
// Hugging Face tensor-cache.json, measured 2026-09-29); needGB is WebLLM's own vram_required_MB. Pure &
// DOM-free → host-testable (tools/anima-host/forge-local-models.test.mjs).
//
// What changed and why:
//   • Current generation: Qwen3 0.6B / 1.7B / 4B / 8B (multilingual, tool-aware) instead of Qwen2.5.
//   • No false promises: the old default claimed "on the SD, installs without internet" — it was never staged
//     and WebLLM's model_lib always came from GitHub. Nothing here claims offline install any more.
//   • shader-f16: every id is a q4f16 build; a GPU without shader-f16 transparently gets the q4f32 sibling
//     (buildFor), instead of WebLLM's ShaderF16SupportError surfacing as "check your connection".
//   • The recommendation reads the real adapter class (discrete / integrated / fallback), not maxBufferSize
//     dressed up as VRAM. An explicit user choice still always wins.
//   • Qwen2.5 entries stay (legacy) so a model someone already downloaded keeps working.

export const LOCAL_MODELS = [
  { id: 'Qwen3-8B-q4f16_1-MLC',   label: 'Qwen3 8B',   sizeGB: 4.61, needGB: 5.7, needF32GB: 6.9, tier: 'max',      note: 'massima qualità, GPU dedicata ≥ 8 GB' },
  { id: 'Qwen3-4B-q4f16_1-MLC',   label: 'Qwen3 4B',   sizeGB: 2.26, needGB: 3.4, needF32GB: 4.3, tier: 'strong',   note: 'ottimo agente su GPU dedicata' },
  { id: 'Qwen3-1.7B-q4f16_1-MLC', label: 'Qwen3 1.7B', sizeGB: 0.97, needGB: 2.0, needF32GB: 2.6, tier: 'balanced', best: true, note: 'consigliato — gira anche su GPU integrate' },
  { id: 'Qwen3-0.6B-q4f16_1-MLC', label: 'Qwen3 0.6B', sizeGB: 0.34, needGB: 1.4, needF32GB: 1.9, tier: 'light',    note: 'minimo, gira quasi ovunque' },
  // legacy (previous catalog): still valid ids, shown only when already chosen
  { id: 'Qwen2.5-7B-Instruct-q4f16_1-MLC',   label: 'Qwen2.5 7B',   sizeGB: 4.7,  needGB: 5.1, needF32GB: 5.9, tier: 'max',      legacy: true },
  { id: 'Llama-3.1-8B-Instruct-q4f16_1-MLC', label: 'Llama 3.1 8B', sizeGB: 5.0,  needGB: 5.3, needF32GB: 6.1, tier: 'max',      legacy: true },
  { id: 'Qwen2.5-3B-Instruct-q4f16_1-MLC',   label: 'Qwen2.5 3B',   sizeGB: 2.0,  needGB: 2.5, needF32GB: 2.9, tier: 'strong',   legacy: true },
  { id: 'Qwen2.5-1.5B-Instruct-q4f16_1-MLC', label: 'Qwen2.5 1.5B', sizeGB: 0.87, needGB: 1.6, needF32GB: 1.9, tier: 'balanced', legacy: true },
  { id: 'Qwen2.5-0.5B-Instruct-q4f16_1-MLC', label: 'Qwen2.5 0.5B', sizeGB: 0.3,  needGB: 0.9, needF32GB: 1.1, tier: 'light',    legacy: true },
];

// The recommended default when nothing better is known: fits integrated GPUs, a real step up from 0.6B.
export const DEFAULT_LOCAL_MODEL = 'Qwen3-1.7B-q4f16_1-MLC';

export function localModelById(id) {
  const base = String(id || '').replace('-q4f32_1-', '-q4f16_1-');
  return LOCAL_MODELS.find((m) => m.id === base) || null;
}
// The models to OFFER in a picker: the current generation, plus a legacy one if it is the current choice.
export function offeredModels(current) {
  return LOCAL_MODELS.filter((m) => !m.legacy || m.id === current);
}

// The build to actually load on this GPU: q4f16 when shader-f16 exists, else its q4f32 sibling.
export function buildFor(id, caps = {}) {
  if (!id) return id;
  return caps.f16 === false ? String(id).replace('-q4f16_1-', '-q4f32_1-') : String(id).replace('-q4f32_1-', '-q4f16_1-');
}

// Best model for this machine, from the adapter class (WebGPU never exposes total VRAM):
//   discrete GPU → Qwen3 4B · integrated/unified with ≥ 8 GB RAM → 1.7B · anything weaker → 0.6B.
export function recommendModel(caps = {}) {
  if (!caps.webgpu) return null;
  const cls = caps.adapterClass || 'integrated';
  if (cls === 'fallback') return 'Qwen3-0.6B-q4f16_1-MLC';
  if (cls === 'discrete') return 'Qwen3-4B-q4f16_1-MLC';
  const ram = caps.deviceMemoryGB || 0;
  return ram && ram < 8 ? 'Qwen3-0.6B-q4f16_1-MLC' : 'Qwen3-1.7B-q4f16_1-MLC';
}

// The model to load: an explicit, valid user choice ALWAYS wins (the user knows their GPU); unset → the
// recommendation for this machine, else the default.
export function resolveLocalModel(stored, caps) {
  if (stored && localModelById(stored)) return localModelById(stored).id;
  const c = (caps && typeof caps === 'object') ? caps : {};
  return recommendModel(c) || DEFAULT_LOCAL_MODEL;
}

// Compatibility verdict for a model on this client. caps: { webgpu, f16, adapterClass, vramMB }.
//   { ok, level: 'ok'|'tight'|'no-webgpu'|'unknown', cause?, msg }
// Only "no WebGPU at all" blocks. 'tight' is advisory and never blocks a deliberate choice.
export function localModelCompat(id, caps = {}, env = {}) {
  const m = localModelById(id);
  if (!m) return { ok: false, level: 'unknown', msg: 'Modello sconosciuto.' };
  if (!caps.webgpu) return { ok: false, level: 'no-webgpu', cause: webgpuCause(caps, env), msg: 'Richiede WebGPU (Chrome/Edge con accelerazione hardware).' };
  const need = caps.f16 === false ? m.needF32GB : m.needGB;
  const cls = caps.adapterClass || '';
  const tight = cls === 'fallback' || (cls === 'integrated' && need > 3.5)
    || (caps.vramMB > 0 && caps.vramMB < need * 1024 * 0.4);
  if (tight) return { ok: true, level: 'tight', need, msg: `Potrebbe non entrare in GPU: ~${need} GB di VRAM consigliati. Se va in errore, scegli un modello più piccolo.` };
  return { ok: true, level: 'ok', need, msg: `~${m.sizeGB} GB, scaricati una volta dal web e poi offline.` };
}

// WHY there is no WebGPU (kept for callers that have not moved to /capabilities.js diagnose()).
//   'insecure' — plain-HTTP page (http://<device-ip>) · 'browser' — no WebGPU at all · 'blocked' — no adapter
export function webgpuCause(caps = {}, env = {}) {
  if (caps.webgpu) return null;
  const r = String(caps.reason || 'no-webgpu');
  if (r === 'no-webgpu') return env.secure === false ? 'insecure' : 'browser';
  return 'blocked';
}

// GPU out-of-memory / device-lost (→ "pick a smaller model") vs a transient failure (→ "retry").
export function isOutOfMemoryError(err) {
  const s = String((err && (err.message || err.name)) || err || '').toLowerCase();
  return /out of memory|oom|exceeds the limit|maxbuffer|maxstoragebuffer|device lost|allocation failed|out of device memory/.test(s);
}
// WebLLM's "this GPU has no shader-f16" (only reachable if a q4f16 build is forced on such a GPU).
export function isShaderF16Error(err) {
  return /shader-?f16|ShaderF16SupportError/i.test(String((err && (err.message || err.name)) || err || ''));
}

// Strip a reasoning block a thinking model may still emit (Qwen3 with thinking left on).
export function stripThinking(text) {
  return String(text || '').replace(/<think>[\s\S]*?<\/think>\s*/g, '').replace(/^[\s\S]*?<\/think>\s*/, '').trim();
}

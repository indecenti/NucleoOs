// webgpu-probe.js — decide whether (and how) the diffusion engine can run on this client, honestly. Mirrors
// the WebLLM VRAM probe (router.js) but with diffusion-appropriate thresholds: SDXS is one-step and ~430 MB,
// so it runs on modest GPUs and even falls back to the WASM execution provider — far below WebLLM's needs.
// Pure & DOM-free: navigator is injected so the decision logic is host-testable.
//
// probeDiffusion({ navigator }) -> { ok, ep:'webgpu'|'wasm'|'none', vramMB, reason, adapter }
//   ok=false only when neither WebGPU nor a WASM fallback is usable.

export const MIN_VRAM_MB = 1500;      // below this WebGPU works but may page; we warn, not block
export const RECOMMEND_VRAM_MB = 2500;

export async function probeDiffusion({ navigator = (typeof globalThis !== 'undefined' ? globalThis.navigator : undefined), allowWasm = true } = {}) {
  const nav = navigator;
  // WebGPU path
  if (nav && nav.gpu && typeof nav.gpu.requestAdapter === 'function') {
    try {
      const adapter = await nav.gpu.requestAdapter({ powerPreference: 'high-performance' });
      if (adapter) {
        const lim = adapter.limits || {};
        const maxBuf = lim.maxBufferSize || lim.maxStorageBufferBindingSize || 0;
        const vramMB = maxBuf ? Math.round(maxBuf / (1024 * 1024)) : 0;
        const adapterInfo = (adapter.info || {});
        return {
          ok: true, ep: 'webgpu', vramMB,
          warn: vramMB && vramMB < MIN_VRAM_MB ? 'low-vram' : null,
          reason: vramMB ? `webgpu adapter, ~${vramMB} MB max buffer` : 'webgpu adapter (vram unknown)',
          adapter: { vendor: adapterInfo.vendor || '', architecture: adapterInfo.architecture || '' },
        };
      }
    } catch (e) { /* fall through to WASM */ }
  }
  // WASM fallback (onnxruntime-web wasm EP). Slow but works without a GPU.
  if (allowWasm) return { ok: true, ep: 'wasm', vramMB: 0, warn: 'wasm-slow', reason: 'no WebGPU adapter — WASM fallback (slow)' };
  return { ok: false, ep: 'none', vramMB: 0, reason: 'no WebGPU and WASM disabled' };
}

// Short, honest one-liner for the UI from a probe result.
export function probeSummary(p, lang = 'it') {
  if (!p || !p.ok) return lang === 'en' ? 'No GPU support — generation unavailable here.' : 'Nessun supporto GPU — generazione non disponibile qui.';
  if (p.ep === 'wasm') return lang === 'en' ? 'No WebGPU — will run on CPU (WASM), slow.' : 'Niente WebGPU — userà la CPU (WASM), lento.';
  if (p.warn === 'low-vram') return lang === 'en' ? `WebGPU OK (~${p.vramMB} MB — a bit tight).` : `WebGPU OK (~${p.vramMB} MB — un po’ al limite).`;
  return lang === 'en' ? `WebGPU OK${p.vramMB ? ` (~${p.vramMB} MB)` : ''}.` : `WebGPU OK${p.vramMB ? ` (~${p.vramMB} MB)` : ''}.`;
}

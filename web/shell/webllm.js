/* SPDX-License-Identifier: AGPL-3.0-or-later */
// NucleoOS — language models that run IN THE BROWSER, on the user's GPU (WebGPU + WebLLM).
//
// Nothing leaves the computer: the weights download once from the WebLLM CDN, are cached by the
// browser, and answer offline afterwards. The catalogue is WebLLM's own prebuilt list (read live, so
// a new WebLLM release brings its new models), filtered to chat models and ranked against this GPU.
// DOM-free; the UI lives in ai-keys.js (Settings) and copilot.js uses chat().
//
//   import * as GPU from '/webllm.js';
//   const caps = await GPU.probe();                    // {webgpu, vramMB, adapter, reason}
//   const list = await GPU.catalogue();                // [{id, label, vramMB, fits, small, cached}]
//   await GPU.load(id, (p) => console.log(p.progress, p.text));
//   const text = await GPU.chat([{role:'user', content:'ciao'}], {signal});
//   await GPU.unload(); await GPU.remove(id);

// Pinned (the same release ANIMA loads, apps/anima/www/local-llm.js): an unpinned name runs whatever is published
// under it next, with the shell's origin.
const CDN = 'https://cdn.jsdelivr.net/npm/@mlc-ai/web-llm@0.2.84/+esm';
const KEY = 'anima.webgpuModel';            // the chosen model id (per browser)

let _lib = null, _engine = null, _loaded = null, _loading = null, _caps = null;

async function lib() {
  if (!_lib) _lib = await import(/* webpackIgnore: true */ CDN);
  return _lib;
}

// {webgpu, vramMB, adapter, reason}: vramMB is an estimate (the adapter's max buffer size).
export async function probe() {
  if (_caps) return _caps;
  const c = { webgpu: false, vramMB: 0, adapter: '', reason: '' };
  try {
    if (!window.isSecureContext)
      // WebGPU and the model cache exist only in a secure context. The device serves plain HTTP, so
      // the browser hides them: say how to get them instead of a bare "not available".
      c.reason = 'il browser attiva WebGPU solo su HTTPS: in Chrome/Edge apri chrome://flags/#unsafely-treat-insecure-origin-as-secure, ' +
                 'aggiungi ' + location.origin + ', riavvia il browser. In alternativa usa un Server locale (Ollama).';
    else if (!('gpu' in navigator)) c.reason = 'WebGPU non disponibile in questo browser (serve Chrome / Edge recente)';
    else {
      const a = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
      if (!a) c.reason = 'nessuna GPU compatibile WebGPU';
      else {
        c.webgpu = true;
        c.vramMB = Math.round(((a.limits && a.limits.maxBufferSize) || 0) / 1048576);
        try { const i = a.info || (a.requestAdapterInfo ? await a.requestAdapterInfo() : null); if (i) c.adapter = [i.vendor, i.architecture || i.device].filter(Boolean).join(' '); } catch {}
      }
    }
  } catch (e) { c.reason = String(e && e.message || e); }
  _caps = c;
  return c;
}

const prettify = (id) => id.replace(/-MLC$/, '').replace(/-q\d+f\d+(_\d)?/, '').replace(/-/g, ' ');

// The chat models WebLLM can run, smallest first, each marked against this GPU.
export async function catalogue() {
  const w = await lib();
  const caps = await probe();
  const list = (w.prebuiltAppConfig && w.prebuiltAppConfig.model_list) || [];
  const out = [];
  for (const m of list) {
    const id = m.model_id || '';
    if (!/instruct|chat|-it-/i.test(id) || /embed|vision|-1k$/i.test(id)) continue;
    const vram = Math.round(m.vram_required_MB || 0);
    let cached = false;
    try { cached = await w.hasModelInCache(id); } catch {}
    out.push({ id, label: prettify(id), vramMB: vram, small: !!m.low_resource_required,
               fits: !caps.vramMB || !vram || vram <= caps.vramMB * 0.9, cached });
  }
  return out.sort((a, b) => (b.cached - a.cached) || (a.vramMB - b.vramMB));
}

export function chosen() { try { return localStorage.getItem(KEY) || ''; } catch { return ''; } }
export function choose(id) { try { id ? localStorage.setItem(KEY, id) : localStorage.removeItem(KEY); } catch {} }
export function loaded() { return _loaded; }
export function ready() { return !!_engine; }

// Load a model (download + cache on first use). onProgress({progress 0..1, text}).
export async function load(id, onProgress) {
  id = id || chosen();
  if (!id) throw new Error('nessun modello scelto');
  if (_engine && _loaded === id) return _engine;
  if (_loading) return _loading;
  if (_engine) await unload();
  _loading = (async () => {
    const w = await lib();
    const eng = await w.CreateMLCEngine(id, { initProgressCallback: (p) => { try { onProgress && onProgress(p); } catch {} } });
    _engine = eng; _loaded = id; choose(id);
    try { localStorage.setItem('anima.webgpuReady', id); } catch {}   // Settings: "a browser model is ready"
    return eng;
  })();
  try { return await _loading; }
  catch (e) {
    const msg = String(e && e.message || e);
    if (/out of memory|OOM|device.*lost|allocate/i.test(msg)) throw new Error('il modello non entra nella memoria della GPU: scegline uno più piccolo');
    throw e;
  } finally { _loading = null; }
}

export async function unload() {
  const e = _engine; _engine = null; _loaded = null;
  if (e && e.unload) { try { await e.unload(); } catch {} }
}

// Drop a model's weights from the browser cache.
export async function remove(id) {
  if (_loaded === id) await unload();
  const w = await lib();
  await w.deleteModelAllInfoInCache(id);
  try { if (localStorage.getItem('anima.webgpuReady') === id) localStorage.removeItem('anima.webgpuReady'); } catch {}
}

// One chat completion on the loaded model (loads the chosen one if needed). Streams to onToken.
export async function chat(messages, { signal, maxTokens = 768, temperature = 0.4, onToken } = {}) {
  const eng = _engine || await load();
  const stream = await eng.chat.completions.create({ messages, stream: true, max_tokens: maxTokens, temperature });
  let text = '';
  for await (const chunk of stream) {
    if (signal && signal.aborted) { try { eng.interruptGenerate(); } catch {} break; }
    const d = chunk.choices && chunk.choices[0] && chunk.choices[0].delta && chunk.choices[0].delta.content;
    if (d) { text += d; if (onToken) onToken(d, text); }
  }
  return text;
}

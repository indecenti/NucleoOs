// local-llm.js — the "GPU locale" tier for the ANIMA web CHAT (distinct from the forge editor). Reuses
// the same browser model as the programming skill (Qwen2.5-Coder on WebGPU via WebLLM): it runs OFFLINE
// on the user's own GPU — no cloud. The chat uses it as the tier BETWEEN the grounded device dictionary
// and Grok, so the fallback order for translation is:  device dictionary → GPU locale (this) → Grok (last).
// The ~0.9 GB model is downloaded+cached by WebLLM on first use (lazy). DOM-free; only WebGPU + dynamic
// import of @mlc-ai/web-llm (vendored on the device SD, CDN fallback) — exactly forge-demo's loader.
import { probeWebGPU } from './forge/webllm-engine.js';
import { resolveLocalModel, localModelById, isOutOfMemoryError } from './forge/local-models.js';
import * as ctxkit from './contextkit.js';   // same context engine as the cloud path, with the small WebLLM profile

const importWebLLM = async () => {
  try { return await import('./forge/vendor/web-llm.js'); }
  catch { return await import('https://esm.run/@mlc-ai/web-llm'); }
};

let _engine = null, _loading = null, _caps = null, _loadedModel = null;

// The browser model to load: the user's explicit choice (Settings ▸ IA), else the recommended best for a
// strong GPU. WebLLM downloads it from the CDN on first use (automatic + transparent) and caches it offline.
export function chosenLocalModel() {
  let stored = null; try { stored = localStorage.getItem('anima.localModel'); } catch {}
  return resolveLocalModel(stored, (_caps && _caps.vramMB) || 0);
}
export function loadedLocalModel() { return _loadedModel; }

// Drop the resident engine so a model change (or a VRAM reclaim) takes effect on the next inference.
export async function unloadLocal() {
  const e = _engine; _engine = null; _loadedModel = null; _loading = null;
  if (e && typeof e.unload === 'function') { try { await e.unload(); } catch { /* best-effort */ } }
}

// { webgpu, vramMB, reason } — cached. webgpu=false → the chat must skip the GPU-locale tier.
export async function probeLocal() {
  if (_caps) return _caps;
  const gpu = await probeWebGPU();
  _caps = { webgpu: gpu.supported, vramMB: gpu.vramMB || 0, reason: gpu.reason };
  return _caps;
}

export function localReady() { return !!_engine; }

// Lazily create the WebLLM engine (downloads+caches the model on first call). onProgress receives
// WebLLM's { progress (0..1), text } so the chat can show a one-time loading line.
export async function loadLocal(onProgress) {
  if (_engine) return _engine;
  if (_loading) return _loading;
  const modelId = chosenLocalModel();
  _loading = (async () => {
    const webllm = await importWebLLM();
    const eng = await webllm.CreateMLCEngine(modelId, {
      initProgressCallback: (p) => { try { onProgress && onProgress({ ...p, model: modelId }); } catch { /* ignore */ } },
    });
    _engine = eng; _loadedModel = modelId; _loading = null; return eng;
  })().catch((e) => {
    _loading = null;
    // An OOM/device-lost means the model didn't fit this GPU → an HONEST, actionable error, not a raw stack.
    if (isOutOfMemoryError(e)) {
      const m = localModelById(modelId);
      const e2 = new Error('Il modello ' + (m ? m.label : modelId) + ' non è entrato nella GPU (serve più VRAM). Scegli un modello più piccolo in Impostazioni ▸ IA.');
      e2.kind = 'oom'; e2.model = modelId; throw e2;
    }
    throw e;
  });
  return _loading;
}

// Translate a request with the local model. Returns the translation text (throws on load/inference error,
// so the caller can fall back to Grok). The model is a coder model but multilingual — fine for IT<->EN.
export async function translateLocal(q, lang, onProgress) {
  const eng = await loadLocal(onProgress);
  const sys = 'You are a translation engine between Italian and English. The user gives a request such as '
    + '"traduci X in inglese" / "translate X to italian" / "come si dice X in inglese". Carry it out: output '
    + 'ONLY the translation of the phrase X into the requested language — no preamble, no quotes, no notes. '
    + 'If no target language is given, translate Italian->English or English->Italian.';
  const res = await eng.chat.completions.create({
    messages: [{ role: 'system', content: sys }, { role: 'user', content: String(q) }],
    temperature: 0.2,
  });
  const txt = res && res.choices && res.choices[0] && res.choices[0].message && res.choices[0].message.content;
  return (txt || '').trim();
}

// General QA with the local browser model (WebLLM) — the ONLINE-mode 2nd priority, used when the cloud
// API is unavailable (no key / no internet / failed). A real generative model on the user's own GPU, so
// online mode never has to fall back to the offline retrieval cascade. Returns a result object shaped
// like the chat expects, or null on empty output. Throws on load/inference error (caller -> honest error).
export async function queryLocal(q, lang, history, onProgress) {
  const eng = await loadLocal(onProgress);
  // Build the SAME budgeted, injection-safe transcript as the cloud path, but with the small WebLLM
  // profile (short window, few verbatim turns, brief replies) — these models are reduced, so we keep
  // the context lean on purpose. system goes as the OpenAI 'system' message; messages are user/assistant.
  const { system, messages, maxTokens, temperature } = ctxkit.assemble({ history: history || [], user: q, mode: 'webllm', lang });
  const res = await eng.chat.completions.create({
    messages: [{ role: 'system', content: system }, ...messages],
    temperature, max_tokens: maxTokens,
  });
  const txt = res && res.choices && res.choices[0] && res.choices[0].message && res.choices[0].message.content;
  const reply = (txt || '').trim();
  if (!reply) return null;
  return { reply, tier: 'M4-local', intent: /```/.test(reply) ? 'code' : 'local', confidence: 60, domain: 'local', trace: 'Browser LLM · WebLLM' };
}

// Browser-safe translation-request detector (mirror of firmware nucleo_anima_translate_is_request and the
// Node twin — but no fs). Italian-vowel fold + the same 0-FP triggers (verb / "come si dice" frame / noun+lang).
const FOLD = { 'à':'a','á':'a','â':'a','è':'e','é':'e','ê':'e','ì':'i','í':'i','î':'i','ò':'o','ó':'o','ô':'o','ù':'u','ú':'u','û':'u' };
function toks(raw) {
  const out = []; let cur = '';
  for (const ch of String(raw || '')) {
    let c = FOLD[ch];
    if (c === undefined && ch.charCodeAt(0) < 128 && /[0-9a-z]/i.test(ch)) c = ch.toLowerCase();
    if (c) { if (cur.length < 23) cur += c; } else if (cur) { out.push(cur); cur = ''; if (out.length >= 24) return out; }
  }
  if (cur && out.length < 24) out.push(cur);
  return out;
}
const FRAMES = [['come','si','dice'], ['come','si','dicono'], ['how','do','you','say'], ['how','do','i','say'], ['how','to','say']];
export function isTranslateRequest(raw) {
  const t = toks(raw);
  if (!t.length) return false;
  let verb = false, noun = false, lang = false;
  for (const w of t) {
    if (w.startsWith('traduc') || w.startsWith('tradur') || w.startsWith('translat')) verb = true;
    else if (w.startsWith('traduzion') || w === 'translation' || w === 'translations') noun = true;
    if (w.startsWith('ingles') || w === 'english' || w.startsWith('italian') || w === 'italiano') lang = true;
  }
  let phrase = false;
  for (const f of FRAMES) {
    for (let s = 0; s + f.length <= t.length; s++) {
      let k = 0; for (; k < f.length; k++) if (t[s + k] !== f[k]) break;
      if (k === f.length) phrase = true;
    }
  }
  return verb || phrase || (noun && lang);
}

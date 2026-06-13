// local-llm.js — the "GPU locale" tier for the ANIMA web CHAT (distinct from the forge editor). Reuses
// the same browser model as the programming skill (Qwen2.5-Coder on WebGPU via WebLLM): it runs OFFLINE
// on the user's own GPU — no cloud. The chat uses it as the tier BETWEEN the grounded device dictionary
// and Grok, so the fallback order for translation is:  device dictionary → GPU locale (this) → Grok (last).
// The ~0.9 GB model is downloaded+cached by WebLLM on first use (lazy). DOM-free; only WebGPU + dynamic
// import of @mlc-ai/web-llm (vendored on the device SD, CDN fallback) — exactly forge-demo's loader.
import { CODER_MODEL, probeWebGPU } from './forge/webllm-engine.js';

const importWebLLM = async () => {
  try { return await import('./forge/vendor/web-llm.js'); }
  catch { return await import('https://esm.run/@mlc-ai/web-llm'); }
};

let _engine = null, _loading = null, _caps = null;

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
  _loading = (async () => {
    const webllm = await importWebLLM();
    const eng = await webllm.CreateMLCEngine(CODER_MODEL, {
      initProgressCallback: (p) => { try { onProgress && onProgress(p); } catch { /* ignore */ } },
    });
    _engine = eng; _loading = null; return eng;
  })().catch((e) => { _loading = null; throw e; });
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

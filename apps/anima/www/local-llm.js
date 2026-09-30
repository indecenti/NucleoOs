// local-llm.js — the "GPU locale" tier for the ANIMA web CHAT (distinct from the forge editor). Reuses
// the same browser model as the programming skill (Qwen2.5-Coder on WebGPU via WebLLM): it runs OFFLINE
// on the user's own GPU — no cloud. The chat uses it as the tier BETWEEN the grounded device dictionary
// and Grok, so the fallback order for translation is:  device dictionary → GPU locale (this) → Grok (last).
// The model (~0.3–5 GB) is NEVER pulled implicitly: the chat uses this tier only when it is already
// installed (localAvailable), and a download happens only through loadLocal(..., { allowDownload:true })
// — the Settings "Download now" button, i.e. explicit consent — under the OS-wide download lock.
// DOM-free; only WebGPU + dynamic import of @mlc-ai/web-llm (vendored on the device SD, CDN fallback).
import { probeWebGPU } from './forge/webllm-engine.js';
import { resolveLocalModel, localModelById, isOutOfMemoryError, isShaderF16Error, buildFor, stripThinking } from './forge/local-models.js';
import * as ctxkit from './contextkit.js';   // same context engine as the cloud path, with the small WebLLM profile

// CDN FIRST (the same pinned version as the vendored copy), SD copy only as the offline fallback: reading
// a 6 MB bundle from the Cardputer's single-task web server on every first use is exactly the load the
// device must never carry when the internet is right there. A stalled CDN falls back after 8 s.
const WEBLLM_CDN = 'https://cdn.jsdelivr.net/npm/@mlc-ai/web-llm@0.2.84/+esm';
const importWebLLM = async () => {
  try {
    return await Promise.race([import(WEBLLM_CDN), new Promise((_, rej) => setTimeout(() => rej(new Error('cdn timeout')), 8000))]);
  } catch { return await import('./forge/vendor/web-llm.js'); }
};

let _engine = null, _loading = null, _caps = null, _loadedModel = null;

// The browser model to load: the user's explicit choice (Settings ▸ IA), else the recommended best for a
// strong GPU. It is downloaded only on explicit consent (loadLocal allowDownload), then cached offline.
export function chosenLocalModel() {
  let stored = null; try { stored = localStorage.getItem('anima.localModel'); } catch {}
  return resolveLocalModel(stored, _caps || {});
}
// The exact BUILD loaded on this GPU (q4f32 when the adapter lacks shader-f16).
export function chosenLocalBuild() { return buildFor(chosenLocalModel(), _caps || {}); }
export function loadedLocalModel() { return _loadedModel; }

// Drop the resident engine so a model change (or a VRAM reclaim) takes effect on the next inference.
export async function unloadLocal() {
  const e = _engine; _engine = null; _loadedModel = null; _loading = null;
  if (e && typeof e.unload === 'function') { try { await e.unload(); } catch { /* best-effort */ } }
}

// { webgpu, vramMB, reason } — cached. webgpu=false → the chat must skip the GPU-locale tier.
export async function probeLocal({ fresh = false } = {}) {
  if (_caps && !fresh) return _caps;
  const gpu = await probeWebGPU();
  let deviceMemoryGB = 0; try { deviceMemoryGB = navigator.deviceMemory || 0; } catch {}
  _caps = { webgpu: gpu.supported, vramMB: gpu.vramMB || 0, reason: gpu.reason, f16: gpu.supported ? gpu.f16 : undefined,
    adapter: gpu.adapter || null, adapterClass: gpu.adapterClass || null, deviceMemoryGB };
  return _caps;
}

export function localReady() { return !!_engine; }

// ---- is the model already on this machine? -------------------------------------------------------
// Answers WITHOUT downloading anything and without importing the 6 MB WebLLM bundle: it reads the
// browser Cache Storage the weights live in. Two places count as installed:
//   1. WebLLM's own cache ('webllm/model', keyed by the Hugging Face weight URL) — a finished load;
//   2. the Forge model manager's verified cache ('anima-forge-models', '/fc/<id>/<file>'), which the shell
//      service worker serves WebLLM's weight requests from — so only when a SW controls this page
//      (it is inert on http://LAN-IP, where WebLLM would go to the network instead).
// "Installed" = the tensor index is cached AND every shard it lists is too; a half-finished pull is not.
const _installed = new Set();   // positive answers only: a model doesn't uninstall itself mid-session
async function allShardsCached(cache, indexKey, keyOf) {
  const resp = await cache.match(indexKey);
  if (!resp) return false;
  const j = await resp.json();
  const recs = (j && Array.isArray(j.records)) ? j.records : [];
  if (!recs.length) return false;
  const seen = new Set();
  for (const r of recs) {
    const p = r && r.dataPath;
    if (!p || seen.has(p)) continue;
    seen.add(p);
    if (!(await cache.match(keyOf(p)))) return false;
  }
  return true;
}
export async function localModelCached(modelId = chosenLocalModel()) {
  if (!modelId) return false;
  if (_installed.has(modelId)) return true;
  if (typeof caches === 'undefined' || !caches || typeof caches.has !== 'function') return false;
  let ok = false;
  try {
    if (await caches.has('webllm/model')) {
      const c = await caches.open('webllm/model');
      const idx = (await c.keys()).find((k) => k.url.includes('/' + modelId + '/') && /\/(tensor|ndarray)-cache\.json$/.test(k.url));
      if (idx) ok = await allShardsCached(c, idx, (p) => new URL(p, idx.url).href);
    }
  } catch { ok = false; }
  if (!ok) {
    try {
      const sw = typeof navigator !== 'undefined' && navigator.serviceWorker && navigator.serviceWorker.controller;
      if (sw && await caches.has('anima-forge-models')) {
        const c = await caches.open('anima-forge-models');
        for (const n of ['tensor-cache.json', 'ndarray-cache.json']) {
          if (await allShardsCached(c, '/fc/' + modelId + '/' + n, (p) => '/fc/' + modelId + '/' + p)) { ok = true; break; }
        }
      }
    } catch { ok = false; }
  }
  if (ok) _installed.add(modelId);
  return ok;
}
// Can the chat use the GPU tier RIGHT NOW with zero download? (resident engine, or installed weights)
export async function localAvailable() {
  await probeLocal();
  if (_engine && _loadedModel === chosenLocalBuild()) return true;
  return localModelCached(chosenLocalBuild());
}

// OS-wide download gate (web/shell/dlgate.js): one big pull at a time across every app/tab. Lazy and
// optional — if the shell module isn't reachable the consented download simply runs ungated.
async function withDlLock(label, fn) {
  let gate = null;
  try { gate = (await import('/dlgate.js')).withDownloadLock; } catch { gate = null; }
  return gate ? gate(label, fn) : fn();
}

// Create the WebLLM engine. Without opts.allowDownload it loads ONLY an installed model and otherwise
// throws { code:'NOT_INSTALLED' } — so no chat/translate path can trigger a surprise multi-GB pull.
// opts.allowDownload = explicit user consent (Settings); that pull runs under the download lock.
// onProgress receives WebLLM's { progress (0..1), text }.
export async function loadLocal(onProgress, opts = {}) {
  if (_engine) return _engine;
  if (_loading) return _loading;
  await probeLocal();                          // shader-f16 decides which build loads
  const modelId = chosenLocalBuild();
  if (!opts.allowDownload && !(await localModelCached(modelId))) {
    throw Object.assign(new Error('local model not installed: ' + modelId), { code: 'NOT_INSTALLED', model: modelId });
  }
  if (_engine) return _engine;     // re-check: another caller may have finished while we probed the cache
  if (_loading) return _loading;
  _loading = (async () => {
    const webllm = await importWebLLM();
    const create = () => webllm.CreateMLCEngine(modelId, {
      initProgressCallback: (p) => { try { onProgress && onProgress({ ...p, model: modelId }); } catch { /* ignore */ } },
    });
    const m = localModelById(modelId);
    const eng = opts.allowDownload ? await withDlLock('ANIMA · ' + (m ? m.label : modelId), create) : await create();
    _engine = eng; _loadedModel = modelId; _loading = null; _installed.add(modelId); return eng;
  })().catch((e) => {
    _loading = null;
    // An OOM/device-lost means the model didn't fit this GPU → an HONEST, actionable error, not a raw stack.
    if (isOutOfMemoryError(e)) {
      const m = localModelById(modelId);
      const e2 = new Error('Il modello ' + (m ? m.label : modelId) + ' non è entrato nella GPU (serve più VRAM). Scegli un modello più piccolo in Impostazioni ▸ IA.');
      e2.kind = 'oom'; e2.model = modelId; throw e2;
    }
    if (isShaderF16Error(e)) {               // the probe said f16 but the device refused: remember, load q4f32 next time
      if (_caps) _caps.f16 = false;
      const e3 = new Error('GPU without shader-f16: retrying with the 32-bit build.'); e3.kind = 'no-f16'; e3.model = modelId; throw e3;
    }
    throw e;
  });
  return _loading;
}

// Run one chat completion on the resident engine. opts.signal stops generation (WebLLM's
// interruptGenerate — the GPU stops, the partial text is discarded by the caller); opts.onDelta streams
// tokens as they are decoded. Throws the abort reason when stopped.
async function generate(eng, body, opts = {}) {
  const { signal, onDelta } = opts;
  if (signal && signal.aborted) throw signal.reason || Object.assign(new Error('Stopped'), { name: 'AbortError' });
  const stop = () => { try { eng.interruptGenerate && eng.interruptGenerate(); } catch { /* best-effort */ } };
  if (signal) signal.addEventListener('abort', stop, { once: true });
  // Qwen3 thinks out loud by default (<think>…</think>): slow on a small GPU and leaks into the chat.
  // Off for chat/translation; WebLLM honours extra_body.enable_thinking. Any residue is stripped below.
  if (/^Qwen3-/.test(_loadedModel || '') && !(body.extra_body && 'enable_thinking' in body.extra_body)) body = { ...body, extra_body: { ...(body.extra_body || {}), enable_thinking: false } };
  try {
    if (onDelta) {
      const chunks = await eng.chat.completions.create({ ...body, stream: true });
      let full = '';
      for await (const ch of chunks) {
        if (signal && signal.aborted) break;
        const d = ch && ch.choices && ch.choices[0] && ch.choices[0].delta && ch.choices[0].delta.content;
        if (d) { full += d; try { onDelta(d); } catch { /* renderer errors never kill the stream */ } }
      }
      if (signal && signal.aborted) throw signal.reason || Object.assign(new Error('Stopped'), { name: 'AbortError' });
      return stripThinking(full);
    }
    const res = await eng.chat.completions.create(body);
    if (signal && signal.aborted) throw signal.reason || Object.assign(new Error('Stopped'), { name: 'AbortError' });
    return stripThinking((res && res.choices && res.choices[0] && res.choices[0].message && res.choices[0].message.content) || '');
  } finally {
    if (signal) signal.removeEventListener('abort', stop);
  }
}

// Translate a request with the local model. Returns the translation text (throws on load/inference error —
// incl. NOT_INSTALLED: it never downloads — so the caller can fall back to Grok). The model is a coder model but multilingual — fine for IT<->EN.
export async function translateLocal(q, lang, onProgress, opts = {}) {
  const eng = await loadLocal(onProgress);
  const sys = 'You are a translation engine for Italian, English, Spanish, French and German. The user gives a request '
    + 'such as "traduci X in inglese" / "translate X to German" / "¿cómo se dice X en francés?". Carry it out: output '
    + 'ONLY the translation of the phrase X into the requested language — no preamble, no quotes, no notes. '
    + 'If no target language is given, translate into English (or into Italian if X is already English).';
  const txt = await generate(eng, {
    messages: [{ role: 'system', content: sys }, { role: 'user', content: String(q) }],
    temperature: 0.2,
  }, { signal: opts.signal });
  return (txt || '').trim();
}

// General QA with the local browser model (WebLLM) — the ONLINE-mode 2nd priority, used when the cloud
// API is unavailable (no key / no internet / failed). A real generative model on the user's own GPU, so
// online mode never has to fall back to the offline retrieval cascade. Returns a result object shaped
// like the chat expects, or null on empty output. Throws on load/inference error (caller -> honest error),
// including NOT_INSTALLED: this never downloads the model.
// opts.signal: Stop/timeout interrupts generation. opts.onDelta(text): stream tokens as they decode.
export async function queryLocal(q, lang, history, onProgress, opts = {}) {
  const eng = await loadLocal(onProgress);
  // Build the SAME budgeted, injection-safe transcript as the cloud path, but with the small WebLLM
  // profile (short window, few verbatim turns, brief replies) — these models are reduced, so we keep
  // the context lean on purpose. system goes as the OpenAI 'system' message; messages are user/assistant.
  const { system, messages, maxTokens, temperature } = ctxkit.assemble({ history: history || [], user: q, mode: 'webllm', lang });
  const txt = await generate(eng, {
    messages: [{ role: 'system', content: system }, ...messages],
    temperature, max_tokens: maxTokens,
  }, opts);
  const reply = (txt || '').trim();
  if (!reply) return null;
  return { reply, tier: 'M4-local', intent: /```/.test(reply) ? 'code' : 'local', confidence: 60, domain: 'local', trace: 'Browser LLM · WebLLM' };
}

// The installed GPU model as an AGENT engine for the Agenti runtime's local loop (apps/agent/local-worker.js):
// { chat(messages, { grammar, temperature, signal }) → { text } }. Loads only an INSTALLED model (never a
// download mid-task); null when this browser has none.
// The action grammar is NOT handed to WebLLM: measured 2026-09-30 with Qwen3-1.7B on an RTX 5070, every
// constrained request (GBNF, even a one-rule grammar, or json_object) hung forever on 0.2.84 and aborted on
// 0.2.85 — and left the engine answering nothing afterwards. Free decoding runs at ~21 tok/s; the loop
// normalizes and strictly re-validates what the model wrote (normalizeActionsText + grammarAccepts).
// localStorage 'anima.webgpuGrammar'='1' re-enables it, to re-test a newer WebLLM.
const AGENT_STEP_MS = 120000;
export async function localAgentEngine() {
  if (!(await localAvailable())) return null;
  return {
    async chat(messages, opts = {}) {
      const eng = await loadLocal(null);
      const body = { messages, temperature: typeof opts.temperature === 'number' ? opts.temperature : 0.2, max_tokens: opts.maxTokens || 1024 };
      let useGrammar = false; try { useGrammar = localStorage.getItem('anima.webgpuGrammar') === '1'; } catch {}
      if (opts.grammar && useGrammar) body.response_format = { type: 'grammar', grammar: opts.grammar };
      // A step that never ends must not hang the turn: stop the GPU after AGENT_STEP_MS (or on the user's Stop).
      const ac = new AbortController();
      const timer = setTimeout(() => ac.abort(Object.assign(new Error('local model step timed out'), { name: 'TimeoutError' })), AGENT_STEP_MS);
      const outer = opts.signal; const relay = () => ac.abort(outer.reason);
      if (outer) { if (outer.aborted) relay(); else outer.addEventListener('abort', relay, { once: true }); }
      try { return { text: await generate(eng, body, { signal: ac.signal }), usage: {} }; }
      finally { clearTimeout(timer); if (outer) outer.removeEventListener('abort', relay); }
    },
    get model() { return _loadedModel || chosenLocalBuild(); },
  };
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

// providers.js — Atelier image-provider resolution + selection. Pure & DOM-free → host-tested.
//
// Paint's Atelier can produce an image three ways. This module decides which are AVAILABLE right now
// (from the device key vault + the live capabilities) and in what ORDER to prefer them, so the UI can
// offer the user full control with honest status:
//
//   1. ONLINE  — a real cloud image model (xAI Grok `grok-2-image`, OpenAI-images-compatible). Needs an
//                image-capable key + internet. The image bytes come back inline (b64) → drawn client-side;
//                the Cardputer never proxies them ("tutto cliente").
//   2. LOCAL   — the in-browser diffusion model (Stable Diffusion XS, ONNX Runtime Web on the client GPU).
//                Needs the model cached/on-SD + a usable runtime. Real neural generation, fully offline.
//   3. PREVIEW — the deterministic procedural fallback (NO weights, NO network). Explicitly NOT AI; only
//                offered when neither real engine can run (or the user opts in to a no-download sketch).
//
// Separately we resolve the best CHAT LLM (Claude > Grok > Groq) used to ENHANCE the prompt — so the whole
// pipeline is LLM-driven end to end: an LLM rewrites the idea into a professional prompt, a generative
// model renders it. Claude can't draw, so when it's the only cloud brain it powers enhancement and the
// pixels come from the local model — the honest meaning of "use Grok or Claude first".
//
// Mirrors the provider truth in apps/games/www/llm.js: the firmware nicknames the cloud teacher "Grok"
// but the real brand is whatever teacher.json points at. Grok (xAI) ≠ Groq (Llama host) — never conflate.

// ── brand identification (label never lies) ───────────────────────────────────────────────────────────
export function brandOf(cfg) {
  if (!cfg) return null;
  if (cfg.provider === 'anthropic') return 'Claude';
  if (cfg.provider === 'google') return 'Gemini';
  const h = (cfg.base || '').toLowerCase();
  if (h.includes('generativelanguage')) return 'Gemini';
  if (h.includes('x.ai')) return 'Grok';
  if (h.includes('groq.com')) return 'Groq';
  if (h.includes('openai.com')) return 'OpenAI';
  if (h.includes('mistral')) return 'Mistral';
  if (h.includes('together')) return 'Together';
  try { return new URL(cfg.base).host.replace(/^api\./, ''); } catch { return 'LLM'; }
}

// ── teacher.json → {chat, image} ──────────────────────────────────────────────────────────────────────
// chat  = best LLM for prompt enhancement (Claude > Grok > Groq/OpenAI) or null.
// image = best ONLINE image generator (xAI Grok, or any OpenAI-images-compatible endpoint via keys.image
//         / imageBase+imageModel) or null. Groq and Anthropic have NO image API → null unless an explicit
//         image endpoint is configured. Pure function of the parsed JSON (no I/O).
export function resolveConfigs(teacher) {
  const j = (teacher && typeof teacher === 'object') ? teacher : {};
  const keys = (j.keys && typeof j.keys === 'object') ? j.keys : {};

  // --- chat (same precedence as games/llm.js loadCfg) ---
  let chat = null;
  const anth = (keys.anthropic && keys.anthropic.key) ? keys.anthropic
    : (j.provider === 'anthropic' && j.key ? { base: j.base, model: j.model, key: j.key, version: j.version } : null);
  const xai = (keys.xai && keys.xai.key) ? keys.xai : (keys.grok && keys.grok.key) ? keys.grok
    : (j.key && ((j.provider === 'xai' || j.provider === 'grok') || /x\.ai/i.test(j.base || '')) ? { base: j.base, model: j.model, key: j.key } : null);
  const oa = (keys.groq && keys.groq.key) ? keys.groq : (keys.openai && keys.openai.key) ? keys.openai
    : (j.key && (!j.provider || j.provider === 'openai' || j.provider === 'groq') ? { base: j.base, model: j.model, key: j.key } : null);
  // Google Gemini — OpenAI-compat but CORS-blocked, so it MUST go via the device /api/llm proxy (proxy:true).
  const goog = (keys.google && keys.google.key) ? keys.google
    : (j.key && (j.provider === 'google' || /generativelanguage/i.test(j.base || '')) ? { base: j.base, model: j.model, key: j.key } : null);
  const gChat = (goog && goog.key) ? { provider: 'google', base: goog.base || 'https://generativelanguage.googleapis.com/v1beta/openai', model: goog.model || 'gemini-2.5-flash', key: goog.key, proxy: true } : null;
  if (j.provider === 'google' && gChat) chat = gChat;                  // active Gemini wins
  else if (anth && anth.key) chat = { provider: 'anthropic', base: anth.base || 'https://api.anthropic.com', model: anth.model || 'claude-sonnet-4-6', key: anth.key, version: anth.version || '2023-06-01' };
  else if (xai && xai.key) chat = { provider: 'openai', base: xai.base || 'https://api.x.ai/v1', model: xai.model || 'grok-3-mini', key: xai.key };
  else if (oa && oa.key) chat = { provider: 'openai', base: oa.base || 'https://api.groq.com/openai/v1', model: oa.model || 'llama-3.1-8b-instant', key: oa.key };
  else if (gChat) chat = gChat;                                        // saved Gemini as last resort (Claude/Grok/Groq preferred: browser-direct)

  // --- image (online) ---
  // Priority: an explicit image slot (any OpenAI-images endpoint, e.g. a future gpt-image-1) → xAI Grok.
  let image = null;
  const imgSlot = (keys.image && keys.image.key) ? keys.image
    : (j.imageKey ? { base: j.imageBase, model: j.imageModel, key: j.imageKey } : null);
  if (imgSlot && imgSlot.key) {
    image = { provider: 'openai', base: imgSlot.base || 'https://api.x.ai/v1', model: imgSlot.model || 'grok-2-image', key: imgSlot.key };
  } else if (xai && xai.key) {
    image = { provider: 'openai', base: (xai.base || 'https://api.x.ai/v1'), model: 'grok-2-image', key: xai.key };
  }
  return { chat, image };
}

// ── provider list with availability + reasons ──────────────────────────────────────────────────────────
// caps = { online:bool, webgpu:bool, modelCached:bool, modelOnSD:bool, runtimeOk:bool, lang:'it'|'en' }
// Returns ordered descriptors (online → local → preview):
//   { id, kind:'online'|'local'|'preview', brand, model, available, reason }
export function listProviders(image, caps = {}) {
  const lang = caps.lang === 'en' ? 'en' : 'it';
  const T = (it, en) => (lang === 'en' ? en : it);
  const online = caps.online !== false;     // default optimistic; UI refines with navigator.onLine
  const out = [];

  // 1) online cloud image
  if (image && image.key) {
    const ok = online;
    out.push({
      id: 'online', kind: 'online', brand: brandOf(image) || 'Online', model: image.model,
      available: ok,
      reason: ok ? T('online · nessun download', 'online · no download')
                 : T('offline — nessuna connessione', 'offline — no connection'),
    });
  } else {
    out.push({
      id: 'online', kind: 'online', brand: T('Online', 'Online'), model: null, available: false,
      reason: T('nessuna chiave immagini (configura xAI/Grok in Impostazioni · IA)',
                'no image key (set xAI/Grok in Settings · AI)'),
    });
  }

  // 2) local in-browser diffusion model
  const hasModel = !!(caps.modelCached || caps.modelOnSD);
  const localOk = hasModel && caps.runtimeOk !== false;
  out.push({
    id: 'local', kind: 'local', brand: 'SDXS', model: 'sdxs-512',
    available: localOk,
    reason: !caps.runtimeOk ? T('runtime ONNX non disponibile', 'ONNX runtime unavailable')
      : !hasModel ? T('modello non scaricato (apri ⚙ Modello)', 'model not downloaded (open ⚙ Model)')
      : caps.webgpu ? T('nel browser · WebGPU', 'in browser · WebGPU')
                    : T('nel browser · CPU (lento)', 'in browser · CPU (slow)'),
  });

  // 3) procedural preview (always there, honestly labelled as non-AI)
  out.push({
    id: 'preview', kind: 'preview', brand: T('Anteprima', 'Preview'), model: null, available: true,
    reason: T('procedurale · non IA · senza download', 'procedural · not AI · no download'),
  });
  return out;
}

// Best available provider id, honouring the requested cascade (online → local → preview). An explicit
// `preferred` id wins when it is currently available.
export function defaultProvider(list, preferred) {
  if (preferred) { const p = list.find((x) => x.id === preferred && x.available); if (p) return p.id; }
  const order = ['online', 'local', 'preview'];
  for (const id of order) { const p = list.find((x) => x.id === id && x.available); if (p) return p.id; }
  return 'preview';
}

// Short human label for the prompt-enhancer chip ("Migliora prompt: Claude"), or null when no LLM.
export function enhancerLabel(chat, lang = 'it') {
  if (!chat) return null;
  const b = brandOf(chat);
  return (lang === 'en' ? 'enhance with ' : 'migliora con ') + b;
}

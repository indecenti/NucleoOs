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
// The LLM that ENHANCES the prompt is not chosen here any more: Paint asks the OS-wide decision (/ai-ask.js), so
// a model on the user's PC rewrites prompts with no key, Private mode keeps it on this computer, and the cloud
// key is the one the shared router picks. (This file used to copy Games' chat resolver: a fixed Claude > Grok >
// Groq order and hard-coded models.) Private mode also takes the ONLINE image engine out: the prompt would
// leave the machine.
//
// The real brand is whatever teacher.json points at: Grok (xAI) ≠ Groq (Llama host) — never conflate.

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

// ── teacher.json → { image } ───────────────────────────────────────────────────────────────────────────
// image = best ONLINE image generator (xAI Grok, or any OpenAI-images-compatible endpoint via keys.image
//         / imageBase+imageModel) or null. Groq and Anthropic have NO image API → null unless an explicit
//         image endpoint is configured. Pure function of the parsed JSON (no I/O). The live model is re-picked
//         from the key's own list at generation time (ai.js resolveModel need:'image').
export function resolveConfigs(teacher) {
  const j = (teacher && typeof teacher === 'object') ? teacher : {};
  const keys = (j.keys && typeof j.keys === 'object') ? j.keys : {};
  const xai = (keys.xai && keys.xai.key) ? keys.xai : (keys.grok && keys.grok.key) ? keys.grok
    : (j.key && ((j.provider === 'xai' || j.provider === 'grok') || /x\.ai/i.test(j.base || '')) ? { base: j.base, model: j.model, key: j.key } : null);

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
  return { image };
}

// Every reason the provider list shows, in the five OS languages (an unknown language reads English).
const STR = {
  online_ok:    { it: 'online · nessun download', en: 'online · no download', es: 'en línea · sin descarga', fr: 'en ligne · aucun téléchargement', de: 'online · kein Download' },
  offline:      { it: 'offline — nessuna connessione', en: 'offline — no connection', es: 'sin conexión — no hay red', fr: 'hors ligne — aucune connexion', de: 'offline — keine Verbindung' },
  private:      { it: 'modalità Privato — niente esce da questo computer', en: 'Private mode — nothing leaves this computer', es: 'modo Privado — nada sale de este equipo', fr: 'mode Privé — rien ne quitte cet ordinateur', de: 'Privat-Modus — nichts verlässt diesen Computer' },
  no_image_key: { it: 'nessuna chiave immagini (configura xAI/Grok in Impostazioni · IA)', en: 'no image key (set xAI/Grok in Settings · AI)', es: 'sin clave de imágenes (configura xAI/Grok en Ajustes · IA)', fr: 'aucune clé d’images (configurez xAI/Grok dans Réglages · IA)', de: 'kein Bild-Schlüssel (xAI/Grok unter Einstellungen · KI einrichten)' },
  no_runtime:   { it: 'runtime ONNX non disponibile', en: 'ONNX runtime unavailable', es: 'runtime ONNX no disponible', fr: 'runtime ONNX indisponible', de: 'ONNX-Laufzeit nicht verfügbar' },
  no_model:     { it: 'modello non scaricato (apri ⚙ Modello)', en: 'model not downloaded (open ⚙ Model)', es: 'modelo no descargado (abre ⚙ Modelo)', fr: 'modèle non téléchargé (ouvrez ⚙ Modèle)', de: 'Modell nicht heruntergeladen (⚙ Modell öffnen)' },
  webgpu:       { it: 'nel browser · WebGPU', en: 'in browser · WebGPU', es: 'en el navegador · WebGPU', fr: 'dans le navigateur · WebGPU', de: 'im Browser · WebGPU' },
  cpu:          { it: 'nel browser · CPU (lento)', en: 'in browser · CPU (slow)', es: 'en el navegador · CPU (lento)', fr: 'dans le navigateur · CPU (lent)', de: 'im Browser · CPU (langsam)' },
  preview:      { it: 'Anteprima', en: 'Preview', es: 'Vista previa', fr: 'Aperçu', de: 'Vorschau' },
  preview_why:  { it: 'procedurale · non IA · senza download', en: 'procedural · not AI · no download', es: 'procedural · no es IA · sin descarga', fr: 'procédural · pas d’IA · aucun téléchargement', de: 'prozedural · keine KI · kein Download' },
  enhance_with: { it: 'migliora con ', en: 'enhance with ', es: 'mejorar con ', fr: 'améliorer avec ', de: 'verbessern mit ' },
};
const LANGS = ['it', 'en', 'es', 'fr', 'de'];
export const providerText = (key, lang) => STR[key][LANGS.includes(lang) ? lang : 'en'];

// ── provider list with availability + reasons ──────────────────────────────────────────────────────────
// caps = { online:bool, webgpu:bool, modelCached:bool, modelOnSD:bool, runtimeOk:bool, private:bool, lang:'it'|'en'|'es'|'fr'|'de' }
// Returns ordered descriptors (online → local → preview):
//   { id, kind:'online'|'local'|'preview', brand, model, available, reason }
export function listProviders(image, caps = {}) {
  const T = (k) => providerText(k, caps.lang);   // es/fr/de used to read Italian here: only it/en existed
  const online = caps.online !== false;     // default optimistic; UI refines with navigator.onLine
  const out = [];

  // 1) online cloud image — never in Private mode: the prompt would leave this computer
  if (image && image.key) {
    const ok = online && !caps.private;
    out.push({
      id: 'online', kind: 'online', brand: brandOf(image) || 'Online', model: image.model,
      available: ok,
      reason: caps.private ? T('private') : ok ? T('online_ok') : T('offline'),
    });
  } else {
    out.push({
      id: 'online', kind: 'online', brand: 'Online', model: null, available: false,
      reason: caps.private ? T('private') : T('no_image_key'),
    });
  }

  // 2) local in-browser diffusion model
  const hasModel = !!(caps.modelCached || caps.modelOnSD);
  const localOk = hasModel && caps.runtimeOk !== false;
  out.push({
    id: 'local', kind: 'local', brand: 'SDXS', model: 'sdxs-512',
    available: localOk,
    reason: !caps.runtimeOk ? T('no_runtime') : !hasModel ? T('no_model') : caps.webgpu ? T('webgpu') : T('cpu'),
  });

  // 3) procedural preview (always there, honestly labelled as non-AI)
  out.push({
    id: 'preview', kind: 'preview', brand: T('preview'), model: null, available: true,
    reason: T('preview_why'),
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

// Short human label for the prompt-enhancer chip ("enhance with Ollama", "migliora con Claude"), or null when no
// engine may answer. brand = who /ai-ask.js would ask first.
export function enhancerLabel(brand, lang = 'en') {
  if (!brand) return null;
  return providerText('enhance_with', lang) + brand;
}

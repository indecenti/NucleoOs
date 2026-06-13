// online-image.js — the ONLINE (cloud) image engine + the LLM prompt enhancer for Paint's Atelier.
// Pure & DOM-free: `fetchImpl`, `decode` and `ask` are INJECTED so the whole thing is host-testable and
// the same code runs in the browser. ISOLATED inside Paint (no ANIMA dependency).
//
// "tutto cliente": the browser calls the provider DIRECTLY (the PSRAM-less Cardputer never proxies the
// heavy bytes), and we request `response_format:"b64_json"` so the image comes back INLINE — decoded to a
// same-origin data: URL → canvas, with no cross-origin image fetch (which would taint the canvas / hit CORS).
//
// Image API = OpenAI-images-compatible `/images/generations` (xAI Grok `grok-2-image` is the configured
// one). grok-2-image accepts only { model, prompt, n, response_format } (NO size/quality/style/negative),
// so a negative prompt is folded into the prompt text and creative direction comes from the LLM enhancer.

// ── pure request/response helpers (asserted directly by the host tests) ─────────────────────────────────

// Fold a negative prompt into the positive one (the image API has no negative field).
export function composeImagePrompt(prompt, negative) {
  const p = String(prompt || '').trim();
  const n = String(negative || '').trim();
  if (!n) return p;
  return p ? `${p}. Avoid: ${n}` : `Avoid: ${n}`;   // never emit a stray leading ". "
}

// Build the exact HTTP request for an OpenAI-images-compatible endpoint. No I/O.
export function buildImageRequest(cfg, { prompt = '', negativePrompt = '', n = 1, responseFormat = 'b64_json', aspectRatio } = {}) {
  const base = (cfg.base || 'https://api.x.ai/v1').replace(/\/+$/, '');
  const body = {
    model: cfg.model || 'grok-2-image',
    prompt: composeImagePrompt(prompt, negativePrompt),
    n: Math.max(1, Math.min(10, n | 0 || 1)),
    response_format: responseFormat,
  };
  if (aspectRatio) body.aspect_ratio = aspectRatio;   // xAI extension; harmless if ignored
  return {
    url: base + '/images/generations',
    headers: { 'content-type': 'application/json', 'authorization': 'Bearer ' + (cfg.key || '') },
    body,
  };
}

// Sniff an image MIME from the first base64 bytes (jpeg/png/webp/gif) so the data: URL is correct.
export function mimeFromB64(b64) {
  const s = String(b64 || '');
  if (s.startsWith('/9j/')) return 'image/jpeg';
  if (s.startsWith('iVBOR')) return 'image/png';
  if (s.startsWith('UklGR')) return 'image/webp';
  if (s.startsWith('R0lGOD')) return 'image/gif';
  return 'image/jpeg';   // grok-2-image returns jpg
}

// Normalise the provider JSON → { ok, images:[{b64,mime,url,revisedPrompt}], error }.
export function parseImageResponse(json, httpStatus = 200) {
  if (!json || typeof json !== 'object') return { ok: false, error: 'risposta vuota', status: httpStatus };
  if (json.error) {
    const m = typeof json.error === 'string' ? json.error : (json.error.message || JSON.stringify(json.error));
    return { ok: false, error: m, status: httpStatus };
  }
  const data = Array.isArray(json.data) ? json.data : [];
  if (!data.length) return { ok: false, error: 'nessuna immagine nella risposta', status: httpStatus };
  const images = data.map((d) => {
    const b64 = d && (d.b64_json || d.b64 || null);
    return { b64, mime: b64 ? mimeFromB64(b64) : 'image/jpeg', url: (d && d.url) || null, revisedPrompt: (d && d.revised_prompt) || null };
  }).filter((x) => x.b64 || x.url);
  if (!images.length) return { ok: false, error: 'risposta senza dati immagine', status: httpStatus };
  return { ok: true, images, status: httpStatus };
}

// Map a failure into a short, honest Italian message the UI can show as-is.
export function explainImageError(status, msg) {
  if (status === 401 || status === 403) return 'chiave non valida o senza accesso alle immagini';
  if (status === 404) return 'modello/endpoint non trovato (controlla il modello immagini)';
  if (status === 429) return 'limite di richieste raggiunto — riprova tra poco';
  if (status === 0) return 'connessione bloccata (rete o CORS) — il provider potrebbe non permettere chiamate dal browser';
  if (status >= 500) return 'errore del provider (' + status + ')';
  return msg || ('HTTP ' + status);
}

// ── the online image engine (same interface as diffusion-engine.js engines) ─────────────────────────────
// { kind, isOnline, async load(), async generate({prompt,negativePrompt,seed,...}, onStep) -> {image,meta},
//   unload(), info() }
//   decode(dataUrl) -> Promise<{width,height,data:Uint8ClampedArray}>  (browser: <img>→canvas; tests: stub)
export function makeOnlineImageEngine({ cfg, fetchImpl, decode, label } = {}) {
  if (!cfg || !cfg.key) throw new Error('cfg.key required');
  if (typeof fetchImpl !== 'function') throw new Error('fetchImpl must be injected');
  if (typeof decode !== 'function') throw new Error('decode must be injected');
  const brand = label || 'online';
  let ready = false;

  async function callOnce(opts, onStep) {
    const req = buildImageRequest(cfg, opts);
    onStep && onStep({ step: 'request' });
    let resp, json;
    try {
      resp = await fetchImpl(req.url, { method: 'POST', headers: req.headers, body: JSON.stringify(req.body) });
    } catch (e) {
      // network / CORS / abort → status 0
      const err = new Error(explainImageError(0, e && e.message)); err.status = 0; throw err;
    }
    const status = resp.status == null ? 200 : resp.status;
    try { json = await resp.json(); } catch { json = null; }
    if (!resp.ok) {
      const detail = json && json.error && (json.error.message || json.error);
      const err = new Error(explainImageError(status, typeof detail === 'string' ? detail : null)); err.status = status; throw err;
    }
    const parsed = parseImageResponse(json, status);
    if (!parsed.ok) { const err = new Error(explainImageError(status, parsed.error)); err.status = status; throw err; }
    return parsed.images;
  }

  return {
    kind: 'online', isOnline: true, brand,
    async load() { ready = true; return { ok: true, ep: 'cloud', label: brand }; },
    // One image per call (uniform with the local engines). Variants are driven by the pipeline.
    async generate({ prompt = '', negativePrompt = '', seed = 0, n = 1, aspectRatio } = {}, onStep = () => {}) {
      if (!ready) throw new Error('engine not loaded');
      onStep({ step: 'tokenize' });
      const images = await callOnce({ prompt, negativePrompt, n: Math.max(1, n | 0 || 1), aspectRatio }, onStep);
      const first = images[0];
      onStep({ step: 'decode' });
      // "tutto cliente": we ask for inline b64 so the image is decoded on a same-origin canvas (no cross-origin
      // fetch, no taint, no MCU proxy). If a provider ignores response_format and returns only a URL, decline
      // honestly rather than handing a remote URL to a canvas (which would taint it / hang the decode).
      if (!first.b64) { const e = new Error('il provider ha restituito un URL invece dei byte inline (b64) — non supportato (servirebbe un proxy)'); e.status = 0; throw e; }
      const image = await decode(`data:${first.mime};base64,${first.b64}`);
      return { image, meta: { seed, ep: 'cloud', steps: 1, label: brand, online: true, revisedPrompt: first.revisedPrompt || null } };
    },
    unload() { ready = false; },
    info() { return { kind: 'online', isOnline: true, ready, model: cfg.model, brand }; },
  };
}

// ── pure chat request/response (the browser wires fetch to these; host tests pin both provider shapes) ──
// Anthropic and OpenAI-compatible (Grok/Groq) differ in endpoint, auth, system placement AND base convention
// (Anthropic base has no /v1, the others end in /v1) — so this asymmetry is captured in one tested place.
export function buildChatRequest(cfg, system, user, maxTokens = 320) {
  const base = (cfg.base || '').replace(/\/+$/, '');
  if (cfg.provider === 'anthropic') {
    return {
      url: base + '/v1/messages',
      headers: { 'content-type': 'application/json', 'x-api-key': cfg.key, 'anthropic-version': cfg.version || '2023-06-01', 'anthropic-dangerous-direct-browser-access': 'true' },
      body: { model: cfg.model, max_tokens: maxTokens, ...(system ? { system } : {}), messages: [{ role: 'user', content: user }] },
    };
  }
  const direct = base + '/chat/completions';
  return {
    // Gemini (cfg.proxy) has no CORS → relay the enhancer call through the device same-origin /api/llm proxy.
    url: cfg.proxy ? '/api/llm?url=' + encodeURIComponent(direct) : direct,
    headers: { 'content-type': 'application/json', 'authorization': 'Bearer ' + cfg.key },
    body: { model: cfg.model, max_tokens: maxTokens, messages: [...(system ? [{ role: 'system', content: system }] : []), { role: 'user', content: user }] },
  };
}
export function parseChatResponse(provider, json) {
  if (!json) return '';
  if (provider === 'anthropic') return Array.isArray(json.content) ? json.content.filter((b) => b && b.type === 'text').map((b) => b.text).join('') : '';
  return (json.choices && json.choices[0] && json.choices[0].message && json.choices[0].message.content) || '';
}

// ── LLM prompt enhancer — makes the pipeline LLM-driven end to end ──────────────────────────────────────
// An LLM rewrites the user's idea into ONE professional, concrete image prompt (and, when useful, a
// negative). `ask(system, user)` is injected (browser: a browser-direct chat call; tests: a stub). On any
// failure it returns the original prompt unchanged — enhancement is additive, it NEVER blocks generation.

const STYLE_HINT = {
  icon:        'a flat minimal vector ICON, single subject, centered, solid plain background, crisp edges',
  logo:        'a clean minimal LOGO mark, vector, balanced, plain background, memorable',
  background:  'a wide scenic BACKGROUND / wallpaper, depth, cohesive lighting, no central subject',
  photo:       'a PHOTOREALISTIC photograph, natural lighting, realistic detail, shallow depth of field',
  painting:    'a PAINTING, expressive brushwork, artistic composition, rich colour',
  illustration:'a polished ILLUSTRATION, clean line work, appealing colour palette',
  portrait:    'a PORTRAIT, flattering lighting, sharp focus on the face, pleasing bokeh',
  drawing:     'a hand DRAWING / sketch, confident lines, clear subject',
  image:       'a high-quality image, strong composition, good lighting',
};

export function buildEnhancePrompt(raw, { style = 'image', negative = '', lang = 'it' } = {}) {
  const hint = STYLE_HINT[style] || STYLE_HINT.image;
  const system =
    'You are an expert prompt engineer for text-to-image diffusion models. ' +
    'Rewrite the user\'s idea into ONE single vivid, concrete ENGLISH image prompt of 12-40 words. ' +
    'Make ' + hint + '. Add helpful detail about subject, composition, lighting, colour and style, but ' +
    'do NOT invent unrelated content and do NOT add people/text unless asked. ' +
    'Reply with ONLY the prompt — no quotes, no preamble, no explanation.';
  const langNote = lang === 'en' ? '' : ' (the idea may be in Italian; output the prompt in English)';
  const neg = negative ? `\nAvoid: ${negative}` : '';
  const user = `Idea: ${String(raw || '').trim()}${langNote}${neg}`;
  return { system, user };
}

// Trim an LLM reply to a single clean prompt line (strip quotes / "Prompt:" prefixes / trailing notes).
export function cleanEnhanced(text) {
  let s = String(text || '').trim();
  if (!s) return '';
  s = s.split(/\n+/).map((l) => l.trim()).filter(Boolean)[0] || '';   // first non-empty line
  s = s.replace(/^["'`]+|["'`]+$/g, '').trim();
  s = s.replace(/^(prompt|image prompt|output)\s*[:\-–]\s*/i, '').trim();
  return s;
}

export function makePromptEnhancer({ cfg, ask, label } = {}) {
  const brand = label || 'LLM';
  return {
    brand, available: !!(cfg && ask),
    async enhance(raw, opts = {}) {
      const original = String(raw || '').trim();
      if (!cfg || typeof ask !== 'function' || !original) return { prompt: original, negative: opts.negative || '', source: 'fallback', brand };
      try {
        const { system, user } = buildEnhancePrompt(original, opts);
        const out = await ask(system, user);
        const cleaned = cleanEnhanced(out);
        if (!cleaned || cleaned.length < 3) return { prompt: original, negative: opts.negative || '', source: 'fallback', brand };
        return { prompt: cleaned, negative: opts.negative || '', source: 'llm', brand };
      } catch {
        return { prompt: original, negative: opts.negative || '', source: 'fallback', brand };
      }
    },
  };
}

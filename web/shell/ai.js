// NucleoOS — shared online-AI client for the shell surfaces (onboarding + copilot).
//
// One device-held key vault (/data/anima/teacher.json, written via the paired /api/fs/*), and
// BROWSER-DIRECT calls so the PSRAM-less Cardputer is never loaded by the heavy TLS handshake or
// the token generation. Two wire formats: Anthropic (Claude — x-api-key + anthropic-version +
// /v1/messages + content[].text) and OpenAI-compatible (Groq/OpenAI — Bearer + /chat/completions).
// EXCEPTION — Google Gemini speaks the OpenAI-compatible wire BUT its API sends no CORS headers, so a
// browser fetch is blocked. Those (provider.proxy) are relayed through the device's same-origin
// /api/llm proxy: the firmware dials Google server-side (no CORS), passes the key through, never
// stores it, and streams the reply back heap-gated. See viaProxy().
//
// The key is the user's own, on their own single-tenant device, served to their own browser — so
// the Anthropic "dangerous-direct-browser-access" opt-in is appropriate here. The key is never
// logged and never sent anywhere except the provider the user chose.

export const AI_PATH = '/data/anima/teacher.json';

export const PROVIDERS = {
  anthropic: {
    label: 'Claude', base: 'https://api.anthropic.com', version: '2023-06-01',
    prefix: /^sk-ant-/, ph: 'sk-ant-…', def: 'claude-sonnet-4-6',
    // model tuples are [id, it-label, en-label] — render via modelLabel() so the picker follows the OS language.
    models: [['claude-sonnet-4-6', 'Sonnet 4.6 · equilibrio', 'Sonnet 4.6 · balanced'], ['claude-opus-4-8', 'Opus 4.8 · massima qualità', 'Opus 4.8 · top quality'], ['claude-haiku-4-5', 'Haiku 4.5 · veloce/economico', 'Haiku 4.5 · fast/cheap']],
  },
  openai: {
    label: 'Groq', base: 'https://api.groq.com/openai/v1', version: '',
    prefix: /^(gsk_|sk-)/, ph: 'gsk_…', def: 'llama-3.1-8b-instant',
    models: [['llama-3.1-8b-instant', 'Llama 3.1 8B · veloce', 'Llama 3.1 8B · fast'], ['llama-3.3-70b-versatile', 'Llama 3.3 70B · qualità', 'Llama 3.3 70B · quality']],
  },
  xai: {
    label: 'Grok (xAI)', base: 'https://api.x.ai/v1', version: '',
    prefix: /^xai-/, ph: 'xai-…', def: 'grok-2-latest',   // OpenAI-compatible wire (Bearer + /chat/completions)
    models: [['grok-2-latest', 'Grok 2 · latest', 'Grok 2 · latest'], ['grok-2-1212', 'Grok 2 (1212)', 'Grok 2 (1212)'], ['grok-beta', 'Grok beta', 'Grok beta']],
  },
  google: {
    label: 'Gemini', base: 'https://generativelanguage.googleapis.com/v1beta/openai', version: '',
    prefix: /^(AIza|AQ\.)/, ph: 'AIza… / AQ.…', def: 'gemini-2.5-flash', proxy: true,   // proxy: CORS-blocked browser-direct → relay via /api/llm; keys: classic AIza… or newer AQ.… tokens
    // STATIC FALLBACK — the live calibrateGemini() refines this to the key's REAL /models. IDs verified
    // against the API: gemini-3.5-flash / gemini-3.1-pro do NOT exist (404). Real lineup: gemini-2.5-flash
    // (recommended, stable, free), gemini-flash-latest (always-current Flash), gemini-2.5-pro (paid quality),
    // gemini-2.5-flash-lite (cheap, weak — fine for quick lookups, NOT for careful code).
    models: [['gemini-2.5-flash', 'Gemini 2.5 Flash · consigliato', 'Gemini 2.5 Flash · recommended'], ['gemini-flash-latest', 'Gemini Flash · ultimo', 'Gemini Flash · latest'], ['gemini-2.5-pro', 'Gemini 2.5 Pro · qualità (a pagamento)', 'Gemini 2.5 Pro · quality (paid)'], ['gemini-2.5-flash-lite', 'Gemini 2.5 Flash-Lite · economico', 'Gemini 2.5 Flash-Lite · cheap']],
  },
};

// Pick a model tuple's label for the active OS language. Tuples are [id, it, en]; the live Gemini
// calibration builds [id, id] pairs (no descriptor) — those fall back to the id. Reads anima.lang
// directly (no engine import) so ai.js stays usable from the Node host gates too.
export function modelLabel(entry) {
  if (!entry) return '';
  let lang = 'it';
  try { lang = (localStorage.getItem('anima.lang') || document.documentElement.lang || 'it').slice(0, 2); } catch {}
  return (lang === 'en' ? (entry[2] || entry[1]) : entry[1]) || entry[1] || entry[0];
}

// What each provider can actually DO. The one source of truth for "Claude can't draw/transcribe",
// consumed by Settings' preset engine + the which-apps panel so a feature gap is shown, not hit silently.
// image → /images/generations (only xAI grok-2-image); whisper → audio transcription (Groq/OpenAI only);
// ir → the IR app's NL skill (apps/ir-remote line 607 accepts only Groq/Gemini today).
// ir = the IR app's NL skill now works on EVERY chat provider (cloudToolCall below: Anthropic tool_use +
// OpenAI-compat tool_calls), so it is true across the board — kept as a field so the matrix stays the
// single place that answers "which provider can do what".
export const CAPMATRIX = {
  anthropic: { chat: true, image: false, whisper: false, toolUse: true, ir: true },
  openai:    { chat: true, image: false, whisper: true,  toolUse: true, ir: true },   // Groq
  xai:       { chat: true, image: true,  whisper: false, toolUse: true, ir: true },
  google:    { chat: true, image: false, whisper: false, toolUse: true, ir: true },
};
// Per-provider quality tiers, mapped to REAL ids from PROVIDERS.models (validated against the registry
// by the engine; google.max=Pro is offered only when geminiTier==='paid'). Single source so a preset
// can ask for "max"/"mid"/"fast" without re-hardcoding model strings.
export const TIERS = {
  anthropic: { max: 'claude-opus-4-8',          mid: 'claude-sonnet-4-6',    fast: 'claude-haiku-4-5' },
  openai:    { max: 'llama-3.3-70b-versatile',  mid: 'llama-3.1-8b-instant', fast: 'llama-3.1-8b-instant' },
  xai:       { max: 'grok-2-latest',            mid: 'grok-2-latest',        fast: 'grok-2-1212' },
  google:    { max: 'gemini-2.5-pro',           mid: 'gemini-2.5-flash',     fast: 'gemini-2.5-flash-lite' },
};

// ── multi-model router ────────────────────────────────────────────────────────────────────────────
// Leverage EVERY configured model: route a subtask to the best available across ALL keys in teacher.json's
// keys{} map. Coarse cross-provider hints (not a benchmark): strength drives 'hard' picks (deep reasoning /
// codegen), cost/speed drives 'fast' picks (planning, triage, file summaries). Capability needs (whisper,
// image) are answered by CAPMATRIX. Models come from TIERS so there is no second source of truth.
// Provider-level routing hints: `cost` orders 'fast' picks (cheapest/quickest first), `strength` orders
// 'mid'/'hard' picks (most capable first). The MODEL within the chosen provider comes from its difficulty
// tier (fast→TIERS.fast, mid→TIERS.mid, hard→TIERS.max), so there is one source of truth for model ids.
export const ROUTE_RANK = {
  anthropic: { cost: 2, strength: 10 },
  openai:    { cost: 1, strength: 7  },  // Groq Llama
  xai:       { cost: 3, strength: 7  },
  google:    { cost: 1, strength: 8  },
};
const TIER_OF = { fast: 'fast', mid: 'mid', hard: 'max' };

// Build a concrete call cfg {provider, base, model, key, version, exec} for a provider + a chosen model.
function cfgFor(provider, entry, model, exec) {
  const p = providerOf(provider);
  entry = entry || {};
  return { provider, base: entry.base || p.base, model: model || entry.model || p.def, key: entry.key || '', version: entry.version || p.version, exec: exec || entry.exec || 'browser' };
}

// Pick the best call cfg for a subtask across all configured keys.
//   spec   = { difficulty:'fast'|'mid'|'hard', capability?:'chat'|'whisper'|'image', exclude?:[provider] }
//   keys   = teacher.json keys{} map (provider -> {base,model,key,version}); only entries WITH a key count
//   active = the user's current cfg (readTeacher result); used for geminiTier + as the safe default
// Returns a cfg, or null when no configured key can serve the need (caller decides: degrade / decline honestly).
export function routeFor(spec = {}, keys = {}, active = null) {
  const difficulty = spec.difficulty === 'hard' ? 'hard' : spec.difficulty === 'mid' ? 'mid' : 'fast';
  const tier = TIER_OF[difficulty];
  const cap = spec.capability && spec.capability !== 'chat' ? spec.capability : null;
  const exclude = new Set(spec.exclude || []);
  const activeP = active && active.provider;
  keys = keys || {};
  const configured = Object.keys(keys).filter((p) => keys[p] && keys[p].key && PROVIDERS[p] && !exclude.has(p));
  // Rank a provider set: cheapest for 'fast', strongest for 'mid'/'hard'; tie → the user's active provider.
  const rank = (list) => list.map((p) => ({ p, ...(ROUTE_RANK[p] || ROUTE_RANK.anthropic) }))
    .sort((a, b) => { const d = difficulty === 'fast' ? (a.cost - b.cost) : (b.strength - a.strength); return d !== 0 ? d : (a.p === activeP ? -1 : b.p === activeP ? 1 : 0); });
  // Gemini Pro (the 'max' tier) is PAID-only; without a detected paid plan fall back to Flash. The tier is
  // read from the CHOSEN provider's OWN key entry (buildTeacherDoc persists it per-key so it survives a
  // provider switch), then from `active` only when that provider is the active one.
  const modelFor = (p) => {
    let m = (TIERS[p] && TIERS[p][tier]) || (keys[p] && keys[p].model);
    const paid = (keys.google && keys.google.geminiTier === 'paid') || (active && active.provider === 'google' && active.geminiTier === 'paid');
    if (p === 'google' && tier === 'max' && !paid) m = TIERS.google.mid;
    return m;
  };
  // CAPABILITY routing (whisper/image): only a provider whose CAPMATRIX has it — pick the BEST such one.
  if (cap) {
    const able = rank(configured.filter((p) => CAPMATRIX[p] && CAPMATRIX[p][cap]));
    if (!able.length) return null;                       // honest: no configured key can do it
    return cfgFor(able[0].p, keys[able[0].p], modelFor(able[0].p));
  }
  // No configured key (or all excluded) → the active default, but never an EXCLUDED provider (exclude contract).
  if (!configured.length) return (active && active.key && !exclude.has(active.provider)) ? Object.assign({ exec: 'browser' }, active) : null;
  const chosen = rank(configured)[0];
  return cfgFor(chosen.p, keys[chosen.p], modelFor(chosen.p));
}

export const providerOf = (p) => PROVIDERS[p] || PROVIDERS.anthropic;
// Gemini's endpoint has no CORS → a browser fetch is blocked. Relay it through the device same-origin
// /api/llm proxy (firmware dials it server-side). Other providers go browser-direct (no extra hop).
export const viaProxy = (provider, url) => (providerOf(provider).proxy ? '/api/llm?url=' + encodeURIComponent(url) : url);

// ── Gemini plan/tier calibration ────────────────────────────────────────────────────────────────────
// Google doesn't expose "your plan" via the API, but it's INFERABLE: Pro models (gemini-*-pro) are PAID-ONLY
// (since 2026-04 the free tier serves Flash/Flash-Lite only). So a Pro-model call that SUCCEEDS ⟹ billing is on
// ⟹ "paid/Pro"; a Pro-model call REFUSED (4xx: quota/permission/not-found) ⟹ free tier. We also LIST the key's
// real models so the dropdown reflects what it can actually use (no stale hardcoded guesses). All through the
// /api/llm proxy (Gemini has no CORS) and BEST-EFFORT: every step degrades to a safe default, never throws.
const GEMINI_BASE = PROVIDERS.google.base;
// Recommend a STRONG flash (never -lite): 2.5-flash (stable, = the registry default) → flash-latest (current) → 3-flash → any non-lite flash.
const pickFlash = (ids) => ids.find((m) => /gemini-2\.5-flash$/.test(m)) || ids.find((m) => /gemini-flash-latest$/.test(m))
  || ids.find((m) => /gemini-3-flash/.test(m)) || ids.find((m) => /flash$/.test(m) && !/lite$/.test(m))
  || ids.find((m) => /flash/.test(m) && !/lite/.test(m)) || ids[0] || null;
const pickPro = (ids) => ids.find((m) => /gemini-2\.5-pro$/.test(m)) || ids.find((m) => /gemini-3\.1-pro$/.test(m))
  || ids.find((m) => /pro-latest$/.test(m)) || ids.find((m) => /-pro$/.test(m)) || null;

// GET the key's available models through the proxy → bare id list (['gemini-3.5-flash', …]) or null on failure.
export async function geminiListModels(cfg) {
  try {
    const url = '/api/llm?url=' + encodeURIComponent((cfg.base || GEMINI_BASE).replace(/\/+$/, '') + '/models');
    const r = await fetch(url, { headers: { authorization: 'Bearer ' + (cfg.key || '') }, cache: 'no-store' });
    if (!r.ok) return null;
    const j = await r.json().catch(() => null);
    const data = (j && (j.data || j.models)) || null;        // OpenAI-compat {data:[{id}]} | native {models:[{name}]}
    if (!Array.isArray(data)) return null;
    const ids = [...new Set(data.map((m) => String((m && (m.id || m.name)) || '').replace(/^models\//, ''))
      .filter((id) => /^gemini/i.test(id) && !/embedding|image|tts|aqa|live/i.test(id)))];   // chat models only
    return ids.length ? ids : null;
  } catch { return null; }
}

// One tiny call to a Pro model → tier. 200 ⟹ 'paid'; 4xx (quota/permission/not-found) ⟹ 'free'; else 'unknown'.
export async function geminiProbeTier(cfg, proModel = 'gemini-2.5-pro') {   // a REAL Pro model (gemini-3.1-pro 404s → would always read "free")
  try {
    const url = '/api/llm?url=' + encodeURIComponent((cfg.base || GEMINI_BASE).replace(/\/+$/, '') + '/chat/completions');
    const r = await fetch(url, { method: 'POST',
      headers: { 'content-type': 'application/json', authorization: 'Bearer ' + (cfg.key || '') },
      body: JSON.stringify({ model: proModel, max_tokens: 1, messages: [{ role: 'user', content: 'hi' }] }) });
    if (r.ok) return 'paid';
    if (r.status >= 400 && r.status < 500) return 'free';    // Pro refused → no billing → free tier
    return 'unknown';                                        // 5xx / proxy-busy → can't tell
  } catch { return 'unknown'; }
}

// Best-effort calibration: the key's real models + billing tier + a tier-appropriate recommended model. Probes
// SEQUENTIALLY (one device TLS at a time, RAM-friendly). Never throws; falls back to the static registry list.
export async function calibrateGemini(cfg) {
  const fallback = PROVIDERS.google.models.map((m) => m[0]);
  const live = await geminiListModels(cfg);
  const models = live || fallback;
  const pro = pickPro(models);
  const flash = pickFlash(models) || models[0];
  let tier;
  if (pro) tier = await geminiProbeTier(cfg, pro);           // can it actually USE a Pro model? 200=paid, 4xx=free
  else if (live) tier = 'free';                              // listed real models, none Pro → free-tier key
  else tier = await geminiProbeTier(cfg, pickPro(fallback) || 'gemini-2.5-pro');   // no list → probe a real Pro model
  const proPick = pro || pickPro(fallback);
  const recommended = (tier === 'paid' && proPick) ? proPick : flash;   // paid → best Pro; free/unknown → free-safe Flash
  return { tier, models, recommended, hasPro: tier === 'paid' };
}

// Honest one-line tier label for the UI (bilingual).
export function geminiTierLabel(tier, en) {
  if (tier === 'paid') return en ? 'Paid plan · Pro models available' : 'Piano a pagamento · modelli Pro disponibili';
  if (tier === 'free') return en ? 'Free tier · Flash only (Pro needs billing)' : 'Free tier · solo Flash (Pro richiede billing)';
  return en ? 'Plan not detected' : 'Piano non rilevato';
}
export const maskKey = (k) => (k && k.length > 10 ? k.slice(0, 6) + '…' + k.slice(-4) : (k ? '…' : ''));

// Lightweight capability probe — preferred over readTeacher() to answer "is AI configured?" because
// it never returns the raw key. {hasKey, online, enabled, provider?, model?} or null on error.
export async function caps() {
  try { const r = await fetch('/api/anima/caps', { cache: 'no-store' }); return r.ok ? await r.json() : null; }
  catch { return null; }
}

// Short-TTL memo for teacher.json: it's read by ~10 modules (settings reads it up to 3x on open,
// chat/spreadsheet/ir-remote/games/paint/agent/dictation on first action) and changes only when the
// user edits a key. A 30 s cache collapses the open-burst of /api/fs/read into one device read;
// writeTeacher() and a fs.changed on the path invalidate it immediately, so a key change is never stale.
let _teacherCache = null, _teacherAt = 0;
const TEACHER_TTL = 30000;
export function invalidateTeacher() { _teacherCache = null; _teacherAt = 0; }

// Read the active teacher config (paired). Returns a normalized cfg, {unpaired:true}, or null.
export async function readTeacher(opts = {}) {
  if (!opts.fresh && _teacherCache && (Date.now() - _teacherAt) < TEACHER_TTL) return _teacherCache;
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(AI_PATH), { cache: 'no-store' });
    if (r.status === 401 || r.status === 403) return { unpaired: true };   // transient — not cached
    if (!r.ok) return null;
    const j = JSON.parse(await r.text()) || {};
    const provider = j.provider || (j.base && /anthropic/.test(j.base) ? 'anthropic' : (j.base && /generativelanguage/.test(j.base) ? 'google' : (j.base && /x\.ai/.test(j.base) ? 'xai' : (j.key ? 'openai' : 'anthropic'))));
    const p = providerOf(provider);
    const cfg = { provider, base: j.base || p.base, model: j.model || p.def, key: j.key || '', version: j.version || p.version, exec: j.exec || 'browser', keys: j.keys || {}, geminiTier: j.geminiTier || '' };
    _teacherCache = cfg; _teacherAt = Date.now();
    return cfg;
  } catch { return null; }
}

// Canonical teacher.json: the active provider at top-level (what the firmware reads) + a keys map
// so switching providers doesn't lose the other key.
export function buildTeacherDoc(cfg) {
  const p = providerOf(cfg.provider);
  // The detected Gemini plan lives BOTH on the per-key entry (so the multi-model router can read it after the
  // user switches the active provider away from Google) AND at the top level (what the firmware reads today).
  const entry = Object.assign({ base: cfg.base || p.base, model: cfg.model || p.def, key: cfg.key || '' },
    cfg.provider === 'anthropic' ? { version: cfg.version || p.version } : {},
    (cfg.provider === 'google' && cfg.geminiTier) ? { geminiTier: cfg.geminiTier } : {});
  const keys = Object.assign({}, cfg.keys || {});
  if (entry.key) keys[cfg.provider] = entry; else delete keys[cfg.provider];
  const extra = (cfg.provider === 'google' && cfg.geminiTier) ? { geminiTier: cfg.geminiTier } : {};   // top-level mirror for the firmware
  return Object.assign({ provider: cfg.provider, exec: cfg.exec || 'browser' }, entry, extra, { keys });
}

// Write the vault (paired). true | 'unpaired' | false.
export async function writeTeacher(cfg) {
  try {
    const r = await fetch('/api/fs/write?path=' + encodeURIComponent(AI_PATH), { method: 'POST', body: JSON.stringify(buildTeacherDoc(cfg)) });
    if (r.ok) invalidateTeacher();   // our own write changed it — drop the memo so the next read is fresh
    return r.ok ? true : ((r.status === 401 || r.status === 403) ? 'unpaired' : false);
  } catch { return false; }
}

// Browser-direct completion. Returns the assistant text; throws Error on failure.
// opts.signal — an AbortSignal so a user-facing Stop can cancel a hung cloud call (copilot.js).
export async function cloudComplete(cfg, system, user, maxTokens, opts = {}) {
  if (cfg.provider === 'anthropic') {
    const resp = await fetch((cfg.base || PROVIDERS.anthropic.base).replace(/\/+$/, '') + '/v1/messages', {
      method: 'POST', signal: opts.signal,
      headers: { 'content-type': 'application/json', 'x-api-key': cfg.key, 'anthropic-version': cfg.version || '2023-06-01', 'anthropic-dangerous-direct-browser-access': 'true' },
      body: JSON.stringify({ model: cfg.model || PROVIDERS.anthropic.def, max_tokens: maxTokens || 1024, ...(system ? { system } : {}), messages: [{ role: 'user', content: user }] }),
    });
    const j = await resp.json().catch(() => null);
    if (!resp.ok || !j || j.type === 'error') throw new Error((j && j.error && j.error.message) || ('HTTP ' + resp.status));
    return Array.isArray(j.content) ? j.content.filter((b) => b && b.type === 'text').map((b) => b.text).join('') : '';
  }
  const epurl = (cfg.base || PROVIDERS.openai.base).replace(/\/+$/, '') + '/chat/completions';
  const resp = await fetch(viaProxy(cfg.provider, epurl), {
    method: 'POST', signal: opts.signal,
    headers: { 'content-type': 'application/json', 'authorization': 'Bearer ' + cfg.key },
    body: JSON.stringify({ model: cfg.model || PROVIDERS.openai.def, temperature: 0.4, messages: [...(system ? [{ role: 'system', content: system }] : []), { role: 'user', content: user }] }),
  });
  const j = await resp.json().catch(() => null);
  if (!resp.ok || !j || j.error) throw new Error((j && j.error && (j.error.message || j.error)) || ('HTTP ' + resp.status));
  return (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';
}

// Provider-aware single-shot TOOL selection. `tools` is an OpenAI-style tool list (anima-skill's
// actionsToOpenAITools); `messages` is an OpenAI-style [{role,content}] history+turn. Returns
// {tool, args} | {text} | null; throws on a definite HTTP/network failure (callers try/catch → miss).
// Anthropic uses native tool_use; Groq/xAI go browser-direct; Gemini relays via the device /api/llm
// proxy. This is the ONE place a NucleoOS app needs to call to get tool-calling on EVERY provider —
// it replaces each app re-implementing the per-provider wire (the fragmentation we're undoing).
export async function cloudToolCall(cfg, { system, messages = [], tools = [], signal, maxTokens = 512, temperature = 0.2, responseFormat = null } = {}) {
  if (cfg.provider === 'anthropic') {
    const atools = (tools || []).map((t) => ({ name: t.function.name, description: t.function.description, input_schema: t.function.parameters || { type: 'object', properties: {} } }));
    const resp = await fetch((cfg.base || PROVIDERS.anthropic.base).replace(/\/+$/, '') + '/v1/messages', {
      method: 'POST', signal,
      headers: { 'content-type': 'application/json', 'x-api-key': cfg.key, 'anthropic-version': cfg.version || '2023-06-01', 'anthropic-dangerous-direct-browser-access': 'true' },
      body: JSON.stringify({ model: cfg.model || PROVIDERS.anthropic.def, max_tokens: maxTokens, ...(system ? { system } : {}), ...(atools.length ? { tools: atools } : {}), messages }),
    });
    const j = await resp.json().catch(() => null);
    if (!resp.ok || !j || j.type === 'error') throw new Error((j && j.error && j.error.message) || ('HTTP ' + resp.status));
    const blocks = Array.isArray(j.content) ? j.content : [];
    const tu = blocks.find((b) => b && b.type === 'tool_use');
    if (tu) return { tool: tu.name, args: tu.input || {} };
    const text = blocks.filter((b) => b && b.type === 'text').map((b) => b.text).join('');
    return text ? { text } : null;
  }
  const epurl = (cfg.base || PROVIDERS.openai.base).replace(/\/+$/, '') + '/chat/completions';
  const resp = await fetch(viaProxy(cfg.provider, epurl), {
    method: 'POST', signal,
    headers: { 'content-type': 'application/json', authorization: 'Bearer ' + cfg.key },
    body: JSON.stringify({ model: cfg.model || PROVIDERS.openai.def, max_tokens: maxTokens, temperature, messages: [...(system ? [{ role: 'system', content: system }] : []), ...messages], ...(tools.length ? { tools, tool_choice: 'auto' } : {}), ...(responseFormat ? { response_format: responseFormat } : {}) }),
  });
  const j = await resp.json().catch(() => null);
  if (!resp.ok || !j || j.error) throw new Error((j && j.error && (j.error.message || j.error)) || ('HTTP ' + resp.status));
  const msg = j.choices && j.choices[0] && j.choices[0].message;
  if (!msg) return null;
  const tc = msg.tool_calls && msg.tool_calls[0];
  if (tc && tc.function) { let a = {}; try { a = JSON.parse(tc.function.arguments || '{}'); } catch {} return { tool: tc.function.name, args: a }; }
  return msg.content ? { text: msg.content } : null;
}

// Verify a key works (a tiny browser-direct call). true/false; throws on a definite rejection.
export async function cloudPing(cfg) {
  const t = await cloudComplete(cfg, null, 'Reply with exactly: ok', 16);
  return /\bok\b/i.test(t || '');
}

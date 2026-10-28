/* SPDX-License-Identifier: AGPL-3.0-or-later */
// NucleoOS — shared online-AI client for the shell surfaces (onboarding + copilot).
//
// One device-held key vault (/data/anima/teacher.json, written via the paired /api/fs/*), and
// BROWSER-DIRECT calls so the PSRAM-less Cardputer is never loaded by the heavy TLS handshake or
// the token generation. Two wire formats: Anthropic (Claude — x-api-key + anthropic-version +
// /v1/messages + content[].text) and OpenAI-compatible (Groq/OpenAI — Bearer + /chat/completions).
// EXCEPTION — Google Gemini speaks the OpenAI-compatible wire BUT its API sends no CORS headers, so a
// browser fetch is blocked. Those (provider.proxy) are relayed through the device's same-origin
// /api/llm proxy: the firmware dials Google server-side (no CORS), passes the key through, never
// stores it, and streams the reply back heap-gated. See viaProxy() / apiUrl().
//
// SEALED VAULT (a firmware that answers /api/anima/teacher — the ESP32-P4 today): the keys never leave
// the device. That endpoint blanks every key, readTeacher() puts VAULT_KEY where a saved key is (so
// "is a key set?" checks keep working), and EVERY model call then goes through the device relay
// /api/llm, which adds the real key on the way out — only towards the host it was saved for. A key the
// user is typing (test before save, a model list) is the one key a page sends itself. Writes go to
// /api/anima/teacher too and are MERGED on the device: a blank key keeps the saved one, so a page can
// never wipe a key it was not shown. A firmware without that endpoint (404) keeps the classic vault.
//
// The key is the user's own, on their own single-tenant device, served to their own browser — so
// the Anthropic "dangerous-direct-browser-access" opt-in is appropriate here. The key is never
// logged and never sent anywhere except the provider the user chose.

export const AI_PATH = '/data/anima/teacher.json';
const TEACHER_API = '/api/anima/teacher';
// Where a saved key would be: never a secret, never sent (authHeaders), written back as "" (kept).
export const VAULT_KEY = '\u2022device-key\u2022';
export const isVaultKey = (k) => k === VAULT_KEY;
const realKey = (k) => (k && k !== VAULT_KEY ? k : '');

export const PROVIDERS = {
  anthropic: {
    label: 'Claude', base: 'https://api.anthropic.com', version: '2023-06-01',
    prefix: /^sk-ant-/, ph: 'sk-ant-…', def: 'claude-sonnet-5-5',
    // model tuples are [id, it, en, es, fr, de] — render via modelLabel() so the picker follows the OS language.
    models: [['claude-sonnet-5-5', 'Sonnet 5.5 · equilibrio', 'Sonnet 5.5 · balanced', 'Sonnet 5.5 · equilibrado', 'Sonnet 5.5 · équilibré', 'Sonnet 5.5 · ausgewogen'], ['claude-opus-5-5', 'Opus 5.5 · massima qualità', 'Opus 5.5 · top quality', 'Opus 5.5 · máxima calidad', 'Opus 5.5 · qualité maximale', 'Opus 5.5 · höchste Qualität'], ['claude-haiku-4-5', 'Haiku 4.5 · veloce/economico', 'Haiku 4.5 · fast/cheap', 'Haiku 4.5 · rápido/económico', 'Haiku 4.5 · rapide/économique', 'Haiku 4.5 · schnell/günstig']],
  },
  openai: {
    label: 'Groq', base: 'https://api.groq.com/openai/v1', version: '',
    prefix: /^(gsk_|sk-)/, ph: 'gsk_…', def: 'llama-3.1-8b-instant',
    models: [['llama-3.1-8b-instant', 'Llama 3.1 8B · veloce', 'Llama 3.1 8B · fast', 'Llama 3.1 8B · rápido', 'Llama 3.1 8B · rapide', 'Llama 3.1 8B · schnell'], ['llama-3.3-70b-versatile', 'Llama 3.3 70B · qualità', 'Llama 3.3 70B · quality', 'Llama 3.3 70B · calidad', 'Llama 3.3 70B · qualité', 'Llama 3.3 70B · Qualität']],
  },
  xai: {
    label: 'Grok (xAI)', base: 'https://api.x.ai/v1', version: '',
    prefix: /^xai-/, ph: 'xai-…', def: 'grok-2-latest',   // OpenAI-compatible wire (Bearer + /chat/completions)
    models: [['grok-2-latest', 'Grok 2 · latest', 'Grok 2 · latest', 'Grok 2 · latest', 'Grok 2 · latest', 'Grok 2 · latest'], ['grok-2-1212', 'Grok 2 (1212)', 'Grok 2 (1212)', 'Grok 2 (1212)', 'Grok 2 (1212)', 'Grok 2 (1212)'], ['grok-beta', 'Grok beta', 'Grok beta', 'Grok beta', 'Grok beta', 'Grok beta']],
  },
  google: {
    label: 'Gemini', base: 'https://generativelanguage.googleapis.com/v1beta/openai', version: '',
    prefix: /^(AIza|AQ\.)/, ph: 'AIza… / AQ.…', def: 'gemini-2.5-flash', proxy: true,   // proxy: CORS-blocked browser-direct → relay via /api/llm; keys: classic AIza… or newer AQ.… tokens
    // STATIC FALLBACK — the live calibrateGemini() refines this to the key's REAL /models. IDs verified
    // against the API: gemini-3.5-flash / gemini-3.1-pro do NOT exist (404). Real lineup: gemini-2.5-flash
    // (recommended, stable, free), gemini-flash-latest (always-current Flash), gemini-2.5-pro (paid quality),
    // gemini-2.5-flash-lite (cheap, weak — fine for quick lookups, NOT for careful code).
    models: [['gemini-2.5-flash', 'Gemini 2.5 Flash · consigliato', 'Gemini 2.5 Flash · recommended', 'Gemini 2.5 Flash · recomendado', 'Gemini 2.5 Flash · recommandé', 'Gemini 2.5 Flash · empfohlen'], ['gemini-flash-latest', 'Gemini Flash · ultimo', 'Gemini Flash · latest', 'Gemini Flash · último', 'Gemini Flash · dernier', 'Gemini Flash · neueste'], ['gemini-2.5-pro', 'Gemini 2.5 Pro · qualità (a pagamento)', 'Gemini 2.5 Pro · quality (paid)', 'Gemini 2.5 Pro · calidad (de pago)', 'Gemini 2.5 Pro · qualité (payant)', 'Gemini 2.5 Pro · Qualität (kostenpflichtig)'], ['gemini-2.5-flash-lite', 'Gemini 2.5 Flash-Lite · economico', 'Gemini 2.5 Flash-Lite · cheap', 'Gemini 2.5 Flash-Lite · económico', 'Gemini 2.5 Flash-Lite · économique', 'Gemini 2.5 Flash-Lite · günstig']],
  },
};
// A model server on the LAN: Ollama (:11434), LM Studio (:1234), llama.cpp / LocalAI (:8080), any
// OpenAI-compatible endpoint. No CORS and plain HTTP, so the browser reaches it through the device
// relay (/api/llm); the key is optional and the models are whatever the server lists (listModels).
// Only a relay that admits plain-http LAN hosts can reach it (lanRelay(): the sealed-vault firmware); the
// Cardputer relay is https-only and the Handheld one Google-only. So it is NON-ENUMERABLE: every lookup
// (providerOf, readTeacher, routeFor) knows it on every edition, but code that LISTS the providers (pickers,
// Settings' preset engine) does not see it — ai-keys.js adds its chip only where lanRelay() is true.
Object.defineProperty(PROVIDERS, 'local', { enumerable: false, value: {
  label: 'Local server', base: 'http://192.168.1.10:11434/v1', version: '', local: true, keyOptional: true,
  prefix: /.*/, ph: '(optional)', def: '', proxy: true, models: [],
  presets: [['Ollama', 11434], ['LM Studio', 1234], ['llama.cpp', 8080]],
} });

// Pick a model tuple's label for the active OS language. Tuples are [id, it, en, es, fr, de]; the live
// Gemini calibration builds [id, id] pairs (no descriptor) — those fall back to the id. Reads anima.lang
// directly (no engine import) so ai.js stays usable from the Node host gates too.
export function modelLabel(entry) {
  if (!entry) return '';
  let lang = 'en';
  try { lang = (localStorage.getItem('anima.lang') || document.documentElement.lang || 'en').slice(0, 2); } catch {}
  const i = { it: 1, en: 2, es: 3, fr: 4, de: 5 }[lang] || 2;
  return entry[i] || entry[2] || entry[1] || entry[0];   // active language → en → it → id
}

// What each provider can actually DO. The one source of truth for "Claude can't draw/transcribe",
// consumed by Settings' preset engine + the which-apps panel so a feature gap is shown, not hit silently.
// image → /images/generations (only xAI grok-2-image); whisper → audio transcription (Groq/OpenAI only);
// ir → the IR app's NL skill (apps/ir-remote line 607 accepts only Groq/Gemini today).
// ir = the IR app's NL skill now works on EVERY chat provider (cloudToolCall below: Anthropic tool_use +
// OpenAI-compat tool_calls), so it is true across the board — kept as a field so the matrix stays the
// single place that answers "which provider can do what".
// audioLLM = the model takes audio DIRECTLY in a chat request (Gemini's inline audio), reaching the
// same goal as Whisper by another road — and in one call instead of two, which on a chip with no
// PSRAM is a RAM win as much as a latency one. `whisper` stays specifically "has /audio/transcriptions".
export const CAPMATRIX = {
  anthropic: { chat: true, image: false, whisper: false, audioLLM: false, toolUse: true, ir: true },
  openai:    { chat: true, image: false, whisper: true,  audioLLM: false, toolUse: true, ir: true },   // Groq
  xai:       { chat: true, image: true,  whisper: false, audioLLM: false, toolUse: true, ir: true },
  google:    { chat: true, image: false, whisper: false, audioLLM: true,  toolUse: true, ir: true },
  local:     { chat: true, image: false, whisper: false, audioLLM: false, toolUse: true, ir: true },
};
// Per-provider quality tiers, mapped to REAL ids from PROVIDERS.models (validated against the registry
// by the engine; google.max=Pro is offered only when geminiTier==='paid'). Single source so a preset
// can ask for "max"/"mid"/"fast" without re-hardcoding model strings.
export const TIERS = {
  anthropic: { max: 'claude-opus-5-5',          mid: 'claude-sonnet-5-5',    fast: 'claude-haiku-4-5' },
  openai:    { max: 'llama-3.3-70b-versatile',  mid: 'llama-3.1-8b-instant', fast: 'llama-3.1-8b-instant' },
  xai:       { max: 'grok-2-latest',            mid: 'grok-2-latest',        fast: 'grok-2-1212' },
  google:    { max: 'gemini-2.5-pro',           mid: 'gemini-2.5-flash',     fast: 'gemini-2.5-flash-lite' },
};
// (local: no tiers - the server runs the one model the user picked)

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
  local:     { cost: 0, strength: 5  },   // free and private, usually a smaller model
};
const TIER_OF = { fast: 'fast', mid: 'mid', hard: 'max' };

// Build a concrete call cfg {provider, base, model, key, version, exec} for a provider + a chosen model.
function cfgFor(provider, entry, model, exec, tier) {
  const p = providerOf(provider);
  entry = entry || {};
  // `tier` travels with the cfg: the static TIERS id is only a first guess, and withAutoModel re-picks at call
  // time for the same tier when the provider no longer serves it.
  return { provider, base: entry.base || p.base, model: model || entry.model || p.def, key: entry.key || '', version: entry.version || p.version, exec: exec || entry.exec || 'browser', tier: tier || 'mid' };
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
  const usable = (p) => keys[p] && (keys[p].key || (PROVIDERS[p] && PROVIDERS[p].keyOptional && keys[p].base && keys[p].model));
  const configured = Object.keys(keys).filter((p) => usable(p) && PROVIDERS[p] && !exclude.has(p));
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
    if (able.length) return cfgFor(able[0].p, keys[able[0].p], modelFor(able[0].p), undefined, tier);
    // 'whisper' names a transport, not the goal. The goal is audio -> text, and a provider whose
    // model eats audio directly gets there too. Answering with that beats declining when the user
    // has a perfectly capable key configured.
    if (cap === 'whisper') {
      const alt = rank(configured.filter((p) => CAPMATRIX[p] && CAPMATRIX[p].audioLLM));
      if (alt.length) return Object.assign(cfgFor(alt[0].p, keys[alt[0].p], modelFor(alt[0].p), undefined, tier), { audioLLM: true });
    }
    return null;                                         // honest: no configured key can do it
  }
  // No configured key (or all excluded) → the active default, but never an EXCLUDED provider (exclude contract).
  if (!configured.length) return (active && active.key && !exclude.has(active.provider)) ? Object.assign({ exec: 'browser' }, active) : null;
  const chosen = rank(configured)[0];
  return cfgFor(chosen.p, keys[chosen.p], modelFor(chosen.p), undefined, tier);
}

export const providerOf = (p) => PROVIDERS[p] || PROVIDERS.anthropic;

// Speech -> text (Whisper) endpoint for an OpenAI-compatible base. Groq serves it ONLY under /openai/v1 —
// https://api.groq.com/v1/audio/transcriptions answers 404 (checked 2026-10-11) — yet Dictation and the agent's
// transcribe tool rewrote the base to exactly that route. One builder, so the callers cannot drift again.
export function whisperUrl(base) {
  let b = String(base || '').trim().replace(/\/+$/, '');
  try { if (!b || new URL(b).hostname === 'api.groq.com') b = PROVIDERS.openai.base; } catch { b = PROVIDERS.openai.base; }
  return b + '/audio/transcriptions';
}
// Set by readTeacher() when the device answered /api/anima/teacher (the SEALED vault — see the header): the page
// then holds no key and every provider call must go through the device relay, which adds it. Elsewhere it stays
// false and only the CORS-less proxy providers are relayed.
let _sealed = false;
export const sealedVault = () => _sealed;
// Can the device relay reach a plain-http server on the LAN (the 'local' provider)? Only the sealed-vault
// firmware's relay does (it mirrors its url_is_local); the Cardputer relay is https-only, the Handheld's Google-only.
export const lanRelay = () => _sealed;
// Gemini's endpoint has no CORS → a browser fetch is blocked. Relay it through the device same-origin
// /api/llm proxy (firmware dials it server-side). Other providers go browser-direct (no extra hop) — except
// on a sealed vault, where the page has no key to send and everything rides the relay.
export const viaProxy = (provider, url) => ((_sealed || providerOf(provider).proxy) ? '/api/llm?url=' + encodeURIComponent(url) : url);
// A private / loopback / .local host — a server on the LAN (mirrors the firmware's url_is_local).
export const isLanUrl = (u) => {
  const m = /^https?:\/\/([^/:]+)/i.exec(String(u || '')); if (!m) return false;
  const h = m[1].toLowerCase();
  if (h === 'localhost' || h.endsWith('.local')) return true;
  const o = h.split('.').map(Number);
  return o.length === 4 && o.every((x) => x >= 0 && x <= 255) &&
    (o[0] === 10 || o[0] === 127 || (o[0] === 192 && o[1] === 168) || (o[0] === 172 && o[1] >= 16 && o[1] <= 31) || (o[0] === 169 && o[1] === 254));
};

// ── the ONE cloud wire ──────────────────────────────────────────────────────────────────────────────
// Every request to a provider is addressed by apiUrl() and authenticated by authHeaders() — in this file and
// in the apps that import them — so "where may a key go" is decided in exactly one place:
//   · to cfg.base, the endpoint the user configured for THAT key;
//   · with no base, to the base of cfg.provider's OWN registry entry — never another provider's (the old
//     fallback sent any non-Anthropic key without a base to Groq's host, Gemini keys included);
//   · to the same-origin /api/llm relay for a proxy provider (Gemini: no CORS), an explicit cfg.proxy (a custom
//     non-CORS endpoint), or a sealed vault (the relay adds the saved key).
// Nothing here logs a key, and the key never rides in a URL (header only).
const isAnth = (cfg) => !!cfg && cfg.provider === 'anthropic';
const regOf = (cfg) => (isAnth(cfg) ? PROVIDERS.anthropic : (PROVIDERS[cfg && cfg.provider] || PROVIDERS.openai));

// Endpoint for `path` ('/v1/messages', '/chat/completions', '/models'…) on this cfg, relayed when it must be.
export function apiUrl(cfg, path) {
  const url = String(cfg.base || regOf(cfg).base || '').replace(/\/+$/, '') + path;
  return (_sealed || isVaultKey(cfg.key) || cfg.proxy || regOf(cfg).proxy) ? '/api/llm?url=' + encodeURIComponent(url) : url;
}

// Auth + content headers for this cfg's wire. Only a key the user is typing goes out from the page; a saved one
// on a sealed vault (VAULT_KEY) is added by the relay, so no key header is sent for it (nor for a keyless LAN
// server). Anthropic needs the dangerous-direct-browser-access opt-in for a browser-direct call (see the header).
export function authHeaders(cfg) {
  const k = realKey(cfg && cfg.key);
  if (isAnth(cfg)) return Object.assign({ 'content-type': 'application/json', 'anthropic-version': cfg.version || PROVIDERS.anthropic.version },
    k ? { 'x-api-key': k, 'anthropic-dangerous-direct-browser-access': 'true' } : {});
  return Object.assign({ 'content-type': 'application/json' }, k ? { authorization: 'Bearer ' + k } : {});
}

// The real brand behind a cfg, from the provider + endpoint host, so a label never lies: the firmware
// nicknames the cloud teacher "Grok" everywhere, but Grok (xAI) and Groq (Llama host) are different
// companies. Shared by the apps that show which brain actually answered.
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

// ── AUTOMATIC MODEL SELECTION ───────────────────────────────────────────────────────────────────────
// Providers retire model ids on their own schedule — Groq shut down groq/compound on 2026-09-21 and
// llama-3.1-8b-instant is on its list — and a hardcoded id then 404s and takes every AI feature with it.
// So a model is CHOSEN at call time, never pinned: the key's own /models list (cached 12 h per key) is
// ranked for the quality tier the caller asks for; a saved model is only a PREFERENCE, honoured while the
// provider still serves it; and a call that comes back "model not found / decommissioned" drops the list,
// re-picks and retries (withAutoModel). With no list (offline, provider without /models) the saved or
// default id is tried as before, so nothing gets worse than it was.
const MODEL_TTL = 12 * 3600 * 1000;
// Not chat models (speech, embeddings, moderation, images, agentic systems): never picked for a chat call.
const NON_CHAT = /whisper|tts|speech|audio|transcri|embed|guard|moderat|orpheus|playai|dall-?e|image|rerank|compound|realtime|native|-live|-search|aqa/i;
// Built-in web search on Groq's chat endpoint ({type:'browser_search'} tool): the gpt-oss family.
export const WEB_SEARCH_MODEL = /gpt-oss-(20b|120b)$/i;
// Image generation models (the /images/generations endpoint): newest first when picking.
export const IMAGE_MODEL = /image|imagine|dall-?e|flux|sdxl/i;
// Coarse [strength, speed] per family (0-100). Not a benchmark: an ordering that keeps a strong general
// chat model on top, a fast one for 'fast', and never an obscure tiny model when a real one is served.
const FAMILIES = [
  [/opus/i, 96, 25], [/sonnet/i, 88, 55], [/haiku/i, 64, 92],
  [/gpt-oss-120b/i, 86, 62], [/gpt-oss-20b/i, 70, 88],
  [/kimi-k2/i, 86, 55], [/deepseek-(r1|v3)/i, 82, 35], [/minimax/i, 80, 55],
  [/llama-4-maverick/i, 82, 60], [/llama-4-scout/i, 74, 80], [/405b/i, 88, 20], [/70b/i, 74, 62],
  [/llama-?3\.\d-(8b|3b|1b)|8b-instant/i, 46, 96], [/qwen/i, 72, 70],
  [/grok-[4-9]/i, 94, 45], [/grok-.*(mini|fast)/i, 68, 90], [/grok-3/i, 86, 55], [/grok/i, 72, 60],
  [/gemini-.*pro/i, 92, 40], [/gemini-.*flash-lite/i, 56, 96], [/gemini-.*flash/i, 78, 82],
  [/gemma/i, 52, 85], [/mixtral|mistral/i, 58, 75],
];
function modelTraits(id) {
  const s = String(id).toLowerCase();
  for (const [re, strength, speed] of FAMILIES) if (re.test(s)) return { strength, speed };
  const b = s.match(/(\d+(?:\.\d+)?)b\b/);                       // unknown family: judge by its parameter count
  if (b) { const st = Math.min(84, Math.round(30 + 10 * Math.log2(parseFloat(b[1])))); return { strength: st, speed: Math.max(20, 100 - st) }; }
  return { strength: 50, speed: 60 };
}
// Ids NucleoOS itself shipped as defaults in the past (see resolveModel): a config still holding one follows
// its family forward instead of staying on an old model forever.
// ('claude-sonnet-5' was the ESP32-P4 shell's default before the editions shared one shell.)
const FORMER_DEFAULTS = new Set(['claude-sonnet-4-6', 'claude-opus-4-8', 'claude-sonnet-5']);
const familyOf = (id) => { const s = String(id).toLowerCase(); return FAMILIES.findIndex(([re]) => re.test(s)); };
// Newer beats older inside a family: every number in the id, compared left to right ("4-6" > "4-5").
const versionOf = (id) => (String(id).match(/\d+(?:\.\d+)?/g) || []).map(Number);
function cmpVersion(a, b) { for (let i = 0; i < Math.max(a.length, b.length); i++) { const d = (a[i] || 0) - (b[i] || 0); if (d) return d; } return 0; }
const TIER_W = { max: [1, 0.05], mid: [0.7, 0.3], fast: [0.2, 0.8] };

// Chat-capable ids of `ids`, best first for `tier` ('max' | 'mid' | 'fast').
export function rankModels(ids, tier = 'mid') {
  const [ws, wf] = TIER_W[tier] || TIER_W.mid;
  const all = (ids || []).filter((id) => id && !NON_CHAT.test(id))
    .map((id) => { const t = modelTraits(id); return { id, strength: t.strength, score: t.strength * ws + t.speed * wf, v: versionOf(id) }; });
  const pool = tier === 'fast' && all.some((m) => m.strength >= 45) ? all.filter((m) => m.strength >= 45) : all;   // fast, but still useful
  return pool.sort((a, b) => (b.score - a.score) || cmpVersion(b.v, a.v)).map((m) => m.id);
}

const _modelMem = new Map();
// A cache key per API key without storing the key: FNV-1a of it.
function keyTag(key) { let h = 2166136261; for (const c of String(key || '')) { h ^= c.codePointAt(0); h = Math.imul(h, 16777619); } return (h >>> 0).toString(36); }
// What identifies "this key" for the cache: the key itself; a sealed-vault key by its device hint (the page never
// sees the key); a keyless LAN server by its address.
const modelCacheKey = (cfg) => 'ai.models.' + (cfg && cfg.provider) + '.' + keyTag(cfg && (isVaultKey(cfg.key) ? (cfg.keyHint || cfg.key) : (cfg.key || cfg.base)));

// The model ids this key can use right now (cached 12 h), or null when the provider can't be asked.
export async function listModels(cfg, { fresh = false } = {}) {
  if (!cfg || (!cfg.key && !providerOf(cfg.provider).keyOptional)) return null;
  const ck = modelCacheKey(cfg);
  if (!fresh) {
    const m = _modelMem.get(ck);
    if (m && Date.now() - m.at < MODEL_TTL) return m.ids;
    try { const j = JSON.parse(localStorage.getItem(ck) || 'null'); if (j && Array.isArray(j.ids) && Date.now() - j.at < MODEL_TTL) { _modelMem.set(ck, j); return j.ids; } } catch {}
  }
  let ids = null;
  try {
    if (cfg.provider === 'google') ids = await geminiListModels(cfg);
    else if (cfg.provider === 'anthropic') {
      const r = await fetch(apiUrl(cfg, '/v1/models?limit=100'), { headers: authHeaders(cfg), cache: 'no-store' });
      const j = r.ok ? await r.json().catch(() => null) : null;
      ids = j && Array.isArray(j.data) ? j.data.map((m) => m && m.id).filter(Boolean) : null;
    } else {
      const r = await fetch(apiUrl(cfg, '/models'), { headers: authHeaders(cfg), cache: 'no-store' });
      const j = r.ok ? await r.json().catch(() => null) : null;
      const data = j && (j.data || j.models);                      // OpenAI-compatible {data:[{id}]} | Ollama {models:[{name}]}
      ids = Array.isArray(data) ? data.filter((m) => m && (m.id || m.name || m.model) && m.active !== false).map((m) => m.id || m.name || m.model) : null;
    }
  } catch { ids = null; }
  if (!ids || !ids.length) return null;
  const e = { at: Date.now(), ids };
  _modelMem.set(ck, e);
  try { localStorage.setItem(ck, JSON.stringify(e)); } catch {}
  return ids;
}
export function forgetModels(cfg) { const ck = modelCacheKey(cfg); _modelMem.delete(ck); try { localStorage.removeItem(ck); } catch {} }

// The model to call for `cfg`: the saved one while the provider still serves it, else the best served one for
// the tier; need:'web' = a model with built-in web search. null only when the provider serves nothing usable.
export async function resolveModel(cfg, { tier, exclude = [], fresh = false, need } = {}) {
  const t = tier || (cfg && cfg.tier) || 'mid';
  const saved = cfg && cfg.model && cfg.model !== 'auto' ? cfg.model : '';
  const ids = await listModels(cfg, { fresh });
  if (!ids) {                                                       // can't ask: the saved/static guess, as before
    if (need === 'web') return null;
    if (need === 'image') return saved && !exclude.includes(saved) ? saved : null;
    const stat = (TIERS[cfg && cfg.provider] || {})[t] || providerOf(cfg && cfg.provider).def;
    return [saved, stat].find((m) => m && !exclude.includes(m)) || null;
  }
  let pool = ids.filter((id) => !exclude.includes(id));
  if (need === 'image') {                                          // an image model: the saved one while served, else the newest
    const imgs = pool.filter((id) => IMAGE_MODEL.test(id));
    if (saved && imgs.includes(saved)) return saved;
    return imgs.sort((a, b) => cmpVersion(versionOf(b), versionOf(a)))[0] || null;
  }
  if (need === 'web') pool = pool.filter((id) => WEB_SEARCH_MODEL.test(id));
  else if (saved && pool.includes(saved) && !NON_CHAT.test(saved)) {
    // A saved id that is only a FORMER FACTORY DEFAULT was never the user's choice: it follows the newest
    // served model of its family (Sonnet 4.6 → 5.5). A model the user picked is honoured while served.
    if (FORMER_DEFAULTS.has(saved)) {
      const fam = familyOf(saved);
      const newer = pool.filter((id) => familyOf(id) === fam && !NON_CHAT.test(id) && cmpVersion(versionOf(id), versionOf(saved)) > 0)
        .sort((a, b) => cmpVersion(versionOf(b), versionOf(a)))[0];
      if (newer) return newer;
    }
    return saved;
  }
  return rankModels(pool, need === 'web' ? 'max' : t)[0] || null;
}

// `model` when the key's provider still serves it (or its list can't be read), else the best served model of the
// same kind — for code with its own model tables (the agent runtime) that must never call a retired id.
export function tierOfModel(id) {
  const s = String(id || '').toLowerCase();
  if (/haiku|8b|lite|instant|mini|nano|small|fast/.test(s)) return 'fast';
  if (/opus|70b|120b|405b|-pro\b|maverick|grok-[4-9]/.test(s)) return 'max';
  return 'mid';
}
export async function servedModel(cfg, model, { exclude = [], fresh = false } = {}) {
  const ids = await listModels(cfg, { fresh });
  if (!ids) return model;
  if (model && ids.includes(model) && !exclude.includes(model)) return model;
  return (await resolveModel({ ...cfg, model: '' }, { tier: tierOfModel(model), exclude: [...exclude, model].filter(Boolean) })) || model;
}

// ── errors the user can act on ─────────────────────────────────────────────────────────────────────
// Every provider failure becomes an AiError with a KIND the UI can explain in plain words (explainAiError):
// auth | forbidden | model | quota | rate | too_long | provider_down | device_busy | network | timeout | bad_request
// | no_engine (no key and no PC model) | private_no_pc (Private mode and no model on this computer) — ai-ask.js
export class AiError extends Error {
  constructor(kind, message, extra = {}) { super(message); this.name = 'AiError'; this.kind = kind; Object.assign(this, extra); }
}
const RATE_TEXT = /rate.?limit (reached|exceeded)|tokens per (minute|day)|requests per (minute|day)|\b(tpm|tpd|rpm|rpd)\b/;
// "Please try again in 7m12.5s" / "in 1h2m" / "in 850ms" → seconds (Groq puts the wait in the text, not always in Retry-After).
export function retryAfterFromText(text) {
  const m = String(text || '').match(/try again in\s+(?:(\d+)h)?\s*(?:(\d+)m(?!s))?\s*(?:([\d.]+)s)?\s*(?:(\d+)ms)?/i);
  if (!m || !(m[1] || m[2] || m[3] || m[4])) return 0;
  return Math.max(1, Math.ceil((+m[1] || 0) * 3600 + (+m[2] || 0) * 60 + (+m[3] || 0) + (+m[4] || 0) / 1000));
}
export function aiErrorKind(status, text) {
  const m = String(text || '').toLowerCase();
  if (status === 401 || /invalid[_ ]?api[_ ]?key|authentication_error|invalid x-api-key|incorrect api key|api key not valid|unauthori[sz]ed/.test(m)) return 'auth';
  if (/model_not_found|model_decommissioned|decommission|does not exist|no longer (supported|available)|not_found_error|unknown model|model .{0,60}not found|deprecated model/.test(m)) return 'model';
  if (status === 413 || /context_length|context length|maximum context|too many tokens|request too large|prompt is too long/.test(m)) return 'too_long';
  // Groq's 429 "Rate limit reached … tokens per day (TPD) … Upgrade … settings/billing" is a LIMIT that resets, not a
  // spent credit — it said "credit used up" to a free-tier user who only had to wait a few minutes.
  if (RATE_TEXT.test(m)) return 'rate';
  if (/insufficient_quota|exceeded your (current )?quota|credit balance|billing|out of credits|payment required/.test(m) || status === 402) return 'quota';
  if (status === 429 || /rate.?limit|too many requests/.test(m)) return 'rate';
  if (status === 404 || /\bhttp 404\b/.test(m)) return 'model';
  if (status === 403 || /permission|forbidden|not allowed|unsupported (region|country)|location is not supported/.test(m)) return 'forbidden';
  if (status === 503 && /"busy"|device busy|low-mem|arbiter/.test(m)) return 'device_busy';   // the Cardputer's /api/llm relay
  if (status >= 500 || /overloaded/.test(m)) return 'provider_down';
  return 'bad_request';
}
// Provider errors can echo the request's credentials ("Invalid API key: gsk_…", "Bearer sk-…"), and that text
// reaches every app's UI through explainAiError. Mask the configured key and anything key-shaped.
export function redactSecrets(text, key) {
  let s = String(text == null ? '' : text);
  if (key && String(key).length >= 8) s = s.split(String(key)).join(maskKey(String(key)));
  return s.replace(/(Bearer\s+)[A-Za-z0-9._~+/=-]{8,}/gi, '$1***')
    .replace(/(sk-ant-|sk-|gsk_|xai-|AIza)[A-Za-z0-9_-]{6,}/g, '$1***');
}
// Build an AiError from a failed provider response (reads its JSON/text body once).
export async function aiErrorFromResponse(resp, cfg, bodyText) {
  let text = bodyText;
  if (text == null) { try { text = await resp.text(); } catch { text = ''; } }
  let msg = '';
  try { const j = JSON.parse(text); const e = j && (j.error || j); msg = [e.code, e.type, e.message || (typeof e === 'string' ? e : '')].filter(Boolean).join(' · '); } catch { msg = String(text || '').slice(0, 300); }
  const kind = aiErrorKind(resp.status, msg || text);
  const ra = Number(resp.headers && resp.headers.get && resp.headers.get('retry-after')) || retryAfterFromText(msg || text);
  // .body: the raw reply (first 300 chars, secrets masked) for callers that classify on it (AI Chat's 401/404 hints).
  return new AiError(kind, redactSecrets(msg, cfg && cfg.key) || ('HTTP ' + resp.status), { status: resp.status, provider: cfg && cfg.provider, model: cfg && cfg.model, retryAfter: ra, body: redactSecrets(String(text || '').slice(0, 300), cfg && cfg.key) });
}
// Wrap anything thrown around a provider call (network TypeError, abort, an AiError already) as an AiError.
export function toAiError(e, cfg) {
  if (e instanceof AiError) return e;
  const name = e && e.name, msg = redactSecrets((e && e.message) || e || '', cfg && cfg.key);
  const extra = { provider: (cfg && cfg.provider) || (e && e.provider), model: (cfg && cfg.model) || (e && e.model), cause: e };   // a thrown error may carry them
  if (e && e.partial) extra.partial = e.partial;   // a stream that died mid-answer keeps what already arrived
  if (name === 'AbortError') return new AiError('stopped', msg, extra);
  if (name === 'TimeoutError' || /timed? ?out/i.test(msg)) return new AiError('timeout', msg, extra);
  if (name === 'TypeError' || /failed to fetch|networkerror|load failed|network/i.test(msg)) return new AiError('network', msg, extra);
  if (e && e.status) Object.assign(extra, { status: e.status, retryAfter: e.retryAfter || retryAfterFromText(msg) });
  return new AiError(aiErrorKind(e && e.status, msg + ' ' + ((e && e.code) || '')), msg, extra);
}

// Run fn(cfg) with an auto-chosen model, re-picking on "model gone": up to three different models, so a
// provider retiring its models can never silently break a feature again. Other failures are thrown as
// AiError (never retried here: a bad key stays a bad key). Resolves with fn's result; the model that
// answered is left on opts.used (callers show it).
export async function withAutoModel(cfg, fn, opts = {}) {
  const tier = opts.tier || cfg.tier;
  const tried = [];
  let model = await resolveModel(cfg, { tier, need: opts.need });
  for (let attempt = 0; attempt < 3; attempt++) {
    if (!model) throw new AiError('model', 'no usable model', { provider: cfg.provider, model: tried[tried.length - 1] || cfg.model });
    try { const out = await fn({ ...cfg, model }); opts.used = model; return out; }
    catch (e) {
      const err = toAiError(e, { ...cfg, model });
      if (err.kind !== 'model') throw err;
      tried.push(model);
      model = await resolveModel(cfg, { tier, need: opts.need, exclude: tried, fresh: attempt === 0 });
    }
  }
  throw new AiError('model', 'no usable model', { provider: cfg.provider, model: tried[tried.length - 1] || cfg.model });
}

// One plain sentence that says WHAT went wrong and HOW to fix it, in the OS language (it/en/es/fr/de).
const AI_ERR_TEXT = {
  auth: {
    it: 'La chiave {P} non è valida o è stata revocata. Aprila in {SET} e incolla una chiave nuova.',
    en: 'The {P} key is invalid or was revoked. Open {SET} and paste a new key.',
    es: 'La clave de {P} no es válida o fue revocada. Abre {SET} y pega una clave nueva.',
    fr: 'La clé {P} est invalide ou a été révoquée. Ouvrez {SET} et collez une nouvelle clé.',
    de: 'Der {P}-Schlüssel ist ungültig oder wurde widerrufen. Öffne {SET} und füge einen neuen ein.' },
  forbidden: {
    it: '{P} ha rifiutato l’accesso (permessi o area geografica della chiave). Controlla l’account {P} o scegli un altro provider in {SET}.',
    en: '{P} refused access (key permissions or region). Check your {P} account or pick another provider in {SET}.',
    es: '{P} rechazó el acceso (permisos o región de la clave). Revisa tu cuenta de {P} o elige otro proveedor en {SET}.',
    fr: '{P} a refusé l’accès (droits ou région de la clé). Vérifiez votre compte {P} ou choisissez un autre fournisseur dans {SET}.',
    de: '{P} hat den Zugriff verweigert (Rechte oder Region des Schlüssels). Prüfe dein {P}-Konto oder wähle einen anderen Anbieter unter {SET}.' },
  model: {
    it: '{P} non offre più il modello «{M}» e con questa chiave non ho trovato un altro modello di chat. Controlla la chiave o scegli un altro provider in {SET}.',
    en: '{P} no longer serves the model “{M}” and I found no other chat model for this key. Check the key or pick another provider in {SET}.',
    es: '{P} ya no ofrece el modelo «{M}» y no encontré otro modelo de chat para esta clave. Revisa la clave o elige otro proveedor en {SET}.',
    fr: '{P} ne propose plus le modèle « {M} » et je n’ai trouvé aucun autre modèle de chat pour cette clé. Vérifiez la clé ou choisissez un autre fournisseur dans {SET}.',
    de: '{P} bietet das Modell „{M}“ nicht mehr an, und für diesen Schlüssel fand ich kein anderes Chat-Modell. Prüfe den Schlüssel oder wähle einen anderen Anbieter unter {SET}.' },
  quota: {
    it: 'Il credito o la quota di {P} è esaurito. Ricarica o attiva la fatturazione sul sito di {P}, oppure usa un altro provider in {SET}.',
    en: 'Your {P} credit or quota is used up. Top up or enable billing on the {P} site, or use another provider in {SET}.',
    es: 'Se agotó el crédito o la cuota de {P}. Recarga o activa la facturación en {P}, o usa otro proveedor en {SET}.',
    fr: 'Le crédit ou le quota {P} est épuisé. Rechargez ou activez la facturation sur le site {P}, ou utilisez un autre fournisseur dans {SET}.',
    de: 'Dein {P}-Guthaben oder -Kontingent ist aufgebraucht. Lade es auf der {P}-Seite auf oder nutze einen anderen Anbieter unter {SET}.' },
  rate: {
    it: '{P} sta limitando le richieste per un momento{R}. Riprova tra poco.',
    en: '{P} is rate-limiting requests for a moment{R}. Try again shortly.',
    es: '{P} está limitando las solicitudes un momento{R}. Vuelve a intentarlo en breve.',
    fr: '{P} limite les requêtes pour un instant{R}. Réessayez bientôt.',
    de: '{P} begrenzt gerade die Anfragen{R}. Versuche es gleich noch einmal.' },
  too_long: {
    it: 'La conversazione è troppo lunga per il modello «{M}». Inizia una nuova conversazione o accorcia il messaggio.',
    en: 'The conversation is too long for the model “{M}”. Start a new conversation or shorten the message.',
    es: 'La conversación es demasiado larga para el modelo «{M}». Empieza una nueva o acorta el mensaje.',
    fr: 'La conversation est trop longue pour le modèle « {M} ». Commencez-en une nouvelle ou raccourcissez le message.',
    de: 'Die Unterhaltung ist zu lang für das Modell „{M}“. Starte eine neue oder kürze die Nachricht.' },
  provider_down: {
    it: '{P} ha un problema temporaneo{S}. Riprova fra qualche minuto.',
    en: '{P} has a temporary problem{S}. Try again in a few minutes.',
    es: '{P} tiene un problema temporal{S}. Inténtalo de nuevo en unos minutos.',
    fr: '{P} a un problème temporaire{S}. Réessayez dans quelques minutes.',
    de: '{P} hat ein vorübergehendes Problem{S}. Versuche es in ein paar Minuten erneut.' },
  device_busy: {
    it: 'Il dispositivo è occupato a fare da ponte verso {P}. Riprova tra qualche secondo.',
    en: 'The device is busy relaying to {P}. Try again in a few seconds.',
    es: 'El dispositivo está ocupado haciendo de puente hacia {P}. Inténtalo en unos segundos.',
    fr: 'L’appareil est occupé à relayer vers {P}. Réessayez dans quelques secondes.',
    de: 'Das Gerät ist mit der Weiterleitung zu {P} beschäftigt. Versuche es in ein paar Sekunden.' },
  network: {
    it: 'Non riesco a raggiungere {P}: controlla la connessione a Internet di questo computer (o un blocco di rete / estensione del browser).',
    en: 'I can’t reach {P}: check this computer’s Internet connection (or a network block / browser extension).',
    es: 'No puedo llegar a {P}: revisa la conexión a Internet de este equipo (o un bloqueo de red / extensión del navegador).',
    fr: 'Impossible de joindre {P} : vérifiez la connexion Internet de cet ordinateur (ou un blocage réseau / une extension).',
    de: '{P} ist nicht erreichbar: prüfe die Internetverbindung dieses Computers (oder eine Netzsperre / Browser-Erweiterung).' },
  timeout: {
    it: '{P} non ha risposto in tempo. Riprova; se succede spesso, scegli un modello più veloce in {SET}.',
    en: '{P} didn’t answer in time. Try again; if it keeps happening, pick a faster model in {SET}.',
    es: '{P} no respondió a tiempo. Inténtalo de nuevo; si se repite, elige un modelo más rápido en {SET}.',
    fr: '{P} n’a pas répondu à temps. Réessayez ; si cela se répète, choisissez un modèle plus rapide dans {SET}.',
    de: '{P} hat nicht rechtzeitig geantwortet. Versuche es erneut; wenn es öfter passiert, wähle ein schnelleres Modell unter {SET}.' },
  no_engine: {
    it: 'Nessuna IA disponibile: aggiungi una chiave in {SET} oppure avvia un modello sul tuo PC (Ollama o LM Studio).',
    en: 'No AI is available: add a key in {SET}, or start a model on your PC (Ollama or LM Studio).',
    es: 'No hay ninguna IA disponible: añade una clave en {SET} o inicia un modelo en tu PC (Ollama o LM Studio).',
    fr: 'Aucune IA n’est disponible : ajoutez une clé dans {SET} ou lancez un modèle sur votre PC (Ollama ou LM Studio).',
    de: 'Keine KI verfügbar: Füge unter {SET} einen Schlüssel hinzu oder starte ein Modell auf deinem PC (Ollama oder LM Studio).' },
  private_no_pc: {
    it: 'Modalità Privato: può rispondere solo un modello su questo computer, e nessuno è acceso. Avvia Ollama o LM Studio, oppure passa ad Auto.',
    en: 'Private mode: only a model on this computer may answer, and none is running. Start Ollama or LM Studio, or switch to Auto.',
    es: 'Modo Privado: solo puede responder un modelo en este equipo y no hay ninguno en marcha. Inicia Ollama o LM Studio, o cambia a Auto.',
    fr: 'Mode Privé : seul un modèle sur cet ordinateur peut répondre, et aucun n’est lancé. Lancez Ollama ou LM Studio, ou passez en Auto.',
    de: 'Privat-Modus: Nur ein Modell auf diesem Computer darf antworten, und keines läuft. Starte Ollama oder LM Studio oder wechsle zu Auto.' },
  bad_request: {
    it: '{P} ha rifiutato la richiesta{D}.',
    en: '{P} rejected the request{D}.',
    es: '{P} rechazó la solicitud{D}.',
    fr: '{P} a refusé la requête{D}.',
    de: '{P} hat die Anfrage abgelehnt{D}.' },
};
// opts.settings: where THIS surface keeps its key (default: the OS Settings ▸ AI panel, localized).
const AI_SETTINGS_AT = { it: 'Impostazioni ▸ AI', en: 'Settings ▸ AI', es: 'Ajustes ▸ IA', fr: 'Réglages ▸ IA', de: 'Einstellungen ▸ KI' };
export function explainAiError(e, lang, opts = {}) {
  const err = e instanceof AiError ? e : toAiError(e);
  if (err.kind === 'stopped') return '';
  let l = lang;
  if (!l) { try { l = (localStorage.getItem('anima.lang') || document.documentElement.lang || 'en'); } catch { l = 'en'; } }
  l = String(l).slice(0, 2);
  const row = AI_ERR_TEXT[err.kind] || AI_ERR_TEXT.bad_request;
  const tpl = row[l] || row.en;
  // an unknown provider is "AI" — providerOf() falls back to Anthropic, and a Groq rate limit read "Claude is limiting…"
  const P = err.provider ? (providerOf(err.provider).label || err.provider) : ({ it: 'IA', es: 'IA', fr: 'IA', de: 'KI' }[l] || 'AI');
  const detail = redactSecrets(err.message || '').replace(/\s+/g, ' ').slice(0, 160);   // last line of defence
  return tpl.replace(/\{SET\}/g, opts.settings || AI_SETTINGS_AT[l] || AI_SETTINGS_AT.en)
    .replace(/\{P\}/g, P).replace(/\{M\}/g, err.model || '?')
    .replace('{R}', err.retryAfter ? ' (' + (err.retryAfter < 90 ? err.retryAfter + ' s' : Math.ceil(err.retryAfter / 60) + ' min') + ')' : '')
    .replace('{S}', err.status ? ` (HTTP ${err.status})` : '')
    .replace('{D}', detail ? `: ${detail}` : '');
}
// Kinds the user can do something about (vs. a transient blip): worth showing even when another tier answered.
export const actionableAiError = (e) => !!e && ['auth', 'forbidden', 'model', 'quota'].includes(e.kind);

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
    const r = await fetch(url, { headers: authHeaders({ provider: 'google', key: cfg.key }), cache: 'no-store' });
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
      headers: { 'content-type': 'application/json', ...authHeaders({ provider: 'google', key: cfg.key }) },
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

// Honest one-line tier label for the UI. `lang` accepts a 2-letter OS code ('it'|'en'|'es'|'fr'|'de')
// or a legacy boolean (true→en, false→it) so existing callers and host gates keep working.
export function geminiTierLabel(tier, lang) {
  const l = lang === true ? 'en' : (lang === false || lang == null ? 'it' : String(lang).slice(0, 2));
  const T = {
    paid:    { it: 'Piano a pagamento · modelli Pro disponibili', en: 'Paid plan · Pro models available', es: 'Plan de pago · modelos Pro disponibles', fr: 'Offre payante · modèles Pro disponibles', de: 'Bezahltarif · Pro-Modelle verfügbar' },
    free:    { it: 'Free tier · solo Flash (Pro richiede billing)', en: 'Free tier · Flash only (Pro needs billing)', es: 'Nivel gratis · solo Flash (Pro requiere facturación)', fr: 'Offre gratuite · Flash uniquement (Pro requiert la facturation)', de: 'Kostenlos · nur Flash (Pro erfordert Abrechnung)' },
    unknown: { it: 'Piano non rilevato', en: 'Plan not detected', es: 'Plan no detectado', fr: 'Offre non détectée', de: 'Tarif nicht erkannt' },
  };
  const row = tier === 'paid' ? T.paid : tier === 'free' ? T.free : T.unknown;
  return row[l] || row.it;
}
// A saved key shows as the device's hint ("gsk_…wxyz"); a key being typed as its own ends.
export const maskKey = (k, hint) => (isVaultKey(k) ? (hint || '••••') : (k && k.length > 10 ? k.slice(0, 6) + '…' + k.slice(-4) : (k ? '…' : '')));

// "martedì 29 settembre 2026 alle ore 22:40 (Europe/Rome)" in the OS language — appended to every system
// prompt the shell sends, because a model has no clock (twin of apps/anima/www/contextkit.js nowText).
const CLOCK_LOCALE = { it: 'it-IT', en: 'en-GB', es: 'es-ES', fr: 'fr-FR', de: 'de-DE' };
export function nowText(lang = 'en', d = new Date()) {
  let tz = ''; try { tz = Intl.DateTimeFormat().resolvedOptions().timeZone || ''; } catch {}
  try {
    const s = new Intl.DateTimeFormat(CLOCK_LOCALE[lang] || 'en-GB', { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric', hour: '2-digit', minute: '2-digit' }).format(d);
    return tz ? `${s} (${tz})` : s;
  } catch { return d.toISOString(); }
}
// The reply-language rule for an English-worded prompt (es/fr/de otherwise get English answers).
const REPLY_LANG = { it: 'Italian', en: 'English', es: 'Spanish', fr: 'French', de: 'German' };
export const replyRule = (lang) => 'Always reply in ' + (REPLY_LANG[lang] || 'English') + '.';

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
let _teacherCache = null, _teacherAt = 0, _teacherJob = null;
const TEACHER_TTL = 30000;
export function invalidateTeacher() { _teacherCache = null; _teacherAt = 0; _teacherJob = null; }

// The device's public view -> what the page works with: a saved key becomes VAULT_KEY (+ keyHint).
function fromPublic(j) {
  const sec = (o, name) => (o && o[name + '_set'] ? VAULT_KEY : ((o && typeof o[name] === 'string') ? o[name] : ''));
  const keys = {};
  for (const [p, e] of Object.entries((j && j.keys) || {})) {
    if (!e || typeof e !== 'object') continue;
    const c = Object.assign({}, e, { key: sec(e, 'key') });
    if (e.key_hint) c.keyHint = e.key_hint;
    delete c.key_set; delete c.key_hint;
    keys[p] = c;
  }
  return Object.assign({}, j, { key: sec(j, 'key'), keyHint: j.key_hint || '', vision_key: sec(j, 'vision_key'), keys });
}

// Read the active teacher config (paired). Returns a normalized cfg, {unpaired:true}, or null. Keys
// come back as VAULT_KEY (the device keeps them); an older firmware without /api/anima/teacher still
// answers through /api/fs/read.
export async function readTeacher(opts = {}) {
  if (!opts.fresh && _teacherCache && (Date.now() - _teacherAt) < TEACHER_TTL) return _teacherCache;
  // Concurrent readers (Settings' tabs, the capability probe, a preset) share ONE device read.
  if (!opts.fresh && _teacherJob) return _teacherJob;
  const job = _readTeacher();
  _teacherJob = job; job.finally(() => { if (_teacherJob === job) _teacherJob = null; });
  return job;
}
// A firmware without the sealed-vault endpoint answers 404 there: remembered, so the Cardputer and the Handheld
// pay that probe once per page, not on every vault read.
let _noTeacherApi = false;
async function _readTeacher() {
  try {
    let r = null, pub = false;
    // Only a device that keeps the keys sealed (vault.sealed) has the vault endpoint: elsewhere not even one probe.
    if (!_noTeacherApi && !(await import('./nucleo-caps.js').then((m) => m.default.has('vault.sealed')).catch(() => true))) _noTeacherApi = true;
    if (!_noTeacherApi) {
      r = await fetch(TEACHER_API, { cache: 'no-store' });
      if (r.status === 404) _noTeacherApi = true; else pub = true;
    }
    if (!pub) r = await fetch('/api/fs/read?path=' + encodeURIComponent(AI_PATH), { cache: 'no-store' });
    if (r.status === 401 || r.status === 403) return { unpaired: true };   // transient — not cached
    // 404 = nessuna chiave mai salvata: e' la configurazione vuota, non un guasto.
    // Prima tornava null e le Impostazioni dicevano "Non riesco a leggere la chiave"
    // su ogni console nuova (prova a schermo del 13/10/2026); null resta per i guasti veri.
    // Si memorizza come gli altri: senza, ogni apertura delle Impostazioni chiedeva
    // il file tre volte alla console (tre 404 di fila, prova del 13/10/2026).
    if (r.status === 404) {
      const p = providerOf('anthropic');
      _teacherCache = { provider: 'anthropic', base: p.base, model: p.def, key: '', version: p.version, exec: 'browser', keys: {}, geminiTier: '' };
      _teacherAt = Date.now();
      return _teacherCache;
    }
    if (!r.ok) return null;
    let j = JSON.parse(await r.text()) || {};
    if (pub) { j = fromPublic(j); _sealed = true; }
    const provider = (j.provider && PROVIDERS[j.provider]) ? j.provider : (isLanUrl(j.base) ? 'local' : (j.base && /anthropic/.test(j.base) ? 'anthropic' : (j.base && /generativelanguage/.test(j.base) ? 'google' : (j.base && /x\.ai/.test(j.base) ? 'xai' : (j.key ? 'openai' : 'anthropic')))));
    const p = providerOf(provider);
    const cfg = { provider, base: j.base || p.base, model: j.model || p.def, key: j.key || '', keyHint: j.keyHint || '', version: j.version || p.version, exec: j.exec || 'browser', keys: j.keys || {}, geminiTier: j.geminiTier || '', stt_url: j.stt_url || '', stt_model: j.stt_model || '', vision_model: j.vision_model || '', vision_base: j.vision_base || '', vision_key: j.vision_key || '', vision: typeof j.vision === 'boolean' ? j.vision : null };
    _teacherCache = cfg; _teacherAt = Date.now();
    return cfg;
  } catch { return null; }
}

// The vault's key that can transcribe (Groq / OpenAI Whisper) -> { key, base } or null. Same choice as the router
// (and the firmware): a Whisper-capable configured key, else the legacy keys.groq slot the firmware also reads.
export async function whisperKey(opts = {}) {
  const t = await readTeacher(opts);
  if (!t || t.unpaired) return null;
  const r = routeFor({ capability: 'whisper' }, t.keys || {}, t);
  if (r && r.key && !r.audioLLM) return { key: r.key, base: r.base };
  if (t.provider === 'openai' && t.key) return { key: t.key, base: t.base };
  const g = t.keys && t.keys.groq;
  return g && g.key ? { key: g.key, base: g.base || PROVIDERS.openai.base } : null;
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
  const keyless = providerOf(cfg.provider).keyOptional && entry.base && entry.model;   // a LAN server needs no key
  if (entry.key || keyless) keys[cfg.provider] = entry; else delete keys[cfg.provider];
  const extra = (cfg.provider === 'google' && cfg.geminiTier) ? { geminiTier: cfg.geminiTier } : {};   // top-level mirror for the firmware
  // The home speech-to-text server (whisper.cpp / speaches) rides along so a key change never drops it.
  const stt = cfg.stt_url ? Object.assign({ stt_url: cfg.stt_url }, cfg.stt_model ? { stt_model: cfg.stt_model } : {}) : {};
  // The multimodal settings too: the vision helper (a model that sees images, for a text-only chat
  // model) and the "vision" override of the detected capability.
  const vis = {};
  for (const k of ['vision_model', 'vision_base', 'vision_key']) if (cfg[k]) vis[k] = cfg[k];
  if (typeof cfg.vision === 'boolean') vis.vision = cfg.vision;
  const doc = Object.assign({ provider: cfg.provider, exec: cfg.exec || 'browser' }, entry, extra, stt, vis, { keys });
  return toDevice(doc);
}

// What goes to the device: a saved key (VAULT_KEY) is sent as "" — the device keeps it — and the
// page-side hints never leave the page.
function toDevice(o) {
  if (Array.isArray(o)) return o.map(toDevice);
  if (!o || typeof o !== 'object') return o;
  const out = {};
  for (const [k, v] of Object.entries(o)) {
    if (k === 'keyHint' || /_(set|hint)$/.test(k)) continue;
    out[k] = isVaultKey(v) ? '' : toDevice(v);
  }
  return out;
}

// Write the vault (paired): merged on the device, keys left blank are kept. true | 'unpaired' | false.
export async function writeTeacher(cfg) {
  try {
    const body = JSON.stringify(buildTeacherDoc(cfg));
    let r = _noTeacherApi ? null : await fetch(TEACHER_API, { method: 'POST', body });
    if (!r || r.status === 404) { _noTeacherApi = true; r = await fetch('/api/fs/write?path=' + encodeURIComponent(AI_PATH), { method: 'POST', body }); }   // classic vault
    if (r.ok) invalidateTeacher();   // our own write changed it — drop the memo so the next read is fresh
    return r.ok ? true : ((r.status === 401 || r.status === 403) ? 'unpaired' : false);
  } catch { return false; }
}

// ── keys typed inside an app (AI Chat, Dictation, Recorder) ──────────────────────────────────────────
const originOf = (u) => { try { return new URL(u).origin; } catch { return ''; } };
const providerForBase = (base) => (/\bx\.ai\b/.test(base) ? 'xai' : /anthropic\.com/.test(base) ? 'anthropic'
  : /generativelanguage/.test(base) ? 'google' : isLanUrl(base) ? 'local' : 'openai');

// Does the vault hold a key for `base`'s host (on a sealed vault: the relay will add it)? false when unpaired / unknown.
export async function hasKeyFor(base, opts = {}) {
  const t = await readTeacher(opts).catch(() => null);
  if (!t || t.unpaired) return false;
  const o = originOf(base);
  if (t.key && originOf(t.base) === o) return true;
  return Object.values(t.keys || {}).some((e) => e && e.key && originOf(e.base) === o);
}

// Save a key the user typed in an app into the device vault, under the provider of `base`'s host; the
// active provider stays unless there is none yet. true | 'unpaired' | false.
export async function saveAppKey(base, key, model) {
  const t = await readTeacher({ fresh: true }).catch(() => null);
  if (!t) return false;
  if (t.unpaired) return 'unpaired';
  const id = providerForBase(base);
  const b = String(base || providerOf(id).base).replace(/\/+$/, '');
  const keys = Object.assign({}, t.keys || {});
  keys[id] = Object.assign({}, keys[id] || {}, { base: b, model: model || (keys[id] && keys[id].model) || providerOf(id).def, key });
  const cfg = Object.assign({}, t, { keys });
  if (!t.key) Object.assign(cfg, { provider: id, base: b, model: keys[id].model, key, version: providerOf(id).version });
  return writeTeacher(cfg);
}

// Older pages kept a Groq key in this browser (localStorage 'groq.key', shared by AI Chat, Dictation and
// Recorder — Dictation even copied the device's key there). The device holds keys now: move it into the
// vault when the vault has none for that host, then drop it from the browser.
export async function adoptBrowserKey() {
  let k = '', base = '';
  try { k = localStorage.getItem('groq.key') || ''; base = localStorage.getItem('groq.base') || ''; } catch { return; }
  if (!k) return;
  base = (base || PROVIDERS.openai.base).replace(/\/+$/, '');
  if (!(await hasKeyFor(base, { fresh: true }))) {
    const r = await saveAppKey(base, k);
    if (r !== true) return;   // unpaired / offline: keep it until the vault can take it
  }
  try { localStorage.removeItem('groq.key'); } catch {}
}

// Sonnet 5 runs ADAPTIVE thinking when `thinking` is omitted (the 4.x family ran thinking-off): for chat
// surfaces that is slower + costlier, and a tiny max_tokens can be eaten by thinking before any text. The
// rule as the ESP32-P4 apps (agent runtime, games coach) import it: disabled ONLY for sonnet-5* (fable-5
// rejects "disabled" with a 400; 4.x is already off). This file's own wire does not apply it yet.
export const anthThinking = (model) => (String(model || '').includes('sonnet-5') ? { thinking: { type: 'disabled' } } : {});

// Completion. Returns the assistant text; throws an AiError (explainAiError() says what to do).
// The model is auto-chosen and re-picked if the provider retired it (withAutoModel). opts.tier = 'max'|'mid'|'fast'.
// opts.signal — an AbortSignal so a user-facing Stop can cancel a hung cloud call (copilot.js).
// opts.meta — an object that receives { model } = the model that actually answered (labels that never lie).
export async function cloudComplete(cfg, system, user, maxTokens, opts = {}) {
  const run = { tier: opts.tier };
  const out = await withAutoModel(cfg, (c) => completeOnce(c, system, user, maxTokens, opts), run);
  if (opts.meta) opts.meta.model = run.used;
  return out;
}
async function completeOnce(cfg, system, user, maxTokens, opts) {
  if (isAnth(cfg)) {
    const resp = await fetch(apiUrl(cfg, '/v1/messages'), {
      method: 'POST', signal: opts.signal,
      headers: authHeaders(cfg),
      body: JSON.stringify({ model: cfg.model, max_tokens: maxTokens || 1024, ...(system ? { system } : {}), messages: [{ role: 'user', content: user }] }),
    });
    const text = await resp.text().catch(() => '');
    let j = null; try { j = JSON.parse(text); } catch {}
    if (!resp.ok || !j || j.type === 'error') throw await aiErrorFromResponse(resp, cfg, text);
    return Array.isArray(j.content) ? j.content.filter((b) => b && b.type === 'text').map((b) => b.text).join('') : '';
  }
  const resp = await fetch(apiUrl(cfg, '/chat/completions'), {
    method: 'POST', signal: opts.signal,
    headers: authHeaders(cfg),
    body: JSON.stringify({ model: cfg.model, temperature: 0.4, messages: [...(system ? [{ role: 'system', content: system }] : []), { role: 'user', content: user }] }),
  });
  const text = await resp.text().catch(() => '');
  let j = null; try { j = JSON.parse(text); } catch {}
  if (!resp.ok || !j || j.error) throw await aiErrorFromResponse(resp, cfg, text);
  return (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';
}

const anthText = (j) => (Array.isArray(j && j.content) ? j.content.filter((b) => b && b.type === 'text').map((b) => b.text).join('') : '');
const oaText = (j) => (j && j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';

// Chat completion on ANY provider, multi-turn and optionally streamed — the call the apps share (AI Chat, Games,
// Paint, the agent) instead of each carrying its own wire. Returns the assistant text; throws an AiError.
//   messages  — a [{role,content}] transcript; or `user` for a single turn
//   system    — top-level `system` on Anthropic, a leading system message on the OpenAI-compatible wire
//   maxTokens — Anthropic always needs one (default 1024); on the OpenAI wire it is sent ONLY when given
//   temperature — OpenAI-compatible wire only
//   signal    — AbortSignal (Stop button, timeouts);  tier / meta — as cloudComplete
//   onDelta(piece, full) — STREAM the reply (SSE) and call back per fragment; the full text is returned.
//               OpenAI-compatible `choices[].delta.content` and Anthropic `content_block_delta` both parse.
export async function cloudChat(cfg, opts = {}) {
  const run = { tier: opts.tier };
  const out = await withAutoModel(cfg, (c) => chatOnce(c, opts), run);
  if (opts.meta) opts.meta.model = run.used;
  return out;
}
// Groq retired its `compound` web-search model on 2026-09-21. Handheld apps (ANIMA, AI Chat) still call
// AI.groqCompound for their Web mode: answer with a clear model error instead of a TypeError on undefined.
export async function groqCompound(cfg) {
  throw new AiError('model', 'groq/compound is no longer served', { provider: (cfg && cfg.provider) || 'openai', model: 'groq/compound' });
}
async function chatOnce(cfg, { system, messages, user, maxTokens, temperature, signal, onDelta } = {}) {
  const msgs = Array.isArray(messages) ? messages : [{ role: 'user', content: String(user == null ? '' : user) }];
  const anth = isAnth(cfg);
  const body = anth
    ? { model: cfg.model, max_tokens: maxTokens || 1024, ...(system ? { system } : {}), messages: msgs }
    : { model: cfg.model, ...(maxTokens != null ? { max_tokens: maxTokens } : {}), ...(typeof temperature === 'number' ? { temperature } : {}),
        messages: [...(system ? [{ role: 'system', content: system }] : []), ...msgs] };
  const stream = typeof onDelta === 'function';
  if (stream) body.stream = true;
  const resp = await fetch(apiUrl(cfg, anth ? '/v1/messages' : '/chat/completions'), { method: 'POST', signal, headers: authHeaders(cfg), body: JSON.stringify(body) });
  if (!stream) {
    const text = await resp.text().catch(() => '');
    let j = null; try { j = JSON.parse(text); } catch {}
    if (!resp.ok || !j || j.type === 'error' || j.error) throw await aiErrorFromResponse(resp, cfg, text);
    return anth ? anthText(j) : oaText(j);
  }
  if (!resp.ok || !resp.body) throw await aiErrorFromResponse(resp, cfg);
  const reader = resp.body.getReader(), dec = new TextDecoder();
  let buf = '', full = '';
  for (;;) {
    const { value, done } = await reader.read();
    if (done) break;
    buf += dec.decode(value, { stream: true });
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, nl).trim(); buf = buf.slice(nl + 1);
      if (!line.startsWith('data:')) continue;           // `event:` lines, keep-alives, blank separators
      const data = line.slice(5).trim();
      if (data === '[DONE]') { buf = ''; break; }
      let ev; try { ev = JSON.parse(data); } catch { continue; }   // partial / keep-alive frame
      if (anth && ev && ev.type === 'error') {           // Anthropic in-stream error (e.g. overloaded)
        const e = await aiErrorFromResponse(resp, cfg, data);
        if (full) e.partial = full;                      // keep what already arrived
        throw e;
      }
      const piece = anth
        ? (ev && ev.type === 'content_block_delta' && ev.delta && ev.delta.type === 'text_delta' ? ev.delta.text : '')
        : (ev && ev.choices && ev.choices[0] && ev.choices[0].delta && ev.choices[0].delta.content);
      if (piece) { full += piece; onDelta(piece, full); }
    }
  }
  return full;
}

// Provider-aware single-shot TOOL selection. `tools` is an OpenAI-style tool list (anima-skill's
// actionsToOpenAITools); `messages` is an OpenAI-style [{role,content}] history+turn. Returns
// {tool, args} | {text} | null; throws an AiError on a definite HTTP/network failure (callers try/catch → miss).
// Anthropic uses native tool_use; Groq/xAI go browser-direct; Gemini (and every provider on a sealed vault)
// relays via the device /api/llm proxy. This is the ONE place a NucleoOS app needs to call to get tool-calling
// on EVERY provider — it replaces each app re-implementing the per-provider wire (the fragmentation we're undoing).
// The model is auto-chosen like cloudComplete (tier: cfg.tier, else 'mid').
export async function cloudToolCall(cfg, opts = {}) {
  const run = { tier: opts.tier };
  const out = await withAutoModel(cfg, (c) => toolCallOnce(c, opts), run);
  if (opts.meta) opts.meta.model = run.used;
  return out;
}
async function toolCallOnce(cfg, { system, messages = [], tools = [], signal, maxTokens = 512, temperature = 0.2, responseFormat = null } = {}) {
  if (isAnth(cfg)) {
    const atools = (tools || []).map((t) => ({ name: t.function.name, description: t.function.description, input_schema: t.function.parameters || { type: 'object', properties: {} } }));
    const resp = await fetch(apiUrl(cfg, '/v1/messages'), {
      method: 'POST', signal,
      headers: authHeaders(cfg),
      body: JSON.stringify({ model: cfg.model, max_tokens: maxTokens, ...(system ? { system } : {}), ...(atools.length ? { tools: atools } : {}), messages }),
    });
    const text = await resp.text().catch(() => '');
    let j = null; try { j = JSON.parse(text); } catch {}
    if (!resp.ok || !j || j.type === 'error') throw await aiErrorFromResponse(resp, cfg, text);
    const blocks = Array.isArray(j.content) ? j.content : [];
    const tu = blocks.find((b) => b && b.type === 'tool_use');
    if (tu) return { tool: tu.name, args: tu.input || {} };
    const out = anthText(j);
    return out ? { text: out } : null;
  }
  const resp = await fetch(apiUrl(cfg, '/chat/completions'), {
    method: 'POST', signal,
    headers: authHeaders(cfg),
    body: JSON.stringify({ model: cfg.model, max_tokens: maxTokens, temperature, messages: [...(system ? [{ role: 'system', content: system }] : []), ...messages], ...(tools.length ? { tools, tool_choice: 'auto' } : {}), ...(responseFormat ? { response_format: responseFormat } : {}) }),
  });
  const text = await resp.text().catch(() => '');
  let j = null; try { j = JSON.parse(text); } catch {}
  if (!resp.ok || !j || j.error) throw await aiErrorFromResponse(resp, cfg, text);
  const msg = j.choices && j.choices[0] && j.choices[0].message;
  if (!msg) return null;
  const tc = msg.tool_calls && msg.tool_calls[0];
  if (tc && tc.function) { let a = {}; try { a = JSON.parse(tc.function.arguments || '{}'); } catch {} return { tool: tc.function.name, args: a }; }
  return msg.content ? { text: msg.content } : null;
}

// Verify a key works (a tiny call: browser-direct, or through the device relay). true/false; throws on a definite rejection.
export async function cloudPing(cfg) {
  const t = await cloudComplete(cfg, null, 'Reply with exactly: ok', 16);
  return /\bok\b/i.test(t || '');
}

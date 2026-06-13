// llm.js — minimal online-LLM client for the Game Center, mirroring apps/agent/www/runtime.js.
// Reads the device key vault (/data/anima/teacher.json), picks the best available provider and exposes
// a single `ask(system, user)` that returns text. Used by the LLM coach to set an opponent's STYLE
// between points — never to steer the paddle frame-by-frame (that's impossible at LLM latency).
//
// PROVIDER TRUTH — the firmware nicknames the cloud teacher "Grok" everywhere, but the actual brand is
// whatever teacher.json points at. We resolve and LABEL it honestly so the UI can prove which brain ran:
//   · Anthropic Claude   → api.anthropic.com         (Messages API)
//   · xAI Grok           → api.x.ai/v1               (OpenAI-compatible, model grok-*)
//   · Groq (Llama etc.)  → api.groq.com/openai/v1    (OpenAI-compatible, NOT xAI — different company!)
//   · OpenAI / other     → any OpenAI-compatible /chat/completions endpoint
// Grok and Groq are different companies; conflating them is the bug we fix here.

const KEY_PATH = '/data/anima/teacher.json';

// Identify the real brand from the endpoint host + provider, so the label never lies.
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

// Human label "Brand (model)" and the bare endpoint host — both shown in-game to verify the real brain.
export function providerLabel(cfg) { if (!cfg) return null; const b = brandOf(cfg); return cfg.model ? `${b} (${cfg.model})` : b; }
export function endpointHost(cfg) { if (!cfg) return null; try { return new URL(cfg.base).host; } catch { return cfg.base || null; } }

// Resolve provider config the same way the Agents app does, with explicit xAI-Grok support.
export async function loadCfg() {
  let j = null;
  try { const r = await fetch('/api/fs/read?path=' + encodeURIComponent(KEY_PATH), { cache: 'no-store' }); if (r.ok) j = await r.json(); } catch {}
  j = j || {};
  const keys = (j.keys && typeof j.keys === 'object') ? j.keys : {};

  // 0) Google Gemini — OpenAI-compat wire but CORS-blocked, so it MUST go through the device /api/llm proxy
  //    (proxy:true). Honour it when it's the ACTIVE provider (the user's chosen brain); a saved-but-not-active
  //    Gemini key is picked up lower down, so a browser-direct provider is preferred when one is also present.
  if (j.provider === 'google' && j.key) return { provider: 'google', base: j.base || 'https://generativelanguage.googleapis.com/v1beta/openai', model: j.model || 'gemini-2.5-flash', key: j.key, proxy: true };

  // 1) Anthropic Claude (Messages API).
  const anth = (keys.anthropic && keys.anthropic.key) ? keys.anthropic
    : (j.provider === 'anthropic' && j.key ? { base: j.base, model: j.model, key: j.key, version: j.version } : null);
  if (anth && anth.key) return { provider: 'anthropic', base: anth.base || 'https://api.anthropic.com', model: anth.model || 'claude-sonnet-4-6', key: anth.key, version: anth.version || '2023-06-01' };

  // 2) xAI Grok — its own slot (keys.xai / keys.grok) or a flat config whose base points at x.ai.
  const xai = (keys.xai && keys.xai.key) ? keys.xai : (keys.grok && keys.grok.key) ? keys.grok
    : (j.key && ((j.provider === 'xai' || j.provider === 'grok') || /x\.ai/i.test(j.base || '')) ? { base: j.base, model: j.model, key: j.key } : null);
  if (xai && xai.key) return { provider: 'openai', base: xai.base || 'https://api.x.ai/v1', model: xai.model || 'grok-2-latest', key: xai.key };

  // 3) Groq / OpenAI / any other OpenAI-compatible endpoint (flat config or keys.groq/keys.openai).
  const oa = (keys.groq && keys.groq.key) ? keys.groq : (keys.openai && keys.openai.key) ? keys.openai
    : (j.key && (!j.provider || j.provider === 'openai' || j.provider === 'groq') ? { base: j.base, model: j.model, key: j.key } : null);
  if (oa && oa.key) return { provider: 'openai', base: oa.base || 'https://api.groq.com/openai/v1', model: oa.model || 'llama-3.1-8b-instant', key: oa.key };

  // 4) Saved Google Gemini key (no stronger browser-direct key configured) — via the device proxy.
  const goog = (keys.google && keys.google.key) ? keys.google : (j.key && /generativelanguage/i.test(j.base || '') ? { base: j.base, model: j.model, key: j.key } : null);
  if (goog && goog.key) return { provider: 'google', base: goog.base || 'https://generativelanguage.googleapis.com/v1beta/openai', model: goog.model || 'gemini-2.5-flash', key: goog.key, proxy: true };

  return null;   // no keys → caller falls back to the local heuristic
}

function authHeaders(cfg) {
  return cfg.provider === 'anthropic'
    ? { 'content-type': 'application/json', 'x-api-key': cfg.key, 'anthropic-version': cfg.version || '2023-06-01', 'anthropic-dangerous-direct-browser-access': 'true' }
    : { 'content-type': 'application/json', 'authorization': 'Bearer ' + cfg.key };
}

// One short completion. Returns text or '' on failure (never throws into the game loop). Logs the exact
// endpoint hit so "is it really Grok?" is answerable from the browser console / network tab.
export async function ask(cfg, system, user, { maxTokens = 300, signal } = {}) {
  if (!cfg) return '';
  try {
    if (cfg.provider === 'anthropic') {
      const base = (cfg.base || 'https://api.anthropic.com').replace(/\/+$/, '');
      console.info('[coach] →', brandOf(cfg), base + '/v1/messages', cfg.model);
      const body = { model: cfg.model, max_tokens: maxTokens, system, messages: [{ role: 'user', content: user }] };
      const resp = await fetch(base + '/v1/messages', { method: 'POST', headers: authHeaders(cfg), body: JSON.stringify(body), signal });
      const j = await resp.json().catch(() => null);
      if (!resp.ok || !j || j.type === 'error') return '';
      return Array.isArray(j.content) ? j.content.filter(b => b && b.type === 'text').map(b => b.text).join('') : '';
    } else {
      const base = (cfg.base || 'https://api.groq.com/openai/v1').replace(/\/+$/, '');
      const ep = base + '/chat/completions';
      const url = cfg.proxy ? '/api/llm?url=' + encodeURIComponent(ep) : ep;   // Gemini (proxy): relay via the device, no CORS
      console.info('[coach] →', brandOf(cfg), url, cfg.model);
      const msgs = [{ role: 'system', content: system }, { role: 'user', content: user }];
      const resp = await fetch(url, { method: 'POST', headers: authHeaders(cfg), body: JSON.stringify({ model: cfg.model, max_tokens: maxTokens, messages: msgs }), signal });
      const j = await resp.json().catch(() => null);
      if (!resp.ok || !j || j.error) return '';
      return (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';
    }
  } catch { return ''; }
}

export function extractJson(text) {
  if (!text) return null;
  const a = text.indexOf('{'), b = text.lastIndexOf('}');
  if (a < 0 || b <= a) return null;
  try { return JSON.parse(text.slice(a, b + 1)); } catch { return null; }
}

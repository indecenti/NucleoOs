// llm.js — the Game Center's LLM brain, on the OS-wide AI decision (/ai-ask.js). The game asks whatever engine every
// NucleoOS surface would use right now: in Private mode only a model on THIS computer plays; a model on the PC
// (Ollama, LM Studio, llama.cpp, Jan) plays with no key at all; otherwise the cloud key from Settings ▸ AI, chosen
// by the shared router. It used to read the key vault itself, with its own fixed provider order and hard-coded
// models, never used a PC model, and called the cloud in Private mode too.
//
// Used by the coach to set an opponent's STYLE between points, and by the mover to pick a turn-based move — never
// to steer a paddle frame by frame (impossible at LLM latency).
//
// PROVIDER TRUTH: the engine chosen at the preflight is PINNED for the whole match, and the label shown in game
// ("Groq (llama-…) @ api.groq.com", "Ollama (qwen3.5:9b) @ localhost:11434") is the brain that really plays —
// Grok (xAI) and Groq are different companies, and the label never conflates them.

let _ask = null, _ai = null;
const askMod = () => _ask || (_ask = import('/ai-ask.js').catch(() => { _ask = null; return null; }));
const aiMod = () => _ai || (_ai = import('/ai.js').catch(() => { _ai = null; return null; }));
const hostOf = (base) => { try { return new URL(base).host; } catch { return base || null; } };

// Why the last loadCfg()/ask() had no brain, in one plain sentence (the OS language) — '' when it worked.
export let lastAiError = '';

// The brain this match would use: { kind: 'pc'|'cloud', brand, model, host } — or null when nothing may play
// (lastAiError says why: no key and no PC model, or Private mode with no PC model running).
export async function loadCfg() {
  lastAiError = '';
  const A = await askMod(), AI = await aiMod();
  if (!A || !AI) return null;
  let plan;
  try { plan = await A.planEngines({ tier: 'fast', probe: true }); } catch { return null; }
  const kind = plan.order[0];
  if (!kind) { lastAiError = AI.explainAiError(new AI.AiError(plan.private ? 'private_no_pc' : 'no_engine', '')); return null; }
  if (kind === 'pc') return { kind, brand: plan.pc.server, model: plan.pc.model, host: hostOf(plan.pc.base) };
  const c = plan.cloud;
  return { kind, provider: c.provider, brand: AI.providerOf(c.provider).label, model: c.model, host: hostOf(c.base) };
}

// Human label "Brand (model)" and the endpoint host — both shown in game, so the player can verify the real brain.
export function brandOf(cfg) { return cfg ? cfg.brand : null; }
export function providerLabel(cfg) { if (!cfg) return null; return cfg.model ? `${cfg.brand} (${cfg.model})` : cfg.brand; }
export function endpointHost(cfg) { return cfg ? cfg.host : null; }

// One short completion on the pinned engine. Returns text, or '' on failure (never throws into the game loop);
// a failure is logged in plain words (what went wrong, how to fix it) and left in lastAiError.
export async function ask(cfg, system, user, opts = {}) {
  if (!cfg) return '';
  lastAiError = '';
  const A = await askMod();
  if (!A) return '';
  try {
    const r = await A.askAI({ system, user, tier: 'fast', maxTokens: opts.maxTokens || 300, signal: opts.signal, pin: cfg.kind });
    if (r.engine && r.engine.model) cfg.model = r.engine.model;   // the label follows the model that really answered
    return r.text || '';
  } catch (e) {
    const AI = await aiMod();
    lastAiError = (AI && AI.explainAiError(e)) || String((e && e.message) || e);
    if (lastAiError) console.warn('[coach]', lastAiError);
    return '';
  }
}

export function extractJson(text) {
  if (!text) return null;
  const a = text.indexOf('{'), b = text.lastIndexOf('}');
  if (a < 0 || b <= a) return null;
  try { return JSON.parse(text.slice(a, b + 1)); } catch { return null; }
}

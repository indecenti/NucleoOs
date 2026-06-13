// preset-engine.js — intelligent, capability-aware AI profiles for NucleoOS.
//
// A "preset" is the ONE thing the owner picks ("make ANIMA fast / private / offline / best"); the engine
// reasons over a live CapabilityModel and translates that intent into the concrete brain knobs scattered
// across the four backends (settings.json on SD, localStorage anima.*, firmware /api/anima/l1, teacher.json).
//
// Design pillars, in order:
//   1. PURE. Like scenes.js / cascade.js, this module does NO I/O: it takes a capability snapshot (built by
//      the caller via probeSignals in index.html) + a registry (from web/shell/ai.js) and returns DATA —
//      the writes/apiCalls to run. The caller runs them one at a time (the Cardputer is single-task). This
//      makes every decision unit-testable in Node with synthetic capability models.
//   2. INTENT IS DURABLE, KNOBS ARE DERIVED. Only ai.preset is persisted to SD (settings-merge DEFAULTS).
//      Everything else is RE-DERIVED from the preset + the current CapabilityModel on each load, so the
//      profile survives a browser wipe without mirroring a dozen volatile keys onto the device.
//   3. SUSTAINABLE FOR THE CARDPUTER. No new device polling; capability probing (WebGPU, pack-cache) is
//      browser-side; the only live device call a preset makes is /api/anima/l1; "Bilanciato" routes through
//      the firmware cascade (mode=on) so the two-bar heap gate still governs RAM under pressure.
//   4. HONEST. Impossible presets are reported (state:'blocked'/'needs-prep') with the exact missing
//      requirement; gaps() names the features the active provider can't serve so they're shown, not hit silently.

import { merge } from './settings-merge.js';

// The ONLY live endpoint a preset drives (in addition to the SD writes). Kept tiny + asserted by the tests,
// so a preset can't silently start driving an endpoint that doesn't exist. (voice/tts are left to the user.)
export const PRESET_ENDPOINTS = ['/api/anima/l1'];

// Where each knob a preset can touch actually LIVES — drives the "what survives a reboot?" scope badges.
export const SCOPE = {
  preset: 'sd',                                            // settings.json ai.preset (durable intent)
  model: 'sd',                                             // teacher.json (paired SD vault)
  l1: 'live',                                              // firmware RAM — resets on reboot
  mode: 'browser', localModel: 'browser', edgeWeb: 'browser', // localStorage — lost on browser switch/wipe
};

// ── CapabilityModel ────────────────────────────────────────────────────────────────────────────────────
// The snapshot the caller builds (probeSignals). Documented here as the engine's input contract.
// { online, enabled, hasKey, provider, model, keys:{anthropic,openai,xai,google}, geminiTier,
//   l1Mode, l1Serving, webgpu, vramMB, deviceMemory, packCached, packUsable, localModelReady, localModelId,
//   tts, exec }
// A defensive default so the engine never throws on a partial snapshot (offline / pre-pairing).
export function defaultSig() {
  return {
    online: false, enabled: true, hasKey: false, provider: 'anthropic', model: '',
    keys: { anthropic: false, openai: false, xai: false, google: false }, geminiTier: '',
    l1Mode: 'auto', l1Serving: false, webgpu: false, vramMB: null, deviceMemory: null,
    packCached: false, packUsable: false, localModelReady: false, localModelId: null, tts: false, exec: 'browser',
  };
}

// Build the registry the engine needs from web/shell/ai.js's single source (PROVIDERS + CAPMATRIX + TIERS).
// Done here (not inline in two places) so browser and tests build it identically.
export function buildRegistry(PROVIDERS, CAPMATRIX, TIERS) {
  const providers = {};
  for (const id of Object.keys(PROVIDERS || {})) {
    const p = PROVIDERS[id];
    providers[id] = { label: p.label, def: p.def, models: p.models || [], caps: (CAPMATRIX && CAPMATRIX[id]) || {}, tiers: (TIERS && TIERS[id]) || {} };
  }
  return { providers };
}

const provOf = (id, reg) => (reg && reg.providers && reg.providers[id]) || null;
const capOf = (id, reg) => { const p = provOf(id, reg); return (p && p.caps) || {}; };
const labelOf = (id, reg) => { const p = provOf(id, reg); return (p && p.label) || id; };
const anyKey = (sig) => !!sig.keys && Object.values(sig.keys).some(Boolean);

// Validate a wanted tier model against the registry; NEVER invent or rewrite — fall back to the provider
// default only if the wanted id is absent (the claude-*-4-x / gemini-2.5-* ids here are the real 2026 ids).
// google.max (Pro) is downgraded to mid (Flash) unless the key's plan is paid.
export function resolveModel(provider, tier, reg, geminiPaid) {
  const p = provOf(provider, reg); if (!p) return null;
  let want = p.tiers[tier];
  if (provider === 'google' && tier === 'max' && !geminiPaid) want = p.tiers.mid;
  const ok = want && (p.models || []).some((m) => m[0] === want);
  return ok ? want : p.def;
}

// Helper: shape a plan result.
const mk = (landedRung, mode, l1, browserLLM, extra = {}) => Object.assign(
  { landedRung, mode, l1, browserLLM, exec: 'browser', teacherModel: null, localModel: null, edgeWeb: null, using: { it: '', en: '' } },
  extra,
);

// ── PRESETS ──────────────────────────────────────────────────────────────────────────────────────────
// Each entry is pure metadata + plan(sig,reg) (resolved knobs) + feasible(sig,reg) ({state,reason}).
// states: 'recommended' (Auto), 'available' (offerable now), 'needs-prep' (offerable, but a download
// unlocks the strong rung), 'blocked' (a hard requirement is missing — only Massima qualità can be).
export const PRESETS = [
  {
    id: 'auto', icon: '✨', it: 'Auto', en: 'Auto',
    intent: { it: 'Sceglie il cervello più forte che è davvero pronto', en: 'Picks the strongest brain that is actually ready' },
    plan(sig, reg) {
      if (sig.online && sig.enabled && sig.hasKey)
        return mk('cloud', 'only', 'auto', false, { using: { it: 'Cloud · ' + labelOf(sig.provider, reg), en: 'Cloud · ' + labelOf(sig.provider, reg) } });
      if (sig.webgpu && (sig.vramMB || 0) >= 900 && sig.localModelReady)
        return mk('gpu', 'local', 'auto', true, { localModel: sig.localModelId || 'auto', using: { it: 'LLM nella GPU del browser', en: 'LLM on the browser GPU' } });
      if (sig.packUsable)
        return mk('wasm', 'edge', 'auto', false, { using: { it: 'Cervello WASM del browser', en: 'Browser WASM brain' } });
      return mk('device', 'on', 'auto', false, { using: { it: 'Cervello del Cardputer', en: 'The Cardputer brain' } });
    },
    feasible() { return { state: 'recommended', reason: { it: '', en: '' } }; },
  },
  {
    id: 'max', icon: '🚀', it: 'Massima qualità', en: 'Max quality',
    intent: { it: 'Cloud al massimo, il Cardputer resta a riposo', en: 'Best cloud, the Cardputer stays idle' },
    plan(sig, reg) {
      const model = resolveModel(sig.provider, 'max', reg, sig.geminiTier === 'paid');
      return mk('cloud', 'only', 'off', true, { teacherModel: model, using: { it: labelOf(sig.provider, reg) + ' · ' + (model || ''), en: labelOf(sig.provider, reg) + ' · ' + (model || '') } });
    },
    feasible(sig) {
      if (!sig.online || !sig.enabled) return { state: 'blocked', reason: { it: 'Serve una connessione a Internet', en: 'Needs an internet connection' } };
      if (!sig.hasKey) return anyKey(sig)
        ? { state: 'blocked', reason: { it: 'Hai una chiave per un altro provider — selezionalo in Avanzate', en: 'You have a key for another provider — pick it in Advanced' } }
        : { state: 'blocked', reason: { it: 'Aggiungi una chiave online per usarlo', en: 'Add an online key to use this' } };
      return { state: 'available', reason: { it: '', en: '' } };
    },
  },
  {
    id: 'balanced', icon: '⚖️', it: 'Bilanciato', en: 'Balanced',
    intent: { it: 'Cloud quando c’è, cervello del device di riserva', en: 'Cloud when available, device brain as fallback' },
    plan(sig, reg) {
      const model = sig.hasKey ? resolveModel(sig.provider, 'mid', reg, sig.geminiTier === 'paid') : null;
      return mk('cloud', 'on', 'auto', false, { teacherModel: model, using: sig.hasKey
        ? { it: labelOf(sig.provider, reg) + ' + L1 di riserva', en: labelOf(sig.provider, reg) + ' + L1 fallback' }
        : { it: 'Nessuna chiave: solo cervello del dispositivo', en: 'No key: device brain only' } });
    },
    feasible() { return { state: 'available', reason: { it: '', en: '' } }; },
  },
  {
    id: 'local', icon: '💻', it: 'Solo locale', en: 'Local only',
    intent: { it: 'Tutto nel browser, il Cardputer può dormire', en: 'All in the browser, the Cardputer can sleep' },
    plan(sig) {
      if (sig.webgpu && (sig.vramMB || 0) >= 900 && sig.localModelReady)
        return mk('gpu', 'local', 'auto', true, { localModel: sig.localModelId || 'auto', using: { it: 'LLM nella GPU del browser', en: 'LLM on the browser GPU' } });
      if (sig.packUsable)
        return mk('wasm', 'edge', 'auto', false, { using: { it: 'Cervello WASM del browser', en: 'Browser WASM brain' } });
      return mk('wasm', 'edge', 'auto', false, { using: { it: 'Da preparare per funzionare offline', en: 'Needs preparing to run offline' } });
    },
    feasible(sig) {
      if (sig.localModelReady || sig.packUsable) return { state: 'available', reason: { it: '', en: '' } };
      return { state: 'needs-prep', reason: { it: 'Scarica il cervello (~14 MB) per funzionare offline nel browser', en: 'Download the brain (~14 MB) to run offline in the browser' } };
    },
  },
  {
    id: 'private', icon: '🔒', it: 'Privacy', en: 'Privacy',
    intent: { it: 'Niente esce dal dispositivo, nemmeno il web', en: 'Nothing leaves the device, not even the web' },
    plan(sig) {
      const mode = sig.packUsable ? 'edge' : 'off';
      return mk(sig.packUsable ? 'wasm' : 'device', mode, 'on', false, { edgeWeb: 0, using: { it: 'Air-gapped · nessuna uscita di rete', en: 'Air-gapped · no network egress' } });
    },
    feasible() { return { state: 'available', reason: { it: '', en: '' } }; },
  },
];

export const PRESET_IDS = PRESETS.map((p) => p.id);
const find = (id) => PRESETS.find((p) => p.id === id) || null;

export function planPreset(id, sig, reg) { const p = find(id); return p ? p.plan(sig || defaultSig(), reg || { providers: {} }) : null; }
export function feasibility(id, sig, reg) { const p = find(id); return p ? p.feasible(sig || defaultSig(), reg || { providers: {} }) : null; }

// ── gaps + which-apps ────────────────────────────────────────────────────────────────────────────────
// What the ACTIVE chat provider cannot do that a NucleoOS feature needs, and which secondary key would fix it.
// (image=xAI grok-2-image, whisper=Groq Whisper; IR's NL skill reads the active provider only — line 607 —
// so a secondary key does NOT help it today: that's flagged as a warn, repaired in a follow-up.)
export function gaps(sig, reg) {
  const cap = capOf(sig.provider, reg), out = [];
  if (!cap.image && !(sig.keys && sig.keys.xai)) out.push({ feature: 'image', app: 'Paint · Atelier', it: 'Le immagini IA richiedono una chiave Grok (xAI)', en: 'AI images need an xAI (Grok) key', fixKey: 'xai' });
  if (!cap.whisper && !(sig.keys && sig.keys.openai)) out.push({ feature: 'whisper', app: 'Recorder · Dettatura', it: 'La trascrizione richiede una chiave Groq (Whisper)', en: 'Transcription needs a Groq (Whisper) key', fixKey: 'openai' });
  if (!cap.ir) out.push({ feature: 'ir', app: 'Telecomando IR', it: 'La skill in linguaggio naturale funziona con Groq o Gemini', en: 'The natural-language skill works with Groq or Gemini', fixKey: null });
  return out;
}

// Central teacher.json consumers, for the "su quali app agisce" panel. need → the capability they require.
export const APP_MAP = [
  { id: 'chat', it: 'Chat ANIMA · Copilota · Agenti · Giochi', en: 'ANIMA chat · Copilot · Agents · Games', need: 'chat' },
  { id: 'image', it: 'Paint · Atelier', en: 'Paint · Atelier', need: 'image' },
  { id: 'whisper', it: 'Recorder · Dettatura', en: 'Recorder · Dictation', need: 'whisper' },
  { id: 'ir', it: 'Telecomando IR', en: 'IR Remote', need: 'ir' },
];
export function appStatus(app, sig, reg) {
  const cap = capOf(sig.provider, reg);
  if (app.need === 'chat') return { ok: true };
  if (app.need === 'image') return (cap.image || (sig.keys && sig.keys.xai)) ? { ok: true } : { ok: false, it: 'richiede una chiave Grok (xAI)', en: 'needs an xAI (Grok) key' };
  if (app.need === 'whisper') return (cap.whisper || (sig.keys && sig.keys.openai)) ? { ok: true } : { ok: false, it: 'richiede una chiave Groq (Whisper)', en: 'needs a Groq (Whisper) key' };
  if (app.need === 'ir') return cap.ir ? { ok: true } : { ok: false, it: 'NL solo con Groq o Gemini', en: 'NL only with Groq or Gemini' };
  return { ok: true };
}

// ── reconciler ─────────────────────────────────────────────────────────────────────────────────────────
// Twin of applyScene: returns DATA only (never touches the network). The caller writes settings.json,
// then localStorage anima.*, then the optional teacher.json model, then runs apiCalls SEQUENTIALLY.
export function applyPreset(model, id, sig, reg) {
  const preset = find(id); if (!preset) return null;
  sig = sig || defaultSig(); reg = reg || { providers: {} };
  const p = preset.plan(sig, reg);
  const feas = preset.feasible(sig, reg);
  const patch = { ai: { preset: id } };
  const local = { 'anima.mode': p.mode, 'anima.modeSet': '1' };
  if (p.localModel) local['anima.localModel'] = p.localModel;
  if (p.edgeWeb !== null && p.edgeWeb !== undefined) local['anima.edgeWeb'] = String(p.edgeWeb);
  return {
    preset, id,
    state: feas.state, reason: feas.reason, landedRung: p.landedRung, using: p.using,
    gaps: gaps(sig, reg),
    patch,
    model: merge(model, patch),
    local,
    teacherModel: p.teacherModel || null,                          // caller writes only this field to teacher.json
    apiCalls: [{ path: '/api/anima/l1', method: 'POST', body: { mode: p.l1, browserLLM: !!p.browserLLM } }],
  };
}

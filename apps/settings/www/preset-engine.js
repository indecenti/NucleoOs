/* SPDX-License-Identifier: AGPL-3.0-or-later */
// preset-engine.js — intelligent, capability-aware AI profiles for NucleoOS.
//
// A "preset" is the ONE thing the owner picks ("make ANIMA fast / private / offline / best"); the engine
// reasons over a live CapabilityModel and translates that intent into the concrete brain knobs scattered
// across the four backends (settings.json on SD, localStorage anima.*, firmware /api/anima/l1, teacher.json).
//
// Design pillars, in order:
//   1. PURE. Like scenes.js / cascade.js, this module does NO I/O: it takes a capability snapshot (built by
//      the caller via probeSignals in index.html) + a registry (from web/shell/ai.js) and returns DATA —
//      the writes/apiCalls to run. The caller runs them one at a time (the device is single-task). This
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
//   tts, exec, pcModel, pcServer, pcFirst, pcState, pcLoopback, deviceBrain, ir }
// pc*: a model server on THIS computer (/ai-engines.js) — pcModel = the chat model it would answer with ('' = none),
// pcFirst = the user put the PC before the cloud (ai.local.engines.prefer), pcState = 'ok' | 'refused' | 'none',
// pcLoopback = that server is on localhost — the only kind Private mode (Local only, Privacy) may use.
// deviceBrain = ANIMA's engine runs on the device (the ESP32 firmwares); false = the device answers only its
// own data and commands (the Handheld's server). ir = the device has the IR remote (Cardputer).
// A defensive default so the engine never throws on a partial snapshot (offline / pre-pairing).
export function defaultSig() {
  return {
    online: false, enabled: true, hasKey: false, provider: 'anthropic', model: '',
    keys: { anthropic: false, openai: false, xai: false, google: false }, geminiTier: '',
    l1Mode: 'auto', l1Serving: false, webgpu: false, vramMB: null, deviceMemory: null,
    packCached: false, packUsable: false, localModelReady: false, localModelId: null, tts: false, exec: 'browser',
    pcModel: '', pcServer: '', pcFirst: false, pcState: 'none', pcLoopback: false,
    deviceBrain: true, ir: false,
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

// Labels are { it, en, es, fr, de } objects, rendered by the caller with L(o) = o[lang] || o.en || o.it.
// same(): a label that reads identically in every language (provider names, model ids, '' = none).
const same = (s) => ({ it: s, en: s, es: s, fr: s, de: s });

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
  { landedRung, mode, l1, browserLLM, exec: 'browser', teacherModel: null, localModel: null, edgeWeb: null, using: same('') },
  extra,
);

const GPU_LLM = () => ({ it: 'LLM nella GPU del browser', en: 'LLM on the browser GPU', es: 'LLM en la GPU del navegador', fr: 'LLM sur le GPU du navigateur', de: 'LLM auf der Browser-GPU' });
const PC_LLM = (sig) => { const m = (sig.pcServer ? sig.pcServer + ' · ' : '') + sig.pcModel; return { it: 'Sul PC · ' + m, en: 'On the PC · ' + m, es: 'En el PC · ' + m, fr: 'Sur le PC · ' + m, de: 'Auf dem PC · ' + m }; };
// The last rung: the device's own brain where it has one, else its own data and commands only.
const DEVICE_RUNG = (sig) => sig.deviceBrain === false
  ? { it: 'Solo comandi e dati del dispositivo', en: 'Device commands and data only', es: 'Solo órdenes y datos del dispositivo', fr: 'Seulement les commandes et données de l’appareil', de: 'Nur Befehle und Daten des Geräts' }
  : { it: 'Cervello del dispositivo', en: 'The device brain', es: 'El cerebro del dispositivo', fr: 'Le cerveau de l’appareil', de: 'Das Gerätegehirn' };
const WASM_BRAIN = () => ({ it: 'Cervello WASM del browser', en: 'Browser WASM brain', es: 'Cerebro WASM del navegador', fr: 'Cerveau WASM du navigateur', de: 'WASM-Gehirn im Browser' });

// ── PRESETS ──────────────────────────────────────────────────────────────────────────────────────────
// Each entry is pure metadata + plan(sig,reg) (resolved knobs) + feasible(sig,reg) ({state,reason}).
// states: 'recommended' (Auto), 'available' (offerable now), 'needs-prep' (offerable, but a download
// unlocks the strong rung), 'blocked' (a hard requirement is missing — only Massima qualità can be).
export const PRESETS = [
  {
    id: 'auto', icon: '✨', it: 'Auto', en: 'Auto', es: 'Auto', fr: 'Auto', de: 'Auto',
    intent: { it: 'Sceglie il cervello più forte che è davvero pronto', en: 'Picks the strongest brain that is actually ready', es: 'Elige el cerebro más potente que esté realmente listo', fr: 'Choisit le cerveau le plus puissant réellement prêt', de: 'Wählt das stärkste Gehirn, das wirklich bereit ist' },
    plan(sig, reg) {
      const cloudOk = sig.online && sig.enabled && sig.hasKey;
      // ANIMA's own order (docs/anima-engines.md): the PC's model answers first with no usable cloud key, or when
      // the user put the PC first; otherwise it is the cloud's fallback.
      if (sig.pcModel && (sig.pcFirst || !cloudOk))
        return mk('pc', 'pc', 'auto', false, { using: PC_LLM(sig) });
      if (cloudOk)
        return mk('cloud', 'only', 'auto', false, { using: same('Cloud · ' + labelOf(sig.provider, reg)) });
      if (sig.webgpu && (sig.vramMB || 0) >= 900 && sig.localModelReady)
        return mk('gpu', 'local', 'auto', true, { localModel: sig.localModelId || 'auto', using: GPU_LLM() });
      if (sig.packUsable)
        return mk('wasm', 'edge', 'auto', false, { using: WASM_BRAIN() });
      return mk('device', 'on', 'auto', false, { using: DEVICE_RUNG(sig) });
    },
    feasible() { return { state: 'recommended', reason: same('') }; },
  },
  {
    id: 'max', icon: '🚀', it: 'Massima qualità', en: 'Max quality', es: 'Máxima calidad', fr: 'Qualité maximale', de: 'Maximale Qualität',
    intent: { it: 'Cloud al massimo, il dispositivo resta a riposo', en: 'Best cloud, the device stays idle', es: 'La mejor nube, el dispositivo en reposo', fr: 'Le meilleur cloud, l’appareil reste au repos', de: 'Beste Cloud, das Gerät bleibt im Leerlauf' },
    plan(sig, reg) {
      const model = resolveModel(sig.provider, 'max', reg, sig.geminiTier === 'paid');
      return mk('cloud', 'only', 'off', true, { teacherModel: model, using: same(labelOf(sig.provider, reg) + ' · ' + (model || '')) });
    },
    feasible(sig) {
      if (!sig.online || !sig.enabled) return { state: 'blocked', reason: { it: 'Serve una connessione a Internet', en: 'Needs an internet connection', es: 'Necesita conexión a Internet', fr: 'Nécessite une connexion Internet', de: 'Benötigt eine Internetverbindung' } };
      if (!sig.hasKey) return anyKey(sig)
        ? { state: 'blocked', reason: { it: 'Hai una chiave per un altro provider — selezionalo in Avanzate', en: 'You have a key for another provider — pick it in Advanced', es: 'Tienes una clave de otro proveedor — elígelo en Avanzado', fr: 'Vous avez une clé pour un autre fournisseur — choisissez-le dans Avancé', de: 'Du hast einen Schlüssel für einen anderen Anbieter — wähle ihn unter Erweitert' } }
        : { state: 'blocked', reason: { it: 'Aggiungi una chiave online per usarlo', en: 'Add an online key to use this', es: 'Añade una clave online para usarlo', fr: 'Ajoutez une clé en ligne pour l’utiliser', de: 'Füge einen Online-Schlüssel hinzu, um dies zu nutzen' } };
      return { state: 'available', reason: same('') };
    },
  },
  {
    id: 'balanced', icon: '⚖️', it: 'Bilanciato', en: 'Balanced', es: 'Equilibrado', fr: 'Équilibré', de: 'Ausgewogen',
    intent: { it: 'Cloud quando c’è, il dispositivo di riserva', en: 'Cloud when available, the device as fallback', es: 'Nube cuando hay, el dispositivo como reserva', fr: 'Le cloud quand il est là, l’appareil en secours', de: 'Cloud, wenn verfügbar, das Gerät als Reserve' },
    plan(sig, reg) {
      const model = sig.hasKey ? resolveModel(sig.provider, 'mid', reg, sig.geminiTier === 'paid') : null;
      return mk('cloud', 'on', 'auto', false, { teacherModel: model, using: sig.hasKey
        ? (sig.deviceBrain === false
          ? { it: labelOf(sig.provider, reg) + ' + comandi del dispositivo', en: labelOf(sig.provider, reg) + ' + device commands', es: labelOf(sig.provider, reg) + ' + órdenes del dispositivo', fr: labelOf(sig.provider, reg) + ' + commandes de l’appareil', de: labelOf(sig.provider, reg) + ' + Gerätebefehle' }
          : { it: labelOf(sig.provider, reg) + ' + L1 di riserva', en: labelOf(sig.provider, reg) + ' + L1 fallback', es: labelOf(sig.provider, reg) + ' + L1 de reserva', fr: labelOf(sig.provider, reg) + ' + L1 en secours', de: labelOf(sig.provider, reg) + ' + L1 als Reserve' })
        : (sig.deviceBrain === false
          ? { it: 'Nessuna chiave: solo comandi e dati del dispositivo', en: 'No key: device commands and data only', es: 'Sin clave: solo órdenes y datos del dispositivo', fr: 'Pas de clé : seulement les commandes et données de l’appareil', de: 'Kein Schlüssel: nur Befehle und Daten des Geräts' }
          : { it: 'Nessuna chiave: solo cervello del dispositivo', en: 'No key: device brain only', es: 'Sin clave: solo el cerebro del dispositivo', fr: 'Pas de clé : cerveau de l’appareil uniquement', de: 'Kein Schlüssel: nur das Gerätegehirn' }) });
    },
    feasible() { return { state: 'available', reason: same('') }; },
  },
  {
    id: 'local', icon: '💻', it: 'Solo locale', en: 'Local only', es: 'Solo local', fr: 'Local uniquement', de: 'Nur lokal',
    intent: { it: 'Tutto su questo computer, il dispositivo può dormire', en: 'Everything on this computer, the device can sleep', es: 'Todo en este equipo, el dispositivo puede dormir', fr: 'Tout sur cet ordinateur, l’appareil peut dormir', de: 'Alles auf diesem Computer, das Gerät kann schlafen' },
    plan(sig) {
      if (sig.pcModel && sig.pcLoopback) return mk('pc', 'pc', 'auto', false, { using: PC_LLM(sig) });
      if (sig.webgpu && (sig.vramMB || 0) >= 900 && sig.localModelReady)
        return mk('gpu', 'local', 'auto', true, { localModel: sig.localModelId || 'auto', using: GPU_LLM() });
      if (sig.packUsable)
        return mk('wasm', 'edge', 'auto', false, { using: WASM_BRAIN() });
      return mk('wasm', 'edge', 'auto', false, { using: { it: 'Da preparare per funzionare offline', en: 'Needs preparing to run offline', es: 'Hay que prepararlo para funcionar offline', fr: 'À préparer pour fonctionner hors ligne', de: 'Muss für den Offline-Betrieb vorbereitet werden' } });
    },
    feasible(sig) {
      if ((sig.pcModel && sig.pcLoopback) || sig.localModelReady || sig.packUsable) return { state: 'available', reason: same('') };
      return { state: 'needs-prep', reason: { it: 'Scarica il cervello (~88 MB) per funzionare offline nel browser', en: 'Download the brain (~88 MB) to run offline in the browser', es: 'Descarga el cerebro (~88 MB) para funcionar offline en el navegador', fr: 'Téléchargez le cerveau (~88 MB) pour fonctionner hors ligne dans le navigateur', de: 'Lade das Gehirn (~88 MB) herunter, um offline im Browser zu laufen' } };
    },
  },
  {
    id: 'private', icon: '🔒', it: 'Privacy', en: 'Privacy', es: 'Privacidad', fr: 'Confidentialité', de: 'Privatsphäre',
    intent: { it: 'Niente esce dal dispositivo, nemmeno il web', en: 'Nothing leaves the device, not even the web', es: 'Nada sale del dispositivo, ni siquiera la web', fr: 'Rien ne quitte l’appareil, pas même le web', de: 'Nichts verlässt das Gerät, nicht einmal das Web' },
    plan(sig) {
      if (sig.pcModel && sig.pcLoopback) return mk('pc', 'pc', 'on', false, { edgeWeb: 0, using: { it: 'Sul PC · ' + sig.pcModel + ' · niente esce in Internet', en: 'On the PC · ' + sig.pcModel + ' · nothing goes to the Internet', es: 'En el PC · ' + sig.pcModel + ' · nada sale a Internet', fr: 'Sur le PC · ' + sig.pcModel + ' · rien ne part sur Internet', de: 'Auf dem PC · ' + sig.pcModel + ' · nichts geht ins Internet' } });
      const mode = sig.packUsable ? 'edge' : 'off';
      return mk(sig.packUsable ? 'wasm' : 'device', mode, 'on', false, { edgeWeb: 0, using: { it: 'Air-gapped · nessuna uscita di rete', en: 'Air-gapped · no network egress', es: 'Air-gapped · sin salida de red', fr: 'Air-gapped · aucune sortie réseau', de: 'Air-gapped · kein ausgehender Netzverkehr' } });
    },
    feasible() { return { state: 'available', reason: same('') }; },
  },
];

export const PRESET_IDS = PRESETS.map((p) => p.id);
// The ANIMA mode each preset sets (see applyPreset): only these two keep everything on this machine.
export const PRESET_MODE = Object.freeze({ private: 'private', local: 'private' });
const find = (id) => PRESETS.find((p) => p.id === id) || null;

export function planPreset(id, sig, reg) { const p = find(id); return p ? p.plan(sig || defaultSig(), reg || { providers: {} }) : null; }
export function feasibility(id, sig, reg) { const p = find(id); return p ? p.feasible(sig || defaultSig(), reg || { providers: {} }) : null; }

// ── gaps + which-apps ────────────────────────────────────────────────────────────────────────────────
// What the ACTIVE chat provider cannot do that a NucleoOS feature needs, and which secondary key would fix it.
// (image=xAI grok-2-image, whisper=Groq Whisper; IR's NL skill reads the active provider only — line 607 —
// so a secondary key does NOT help it today: that's flagged as a warn, repaired in a follow-up.)
export function gaps(sig, reg) {
  const cap = capOf(sig.provider, reg), out = [];
  if (!cap.image && !(sig.keys && sig.keys.xai)) out.push({ feature: 'image', app: 'Paint · Atelier', it: 'Le immagini IA richiedono una chiave Grok (xAI)', en: 'AI images need an xAI (Grok) key', es: 'Las imágenes IA necesitan una clave de xAI (Grok)', fr: 'Les images IA nécessitent une clé xAI (Grok)', de: 'KI-Bilder brauchen einen xAI-Schlüssel (Grok)', fixKey: 'xai' });
  if (!cap.whisper && !(sig.keys && sig.keys.openai)) out.push({ feature: 'whisper', app: 'Recorder · Dettatura', it: 'La trascrizione richiede una chiave Groq (Whisper)', en: 'Transcription needs a Groq (Whisper) key', es: 'La transcripción necesita una clave de Groq (Whisper)', fr: 'La transcription nécessite une clé Groq (Whisper)', de: 'Die Transkription braucht einen Groq-Schlüssel (Whisper)', fixKey: 'openai' });
  if (!cap.ir && sig.ir) out.push({ feature: 'ir', app: 'Telecomando IR', it: 'La skill in linguaggio naturale funziona con Groq o Gemini', en: 'The natural-language skill works with Groq or Gemini', es: 'La habilidad en lenguaje natural funciona con Groq o Gemini', fr: 'La compétence en langage naturel fonctionne avec Groq ou Gemini', de: 'Der Skill in natürlicher Sprache funktioniert mit Groq oder Gemini', fixKey: null });
  return out;
}

// Central teacher.json consumers, for the "su quali app agisce" panel. need → the capability they require.
export const APP_MAP = [
  { id: 'chat', it: 'Chat ANIMA · Copilota · Agenti · Giochi', en: 'ANIMA chat · Copilot · Agents · Games', es: 'Chat ANIMA · Copiloto · Agentes · Juegos', fr: 'Chat ANIMA · Copilote · Agents · Jeux', de: 'ANIMA-Chat · Copilot · Agenten · Spiele', need: 'chat' },
  { id: 'image', it: 'Paint · Atelier', en: 'Paint · Atelier', es: 'Paint · Atelier', fr: 'Paint · Atelier', de: 'Paint · Atelier', need: 'image' },
  { id: 'whisper', it: 'Recorder · Dettatura', en: 'Recorder · Dictation', es: 'Grabadora · Dictado', fr: 'Enregistreur · Dictée', de: 'Rekorder · Diktat', need: 'whisper' },
  { id: 'ir', it: 'Telecomando IR', en: 'IR Remote', es: 'Mando IR', fr: 'Télécommande IR', de: 'IR-Fernbedienung', need: 'ir' },
];
export function appStatus(app, sig, reg) {
  const cap = capOf(sig.provider, reg);
  if (app.need === 'chat') return { ok: true };
  if (app.need === 'image') return (cap.image || (sig.keys && sig.keys.xai)) ? { ok: true } : { ok: false, it: 'richiede una chiave Grok (xAI)', en: 'needs an xAI (Grok) key', es: 'necesita una clave de xAI (Grok)', fr: 'nécessite une clé xAI (Grok)', de: 'braucht einen xAI-Schlüssel (Grok)' };
  if (app.need === 'whisper') return (cap.whisper || (sig.keys && sig.keys.openai)) ? { ok: true } : { ok: false, it: 'richiede una chiave Groq (Whisper)', en: 'needs a Groq (Whisper) key', es: 'necesita una clave de Groq (Whisper)', fr: 'nécessite une clé Groq (Whisper)', de: 'braucht einen Groq-Schlüssel (Whisper)' };
  if (app.need === 'ir') return cap.ir ? { ok: true } : { ok: false, it: 'NL solo con Groq o Gemini', en: 'NL only with Groq or Gemini', es: 'LN solo con Groq o Gemini', fr: 'LN uniquement avec Groq ou Gemini', de: 'NL nur mit Groq oder Gemini' };
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
  // anima.mode is the OS-wide TWO-mode switch (web/shell/anima-mode.js: auto | private) that ANIMA, the copilot
  // and Agenti read. p.mode still names the rung the preset lands on (only/local/edge/on/off — verifyNow and the
  // summary use it), but writing that rung as the mode turned "Auto" with only the browser pack ready into
  // Private ('edge' → private: no cloud, no web). Private = nothing leaves this machine: the Privacy preset and
  // Local only (everything in the browser / on this PC). Every other preset is Auto.
  const local = { 'anima.mode': PRESET_MODE[id] || 'auto', 'anima.modeSet': '1' };
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

// router.js — the two-stage substrate router for "one ANIMA, four substrates". Stage A is a free,
// deterministic pre-router (a pure function of the text + client capabilities) mirroring the
// device's own nucleo_anima_query dispatch order: slash → file-op → code → knowledge. Stage B (the
// tiny orchestrator LLM) only runs for the ambiguous long tail and is driven elsewhere. Also owns
// the VRAM-aware model plan so `auto` never attempts two-model residency where it cannot fit.
// Pure & DOM-free → host-testable.

import { parseFileIntent } from '../nlfs.js';

// caps: { webgpu, online, coderCached, orchestratorCached, vramMB }
export function prerouteStageA(text, caps = {}) {
  const raw = String(text || '').trim();
  if (!raw) return null;
  if (raw[0] === '/') return { substrate: 'local', kind: 'slash', reason: 'slash-command' };

  const intent = parseFileIntent(raw);
  if (intent) return { substrate: 'hands', kind: 'file', intent, reason: 'deterministic-file-op' };

  // Translation request — mirrors the firmware nucleo_anima_translate detector. The device's grounded
  // dictionary is the floor (always reachable, zero-hallucination), so even offline this routes to a real
  // skill; a local LLM / cloud teacher escalate for phrases the dictionary doesn't cover (the client's
  // viaTranslate does the dictionary-first ladder). target = the language to translate INTO ('en'|'it'|null).
  if (isTranslateRequest(raw)) {
    const target = translateTarget(raw);
    if (caps.webgpu && caps.coderCached) return { substrate: 'M4-local', kind: 'translate', target, reason: 'local-llm-can-translate' };
    if (caps.online) return { substrate: 'M2', kind: 'translate', target, reason: 'device-dictionary-then-cloud' };
    return { substrate: 'M1', kind: 'translate', target, reason: 'device-dictionary-offline' };
  }

  if (isCodeRequest(raw)) {
    if (caps.webgpu && caps.coderCached) return { substrate: 'M4-local', kind: 'code', reason: 'local-coder-available' };
    if (caps.online) return { substrate: 'M3', kind: 'code', reason: 'grok-online-fallback' };
    return { substrate: 'M1', kind: 'code', reason: 'device-template-or-decline' };
  }
  // knowledge / math / live — the grounded device brain answers first, never the LLM.
  return { substrate: caps.online ? 'M2' : 'M1', kind: 'knowledge', reason: 'device-grounded-first' };
}

const LANG = /\b(javascript|js|typescript|ts|jsx|tsx|react|node(?:js)?|html|css)\b/i;
const GEN  = /\b(scrivi|scrivimi|scriva|genera|generami|crea|creami|implementa|implementami|fammi|mostrami|refactor(?:a|izza)?|sistema|correggi|debugga|write|generate|create|implement|build|make|refactor|fix|debug)\b/i;
const CODE = /\b(codice|funzione|funzioni|metodo|script|snippet|programma|programmino|algoritmo|classe|componente|code|function|method|class|component|algorithm|program|snippet)\b/i;

// Token-exact-ish code-request detector (mirrors the firmware a_is_code_request philosophy):
// a generation verb together with a code noun OR a language name. "cos'è javascript" (no gen verb)
// stays a knowledge query.
export function isCodeRequest(text) {
  const n = String(text || '').toLowerCase();
  if (!GEN.test(n)) return false;
  return CODE.test(n) || LANG.test(n);
}

// Translation detector — host mirror of nucleo_anima_translate.c's trigger. Strong (zero-false-positive):
// an explicit translate VERB (traduci/traduce/tradurre/translate…), or the FRAME "come si dice" / "how do
// you say", or the NOUN traduzione/translation WITH a named language. NB "traduzione/translation" alone is
// NOT a verb (no c/r after "tradu"), so "cos'è una traduzione" stays a knowledge query.
const TR_LANG = /\b(ingles\w*|english|italian\w*|italiano)\b/i;
export function isTranslateRequest(text) {
  const n = String(text || '').toLowerCase();
  if (/\btradu(?:c|r)\w*/.test(n)) return true;            // traduci/traduce/traduco/tradurre/traducimi…
  if (/\btranslat\w*/.test(n)) return true;                // translate/translating/translated
  if (/\bcome si (?:dice|dicono|traduce)\b/.test(n)) return true;
  if (/\bhow (?:do you|to|do i) say\b/.test(n)) return true;
  if (/\b(traduzione|translation)\b/.test(n) && TR_LANG.test(n)) return true;
  return false;
}

// The language to translate INTO: 'en' (English) / 'it' (Italian) / null (auto-detect from the word).
export function translateTarget(text) {
  const n = String(text || '').toLowerCase();
  if (/\b(ingles\w*|english)\b/.test(n)) return 'en';
  if (/\b(italian\w*|italiano)\b/.test(n)) return 'it';
  return null;
}

// VRAM-aware plan (must-fix: two-model residency does NOT fit ≤4 GB iGPUs → thrash).
// Returns the residency mode `auto` should pick BY MEASUREMENT, not by mere availability.
export function modelPlan(caps = {}) {
  if (!caps.webgpu) return { mode: 'none', orchestrator: false, coderResident: false, reason: 'no-webgpu' };
  const vram = caps.vramMB || 0;
  if (vram >= 6000) return { mode: 'two-model', orchestrator: true, coderResident: true, reason: 'discrete-gpu-≥6gb' };
  if (vram >= 3000) return { mode: 'two-model-paged', orchestrator: true, coderResident: false, reason: 'coder-lazy-paged' };
  // ≤~3 GB usable: drop the orchestrator; deterministic pre-router + grammar-constrained coder
  // emits BOTH route and code, so the single resident model is the coder.
  return { mode: 'single-model', orchestrator: false, coderResident: true, reason: 'single-model-degraded' };
}

// Whether the orchestrator LLM (Stage B) should be consulted for this turn.
export function needsOrchestrator(stageA, caps = {}) {
  if (!stageA) return modelPlan(caps).orchestrator;   // genuinely ambiguous → only if a planner exists
  return false;                                        // Stage A decided deterministically
}

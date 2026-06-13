// sheet-router.js — the offline-FIRST substrate router for "one ANIMA, four substrates" inside the
// spreadsheet. It answers a single question per turn: given the dial, the device/browser
// capabilities, and whether the deterministic parser already understood the request, WHICH substrate
// should run? The ladder is deliberately offline-first: the zero-model deterministic parser
// (localIntent) is the FLOOR and always wins when it understands; a learned offline recipe replays
// next; only genuinely novel/ambiguous turns escalate, and even then they prefer the most local
// option available (browser GPU → cloud → device hybrid → device offline). Pure & DOM-free →
// host-testable. Mirrors forge/router.js prerouteStageA + modelPlan but with sheet semantics.

// caps: { webgpu:bool, coderReady:bool, orchReady:bool, online:bool, vramMB:number }
//   webgpu      — navigator.gpu present & an adapter was obtained
//   coderReady  — the Qwen2.5-Coder weights are cached/loadable in the browser
//   orchReady   — the orchestrator (Llama-3.2-1B / Qwen2.5-0.5B) weights are cached/loadable
//   online      — a teacher/Grok key is present (cloud reachable)
// SUBSTRATES (envelope-compatible + two sheet-only routes):
//   'det'       — deterministic localIntent executor (no model, instant, offline)   [FLOOR]
//   'recipe'    — a previously-verified plan replayed from the offline cache (no model, offline)
//   'M4-plan'   — browser orchestrator emits a grammar-constrained typed PLAN (offline, GPU)
//   'M4-code'   — browser coder writes a sandboxed JS transform (offline, GPU)
//   'M3'        — cloud Grok skill (verified formula)
//   'M2'        — device hybrid brain (/api/anima?mode=on)
//   'M1'        — device offline brain (/api/anima?mode=off, =ANIMA knowledge)

export const DIAL = ['auto', 'off', 'on', 'only', 'local'];

// Does this request smell like it needs a custom computed transform (beyond the fixed verbs)?
// e.g. "group by region and rank", "year-over-year growth", "pivot", "normalise then z-score".
const CODE_CUE = /\b(raggrupp\w*|group\s*by|pivot|classific\w*|rank\w*|punteggi\w*|score\b|normalizz\w*|z-?score|crescit\w*|growth|year[- ]over[- ]year|yoy|variazion\w*|trasform\w*\s+(?:custom|personalizz)|regression\w*|interpol\w*|per ogni gruppo|for each group|cumulat\w*|running total|percentile|quantile)\b/i;

export function looksLikeCodeTransform(text) {
  return CODE_CUE.test(String(text || ''));
}

// modelPlan(caps) → VRAM-aware residency for `local`. ≤~3GB usable iGPU can't hold orchestrator+coder
// at once, so it runs single-model: deterministic pre-route + grammar-constrained planner only (no
// coder unless re-paged). Mirrors forge modelPlan thresholds.
export function modelPlan(caps = {}) {
  if (!caps.webgpu) return { mode: 'none', orchestrator: false, coder: false, reason: 'no-webgpu' };
  const vram = caps.vramMB || 0;
  if (vram >= 6000) return { mode: 'two-model', orchestrator: true, coder: true, reason: 'discrete-gpu-≥6gb' };
  if (vram >= 3000) return { mode: 'two-model-paged', orchestrator: true, coder: false, reason: 'coder-lazy-paged' };
  return { mode: 'single-model', orchestrator: true, coder: false, reason: 'single-model-degraded' };
}

// The escalation order for an ambiguous turn, most-LOCAL first. Returns the FIRST available substrate.
function escalate(text, caps) {
  const wantsCode = looksLikeCodeTransform(text);
  // browser GPU first (offline + private)
  if (caps.webgpu) {
    if (wantsCode && caps.coderReady) return { substrate: 'M4-code', reason: 'local-coder-for-custom-transform' };
    if (caps.orchReady) return { substrate: 'M4-plan', reason: 'local-orchestrator-plan' };
    if (caps.coderReady) return { substrate: 'M4-code', reason: 'local-coder-fallback' };
  }
  if (caps.online) return { substrate: 'M3', reason: 'cloud-grok-verified' };
  // device brain last (it answers knowledge well but isn't a planner)
  return { substrate: caps.online ? 'M2' : 'M1', reason: 'device-grounded' };
}

// route(turn) → { substrate, reason }.
// turn: { text, dial:'auto'|'off'|'on'|'only'|'local', caps, localHit:bool, recipeHit:bool }
//   localHit  — localIntent(text) returned a non-null deterministic intent
//   recipeHit — the offline recipe cache holds a verified plan for this (normalised) query
export function route(turn = {}) {
  const { text = '', dial = 'auto', caps = {}, localHit = false, recipeHit = false } = turn;

  // Explicit dial values pin the substrate (offline-first stays honest: 'off' never reaches the net).
  switch (dial) {
    case 'off':   return localHit ? { substrate: 'det', reason: 'dial-off-deterministic' }
                                  : (recipeHit ? { substrate: 'recipe', reason: 'dial-off-recipe' }
                                  : (caps.webgpu && (caps.orchReady || caps.coderReady)
                                      ? escalate(text, { ...caps, online: false })            // GPU is still offline
                                      : { substrate: 'M1', reason: 'dial-off-device' }));
    case 'on':    return localHit ? { substrate: 'det', reason: 'dial-on-deterministic' } : { substrate: 'M2', reason: 'dial-on-hybrid' };
    case 'only':  return { substrate: 'M3', reason: 'dial-only-cloud' };
    case 'local': return looksLikeCodeTransform(text) && caps.coderReady
                    ? { substrate: 'M4-code', reason: 'dial-local-coder' }
                    : { substrate: caps.orchReady ? 'M4-plan' : 'M4-code', reason: 'dial-local-gpu' };
    default: break; // 'auto'
  }

  // AUTO — the offline-first ladder.
  if (localHit) return { substrate: 'det', reason: 'auto-deterministic-floor' };  // FLOOR: zero-model, instant
  if (recipeHit) return { substrate: 'recipe', reason: 'auto-learned-recipe' };   // novel-once, then offline forever
  return escalate(text, caps);                                                    // genuinely new → best available
}

// Whether a substrate is generative (its output MUST be verified by recompute before trust).
const GENERATIVE = new Set(['M4-plan', 'M4-code', 'M3']);
export function isGenerative(substrate) { return GENERATIVE.has(substrate); }

// Whether a substrate runs entirely without the network (for the honest "offline" badge).
const OFFLINE = new Set(['det', 'plan-det', 'recipe', 'M4-plan', 'M4-code', 'M1']);
export function isOffline(substrate) { return OFFLINE.has(substrate); }

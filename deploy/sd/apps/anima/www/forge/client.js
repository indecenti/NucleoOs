// client.js — STAGED, not wired into the chat UI yet (pending hardware phase). No runtime consumer:
// the only exerciser is the host gate tools/anima-host/forge-client.test.mjs. Keep until wired.
// client.js — the ONE client adapter the chat UI calls. It hides "one ANIMA, four substrates"
// behind a single ask(): the dial value (or `auto` via the Stage-A pre-router) resolves a substrate,
// the matching injected provider runs, and the raw result is normalised into the canonical
// Envelope v1. Providers are INJECTED (deviceTransport for M1/M2/M3, localProvider for M4) so the
// whole adapter is host-testable with mocks and never touches fetch/WebGPU at import time.
// Honesty contract: M4 (browser-local) may NEVER borrow a device substrate label — it is always
// stamped 'M4-local' (ungrounded), regardless of what the provider returns.
// Pure & DOM-free → host-testable.

import { prerouteStageA } from './router.js';
import { normalize } from './envelope.js';

// dial value → device mode the firmware understands, and the substrate the envelope is stamped with.
// off→M1 (offline brain), on→M2 (hybrid grounded), only→M3 (cloud teacher). 'local' is M4 (browser GPU).
const DIAL_MODE = { off: 'off', on: 'on', only: 'only' };

// Map a Stage-A pre-route result to a concrete dial mode for the device transport.
// M1→off, M2→on, M3→only. (hands/local are handled by ask() before we get here.)
const SUBSTRATE_TO_MODE = { M1: 'off', M2: 'on', M3: 'only' };

export function makeClient({ deviceTransport, localProvider, caps = {} } = {}) {
  if (typeof deviceTransport !== 'function') throw new Error('makeClient: deviceTransport(q,opts) is required');

  // Run the device path for a resolved dial mode ('off'|'on'|'only') and envelope it with the
  // matching device substrate. The substrate is derived from `mode` inside normalize().
  async function viaDevice(q, mode, lang, signal) {
    const raw = await deviceTransport(q, { mode, lang, signal });
    return normalize(raw, { mode, lang });   // substrate ← MODE_TO_SUBSTRATE[mode]
  }

  // Run the browser-local M4 provider and HARD-stamp 'M4-local' so it can never masquerade as
  // a grounded device answer (provenance().grounded === false by construction).
  async function viaLocal(q, lang, opts) {
    if (typeof localProvider !== 'function') throw new Error('client: localProvider required for M4-local but not injected');
    const raw = await localProvider(q, opts);
    return normalize(raw, { substrate: 'M4-local', lang });   // substrate forced; raw.substrate ignored
  }

  // The device dictionary's two non-translation replies (mirrors nucleo_anima_translate.c): a miss
  // (escalate to a generative tier) and an ask (no word given → do NOT escalate, just relay the prompt).
  const TR_MISS = /non ho .*dizionario|don'?t have .*dictionary/i;
  const TR_ASK = /cosa traduco|what should i translate/i;

  // Translation ladder: DICTIONARY-FIRST, then escalate. The device's grounded IT<->EN dictionary is
  // tried first (instant, private, zero-hallucination) — perfect for common words/phrases. On a MISS we
  // climb to whatever generative tier the router picked (local LLM → cloud teacher) for phrases the
  // dictionary doesn't cover; if none is available we keep the device's honest decline. This is the
  // "scale to the last available" fallback, with the always-reachable grounded floor at the bottom.
  async function viaTranslate(q, lang, opts, stageA) {
    const dev = await viaDevice(q, 'off', lang, opts.signal);   // mode off = offline dictionary
    const reply = (dev && dev.reply) || '';
    if (reply && !TR_MISS.test(reply)) return dev;              // a hit, or an ask — relay as-is
    try {
      if (stageA.substrate === 'M4-local') {
        const r = await viaLocal(q, lang, { ...opts, kind: 'translate', target: stageA.target });
        if (r && r.reply) return r;
      } else if (stageA.substrate === 'M2' || stageA.substrate === 'M3') {
        const r = await viaDevice(q, 'only', lang, opts.signal);   // cloud teacher translates the phrase
        if (r && r.reply && !TR_MISS.test(r.reply)) return r;
      }
    } catch { /* model/network failure → fall back to the honest device answer below */ }
    return dev;                                                 // grounded decline — the last available
  }

  // The single call site for the UI.
  // mode: 'off'|'on'|'only'|'local'|'auto'. Returns either a canonical Envelope v1, OR — when the
  // pre-router recognises a deterministic file op — a {substrate:'hands', intent, ...} marker for the
  // caller's fsclient to execute (no LLM, no device round-trip).
  async function ask(q, { mode = 'auto', lang = 'it', history = [], root = '/', signal } = {}) {
    // --- concrete dial values bypass the router entirely ---
    if (mode === 'local') return viaLocal(q, lang, { history, root, signal, caps });
    if (DIAL_MODE[mode]) return viaDevice(q, DIAL_MODE[mode], lang, signal);

    // --- mode === 'auto' (or anything unknown): Stage-A deterministic pre-router ---
    const stageA = prerouteStageA(q, caps);
    if (!stageA) {
      // empty / unroutable — let the device's hybrid (or offline) brain decide.
      return viaDevice(q, caps.online ? 'on' : 'off', lang, signal);
    }
    if (stageA.kind === 'translate') return viaTranslate(q, lang, { history, root, signal, caps }, stageA);
    switch (stageA.substrate) {
      case 'hands':
        // deterministic file op — NOT an envelope. The caller's fsclient executes intent.
        return { substrate: 'hands', kind: stageA.kind, intent: stageA.intent, reason: stageA.reason, query: q };
      case 'local':
        // slash-command handled in-browser; still goes to the local provider so it gets enveloped.
        return viaLocal(q, lang, { history, root, signal, caps, kind: stageA.kind });
      case 'M4-local':
        return viaLocal(q, lang, { history, root, signal, caps, kind: stageA.kind });
      case 'M1':
      case 'M2':
      case 'M3':
        return viaDevice(q, SUBSTRATE_TO_MODE[stageA.substrate], lang, signal);
      default:
        return viaDevice(q, caps.online ? 'on' : 'off', lang, signal);
    }
  }

  return { ask };
}

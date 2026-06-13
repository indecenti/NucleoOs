// ANIMA — offline-resolution policy (pure, side-effect-free, import-free).
//
// This module owns the ONE decision the cascade cares about: when an answer must come from an
// offline brain, do we ask the in-browser WASM engine or the Cardputer first, and which reply
// counts as "answered"? It is deliberately DOM-free, fetch-free and import-free so it can be
// unit-tested in Node (apps/anima/local/cascade.test.mjs) with mocked runners — the browser
// (index.html) injects the real runners. Keeping the policy here means the guarantee the user
// asked for — "the browser WASM brain is tried before we scale onto the Cardputer" — is provable
// in isolation and can't silently regress when ask()'s many tiers move around.
//
// INVARIANT: the device is always reachable. The browser WASM is purely ADDITIVE — when it is not
// provisioned, throws, or abstains, resolveOffline falls through to the device (or returns null so
// the caller's honest-miss tail runs). Nothing here generates text; it only routes + filters.

// answered(r): does this shaped result actually carry a usable answer?
//
// Tier-AGNOSTIC on purpose. The engine.js shaper maps the C tier enum to strings but historically
// drops STITCH/L2 (tier 3 -> 'none') even when the reply is correct and non-empty, so gating on
// `tier !== 'none'` would silently discard good L2 answers (and the old online last-resort guard at
// index.html did exactly that). We gate on the reply plus a real action/domain instead, so a
// stitched descriptive answer (tier 'none' but domain 'knowledge') still counts, while a true
// abstention (empty reply, no action, no domain) correctly does not.
export function answered(r) {
  if (!r || !r.reply) return false;            // no text -> abstention
  if (r.tier && r.tier !== 'none') return true; // command / fact / remote
  if (r.action && r.action !== 'none') return true; // launch / system / answer / tool
  return !!(r.domain && r.domain !== 'none');  // stitch / L1-as-domain still answered
}

// resolveOffline(q, lang, opts, runners): try the browser's own brains and the device, in the preferred
// order, and return the first real answer, or null if none has one. Never throws (a throwing runner is
// treated as an abstention so the next source still gets its turn).
//
// THREE tiers, in this fixed relative order (the "browser exhausts itself before the Cardputer"):
//   browser   — the in-browser WASM offline cascade (grounded, instant, MCU-sparing).
//   webindex  — the in-browser web indexer: when the WASM brain abstains on a knowledge question it
//               fetches LIVE from Wikipedia/Wikidata DIRECTLY from the browser (or serves a cached card),
//               never through the Cardputer. Optional — omit the runner and this tier is simply skipped.
//   device    — the Cardputer's own offline cascade (/api/anima). The always-present safety net.
//
//   opts.prefer  'browser' (default) -> browser, webindex, device.
//                'device'            -> device, browser, webindex  (device chosen; browser as resilience).
//   opts.silent  true (default)  -> mid-chat fallback: the browser WASM is consulted ONLY if already
//                                   provisioned (runners.browserProvisioned() true), so we never trigger a
//                                   surprise ~14 MB pack download mid-turn; unprovisioned -> skip that tier.
//                                   (webindex is light — a few small API calls — so it is NOT silent-gated.)
//                false            -> explicit Browser mode: allow runners.browser's blocking provisioning gate.
//
//   runners.browser(q, lang)        async -> shaped result | null   (in-browser WASM cascade)
//   runners.webindex(q, lang)       async -> shaped result | null   (in-browser web indexer; optional)
//   runners.device(q, lang)         async -> shaped result | null   (/api/anima offline)
//   runners.browserProvisioned()    async -> boolean  (loaded || pack cached; MUST NOT download)
export async function resolveOffline(q, lang, opts = {}, runners = {}) {
  const prefer = opts.prefer === 'device' ? 'device' : 'browser';
  const silent = opts.silent !== false;        // default true
  const order = prefer === 'device' ? ['device', 'browser', 'webindex'] : ['browser', 'webindex', 'device'];
  for (const src of order) {
    // Silent fallback never pulls the browser PACK: if the WASM brain isn't already here, skip it (the
    // web index and device don't need provisioning, so they are never gated this way).
    if (src === 'browser' && silent) {
      let ok = false;
      try { ok = await runners.browserProvisioned(); } catch { ok = false; }
      if (!ok) continue;
    }
    const run = runners[src];
    if (typeof run !== 'function') continue;
    let r = null;
    try { r = await run(q, lang); } catch { r = null; }
    if (answered(r)) return r;
  }
  return null;
}

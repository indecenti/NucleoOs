// dlgate.js — OS-wide download gate for NucleoOS.
//
// The Cardputer serves the whole OS from a single-task, no-PSRAM HTTP server: two concurrent multi-MB
// pulls collapse it (the 503 "low memory, retry" read-storm in nucleo_webfs). So EVERY large download
// in the OS — the ANIMA brain pack, the voice models, the Forge weights — funnels through ONE exclusive
// lock held for the whole transfer. Web Locks are coordinated across same-origin contexts, so this is
// genuinely OS-wide: it spans every app, tab, and iframe the device serves. Requests QUEUE (they never
// fail) and run strictly one at a time, in order.
//
// This module only SERIALISES downloads the caller starts — it never starts one itself. "Don't auto-
// download" is each caller's policy; the gate just guarantees there is never more than one at a time.

const LOCK = 'nucleo-dl';
let _chain = Promise.resolve();    // same-tab fallback when Web Locks is unavailable
let _busy = false;                 // fallback-only busy flag

function _hasLocks() { return typeof navigator !== 'undefined' && navigator.locks && navigator.locks.request; }

// Run `fn` while holding the single OS-wide download lock. `label` is a short human string naming the
// transfer (e.g. "ANIMA brain pack"). Returns whatever `fn` resolves to. By default a busy gate makes the caller WAIT
// (FIFO queue). Pass {ifAvailable:true} for background/opportunistic work that must yield rather than
// queue — it resolves to opts.skipValue (default null) without running `fn` when something else holds.
export async function withDownloadLock(label, fn, opts = {}) {
  const skip = ('skipValue' in opts) ? opts.skipValue : null;
  const run = async () => fn();   // `label` is kept in the signature for callers/readability; nothing displays it

  if (_hasLocks()) {
    if (opts.ifAvailable) {
      return navigator.locks.request(LOCK, { mode: 'exclusive', ifAvailable: true },
        (lock) => (lock ? run() : skip));
    }
    return navigator.locks.request(LOCK, { mode: 'exclusive' }, () => run());
  }

  // Fallback (no Web Locks): serialise within this tab via a promise chain.
  if (opts.ifAvailable && _busy) return skip;
  const prev = _chain;
  let release; _chain = new Promise((r) => { release = r; });
  await prev;
  _busy = true;
  try { return await run(); } finally { _busy = false; release(); }
}


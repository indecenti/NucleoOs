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
const _subs = new Set();
const _bc = (typeof BroadcastChannel !== 'undefined') ? new BroadcastChannel('nucleo-dl') : null;
let _local = null;                 // label downloading in THIS tab (best-effort, for activeLabel())
let _chain = Promise.resolve();    // same-tab fallback when Web Locks is unavailable
let _busy = false;                 // fallback-only busy flag

function _notify(label) {
  _local = label || null;
  try { _bc && _bc.postMessage({ active: _local }); } catch {}
  for (const cb of _subs) { try { cb(_local); } catch {} }
}
if (_bc) _bc.onmessage = (e) => { const a = e && e.data ? e.data.active : null; for (const cb of _subs) { try { cb(a); } catch {} } };

function _hasLocks() { return typeof navigator !== 'undefined' && navigator.locks && navigator.locks.request; }

// Run `fn` while holding the single OS-wide download lock. `label` is a short human string shown in the
// UI ("Scaricando …"). Returns whatever `fn` resolves to. By default a busy gate makes the caller WAIT
// (FIFO queue). Pass {ifAvailable:true} for background/opportunistic work that must yield rather than
// queue — it resolves to opts.skipValue (default null) without running `fn` when something else holds.
export async function withDownloadLock(label, fn, opts = {}) {
  const skip = ('skipValue' in opts) ? opts.skipValue : null;
  const run = async () => { _notify(label); try { return await fn(); } finally { _notify(null); } };

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

// Best-effort: is ANY download running in the OS right now? (Cross-tab via the shared lock manager.)
export async function isDownloading() {
  try {
    if (_hasLocks() && navigator.locks.query) {
      const q = await navigator.locks.query();
      return ((q && q.held) || []).some((l) => l.name === LOCK);
    }
  } catch {}
  return _busy || !!_local;
}

// The label downloading in THIS tab right now (or null). A cross-tab label arrives via onDownloadStatus.
export function activeLabel() { return _local; }

// Subscribe to status changes (label string while downloading, null when idle). Returns an unsubscribe.
export function onDownloadStatus(cb) { _subs.add(cb); return () => _subs.delete(cb); }

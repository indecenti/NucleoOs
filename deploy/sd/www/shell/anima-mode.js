// anima-mode.js — the ONE definition of ANIMA's engine mode, plus the turn abort/timeout helper,
// shared by the ANIMA app (apps/anima/www/index.html) and the OS copilot (web/shell/copilot.js).
//
// Served from the shell root, so both surfaces import it the same way the i18n engine is imported:
// `import * as Mode from '/anima-mode.js'` (an app iframe) or `'./anima-mode.js'` (the shell).
// DOM-free and import-free: the copilot catalog test imports copilot.js in Node, which pulls this in.
//
// Why a module and not two copies: the ANIMA app stored auto|private while the copilot only knew
// off|on|only and defaulted to 'on', so "Private" in ANIMA meant hybrid + live Wikipedia in Ctrl+Space.
// One storage key, one value set, one legacy map, one mapping to the device's ?mode= parameter.
//
//   auto    - use the best brain available right now; the network may be used (cloud with a key,
//             live web index, the device's own online tier).
//   private - the same ladder with every network rung removed. Nothing leaves this machine: no cloud,
//             no live web index (cached cards only), and the device is asked in offline-only mode.

export const MODE_KEY = 'anima.mode';          // the localStorage key both surfaces read and write
export const MODE_SET_KEY = 'anima.modeSet';   // '1' once the user picked a mode explicitly
export const MODES = Object.freeze(['auto', 'private']);
export const DEFAULT_MODE = 'auto';

// Stored values from the five-mode era (and the copilot's off/on/only) map onto the two modes.
const LEGACY = Object.freeze({ off: 'private', edge: 'private', on: 'auto', local: 'auto', only: 'auto' });

export function normMode(m) {
  const v = String(m == null ? '' : m).trim().toLowerCase();
  return MODES.includes(v) ? v : (LEGACY[v] || DEFAULT_MODE);
}

const store = () => { try { return globalThis.localStorage || null; } catch { return null; } };

export function readMode() {
  const s = store();
  try { return normMode(s && s.getItem(MODE_KEY)); } catch { return DEFAULT_MODE; }
}

// Persist a mode. explicit=true records that the user chose it (the ANIMA app then never auto-switches).
export function writeMode(m, { explicit = true } = {}) {
  const v = normMode(m), s = store();
  try { if (s) { s.setItem(MODE_KEY, v); if (explicit) s.setItem(MODE_SET_KEY, '1'); } } catch {}
  return v;
}

export function isExplicit() {
  const s = store();
  try { return !!s && s.getItem(MODE_SET_KEY) === '1'; } catch { return false; }
}

export const isPrivate = (m) => normMode(m) === 'private';
// May the BROWSER reach the internet for this turn (cloud LLM, live Wikipedia, web search)?
export const allowsNetwork = (m) => !isPrivate(m);
// The /api/anima ?mode= value for the device. Private: offline-only (the device must not go online
// either). Auto: hybrid — the device may use its own online tier (weather, teacher), heap-guarded.
// 'only' is never emitted any more: the browser talks to the cloud directly.
export const deviceMode = (m) => (isPrivate(m) ? 'off' : 'on');

// Cross-window sync: another document (the ANIMA iframe, the shell, another tab) changed the mode.
// The `storage` event fires in every same-origin document except the writer. Returns an unsubscribe.
export function onModeChange(cb) {
  if (typeof globalThis.addEventListener !== 'function') return () => {};
  const h = (e) => { if (e && e.key === MODE_KEY) { try { cb(normMode(e.newValue)); } catch {} } };
  globalThis.addEventListener('storage', h);
  return () => globalThis.removeEventListener('storage', h);
}

// ---- conversation id for /api/anima ---------------------------------------------------------------
// The device keeps ONE conversation; a request whose `sid` differs from the previous one starts a
// fresh context, so two surfaces no longer chain each other's follow-ups. ≤23 chars of [A-Za-z0-9_-].
export function newSid(prefix = 'a') {
  const p = String(prefix).replace(/[^A-Za-z0-9_-]/g, '').slice(0, 3);
  const r = Math.random().toString(36).slice(2, 9);
  return (p + '-' + Date.now().toString(36) + r).slice(0, 23);
}
export const validSid = (s) => typeof s === 'string' && /^[A-Za-z0-9_-]{1,23}$/.test(s);

// ---- turn lifecycle: one AbortController per turn, one timeout per step --------------------------
// Every rung of a turn (device, web index, cloud, browser GPU) runs under stepSignal(turnSignal, ms):
// Stop/Esc aborts the turn signal and with it whatever step is in flight; a slow step times out on its
// own and the ladder moves on to the next rung. Reasons are DOMExceptions so callers can tell them apart.
export const STEP_MS = Object.freeze({
  device: 25000,   // /api/anima (a hybrid turn may open a TLS session on the device)
  web: 15000,      // live Wikipedia/Wikidata from the browser
  cloud: 90000,    // one non-streamed cloud completion
  stream: 180000,  // whole streamed completion (tokens keep arriving)
  idle: 30000,     // a stream that sends nothing for this long is dead
  local: 120000,   // browser GPU generation
});

const mkErr = (msg, name) => {
  try { return new DOMException(msg, name); }
  catch { const e = new Error(msg); e.name = name; return e; }
};
export const stopReason = () => mkErr('Stopped', 'AbortError');
export const timeoutReason = (ms) => mkErr('Timed out after ' + ms + ' ms', 'TimeoutError');
export const isTimeout = (e) => !!e && e.name === 'TimeoutError';
export const isAbort = (e) => !!e && (e.name === 'AbortError' || e.name === 'TimeoutError');

// A child signal that aborts when `parent` aborts (same reason) or after `ms` (TimeoutError).
// Call done() when the step settles so the timer and the listener are released.
export function stepSignal(parent, ms) {
  const ac = new AbortController();
  let timer = 0;
  const onParent = () => { try { ac.abort(parent.reason || stopReason()); } catch {} };
  if (parent) {
    if (parent.aborted) onParent();
    else parent.addEventListener('abort', onParent, { once: true });
  }
  if (ms > 0 && !ac.signal.aborted) timer = setTimeout(() => { try { ac.abort(timeoutReason(ms)); } catch {} }, ms);
  return {
    signal: ac.signal,
    abort: (reason) => { try { ac.abort(reason || stopReason()); } catch {} },
    done() { clearTimeout(timer); if (parent) parent.removeEventListener('abort', onParent); },
  };
}

// Run fn(signal) as one step of a turn. Rejects with the abort reason when the parent is stopped or
// the step times out, even if fn itself ignores the signal (the late result is dropped).
export async function withStep(parent, ms, fn) {
  const st = stepSignal(parent, ms);
  try {
    if (st.signal.aborted) throw st.signal.reason || stopReason();
    let onAbort;
    const gate = new Promise((_, rej) => {
      onAbort = () => rej(st.signal.reason || stopReason());
      st.signal.addEventListener('abort', onAbort, { once: true });
    });
    try { return await Promise.race([Promise.resolve().then(() => fn(st.signal)), gate]); }
    finally { st.signal.removeEventListener('abort', onAbort); }
  } finally { st.done(); }
}

// A fetch bound to a signal, for helpers that accept an injected fetch (web index, engine helpers).
export const fetchWith = (signal) => (url, opts = {}) => fetch(url, { ...opts, signal });

/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-user.js — who is using the web OS, answered the same way on every edition.
//
// A device with accounts (capability users.multi, docs/multiuser.md) says who is signed in. Every other device —
// the Cardputer, the P4, the browser edition, any firmware older than accounts — has one implicit owner, and this
// module answers as if that owner were signed in. Code that asks "who is this?" never looks at the edition:
//
//   import User from '/nucleo-user.js';
//   const me = await User.current();        // {id, name, role, multi} — null only when accounts are on and nobody is signed in
//   if (me && me.role === 'admin') …        // single-user devices: the owner is the administrator, as today
//
// The browser side of a change of person. The shell and the apps keep small state in localStorage (recent files,
// ANIMA's settings, an AI key typed in a browser app, the onboarding flag) without knowing about people. When a
// different person signs in on the same browser, switchTo() puts the previous person's keys aside
// (`nucleo.u.<id>:<key>`) and brings back the new person's, empties sessionStorage and the service worker's image
// cache (pictures read from the previous person's folders), and the shell reloads. No app changes: each one
// finds its own keys, now this person's. Keys that describe the browser rather than a person stay (KEEP).
// This separates people sharing one browser; it is not secrecy against someone with the browser's developer
// tools — separate browser profiles are, and docs/multiuser.md says so.
//
// Routes are written in full ('/api/…') so the browser edition's build moves them under its base path.

export const LAST = 'nucleo.user.last';
export const STASH = 'nucleo.u.';
// The account that took over the device's data when accounts were switched on (always the first one, u1):
// state a browser holds from before accounts was that person's.
export const OWNER = 'u1';
// Facts about this browser, not about a person.
export const KEEP = new Set([LAST, 'nucleopad.cid', 'anima.useWebGPU', 'anima.webgpuReady', 'anima.webgpuGrammar']);
export const PER_USER_CACHES = ['nucleo-wallpaper'];
// Unlocked keys of the encrypted cloud archive: forgotten at every change of person and at sign-out, so the next
// person can never use them (they unlock their own archive with their own phrase).
export const PER_USER_DATABASES = ['nucleo.archive'];
const IMPLICIT = Object.freeze({ id: 'owner', name: '', role: 'admin', multi: false });

export function createUser({ fetchFn = (...a) => fetch(...a), storage = safeStorage('localStorage'),
  session = safeStorage('sessionStorage'), cachesApi = typeof caches !== 'undefined' ? caches : null,
  idb = typeof indexedDB !== 'undefined' ? indexedDB : null } = {}) {
  const listeners = new Set();
  let last = null;

  async function status() {
    try {
      const r = await fetchFn('/api/auth/status', { cache: 'no-store' });
      return r.ok ? await r.json() : null;
    } catch { return null; }
  }

  /** The person using the web OS. Accounts off (or unknown device): the implicit owner. */
  async function current() {
    const st = await status();
    let me;
    if (!st || !st.multi) me = IMPLICIT;
    else if (st.user) me = { id: st.user.id, name: st.user.name, role: st.user.role, multi: true, must_change: !!st.user.must_change, avatar: st.user.avatar || '' };
    else me = null;
    const key = me ? me.id + '/' + me.role : '';
    if (last !== null && key !== last) for (const fn of listeners) try { fn(me); } catch {}
    last = key;
    return me;
  }

  async function post(path, body) {
    let r;
    try {
      r = await fetchFn(path, { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify(body || {}), cache: 'no-store' });
    } catch { return { ok: false, error: 'offline' }; }
    let j = {};
    try { j = await r.json(); } catch {}
    if (r.ok) return Object.assign({ ok: true }, j);
    return { ok: false, error: (j && j.error) || (r.status === 404 ? 'unsupported' : 'failed'), status: r.status, retry_after: j && j.retry_after };
  }

  /** {ok:true, user} or {ok:false, error: bad_credentials | locked (retry_after) | disabled | active_limit | offline | …} */
  const signIn = (name, password, remember = false) => post('/api/auth/login', { user: name, password, remember: !!remember });

  /** Ends this browser's session and forgets the pictures it cached; the person's other state stays put
   *  until someone else signs in here (switchTo). */
  async function signOut() {
    const r = await post('/api/auth/logout');
    await dropCaches();
    try { session && session.clear(); } catch {}
    return r;
  }

  const changePassword = (oldPw, newPw) => post('/api/auth/password', { old: oldPw, new: newPw });
  /** The first administrator (the device has users.multi and no account yet). `pin`: the pairing PIN read on the
   *  device, the proof a browser on the network gives; from the device itself or its cable none is needed. */
  const setup = (name, password, pin) => post('/api/auth/setup', pin ? { name, password, pin } : { name, password });

  async function dropCaches() {
    if (cachesApi) for (const name of PER_USER_CACHES) try { await cachesApi.delete(name); } catch {}
    if (idb) for (const name of PER_USER_DATABASES) try { idb.deleteDatabase(name); } catch {}
  }

  /** Make this browser's state the given person's. True when it changed: the caller reloads the page. */
  async function switchTo(id) {
    if (!storage || !id) return false;
    const prev = read(storage, LAST) || OWNER;
    if (prev === id) { write(storage, LAST, id); return false; }
    const keys = allKeys(storage);
    for (const k of keys) {
      if (KEEP.has(k) || k.startsWith(STASH)) continue;
      const v = read(storage, k);
      // Put aside, then remove. If the stash does not fit (a full storage), the value is still removed:
      // the next person must never find it.
      write(storage, STASH + prev + ':' + k, v);
      try { storage.removeItem(k); } catch {}
    }
    const mine = STASH + id + ':';
    for (const k of allKeys(storage)) {
      if (!k.startsWith(mine)) continue;
      write(storage, k.slice(mine.length), read(storage, k));
      try { storage.removeItem(k); } catch {}
    }
    try { session && session.clear(); } catch {}
    await dropCaches();
    write(storage, LAST, id);
    return true;
  }

  const onChange = (fn) => { listeners.add(fn); return () => listeners.delete(fn); };

  return { status, current, signIn, signOut, changePassword, setup, switchTo, onChange };
}

function safeStorage(name) {
  try { return typeof globalThis[name] !== 'undefined' ? globalThis[name] : null; } catch { return null; }
}
function allKeys(s) {
  const out = [];
  try { for (let i = 0; i < s.length; i++) { const k = s.key(i); if (k !== null) out.push(k); } } catch {}
  return out;
}
function read(s, k) { try { return s.getItem(k); } catch { return null; } }
function write(s, k, v) { try { if (v !== null && v !== undefined) s.setItem(k, v); return true; } catch { return false; } }

const User = createUser();
export default User;

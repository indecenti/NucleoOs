/* SPDX-License-Identifier: AGPL-3.0-or-later */
// appbroker.js — the capability broker for SANDBOXED apps.
//
// WHY THIS EXISTS. Every app in NucleoOS runs in a same-origin iframe, which means it inherits the
// shell's origin: the pairing cookie rides along on every fetch it makes, it can read the shell's
// localStorage, and it can call ANY /api/* route. So an app that declared `storage.app` could just as
// well do GET /api/fs/read?path=/data/anima/teacher.json and walk off with the user's Claude, Groq and
// xAI keys. The Feature Policy added earlier covers exactly what the BROWSER can mediate — microphone,
// camera, geolocation — and nothing else; the /api/* half was still wide open, for every app.
//
// A service worker cannot fix this: on the device the OS is served over plain http, which is not a
// secure context, so there IS no service worker there (see the three notes in shell.js). Any gate put
// in sw.js is inert exactly where it matters.
//
// The only real fix is to take the origin away — sandbox="allow-scripts" WITHOUT allow-same-origin, so
// the frame gets an opaque origin with no cookie and no shared storage — and give it back a narrow,
// declared surface through postMessage. That is this file.
//
// SCOPE, deliberately. Only apps marked created_by:'agent' in the registry are sandboxed. They are the
// ones nobody vetted — written by a model, installed live — and, since none is installed yet, the
// contract can be their birth contract instead of a migration. The 46 curated apps are untouched.
//
// DEVICE DISCIPLINE. Every brokered filesystem call goes through ONE serial chain with a timeout. That
// is the point, not a side effect: N sandboxed apps sharing the Cardputer's 4-6 sockets cannot burst,
// because the broker is the only door and the door admits one at a time. This makes the device work
// LESS than the status quo, where each app fetched on its own schedule.

import { createFs } from './nucleo-fs.js';

// ── app record policy (pure) ──────────────────────────────────────────────────────────────────
// /api/apps streams id/name/route/icon straight from each manifest — and the agent can publish a
// manifest with any strings it likes. Those fields end up in innerHTML (taskbar, Start, window bar),
// in attribute values (data-src, data-app) and as the iframe src. So every app record is normalised
// HERE, once, before the shell ever renders it: an id is a plain slug, a route is a same-origin path,
// an icon is a same-origin path or an inline image. Rendering sites escape as well (defence in depth);
// this is what makes a hostile manifest inert even where one of them forgets.
const APP_ID = /^[a-z0-9][a-z0-9._-]{0,63}$/i;
const SAFE_PATH = /^[A-Za-z0-9._~\/-]+$/;              // no quotes, spaces, brackets, colons, backslashes
function samePath(p) {
  if (typeof p !== 'string' || !p) return '';
  if (p.startsWith('//') || !SAFE_PATH.test(p) || /(^|\/)\.\.(\/|$)/.test(p)) return '';
  return p;
}
export function sanitizeApp(a) {
  if (!a || typeof a !== 'object' || typeof a.id !== 'string' || !APP_ID.test(a.id)) return null;
  const out = { ...a };
  out.name = String(a.name == null || a.name === '' ? a.id : a.name).replace(/[\u0000-\u001f\u007f]/g, '').slice(0, 80);
  // A route is where the window's iframe points: only an absolute same-origin path (never a scheme).
  out.route = typeof a.route === 'string' && a.route.startsWith('/') ? samePath(a.route) : '';
  if (typeof a.web_route === 'string') out.web_route = a.web_route.startsWith('/') ? samePath(a.web_route) : '';
  if (typeof a.path === 'string') out.path = a.path.startsWith('/') ? samePath(a.path) : '';
  // verify: where the signed package of a store app is served (a same-origin folder): the shell re-checks
  // it before each load (wm.js setVerifier). A malformed one does not skip the check: the app loses its route.
  if ('verify' in a) {
    out.verify = typeof a.verify === 'string' && a.verify.startsWith('/') && a.verify.endsWith('/') ? samePath(a.verify) : '';
    if (!out.verify) out.route = '';
  }
  // An icon is a same-origin (relative or absolute) path, or an inline raster/SVG image. Remote URLs are
  // dropped: an icon that pings a third-party host would leak "this device is on" to anyone.
  const ic = typeof a.icon === 'string' ? a.icon.trim() : '';
  if (/^data:image\/(png|gif|jpeg|webp|svg\+xml);base64,[A-Za-z0-9+/=]+$/.test(ic)) out.icon = ic;
  else out.icon = samePath(ic);
  return out;
}

// ── path policy (pure) ────────────────────────────────────────────────────────────────────────
// An app's reach is decided by what its manifest DECLARED, not by what it asks for:
//   storage.app     → /data/apps/<id>/…      its own private corner
//   storage.shared  → /data/…                the shared user area (documents, pictures, recordings)
// Anything else — /system, /apps, /www, the ANIMA brain, another app's folder — is out of reach for
// both. Note /data/anima is excluded even under storage.shared: that is where the key vault lives.
const SHARED_ROOT = '/data';
const DENY_UNDER_SHARED = ['/data/anima', '/data/agent'];

// Normalise like the device does: collapse //, drop ./, resolve .. WITHOUT ever climbing past the root.
export function normPath(p) {
  const out = [];
  for (const seg of String(p == null ? '' : p).split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') { out.pop(); continue; }
    out.push(seg);
  }
  return '/' + out.join('/');
}

// Resolve an app-supplied path to the absolute SD path it is allowed to touch, or null if it is not.
// Returns null rather than throwing so a hostile app cannot tell "denied" from "malformed" by timing.
export function resolveAppPath(appId, permissions, path) {
  const id = String(appId || '').trim();
  if (!id || /[^a-z0-9._-]/i.test(id)) return null;
  const perms = Array.isArray(permissions) ? permissions : [];
  const abs = normPath(path);
  if (abs === '/') return null;

  const own = '/data/apps/' + id;
  if (perms.includes('storage.app') && (abs === own || abs.startsWith(own + '/'))) return abs;

  if (perms.includes('storage.shared')) {
    if (abs !== SHARED_ROOT && !abs.startsWith(SHARED_ROOT + '/')) return null;
    for (const d of DENY_UNDER_SHARED) if (abs === d || abs.startsWith(d + '/')) return null;
    return abs;
  }
  return null;
}

// The whole vocabulary. Anything not listed is refused — a deny-list would be a promise we cannot keep.
export const METHODS = ['fs.read', 'fs.write', 'fs.list', 'notify', 'sys.info', 'sys.status', 'ai.ask', 'ai.complete',
  'app.i18n', 'kv.get', 'kv.set', 'kv.remove', 'kv.all', 'sys.caps', 'file.save', 'file.open', 'file.launch', 'file.write',
  'appfs.read', 'appfs.write', 'appfs.list', 'appfs.remove'];
// file.save bounds: a document the user made in the app, handed to the browser as a download. The
// type comes from the extension, from a short list of inert formats (nothing a browser would run).
export const SAVE_MAX_BYTES = 64 * 1024 * 1024;
export const SAVE_TYPES = { pdf: 'application/pdf', png: 'image/png', jpg: 'image/jpeg', jpeg: 'image/jpeg',
  webp: 'image/webp', txt: 'text/plain', csv: 'text/csv', json: 'application/json', md: 'text/markdown' };
const SAVE_GAP_MS = 1500;           // one download at a time, never a burst
export const OPEN_MAX_FILES = 32;
// appfs.*: an app's own FILES (binary), next to its kv: /data/apps/<id>/files/<name>. What kv is for settings,
// this is for content an app keeps for itself — signature images, a cache of covers, exported drafts.
export const APPFS_MAX_FILE = 16 * 1024 * 1024;
export const APPFS_MAX_TOTAL = 64 * 1024 * 1024;
export const APPFS_MAX_FILES = 512;
const APPFS_NAME = /^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$/;       // one level, no hidden names, no paths
export const appfsDir = (appId) => '/data/apps/' + appId + '/files';
export function appfsName(name) {
  const n = String(name == null ? '' : name);
  return APPFS_NAME.test(n) && !n.includes('..') ? n : null;
}
// file.write handles: what the user picked in an Open/Save sheet (or opened the app with), for "Save" without
// a sheet. Random, per app, kept by the shell for its session only: a handle is the user's choice, never a path.
const HANDLE_RE = /^h[0-9a-f]{32}$/;
function newHandle() {
  const b = new Uint8Array(16);
  globalThis.crypto.getRandomValues(b);
  return 'h' + Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
}
// The extensions an app may ask the Open sheet to show: plain short words, at most 16 of them.
export function openTypes(types) {
  if (!Array.isArray(types)) return [];
  return [...new Set(types.map((x) => String(x == null ? '' : x).replace(/^\./, '').toLowerCase()).filter((x) => /^[a-z0-9]{1,8}$/.test(x)))].slice(0, 16);
}
// The name a saved file gets: no path, no control or reserved characters, an allowed extension.
// null when nothing usable is left.
export function saveName(name) {
  const base = String(name == null ? '' : name).split(/[\\/]/).pop()
    .replace(/[\u0000-\u001f\u007f<>:"|?*]/g, '').replace(/^[.\s]+|[.\s]+$/g, '').slice(0, 120);
  const m = base.match(/\.([A-Za-z0-9]{1,5})$/);
  if (!m || !Object.prototype.hasOwnProperty.call(SAVE_TYPES, m[1].toLowerCase()) || base.length <= m[0].length) return null;
  return base;
}
// kv.* bounds: an app's own small state (settings, a score sheet), the size of what a page keeps in
// localStorage — not a database. One JSON file per app, rewritten whole, so it stays small on purpose.
export const KV_MAX_BYTES = 64 * 1024;
export const KV_MAX_KEYS = 256;
const KV_KEY = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;   // no leading '_' or '.': no __proto__, no hidden keys
const I18N_MAX_BYTES = 128 * 1024;
export const kvPath = (appId) => '/data/apps/' + appId + '/kv.json';
// ai.* bounds. Small on purpose: a sandboxed app asks questions, it does not stream conversations.
const AI_Q_MAX = 256;            // ai.ask query (chars)
const AI_PROMPT_MAX = 4096;      // ai.complete prompt (chars)
const AI_REPLY_MAX = 4000;       // either reply, truncated before it re-enters the sandbox
export function methodAllowed(method, permissions) {
  const perms = Array.isArray(permissions) ? permissions : [];
  if (!METHODS.includes(method)) return false;
  // sys.info needs no declaration: it returns only the OS display language. A sandboxed app cannot
  // import /nucleo-i18n.js (cross-origin module from an opaque origin), so without this it would be
  // stuck in one language — and shipping monolingual apps into a five-language OS is not a trade we
  // make for isolation. It discloses nothing the user has not already chosen and can see on screen.
  if (method === 'sys.info') return true;
  // app.i18n is the same trade for the app's OWN strings: the shell reads core + the app's catalogs
  // (public files the app ships) and hands them over, because the frame cannot fetch them itself.
  // kv.* is the app's own small state under /data/apps/<id>/kv.json — what every web page gets from
  // localStorage, which a frame without an origin does not have. Neither reaches anything but the
  // calling app's own files, so neither needs a declaration.
  if (method === 'app.i18n' || method.startsWith('kv.')) return true;
  // sys.caps says which device features exist (nucleo-caps.js), so an app can hide what is missing instead of
  // failing on it. It is the same list the device's own /api/system/caps publishes without pairing.
  if (method === 'sys.caps') return true;
  // file.save hands the user a file the app made (a signed PDF, an exported image) as a browser download:
  // what any web page may do, and the one thing a frame without allow-downloads cannot. It reaches
  // nothing on the device and reads nothing back, so it needs no declaration.
  if (method === 'file.save') return true;
  // file.open and file.launch hand over ONLY what the user picked in the shell's own Open sheet, or the file
  // the user opened the app with (desktop, File Commander): the user's gesture is the grant, as with a
  // browser's file input. file.write rewrites only such a file, by its handle. The app never names a path.
  if (method === 'file.open' || method === 'file.launch' || method === 'file.write') return true;
  // appfs.* is kv's binary sibling: the calling app's own folder, nothing else, so no declaration either.
  if (method.startsWith('appfs.')) return true;
  // sys.status is the device telemetry every curated app reads from /api/status — time, uptime, SD
  // space, Wi-Fi name. Read-only and cheap, but it does describe the household's network, so it is
  // DECLARED (system.events), not free like sys.info.
  if (method === 'sys.status') return perms.includes('system.events');
  if (method === 'notify') return perms.includes('system.notify');
  // Intelligence as a declared capability, in two distinct trust tiers: ai.anima is the on-device
  // deterministic brain (cheap, offline, can't hallucinate); ai.cloud spends the USER'S API key.
  // Neither implies the other, and neither buys any storage reach.
  if (method === 'ai.ask') return perms.includes('ai.anima');
  if (method === 'ai.complete') return perms.includes('ai.cloud');
  return perms.includes('storage.app') || perms.includes('storage.shared');
}

// ── the broker (needs the shell's deps injected, so the policy above stays testable) ───────────
// The theme a sandboxed app is told about: mode, accent and base font size, nothing else of the shell.
export function themeInfo(t) {
  const s = t || {};
  const accent = /^#[0-9a-f]{3,8}$/i.test(String(s.accent || '')) ? s.accent : '#4ea1ff';
  const fontSize = /^\d{1,2}px$/.test(String(s.fontSize || '')) ? s.fontSize : '14px';
  return { mode: s.theme === 'light' ? 'light' : 'dark', accent, fontSize };
}

// A pushed event (language or theme changed). The shell posts it to every app frame; apps that use
// nucleo-app.js repaint, others ignore it.
export function brokerEvent(event, payload) {
  return { type: 'nucleo.broker.event', event, ...payload };
}

export function createBroker({ fetchFn, fs: fsIn, findApp, notify, onFsChange, getLang, getTheme, getCaps, aiComplete, download,
  pickOpen, pickSave, launchFile, aiCooldownMs = 2000 } = {}) {
  const doFetch = fetchFn || ((...a) => fetch(...a));
  // Every disk access of the broker goes through the core file layer (nucleo-fs.js): atomic replace, space
  // check, one error vocabulary — the same code the shell and the system apps use.
  const fs = fsIn || createFs({ fetchFn: doFetch, caps: getCaps || (async () => ({ caps: [], limits: {} })), onChange: onFsChange || null });
  const handles = new Map();          // handle -> { appId, path, name }
  const grant = (appId, path) => { const h = newHandle(); handles.set(h, { appId, path, name: path.split('/').pop() }); return h; };
  // ONE call in flight, ever. See the device-discipline note at the top.
  let chain = Promise.resolve();
  const serial = (fn) => {
    const run = chain.then(fn, fn);
    chain = run.then(() => {}, () => {});
    return run;
  };
  // ai.complete is browser-direct (never touches the device), so it does not ride the device chain —
  // but it spends the user's key, so it is single-flight AND cooled down. Both refusals are immediate:
  // a queue here would just let an app buy delayed spending.
  let aiBusy = false, aiLastAt = 0;
  let saveLastAt = -Infinity;
  let sheetBusy = false;              // one Open/Save sheet at a time, whichever app asks
  const lang = () => (getLang ? getLang() : 'en') || 'en';
  const timeoutOpts = (extra) => {
    const o = { cache: 'no-store', ...extra };
    if (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) o.signal = AbortSignal.timeout(8000);
    return o;
  };

  // ── app.i18n: core + the app's own catalogs, base (en) under the active language ──
  const i18nCache = new Map();          // `${id}|${lang}` -> Promise of the merged flat map
  async function readCatalog(url) {
    try {
      const r = await doFetch(url, timeoutOpts());
      if (!r.ok) return {};
      const text = await r.text();
      if (text.length > I18N_MAX_BYTES) return {};
      const j = JSON.parse(text);
      if (!j || typeof j !== 'object' || Array.isArray(j)) return {};
      const out = {};
      for (const [k, v] of Object.entries(j)) {
        if (k.startsWith('_')) continue;          // _lang and other metadata
        if (typeof v === 'string' || (v && typeof v === 'object' && !Array.isArray(v))) out[k] = v;
      }
      return out;
    } catch { return {}; }
  }
  function appStrings(app, lg) {
    const key = app.id + '|' + lg;
    if (!i18nCache.has(key)) {
      // Same files and same order as nucleo-i18n.js: core under the app, English under the language.
      const urls = ['/i18n/core.en.json', '/i18n/core.' + lg + '.json', '/apps/' + app.id + '/i18n.en.json', '/apps/' + app.id + '/i18n.' + lg + '.json'];
      const p = serial(async () => {
        const parts = [];
        for (const u of (lg === 'en' ? [urls[0], urls[2]] : urls)) parts.push(await readCatalog(u));
        return Object.assign({}, ...parts);
      });
      i18nCache.set(key, p);
      p.then((m) => { if (!Object.keys(m).length) i18nCache.delete(key); }, () => i18nCache.delete(key));
    }
    return i18nCache.get(key);
  }

  // ── kv.*: one cached JSON object per app, written back whole, writes coalesced ──
  const kv = new Map();                 // app id -> { data, loading, flushing, again, dirReady }
  function kvEntry(id) {
    if (!kv.has(id)) kv.set(id, { data: null, loading: null, flushing: null, again: false, dirReady: false });
    return kv.get(id);
  }
  async function kvLoad(id) {
    const e = kvEntry(id);
    if (e.data) return e.data;
    if (!e.loading) {
      e.loading = serial(async () => {
        // a damaged file starts over (readJSON's fallback), it does not wedge the app
        const j = await fs.readJSON(kvPath(id), {});
        return j && typeof j === 'object' && !Array.isArray(j) ? j : {};
      }).then((d) => { e.data = d; return d; }).finally(() => { e.loading = null; });
    }
    return e.loading;
  }
  // Atomic: a cut connection never leaves the app's settings half written (nucleo-fs.js).
  async function kvWrite(id, body) {
    await fs.write(kvPath(id), body);
  }
  // A burst of kv.set (a score tapped five times) becomes at most one write in flight plus one after it,
  // always of the latest state: the device sees a handful of writes, not one per tap.
  function kvFlush(id) {
    const e = kvEntry(id);
    if (e.flushing) { e.again = true; return e.flushing; }
    e.flushing = (async () => {
      do { e.again = false; await serial(() => kvWrite(id, JSON.stringify(e.data))); } while (e.again);
    })().finally(() => { e.flushing = null; });
    return e.flushing;
  }
  async function kvExec(app, method, args) {
    const a = args || {};
    if (method !== 'kv.all' && (typeof a.key !== 'string' || !KV_KEY.test(a.key))) return { ok: false, error: 'bad key (1-64 of A-Z a-z 0-9 . _ -, starting with a letter or digit)' };
    let data;
    try { data = await kvLoad(app.id); } catch (err) { return { ok: false, error: String((err && err.message) || err) }; }
    if (method === 'kv.get') return { ok: true, value: Object.prototype.hasOwnProperty.call(data, a.key) ? data[a.key] : null };
    if (method === 'kv.all') return { ok: true, values: { ...data } };
    const next = { ...data };
    if (method === 'kv.remove') {
      if (!Object.prototype.hasOwnProperty.call(next, a.key)) return { ok: true };
      delete next[a.key];
    } else {
      let json;
      try { json = JSON.stringify(a.value); } catch { json = undefined; }
      if (json === undefined) return { ok: false, error: 'value must be JSON' };
      next[a.key] = JSON.parse(json);                   // a plain copy: nothing of the caller survives
      if (Object.keys(next).length > KV_MAX_KEYS) return { ok: false, error: 'too many keys (max ' + KV_MAX_KEYS + ')' };
    }
    if (JSON.stringify(next).length > KV_MAX_BYTES) return { ok: false, error: 'too large (max ' + KV_MAX_BYTES + ' bytes)' };
    const e = kvEntry(app.id);
    const before = e.data;
    e.data = next;
    try { await kvFlush(app.id); return { ok: true }; }
    catch (err) {
      if (e.data === next) e.data = before;             // not saved: do not pretend it was
      return { ok: false, error: String((err && err.message) || err) };
    }
  }

  // The shell's own one-time copy of an app's OLD state into its kv (shell/migrations.js). Never reachable from
  // a frame. Same chain, cache and limits as kv.set, so it cannot race the app's own writes, and it never
  // replaces a key the app already has. The check and the set happen in one step, with no await between them.
  async function kvSeed(id, key, value) {
    if (typeof id !== 'string' || !APP_ID.test(id) || typeof key !== 'string' || !KV_KEY.test(key)) throw new Error('bad app id or key');
    await kvLoad(id);                                   // throws when the device cannot be read: retried later
    const e = kvEntry(id);
    const data = e.data || {};
    if (Object.prototype.hasOwnProperty.call(data, key)) return 'exists';
    let json;
    try { json = JSON.stringify(value); } catch { json = undefined; }
    if (json === undefined) throw new Error('value must be JSON');
    const next = { ...data, [key]: JSON.parse(json) };
    if (Object.keys(next).length > KV_MAX_KEYS || JSON.stringify(next).length > KV_MAX_BYTES) return 'too-large';
    const before = e.data;
    e.data = next;
    try { await kvFlush(id); }
    catch (err) { if (e.data === next) e.data = before; throw err; }
    return 'written';
  }

  function dataBytes(d) {
    if (d instanceof ArrayBuffer) return new Uint8Array(d);
    if (ArrayBuffer.isView(d)) return new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
    if (typeof d === 'string') return new TextEncoder().encode(d);
    throw new Error('bad data');
  }
  // appfs.*: one flat folder per app, bounded per file, in number and in total. Errors are nucleo-fs codes.
  async function appfsExec(app, method, a) {
    const dir = appfsDir(app.id);
    try {
      if (method === 'appfs.list') return { ok: true, entries: (await fs.list(dir)).filter((e) => e.type === 'file').map((e) => ({ name: e.name, size: e.size })) };
      const name = appfsName(a.name);
      if (!name) return { ok: false, error: 'bad name' };
      const path = dir + '/' + name;
      if (method === 'appfs.read') {
        const b = await fs.read(path);
        return { ok: true, data: b == null ? null : b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength) };
      }
      if (method === 'appfs.remove') return { ok: true, removed: await fs.remove(path) };
      let bytes;
      try { bytes = dataBytes(a.data); } catch { return { ok: false, error: 'bad data' }; }
      if (bytes.byteLength > APPFS_MAX_FILE) return { ok: false, error: 'too-large' };
      const others = (await fs.list(dir)).filter((e) => e.type === 'file' && e.name !== name);
      if (others.length >= APPFS_MAX_FILES) return { ok: false, error: 'too-many' };
      if (others.reduce((t, e) => t + e.size, 0) + bytes.byteLength > APPFS_MAX_TOTAL) return { ok: false, error: 'no-space' };
      await serial(() => fs.write(path, bytes));
      return { ok: true, bytes: bytes.byteLength };
    } catch (e) { return { ok: false, error: (e && e.code) || String((e && e.message) || e) }; }
  }

  async function exec(app, method, args) {
    const perms = (app && app.permissions) || [];
    if (!methodAllowed(method, perms)) return { ok: false, error: 'denied: ' + method };

    if (method === 'sys.info') return { ok: true, lang: lang(), theme: themeInfo(getTheme ? getTheme() : null) };

    if (method === 'app.i18n') {
      const want = String((args && args.lang) || '');
      const lg = /^[a-z]{2}$/.test(want) ? want : lang();
      return { ok: true, lang: lg, strings: await appStrings(app, lg) };
    }

    if (method.startsWith('kv.')) return kvExec(app, method, args);

    if (method === 'sys.caps') {
      const c = getCaps ? await getCaps().catch(() => null) : null;
      return { ok: true, caps: c && Array.isArray(c.caps) ? c.caps.slice() : [] };
    }

    if (method === 'file.save') {
      if (!download && !pickSave) return { ok: false, error: 'unsupported' };
      const name = saveName(args && args.name);
      if (!name) return { ok: false, error: 'bad name' };
      const d = args && args.data;
      let bytes = null;
      if (d instanceof ArrayBuffer) bytes = new Uint8Array(d);
      else if (ArrayBuffer.isView(d)) bytes = new Uint8Array(d.buffer, d.byteOffset, d.byteLength);
      else if (typeof d === 'string') bytes = new TextEncoder().encode(d);
      if (!bytes) return { ok: false, error: 'bad data' };
      if (bytes.byteLength > SAVE_MAX_BYTES) return { ok: false, error: 'too large (max 64 MB)' };
      const mime = SAVE_TYPES[name.split('.').pop().toLowerCase()];
      // The Save sheet: the user picks a folder on the device, or the download. Nothing is written until they do.
      if (pickSave) {
        if (sheetBusy) return { ok: false, error: 'busy' };
        sheetBusy = true;
        try {
          const r = await pickSave(app, { name, mime, bytes });
          if (!r) return { ok: false, error: 'cancelled' };
          if (r.where === 'device') return { ok: true, name: r.name || name, where: 'device', handle: grant(app.id, r.path), bytes: bytes.byteLength };
          return { ok: true, name: r.name || name, where: 'download', bytes: bytes.byteLength };
        } catch (e) { return { ok: false, error: String((e && e.message) || e) }; }
        finally { sheetBusy = false; }
      }
      const now = Date.now();
      if (now - saveLastAt < SAVE_GAP_MS) return { ok: false, error: 'busy' };
      saveLastAt = now;
      try { await download(name, mime, bytes); }
      catch (e) { return { ok: false, error: String((e && e.message) || e) }; }
      return { ok: true, name, where: 'download', bytes: bytes.byteLength };
    }

    if (method === 'file.open') {
      if (!pickOpen) return { ok: false, error: 'unsupported' };
      if (sheetBusy) return { ok: false, error: 'busy' };
      sheetBusy = true;
      try {
        const files = await pickOpen(app, { types: openTypes(args && args.types), multiple: !!(args && args.multiple) });
        if (!files || !files.length) return { ok: false, error: 'cancelled' };
        return { ok: true, files: files.slice(0, OPEN_MAX_FILES).map((f) => ({ name: String(f.name || '').split(/[\\/]/).pop().slice(0, 200), data: f.bytes,
          where: f.path ? 'device' : 'local', ...(f.path ? { handle: grant(app.id, f.path) } : {}) })) };
      } catch (e) { return { ok: false, error: String((e && e.message) || e) }; }
      finally { sheetBusy = false; }
    }

    if (method === 'file.launch') {
      if (!launchFile) return { ok: true, file: null };
      try {
        const f = await launchFile(app);
        return { ok: true, file: f ? { name: String(f.name || '').split(/[\\/]/).pop().slice(0, 200), data: f.bytes, where: 'device',
          ...(f.path ? { handle: grant(app.id, f.path) } : {}) } : null };
      } catch (e) { return { ok: false, error: String((e && e.message) || e) }; }
    }

    // file.write {handle, data}: "Save" onto a file the USER chose for this app (Open/Save sheet, or the file the
    // app was opened with). Atomic like every write; a handle of another app, or none, is refused.
    if (method === 'file.write') {
      const h = String((args && args.handle) || '');
      const g = HANDLE_RE.test(h) ? handles.get(h) : null;
      if (!g || g.appId !== app.id) return { ok: false, error: 'bad handle' };
      let bytes;
      try { bytes = dataBytes(args && args.data); } catch { return { ok: false, error: 'bad data' }; }
      if (bytes.byteLength > SAVE_MAX_BYTES) return { ok: false, error: 'too large (max 64 MB)' };
      try { await fs.write(g.path, bytes); return { ok: true, name: g.name, where: 'device', bytes: bytes.byteLength }; }
      catch (e) { return { ok: false, error: (e && e.code) || 'failed' }; }
    }

    if (method.startsWith('appfs.')) return appfsExec(app, method, args || {});

    if (method === 'notify') {
      if (notify) notify(String((args && args.text) || '').slice(0, 200), app.name || app.id);
      return { ok: true };
    }

    if (method === 'sys.status') {
      // One device socket, serialised like fs.*, and a REDUCED shape on purpose: a sandboxed app gets
      // what a status widget needs (clock, uptime, SD space, network name), not the device's IP or
      // partition table. Least privilege is the broker's whole reason to exist.
      return serial(async () => {
        const opts = { cache: 'no-store' };
        if (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) opts.signal = AbortSignal.timeout(8000);
        try {
          const r = await doFetch('/api/status', opts);
          if (!r.ok) return { ok: false, error: 'http-' + r.status };
          const j = await r.json().catch(() => null);
          if (!j) return { ok: false, error: 'bad-json' };
          return { ok: true, status: {
            os: j.os, version: j.version, time: j.time || null, uptime_s: j.uptime_s | 0,
            storage: j.storage ? { mounted: !!j.storage.mounted, free_bytes: j.storage.free_bytes | 0, total_bytes: j.storage.total_bytes | 0 } : null,
            wifi: j.wifi ? { mode: j.wifi.mode || null, ssid: j.wifi.ssid || null } : null,
          } };
        } catch (e) { return { ok: false, error: String((e && e.message) || e) }; }
      });
    }

    // ai.ask — the on-device deterministic brain. Rides the SAME serial chain as fs.*: it is a device
    // call, and the device discipline (one door, one at a time) is the whole point of this file.
    // mode=off pins the engine to its offline path: a sandboxed app must not make the device network.
    // The answer is STRIPPED to text: /api/anima replies can carry {action,tool,arg} OS effects, and
    // those belong to the copilot's dispatcher, never to untrusted machine-written code.
    if (method === 'ai.ask') {
      const q = String((args && args.q) || '').trim();
      if (!q) return { ok: false, error: 'empty query' };
      if (q.length > AI_Q_MAX) return { ok: false, error: 'query too long (max ' + AI_Q_MAX + ')' };
      const lang = (getLang ? getLang() : 'en') || 'en';
      return serial(async () => {
        const opts = { cache: 'no-store' };
        if (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) opts.signal = AbortSignal.timeout(8000);
        try {
          const r = await doFetch('/api/anima?q=' + encodeURIComponent(q) + '&lang=' + encodeURIComponent(lang) + '&mode=off', opts);
          if (!r.ok) return { ok: false, error: 'http-' + r.status };
          const j = await r.json().catch(() => null);
          if (!j) return { ok: false, error: 'bad reply' };
          return { ok: true, reply: String(j.reply || '').slice(0, AI_REPLY_MAX), intent: typeof j.intent === 'string' ? j.intent : '' };
        } catch (e) {
          return { ok: false, error: String((e && e.message) || e) };
        }
      });
    }

    // ai.complete — the cloud, on the user's key, executed by the SHELL in its own origin. The key
    // never crosses into the sandbox; only the completion text comes back, truncated.
    if (method === 'ai.complete') {
      if (!aiComplete) return { ok: false, error: 'no-ai' };
      if (aiBusy) return { ok: false, error: 'busy' };
      const now = Date.now();
      if (now - aiLastAt < aiCooldownMs) return { ok: false, error: 'rate-limited' };
      const prompt = String((args && args.prompt) || '').trim();
      if (!prompt) return { ok: false, error: 'empty prompt' };
      if (prompt.length > AI_PROMPT_MAX) return { ok: false, error: 'prompt too long (max ' + AI_PROMPT_MAX + ')' };
      aiBusy = true; aiLastAt = now;
      try {
        const signal = (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) ? AbortSignal.timeout(20000) : undefined;
        const text = await aiComplete(prompt, { maxTokens: 256, signal });
        if (text == null || text === '') return { ok: false, error: 'no-ai' };
        return { ok: true, text: String(text).slice(0, AI_REPLY_MAX) };
      } catch (e) {
        return { ok: false, error: String((e && e.message) || e) };
      } finally {
        aiBusy = false;
      }
    }

    const abs = resolveAppPath(app.id, perms, args && args.path);
    if (!abs) return { ok: false, error: 'denied: path outside this app\'s reach' };

    // Through the core file layer like everything else; the answers keep the shape apps already rely on.
    return serial(async () => {
      try {
        if (method === 'fs.read') {
          const t = await fs.readText(abs);
          return t == null ? { ok: false, error: 'http-404' } : { ok: true, content: t };
        }
        if (method === 'fs.list') return { ok: true, entries: await fs.list(abs) };
        // fs.write — bounded on purpose: an app that needs megabytes is not a small app, it is a bug,
        // and the device pays for the mistake. Atomic, like every write of the core layer.
        const body = String((args && args.content) != null ? args.content : '');
        if (body.length > 256 * 1024) return { ok: false, error: 'too large (max 256 KB)' };
        await fs.write(abs, body);
        return { ok: true, bytes: body.length };
      } catch (e) {
        return { ok: false, error: (e && e.code) || String((e && e.message) || e) };
      }
    });
  }

  // The message handler the shell installs. IDENTITY IS BY SOURCE, never by origin: a sandboxed frame
  // has an opaque origin that arrives as the string "null", so e.origin proves nothing. findApp maps
  // e.source back to the window WE created — the only frames that exist.
  const onMessage = async function onMessage(e) {
    const d = e && e.data;
    if (!d || d.type !== 'nucleo.broker' || typeof d.method !== 'string') return;
    const app = findApp ? findApp(e.source) : null;
    const reply = (payload) => { try { e.source.postMessage({ type: 'nucleo.broker.reply', id: d.id, ...payload }, '*'); } catch {} };
    if (!app) { reply({ ok: false, error: 'unknown caller' }); return; }
    reply(await exec(app, d.method, d.args || {}));
  };
  onMessage.kvSeed = kvSeed;          // for the shell only (a property of the handler, not a broker method)
  return onMessage;
}

/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-assoc.js — which app opens which file, decided in one place for the whole OS.
//
// An app says which file types it opens in its manifest (handles.extensions); the shell gathers that from every
// installed app — system apps through app-catalog.json (tools/build.py), Store apps from their own installed
// manifest — and this module turns it, the device's own table (/api/associations) and the user's choices into
// two answers: the app a double-click opens, and the ordered list "Open with" offers. Pure: the shell feeds it
// data and keeps the user's choices; tests drive it directly (tests/nucleo-assoc.test.mjs).
//
// The order, and why (an OS that changes what a click does behind the user's back is not trustworthy):
//   1. the user's own choice ("Always use this app"), while that app is installed;
//   2. the device's and the OS's table (the defaults every edition ships);
//   3. a system app that declares the type;
//   4. a Store app that declares a type NO other app opens — a new kind of file gets its app as it is installed;
//   5. several Store apps and nothing above: nobody is guessed, the user is asked once ("Open with").
// A Store app never takes a type another app already opens, and never an ACTIVE type (pages, scripts, images
// that can carry script, shortcuts) — those it gets only when the user picks it, and shortcuts never.

// Types whose content can act (script, markup, a link the shell follows): never claimed by a Store app on its own.
export const ACTIVE_TYPES = new Set(['html', 'htm', 'xhtml', 'js', 'mjs', 'cjs', 'svg', 'sh', 'bat', 'cmd', 'ps1',
  'py', 'exe', 'com', 'jsdos', 'wasm', 'nap', 'lnk', 'url', 'desktop', 'webmanifest']);
// Handled by the shell itself: never an app's, whatever anyone declares or picks.
export const SHELL_TYPES = new Set(['lnk']);
const EXT_RE = /^[a-z0-9][a-z0-9_-]{0,15}$/;
const ROLE_RANK = { editor: 0, viewer: 1, player: 2 };

export function extOf(name) {
  const base = String(name == null ? '' : name).split('/').pop();
  const i = base.lastIndexOf('.');
  if (i <= 0 || i === base.length - 1) return '';
  const e = base.slice(i + 1).toLowerCase();
  return EXT_RE.test(e) ? e : '';
}
// A manifest's declared types, cleaned the same way the build cleans system apps' (tools/build.py).
export function declaredTypes(manifest) {
  const h = manifest && typeof manifest.handles === 'object' ? manifest.handles : null;
  const list = h && Array.isArray(h.extensions) ? h.extensions : [];
  const out = [];
  for (const raw of list) {
    const e = String(raw == null ? '' : raw).trim().toLowerCase().replace(/^\./, '');
    if (EXT_RE.test(e) && !out.includes(e)) out.push(e);
  }
  return out.slice(0, 64);
}

// apps: [{ id, name, store: bool, opens: [ext], role }] — every INSTALLED app (anything not here is not offered).
// table: { ext: appId } — the device's /api/associations merged over the OS seed.
// choices: { ext: appId } — the user's "Always use".
export function createAssoc({ apps = [], table = {}, choices = {} } = {}) {
  const byId = new Map(apps.filter((a) => a && typeof a.id === 'string').map((a) => [a.id, a]));
  const opens = (a, ext) => Array.isArray(a.opens) && a.opens.includes(ext);
  const installed = (id) => byId.has(id);

  function declarers(ext) {
    const sys = [], store = [];
    for (const a of byId.values()) if (opens(a, ext)) (a.store ? store : sys).push(a);
    const order = (x, y) => (ROLE_RANK[x.role] ?? 3) - (ROLE_RANK[y.role] ?? 3) || String(x.name || x.id).localeCompare(String(y.name || y.id));
    return { sys: sys.sort(order), store: store.sort(order) };
  }

  // { app, why } where why ∈ choice | table | system | store; or { app: null, ask: true } when the user must pick;
  // or { app: null } when nothing opens this type.
  function resolve(name) {
    const ext = extOf(name);
    if (!ext || SHELL_TYPES.has(ext)) return { app: null, ext };
    const c = choices[ext];
    if (c && installed(c)) return { app: byId.get(c), why: 'choice', ext };
    const t = table[ext];
    if (t && installed(t)) return { app: byId.get(t), why: 'table', ext };
    const { sys, store } = declarers(ext);
    if (sys.length) return { app: sys[0], why: 'system', ext };
    if (ACTIVE_TYPES.has(ext)) return store.length ? { app: null, ask: true, ext } : { app: null, ext };
    // A type the OS assigns to an app that is not installed here (pdf → pdf-editor) is not up for grabs: a Store app
    // that declares it is offered, never made the default by its absence.
    if (t) return store.length ? { app: null, ask: true, ext } : { app: null, ext };
    if (store.length === 1) return { app: store[0], why: 'store', ext };
    if (store.length > 1) return { app: null, ask: true, ext };
    return { app: null, ext };
  }

  // "Open with": the default first, then every other app that declares the type (system before Store), each once.
  function handlers(name) {
    const ext = extOf(name);
    if (!ext || SHELL_TYPES.has(ext)) return [];
    const r = resolve(name);
    const { sys, store } = declarers(ext);
    const list = [];
    const add = (a) => { if (a && !list.some((x) => x.id === a.id)) list.push(a); };
    add(r.app);
    const t = table[ext];
    if (t && installed(t)) add(byId.get(t));
    sys.forEach(add);
    store.forEach(add);
    return list.map((a) => ({ id: a.id, name: a.name || a.id, store: !!a.store, isDefault: !!r.app && a.id === r.app.id }));
  }

  // Whether the user may make `appId` the default for this type: an installed app, never for a shell type.
  function canChoose(name, appId) {
    const ext = extOf(name);
    return !!ext && !SHELL_TYPES.has(ext) && installed(appId);
  }

  // Whether os.openFile(path, { with: appId }) may launch that app: a system app opens what the user hands it; a
  // Store app only a type it declares, and never an active one (html, js, svg…) without the user's own choice.
  function canOpenWith(name, appId) {
    const ext = extOf(name);
    if (!ext || SHELL_TYPES.has(ext) || !installed(appId)) return false;
    const a = byId.get(appId);
    if (!a.store) return true;
    if (choices[ext] === appId) return true;
    return opens(a, ext) && !ACTIVE_TYPES.has(ext);
  }

  return { resolve, handlers, canChoose, canOpenWith, extOf };
}

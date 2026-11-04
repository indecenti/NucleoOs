/* SPDX-License-Identifier: AGPL-3.0-or-later */
// open-with.js — the shell's side of "which app opens this file": the facts nucleo-assoc.js decides from, the
// "Open with" sheet, and the user's choices. shell.js hands it what only the shell knows (the installed apps,
// the OS table, how to launch an app) and calls openFile() for every file the user opens anywhere: the desktop,
// File Commander, Recent, a shortcut, another app (nucleo-shell.js os.openFile / os.openWith / os.handlers).
//
// Where each fact comes from, on every edition:
//   system apps   app-catalog.json, written by tools/build.py from each manifest's handles.extensions;
//   Store apps    the manifest installed with the app (signed and checked by the Store at install), read where
//                 the edition keeps it: next to the served files, in paths.apps (Handheld, Web), in /apps (SD);
//   the OS table  the shell's seed merged with the device's /api/associations;
//   choices       /system/config/open-with.json, written only from this sheet, on the user's click.

import fs from './nucleo-fs.js';
import { createAssoc, extOf, declaredTypes } from './nucleo-assoc.js';

const CHOICES_PATH = '/system/config/open-with.json';
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

const CSS = `
.ow-scrim{position:fixed;inset:0;z-index:10070;background:#00000066;display:flex;align-items:center;justify-content:center;opacity:0;transition:opacity .15s ease;backdrop-filter:blur(2px)}
.ow-scrim.show{opacity:1}
.ow-box{width:420px;max-width:calc(100vw - 24px);max-height:calc(100vh - 24px);display:flex;flex-direction:column;background:var(--panel);color:var(--text);
  border:1px solid var(--line);border-radius:14px;box-shadow:0 28px 70px #000c;padding:16px;box-sizing:border-box;font-size:13px}
.ow-box h3{margin:0 0 2px;font-size:15px;font-weight:600}
.ow-file{color:var(--muted);font-size:12px;margin-bottom:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.ow-list{overflow-y:auto;border:1px solid var(--line);border-radius:10px;background:var(--bg2);padding:4px;outline:none;min-height:60px}
.ow-list:focus-visible{border-color:var(--accent)}
.ow-app{display:flex;align-items:center;gap:12px;padding:8px 10px;border-radius:8px;cursor:pointer}
.ow-app:hover{background:#ffffff0d}
.ow-app.on{background:color-mix(in srgb,var(--accent) 22%,transparent)}
.ow-app .ow-ic{width:28px;height:28px;flex:none;display:flex;align-items:center;justify-content:center;font-size:20px}
.ow-app .ow-ic img{width:28px;height:28px;object-fit:contain}
.ow-app .ow-n{flex:1;min-width:0}
.ow-app .ow-t{font-size:11px;color:var(--muted)}
.ow-none{color:var(--muted);padding:18px 12px;line-height:1.5}
.ow-always{display:flex;align-items:center;gap:8px;margin-top:12px;color:var(--text)}
.ow-row{display:flex;justify-content:flex-end;gap:8px;margin-top:14px}
@media (pointer:coarse){.ow-app{padding:12px 10px}}
`;
let styled = false;
function style() {
  if (styled) return;
  styled = true;
  try { const sh = new window.CSSStyleSheet(); sh.replaceSync(CSS); document.adoptedStyleSheets = [...document.adoptedStyleSheets, sh]; }
  catch { const st = document.createElement('style'); st.textContent = CSS; document.head.appendChild(st); }
}

// deps: apps() → installed app records ({ id, name, route, store }); table() → { ext: appId }; byId(id);
// launch(app, path); fallback() → the app that shows a file nobody opens (File Commander); appName(app);
// glyph(app) → icon HTML; t(key, vars); caps() → Caps.info(); fetchFn.
export function createOpenWith(deps) {
  const fetchFn = deps.fetchFn || ((...a) => fetch(...a));
  let catalog = null;                       // { appId: { opens: [ext], role } } for the apps the build ships
  const manifests = new Map();              // store app id + route → its declared types (or [])
  let choices = null;                       // { ext: appId }

  async function systemTypes() {
    if (catalog) return catalog;
    try {
      const r = await fetchFn('/app-catalog.json', { cache: 'no-cache' });
      const j = r.ok ? await r.json() : null;
      const out = {};
      if (j && j.apps) for (const [id, a] of Object.entries(j.apps)) if (Array.isArray(a.opens)) out[id] = { opens: declaredTypes({ handles: { extensions: a.opens } }), role: a.role };
      catalog = out;
    } catch { return {}; }                  // not cached: asked again next time
    return catalog;
  }
  async function storeTypes(app) {
    const key = app.id + '|' + (app.route || '');
    if (manifests.has(key)) return manifests.get(key);
    const tries = [];
    if (app.route) {
      // the manifest beside the served files (the P4 serves an app flat), or one level above www/ (the Web
      // edition's store proxy)
      tries.push(() => fetchFn(app.route + 'manifest.json', { cache: 'no-cache' }).then((r) => (r.ok ? r.json() : null)));
      if (/\/www\/$/.test(app.route)) tries.push(() => fetchFn(app.route.replace(/www\/$/, '') + 'manifest.json', { cache: 'no-cache' }).then((r) => (r.ok ? r.json() : null)));
    }
    tries.push(async () => {
      const info = deps.caps ? await deps.caps().catch(() => null) : null;
      const dir = info && info.paths && info.paths.apps;
      return dir ? fs.readJSON(dir + '/' + app.id + '/manifest.json', null) : null;
    });
    tries.push(() => fs.readJSON('/apps/' + app.id + '/manifest.json', null));
    let types = [];
    for (const tr of tries) {
      try { const m = await tr(); if (m && m.id === app.id) { types = declaredTypes(m); break; } } catch {}
    }
    manifests.set(key, types);
    return types;
  }
  async function loadChoices() {
    if (choices) return choices;
    const j = await fs.readJSON(CHOICES_PATH, {}).catch(() => ({}));
    choices = {};
    if (j && typeof j.choices === 'object') for (const [e, id] of Object.entries(j.choices)) if (/^[a-z0-9][a-z0-9_-]{0,15}$/.test(e) && typeof id === 'string') choices[e] = id;
    return choices;
  }
  async function saveChoice(ext, appId) {
    const c = await loadChoices();
    if (appId) c[ext] = appId; else delete c[ext];
    await fs.writeJSON(CHOICES_PATH, { schema: 1, choices: c }, { pretty: true });
  }

  async function engine() {
    const sys = await systemTypes();
    const list = [];
    for (const a of deps.apps()) {
      if (!a || !a.id) continue;
      const opens = a.store ? await storeTypes(a) : ((sys[a.id] && sys[a.id].opens) || []);
      list.push({ id: a.id, name: deps.appName(a), store: !!a.store, opens, role: a.store ? 'viewer' : (sys[a.id] && sys[a.id].role) });
    }
    return createAssoc({ apps: list, table: deps.table(), choices: await loadChoices() });
  }

  function launch(app, path) { deps.launch(app, path); }

  // The sheet: pick an app for `path`; "Always use" makes it the default for that type. Resolves the app or null.
  function choose(path, handlers, { always = false } = {}) {
    style();
    const t = deps.t;
    const ext = extOf(path);
    const name = String(path).split('/').pop();
    return new Promise((resolve) => {
      const scrim = document.createElement('div');
      scrim.className = 'ow-scrim';
      const box = document.createElement('div');
      box.className = 'ow-box';
      box.setAttribute('role', 'dialog');
      box.setAttribute('aria-modal', 'true');
      const rows = handlers.map((h, i) => {
        const app = deps.byId(h.id);
        const tag = h.isDefault ? t('ow_default') : h.store ? t('ow_store') : '';
        return `<div class="ow-app${i === 0 ? ' on' : ''}" role="option" data-i="${i}" aria-selected="${i === 0}">` +
          `<span class="ow-ic">${app ? deps.glyph(app) : ''}</span><span class="ow-n">${esc(h.name)}${tag ? `<div class="ow-t">${esc(tag)}</div>` : ''}</span></div>`;
      }).join('');
      box.innerHTML = `<h3>${esc(t('ow_title'))}</h3><div class="ow-file" title="${esc(path)}">${esc(name)}</div>` +
        (handlers.length
          ? `<div class="ow-list" role="listbox" tabindex="0" aria-label="${esc(t('ow_title'))}">${rows}</div>` +
            (ext ? `<label class="ow-always"><input type="checkbox" class="ow-chk"${always ? ' checked' : ''}> ${esc(t('ow_always', { ext }))}</label>` : '')
          : `<div class="ow-none">${esc(t('ow_none', { ext: ext || '?' }))}</div>`) +
        `<div class="ow-row">` +
          (handlers.length ? '' : `<button type="button" class="os-dialog-btn ow-folder">${esc(t('ow_show_folder'))}</button>`) +
          `<button type="button" class="os-dialog-btn ow-cancel">${esc(t('cancel'))}</button>` +
          (handlers.length ? `<button type="button" class="os-dialog-btn primary ow-ok">${esc(t('ow_open'))}</button>` : '') +
        `</div>`;
      scrim.appendChild(box);
      document.body.appendChild(scrim);
      requestAnimationFrame(() => scrim.classList.add('show'));
      let sel = 0, done = false;
      const items = [...box.querySelectorAll('.ow-app')];
      const mark = (i) => { sel = i; items.forEach((el, k) => { el.classList.toggle('on', k === i); el.setAttribute('aria-selected', String(k === i)); }); if (items[i]) items[i].scrollIntoView({ block: 'nearest' }); };
      const close = async (pick) => {
        if (done) return; done = true;
        document.removeEventListener('keydown', onKey, true);
        scrim.classList.remove('show');
        setTimeout(() => scrim.remove(), 160);
        if (pick != null && handlers[pick]) {
          const chk = box.querySelector('.ow-chk');
          if (chk && chk.checked && ext) await saveChoice(ext, handlers[pick].id).catch(() => {});
          resolve(deps.byId(handlers[pick].id) || null);
        } else resolve(null);
      };
      items.forEach((el, i) => {
        el.addEventListener('click', () => mark(i));
        el.addEventListener('dblclick', () => close(i));
      });
      const ok = box.querySelector('.ow-ok');
      if (ok) ok.addEventListener('click', () => close(sel));
      box.querySelector('.ow-cancel').addEventListener('click', () => close(null));
      const folder = box.querySelector('.ow-folder');
      if (folder) folder.addEventListener('click', () => { close(null); const fb = deps.fallback(); if (fb) deps.launch(fb, String(path).replace(/\/[^/]*$/, '') || '/data', { folder: true }); });
      scrim.addEventListener('pointerdown', (e) => { if (e.target === scrim) close(null); });
      function onKey(e) {
        if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(null); return; }
        if (!items.length) return;
        if (e.key === 'ArrowDown') { e.preventDefault(); mark(Math.min(items.length - 1, sel + 1)); }
        else if (e.key === 'ArrowUp') { e.preventDefault(); mark(Math.max(0, sel - 1)); }
        else if (e.key === 'Enter' && !(e.target && e.target.tagName === 'BUTTON')) { e.preventDefault(); e.stopPropagation(); close(sel); }
      }
      document.addEventListener('keydown', onKey, true);
      const list = box.querySelector('.ow-list');
      (list || box.querySelector('.ow-cancel')).focus();
    });
  }

  return {
    // openFile(path, { with: appId, choose: true }): with → that app if installed and allowed; choose → the sheet.
    async openFile(path, opts = {}) {
      const eng = await engine();
      if (opts.with) {
        const app = deps.byId(opts.with);
        if (app && eng.canOpenWith(path, app.id)) { launch(app, path); return true; }
      }
      const r = eng.resolve(path);
      if (!opts.choose && r.app) { launch(deps.byId(r.app.id) || r.app, path); return true; }
      const list = eng.handlers(path);
      if (!opts.choose && !list.length) {
        const fb = deps.fallback();
        if (fb) { deps.launch(fb, path); return true; }
        return false;
      }
      const app = await choose(path, list, { always: !!r.ask });
      if (app) { launch(app, path); return true; }
      return false;
    },
    // The apps that open `path`, default first: [{ id, name, store, isDefault }].
    async handlers(path) { return (await engine()).handlers(path); },
    async defaultApp(path) { const r = (await engine()).resolve(path); return r.app ? r.app.id : null; },
    // The user removes a default ("Reset"): next time the OS table or the sheet decides.
    async forget(ext) { await saveChoice(ext, null); },
    // Apps were installed or removed: Store manifests are read again.
    invalidate() { manifests.clear(); catalog = null; },
  };
}

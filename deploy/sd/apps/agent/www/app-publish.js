// app-publish.js — the PURE core for the agent's "create a NucleoOS app" skill.
//
// No DOM, no fetch: runs in the browser AND under Node, so the manifest schema, id rules, registry
// upsert and the starter scaffold are HOST-TESTABLE (tools/anima-host/app-publish-check.mjs). runtime.js
// does the privileged I/O (write to /apps + /system/registry via /api/fs) using these building blocks.
//
// The flow it powers: scaffold_app → (agent edits www/index.html) → publish_app. A new app installs OTA
// (the firmware reloads the registry and fires apps.changed → the shell hot-reloads, no reboot — see
// nucleo_fsapi.c write_post). Everything is validated here BEFORE a single system path is touched.

export const CATEGORIES = ['tools', 'productivity', 'media', 'system', 'connectivity', 'games'];

// kebab-case, 2..24 chars, starts with a letter. Lowercases and dash-joins free text so "My Notes!" → "my-notes".
export function sanitizeId(raw) {
  let s = String(raw == null ? '' : raw).trim().toLowerCase()
    .replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').replace(/-{2,}/g, '-');
  if (s && !/^[a-z]/.test(s)) s = 'app-' + s;
  return s.slice(0, 24).replace(/-+$/g, '');
}
export function isValidId(id) { return /^[a-z][a-z0-9-]{1,23}$/.test(String(id || '')); }

// Build a FULL, schema-complete manifest (docs/app-manifest.md) from a tiny spec, so the model only has to
// supply id/name/description/category and we fill the boilerplate the firmware + validate.mjs expect.
export function buildManifest(spec = {}) {
  const id = sanitizeId(spec.id || spec.name || '');
  const category = CATEGORIES.includes(spec.category) ? spec.category : 'tools';
  return {
    id,
    name: String(spec.name || id || 'App').slice(0, 32),
    version: spec.version || '0.1.0',
    version_code: spec.version_code || 1,
    min_os: '0.1.0',
    platforms: ['esp32s3'],
    description: String(spec.description || '').slice(0, 160) || 'App created by NucleoOS Agents.',
    category,
    runtime: 'web',
    entry_service: 'none',
    web_route: '/apps/' + id + '/',
    icon: spec.icon ? String(spec.icon) : 'icon.svg',
    permissions: Array.isArray(spec.permissions) ? spec.permissions : [],
    mounts: spec.mounts && typeof spec.mounts === 'object' ? spec.mounts : {},
    handles: { role: 'none', extensions: [] },
    subscribes: [],
    publishes: [],
    power: { budget_class: 'low', wants_wakeup: ['keyboard'] },
    mesh: { exposes: [], consumes: [] },
    created_by: 'agent',   // marks an agent-authored app so publish_app may safely UPDATE it (never a bundled one)
  };
}

// The minimum a manifest must have for the device registry to load it and the shell to show it.
const REQUIRED = ['id', 'name', 'version', 'web_route', 'runtime', 'category'];
export function validateManifest(m) {
  const errors = [];
  if (!m || typeof m !== 'object') return { ok: false, errors: ['manifest non è un oggetto'] };
  for (const k of REQUIRED) if (m[k] == null || m[k] === '') errors.push('campo mancante: ' + k);
  if (m.id && !isValidId(m.id)) errors.push('id non valido (kebab-case, inizia con lettera, 2-24 char): ' + m.id);
  if (m.web_route && m.id && m.web_route !== '/apps/' + m.id + '/') errors.push('web_route deve essere /apps/' + m.id + '/');
  if (m.category && !CATEGORIES.includes(m.category)) errors.push('category deve essere una di: ' + CATEGORIES.join(', '));
  if (m.runtime && m.runtime !== 'web') errors.push('solo runtime "web" è installabile a runtime');
  return { ok: errors.length === 0, errors };
}

// Scaffold KINDS: the agent picks a working starter to build on, not a blank page. Each is a complete,
// self-contained, i18n-wired app — so a fresh scaffold runs immediately and the model has a real baseline.
export const APP_KINDS = ['blank', 'list', 'timer', 'converter'];
export function normKind(k) { return APP_KINDS.includes(k) ? k : 'blank'; }

// Shared head/style + the i18n boot, so every kind looks consistent and stays valid. body/script are the
// only per-kind parts. The inline script is an ES module (imports the OS i18n) — same shape as before.
function htmlShell(spec, bodyInner, scriptInner) {
  const name = String(spec.name || 'App').replace(/[<&]/g, '');
  const id = sanitizeId(spec.id || name);
  return `<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title data-i18n="title">${name}</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 18px; background: #0e1830; color: #e8eefc; font: 16px/1.5 system-ui, -apple-system, "Segoe UI", sans-serif; }
  h1 { margin: 0 0 14px; font-size: 22px; }
  .card { background: #16223f; border: 1px solid #243352; border-radius: 12px; padding: 16px; }
  input, select { font: inherit; background: #0e1830; color: #e8eefc; border: 1px solid #2c3d63; border-radius: 8px; padding: 8px 10px; }
  button { font: inherit; background: #3a6df0; color: #fff; border: 0; border-radius: 10px; padding: 9px 15px; cursor: pointer; }
  button:active { transform: translateY(1px); }
  ul { list-style: none; padding: 0; margin: 12px 0 0; } li { padding: 8px 2px; border-bottom: 1px solid #243352; cursor: pointer; }
  .big { font-size: 40px; font-weight: 700; letter-spacing: 1px; margin: 14px 0; }
  .row { display: flex; gap: 8px; flex-wrap: wrap; align-items: center; }
</style>
</head>
<body>
  <h1 data-i18n="title">${name}</h1>
  <div class="card">
${bodyInner}
  </div>
  <script type="module">
    import I18N from '/nucleo-i18n.js';
    const t = await I18N.init('${id}');
${scriptInner}
  </script>
</body>
</html>
`;
}

const KIND_BODY = {
  blank: `    <p data-i18n="hello">Ciao da NucleoOS!</p>
    <button id="go" data-i18n="action">Avvia</button>`,
  list: `    <div class="row"><input id="inp" style="flex:1"><button id="add" data-i18n="add">Aggiungi</button></div>
    <ul id="list"></ul>`,
  timer: `    <div class="row"><input id="min" type="number" min="1" value="5" style="width:90px"><span data-i18n="minutes">minuti</span></div>
    <div class="big" id="disp">05:00</div>
    <div class="row"><button id="start" data-i18n="start">Avvia</button><button id="reset" data-i18n="reset">Azzera</button></div>`,
  converter: `    <div class="row"><input id="val" type="number" value="20" style="width:120px">
      <select id="from"><option value="C">°C</option><option value="F">°F</option></select><span>&rarr;</span>
      <select id="to"><option value="F">°F</option><option value="C">°C</option></select></div>
    <div class="big" id="out">68 °F</div>`,
};
const KIND_SCRIPT = {
  blank: `    document.getElementById('go').addEventListener('click', () => {
      // TODO: la logica dell'app
    });`,
  list: `    const KEY = 'items', list = document.getElementById('list'), inp = document.getElementById('inp');
    const items = JSON.parse(localStorage.getItem(KEY) || '[]');
    inp.placeholder = t('placeholder');
    const save = () => localStorage.setItem(KEY, JSON.stringify(items));
    function render() {
      list.innerHTML = '';
      if (!items.length) { const li = document.createElement('li'); li.textContent = t('empty'); list.appendChild(li); return; }
      items.forEach((txt, i) => { const li = document.createElement('li'); li.textContent = txt; li.addEventListener('click', () => { items.splice(i, 1); save(); render(); }); list.appendChild(li); });
    }
    function add() { const v = inp.value.trim(); if (!v) return; items.push(v); inp.value = ''; save(); render(); }
    document.getElementById('add').addEventListener('click', add);
    inp.addEventListener('keydown', (e) => { if (e.key === 'Enter') add(); });
    render();`,
  timer: `    let left = 300, h = null;
    const disp = document.getElementById('disp'), min = document.getElementById('min');
    const fmt = (s) => { const m = Math.floor(s / 60), x = s % 60; return (m < 10 ? '0' : '') + m + ':' + (x < 10 ? '0' : '') + x; };
    const show = () => { disp.textContent = fmt(left); };
    const target = () => Math.max(1, (+min.value || 5)) * 60;
    function tick() { if (left > 0) { left--; show(); } else { clearInterval(h); h = null; } }
    document.getElementById('start').addEventListener('click', () => { if (h) return; if (left <= 0) left = target(); h = setInterval(tick, 1000); });
    document.getElementById('reset').addEventListener('click', () => { clearInterval(h); h = null; left = target(); show(); });
    min.addEventListener('change', () => { if (!h) { left = target(); show(); } });
    show();`,
  converter: `    const val = document.getElementById('val'), from = document.getElementById('from'), to = document.getElementById('to'), out = document.getElementById('out');
    function conv() {
      const v = +val.value || 0, c = (from.value === 'C') ? v : (v - 32) * 5 / 9;
      const r = (to.value === 'C') ? c : c * 9 / 5 + 32;
      out.textContent = (Math.round(r * 10) / 10) + ' °' + to.value;
    }
    [val, from, to].forEach((el) => el.addEventListener('input', conv));
    conv();`,
};

// A valid, self-contained starter page for the chosen kind (default 'blank'). The agent edits it into the
// real app; it exists so a fresh scaffold is immediately installable and the model has a working baseline.
export function starterHtml(spec = {}) {
  const kind = normKind(spec.kind);
  return htmlShell(spec, KIND_BODY[kind], KIND_SCRIPT[kind]);
}

// A tiny rounded-tile icon with the app's initial, so a fresh app looks complete in the launcher.
export function starterIcon(spec = {}) {
  const ch = (String(spec.name || spec.id || 'A').trim()[0] || 'A').toUpperCase().replace(/[<&]/g, 'A');
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64">
  <rect x="4" y="4" width="56" height="56" rx="14" fill="#3a6df0"/>
  <text x="32" y="42" font-family="system-ui,sans-serif" font-size="32" font-weight="700" fill="#fff" text-anchor="middle">${ch}</text>
</svg>
`;
}

const KIND_I18N = {
  it: { blank: { hello: 'Ciao da NucleoOS!', action: 'Avvia' }, list: { add: 'Aggiungi', placeholder: 'Nuovo elemento…', empty: 'Nessun elemento' }, timer: { start: 'Avvia', reset: 'Azzera', minutes: 'minuti' }, converter: {} },
  en: { blank: { hello: 'Hello from NucleoOS!', action: 'Start' }, list: { add: 'Add', placeholder: 'New item…', empty: 'No items' }, timer: { start: 'Start', reset: 'Reset', minutes: 'minutes' }, converter: {} },
};
export function starterI18n(spec, en) {
  const kind = normKind(spec && spec.kind);
  const base = { _lang: en ? 'en' : 'it', title: String((spec && spec.name) || 'App') };
  return JSON.stringify(Object.assign(base, KIND_I18N[en ? 'en' : 'it'][kind] || {}), null, 2);
}

// Upsert an app entry into the registry doc (the {schema, installed:[…]} shape). Returns a NEW doc (never
// mutates the input). Matches by id; preserves order; a fresh app is appended. The caller decides whether
// an existing entry may be replaced (only agent-authored apps — see runtime.js).
export function upsertRegistry(reg, entry) {
  const doc = reg && typeof reg === 'object' ? { ...reg } : {};
  if (typeof doc.schema !== 'number') doc.schema = 1;
  const installed = Array.isArray(doc.installed) ? doc.installed.slice() : [];
  const i = installed.findIndex((a) => a && a.id === entry.id);
  if (i >= 0) installed[i] = { ...installed[i], ...entry };
  else installed.push(entry);
  doc.installed = installed;
  return doc;
}

// The registry entry for an installed app (minimal shape the firmware reads).
export function registryEntry(manifest) {
  return {
    id: manifest.id,
    version: manifest.version || '0.1.0',
    path: '/apps/' + manifest.id,
    enabled: true,
    autostart: false,
    permissions: Array.isArray(manifest.permissions) ? manifest.permissions : [],
    created_by: 'agent',
  };
}

export function isAgentApp(regEntry) { return !!(regEntry && regEntry.created_by === 'agent'); }

// Extract the INLINE, checkable scripts from an HTML page: skips external (src=…) scripts, non-JS types
// (application/json, importmap), and ES-module bodies (top-level import/export) — which the script-body
// syntax checker can't validate without false-alarming. Returns the raw code of each checkable block.
export function inlineScripts(html) {
  const out = [];
  const re = /<script\b([^>]*)>([\s\S]*?)<\/script>/gi;
  let m;
  while ((m = re.exec(String(html == null ? '' : html)))) {
    const attrs = m[1] || '', code = m[2] || '';
    if (/\bsrc\s*=/i.test(attrs)) continue;                                   // external script — not our code
    const type = (attrs.match(/\btype\s*=\s*["']?([^"'\s>]+)/i) || [])[1];
    if (type && !/^(text\/javascript|module|application\/javascript)$/i.test(type)) continue;  // json/importmap/etc
    if (/^\s*(import|export)\s/m.test(code)) continue;                        // ES module body — checker would false-alarm
    if (code.trim()) out.push(code);
  }
  return out;
}

// PRE-PUBLISH QUALITY GATE (pure; checkSyntax injected, host-safe). Refuses to install an app whose code
// is broken — a malformed i18n JSON breaks I18N.init, a broken .js/inline-script breaks the app. Returns
// { ok, errors[] }. Non-code files pass through. Mirrors verifyCode's module-aware caveat.
export function lintApp(files, checkSyntax) {
  const errors = [];
  for (const f of (files || [])) {
    const path = String(f.path || ''), content = String(f.content == null ? '' : f.content);
    const ext = (/\.([a-z0-9]+)$/i.exec(path) || [])[1] || '';
    if (ext === 'json') {
      try { JSON.parse(content); } catch (e) { errors.push(path + ': JSON non valido (' + String((e && e.message) || e) + ')'); }
    } else if ((ext === 'js' || ext === 'mjs' || ext === 'cjs') && typeof checkSyntax === 'function') {
      if (/^\s*(import|export)\s/m.test(content)) continue;                   // module body — skip (see verifyCode)
      const r = checkSyntax(content);
      if (r && !r.ok) errors.push(path + ': errore di sintassi' + (r.line ? ' (riga ' + r.line + ')' : '') + (r.error ? ': ' + r.error : ''));
    } else if (ext === 'html' || ext === 'htm') {
      if (typeof checkSyntax === 'function') for (const code of inlineScripts(content)) {
        const r = checkSyntax(code);
        if (r && !r.ok) { errors.push(path + ': <script> inline con errore di sintassi' + (r.line ? ' (riga ~' + r.line + ')' : '') + (r.error ? ': ' + r.error : '')); break; }
      }
    }
  }
  return { ok: errors.length === 0, errors };
}

// Total bytes an agent-installed app may write to the device (sum of its www/* files). The Cardputer has
// little room and the SD is shared; a runaway generation must not flood it. Generous for a real web app.
export const MAX_APP_BYTES = 512 * 1024;

// Defense-in-depth path guard for the privileged copy into /apps/<id>/www/. The firmware's resolve_path
// already rejects ".." server-side, but we refuse it here too so a bad relative path never even leaves the
// browser. `relUnder` is a path UNDER www/ (e.g. "index.html" or "js/app.js"). Returns the SD-absolute
// destination, or null if it is unsafe (traversal, absolute, backslash, empty).
export function safeAppFilePath(id, relUnder) {
  if (!isValidId(id)) return null;
  const r = String(relUnder == null ? '' : relUnder).trim();
  if (!r) return null;
  if (r[0] === '/' || r.indexOf('\\') >= 0) return null;
  if (r.split('/').some((seg) => seg === '..' || seg === '.' || seg === '')) return null;
  return '/apps/' + id + '/www/' + r;
}

// THE single, PURE decision for "may I update the registry, and what's the new doc?". Centralises the two
// anti-destructive guards so they are host-testable and can never drift:
//   • a NULL / malformed existing registry → REFUSE (a blind rewrite would wipe every other app — the
//     registry is a system file that always exists; a null read means a transient failure, not "empty").
//   • an id already owned by a SYSTEM (non-agent) app → REFUSE (never clobber a bundled app).
// Otherwise returns { ok:true, doc, updating } where doc preserves all other entries (upsert by id).
export function planRegistryUpdate(existingReg, manifest) {
  if (!existingReg || !Array.isArray(existingReg.installed)) return { ok: false, reason: 'registry-unreadable' };
  const existing = existingReg.installed.find((a) => a && a.id === manifest.id);
  if (existing && !isAgentApp(existing)) return { ok: false, reason: 'system-id' };
  return { ok: true, updating: !!existing, doc: upsertRegistry(existingReg, registryEntry(manifest)) };
}

// Enable/disable an AGENT-created app in the registry (the only safe way to "undo" an install — the
// firmware blocks delete on /apps). PURE + symmetric to planRegistryUpdate: refuses an unreadable
// registry, a missing id, and a SYSTEM (non-agent) app (the agent must never hide a bundled app).
// Returns { ok, was, doc } (doc preserves all other entries) or { ok:false, reason }.
export function planRegistrySetEnabled(existingReg, id, enabled) {
  if (!existingReg || !Array.isArray(existingReg.installed)) return { ok: false, reason: 'registry-unreadable' };
  const entry = existingReg.installed.find((a) => a && a.id === id);
  if (!entry) return { ok: false, reason: 'not-found' };
  if (!isAgentApp(entry)) return { ok: false, reason: 'system-id' };
  const doc = { ...existingReg, installed: existingReg.installed.map((a) => (a && a.id === id ? { ...a, enabled: !!enabled } : a)) };
  return { ok: true, was: !!entry.enabled, doc };
}

/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-myapps.js — the person's own apps: small web apps written on request ("write me an app that…"),
// installed in the person's private folder and run like any app, on every edition.
//
// The model only PROPOSES a page. Everything that decides is here, in plain code: the review (no network, no
// browser storage, no /api, no imports, no eval…), the install (a folder of its own, versions that only go up,
// never a system or store app), the record Start shows, the document the window runs, the removal (to the bin).
//
//   import myapps from '/nucleo-myapps.js';
//   const plan = myapps.plan('scrivi un\'app hello world');         // { op: 'create', request } | { op: 'remove', name } | null
//   const pkg = myapps.parse(await llm(myapps.prompt(request)))     // a model's answer → a package, or null
//            || myapps.template(request, lang);                     // no model: the built-in hello world
//   const r = myapps.review(pkg);                                   // { ok, errors: [{ code, detail }] }
//   await myapps.install(pkg);                                      // { id, version_code, updated }
//   await myapps.remove(id);                                        // index first, then the folder goes to the bin
//
// How such an app runs (docs/myapps.md): its window loads /local-app.html (a shell page with its own tight CSP:
// inline scripts, nothing from the network) in a frame without an origin; the page asks the broker for its
// document (method app.doc), which the shell composes here from the files on the device, and writes it. The app
// then reaches the OS only through /nucleo-app.js, like a store app. No backend route: only /api/fs, which every
// edition and every firmware already has.

import fs from './nucleo-fs.js';

export const DIR = '/data/my-apps';                       // OS_PRIVATE (appbroker.js): no app reaches it, the Open sheet hides it
const INDEX = DIR + '/index.json';
export const PREFIX = 'my.';                              // a dot no store or built-in id can have: never the id of another app
const ID_RE = /^my\.[a-z0-9][a-z0-9-]{0,39}$/;
export const LANGS = ['it', 'en', 'es', 'fr', 'de'];
export const CATEGORIES = ['productivity', 'media', 'games', 'learn', 'tools', 'other'];
const BRIDGE = '/nucleo-app.js';
const LIMITS = { html: 96 * 1024, strings: 16 * 1024, apps: 200 };

// ── words ────────────────────────────────────────────────────────────────────────────────────────────
// A request to MAKE an app: a verb of making, then "a/an/un/una/eine…" and the word "app" (or program), in five
// languages. "installa l'app meteo" is the Store's business and "make my app faster" is not a new app: a verb
// that only installs, or no article, is not enough. A how-to question ("come si crea un'app?") is for the model.
const MAKE = '(crea|creami|crearmi|scrivi|scrivimi|fai|fammi|sviluppa|sviluppami|programma|programmami|costruisci|costruiscimi|genera|generami|realizza|prepara|preparami' +
  '|make|create|write|build|generate|develop|craft' +
  '|haz|hazme|escribe|escr[ií]beme|desarrolla|construye|programa|cr[ée]ame' +
  '|cr[ée]e|[ée]cris|fais|d[ée]veloppe|g[ée]n[èe]re|construis|programme' +
  '|erstelle|erstell|schreib|schreibe|mach|mache|baue|bau|entwickle|generiere|programmiere)';
const ART = "(un|una|uno|un'|a|an|me an?|mi|une|moi une?|eine|einen|ein|mir eine?n?)";
const APPW = '(app|apps|applicazione|applicazioncina|programm(?:a|ino)|program|application|aplicaci[oó]n|aplicacion|programa|appli|programme|anwendung|programm)';
const MAKE_RE = new RegExp('\\b' + MAKE + '\\b[^.?!\\n]{0,40}?(?:\\b|^|\\s)' + ART + "(?:\\s|'|’)[^.?!\\n]{0,30}?\\b" + APPW + '\\b', 'i');
const HOWTO_RE = /^\s*(come|how|c[oó]mo|comment|wie|perch[ée]|why|por qu[ée]|pourquoi|warum)\b/i;
const GAME_RE = /\b(gioco|giochino|videogioco|game|juego|videojuego|jeu|spiel|videospiel)\b/i;
const REMOVE_RE = new RegExp('\\b(rimuovi|elimina|disinstalla|cancella|togli|remove|delete|uninstall|borra|desinstala|quita|supprime|d[ée]sinstalle|enl[èe]ve|entferne|l[öo]sche|deinstalliere)\\b' +
  '[^.?!\\n]{0,24}?\\b' + APPW + '\\b\\s*(.*)$', 'i');

// What the person asked for, or null when it is not about their own apps.
export function plan(q) {
  const s = String(q == null ? '' : q).trim();
  if (!s || s.length > 2000) return null;
  const rm = REMOVE_RE.exec(s);
  if (rm) {
    const name = rm[3].replace(/^(che|chiamata|named|called|llamada|nomm[ée]e|namens|«|"|“)\s*/i, '').replace(/[«»"“”.!?]+/g, '').trim();
    return name ? { op: 'remove', name } : null;
  }
  if (MAKE_RE.test(s) && !HOWTO_RE.test(s) && !GAME_RE.test(s.replace(/\b(app|application|aplicaci[oó]n|anwendung)\b.*$/i, ''))) return { op: 'create', request: s };
  return null;
}

// The text the person put in quotes ("che stampa «Ciao»"), if any.
function quoted(q) {
  const m = /[«"“„']([^«»"“”„']{1,120})[»"”“']/.exec(String(q || ''));
  return m ? m[1].trim() : '';
}

// ── ids ──────────────────────────────────────────────────────────────────────────────────────────────
function slug(text) {
  return String(text == null ? '' : text).normalize('NFKD').replace(/[̀-ͯ]/g, '').toLowerCase()
    .replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 32).replace(/-+$/g, '');
}
export function idFor(name) {
  const s = slug(name);
  return PREFIX + (s || 'app');
}
export const isMyApp = (id) => ID_RE.test(String(id || ''));

// ── the package ──────────────────────────────────────────────────────────────────────────────────────
// { name, names: {lang: name}, description, category, html, strings: {lang: {key: text}} } — all the model writes.
function cleanName(v) { return String(v == null ? '' : v).replace(/[\u0000-\u001f\u007f<>&"]/g, '').replace(/\s+/g, ' ').trim().slice(0, 40); }
function cleanStrings(src) {
  const out = {};
  for (const l of LANGS) {
    const o = src && typeof src === 'object' && src[l] && typeof src[l] === 'object' && !Array.isArray(src[l]) ? src[l] : {};
    out[l] = {};
    for (const [k, v] of Object.entries(o)) if (/^[A-Za-z][A-Za-z0-9_.-]{0,40}$/.test(k) && (typeof v === 'string' || typeof v === 'number')) out[l][k] = String(v).slice(0, 600);
  }
  // A language the model left out shows English (then Italian): every app speaks all five, never a raw key.
  const base = Object.keys(out.en).length ? out.en : out.it;
  for (const l of LANGS) for (const [k, v] of Object.entries(base)) if (!(k in out[l])) out[l][k] = v;
  return out;
}
function normalize(pkg) {
  const p = pkg && typeof pkg === 'object' ? pkg : {};
  const strings = cleanStrings(p.strings);
  const names = {};
  for (const l of LANGS) names[l] = cleanName((p.names && p.names[l]) || strings[l].title || p.name) || '';
  const name = cleanName(p.name) || names.en || names.it || 'App';
  for (const l of LANGS) if (!names[l]) names[l] = name;
  const cat = String(p.category || '').toLowerCase().trim();
  // A <link> (a web font, a favicon) is what small models add most and it could only fail here: dropped, not refused.
  const html = String(p.html == null ? '' : p.html).replace(/<\s*link\b[^>]*>/gi, '');
  return { name, names, description: String(p.description || '').replace(/[\u0000-\u001f]/g, ' ').slice(0, 160),
    category: CATEGORIES.includes(cat) ? cat : 'tools', html, strings };
}

// ── the review ───────────────────────────────────────────────────────────────────────────────────────
// Everything an app of the person may NOT do, checked in its source before a byte reaches the device. The page
// it runs in enforces most of it again (CSP: connect-src 'none', no remote script, image or style; no origin, so
// no storage and no cookie), so a rule here that a clever page slips past still meets a wall; the review exists
// so the person never installs an app that would fail on that wall, and so the reason is said in words.
const RULES = [
  ['network', /\bfetch\s*\(|\bXMLHttpRequest\b|\bWebSocket\b|\bEventSource\b|\bsendBeacon\b|\bRTCPeerConnection\b|\bWebTransport\b/],
  ['worker', /\bnew\s+(Shared)?Worker\b|\bimportScripts\b|\bserviceWorker\b/],
  ['storage', /\b(localStorage|sessionStorage|indexedDB)\b|\bdocument\s*\.\s*cookie\b|\bcaches\s*\.\s*open\b/],
  ['api', /\/api\//],
  ['module', /\bimport\s*\(|^\s*import\s+[\w{*'"]|^\s*export\s+(default|const|function|let|class|\{)/m],
  ['eval', /\beval\s*\(|\bnew\s+Function\s*\(|\bset(Timeout|Interval)\s*\(\s*['"`]/],
  ['navigation', /\bwindow\s*\.\s*open\s*\(|\blocation\s*(=[^=]|\.\s*(href\s*=[^=]|(assign|replace)\b))|\bdocument\s*\.\s*domain\b|\bhistory\s*\.\s*(push|replace)State\b/],
  ['parent', /\b(window\s*\.\s*)?(parent|top|opener)\s*\.\s*(postMessage|document|location|frames|NucleoApp)\b|\bframeElement\b/],
  ['remote', /\b(https?|wss?|ftp):\/\/(?!www\.w3\.org\/)|\b(src|href|action|poster|data)\s*=\s*["']?\s*\/\/|url\(\s*["']?\s*\/\//i],
  ['element', /<\s*(iframe|frame|frameset|object|embed|applet|portal|link|base)\b|<\s*meta\b[^>]*http-equiv|<\s*form\b[^>]*\baction\s*=/i],
  ['script_src', /<\s*script\b[^>]*\bsrc\s*=(?!\s*["']?\/nucleo-app\.js["'\s>])/i],
];
// Inline scripts the syntax checker should see (not JSON/importmap/template blocks).
function inlineScripts(html) {
  const out = [];
  const re = /<script\b([^>]*)>([\s\S]*?)<\/script\s*>/gi;
  let m;
  while ((m = re.exec(String(html == null ? '' : html)))) {
    const attrs = m[1] || '';
    if (/\bsrc\s*=/i.test(attrs)) continue;
    const type = (attrs.match(/\btype\s*=\s*["']?([^"'\s>]+)/i) || [])[1];
    if (type && !/^(text\/javascript|module|application\/javascript)$/i.test(type)) continue;
    if (m[2].trim()) out.push({ code: m[2], module: /^module$/i.test(type || '') });
  }
  return out;
}
// review(pkg, { checkSyntax }) → { ok, errors: [{ code, detail }] }. checkSyntax(code, { module }) → { ok, error? }
// is optional (a parser the caller has, acorn in ANIMA): without one the page's own error shows when it runs.
export function review(pkg, { checkSyntax } = {}) {
  const p = normalize(pkg);
  const errors = [];
  const add = (code, detail) => { if (!errors.some((e) => e.code === code)) errors.push({ code, detail: String(detail || '').slice(0, 120) }); };
  const html = p.html;
  if (!/<\s*(body|div|main|p|h1|button|canvas|section)\b/i.test(html)) add('no_html');
  if (html.length > LIMITS.html) add('too_large', Math.round(html.length / 1024) + ' KB');
  for (const l of LANGS) if (JSON.stringify(p.strings[l]).length > LIMITS.strings) add('too_large', 'strings ' + l);
  for (const [code, re] of RULES) { const m = re.exec(html); if (m) add(code, m[0].trim()); }
  // Every [data-i18n] key has a string (English at least): a missing one would show the raw key on screen.
  const keys = new Set();
  for (const m of html.matchAll(/\bdata-i18n\s*=\s*["']([^"']+)["']/gi)) keys.add(m[1]);
  const missing = [...keys].filter((k) => !(k in p.strings.en));
  if (missing.length) add('strings', missing.slice(0, 5).join(', '));
  if (typeof checkSyntax === 'function') {
    for (const s of inlineScripts(html)) {
      let r = null;
      try { r = checkSyntax(s.code, { module: s.module }); } catch { r = null; }
      if (r && r.ok === false) { add('syntax', r.error || ''); break; }
    }
  }
  return { ok: errors.length === 0, errors };
}

// ── the document the window runs ─────────────────────────────────────────────────────────────────────
// The app's page with the OS bridge first in <head>, and without what the review refuses anyway (a <base>, a
// <meta http-equiv>): /local-app.html writes it into a frame without an origin whose CSP admits inline code and
// the bridge, nothing from the network.
export function compose(html, { bridge = BRIDGE } = {}) {
  let h = String(html == null ? '' : html)
    .replace(/<\s*script\b[^>]*\bsrc\s*=\s*["']?\/nucleo-app\.js["']?[^>]*>\s*<\/script\s*>/gi, '')
    .replace(/<\s*base\b[^>]*>/gi, '').replace(/<\s*meta\b[^>]*http-equiv[^>]*>/gi, '');
  const tag = '<script src="' + bridge + '"></script>';
  if (!/^\s*<!doctype/i.test(h)) h = '<!doctype html>\n' + h;
  if (/<head\b[^>]*>/i.test(h)) return h.replace(/<head\b[^>]*>/i, (m) => m + '\n<meta charset="utf-8">' + tag);
  if (/<html\b[^>]*>/i.test(h)) return h.replace(/<html\b[^>]*>/i, (m) => m + '<head><meta charset="utf-8">' + tag + '</head>');
  return h.replace(/^(\s*<!doctype[^>]*>)/i, (m) => m + '<html><head><meta charset="utf-8">' + tag + '</head>');
}

// The record Start, the taskbar and the window manager use (the shell's state.apps). Never an origin.
export function record(entry, { route } = {}) {
  if (!entry || !isMyApp(entry.id)) return null;
  const names = {};
  for (const l of LANGS) if (entry.names && typeof entry.names[l] === 'string') names[l] = cleanName(entry.names[l]);
  return { id: entry.id, name: cleanName(entry.name) || entry.id, names, category: CATEGORIES.includes(entry.category) ? entry.category : 'tools',
    route: route || '', icon: icon(entry.name || entry.id, entry.id), version: String(entry.version || ''),
    enabled: true, local: true, created_by: 'agent', trust: 'sandbox', permissions: [] };
}

// A tile with the app's initial, as an inline image (no file, no request). Its colour follows the id.
function icon(name, id) {
  const ch = (String(name || 'A').trim().match(/[\p{L}\p{N}]/u) || ['A'])[0].toUpperCase();
  let h = 0;
  for (const c of String(id || name || '')) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  const hue = h % 360;
  const svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect x="4" y="4" width="56" height="56" rx="16" fill="hsl(' + hue +
    ',62%,46%)"/><text x="32" y="43" font-family="system-ui,sans-serif" font-size="30" font-weight="700" fill="#fff" text-anchor="middle">' +
    ch.replace(/[<&]/g, 'A') + '</text></svg>';
  const b64 = typeof btoa === 'function' ? btoa(unescape(encodeURIComponent(svg))) : Buffer.from(svg, 'utf8').toString('base64');
  return 'data:image/svg+xml;base64,' + b64;
}

// ── asking a model (small ones too: one page, a tight prompt) ──────────────────────────────────────────
// About 350 tokens of instructions and an answer within 2048 tokens: what a server model with an 8k window gives
// one question (the hosted Gemma clamps max_tokens there), and room enough for a 100-line app.
const MAX_TOKENS = 2048;
export function prompt(request, { lang = 'en' } = {}) {
  const system = [
    'You write ONE small, self-contained web app for NucleoOS. Reply with exactly these four parts and nothing else:',
    'NAME: <short app name, in the user\'s language>',
    'CATEGORY: <one of: ' + CATEGORIES.join(', ') + '>',
    'STRINGS: <one line of JSON: {"it":{...},"en":{...},"es":{...},"fr":{...},"de":{...}}, every text the app shows, same keys in all five, "title" included>',
    '```html',
    '<!doctype html> ... the whole page ... </html>',
    '```',
    'Do exactly what the user asks and nothing more: the simplest page that does it.',
    'Rules for the page:',
    '- One file: CSS in <style>, code in one <script> without src. Under 100 lines, short CSS.',
    '- Each visible text is an element with data-i18n="key" (a key of STRINGS). In code: NucleoApp.t("key").',
    '- NucleoApp is already loaded: `await NucleoApp.ready` first. Saving data: await NucleoApp.kv.get(key, fallback), await NucleoApp.kv.set(key, value).',
    '- Never: external URLs or files, fetch, XMLHttpRequest, WebSocket, localStorage, sessionStorage, indexedDB, cookies, import, eval, window.open, iframes, location changes, parent.',
    '- Colours: background transparent or dark, accent var(--os-accent); light theme under html[data-theme="light"]. Works with mouse, touch and keyboard.',
  ].join('\n');
  const user = 'Language of the user: ' + (LANGS.includes(lang) ? lang : 'en') + '\nThe app: ' + String(request || '').slice(0, 1200);
  return { system, user, maxTokens: MAX_TOKENS };
}

// A JSON object starting at text[i] ('{'), with strings and escapes respected; null if it never closes.
function jsonAt(text, i) {
  let depth = 0, inStr = false, esc = false;
  for (let k = i; k < text.length; k++) {
    const c = text[k];
    if (inStr) { if (esc) esc = false; else if (c === '\\') esc = true; else if (c === '"') inStr = false; continue; }
    if (c === '"') inStr = true;
    else if (c === '{') depth++;
    else if (c === '}' && --depth === 0) { try { return JSON.parse(text.slice(i, k + 1)); } catch { return null; } }
  }
  return null;
}
// A model's answer → a package, or null when it holds no page. Tolerant: missing parts get defaults, the review
// decides whether the page may be installed.
export function parse(text) {
  const t = String(text == null ? '' : text);
  let html = '';
  const fence = /```(?:html|htm)?\s*\n([\s\S]*?)```/i.exec(t);
  if (fence && /<\s*(html|body|div|!doctype)/i.test(fence[1])) html = fence[1];
  if (!html) { const a = t.search(/<!doctype html|<html\b/i), b = t.lastIndexOf('</html>'); if (a >= 0 && b > a) html = t.slice(a, b + 7); }
  if (!html.trim()) return null;
  const head = fence ? t.slice(0, fence.index) : t;
  const name = (/\bNAME\s*\**\s*:\s*\**\s*(.+)$/m.exec(head) || [])[1] || '';
  const category = (/\bCATEGORY\s*\**\s*:\s*\**\s*([a-z]+)/im.exec(head) || [])[1] || '';
  let strings = null;
  const si = head.search(/STRINGS\s*\**\s*:/i);
  if (si >= 0) { const b = head.indexOf('{', si); if (b >= 0) strings = jsonAt(head, b); }
  if (!strings) { const m = /<script\b[^>]*type\s*=\s*["']application\/json["'][^>]*>([\s\S]*?)<\/script>/i.exec(html); if (m) try { strings = JSON.parse(m[1]); } catch {} }
  return normalize({ name: name.replace(/[*`]/g, ''), category, strings: strings || {}, html: html.trim() + '\n' });
}

// ── the built-in app, for a device with no model ─────────────────────────────────────────────────────
// A hello world that shows what the person asked to print (their quoted text), else "Hello, World!".
const HELLO = { it: 'Ciao mondo', en: 'Hello World', es: 'Hola mundo', fr: 'Bonjour le monde', de: 'Hallo Welt' };
const HINT = { it: 'Un\'app scritta per te da ANIMA.', en: 'An app ANIMA wrote for you.', es: 'Una app que ANIMA escribió para ti.',
  fr: 'Une app écrite pour toi par ANIMA.', de: 'Eine App, die ANIMA für dich geschrieben hat.' };
export function template(request) {
  const text = cleanName(quoted(request)) || 'Hello, World!';
  const strings = {};
  for (const l of LANGS) strings[l] = { title: HELLO[l], text, hint: HINT[l] };
  const html = `<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title data-i18n="title">Hello World</title>
<style>
  html, body { height: 100%; margin: 0; }
  body { display: grid; place-items: center; background: #0e1424; color: #eef2ff; font: var(--os-font-size, 15px)/1.5 system-ui, sans-serif; }
  html[data-theme="light"] body { background: #f4f6fb; color: #111827; }
  main { text-align: center; padding: 24px; }
  h1 { margin: 0 0 6px; font-size: 1.1em; font-weight: 600; opacity: .7; }
  p.big { margin: 0; font-size: 2.4em; font-weight: 700; color: var(--os-accent, #4ea1ff); word-break: break-word; }
  p.hint { margin: 14px 0 0; font-size: .85em; opacity: .55; }
</style>
</head>
<body>
<main>
  <h1 data-i18n="title">Hello World</h1>
  <p class="big" data-i18n="text">${text.replace(/[<&]/g, '')}</p>
  <p class="hint" data-i18n="hint"></p>
</main>
<script>
  NucleoApp.ready.then(function () { document.title = NucleoApp.t('title'); });
</script>
</body>
</html>
`;
  return normalize({ name: HELLO.en, names: HELLO, category: 'tools', description: 'Hello World', html, strings });
}

// ── on the device ────────────────────────────────────────────────────────────────────────────────────
export function createMyApps({ fs: dfs = fs, now = () => Date.now() } = {}) {
  // The index is the list Start shows: read once, written last. An index that exists but cannot be read is
  // never written over (it would drop every other app of the person): the install is refused instead.
  async function readIndex() {
    const text = await dfs.readText(INDEX);
    if (text == null || text === '') return { schema: 1, apps: [] };
    let j;
    try { j = JSON.parse(text); } catch { j = null; }
    if (!j || typeof j !== 'object' || !Array.isArray(j.apps)) throw Object.assign(new Error('index unreadable'), { code: 'index' });
    return { schema: 1, apps: j.apps.filter((a) => a && isMyApp(a.id)) };
  }
  async function list() {
    try { return (await readIndex()).apps; } catch { return []; }
  }
  const dirOf = (id) => DIR + '/' + id;

  // install(pkg, { id, update, checkSyntax }) → { id, version_code, updated }. Throws { code } (review | exists |
  // index | full | bad-id, or a file layer code).
  async function install(pkg, { id: wantId, update = false, checkSyntax } = {}) {
    const p = normalize(pkg);
    const rv = review(p, { checkSyntax });
    if (!rv.ok) throw Object.assign(new Error('review'), { code: 'review', errors: rv.errors });
    const id = wantId || idFor(p.names.en || p.name);
    if (!isMyApp(id)) throw Object.assign(new Error('bad id'), { code: 'bad-id' });
    const idx = await readIndex();
    const prev = idx.apps.find((a) => a.id === id);
    if (prev && !update) throw Object.assign(new Error('exists'), { code: 'exists', app: prev });
    if (!prev && idx.apps.length >= LIMITS.apps) throw Object.assign(new Error('full'), { code: 'full' });
    const code = prev ? (Number(prev.version_code) || 0) + 1 : 1;          // only up, whatever the package says
    const version = '1.0.' + (code - 1);
    const at = new Date(now()).toISOString();
    const manifest = { id, name: p.name, names: p.names, version, version_code: code, description: p.description, category: p.category,
      runtime: 'web', permissions: [], created_by: 'agent', made_with: 'myapps' };
    const d = dirOf(id);
    await dfs.mkdirp(d + '/www');
    for (const l of LANGS) await dfs.writeJSON(d + '/www/i18n.' + l + '.json', Object.assign({ _lang: l }, p.strings[l]));
    await dfs.write(d + '/www/index.html', p.html);
    await dfs.writeJSON(d + '/manifest.json', manifest, { pretty: true });
    const entry = { id, name: p.name, names: p.names, category: p.category, description: p.description, version, version_code: code,
      created: prev ? prev.created || at : at, updated: at };
    // Read again just before writing: another window may have installed or removed an app meanwhile.
    const fresh = await readIndex();
    fresh.apps = fresh.apps.filter((a) => a.id !== id).concat([entry]);
    await dfs.writeJSON(INDEX, fresh);
    return { id, version_code: code, updated: !!prev, entry };
  }

  // remove(id) → the bin entry. Only an app of the person (in the index, with the prefix): the index first, so a
  // folder that fails to move is an unlisted leftover, never an app in Start without its files.
  async function remove(id) {
    if (!isMyApp(id)) throw Object.assign(new Error('bad id'), { code: 'bad-id' });
    const idx = await readIndex();
    if (!idx.apps.some((a) => a.id === id)) throw Object.assign(new Error('not found'), { code: 'not-found' });
    idx.apps = idx.apps.filter((a) => a.id !== id);
    await dfs.writeJSON(INDEX, idx);
    let bin = null;
    try { bin = await dfs.trash(dirOf(id)); } catch (e) { if (!e || e.code !== 'not-found') throw e; }
    // What the app kept (NucleoApp.kv and its own files, /data/apps/<id>) goes too, to the bin as well: a person who
    // removes an app does not expect its data to stay behind, and can still take both back.
    try { await dfs.trash('/data/apps/' + id); } catch {}
    return bin;
  }

  // The document a window of this app runs (/local-app.html asks for it through the broker).
  async function doc(id, { bridge = BRIDGE } = {}) {
    if (!isMyApp(id)) return null;
    const html = await dfs.readText(dirOf(id) + '/www/index.html');
    if (html == null || html.length > LIMITS.html * 2) return null;
    return compose(html, { bridge });
  }
  // The app's strings in one language (the broker's app.i18n for an app of the person).
  async function strings(id, lang) {
    if (!isMyApp(id) || !LANGS.includes(lang)) return {};
    const j = await dfs.readJSON(dirOf(id) + '/www/i18n.' + lang + '.json', {});
    return j && typeof j === 'object' && !Array.isArray(j) ? j : {};
  }
  // Find an app of the person by what they call it ("rimuovi l'app ciao mondo").
  async function find(name) {
    const want = slug(name);
    if (!want) return null;
    const apps = await list();
    return apps.find((a) => a.id === PREFIX + want || slug(a.name) === want || Object.values(a.names || {}).some((n) => slug(n) === want))
      || apps.find((a) => [a.name, ...Object.values(a.names || {})].some((n) => { const s = slug(n); return s && (s.includes(want) || want.includes(s)); }))
      || null;
  }
  // make(request, { llm, lang, checkSyntax, onStep }) → what to show the person before anything is installed:
  //   { ok: true, pkg, review, source: 'model'|'template', model, error, id, existing } — ready, after their OK
  //   { ok: false, review, source: 'model' } — the model's page broke the rules twice: nothing to install, the reasons
  // llm(system, user, { maxTokens }) → text (or { text, model }); absent or failing, the built-in hello world. The
  // model is asked once, and once more only to fix what the review refused (each ask counts on a daily cap).
  async function make(request, { llm, lang = 'en', checkSyntax, onStep = () => {} } = {}) {
    let pkg = null, rv = null, source = 'template', model = '', error = '';
    if (typeof llm === 'function') {
      const p = prompt(request, { lang });
      for (let i = 0; i < 2 && !pkg; i++) {
        onStep(i ? 'fix' : 'write');
        let text = '';
        try {
          const out = await llm(p.system, i ? p.user + '\n\n' + fixNote(rv) : p.user, { maxTokens: p.maxTokens });
          text = typeof out === 'string' ? out : String((out && out.text) || '');
          if (out && out.model) model = String(out.model);
        } catch (e) { error = String((e && (e.code || e.message)) || e || 'failed').slice(0, 120); break; }
        const got = parse(text);
        if (!got) { error = 'unparsed'; break; }
        onStep('review');
        rv = review(got, { checkSyntax });
        if (rv.ok) { pkg = got; source = 'model'; }
      }
      if (!pkg && rv && !rv.ok) return { ok: false, review: rv, source: 'model', model, error };
    }
    if (!pkg) { pkg = template(request); rv = review(pkg, { checkSyntax }); source = 'template'; }
    const id = idFor(pkg.names.en || pkg.name);
    const existing = (await list()).find((a) => a.id === id) || null;
    return { ok: rv.ok, pkg, review: rv, source, model, error, id, existing };
  }
  return { list, install, remove, doc, strings, find, make, dir: DIR };
}

// What the second ask tells the model: the rules its page broke, in its own terms.
const FIX = { network: 'no fetch, XMLHttpRequest or WebSocket', worker: 'no workers', storage: 'no localStorage, sessionStorage, indexedDB or cookies: use NucleoApp.kv',
  api: 'no /api/ paths', module: 'no import/export', eval: 'no eval, new Function or string timers', navigation: 'no window.open, location or history changes',
  parent: 'no parent, top or opener', remote: 'no external URLs', element: 'no iframe, object, embed, base or meta http-equiv', script_src: 'no <script src>',
  no_html: 'a whole page in one ```html block', too_large: 'shorter: under 100 lines', strings: 'every data-i18n key in STRINGS', syntax: 'valid JavaScript' };
function fixNote(rv) {
  const codes = (rv && rv.errors ? rv.errors : []).map((e) => FIX[e.code]).filter(Boolean);
  return 'Your previous answer broke these rules: ' + codes.join('; ') + '. Write it again, same four parts.';
}

// describe(made, { t, lang }) → { reply, card } for a chat: what was written, by whom, what installing it means,
// and the page itself (the person sees what will run before saying yes). t(key, vars) is the core catalog
// (nucleo-i18n.js, keys myapps_*); card is null when there is nothing to install.
export function describe(made, { t = (k) => k, lang = 'en' } = {}) {
  if (!made) return { reply: '', card: null };
  if (!made.ok) {
    const reasons = (made.review && made.review.errors || []).map((e) => t('myapps_r_' + e.code)).join(', ');
    return { reply: t('myapps_refused', { reasons }), card: null };
  }
  const p = made.pkg;
  const name = p.names[lang] || p.name;
  const cat = t('myapps_cat_' + p.category);
  const head = made.source === 'model'
    ? t('myapps_written', { name, model: made.model || t('myapps_the_model') })
    : t(made.error ? 'myapps_template_failed' : 'myapps_template', { name });
  const ver = made.existing ? t('myapps_update', { name, from: made.existing.version || '1.0.0', to: '1.0.' + (Number(made.existing.version_code) || 1) })
    : t('myapps_new', { category: cat });
  const reply = head + '\n\n' + ver + ' ' + t('myapps_safety') + '\n\n```html\n' + p.html.replace(/\n$/, '').replace(/```/g, '``​`') + '\n```';
  const kb = Math.max(1, Math.round((p.html.length + JSON.stringify(p.strings).length) / 1024));
  return { reply, card: { kind: 'myapp', op: 'install', id: made.id, title: name, sub: t('myapps_card_sub', { category: cat, kb }) } };
}

const myapps = Object.assign(createMyApps(), { plan, parse, prompt, template, review, record, idFor, isMyApp, compose, describe });
export default myapps;

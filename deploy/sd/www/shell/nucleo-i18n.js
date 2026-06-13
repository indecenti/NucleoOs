// nucleo-i18n.js — NucleoOS centralized internationalization runtime.
//
// One engine, served once from the shell root (`/nucleo-i18n.js`), imported by the shell AND every
// app iframe (same origin → absolute import works from `/apps/<id>/`). It is the single source of
// truth for the active OS language and the only thing that turns catalog keys into on-screen text.
//
// Design goals (why this shape):
//  • CENTRALIZED  — one runtime, one language signal, one persistence path. No per-app i18n forks.
//  • SCALABLE     — adding a language = drop `<ns>.<lang>.json` files + a row in LANGS. Adding a
//                   string = one key in a catalog. No code changes, no rebuild of the engine.
//  • NAMESPACED   — shared OS vocabulary lives in `core`; the shell owns `shell`; each app owns a
//                   namespace = its app id. An app always gets `core` merged underneath for free.
//  • LIVE         — changing the language in Settings repaints every open window with no reload,
//                   using the browser's same-origin `storage` event (fires in all iframes but the
//                   setter) plus an in-document event for same-frame subscribers.
//  • HONEST       — a missing key falls back lang→base(it)→key and is never a blank; the i18n gate
//                   (tools/i18n-check.mjs) keeps catalogs in parity so that fallback rarely fires.
//  • DEVICE-CHEAP — all of this runs in the browser; the Cardputer only serves small static JSON.
//
// Quick start (in an app):
//   import I18N from '/nucleo-i18n.js';
//   const t = await I18N.init('myapp');         // loads core + myapp for the active language
//   el.textContent = t('save');                 // 'Salva' / 'Save'
//   I18N.apply();                               // fills [data-i18n] in the document
//   I18N.onChange(() => repaintDynamicBits());  // re-render anything t() built imperatively

const BASE = 'it';                               // primary language; the fallback floor for every key
const RUNTIME_KEY = 'anima.lang';                // the localStorage key the whole OS already reads
const LOCALE_KEY = 'nucleo.locale';              // optional regional-format override (display lang ≠ format)

// Supported languages. Add a row here + the matching catalog files to ship a new language OS-wide;
// the Settings/onboarding pickers render straight from this list, so nothing else needs to change.
export const LANGS = [
  { code: 'it', label: 'Italiano', flag: '🇮🇹', dir: 'ltr', locale: 'it-IT' },
  { code: 'en', label: 'English',  flag: '🇬🇧', dir: 'ltr', locale: 'en-US' },
];
const LANG_CODES = LANGS.map((l) => l.code);
const META = Object.fromEntries(LANGS.map((l) => [l.code, l]));

// --- internal state ---------------------------------------------------------
const rawCatalogs = new Map();     // url -> flat {key: value|pluralObj} | null (fetched, may be empty)
const merged = new Map();          // `${ns}|${lang}` -> resolved flat map (core+ns, base+active)
const loaded = new Set();          // namespaces whose files we've already fetched for the active lang
const subscribers = new Set();     // onChange callbacks (this document only)
let activeLang = null;             // resolved once on first access
let installed = false;             // global listeners installed?

// --- language resolution & persistence --------------------------------------

function normalize(code) {
  if (!code) return null;
  const c = String(code).toLowerCase().slice(0, 2);
  return LANG_CODES.includes(c) ? c : null;
}

// Optional regional-format locale (BCP-47, e.g. "en-GB"), independent of the display language —
// the modern-OS "display language vs regional format" split. Empty/invalid → use the language's locale.
function readLocaleOverride() {
  try { const v = localStorage.getItem(LOCALE_KEY); return v && /^[a-z]{2}(-[A-Za-z0-9]{2,8})*$/.test(v) ? v : null; }
  catch { return null; }
}

// Resolve the active language from the canonical runtime key, then the document, then the browser,
// then the base. Cached after first read; `setLang` and the storage listener update it.
function resolveLang() {
  if (activeLang) return activeLang;
  let l = null;
  try { l = normalize(localStorage.getItem(RUNTIME_KEY)); } catch {}
  if (!l) l = normalize(document.documentElement.getAttribute('lang'));
  if (!l) l = normalize(navigator.language);
  activeLang = l || BASE;
  return activeLang;
}

// --- catalog loading --------------------------------------------------------

// Where a namespace's catalog lives. `core` and `shell` are centralized in the shell; any other
// namespace is treated as an app id and resolves to its co-located file. Absolute paths only, and
// never with `/www/` — the webfs maps `/apps/<id>/x` → `apps/<id>/www/x` already.
function urlFor(ns, lang) {
  if (ns === 'core' || ns === 'shell') return `/i18n/${ns}.${lang}.json`;
  return `/apps/${ns}/i18n.${lang}.json`;
}

async function fetchCatalog(url) {
  if (rawCatalogs.has(url)) return rawCatalogs.get(url);   // in-memory dedup within this document
  let obj = null;
  try {
    // No app-level persistent cache on purpose: the service worker already precaches core/shell and
    // the HTTP cache covers the rest, so fetches are cheap — and a persistent cache would serve a
    // STALE catalog for the whole session after a deploy/edit. The in-memory Map is enough.
    const r = await fetch(url, { cache: 'no-cache' });
    if (r.ok) obj = await r.json();
  } catch {}
  // A missing/empty catalog is fine (the namespace just inherits core + falls back to keys).
  if (obj == null) obj = {};
  rawCatalogs.set(url, obj);
  return obj;
}

// Build the resolved lookup table for (ns, lang): core(base) ⊂ core(active) ⊂ ns(base) ⊂ ns(active).
// Later layers win, so the active language overrides base, and the namespace overrides core.
async function buildMerged(ns, lang) {
  const key = `${ns}|${lang}`;
  if (merged.has(key)) return merged.get(key);
  // core: base + active. Both are precached by the service worker → zero device read, and the base
  // layer is a free fallback floor. App namespace: ACTIVE language ONLY — the i18n gate guarantees
  // IT/EN key parity, so fetching the base catalog too would be a redundant SD read on the Cardputer.
  const coreLangs = lang === BASE ? [BASE] : [BASE, lang];
  const jobs = coreLangs.map((L) => fetchCatalog(urlFor('core', L)));
  if (ns !== 'core') jobs.push(fetchCatalog(urlFor(ns, lang)));
  const layers = await Promise.all(jobs);               // [core(base), core(active), ns(active)] — later wins
  const out = Object.assign({}, ...layers);
  merged.set(key, out);
  return out;
}

// --- formatting (the "professional OS" touch: locale-correct dates/numbers) --

function dictFor(ns) {
  const k = `${ns || 'core'}|${resolveLang()}`;
  return merged.get(k) || merged.get(`core|${resolveLang()}`) || {};
}

// Substitute {name} placeholders and resolve plural objects ({one,other,...}) via Intl.PluralRules.
function format(value, vars, lang) {
  if (value == null) return value;
  if (typeof value === 'object') {                       // plural form: pick by count
    const n = vars && (vars.count != null ? vars.count : vars.n);
    let cat = 'other';
    try { cat = new Intl.PluralRules(META[lang].locale).select(Number(n)); } catch {}
    value = value[cat] != null ? value[cat] : (value.other != null ? value.other : value.one);
  }
  if (typeof value !== 'string') return String(value);
  if (!vars) return value;
  return value.replace(/\{(\w+)\}/g, (m, k) => (k in vars ? String(vars[k]) : m));
}

// Translate `key` in namespace `ns` with optional interpolation/plural `vars`.
// Fallback chain is already baked into the merged dict (active→base, ns→core); the last resort is
// the key itself so the UI is never blank — and a visible raw key flags a gap during testing.
function translate(ns, key, vars) {
  const lang = resolveLang();
  const d = dictFor(ns);
  const v = (key in d) ? d[key] : key;
  return format(v, vars, lang);
}

// --- DOM binding ------------------------------------------------------------
//
// Declarative annotations on elements:
//   data-i18n="key"                 → textContent = t(key)
//   data-i18n-html="key"            → innerHTML   = t(key)   (catalog strings are trusted/static)
//   data-i18n-attr="placeholder:k1;title:k2;aria-label:k3"  → set each attribute to t(kN)
//   data-i18n-args='{"name":"Ada"}' → interpolation vars for this element's keys
//   data-i18n-ns="appid"            → look this element up in a non-default namespace (rare)
function applyTo(el, defaultNs) {
  let vars;
  const argStr = el.getAttribute('data-i18n-args');
  if (argStr) { try { vars = JSON.parse(argStr); } catch {} }
  const ns = el.getAttribute('data-i18n-ns') || defaultNs;
  const txt = el.getAttribute('data-i18n');
  if (txt) el.textContent = translate(ns, txt, vars);
  const html = el.getAttribute('data-i18n-html');
  if (html) el.innerHTML = translate(ns, html, vars);
  const attr = el.getAttribute('data-i18n-attr');
  if (attr) for (const pair of attr.split(';')) {
    const i = pair.indexOf(':'); if (i < 0) continue;
    const a = pair.slice(0, i).trim(), k = pair.slice(i + 1).trim();
    if (a && k) el.setAttribute(a, translate(ns, k, vars));
  }
}

// --- the public engine ------------------------------------------------------

const I18N = {
  BASE,
  LANGS,
  get lang() { return resolveLang(); },
  meta(code) { return META[code || resolveLang()]; },

  // Regional-format locale used by the Intl helpers. Honors a 'nucleo.locale' override so the user can
  // keep, say, an English UI with Italian date/number formats; otherwise the active language's locale.
  locale(code) {
    if (!code) { const o = readLocaleOverride(); if (o) return o; }
    return (META[code || resolveLang()] || META[BASE]).locale;
  },
  // The override as stored ('' = "follow the display language"). For the Settings region picker.
  formatLocale() { return readLocaleOverride() || ''; },
  // Set/clear the regional-format override. Notifies onChange subscribers so live previews refresh,
  // and (via localStorage) every other open window updates too.
  setLocale(loc) {
    try { localStorage.setItem(LOCALE_KEY, loc || ''); } catch {}
    subscribers.forEach((cb) => { try { cb(activeLang); } catch {} });
  },

  // Load core + the given namespace for the active language, install listeners, paint the document,
  // and return a `t` bound to that namespace. Idempotent and safe to await from many call sites.
  //
  // `observe` (default true): watch the DOM and auto-translate any [data-i18n*] element added later
  // — so an app only has to annotate its HTML/templates; it never has to call apply() after a render.
  // Heavy apps that add thousands of nodes can pass {observe:false} and call I18N.apply() themselves.
  async init(ns = 'core', { apply = true, observe = true } = {}) {
    resolveLang();
    install();
    await buildMerged(ns, activeLang);
    if (ns !== 'core') loaded.add(ns);
    loaded.add('core');
    syncDocAttrs();
    if (apply) this.apply(document, ns);
    if (observe) installObserver(ns);
    return (key, vars) => translate(ns, key, vars);
  },

  // Translate a key. Without a namespace it uses core; pass ns for app-specific keys.
  t(key, vars, ns = 'core') { return translate(ns, key, vars); },

  // Return a namespace-bound t() without re-loading (use after init for convenience).
  scope(ns) { return (key, vars) => translate(ns, key, vars); },

  // Fill every [data-i18n*] element under `root`. Call after building DOM dynamically.
  apply(root = document, defaultNs = 'core') {
    const r = root.nodeType === 1 || root.nodeType === 9 ? root : document;
    const sel = '[data-i18n],[data-i18n-html],[data-i18n-attr]';
    if (r.matches && r.matches(sel)) applyTo(r, defaultNs);
    r.querySelectorAll && r.querySelectorAll(sel).forEach((el) => applyTo(el, defaultNs));
  },

  // Switch the OS language. Persists to the runtime key (which fires `storage` in every other
  // same-origin iframe → they repaint), updates this document, asks the shell to persist it to
  // settings.json, and repaints here. Pass the same value twice = no-op.
  async setLang(code) {
    const l = normalize(code); if (!l || l === activeLang) return;
    try { localStorage.setItem(RUNTIME_KEY, l); } catch {}
    await applyNewLang(l);
    // Persist to the canonical store. The shell listens for this and writes settings.json once.
    try {
      const msg = { type: 'set-language', lang: l };
      if (window.parent && window.parent !== window) window.parent.postMessage(msg, '*');
      else window.postMessage(msg, '*');
    } catch {}
  },

  // Subscribe to language changes in THIS document (for imperatively-built text). Returns unsubscribe.
  // Registering interest implies wanting cross-window notifications, so ensure the storage/message
  // listeners are installed even if the app never called init() (e.g. already-bilingual apps that only
  // want the live-sync hook). install() is idempotent and needs no catalogs.
  onChange(cb) { install(); subscribers.add(cb); return () => subscribers.delete(cb); },

  // Locale-correct formatters (Intl, active language). Cheap, professional, and consistent OS-wide.
  fmtDate(d, opt) { return fmt(d, opt || { dateStyle: 'medium' }, 'date'); },
  fmtTime(d, opt) { return fmt(d, opt || { timeStyle: 'short' }, 'date'); },
  fmtDateTime(d, opt) { return fmt(d, opt || { dateStyle: 'medium', timeStyle: 'short' }, 'date'); },
  fmtNumber(n, opt) { return fmt(n, opt, 'num'); },
  fmtBytes(bytes) {                                   // human storage size in the active locale
    const u = ['B', 'KB', 'MB', 'GB', 'TB']; let i = 0, n = Number(bytes) || 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return this.fmtNumber(n, { maximumFractionDigits: n < 10 && i > 0 ? 1 : 0 }) + ' ' + u[i];
  },
  fmtRelative(value, unit) {                          // e.g. (-3,'minute') → "3 minuti fa"
    try { return new Intl.RelativeTimeFormat(this.locale(), { numeric: 'auto' }).format(value, unit); }
    catch { return String(value); }
  },
  fmtList(arr, opt) {
    try { return new Intl.ListFormat(this.locale(), opt || { type: 'conjunction' }).format(arr); }
    catch { return (arr || []).join(', '); }
  },
};

function fmt(v, opt, kind) {
  try {
    const loc = I18N.locale();
    if (kind === 'date') return new Intl.DateTimeFormat(loc, opt).format(v instanceof Date ? v : new Date(v));
    return new Intl.NumberFormat(loc, opt).format(Number(v));
  } catch { return String(v); }
}

// Repaint flow shared by setLang and the cross-iframe storage listener.
async function applyNewLang(l) {
  activeLang = l;
  // Reload catalogs for every namespace this document had loaded, in the new language.
  for (const ns of loaded) await buildMerged(ns, l);
  syncDocAttrs();
  I18N.apply(document, defaultNamespace());
  subscribers.forEach((cb) => { try { cb(l); } catch {} });
}

// The default namespace of THIS document = the app's own id (from the iframe path) if present,
// else 'shell' for the root document. Used so apply() after a language change uses the right ns.
let _defaultNs;
function defaultNamespace() {
  if (_defaultNs) return _defaultNs;
  const m = location.pathname.match(/^\/apps\/([^/]+)/);
  _defaultNs = m ? m[1] : 'shell';
  return _defaultNs;
}

function syncDocAttrs() {
  const m = META[resolveLang()] || META[BASE];
  document.documentElement.setAttribute('lang', m.code);
  document.documentElement.setAttribute('dir', m.dir);
}

// Auto-translate [data-i18n*] elements added after init (lists, dialogs, rows built by app JS), so
// integrating an app is "annotate the HTML/templates" with no apply() plumbing. Batched per frame;
// only subtrees that actually carry an annotation cost anything.
let observer = null;
const SEL = '[data-i18n],[data-i18n-html],[data-i18n-attr]';
function installObserver(ns) {
  if (observer || typeof MutationObserver === 'undefined') return;
  // Microtask (not rAF): batches the whole synchronous mutation burst and still fires when the tab
  // is hidden — rAF is paused in background tabs, which would leave late DOM untranslated.
  const schedule = (typeof queueMicrotask === 'function') ? queueMicrotask : (cb) => Promise.resolve().then(cb);
  let pending = new Set();
  let scheduled = false;
  const flush = () => { scheduled = false; const nodes = [...pending]; pending = new Set(); for (const n of nodes) if (n.isConnected) I18N.apply(n, ns); };
  observer = new MutationObserver((muts) => {
    for (const mu of muts) for (const n of mu.addedNodes) {
      if (n.nodeType !== 1) continue;
      if (n.matches(SEL) || n.querySelector(SEL)) pending.add(n);   // cheap gate: skip non-i18n subtrees
    }
    if (pending.size && !scheduled) { scheduled = true; schedule(flush); }
  });
  const root = document.body || document.documentElement;
  if (root) observer.observe(root, { childList: true, subtree: true });
}

function install() {
  if (installed) return; installed = true;
  // The magic that makes switching OS-wide and reload-free: localStorage `storage` events fire in
  // every same-origin document EXCEPT the one that wrote — i.e. all the OTHER open app iframes.
  window.addEventListener('storage', (e) => {
    if (e.key === RUNTIME_KEY) {
      const l = normalize(e.newValue);
      if (l && l !== activeLang) applyNewLang(l);
    } else if (e.key === LOCALE_KEY) {
      // Regional format changed in another window → refresh anything formatted via Intl here.
      subscribers.forEach((cb) => { try { cb(activeLang); } catch {} });
    }
  });
  // Same-frame relay (e.g. the shell tells its children, or a test dispatches it directly).
  window.addEventListener('message', (e) => {
    const d = e && e.data;
    if (d && d.type === 'set-language') { const l = normalize(d.lang); if (l && l !== activeLang) applyNewLang(l); }
  });
}

// Expose for non-module scripts and for cross-frame debugging.
try { window.NucleoI18N = I18N; } catch {}

export default I18N;
export const t = (key, vars, ns) => translate(ns || 'core', key, vars);
export const setLang = (c) => I18N.setLang(c);

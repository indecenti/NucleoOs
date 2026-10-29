/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-app.js — the runtime of an app that may run WITHOUT an origin.
//
// Apps from the web apps channel run sandboxed (wm.js isSandboxed): no cookie, no localStorage, and no
// ES module imports from the device, because a frame whose origin is "null" fails the CORS check that
// module scripts need. This file is the way back in. It is a CLASSIC script on purpose — classic scripts
// load cross-origin without CORS — and it talks to the shell only through the broker (appbroker.js).
//
// The same app code runs in three places, and this runtime picks the path:
//   • sandboxed in the shell   → everything through the broker (the only way)
//   • curated in the shell     → the broker too (it answers every frame the shell created)
//   • opened on its own        → localStorage + its own i18n files, so a developer can just open it
//
//   <script src="/nucleo-app.js"></script>
//   <script type="module">
//     const app = await NucleoApp.ready;          // { lang, theme } — strings loaded, [data-i18n] painted
//     const t = NucleoApp.t;                      // t('key', { name: 'Ada', count: 2 })
//     await NucleoApp.kv.set('state', { a: 1 });  // the app's own small state (64 KB), on the device
//     const s = await NucleoApp.kv.get('state', {});
//     NucleoApp.onLang(() => render());           // after the strings of the new language are painted
//     NucleoApp.onTheme((th) => …);               // { mode: 'dark'|'light', accent, fontSize }
//     if (await NucleoApp.caps.has('device.mic')) …  // device features: docs/capabilities.md
//     const [f] = await NucleoApp.files.open({ types: ['pdf'] }) || [];   // the OS Open sheet: { name, data, where }
//     const s = await NucleoApp.files.save('report.pdf', bytes);            // the OS Save sheet: device or download
//     await NucleoApp.files.save('report.pdf', bytes, { handle: s.handle }); // Save again: same file, no sheet
//     await NucleoApp.files.app.write('cover.png', bytes);                  // the app's own folder (docs/files.md)
//     const f0 = await NucleoApp.files.launched();                          // the file the app was opened with, or null
//   </script>
(function () {
  'use strict';
  if (window.NucleoApp) return;

  var LOCALES = { it: 'it-IT', en: 'en-US', es: 'es-ES', fr: 'fr-FR', de: 'de-DE' };
  var inShell = window.parent !== window;
  var seq = 0;
  var pending = {};
  var strings = {};
  var listeners = { lang: [], theme: [] };
  var api = { lang: 'en', theme: { mode: 'dark', accent: '#4ea1ff', fontSize: '14px' } };

  // ── the broker wire ──
  window.addEventListener('message', function (e) {
    if (e.source !== window.parent) return;                 // only the frame that hosts us
    var d = e.data;
    if (!d || typeof d !== 'object') return;
    if (d.type === 'nucleo.broker.reply' && pending[d.id]) {
      var p = pending[d.id];
      delete pending[d.id];
      clearTimeout(p.timer);
      p.resolve(d);
    } else if (d.type === 'nucleo.broker.event') {
      if (d.event === 'lang' && typeof d.lang === 'string' && d.lang !== api.lang) switchLang(d.lang);
      if (d.event === 'theme' && d.theme && typeof d.theme === 'object') setTheme(d.theme);
    }
  });

  function call(method, args, timeoutMs) {
    if (!inShell) return Promise.resolve({ ok: false, error: 'no shell' });
    return new Promise(function (resolve) {
      var id = 'na' + (++seq) + '.' + Date.now().toString(36);
      var timer = setTimeout(function () { delete pending[id]; resolve({ ok: false, error: 'timeout' }); }, timeoutMs || 10000);
      pending[id] = { resolve: resolve, timer: timer };
      window.parent.postMessage({ type: 'nucleo.broker', id: id, method: method, args: args || {} }, '*');
    });
  }

  // ── standalone fallbacks (no shell): this browser's storage, the app's own files ──
  var appId = (location.pathname.match(/\/apps\/([a-z0-9][a-z0-9._-]{0,63})\//i) || [])[1] || 'app';
  var localKey = function (k) { return 'nucleo.app.' + appId + '.' + k; };
  function standaloneLang() {
    var l = '';
    try { l = localStorage.getItem('anima.lang') || ''; } catch (e) {}
    l = (l || navigator.language || 'en').slice(0, 2).toLowerCase();
    return LOCALES[l] ? l : 'en';
  }
  function readJSON(url) {
    return fetch(url, { cache: 'no-store' }).then(function (r) { return r.ok ? r.json() : {}; }).catch(function () { return {}; });
  }
  function standaloneStrings(lang) {
    var urls = ['/i18n/core.en.json', 'i18n.en.json'];
    if (lang !== 'en') urls = ['/i18n/core.en.json', '/i18n/core.' + lang + '.json', 'i18n.en.json', 'i18n.' + lang + '.json'];
    return Promise.all(urls.map(readJSON)).then(function (parts) {
      var out = {};
      parts.forEach(function (p) { for (var k in p) if (k.charAt(0) !== '_') out[k] = p[k]; });
      return out;
    });
  }

  // ── strings ──
  function format(value, vars) {
    if (value == null) return value;
    if (typeof value === 'object') {
      var n = vars && (vars.count != null ? vars.count : vars.n);
      var cat = 'other';
      try { cat = new Intl.PluralRules(LOCALES[api.lang] || 'en-US').select(Number(n)); } catch (e) {}
      value = value[cat] != null ? value[cat] : (value.other != null ? value.other : value.one);
    }
    if (typeof value !== 'string') return String(value);
    if (!vars) return value;
    return value.replace(/\{(\w+)\}/g, function (m, k) { return k in vars ? String(vars[k]) : m; });
  }
  function t(key, vars) {
    return format(Object.prototype.hasOwnProperty.call(strings, key) ? strings[key] : key, vars);
  }
  // Same annotations as nucleo-i18n.js, except data-i18n-html: text from the network never becomes markup here.
  function apply(root) {
    var scope = root || document;
    var els = scope.querySelectorAll('[data-i18n],[data-i18n-attr]');
    for (var i = 0; i < els.length; i++) {
      var el = els[i], vars;
      var a = el.getAttribute('data-i18n-args');
      if (a) { try { vars = JSON.parse(a); } catch (e) {} }
      var key = el.getAttribute('data-i18n');
      if (key) el.textContent = t(key, vars);
      var attr = el.getAttribute('data-i18n-attr');
      if (attr) attr.split(';').forEach(function (pair) {
        var j = pair.indexOf(':');
        if (j < 0) return;
        var name = pair.slice(0, j).trim(), k = pair.slice(j + 1).trim();
        if (name && k && !/^on/i.test(name)) el.setAttribute(name, t(k, vars));
      });
    }
    document.documentElement.lang = api.lang;
  }
  function loadStrings(lang) {
    if (!inShell) return standaloneStrings(lang).then(function (s) { strings = s; api.lang = lang; });
    return call('app.i18n', { lang: lang }).then(function (r) {
      if (r && r.ok && r.strings && typeof r.strings === 'object') { strings = r.strings; api.lang = r.lang || lang; }
    });
  }
  function switchLang(lang) {
    return loadStrings(lang).then(function () {
      apply();
      listeners.lang.forEach(function (fn) { try { fn(api.lang); } catch (e) {} });
    });
  }

  // ── theme ──
  function setTheme(th) {
    api.theme = {
      mode: th.mode === 'light' ? 'light' : 'dark',
      accent: /^#[0-9a-f]{3,8}$/i.test(String(th.accent || '')) ? th.accent : api.theme.accent,
      fontSize: /^\d{1,2}px$/.test(String(th.fontSize || '')) ? th.fontSize : api.theme.fontSize,
    };
    var root = document.documentElement;
    root.dataset.theme = api.theme.mode;
    root.style.setProperty('--os-accent', api.theme.accent);
    root.style.setProperty('--os-font-size', api.theme.fontSize);
    listeners.theme.forEach(function (fn) { try { fn(api.theme); } catch (e) {} });
  }

  // ── kv ──
  function unwrap(r, field) {
    if (r && r.ok) return field ? r[field] : undefined;
    throw new Error((r && r.error) || 'broker error');
  }
  var kv = {
    get: function (key, fallback) {
      if (!inShell) {
        try { var v = localStorage.getItem(localKey(key)); return Promise.resolve(v == null ? (fallback === undefined ? null : fallback) : JSON.parse(v)); }
        catch (e) { return Promise.resolve(fallback === undefined ? null : fallback); }
      }
      return call('kv.get', { key: key }).then(function (r) { var v = unwrap(r, 'value'); return v == null && fallback !== undefined ? fallback : v; });
    },
    set: function (key, value) {
      if (!inShell) { try { localStorage.setItem(localKey(key), JSON.stringify(value)); return Promise.resolve(); } catch (e) { return Promise.reject(e); } }
      return call('kv.set', { key: key, value: value }).then(function (r) { unwrap(r); });
    },
    remove: function (key) {
      if (!inShell) { try { localStorage.removeItem(localKey(key)); } catch (e) {} return Promise.resolve(); }
      return call('kv.remove', { key: key }).then(function (r) { unwrap(r); });
    },
    all: function () {
      if (!inShell) {
        var out = {};
        try { for (var i = 0; i < localStorage.length; i++) { var k = localStorage.key(i); if (k.indexOf(localKey('')) === 0) out[k.slice(localKey('').length)] = JSON.parse(localStorage.getItem(k)); } } catch (e) {}
        return Promise.resolve(out);
      }
      return call('kv.all').then(function (r) { return unwrap(r, 'values'); });
    },
  };

  // ── capabilities (nucleo-caps.js through the broker; on its own, the device's list if it has one) ──
  var capsList = null;
  function loadCaps() {
    if (!capsList) {
      capsList = inShell
        ? call('sys.caps').then(function (r) { return r && r.ok && Array.isArray(r.caps) ? r.caps : []; })
        : fetch('/api/system/caps', { cache: 'no-store' }).then(function (r) { return r.ok ? r.json() : null; })
            .then(function (j) { return j && Array.isArray(j.caps) ? j.caps : []; }).catch(function () { return []; });
    }
    return capsList;
  }
  var caps = {
    has: function (name) { return loadCaps().then(function (l) { return l.indexOf(name) >= 0; }); },
    list: function () { return loadCaps().then(function (l) { return l.slice(); }); },
  };

  // ── files: the OS Open / Save sheets (appbroker.js file.*, shell/filedialog.js) ──
  // The user chooses the device's storage or the computer/phone in front of them; the app gets bytes, never a
  // path, and is the same on every edition. null = the user cancelled. On its own (no shell) a file input and a
  // download stand in, so an app needs no second code path.
  var SHEET_MS = 30 * 60 * 1000;          // a person is choosing: no short timeout
  function fileErr(r) {
    var e = new Error((r && r.error) || 'broker error');
    e.code = /^denied|unknown caller/.test(e.message) ? 'unsupported' : e.message === 'busy' ? 'busy' : 'failed';
    return e;
  }
  function pickHere(opts) {
    return new Promise(function (resolve) {
      var input = document.createElement('input');
      input.type = 'file';
      if (opts.multiple) input.multiple = true;
      if (opts.types && opts.types.length) input.accept = opts.types.map(function (x) { return '.' + x; }).join(',');
      input.style.display = 'none';
      input.addEventListener('change', function () {
        var fl = Array.prototype.slice.call(input.files || []);
        input.remove();
        if (!fl.length) { resolve(null); return; }
        Promise.all(fl.map(function (f) { return f.arrayBuffer().then(function (b) { return { name: f.name, data: b, where: 'local' }; }); })).then(resolve, function () { resolve(null); });
      });
      input.addEventListener('cancel', function () { input.remove(); resolve(null); });
      document.body.appendChild(input);
      input.click();
    });
  }
  var files = {
    open: function (opts) {
      var o = opts || {};
      var types = Array.isArray(o.types) ? o.types : [];
      if (!inShell) return pickHere({ types: types, multiple: !!o.multiple });
      return call('file.open', { types: types, multiple: !!o.multiple }, SHEET_MS).then(function (r) {
        if (r && r.ok) return r.files || [];
        if (r && r.error === 'cancelled') return null;
        throw fileErr(r);
      });
    },
    // save(name, data) is "Save as" (the sheet); save(name, data, { handle }) writes the file the user already
    // chose, without asking again. A handle the shell no longer knows falls back to the sheet.
    save: function (name, data, opts) {
      var handle = opts && opts.handle;
      if (inShell && handle) {
        return call('file.write', { handle: handle, data: data }, SHEET_MS).then(function (r) {
          if (r && r.ok) return { name: r.name || name, where: 'device', handle: handle };
          if (r && r.error === 'bad handle') return files.save(name, data);
          throw fileErr(r);
        });
      }
      if (!inShell) {
        var url = URL.createObjectURL(data instanceof Blob ? data : new Blob([data]));
        var a = document.createElement('a');
        a.href = url; a.download = name; a.style.display = 'none';
        document.body.appendChild(a); a.click(); a.remove();
        setTimeout(function () { URL.revokeObjectURL(url); }, 60000);
        return Promise.resolve({ name: name, where: 'download' });
      }
      return call('file.save', { name: name, data: data }, SHEET_MS).then(function (r) {
        if (r && r.ok) return { name: r.name || name, where: r.where || 'download', handle: r.handle || null };
        if (r && r.error === 'cancelled') return null;
        throw fileErr(r);
      });
    },
    launched: function () {
      if (!inShell) return Promise.resolve(null);
      return call('file.launch', {}, 120000).then(function (r) {
        return r && r.ok && r.file ? r.file : null;
      });
    },
    // The app's own files (binary), kept on the device: /data/apps/<id>/files/<name>. On its own, this browser.
    app: {
      read: function (name) {
        if (!inShell) return idb('readonly', function (st) { return st.get(name); }).then(function (v) { return v == null ? null : v; });
        return call('appfs.read', { name: name }, 120000).then(function (r) { if (r && r.ok) return r.data; throw fileErr(r); });
      },
      write: function (name, data) {
        if (!inShell) return toBuffer(data).then(function (b) { return idb('readwrite', function (st) { return st.put(b, name); }); }).then(function () {});
        return call('appfs.write', { name: name, data: data }, 120000).then(function (r) { if (!(r && r.ok)) throw fileErr(r); });
      },
      list: function () {
        if (!inShell) return idb('readonly', function (st) { return st.getAllKeys(); }).then(function (ks) { return (ks || []).map(function (k) { return { name: String(k), size: 0 }; }); });
        return call('appfs.list', {}).then(function (r) { if (r && r.ok) return r.entries; throw fileErr(r); });
      },
      remove: function (name) {
        if (!inShell) return idb('readwrite', function (st) { return st.delete(name); }).then(function () { return true; });
        return call('appfs.remove', { name: name }).then(function (r) { if (r && r.ok) return r.removed; throw fileErr(r); });
      },
    },
  };
  function toBuffer(d) {
    if (d instanceof ArrayBuffer) return Promise.resolve(d);
    if (ArrayBuffer.isView(d)) return Promise.resolve(d.buffer.slice(d.byteOffset, d.byteOffset + d.byteLength));
    if (typeof Blob !== 'undefined' && d instanceof Blob) return d.arrayBuffer();
    return Promise.resolve(new TextEncoder().encode(String(d)).buffer);
  }
  // On its own (development), the app's files live in this browser: one IndexedDB store per app.
  function idb(mode, fn) {
    return new Promise(function (resolve, reject) {
      var open = indexedDB.open('nucleo.app.' + appId + '.files', 1);
      open.onupgradeneeded = function () { open.result.createObjectStore('f'); };
      open.onerror = function () { reject(open.error); };
      open.onsuccess = function () {
        var tx = open.result.transaction('f', mode);
        var rq = fn(tx.objectStore('f'));
        tx.oncomplete = function () { resolve(rq.result); };
        tx.onerror = function () { reject(tx.error); };
      };
    });
  }

  // ── start ──
  var ready = (inShell ? call('sys.info') : Promise.resolve({ ok: true, lang: standaloneLang() })).then(function (info) {
    if (info && info.theme) setTheme(info.theme);
    return loadStrings((info && info.lang) || 'en');
  }).then(function () {
    if (document.readyState === 'loading') return new Promise(function (r) { document.addEventListener('DOMContentLoaded', r, { once: true }); });
  }).then(function () {
    apply();
    return { lang: api.lang, theme: api.theme };
  });

  window.NucleoApp = {
    ready: ready,
    t: t,
    apply: apply,
    kv: kv,
    caps: caps,
    files: files,
    get lang() { return api.lang; },
    get theme() { return api.theme; },
    get sandboxed() { return window.origin === 'null'; },
    onLang: function (fn) { if (typeof fn === 'function') listeners.lang.push(fn); },
    onTheme: function (fn) { if (typeof fn === 'function') listeners.theme.push(fn); },
    call: call,
  };
})();

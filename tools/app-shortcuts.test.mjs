// Integration tests for the per-app OS keyboard/clipboard handlers. Each app's inline <script
// type="module"> is loaded into a node:vm sandbox with a tiny DOM/window shim, then we drive it the
// way the shell does — by delivering {type:'os-shortcut'} / keydown events — and assert the
// side-effects (clipboard-write postMessages, file-API fetches, the central file-dialog request).
// Mirrors the existing tools/terminal-commands.test.mjs sandbox approach.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

// ---- a minimal DOM/window shim --------------------------------------------------------------
function makeEl(id) {
  const el = {
    id, tagName: 'DIV', _text: '', value: '', className: '', innerHTML: '',
    style: {}, dataset: {}, children: [], _listeners: {}, _selected: false,
    selectionStart: 0, selectionEnd: 0, placeholder: '',
    classList: { _s: new Set(), add(c){this._s.add(c);}, remove(c){this._s.delete(c);}, contains(c){return this._s.has(c);}, toggle(c,f){const on=f===undefined?!this._s.has(c):f; on?this._s.add(c):this._s.delete(c); return on;} },
    appendChild(c){ this.children.push(c); return c; },
    insertBefore(c, ref){ const i = ref ? this.children.indexOf(ref) : -1; if (i < 0) this.children.push(c); else this.children.splice(i, 0, c); return c; },
    removeChild(c){ const i = this.children.indexOf(c); if (i >= 0) this.children.splice(i, 1); return c; },
    append(...kids){ for (const k of kids) this.children.push(k); },
    prepend(...kids){ this.children.unshift(...kids); },
    remove(){},
    addEventListener(t, fn){ (this._listeners[t] = this._listeners[t] || []).push(fn); },
    removeEventListener(){}, focus(){ this._focused = true; }, blur(){},
    select(){ this._selected = true; }, setSelectionRange(){}, querySelector(){ return makeEl('q'); }, querySelectorAll(){ return []; },
    dispatchEvent(){ return true; }, getBoundingClientRect(){ return { left:0, top:0, width:300, height:150 }; },
    // canvas surface (Paint): a no-op 2D context + toBlob that yields a tiny fake PNG blob.
    width: 800, height: 600,
    getContext(){ return { fillStyle:'', strokeStyle:'', lineWidth:1, lineCap:'', lineJoin:'',
      fillRect(){}, strokeRect(){}, clearRect(){}, beginPath(){}, closePath(){}, moveTo(){}, lineTo(){}, arc(){}, ellipse(){}, rect(){}, fill(){}, stroke(){}, drawImage(){}, save(){}, restore(){}, translate(){}, scale(){},
      getImageData(x, y, w, h){ return { data: new Uint8ClampedArray(Math.max(1, (w||1) * (h||1)) * 4), width: w || 1, height: h || 1 }; }, putImageData(){} }; },
    toBlob(cb, type){ cb({ type: type || 'image/png', size: 8 }); },
    get textContent(){ return this._text; }, set textContent(v){ this._text = String(v); },
    set onclick(fn){ this._onclick = fn; }, get onclick(){ return this._onclick; },
  };
  return el;
}

function loadApp(rel, { search = '', fetchImpl } = {}) {
  const html = fs.readFileSync(path.resolve(rel), 'utf8');
  const m = html.match(/<script(?:\s+type="module")?>([\s\S]*?)<\/script>/);   // module or classic inline script
  assert.ok(m, 'inline script not found in ' + rel);

  const els = new Map();
  const getEl = (id) => { if (!els.has(id)) els.set(id, makeEl(id)); return els.get(id); };
  const winListeners = {};
  const postedToParent = [];
  const fetchCalls = [];
  const clipboardWrites = [];
  class ClipboardItem { constructor(parts){ this.types = Object.keys(parts || {}); } }

  const documentShim = {
    _title: '',
    get title(){ return this._title; }, set title(v){ this._title = String(v); },
    documentElement: makeEl('html'),
    body: makeEl('body'),
    getElementById: getEl,
    createElement: () => makeEl('new'),
    createRange: () => ({ selectNodeContents(){}, setStart(){}, setEnd(){} }),
    addEventListener(t, fn){ (winListeners[t] = winListeners[t] || []).push(fn); },
    removeEventListener(){}, querySelector(){ return null; }, querySelectorAll(){ return []; },
    get activeElement(){ return documentShim._active || null; },
  };

  const windowObj = {
    addEventListener(t, fn){ (winListeners[t] = winListeners[t] || []).push(fn); },
    removeEventListener(){},
    parent: { postMessage: (msg) => postedToParent.push(msg) },
    getSelection: () => ({ removeAllRanges(){}, addRange(){} }),
    location: { search, host: 'localhost', protocol: 'http:' },
    matchMedia: () => ({ matches: false, addEventListener(){}, addListener(){} }),
    getComputedStyle: () => ({ getPropertyValue: () => '' }),
    ClipboardItem,
  };

  const fetchDefault = async (url, opts) => { fetchCalls.push({ url: String(url), opts }); return { ok: true, status: 200, text: async () => '', json: async () => ({}) }; };

  const sandbox = {
    document: documentShim,
    window: windowObj,
    location: windowObj.location,
    navigator: { clipboard: { writeText: async () => {}, readText: async () => '5', write: async (items) => { clipboardWrites.push(items); }, read: async () => [] }, platform: 'Test', userAgent: 'node' },
    localStorage: { _m: new Map(), getItem(k){ return this._m.has(k) ? this._m.get(k) : null; }, setItem(k,v){ this._m.set(k, String(v)); }, removeItem(k){ this._m.delete(k); } },
    fetch: fetchImpl || fetchDefault,
    addEventListener(t, fn){ (winListeners[t] = winListeners[t] || []).push(fn); },     // top-level addEventListener('keydown')
    setTimeout: (fn) => { return 0; }, clearTimeout: () => {}, setInterval: () => 0, clearInterval: () => {},
    requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
    ImageData: class { constructor(data, w, h){ this.data = data; this.width = w; this.height = h; } },
    confirm: () => true, prompt: () => null, alert: () => {},
    URLSearchParams,
    URL: Object.assign(function URL(){}, { createObjectURL: () => 'blob:x', revokeObjectURL: () => {} }),
    Blob: class { constructor(p){ this.size = (p || []).join('').length; } },
    ClipboardItem,
    Image: class { constructor(){ this.onload = null; this.onerror = null; } set src(v){ this._src = v; } get src(){ return this._src; } },
    Uint8ClampedArray, FileReader: class { readAsDataURL(){ if (this.onload) this.onload(); } },
    KeyboardEvent: class { constructor(t, o = {}){ Object.assign(this, { type: t }, o); } },
    Event: class { constructor(t, o = {}){ Object.assign(this, { type: t }, o); } },
    MouseEvent: class { constructor(t, o = {}){ Object.assign(this, { type: t }, o); } },
    console, Math, Date, JSON, Promise, parseInt, parseFloat, isFinite, isNaN, String, Number, Array, Object, RegExp,
    // OS i18n engine stub: init() returns a t() passthrough (a string fallback arg if present, else the key).
    I18N: { init: () => ((k, ...rest) => { for (const a of rest) if (typeof a === 'string') return a; return String(k); }), setLang() {}, onChange() {}, getLang: () => 'it' },
  };
  // Drive listeners registered by the app.
  const emit = (type, eventObj) => { for (const fn of (winListeners[type] || [])) fn(eventObj); };
  const osShortcut = (action, extra = {}) => emit('message', { data: { type: 'os-shortcut', action, ...extra } });
  const keydown = (key, o = {}) => emit('keydown', { key, ctrlKey: !!o.ctrl, metaKey: !!o.meta, shiftKey: !!o.shift, preventDefault(){} });

  // Apps now import the OS i18n engine (`import I18N from '/nucleo-i18n.js'`) and init it with a top-level
  // await. vm.runInContext runs a SCRIPT (not a module): strip the import(s) + de-await the init (the
  // sandbox's I18N stub returns a t() passthrough). Paint's matched script is a classic <script> with no
  // import, so the strip is a harmless no-op there.
  const code = m[1]
    .replace(/^[ \t]*import\b[^\n]*\n/gm, '')
    .replace(/\bawait\s+(I18N\.init\s*\()/g, '$1');
  vm.createContext(sandbox);
  vm.runInContext(code, sandbox, { filename: rel });
  return { sandbox, els, getEl, postedToParent, fetchCalls, clipboardWrites, emit, osShortcut, keydown, documentShim, windowObj };
}

// ================= Calculator =================
test('Calculator: os-shortcut copy writes the result to the OS clipboard', () => {
  const app = loadApp('apps/calculator/www/index.html');
  app.keydown('6'); app.keydown('*'); app.keydown('7'); app.keydown('=');
  assert.equal(app.getEl('disp').textContent, '42', 'compute 6*7 via keyboard');
  app.osShortcut('copy');
  const clip = app.postedToParent.find((m) => m.type === 'clipboard-write');
  assert.ok(clip, 'a clipboard-write was posted to the shell');
  assert.deepEqual({ kind: clip.kind, data: clip.data }, { kind: 'text', data: '42' });
});

test('Calculator: os-shortcut paste appends a number from the OS clipboard', () => {
  const app = loadApp('apps/calculator/www/index.html');
  app.keydown('9');                                   // expr = "9"
  app.osShortcut('paste', { clip: { kind: 'text', data: ' 5 ' } });
  assert.equal(app.getEl('disp').textContent, '95', 'pasted number is appended');
});

test('Calculator: selectAll also copies the result (display-only widget)', () => {
  const app = loadApp('apps/calculator/www/index.html');
  app.keydown('1'); app.keydown('2'); app.keydown('3');
  app.osShortcut('selectAll');
  const clip = app.postedToParent.find((m) => m.type === 'clipboard-write');
  assert.equal(clip && clip.data, '123');
});

// ================= Notepad =================
test('Notepad: os-shortcut save on a titled file writes via the OS file API', async () => {
  const writes = [];
  const fetchImpl = async (url, opts) => {
    if (String(url).includes('/api/fs/read')) return { ok: true, status: 200, text: async () => 'hello' };
    if (String(url).includes('/api/fs/write')) { writes.push({ url: String(url), opts }); return { ok: true, status: 200, text: async () => '' }; }
    return { ok: true, status: 200, text: async () => '' };
  };
  const app = loadApp('apps/notepad/www/index.html', { search: '?path=/data/Documents/note.txt', fetchImpl });
  await new Promise((r) => setTimeout(r, 0));         // let the initial load()'s promise settle
  app.getEl('ed').value = 'edited content';
  app.osShortcut('save');
  await new Promise((r) => setTimeout(r, 0));
  assert.equal(writes.length, 1, 'exactly one write fired');
  assert.match(writes[0].url, /\/api\/fs\/write\?path=.*note\.txt/);
  assert.equal(writes[0].opts.method, 'POST');
  assert.equal(writes[0].opts.body, 'edited content');
});

test('Notepad: os-shortcut save on an UNTITLED doc opens the central Save-As dialog', () => {
  const app = loadApp('apps/notepad/www/index.html', { search: '' });   // no ?path → untitled
  app.osShortcut('save');
  const dlg = app.postedToParent.find((m) => m.type === 'os-file-dialog');
  assert.ok(dlg, 'a Save-As file dialog was requested from the shell');
  assert.equal(dlg.mode, 'save');
});

test('Notepad: os-shortcut open requests an open file dialog', () => {
  const app = loadApp('apps/notepad/www/index.html', { search: '' });
  app.osShortcut('open');
  const dlg = app.postedToParent.find((m) => m.type === 'os-file-dialog' && m.mode === 'open');
  assert.ok(dlg, 'an open file dialog was requested');
});

test('Notepad: os-shortcut new resets to an empty untitled buffer', () => {
  const app = loadApp('apps/notepad/www/index.html', { search: '?path=/data/x.txt' });
  app.getEl('ed').value = 'stuff';
  app.osShortcut('new');
  assert.equal(app.getEl('ed').value, '', 'editor cleared on New');
});

// ================= Tasks =================
function tasksApp() {
  const writes = [];
  const list = [{ id: 't1', text: 'buy milk', done: false, pri: 0, ts: 0 }, { id: 't2', text: 'pay bills', done: true, pri: 1, ts: 1 }];
  const fetchImpl = async (url, opts) => {
    const u = String(url);
    if (u.includes('/api/fs/read') && u.includes('tasks.todo')) return { ok: true, status: 200, text: async () => JSON.stringify(list) };
    if (u.includes('/api/fs/write') && u.includes('tasks.todo')) { writes.push({ url: u, body: opts && opts.body }); return { ok: true, status: 200, text: async () => '' }; }
    return { ok: true, status: 200, text: async () => '' };
  };
  return { app: loadApp('apps/tasks/www/index.html', { fetchImpl }), writes };
}

test('Tasks: os-shortcut copy posts the list to the OS clipboard as a Markdown checklist', async () => {
  const { app } = tasksApp();
  await new Promise((r) => setTimeout(r, 0));         // let init() load the list
  app.osShortcut('copy');
  const clip = app.postedToParent.find((m) => m.type === 'clipboard-write');
  assert.ok(clip, 'a clipboard-write was posted');
  assert.equal(clip.data, '- [ ] buy milk\n- [x] pay bills');
});

test('Tasks: os-shortcut new focuses the add input', async () => {
  const { app } = tasksApp();
  await new Promise((r) => setTimeout(r, 0));
  app.getEl('text')._focused = false;
  app.osShortcut('new');
  assert.equal(app.getEl('text')._focused, true);
});

test('Tasks: os-shortcut save flushes a write to the OS file API', async () => {
  const { app, writes } = tasksApp();
  await new Promise((r) => setTimeout(r, 0));
  writes.length = 0;
  app.osShortcut('save');
  await new Promise((r) => setTimeout(r, 0));
  assert.equal(writes.length, 1, 'one write flushed');
  assert.match(writes[0].url, /tasks\.todo/);
});

// ================= Paint =================
test('Paint: os-shortcut save on an untitled canvas opens the central Save dialog', () => {
  const app = loadApp('apps/paint/www/index.html', { search: '' });   // no ?path → untitled
  app.osShortcut('save');
  const dlg = app.postedToParent.find((m) => m.type === 'os-file-dialog' && m.mode === 'save');
  assert.ok(dlg, 'a save file dialog was requested');
  assert.ok((dlg.filter || []).includes('.png'), 'image filters offered');
});

test('Paint: os-shortcut open requests an open image dialog', () => {
  const app = loadApp('apps/paint/www/index.html', { search: '' });
  app.osShortcut('open');
  const dlg = app.postedToParent.find((m) => m.type === 'os-file-dialog' && m.mode === 'open');
  assert.ok(dlg, 'an open image dialog was requested');
});

test('Paint: os-shortcut copy writes the canvas image to the system clipboard', async () => {
  const app = loadApp('apps/paint/www/index.html', { search: '' });
  app.osShortcut('copy');
  await new Promise((r) => setTimeout(r, 0));         // copyImage awaits toBlob + clipboard.write
  assert.equal(app.clipboardWrites.length, 1, 'one clipboard.write happened');
  assert.ok(app.clipboardWrites[0][0].types.includes('image/png'), 'wrote a PNG ClipboardItem');
});

test('Paint: os-shortcut new clears the path (untitled)', () => {
  const app = loadApp('apps/paint/www/index.html', { search: '?path=/data/Pictures/x.png' });
  app.osShortcut('new');
  assert.equal(app.getEl('st-file').textContent, 'senza titolo', 'status shows untitled after New');
});

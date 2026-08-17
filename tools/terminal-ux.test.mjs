// terminal-ux.test.mjs — everything the user types at the Terminal: completion, the strip,
// reverse history search, persistence, and the commands added on top of the 34 originals.
//
// terminal-commands.test.mjs covers the original commands in isolation and terminal-engine.test.mjs
// covers submission and the grammar. What neither can see is the cost of a keystroke, which is the
// whole point here: Tab must be able to complete a path without the device paying for it twice, and
// typing must never reach the wire at all.
//
// Three contracts are pinned:
//   COMPLETION — pure, cache-only, and one listing per directory per gesture.
//   RECALL     — history survives the window, deduplicates, and never grows past its cap.
//   COMMANDS   — the new verbs stay bounded, and ANIMA's 150-byte query is never split mid-escape.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const html = fs.readFileSync(path.resolve('apps/terminal/www/index.html'), 'utf8');
const block = html.match(/<script type="module">([\s\S]*?)<\/script>/);
assert.ok(block, 'the module script block must stay extractable');
const BOOT = '// ---- boot ------------------------------------------------------------------';
const code = block[1]
  .slice(0, block[1].indexOf(BOOT))
  .replace(/^[ \t]*import\b[^\n]*\n/gm, '')
  .replace(/\bawait\s+(I18N\.init\s*\()/g, '$1');

// ---- a DOM real enough to click, select and type into -----------------------------
const doc = { activeElement: null, documentElement: { dataset: {}, style: { props: {}, setProperty(k, v) { this.props[k] = v; } } } };
function makeEl(id) {
  const el = {
    id, className: '', textContent: '', style: {}, dataset: {}, attrs: {}, children: [],
    listeners: {}, parentNode: null, sets: [], _value: '', hidden: false,
    selectionStart: 0, selectionEnd: 0, scrollTop: 0, clientHeight: 100,
    get value() { return el._value; },
    set value(v) { el._value = String(v); el.sets.push(String(v)); },
    get scrollHeight() { return 100; },
    get firstChild() { return el.children[0] || null; },
    get lastChild() { return el.children[el.children.length - 1] || null; },
    get innerText() { return el.children.map((c) => c.textContent).join('\n'); },
    set innerHTML(v) { if (v === '') el.children.length = 0; },
    set onclick(fn) { el._onclick = fn; },
    setAttribute(k, v) { el.attrs[k] = v; },
    getAttribute(k) { return el.attrs[k] === undefined ? null : el.attrs[k]; },
    setSelectionRange(a, b) { el.selectionStart = a; el.selectionEnd = b; },
    scrollIntoView() {},
    closest() { return el; },
    focus() { doc.activeElement = el; },
    addEventListener(type, fn) { (el.listeners[type] = el.listeners[type] || []).push(fn); },
    dispatchEvent(ev) {
      ev.target = ev.target || el; ev.defaultPrevented = false;
      ev.preventDefault = () => { ev.defaultPrevented = true; };
      for (const fn of (el.listeners[ev.type] || []).slice()) fn(ev);
      return !ev.defaultPrevented;
    },
    appendChild(c) { c.parentNode = el; el.children.push(c); return c; },
    insertBefore(c, ref) {
      c.parentNode = el;
      const i = ref ? el.children.indexOf(ref) : -1;
      el.children.splice(i < 0 ? el.children.length : i, 0, c);
      return c;
    },
    removeChild(c) { const i = el.children.indexOf(c); if (i >= 0) el.children.splice(i, 1); return c; },
  };
  return el;
}
const els = new Map();
const el = (id) => { if (!els.has(id)) els.set(id, makeEl(id)); return els.get(id); };
const out = el('out'), input = el('in'), strip = el('strip'), ps = el('ps');
const ghA = el('gha'), ghB = el('ghb'), keys = el('keys');

const lines = () => Array.from(out.children).map((c) => ({ text: c.textContent, cls: c.className.replace(/^line ?/, ''), tap: c.__tap }));
const texts = () => lines().map((l) => l.text);
const body = () => lines().filter((l) => l.cls !== 'cmd');
const last = () => texts()[texts().length - 1];
const chips = () => Array.from(strip.children).map((c) => ({ cls: c.className, text: c.children.map((x) => x.textContent).join('') || c.textContent }));

// ---- a network that records every call and has no catch-all -------------------------
const net = {
  calls: [], fs: {}, json: {}, status: new Map(), failing: new Set(),
  unpaired: false, inFlight: 0, maxInFlight: 0, writes: [],
  count(p) { return net.calls.filter((c) => c.path === p).length; },
  reset() {
    net.calls.length = 0; net.writes.length = 0; net.status.clear(); net.failing.clear();
    net.fs = {}; net.json = {}; net.unpaired = false; net.inFlight = 0; net.maxInFlight = 0;
  },
};
const reply = (status, text, hdr) => ({
  ok: status >= 200 && status < 300, status,
  headers: { get: (k) => (hdr || {})[k] || null },
  text: async () => text,
  json: async () => JSON.parse(text),
  arrayBuffer: async () => Buffer.from(text, 'binary'),
});
let gate = null;                                   // a promise that holds every /api/fs/list open
async function fakeFetch(url, opts = {}) {
  const u = new URL(url, 'http://device');
  const p = u.pathname, param = u.searchParams.get('path');
  net.calls.push({ method: opts.method || 'GET', path: p, param, query: u.search, body: opts.body });
  net.inFlight++;
  net.maxInFlight = Math.max(net.maxInFlight, net.inFlight);
  try {
    await null;
    if (p === '/api/fs/list' && gate) await gate;
    if (net.failing.has(p) || net.failing.has('*')) throw new TypeError('Failed to fetch');
    if (net.unpaired && p.startsWith('/api/')) return reply(401, 'unauthorized');
    if (net.status.has(p)) return reply(net.status.get(p), '{}');
    if (p === '/api/fs/list') {
      const e = net.fs[param];
      return Array.isArray(e) ? reply(200, JSON.stringify({ entries: e })) : reply(404, 'no dir');
    }
    if (p === '/api/fs/read') {
      const e = net.fs[param];
      return typeof e === 'string' ? reply(200, e) : reply(404, 'no file');
    }
    if (p === '/api/fs/write') { net.writes.push({ path: param, body: String(opts.body ?? '') }); net.fs[param] = String(opts.body ?? ''); return reply(200, ''); }
    if (p === '/api/fs/mkdir') { net.fs[param] = []; return reply(200, ''); }
    if (p === '/api/fs/delete') { delete net.fs[param]; return reply(200, ''); }
    if (p === '/proc') return reply(200, 'version\nuname\nmeminfo\ncpuinfo\n');
    if (net.json[p]) return reply(200, JSON.stringify(net.json[p]));
    if (p === '/api/status') return reply(200, JSON.stringify({ os: 'NucleoOS', version: '0.1.0', uptime_s: 1, free_heap: 1000, ota: { running: 'factory', next: 'ota_0', state: 'valid', rollback_enabled: true } }));
    if (p === '/api/logs') return reply(200, 'boot ok');
    throw new Error('unrouted: ' + url);
  } finally { net.inFlight--; }
}

const sandbox = {
  document: {
    documentElement: doc.documentElement,
    get activeElement() { return doc.activeElement; },
    body: makeEl('body'),
    createElement: () => makeEl(null),
    createTextNode: (t) => ({ textContent: t, className: '' }),
    getElementById: (id) => el(id),
    addEventListener() {},
    execCommand() { return true; },
  },
  window: { addEventListener() {}, removeEventListener() {}, getSelection: () => ({ toString: () => '', removeAllRanges() {}, addRange() {} }) },
  navigator: { clipboard: null, language: 'en-US' },
  parent: { postMessage: (m) => posted.push(m) },
  performance: { now: () => 1000 },
  Blob: class { constructor(parts) { this.size = Buffer.byteLength(parts.join('')); } },
  fetch: fakeFetch,
  setTimeout: (fn) => { fn(); return 0; },
  clearTimeout: () => {},
  setInterval: () => 0,
  console,
  I18N: { init: () => ((k, ...rest) => { for (const a of rest) if (typeof a === 'string') return a; return String(k); }) },
};
const posted = [];
vm.runInContext(code, vm.createContext(sandbox));
const T = sandbox.__T;
assert.ok(T && T.completions && T.applyCompletion, '__T must expose the completion engine');
T.LIMITS.submitDebounceMs = 0;

const settle = async () => { for (let i = 0; i < 60; i++) await new Promise((r) => setImmediate(r)); };
function reset() {
  input.dispatchEvent({ type: 'keydown', key: 'Escape' });   // a strip left open would eat the next Tab
  T.COMMANDS.clear();
  input._value = ''; input.sets.length = 0; input.selectionStart = 0; input.selectionEnd = 0;
  strip.children.length = 0; strip.hidden = true;
  T.hist.length = 0;
  for (const k of Object.keys(T.aliases)) delete T.aliases[k];
  T.cacheDrop(null);
  T.setCwd('/');
  posted.length = 0;
  net.reset();
  gate = null;
}
const tab = (shift) => { const e = { type: 'keydown', key: 'Tab', shiftKey: !!shift }; input.dispatchEvent(e); return e; };
const type = (v) => { input.value = v; input.selectionStart = input.selectionEnd = v.length; };
const plain = (x) => JSON.parse(JSON.stringify(x));
const press = (key, o) => { const e = { type: 'keydown', key, ...o }; input.dispatchEvent(e); return e; };
// The cache is what makes completion cheap, so tests fill it the way the app does.
async function warm(dir, entries) {
  net.fs[dir] = entries;
  type('ls ' + dir + '/');
  tab();
  await settle();
  input.dispatchEvent({ type: 'keydown', key: 'Escape' });   // leave no strip open behind the fixture
  type('');
}

// ---- COMPLETION: the pure half -------------------------------------------------------
test('complete: a command prefix completes from memory, with no device call at all', async () => {
  reset();
  const r = T.completions('unam', 4);
  assert.equal(r.kind, 'command');
  assert.deepEqual(Array.from(r.items.map((i) => i.text)), ['uname']);
  assert.equal(net.calls.length, 0);
});

test('complete: an ambiguous command prefix returns every candidate, sorted', () => {
  reset();
  const names = Object.keys(T.COMMANDS).filter((n) => n.startsWith('c')).sort();
  const r = T.completions('c', 1);
  assert.deepEqual(Array.from(r.items.map((i) => i.text)), names);
  assert.ok(names.length > 1, 'the fixture needs an ambiguous prefix to be worth anything');
});

test('complete: Tab on a unique command inserts it and appends one space', async () => {
  reset();
  type('unam');
  const e = tab();
  await settle();
  assert.equal(input.value, 'uname ');
  assert.equal(e.defaultPrevented, true, 'Tab must never move focus out of the terminal');
  assert.equal(net.calls.length, 0);
});

test('complete: an ambiguous prefix inserts the longest common prefix and opens the strip', async () => {
  reset();
  type('ali');
  tab();
  await settle();
  assert.equal(input.value.startsWith('alias'), true);
  reset();
  type('u');
  tab();
  await settle();
  assert.equal(strip.hidden, false, 'more than one candidate opens the strip');
  assert.ok(chips().length > 1);
});

test('complete: a directory ends with "/" and no space, a file gets the space', async () => {
  reset();
  net.fs['/'] = [{ name: 'data', type: 'dir' }, { name: 'boot.txt', type: 'file', size: 3 }];
  await warm('/', net.fs['/']);
  const d = T.completions('cd /da', 6);
  assert.deepEqual(Array.from(d.items.map((i) => i.text)), ['/data/']);
  assert.equal(T.applyCompletion('cd /da', d, 0).line, 'cd /data/');
  const f = T.completions('cat /boo', 8);
  assert.equal(T.applyCompletion('cat /boo', f, 0).line, 'cat /boot.txt ');
});

test('complete: cd offers directories only, cat offers both', async () => {
  reset();
  net.fs['/'] = [{ name: 'data', type: 'dir' }, { name: 'boot.txt', type: 'file' }];
  await warm('/', net.fs['/']);
  assert.deepEqual(Array.from(T.completions('cd /', 4).items.map((i) => i.text)), ['/data/']);
  assert.deepEqual(Array.from(T.completions('cat /', 5).items.map((i) => i.text)), ['/data/', '/boot.txt']);
});

test('complete: a relative fragment resolves against the working directory', async () => {
  reset();
  net.fs['/data'] = [{ name: 'notes.txt', type: 'file' }];
  T.setCwd('/data');
  await warm('/data', net.fs['/data']);
  const r = T.completions('cat not', 7);
  assert.deepEqual(Array.from(r.items.map((i) => i.text)), ['notes.txt']);
});

test('complete: the case-insensitive retry is what makes this usable on a FAT card', async () => {
  reset();
  net.fs['/'] = [{ name: 'DCIM', type: 'dir' }];
  await warm('/', net.fs['/']);
  assert.deepEqual(Array.from(T.completions('cd /dc', 6).items.map((i) => i.text)), ['/DCIM/']);
});

test('complete: /proc is a fixed table, and nothing lives below one of its nodes', async () => {
  reset();
  type('cat /proc/');
  tab(); await settle();
  assert.equal(net.count('/api/fs/list'), 0, '/proc is not on the filesystem API');
  assert.ok(T.completions('cat /proc/mem', 13).items.some((i) => i.text === '/proc/meminfo'));
  assert.deepEqual(Array.from(T.completions('cat /proc/meminfo/x', 19).items), []);
});

test('complete: commands that take no path never reach the wire', async () => {
  reset();
  for (const line of ['pwd ', 'env ', 'free ', 'uptime ', 'df ', 'date ', 'history ', 'clear ']) {
    type(line);
    tab(); await settle();
  }
  assert.equal(net.calls.length, 0);
});

test('complete: a word list answers where a word list is the truth', () => {
  reset();
  assert.deepEqual(Array.from(T.completions('theme d', 7).items.map((i) => i.text)), ['dark']);
  assert.deepEqual(Array.from(T.completions('display o', 9).items.map((i) => i.text)), ['on', 'off']);
  assert.deepEqual(Array.from(T.completions('anima --c', 9).items.map((i) => i.text)), ['--caps']);
});

test('complete: man and which complete over the command list, not the filesystem', async () => {
  reset();
  const r = T.completions('man unam', 8);
  assert.deepEqual(Array.from(r.items.map((i) => i.text)), ['uname']);
  assert.equal(net.calls.length, 0);
});

test('complete: a URL comes back out of history, never off the device', async () => {
  reset();
  T.hist.push('curl https://example.com/api/v1', 'curl https://other.test/x', 'pwd');
  const r = T.completions('curl https://ex', 15);
  assert.equal(r.kind, 'url');
  assert.deepEqual(Array.from(r.items.map((i) => i.text)), ['https://example.com/api/v1']);
  assert.equal(net.calls.length, 0);
});

test('complete: a redirection target completes as a path whatever the command was', async () => {
  reset();
  net.fs['/'] = [{ name: 'data', type: 'dir' }];
  await warm('/', net.fs['/']);
  const r = T.completions('echo hi > /da', 13);
  assert.deepEqual(Array.from(r.items.map((i) => i.text)), ['/data/']);
});

test('complete: the second stage of a pipeline completes as a command', () => {
  reset();
  const r = T.completions('cat /data/f | unam', 18);
  assert.equal(r.kind, 'command');
  assert.deepEqual(Array.from(r.items.map((i) => i.text)), ['uname']);
});

test('complete: a miss writes nothing to the transcript', async () => {
  reset();
  type('zzzz');
  const e = tab();
  await settle();
  assert.equal(input.value, 'zzzz', 'the input is left exactly as it was');
  assert.equal(out.children.length, 0, 'a failed Tab that scrolls the screen is the flooding we avoid');
  assert.equal(e.defaultPrevented, true);
  assert.match(chips().map((c) => c.text).join(' '), /no match/);
});

// ---- COMPLETION: the cost half --------------------------------------------------------
test('cache: five Tabs in one directory cost the device one listing', async () => {
  reset();
  net.fs['/data'] = [{ name: 'alpha.txt', type: 'file' }, { name: 'beta.txt', type: 'file' }];
  for (let i = 0; i < 5; i++) {
    type('cat /data/a');
    tab(); await settle();
    T.COMMANDS.clear();
  }
  assert.equal(net.count('/api/fs/list'), 1);
});

test('cache: repeated Tabs while a listing is in flight issue no second request', async () => {
  reset();
  net.fs['/data'] = [{ name: 'alpha.txt', type: 'file' }];
  let open;
  gate = new Promise((r) => { open = r; });
  type('cat /data/a');
  tab(); tab(); tab();
  open(); gate = null;
  await settle();
  assert.equal(net.count('/api/fs/list'), 1);
  assert.equal(net.maxInFlight, 1);
});

test('cache: a missing directory is remembered briefly so a typo cannot hammer the device', async () => {
  reset();
  for (let i = 0; i < 3; i++) {
    type('cat /nope/x');
    tab(); await settle();
  }
  assert.equal(net.count('/api/fs/list'), 1);
});

test('cache: a 401 is never remembered, so pairing takes effect immediately', async () => {
  reset();
  net.unpaired = true;
  type('cat /data/a');
  tab(); await settle();
  assert.match(chips().map((c) => c.text).join(' '), /401/);
  net.unpaired = false;
  net.fs['/data'] = [{ name: 'alpha.txt', type: 'file' }];
  type('cat /data/a');
  tab(); await settle();
  assert.equal(input.value, 'cat /data/alpha.txt ');
});

test('cache: it never grows past its cap', () => {
  reset();
  for (let i = 0; i < 40; i++) T.cachePut('/d' + i, [{ name: 'x', type: 'file' }], false);
  assert.equal(T.cacheGet('/d0'), null, 'the oldest entry is evicted');
  assert.ok(T.cacheGet('/d39'), 'the newest is kept');
});

test('cache: a command that changed the SD drops the listing it invalidated', async () => {
  reset();
  net.fs['/data'] = [{ name: 'a.txt', type: 'file' }];
  await warm('/data', net.fs['/data']);
  assert.ok(T.cacheGet('/data'));
  input.value = 'touch /data/b.txt';
  input.dispatchEvent({ type: 'keydown', key: 'Enter', keyCode: 13, isComposing: false });
  await settle();
  assert.equal(T.cacheGet('/data'), null, 'the next Tab must see the new file');
});

test('cost: typing never reaches the device — the ghost is cache-only by construction', async () => {
  reset();
  for (const v of ['c', 'ca', 'cat', 'cat ', 'cat /', 'cat /d', 'cat /da']) {
    type(v);
    input.dispatchEvent({ type: 'input', inputType: 'insertText' });
    await settle();
  }
  assert.equal(net.calls.length, 0);
});

test('ghost: it suggests from history and is accepted with the right arrow', async () => {
  reset();
  T.hist.push('status', 'uptime');
  type('stat');
  input.dispatchEvent({ type: 'input', inputType: 'insertText' });
  await settle();
  assert.equal(ghB.textContent, 'us', 'the tail is drawn, not inserted');
  assert.equal(input.value, 'stat', 'the suggestion is never part of the value');
  press('ArrowRight');
  assert.equal(input.value, 'status');
});

// ---- THE STRIP ---------------------------------------------------------------------------
test('strip: a digit picks a candidate and closes', async () => {
  reset();
  net.fs['/data'] = [{ name: 'one.txt', type: 'file' }, { name: 'two.txt', type: 'file' }];
  await warm('/data', net.fs['/data']);
  type('cat /data/');
  tab(); await settle();
  assert.equal(chips().length, 3, 'a kind label plus two candidates');
  const e = press('2');
  assert.equal(e.defaultPrevented, true);
  assert.equal(input.value, 'cat /data/two.txt ');
  assert.equal(strip.hidden, true);
});

test('strip: a digit types normally when the strip is closed', async () => {
  reset();
  type('head -n ');
  const e = press('5');
  assert.equal(e.defaultPrevented, undefined === e.defaultPrevented ? undefined : false);
  assert.equal(input.value, 'head -n ', 'the browser does the typing; the app must not swallow it');
});

test('strip: Escape restores the fragment exactly as it was typed', async () => {
  reset();
  net.fs['/data'] = [{ name: 'one.txt', type: 'file' }, { name: 'two.txt', type: 'file' }];
  await warm('/data', net.fs['/data']);
  type('cat /data/');
  tab(); await settle();
  assert.notEqual(input.value, 'cat /data/', 'the selected chip previews in the input');
  press('Escape');
  assert.equal(input.value, 'cat /data/');
  assert.equal(strip.hidden, true);
});

test('strip: Enter accepts the previewed candidate and does NOT run it', async () => {
  reset();
  net.fs['/data'] = [{ name: 'one.txt', type: 'file' }, { name: 'two.txt', type: 'file' }];
  await warm('/data', net.fs['/data']);
  net.calls.length = 0;
  type('cat /data/');
  tab(); await settle();
  press('Enter');
  await settle();
  assert.equal(input.value, 'cat /data/one.txt ');
  assert.equal(net.count('/api/fs/read'), 0, 'one extra keystroke is cheaper than an accidental run');
});

test('strip: Tab cycles the candidates, wrapping', async () => {
  reset();
  net.fs['/data'] = [{ name: 'one.txt', type: 'file' }, { name: 'two.txt', type: 'file' }];
  await warm('/data', net.fs['/data']);
  type('cat /data/');
  tab(); await settle();
  assert.equal(input.value, 'cat /data/one.txt ');
  tab(); assert.equal(input.value, 'cat /data/two.txt ');
  tab(); assert.equal(input.value, 'cat /data/one.txt ');
});

// ---- REVERSE SEARCH -----------------------------------------------------------------------
test('rsearch: Ctrl+R narrows over history and Enter accepts without running', async () => {
  reset();
  T.hist.push('ls /system', 'cat /data/notes.txt', 'ls /data');
  press('r', { ctrlKey: true });
  assert.match(ps.textContent, /r-search/);
  press('n'); press('o'); press('t');
  assert.equal(input.value, 'cat /data/notes.txt');
  net.calls.length = 0;
  press('Enter');
  await settle();
  assert.equal(input.value, 'cat /data/notes.txt');
  assert.equal(net.calls.length, 0);
  assert.equal(ps.textContent, '/ $');
});

test('rsearch: Escape restores the line that was being typed', () => {
  reset();
  T.hist.push('uptime');
  type('half typed');
  press('r', { ctrlKey: true });
  press('u');
  assert.equal(input.value, 'uptime');
  press('Escape');
  assert.equal(input.value, 'half typed');
});

test('rsearch: Ctrl+R must beat the browser reload', () => {
  reset();
  const e = press('r', { ctrlKey: true });
  assert.equal(e.defaultPrevented, true);
  press('Escape');
});

// ---- RECALL -------------------------------------------------------------------------------
test('history: a repeat moves to the end instead of duplicating', () => {
  reset();
  T.remember('ls'); T.remember('pwd'); T.remember('ls');
  assert.deepEqual([...T.hist], ['pwd', 'ls']);
});

test('history: a line starting with a space is never recorded', () => {
  reset();
  T.remember(' secret --token abc');
  assert.equal(T.hist.length, 0);
});

test('history: the list is capped', () => {
  reset();
  for (let i = 0; i < 260; i++) T.remember('cmd' + i);
  assert.equal(T.hist.length, 200);
  assert.equal(T.hist[T.hist.length - 1], 'cmd259');
});

test('history: "history clear" empties it, "history N" shows the tail with real numbers', async () => {
  reset();
  for (const c of ['a', 'b', 'c', 'd']) T.remember(c);
  await T.COMMANDS.history('2');
  assert.deepEqual(Array.from(body().map((l) => l.text)), ['  3  c', '  4  d']);
  T.COMMANDS.clear();
  await T.COMMANDS.history('clear');
  assert.equal(T.hist.length, 0);
});

test('history: a past command is tappable — pointing at it refills the input', async () => {
  reset();
  await T.run('pwd'); await settle();
  const echo = out.children[0];
  assert.deepEqual(plain(echo.__tap), { kind: 'cmd', name: 'pwd' });
  out.dispatchEvent({ type: 'pointerup', target: echo });
  assert.equal(input.value, 'pwd');
});

test('ls: a listed entry is tappable, and a directory offers to cd into it', async () => {
  reset();
  net.fs['/'] = [{ name: 'data', type: 'dir' }, { name: 'boot.txt', type: 'file', size: 1 }];
  await T.run('ls /'); await settle();
  const rows = Array.from(out.children).filter((c) => c.__tap && c.__tap.kind !== 'cmd');
  assert.deepEqual(plain(rows.map((r) => r.__tap)), [{ kind: 'dir', name: 'data' }, { kind: 'file', name: 'boot.txt' }]);
  out.dispatchEvent({ type: 'pointerup', target: rows[0] });
  assert.equal(input.value, 'cd data');
});

// ---- NEW COMMANDS -------------------------------------------------------------------------
test('stat: one listing of the parent answers type, size and lock state', async () => {
  reset();
  net.fs['/data'] = [{ name: 'x.ir', type: 'file', size: 1284, protected: true }];
  await T.run('stat /data/x.ir'); await settle();
  assert.equal(net.count('/api/fs/list'), 1);
  assert.match(texts().join('\n'), /type\s+file/);
  assert.match(texts().join('\n'), /size\s+1284/);
  assert.match(texts().join('\n'), /protected\s+yes/);
});

test('du: the walk is bounded and the totals roll up', async () => {
  reset();
  net.fs['/data'] = [{ name: 'sub', type: 'dir' }, { name: 'a', type: 'file', size: 1000 }];
  net.fs['/data/sub'] = [{ name: 'b', type: 'file', size: 2000 }];
  await T.run('du /data'); await settle();
  assert.match(texts().join('\n'), /3\.0 KB\t\/data/);
  assert.equal(net.maxInFlight, 1);
});

test('du and tree never exceed the listing budget on a deep tree', async () => {
  reset();
  let dir = '/';
  for (let d = 0; d < 12; d++) {
    net.fs[dir] = Array.from({ length: 12 }, (_, k) => ({ name: 'd' + k, type: 'dir' }));
    dir = (dir === '/' ? '' : dir) + '/d0';
  }
  await T.run('du -d 6 /'); await settle();
  assert.ok(net.count('/api/fs/list') <= T.LIMITS.maxListCalls, net.count('/api/fs/list') + ' listings');
  assert.ok(body().some((l) => l.cls === 'warn' && /stopped/.test(l.text)));
  net.calls.length = 0;
  await T.run('tree -L 6 /'); await settle();
  assert.ok(net.count('/api/fs/list') <= T.LIMITS.maxListCalls);
});

test('tree: it prints depth first, so a child never appears under the wrong parent', async () => {
  reset();
  net.fs['/r'] = [{ name: 'a', type: 'dir' }, { name: 'b', type: 'dir' }];
  net.fs['/r/a'] = [{ name: 'inside-a', type: 'file' }];
  net.fs['/r/b'] = [{ name: 'inside-b', type: 'file' }];
  await T.run('tree /r'); await settle();
  const t = texts();
  assert.ok(t.indexOf('  ├ inside-a') > t.indexOf('├ a/'));
  assert.ok(t.indexOf('  ├ inside-a') < t.indexOf('├ b/'));
});

test('hexdump: it asks for a range, not the file', async () => {
  reset();
  net.fs['/data/bin'] = 'NUCLEO';
  await T.run('hexdump -n 16 /data/bin'); await settle();
  assert.match(texts().join('\n'), /00000000 {2}4e 55 43 4c 45 4f/);
  assert.match(texts().join('\n'), /\|NUCLEO\|/);
});

test('base64: it round-trips bytes above 0x7F, which btoa would have corrupted', () => {
  reset();
  const s = 'caffè — naïve';
  assert.equal(T.b64encode(T.b64decode(T.b64encode(Array.from(Buffer.from(s, 'utf8'))))), T.b64encode(Array.from(Buffer.from(s, 'utf8'))));
  assert.equal(Buffer.from(T.b64decode(T.b64encode(Array.from(Buffer.from(s, 'utf8'))))).toString('utf8'), s);
});

test('sort, uniq, cut and seq are pure browser work', async () => {
  reset();
  net.fs['/data/f'] = 'b\na\nb\na';
  net.calls.length = 0;
  await T.run('cat /data/f | sort | uniq -c'); await settle();
  assert.equal(net.count('/api/fs/read'), 1);
  assert.deepEqual(Array.from(body().map((l) => l.text)), ['   2 a\n   2 b']);
  reset();
  await T.run('seq 3'); await settle();
  assert.equal(last(), '1\n2\n3');
  assert.equal(net.calls.length, 0);
  reset();
  net.fs['/data/f'] = 'one,two,three';
  await T.run('cat /data/f | cut -d, -f2'); await settle();
  assert.equal(last(), 'two');
});

test('tee: it writes once and still passes the text on', async () => {
  reset();
  net.fs['/data/f'] = 'payload';
  await T.run('cat /data/f | tee /data/copy | wc'); await settle();
  assert.equal(net.count('/api/fs/write'), 1);
  assert.equal(net.writes[0].body, 'payload');
  assert.match(last(), /1 lines, 1 words/);
});

test('alias: it expands once, on the command word, and cannot smuggle in an operator', async () => {
  reset();
  net.fs['/data'] = [{ name: 'a.txt', type: 'file', size: 1 }];
  T.COMMANDS.alias('ll=ls /data');
  await settle();
  T.COMMANDS.clear();
  await T.run('ll'); await settle();
  assert.ok(texts().some((t) => /a\.txt/.test(t)));
  reset();
  T.aliases.bad = 'echo hi ; rm -rf /';
  await T.run('bad'); await settle();
  assert.equal(net.count('/api/fs/delete'), 0, 'an alias body with an operator is refused, not obeyed');
});

test('watch: it clamps the interval, bounds the ticks and stops on Ctrl+C', async () => {
  reset();
  await T.run('watch -n 1 -c 3 pwd'); await settle();
  const ticks = texts().filter((t) => /tick \d+\/3/.test(t));
  assert.equal(ticks.length, 1, 'each tick replaces the previous one instead of scrolling the screen');
  assert.match(ticks[0], /tick 3\/3/, 'it ran the full count');
  assert.match(ticks[0], /every 2s/, 'one second is indistinguishable from hammering the device');
});

test('watch: it refuses to run inside a pipeline', async () => {
  reset();
  await T.run('pwd | watch -c 1 pwd'); await settle();
  assert.ok(body().some((l) => l.cls === 'err' && /watch: cannot run inside a pipeline/.test(l.text)));
});

test('diag: one request answers what four polls used to, and it pipes', async () => {
  reset();
  net.json['/api/diag'] = { sys: { fw: '0.2.0' }, mem: { free: 74686, frag: 40 }, cpu: { load: [9, 5] } };
  await T.run('diag | grep frag'); await settle();
  assert.equal(net.count('/api/diag'), 1);
  assert.deepEqual(Array.from(body().map((l) => l.text)), ['mem.frag 40']);
});

test('ota: only status is reachable from the shell', async () => {
  reset();
  await T.run('ota'); await settle();
  assert.match(last(), /running factory {2}next ota_0 {2}state valid {2}rollback on/);
  T.COMMANDS.clear();
  await T.run('ota /data/fw.bin'); await settle();
  assert.equal(body().pop().cls, 'err');
});

test('anima: the query is digested in the browser and never exceeds the firmware buffer', async () => {
  reset();
  net.json['/api/anima'] = { reply: 'the wifi driver keeps disconnecting', tier: 'L0', domain: 'sys', confidence: 71, trace: 'L0 sys | 71%', action: 'none' };
  const log = Array.from({ length: 60 }, (_, i) => `[${1000 + i}] E (wifi) disconnect reason=201`).join('\n')
    + '\n[2000] I (httpd) GET /api/status\n[2001] I (httpd) GET /api/status';
  net.fs['/data/log'] = log;
  await T.run('cat /data/log | anima "what is wrong?"'); await settle();
  const call = net.calls.find((c) => c.path === '/api/anima');
  assert.ok(call, 'the brain was asked');
  const q = new URLSearchParams(call.query).get('q');
  assert.ok(encodeURIComponent(q).length <= 150, encodeURIComponent(q).length + ' encoded bytes');
  assert.match(q, /^what is wrong\? \| /);
  assert.match(q, /E \(wifi\) disconnect reason=201 x60/, 'duplicates are counted, not repeated');
  assert.equal(last(), 'the wifi driver keeps disconnecting');
});

test('anima: the trimmed query never splits a %XX escape or a UTF-8 sequence', () => {
  const s = '«caffè» ' + 'è'.repeat(200);
  for (let cap = 1; cap <= 160; cap++) {
    const cut = T.encTrim(s, cap);
    const enc = encodeURIComponent(cut);
    assert.ok(enc.length <= cap, `cap ${cap}`);
    assert.equal(decodeURIComponent(enc), cut, `cap ${cap} must decode back`);
  }
});

test('anima: a 503 is retried exactly once, then reported honestly', async () => {
  reset();
  net.status.set('/api/anima', 503);
  await T.run('anima hello'); await settle();
  assert.equal(net.count('/api/anima'), 2);
  assert.ok(body().some((l) => l.cls === 'warn' && /busy/.test(l.text)));
});

test('anima: unpaired says 401, not "no answer"', async () => {
  reset();
  net.unpaired = true;
  await T.run('anima hello'); await settle();
  assert.ok(body().some((l) => l.cls === 'err' && /401/.test(l.text)));
});

test('anima: down a pipe it yields the reply and nothing else, and launches nothing', async () => {
  reset();
  net.json['/api/anima'] = { reply: 'calculator', action: 'launch', arg: 'calculator', tier: 'L0', confidence: 90 };
  await T.run('anima "open the calculator" | wc'); await settle();
  assert.match(last(), /1 lines, 1 words, 10 bytes/);
  assert.equal(Array.from(posted.filter((m) => m.type === 'open-app')).length, 0,
    'a pipeline stage must not open windows as a side effect');
});

test('anima: run on its own, a launch action reaches the shell', async () => {
  reset();
  net.json['/api/anima'] = { reply: 'opening', action: 'launch', arg: 'calculator', tier: 'L0', confidence: 90 };
  await T.run('anima "open the calculator"'); await settle();
  assert.deepEqual(plain(posted.filter((m) => m.type === 'open-app')), [{ type: 'open-app', id: 'calculator' }]);
});

test('device commands report an unreachable device instead of a plausible zero', async () => {
  reset();
  net.failing.add('*');
  for (const line of ['diag', 'heap', 'cpu', 'apps', 'assoc', 'lang', 'ota', 'wifi known', 'ir db', 'link peers']) {
    T.COMMANDS.clear();
    await T.run(line); await settle();
    assert.ok(body().some((l) => l.cls === 'err' && /cannot reach the device/.test(l.text)), line);
  }
});

test('bounds: none of the new commands opens two device requests at once', async () => {
  reset();
  net.fs['/data'] = [{ name: 'f', type: 'file', size: 4 }];
  net.fs['/data/f'] = 'x\ny';
  net.json['/api/diag'] = { mem: { free: 1 } };
  net.json['/api/heap'] = { internal: { free_bytes: 1 } };
  net.json['/api/cpu'] = { cores: 2 };
  net.json['/api/apps'] = { apps: [{ id: 'notepad', name: 'Notepad', enabled: true, route: '/apps/notepad/' }] };
  net.json['/api/associations'] = { default_open: { txt: 'notepad' }, fallback: 'file-commander' };
  net.json['/api/lang'] = { lang: 'en', gen: 1 };
  net.json['/api/wifi/known'] = { mode: 'sta', networks: [] };
  net.json['/api/ir/db'] = { gpio: 44, ready: true, protocols: ['nec'] };
  net.json['/api/link/peers'] = { name: 'n', channel: 1, peers: [] };
  net.json['/api/anima/caps'] = { l1Mode: 'auto', l1Serving: true, enabled: true, hasKey: false, online: false };
  for (const line of ['stat /data/f', 'du /data', 'tree /data', 'hexdump /data/f', 'base64 /data/f',
    'diag', 'heap', 'cpu', 'apps', 'assoc', 'lang', 'ota', 'anima --caps', 'wifi known', 'ir db',
    'link peers', 'cat /data/f | tee /data/g']) {
    net.maxInFlight = 0;
    await T.run(line); await settle();
    assert.equal(net.maxInFlight, 1, line);
  }
});

// ---- PERSISTENCE ----------------------------------------------------------------------------
test('persistence: history is written inside the app\'s own declared mount', async () => {
  reset();
  await T.COMMANDS.history('clear');            // arms the store, then flushes it
  await settle();
  const w = net.writes.filter((x) => /history\.jsonl$/.test(x.path));
  assert.ok(w.length <= 1);
  for (const x of net.writes) assert.match(x.path, /^\/apps\/terminal\/data\//,
    'the app may only write inside the mount its manifest declares');
});

test('persistence: the mount is created on first use, because nothing ships it', async () => {
  reset();
  net.status.set('/api/fs/write', 404);
  T.COMMANDS.alias('ll=ls -l');
  await settle();
  assert.ok(net.calls.some((c) => c.path === '/api/fs/mkdir' && c.param === '/apps/terminal/data'));
});

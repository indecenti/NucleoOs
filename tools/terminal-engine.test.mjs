// terminal-engine.test.mjs — the Terminal's shell engine: submission, grammar, caps.
//
// tools/terminal-commands.test.mjs covers the 34 commands in isolation; it cannot see the input
// layer at all (its #in mock discards every listener), which is exactly how a terminal that does
// not react to a mobile keyboard shipped. This suite therefore builds a DOM real enough to
// dispatch events and to be selected from, and reads the engine through `globalThis.__T`.
//
// Three contracts are pinned here:
//   SUBMIT — one gesture runs one command, whichever of the four signals a keyboard emits.
//   GRAMMAR — quoting, $VAR expansion AFTER tokenising, pipes, redirection, ; && || chaining.
//   BOUNDS — no command floods the scrollback, and none opens two device requests at once.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';

const html = fs.readFileSync(path.resolve('apps/terminal/www/index.html'), 'utf8');
const block = html.match(/<script type="module">([\s\S]*?)<\/script>/);
assert.ok(block, 'the module script block must stay extractable');
const BOOT = '// ---- boot ------------------------------------------------------------------';
assert.ok(block[1].includes(BOOT), 'the boot marker must survive: the harness slices on it');

const code = block[1]
  .slice(0, block[1].indexOf(BOOT))
  .replace(/^[ \t]*import\b[^\n]*\n/gm, '')
  .replace(/\bawait\s+(I18N\.init\s*\()/g, '$1');

// ---- a DOM with working events, values and child lists ---------------------------
const doc = { activeElement: null, documentElement: { dataset: {} } };
function makeEl(id) {
  const el = {
    id, className: '', textContent: '', style: {}, dataset: {}, children: [],
    listeners: {}, parentNode: null, sets: [], _value: '',
    selectionStart: 0, selectionEnd: 0,
    scrollTop: 0, clientHeight: 100,
    get value() { return el._value; },
    set value(v) { el._value = String(v); el.sets.push(String(v)); },
    get scrollHeight() { return 100; },
    get firstChild() { return el.children[0] || null; },
    get nextSibling() {
      if (!el.parentNode) return null;
      return el.parentNode.children[el.parentNode.children.indexOf(el) + 1] || null;
    },
    set innerHTML(v) { if (v === '') el.children.length = 0; },
    set onclick(fn) { el._onclick = fn; },
    setSelectionRange(a, b) { el.selectionStart = a; el.selectionEnd = b; },
    focus() { doc.activeElement = el; },
    addEventListener(type, fn) { (el.listeners[type] = el.listeners[type] || []).push(fn); },
    dispatchEvent(ev) {
      ev.target = el; ev.defaultPrevented = false;
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
const out = el('out'), input = el('in'), runBtn = el('run'), form = el('cmdform');

const lines = () => out.children.map((c) => ({ text: c.textContent, cls: c.className.replace(/^line ?/, '') }));
const texts = () => lines().map((l) => l.text);
const body = () => lines().filter((l) => l.cls !== 'cmd');
const last = () => texts()[texts().length - 1];

// ---- a network that records, and has no catch-all --------------------------------
const net = {
  calls: [], fs: {}, status: new Map(), failing: new Set(), writes: [],
  unpaired: false, inFlight: 0, maxInFlight: 0,
  count(p) { return net.calls.filter((c) => c.path === p).length; },
  reset() {
    net.calls.length = 0; net.writes.length = 0; net.status.clear(); net.failing.clear();
    net.fs = {}; net.unpaired = false; net.inFlight = 0; net.maxInFlight = 0;
  },
};
function reply(status, text) {
  return {
    ok: status >= 200 && status < 300, status,
    text: async () => text,
    json: async () => JSON.parse(text),
    arrayBuffer: async () => Buffer.from(text),
  };
}
async function fakeFetch(url, opts = {}) {
  const u = new URL(url, 'http://device');
  const p = u.pathname, param = u.searchParams.get('path');
  net.calls.push({ method: opts.method || 'GET', path: p, param, body: opts.body });
  net.inFlight++;
  net.maxInFlight = Math.max(net.maxInFlight, net.inFlight);
  try {
    await null;                                     // one turn, so overlapping calls are visible
    if (net.failing.has(p) || net.failing.has('*')) throw new TypeError('Failed to fetch');
    if (net.unpaired && p.startsWith('/api/')) return reply(401, 'unauthorized');
    if (net.status.has(p)) return reply(net.status.get(p), '');
    if (p === '/api/fs/list') {
      const e = net.fs[param];
      return Array.isArray(e) ? reply(200, JSON.stringify({ entries: e })) : reply(404, 'no dir');
    }
    if (p === '/api/fs/read') {
      const e = net.fs[param];
      return typeof e === 'string' ? reply(200, e) : reply(404, 'no file');
    }
    if (p === '/api/fs/write') {
      net.writes.push({ path: param, body: String(opts.body ?? '') });
      net.fs[param] = String(opts.body ?? '');
      return reply(200, '');
    }
    if (p === '/api/fs/delete') { delete net.fs[param]; return reply(200, ''); }
    if (p === '/api/fs/mkdir') { net.fs[param] = []; return reply(200, ''); }
    if (p === '/api/status') return reply(200, JSON.stringify({ os: 'NucleoOS', version: '0.1.0', uptime_s: 1, free_heap: 1000 }));
    if (p === '/api/logs') return reply(200, 'boot ok');
    if (p === '/api/reboot') return reply(200, '{}');
    throw new Error('unrouted: ' + url);
  } finally { net.inFlight--; }
}

const sandbox = {
  document: {
    documentElement: doc.documentElement,
    get activeElement() { return doc.activeElement; },
    createElement: () => makeEl(null),
    getElementById: (id) => el(id),
  },
  window: { addEventListener() {} },
  parent: { postMessage() {} },
  performance: { now: () => 1000 },
  Blob: class { constructor(parts) { this.size = Buffer.byteLength(parts.join('')); } },
  fetch: fakeFetch,
  setTimeout: (fn) => { fn(); return 0; },
  setInterval: () => 0,
  console,
  I18N: { init: () => ((k, ...rest) => { for (const a of rest) if (typeof a === 'string') return a; return String(k); }) },
};
vm.runInContext(code, vm.createContext(sandbox));
const T = sandbox.__T;
assert.ok(T && T.run && T.parse, '__T must expose the engine');

const DEBOUNCE = T.LIMITS.submitDebounceMs;
T.LIMITS.submitDebounceMs = 0;              // opt back in only where one gesture is under test

const settle = async () => { for (let i = 0; i < 50; i++) await new Promise((r) => setImmediate(r)); };
function reset() {
  T.COMMANDS.clear();                       // resets the scrollback counters too, not just the DOM
  input._value = ''; input.sets.length = 0;
  T.hist.length = 0;
  for (const k of Object.keys(T.envVars)) delete T.envVars[k];
  Object.assign(T.envVars, { SHELL: '/bin/sh', PATH: '/apps/bin', USER: 'admin', OS: 'NucleoOS', TERM: 'xterm-color', CWD: '/' });
  T.setCwd('/');
  net.reset();
  net.fs['/'] = [{ name: 'data', type: 'dir' }];
  net.fs['/data'] = [{ name: 'a.txt', type: 'file', size: 3 }];
}
// The tokeniser is easier to assert as finished words than as parts.
const words = (line) => {
  const p = T.parse(line);
  return p.error ? { error: p.error } : [...p.list[0].stages[0].argv];
};

// ---- SUBMIT ----------------------------------------------------------------------
// Each entry is one real keyboard's "send" gesture, recorded as the events it emits. The app
// must not care which one it gets — and must run the line exactly once for any of them.
const key = (o) => ({ type: 'keydown', key: 'Enter', keyCode: 13, isComposing: false, ...o });
const PROFILES = [
  { id: 'desktop', send: () => input.dispatchEvent(key({})) },
  { id: 'gboard-latin', send: () => input.dispatchEvent(key({ keyCode: 13 })) },
  {
    id: 'gboard-composing', send: () => {                       // never emits key 'Enter' at all
      input.dispatchEvent({ type: 'compositionstart' });
      input.dispatchEvent({ type: 'keydown', key: 'Unidentified', keyCode: 229, isComposing: true });
      input.dispatchEvent({ type: 'compositionend', data: 'ls' });
      input.dispatchEvent({ type: 'beforeinput', inputType: 'insertLineBreak' });
      input.dispatchEvent({ type: 'input', inputType: 'insertLineBreak' });
    },
  },
  {
    id: 'ios-go', send: () => {                                 // software Go key, then the form
      input.dispatchEvent({ type: 'beforeinput', inputType: 'insertLineBreak' });
      input.dispatchEvent({ type: 'input', inputType: 'insertLineBreak' });
      form.dispatchEvent({ type: 'submit', cancelable: true });
    },
  },
  {
    id: 'swiftkey-autocorrect', send: () => {                   // value settles before Enter lands
      input.dispatchEvent({ type: 'keydown', key: 'Unidentified', keyCode: 229, isComposing: true });
      input.dispatchEvent({ type: 'compositionend', data: 'ls' });
      input.dispatchEvent({ type: 'input', inputType: 'insertText' });
      input.dispatchEvent(key({}));
    },
  },
  { id: 'voice', send: () => runBtn.dispatchEvent({ type: 'click' }) },   // dictation: no key events
];

for (const p of PROFILES) {
  test(`submit: "${p.id}" runs the typed command exactly once`, async () => {
    reset();
    T.LIMITS.submitDebounceMs = DEBOUNCE;                       // the real guard is under test
    await new Promise((r) => setTimeout(r, DEBOUNCE + 5));      // …so the previous test's send must age out
    input.value = 'ls /data';
    p.send();
    await settle();
    T.LIMITS.submitDebounceMs = 0;
    assert.equal(lines().filter((l) => l.cls === 'cmd').length, 1, 'one prompt echo');
    assert.equal(texts()[0], '/ $ ls /data');
    assert.equal(net.count('/api/fs/list'), 1, 'one listing, not two');
    assert.equal(input.value, '');
    assert.deepEqual([...T.hist], ['ls /data']);
  });
}

test('submit: nothing fires while the IME is still composing', async () => {
  reset();
  input.value = 'l';
  input.dispatchEvent({ type: 'compositionstart' });
  input.dispatchEvent({ type: 'keydown', key: 'Unidentified', keyCode: 229, isComposing: true });
  input.dispatchEvent({ type: 'beforeinput', inputType: 'insertCompositionText' });
  await settle();
  assert.equal(out.children.length, 0);
  assert.equal(net.calls.length, 0);
  input.dispatchEvent({ type: 'compositionend', data: 'ls' });
  input.value = 'ls';
  input.dispatchEvent({ type: 'beforeinput', inputType: 'insertLineBreak' });
  await settle();
  assert.equal(lines().filter((l) => l.cls === 'cmd').length, 1);
});

test('submit: a keydown Enter carrying keyCode 229 is composition, not a send', async () => {
  reset();
  input.value = 'ls';
  input.dispatchEvent({ type: 'keydown', key: 'Enter', keyCode: 229, isComposing: false });
  await settle();
  assert.equal(out.children.length, 0);
  assert.equal(input.value, 'ls');
});

test('submit: the form submit is always prevented so the page cannot navigate away', async () => {
  reset();
  input.value = 'pwd';
  const ev = { type: 'submit', cancelable: true };
  form.dispatchEvent(ev);
  await settle();
  assert.equal(ev.defaultPrevented, true);
  assert.equal(last(), '/');
});

test('submit: shift+Enter does not send', async () => {
  reset();
  input.value = 'ls';
  input.dispatchEvent(key({ shiftKey: true }));
  await settle();
  assert.equal(out.children.length, 0);
  assert.equal(input.value, 'ls');
});

test('submit: a pasted line keeps its newline out of the command', async () => {
  reset();
  input.value = 'ls /data\n';
  input.dispatchEvent({ type: 'input', inputType: 'insertFromPaste' });
  await settle();
  assert.equal(texts()[0], '/ $ ls /data');
  assert.equal(net.count('/api/fs/list'), 1);
  assert.equal(input.value, '');
});

test('submit: a blank line echoes the prompt, calls nothing and is not recorded', async () => {
  reset();
  input.value = '   ';
  input.dispatchEvent(key({}));
  await settle();
  assert.equal(out.children.length, 1);
  assert.equal(net.calls.length, 0);
  assert.equal(T.hist.length, 0);
});

test('submit: the input is cleared exactly once per gesture', async () => {
  reset();
  input.value = 'pwd';
  input.sets.length = 0;
  input.dispatchEvent(key({}));
  await settle();
  assert.deepEqual(input.sets, ['']);
});

test('submit: two lines sent back to back run one at a time', async () => {
  reset();
  input.value = 'ls /data'; input.dispatchEvent(key({}));
  input.value = 'ls /data'; input.dispatchEvent(key({}));
  await settle();
  assert.equal(net.count('/api/fs/list'), 2);
  assert.equal(net.maxInFlight, 1, 'a queued line must not race the running one');
  assert.equal(lines().filter((l) => l.cls === 'cmd').length, 2);
});

test('history: ArrowUp recalls, ArrowDown past the newest clears', async () => {
  reset();
  input.value = 'pwd'; input.dispatchEvent(key({})); await settle();
  input.value = 'env'; input.dispatchEvent(key({})); await settle();
  const up = { type: 'keydown', key: 'ArrowUp' };
  input.dispatchEvent(up);
  assert.equal(input.value, 'env');
  assert.equal(up.defaultPrevented, true);
  input.dispatchEvent({ type: 'keydown', key: 'ArrowUp' });
  assert.equal(input.value, 'pwd');
  input.dispatchEvent({ type: 'keydown', key: 'ArrowDown' });
  input.dispatchEvent({ type: 'keydown', key: 'ArrowDown' });
  assert.equal(input.value, '');
});

test('keys: Ctrl+L clears the screen, Ctrl+C empties the input', async () => {
  reset();
  input.value = 'pwd'; input.dispatchEvent(key({})); await settle();
  assert.ok(out.children.length > 0);
  input.dispatchEvent({ type: 'keydown', key: 'l', ctrlKey: true });
  assert.equal(out.children.length, 0);
  input.value = 'half typed';
  input.dispatchEvent({ type: 'keydown', key: 'c', ctrlKey: true });
  assert.equal(input.value, '');
});

// ---- GRAMMAR: tokenising ----------------------------------------------------------
test('tokenize: splits on runs of whitespace', () => {
  assert.deepEqual(words('ls   -l  /data'), ['ls', '-l', '/data']);
});

test('tokenize: double quotes keep spaces in one word', () => {
  assert.deepEqual(words('cat "my file.txt"'), ['cat', 'my file.txt']);
});

test('tokenize: single quotes are literal — no expansion', () => {
  reset(); T.envVars.X = 'oops';
  assert.deepEqual(words("echo '$X'"), ['echo', '$X']);
});

test('tokenize: double quotes expand into a single word', () => {
  reset(); T.envVars.X = 'hi';
  assert.deepEqual(words('echo "$X there"'), ['echo', 'hi there']);
});

test('tokenize: ${VAR} expands and an unset name vanishes unquoted, stays empty quoted', () => {
  reset(); T.envVars.X = 'hi';
  assert.deepEqual(words('echo ${X}!'), ['echo', 'hi!']);
  assert.deepEqual(words('echo $NOPE end'), ['echo', 'end']);
  assert.deepEqual(words('echo "$NOPE" end'), ['echo', '', 'end']);
});

test('tokenize: backslash escapes a space, a quote and a dollar', () => {
  assert.deepEqual(words('cat my\\ file'), ['cat', 'my file']);
  assert.deepEqual(words('echo "a \\" b"'), ['echo', 'a " b']);
  assert.deepEqual(words('echo \\$HOME'), ['echo', '$HOME']);
});

test('tokenize: an empty quoted string survives as an empty argument', () => {
  assert.deepEqual(words('echo "" x'), ['echo', '', 'x']);
});

test('tokenize: operators need no spaces and are inert inside quotes', () => {
  const p = T.parse('cat a|grep b>c');
  assert.equal(p.list[0].stages.length, 2);
  assert.deepEqual(JSON.parse(JSON.stringify(p.list[0].stages[1].redirs)), [{ op: '>', path: 'c' }]);
  const q = T.parse('echo "a | b && c > d"');
  assert.equal(q.list.length, 1);
  assert.equal(q.list[0].stages.length, 1);
  assert.equal(q.list[0].stages[0].redirs.length, 0);
});

test('tokenize: an unterminated quote or a trailing backslash is a syntax error', async () => {
  for (const bad of ['cat "unfinished', "cat 'unfinished", 'cat trailing\\']) {
    reset();
    const code = await T.run(bad);
    assert.equal(code, 2, bad);
    assert.match(last(), /syntax error/, bad);
    assert.equal(body().pop().cls, 'err', bad);
    assert.equal(net.calls.length, 0, 'a line that does not parse must not reach the device');
  }
});

// ---- GRAMMAR: expansion happens after the split -----------------------------------
test('expansion: a value with a space stays one argument', async () => {
  reset();
  net.fs['/data/my file.txt'] = 'payload';
  T.envVars.SRC = '/data/my file.txt';
  await T.run('cp "$SRC" /data/b'); await settle();
  const reads = net.calls.filter((c) => c.path === '/api/fs/read');
  assert.equal(reads.length, 1);
  assert.equal(reads[0].param, '/data/my file.txt');
  assert.equal(net.writes[0].path, '/data/b');
  assert.ok(!texts().some((t) => /usage: cp/.test(t)));
});

test('expansion: a value can never introduce an operator', async () => {
  reset();
  T.envVars.X = 'a && rm -rf /';
  await T.run('echo $X'); await settle();
  assert.equal(body()[0].text, 'a && rm -rf /');
  assert.equal(body().length, 1);
  assert.equal(net.count('/api/fs/delete'), 0);
});

// ---- GRAMMAR: pipes ----------------------------------------------------------------
test('pipe: the second stage reads the first stage\'s output, not the file again', async () => {
  reset();
  net.fs['/data/f'] = 'alpha\nbeta x\ngamma';
  await T.run('cat /data/f | grep x'); await settle();
  assert.deepEqual(body().map((l) => l.text), ['beta x']);
  assert.equal(net.count('/api/fs/read'), 1);
});

test('pipe: only the last stage writes to the screen', async () => {
  reset();
  net.fs['/data/f'] = 'one two\nthree';
  await T.run('cat /data/f | wc'); await settle();
  assert.equal(body().length, 1);
  assert.match(body()[0].text, /2 lines, 3 words, 13 bytes$/);
});

test('pipe: three stages still read the file once', async () => {
  reset();
  net.fs['/data/f'] = 'x1\nx2\ny3';
  await T.run('cat /data/f | grep x | wc'); await settle();
  assert.equal(net.count('/api/fs/read'), 1);
  assert.equal(net.maxInFlight, 1);
  assert.match(body()[0].text, /2 lines/);
});

test('pipe: a failing stage aborts the rest and still shows the error', async () => {
  reset();
  const code = await T.run('cat /missing | wc'); await settle();
  assert.notEqual(code, 0);
  assert.equal(body().filter((l) => l.cls === 'err').length, 1);
  assert.match(body()[0].text, /^cat: /);
  assert.ok(!texts().some((t) => /lines,/.test(t)), 'wc must not run');
});

test('pipe: an unknown stage aborts before any device call', async () => {
  reset();
  const code = await T.run('ls | nope | wc'); await settle();
  assert.equal(code, 1);
  assert.match(last(), /nope: command not found/);
  assert.equal(net.calls.length, 0);
});

test('pipe: more stages than the cap is refused, not attempted', async () => {
  reset();
  const line = 'pwd' + ' | cat'.repeat(T.LIMITS.maxStages + 1);
  const code = await T.run(line); await settle();
  assert.equal(code, 2);
  assert.match(last(), /too many stages/);
  assert.equal(net.calls.length, 0);
});

// ---- GRAMMAR: redirection ----------------------------------------------------------
test('redirect: > captures stdout into a file and prints nothing', async () => {
  reset();
  const code = await T.run('ls /data > /data/out.txt'); await settle();
  assert.equal(code, 0);
  assert.equal(body().length, 0, 'redirected output must not reach the screen');
  assert.equal(net.writes.length, 1);
  assert.equal(net.writes[0].path, '/data/out.txt');
  assert.match(net.writes[0].body, /a\.txt/);
});

test('redirect: >> appends with exactly one read and one write', async () => {
  reset();
  net.fs['/data/log'] = 'old';
  await T.run('pwd >> /data/log'); await settle();
  assert.equal(net.count('/api/fs/read'), 1);
  assert.equal(net.count('/api/fs/write'), 1);
  assert.equal(net.writes[0].body, 'old\n/');
});

test('redirect: a quoted target with spaces resolves whole', async () => {
  reset();
  await T.run('pwd > "/data/my out.txt"'); await settle();
  assert.equal(net.writes[0].path, '/data/my out.txt');
});

test('redirect: a failed write is reported and the line fails', async () => {
  reset();
  net.status.set('/api/fs/write', 500);
  const code = await T.run('pwd > /data/out.txt'); await settle();
  assert.notEqual(code, 0);
  assert.match(last(), /failed to write/);
  assert.equal(body().pop().cls, 'err');
});

test('redirect: > binds to the stage it follows, and < feeds the first one', async () => {
  reset();
  net.fs['/data/f'] = 'keep x\ndrop';
  await T.run('cat /data/f | grep x > /data/o.txt'); await settle();
  assert.equal(body().length, 0);
  assert.equal(net.writes[0].path, '/data/o.txt');
  assert.equal(net.writes[0].body, 'keep x');

  reset();
  net.fs['/data/f'] = 'one\ntwo\nthree';
  await T.run('grep two < /data/f'); await settle();
  assert.deepEqual(body().map((l) => l.text), ['two']);
  assert.equal(net.count('/api/fs/read'), 1);
});

test('redirect: a missing target is a syntax error that reaches no device', async () => {
  reset();
  const code = await T.run('pwd >'); await settle();
  assert.equal(code, 2);
  assert.match(last(), /syntax error/);
  assert.equal(net.calls.length, 0);
});

// ---- GRAMMAR: chaining --------------------------------------------------------------
test('chain: ";" runs both regardless of the first result', async () => {
  reset();
  await T.run('nope ; pwd'); await settle();
  assert.match(body()[0].text, /command not found/);
  assert.equal(body()[1].text, '/');
});

test('chain: "&&" short-circuits after a failure, spending no further request', async () => {
  reset();
  await T.run('ls /missing && ls /data'); await settle();
  assert.equal(net.count('/api/fs/list'), 1);
});

test('chain: "&&" continues after success and "||" is its mirror', async () => {
  reset();
  await T.run('pwd && pwd'); await settle();
  assert.deepEqual(body().map((l) => l.text), ['/', '/']);

  reset();
  await T.run('ls /missing || pwd'); await settle();
  assert.equal(last(), '/');

  reset();
  await T.run('pwd || ls /data'); await settle();
  assert.equal(net.count('/api/fs/list'), 0);
});

test('chain: the operators are left-associative', async () => {
  reset();
  await T.run('nope && pwd || echo fallback'); await settle();
  assert.equal(last(), 'fallback');
  assert.ok(!body().some((l) => l.text === '/'));
});

test('chain: an unreachable device counts as a failure', async () => {
  reset();
  net.unpaired = true;
  await T.run('ls && pwd'); await settle();
  assert.ok(body().some((l) => l.cls === 'err' && /401/.test(l.text)));
  assert.ok(!body().some((l) => l.text === '/'));
});

test('chain: the prompt is echoed once per submitted line, not once per stage', async () => {
  reset();
  net.fs['/data/f'] = 'x';
  await T.run('pwd && cat /data/f | wc'); await settle();
  assert.equal(lines().filter((l) => l.cls === 'cmd').length, 1);
});

test('chain: exit codes are 0 on success and non-zero on failure', async () => {
  reset();
  assert.equal(await T.run('pwd'), 0);
  assert.equal(await T.run('echo x'), 0);
  reset();
  assert.notEqual(await T.run('ls /missing'), 0);
  assert.notEqual(await T.run('cat /missing'), 0);
  assert.notEqual(await T.run('grep'), 0);
});

// ---- BOUNDS ---------------------------------------------------------------------------
test('cap: the scrollback keeps at most LIMITS.maxLines and marks the trim', async () => {
  reset();
  for (let i = 0; i < T.LIMITS.maxLines + 50; i++) await T.run('echo line' + i);
  const kept = out.children.length;
  assert.ok(kept <= T.LIMITS.maxLines + 1, `kept ${kept}`);
  assert.match(out.firstChild.textContent, /earlier output trimmed/);
});

test('cap: an oversized payload is truncated on screen and says so', async () => {
  reset();
  net.fs['/data/big'] = 'z'.repeat(T.LIMITS.maxBytes + 5000);
  await T.run('cat /data/big'); await settle();
  const shown = body()[0];
  assert.equal(shown.text.length, T.LIMITS.maxBytes);
  assert.equal(body()[1].cls, 'warn');
  assert.match(body()[1].text, /truncated/);
});

test('cap: ls stops after LIMITS.maxRows entries and says how many there were', async () => {
  reset();
  net.fs['/many'] = Array.from({ length: T.LIMITS.maxRows + 10 }, (_, i) => ({ name: 'f' + i, type: 'file', size: 1 }));
  await T.run('ls /many'); await settle();
  const rows = body().filter((l) => /^ {2}f\d+/.test(l.text));
  assert.equal(rows.length, T.LIMITS.maxRows);
  assert.ok(body().some((l) => l.cls === 'warn' && /showing first/.test(l.text)));
});

test('cap: find never exceeds LIMITS.maxListCalls and reports the stop honestly', async () => {
  reset();
  let dir = '/';                                     // 12 wide and 12 deep: unbounded is fatal here
  for (let d = 0; d < 12; d++) {
    net.fs[dir] = Array.from({ length: 12 }, (_, k) => ({ name: 'd' + k, type: 'dir' }));
    dir = (dir === '/' ? '' : dir) + '/d0';
  }
  await T.run('find zzz'); await settle();
  assert.ok(net.count('/api/fs/list') <= T.LIMITS.maxListCalls, net.count('/api/fs/list') + ' listings');
  assert.ok(body().some((l) => l.cls === 'warn' && /search stopped/.test(l.text)));
  assert.equal(net.maxInFlight, 1);
});

test('bounds: no command opens two device requests at once', async () => {
  reset();
  net.fs['/data/f'] = 'one\ntwo';
  for (const line of ['ls /data', 'cat /data/f', 'find a', 'cp /data/f /data/g', 'mv /data/g /data/h',
    'wc /data/f', 'head /data/f', 'tail /data/f', 'grep one /data/f', 'df', 'uptime', 'status', 'free',
    'dmesg', 'cat /data/f | grep one | wc']) {
    net.maxInFlight = 0;
    await T.run(line); await settle();
    assert.equal(net.maxInFlight, 1, line);
  }
});

// The acceptance bar for the new grammar: every command that worked before still resolves and
// still runs, because a tokeniser that quietly drops a command is the worst kind of regression.
test('compat: every command still resolves and runs through the parser', async () => {
  const invocations = [
    'help', 'ls /data', 'cd /data', 'pwd', 'cat /data/f', 'cp /data/f /data/g', 'mv /data/g /data/h',
    'mkdir /data/d', 'touch /data/t', 'rm /data/t', 'echo hello world', 'open /data/f',
    'grep one /data/f', 'find f', 'wc /data/f', 'head -n 2 /data/f', 'tail -n 2 /data/f', 'df',
    'uptime', 'dmesg', 'reboot', 'ping', 'curl example.com', 'wget example.com -O /data/w',
    'sleep 0', 'env', 'export A=b', 'man ls', 'history', 'uname -a', 'status', 'theme dark',
    'clear', 'date', 'free',
    // added by the command slice — the device-native half included
    'stat /data/f', 'du -d 1 /data', 'tree -L 1 /data', 'hexdump -n 16 /data/f', 'base64 /data/f',
    'tee /data/t2', 'seq 3', 'sort', 'uniq -c', 'cut -d, -f1',
    'alias ll=ls', 'unalias ll', 'which ls', 'watch -c 1 -n 2 pwd',
    'diag', 'heap', 'cpu', 'apps', 'assoc txt', 'lang', 'ota', 'anima --caps', 'verify a b',
    'wifi known', 'ir db', 'display on', 'screen', 'say hello', 'notify hello', 'clip -o',
    'link peers',
  ];
  assert.equal(new Set(invocations.map((i) => i.split(' ')[0])).size, Object.keys(T.COMMANDS).length);
  for (const line of invocations) {
    reset();
    net.fs['/data/f'] = 'one\ntwo';
    await T.run(line); await settle();
    assert.ok(!texts().some((t) => /command not found/.test(t)), line);
    assert.ok(!body().some((l) => /^[a-z]+: undefined/.test(l.text)), line + ' → ' + last());
  }
});

test('compat: quoted arguments survive the trip back to the string-parsing commands', async () => {
  reset();
  net.fs['/data/two words.txt'] = 'body';
  await T.run('cp "/data/two words.txt" "/data/copy of it.txt"'); await settle();
  assert.equal(net.writes[0].path, '/data/copy of it.txt');

  reset();
  T.envVars.P = 'needle here';
  net.fs['/data/f'] = 'no\nneedle here\nno';
  await T.run('grep "$P" /data/f'); await settle();
  assert.match(body()[0].text, /2: needle here/);

  reset();
  await T.run('export GREETING="good day"'); await settle();
  assert.equal(T.envVars.GREETING, 'good day');
  assert.equal(last(), 'GREETING="good day"');
});

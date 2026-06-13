// nucleo-run.js — NucleoOS shared in-browser JavaScript runtime (single source of truth).
//
// User code runs in a Web Worker (OFF the UI thread), with NO DOM and NO ambient authority:
// fetch/XMLHttpRequest/WebSocket/importScripts are stripped from the worker scope, so the ONLY
// way out is an injected `os` object whose every method is brokered back here over postMessage
// and gated by a capability set. The host enforces a hard wall-clock timeout and can terminate a
// runaway script. The worker is a same-origin Blob, so any app that can import this module gets
// the identical engine — the File Commander "Run", the Code Runner app, and ANIMA's chat/skill
// all share it. No build step: the worker body is this module's own function, stringified.
//
//   import { createRunner, runOnce } from '/apps/code-runner/nucleo-run.js';
//   const rt = createRunner({ cwd:'/data', onLog:(lvl,txt)=>… });
//   const r = await rt.run(code);   // {ok, hasValue, value, error, stack, ms, timeout?, stopped?}
//   rt.stop();  rt.dispose();
//
// The script API (what user code sees):
//   console.log/info/warn/error/debug   print(...)            — captured to onLog
//   await os.fs.read(path) -> string     await os.fs.write(path, str) -> true
//   await os.fs.append(path, str)        await os.fs.list(dir) -> [{name,type,size}]
//   await os.fs.exists(path) -> bool      await os.fs.mkdir(dir)   await os.fs.remove(path)
//   await os.http.get(url) -> {status,ok,body}      await os.http.json(url) -> {status,ok,json}
//   await os.anima(question) -> {reply,tier,intent} await os.notify(text)
//   await os.sleep(ms)                    os.env -> {cwd, lang, args}     args, env (also bound)
// Paths are resolved against caps.cwd when relative.

// ---- the worker program (runs INSIDE the Worker; self-contained, stringified below) ----
function workerMain() {
  var pending = {}, rpcId = 0, started = false;
  function rpc(method, args) {
    return new Promise(function (resolve, reject) {
      var id = ++rpcId; pending[id] = { resolve: resolve, reject: reject };
      self.postMessage({ type: 'rpc', id: id, method: method, args: args });
    });
  }
  function replacer() {
    var seen = new WeakSet();
    return function (k, val) {
      if (typeof val === 'bigint') return val.toString() + 'n';
      if (typeof val === 'function') return '[Function ' + (val.name || 'anonymous') + ']';
      if (typeof val === 'object' && val) { if (seen.has(val)) return '[Circular]'; seen.add(val); }
      return val;
    };
  }
  function fmt(v) {
    if (typeof v === 'string') return v;
    if (v === undefined) return 'undefined';
    if (typeof v === 'function') return '[Function ' + (v.name || 'anonymous') + ']';
    try { return JSON.stringify(v, replacer(), 2); } catch (e) { return String(v); }
  }
  function emit(level, list) { self.postMessage({ type: 'log', level: level, text: list.map(fmt).join(' ') }); }
  var con = {
    log: function () { emit('log', [].slice.call(arguments)); },
    info: function () { emit('info', [].slice.call(arguments)); },
    warn: function () { emit('warn', [].slice.call(arguments)); },
    error: function () { emit('error', [].slice.call(arguments)); },
    debug: function () { emit('debug', [].slice.call(arguments)); },
    table: function (v) { emit('log', [v]); },
    dir: function (v) { emit('log', [v]); },
    clear: function () { self.postMessage({ type: 'clear' }); },
  };
  var os = {
    fs: {
      read:   function (p, o) { return rpc('fs.read', [p, o]); },
      write:  function (p, c, o) { return rpc('fs.write', [p, c, o]); },
      append: function (p, c) { return rpc('fs.append', [p, c]); },
      list:   function (p) { return rpc('fs.list', [p]); },
      exists: function (p) { return rpc('fs.exists', [p]); },
      mkdir:  function (p) { return rpc('fs.mkdir', [p]); },
      remove: function (p) { return rpc('fs.remove', [p]); },
    },
    http: {
      get:  function (u, o) { return rpc('http.get', [u, o]); },
      json: function (u, o) { return rpc('http.json', [u, o]); },
    },
    anima:  function (q) { return rpc('anima', [q]); },
    notify: function (t) { return rpc('notify', [t]); },
    sleep:  function (ms) { return new Promise(function (r) { setTimeout(r, Math.max(0, ms | 0)); }); },
    env: {},
  };
  // Revoke ambient I/O — the capability bridge is the only sanctioned authority.
  try {
    self.fetch = undefined; self.XMLHttpRequest = undefined; self.WebSocket = undefined;
    self.indexedDB = undefined; self.caches = undefined;
    self.importScripts = function () { throw new Error('importScripts is disabled in the NucleoOS sandbox'); };
  } catch (e) {}

  // Friendly DOM trap: code that reaches for the browser (document/window/canvas/alert) gets an
  // actionable message pointing at the sandbox API, instead of a bare "X is not defined".
  function denyDom(name) {
    var msg = name + ' is not available in the NucleoOS sandbox (Web Worker, no DOM). '
      + 'Print with console.log/print and use os.* (os.fs, os.http, os.anima, os.sleep). '
      + 'For animation, redraw text with console.clear() between frames.';
    var trap = function () { throw new Error(msg); };
    return new Proxy(trap, { get: trap, set: trap, apply: trap, construct: trap });
  }
  try {
    ['document', 'window', 'alert', 'prompt', 'confirm', 'requestAnimationFrame',
     'localStorage', 'sessionStorage', 'navigator'].forEach(function (g) {
      try { self[g] = denyDom(g); } catch (e) {}
    });
  } catch (e) {}

  self.onunhandledrejection = function (e) {
    try { emit('error', ['Unhandled rejection:', (e.reason && e.reason.message) || e.reason]); } catch (x) {}
  };
  self.onmessage = function (e) {
    var d = e.data || {};
    if (d.type === 'rpc-result') {
      var pr = pending[d.id]; if (!pr) return; delete pending[d.id];
      if (d.ok) pr.resolve(d.value); else pr.reject(new Error(d.error || 'os call failed'));
      return;
    }
    if (d.type !== 'run' || started) return;
    started = true;
    os.env = d.env || {};
    var args = (d.env && d.env.args) || [];
    var t0 = (self.performance && performance.now) ? performance.now() : 0;
    Promise.resolve().then(function () {
      var AsyncFn = Object.getPrototypeOf(async function () {}).constructor;
      var fn = new AsyncFn('os', 'console', 'print', 'args', 'env',
        '"use strict";\n' + String(d.code || '') + '\n//# sourceURL=nucleo-script.js');
      return fn(os, con, function () { emit('log', [].slice.call(arguments)); }, args, os.env);
    }).then(function (val) {
      var ms = ((self.performance && performance.now) ? performance.now() : 0) - t0;
      self.postMessage({ type: 'done', hasValue: val !== undefined, valueText: val !== undefined ? fmt(val) : '', ms: ms });
    }).catch(function (err) {
      var ms = ((self.performance && performance.now) ? performance.now() : 0) - t0;
      self.postMessage({ type: 'error', message: String((err && err.message) || err), stack: String((err && err.stack) || ''), ms: ms });
    });
  };
  self.postMessage({ type: 'ready' });
}

const RUN_SRC = '(' + workerMain.toString() + ')();';

// Parse-only syntax check (NO execution) — host-safe (works in Node, no Worker/DOM). Used by the
// ANIMA agent loop's VERIFY gate (mode:'check') so a candidate is validated before it can be
// applied/run. Compiling an AsyncFunction parses the body without invoking it.
export function checkSyntax(code) {
  try {
    const AsyncFn = Object.getPrototypeOf(async function () {}).constructor;
    // eslint-disable-next-line no-new
    new AsyncFn('os', 'console', 'print', 'args', 'env', '"use strict";\n' + String(code || ''));
    return { ok: true };
  } catch (e) {
    const msg = String((e && e.message) || e);
    const lc = /<anonymous>:(\d+):(\d+)/.exec((e && e.stack) || '') || /(\d+):(\d+)/.exec((e && e.stack) || '');
    return { ok: false, error: msg, line: lc ? +lc[1] : undefined, col: lc ? +lc[2] : undefined };
  }
}

// ---- path helpers (host side) ----
function normPath(p) {
  const out = [];
  for (const seg of String(p || '').split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') out.pop(); else out.push(seg);
  }
  return '/' + out.join('/');
}

// ---- the host-side runner ----
export function createRunner(opts) {
  const caps = Object.assign({
    fs: true, http: true, anima: true, notify: true,
    cwd: '/', lang: 'it', timeoutMs: 5000,
    maxLogBytes: 256 * 1024,                 // output cap: drop logs past this, with one notice
    onLog: null, onNotify: null,
  }, opts || {});

  let worker = null, blobUrl = null, current = null, logBytes = 0, logCapped = false;

  const resolve = (p) => {
    p = String(p == null ? '' : p);
    if (!p) return normPath(caps.cwd);
    return normPath(p[0] === '/' ? p : (caps.cwd + '/' + p));
  };
  const fsApi = (op, path, init) => fetch('/api/fs/' + op + '?path=' + encodeURIComponent(path), init);

  async function handleRpc(method, args) {
    args = args || [];
    if (method.indexOf('fs.') === 0 && !caps.fs) throw new Error('fs capability denied');
    switch (method) {
      case 'fs.read': {
        const r = await fsApi('read', resolve(args[0]));
        if (!r.ok) throw new Error('read failed (' + r.status + ') ' + resolve(args[0]));
        return await r.text();
      }
      case 'fs.write': {
        const r = await fsApi('write', resolve(args[0]), { method: 'POST', body: String(args[1] == null ? '' : args[1]) });
        if (!r.ok) throw new Error('write failed (' + r.status + ')');
        return true;
      }
      case 'fs.append': {
        const rr = await fsApi('read', resolve(args[0]));
        const cur = rr.ok ? await rr.text() : '';
        const r = await fsApi('write', resolve(args[0]), { method: 'POST', body: cur + String(args[1] == null ? '' : args[1]) });
        if (!r.ok) throw new Error('append failed (' + r.status + ')');
        return true;
      }
      case 'fs.list': {
        const r = await fsApi('list', resolve(args[0] || '.'));
        if (!r.ok) throw new Error('list failed (' + r.status + ')');
        const j = await r.json();
        return j.entries || [];
      }
      case 'fs.exists': { const r = await fsApi('read', resolve(args[0])); return r.ok; }
      case 'fs.mkdir':  { const r = await fsApi('mkdir', resolve(args[0]), { method: 'POST' }); return r.ok; }
      case 'fs.remove': { const r = await fsApi('delete', resolve(args[0]), { method: 'POST' }); return r.ok; }
      case 'http.get': {
        if (!caps.http) throw new Error('http capability denied');
        const r = await fetch(args[0], args[1] || {});
        return { status: r.status, ok: r.ok, body: await r.text() };
      }
      case 'http.json': {
        if (!caps.http) throw new Error('http capability denied');
        const r = await fetch(args[0], args[1] || {});
        return { status: r.status, ok: r.ok, json: await r.json().catch(() => null) };
      }
      case 'anima': {
        if (!caps.anima) throw new Error('anima capability denied');
        const r = await fetch('/api/anima?q=' + encodeURIComponent(args[0] || '') + '&lang=' + (caps.lang || 'it') + '&mode=on');
        const j = await r.json();
        return { reply: j.reply || '', tier: j.tier, intent: j.intent };
      }
      case 'notify': { if (caps.onNotify) caps.onNotify(String(args[0] == null ? '' : args[0])); return true; }
      default: throw new Error('unknown os method: ' + method);
    }
  }

  function onMessage(e) {
    const d = e.data || {};
    if (d.type === 'log') {
      if (logCapped) return;
      logBytes += (d.text ? d.text.length : 0);
      if (logBytes > caps.maxLogBytes) { logCapped = true; caps.onLog && caps.onLog('warn', '… output truncated (exceeded ' + caps.maxLogBytes + ' bytes)'); return; }
      caps.onLog && caps.onLog(d.level, d.text); return;
    }
    if (d.type === 'clear') { logBytes = 0; logCapped = false; caps.onLog && caps.onLog('clear', ''); return; }
    if (d.type === 'rpc') {
      handleRpc(d.method, d.args)
        .then((v) => worker && worker.postMessage({ type: 'rpc-result', id: d.id, ok: true, value: v }))
        .catch((err) => worker && worker.postMessage({ type: 'rpc-result', id: d.id, ok: false, error: String((err && err.message) || err) }));
      return;
    }
    if (d.type === 'done')  { settle({ ok: true,  hasValue: d.hasValue, value: d.valueText, ms: d.ms }); return; }
    if (d.type === 'error') { settle({ ok: false, error: d.message, stack: d.stack, ms: d.ms }); return; }
  }

  function ensure() {
    if (worker) return;
    blobUrl = URL.createObjectURL(new Blob([RUN_SRC], { type: 'text/javascript' }));
    worker = new Worker(blobUrl);
    worker.onmessage = onMessage;
    worker.onerror = (ev) => settle({ ok: false, error: (ev && ev.message) || 'worker crashed' });
  }

  // Settle the in-flight run (if any) and ALWAYS retire the worker: each run() gets a fresh worker
  // (clean slate — no globals, timers or listeners leak from one run into the next). ensure()
  // lazily spins a new one on the next run. The worker's run handler is single-shot by design.
  function settle(result) {
    if (!current) return;
    const c = current; current = null;
    clearTimeout(c.timer);
    teardown();
    c.resolve(result);
  }
  function teardown() {
    if (worker) { try { worker.terminate(); } catch (e) {} worker = null; }
    if (blobUrl) { URL.revokeObjectURL(blobUrl); blobUrl = null; }
  }

  function run(code, env, opts = {}) {
    // Parse-only verification gate — no Worker, no execution (host-safe; the loop's VERIFY step).
    if (opts && opts.mode === 'check') return Promise.resolve(checkSyntax(code));
    if (current) return Promise.reject(new Error('a script is already running'));
    ensure();
    logBytes = 0; logCapped = false;
    return new Promise((res) => {
      const timer = setTimeout(() => settle({ ok: false, error: 'timeout after ' + caps.timeoutMs + ' ms — script terminated', timeout: true, ms: caps.timeoutMs }), caps.timeoutMs);
      current = { resolve: res, timer };
      worker.postMessage({ type: 'run', code: String(code || ''), env: Object.assign({ cwd: caps.cwd, lang: caps.lang, args: (env && env.args) || [] }, env) });
    });
  }
  function stop() { settle({ ok: false, error: 'stopped', stopped: true }); teardown(); }
  function dispose() { current = null; teardown(); }
  function setCwd(p) { caps.cwd = p || '/'; }

  return { run, stop, dispose, setCwd, caps };
}

// One-shot convenience: create, run, dispose. Returns the same result object as run().
export async function runOnce(code, opts) {
  const rt = createRunner(opts);
  try { return await rt.run(code, opts && opts.env); }
  finally { rt.dispose(); }
}

export default createRunner;

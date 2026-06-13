// fsclient.js — workspace-scoped file tools for ANIMA.
// A DOM-free layer over /api/fs/{list,read,write,delete,mkdir,move} that confines every
// operation to a chosen workspace root on the SD, the way Claude Code confines its tools to
// a working directory. Returns plain {ok,...} results the agent loop can render; it never
// throws for an expected filesystem condition (missing file, escape, http error).
//
// Paths are SD paths under the mount (e.g. /data/Documents/proj/notes.txt). A relative path
// joins the root; an absolute path must already sit inside the root or it is refused.

// Text-ish files we are willing to read for search / preview (everything else is opaque blob).
const TEXT_EXT = new Set(['txt','md','markdown','json','jsonl','ndjson','csv','tsv','log','c','h','cpp','hpp','cc','cxx',
  'js','mjs','cjs','ts','tsx','jsx','py','sh','bash','zsh','lua','html','htm','css','scss','xml','yml','yaml','toml','ini',
  'cfg','conf','env','sql','rs','go','java','kt','rb','php','pl','r','swift','vue','svelte','tex','rtf','srt','vtt','diff','patch']);
const TEXT_NAME = /^(makefile|dockerfile|readme|license|licence|changelog|authors|notice|\.[a-z0-9_-]+)$/i;

export function isTextName(name) {
  const dot = name.lastIndexOf('.');
  if (dot <= 0) return TEXT_NAME.test(name);
  return TEXT_EXT.has(name.slice(dot + 1).toLowerCase());
}

// Normalise an SD path: collapse //, resolve . and .. segments, drop the trailing slash.
export function normPath(p) {
  const segs = String(p == null ? '' : p).split('/');
  const out = [];
  for (const s of segs) {
    if (s === '' || s === '.') continue;
    if (s === '..') { out.pop(); continue; }
    out.push(s);
  }
  return '/' + out.join('/');
}

export function byteLen(s) { return new TextEncoder().encode(String(s == null ? '' : s)).length; }

// ** spans path separators, * stays within a segment, ? is one char. Anchored to a rel path.
export function globToRegExp(glob) {
  let re = '';
  const g = String(glob).replace(/^\.?\//, '');
  for (let i = 0; i < g.length; i++) {
    const c = g[i];
    if (c === '*') { if (g[i + 1] === '*') { re += '.*'; i++; if (g[i + 1] === '/') i++; } else re += '[^/]*'; }
    else if (c === '?') re += '[^/]';
    else if ('.+^${}()|[]\\'.includes(c)) re += '\\' + c;
    else re += c;
  }
  return new RegExp('(^|/)' + re + '$', 'i');
}

// Cheap multiset line diff -> {added, removed} for a card badge.
export function diffStat(oldText, newText) {
  const a = String(oldText).split('\n'), b = String(newText).split('\n');
  const am = new Map(); for (const l of a) am.set(l, (am.get(l) || 0) + 1);
  const bm = new Map(); for (const l of b) bm.set(l, (bm.get(l) || 0) + 1);
  let added = 0, removed = 0;
  for (const [l, n] of bm) added += Math.max(0, n - (am.get(l) || 0));
  for (const [l, n] of am) removed += Math.max(0, n - (bm.get(l) || 0));
  return { added, removed };
}

// Compact line-level diff (LCS) for a preview, with collapsed unchanged runs. Bounded so a
// big file degrades to a one-line summary instead of an O(n*m) stall.
export function diffLines(oldText, newText, { maxLines = 800, context = 2 } = {}) {
  const a = String(oldText).split('\n'), b = String(newText).split('\n');
  if (a.length > maxLines || b.length > maxLines) return [{ sign: '~', text: `${a.length} → ${b.length} lines` }];
  const n = a.length, m = b.length;
  const dp = Array.from({ length: n + 1 }, () => new Int32Array(m + 1));
  for (let i = n - 1; i >= 0; i--) for (let j = m - 1; j >= 0; j--)
    dp[i][j] = a[i] === b[j] ? dp[i + 1][j + 1] + 1 : Math.max(dp[i + 1][j], dp[i][j + 1]);
  const ops = []; let i = 0, j = 0;
  while (i < n && j < m) {
    if (a[i] === b[j]) { ops.push({ sign: ' ', text: a[i] }); i++; j++; }
    else if (dp[i + 1][j] >= dp[i][j + 1]) { ops.push({ sign: '-', text: a[i] }); i++; }
    else { ops.push({ sign: '+', text: b[j] }); j++; }
  }
  while (i < n) ops.push({ sign: '-', text: a[i++] });
  while (j < m) ops.push({ sign: '+', text: b[j++] });
  const keep = new Array(ops.length).fill(false);
  for (let k = 0; k < ops.length; k++) if (ops[k].sign !== ' ') for (let c = -context; c <= context; c++) if (k + c >= 0 && k + c < ops.length) keep[k + c] = true;
  const out = []; let gap = false;
  for (let k = 0; k < ops.length; k++) {
    if (keep[k]) { out.push(ops[k]); gap = false; }
    else if (!gap) { out.push({ sign: '@', text: '…' }); gap = true; }
  }
  return out;
}

export function makeFS(initialRoot) {
  let root = initialRoot ? normPath(initialRoot) : '';   // '' = no workspace open
  let cwd = '';                                          // current dir, relative to root (Claude-Code cwd)
  const enc = encodeURIComponent;

  const baseDir = () => (cwd ? normPath(root + '/' + cwd) : root);
  // Resolve a user/agent path to an SD-absolute path confined to the workspace.
  // Relative paths join the current dir (root + cwd); absolute paths must sit inside root.
  function resolve(p) {
    if (!root) throw new RangeError('no-workspace');
    const raw = String(p == null ? '' : p).trim();
    const base = baseDir();
    const joined = raw === '' ? base : (raw[0] === '/' ? raw : base + '/' + raw);
    const full = normPath(joined);
    if (full !== root && !full.startsWith(root + '/')) throw new RangeError('escape');
    return full;
  }
  const rel = (full) => {
    const f = normPath(full);
    if (f === root) return '.';
    return f.startsWith(root + '/') ? f.slice(root.length + 1) : f;
  };
  const parentOf = (full) => full.slice(0, full.lastIndexOf('/')) || '/';
  const baseOf = (full) => full.slice(full.lastIndexOf('/') + 1);

  async function api(op, { method = 'GET', path, qs = '', body } = {}) {
    let url = '/api/fs/' + op + '?';
    if (path != null) url += 'path=' + enc(path);
    if (qs) url += (path != null ? '&' : '') + qs;
    return fetch(url, { method, body, cache: 'no-store' });
  }

  // stat via the parent listing — works for files AND directories, and (unlike a 1-byte
  // range read) reports a 0-byte file as existing.
  async function stat(p) {
    let full; try { full = resolve(p); } catch { return { exists: false }; }
    if (full === root) return { exists: true, type: 'dir', size: 0, path: '.', abs: full };
    try {
      const r = await api('list', { path: parentOf(full) });
      if (!r.ok) return { exists: false };
      const j = await r.json();
      const e = (j.entries || []).find((x) => x.name === baseOf(full));
      return e ? { exists: true, type: e.type, size: e.size || 0, path: rel(full), abs: full } : { exists: false };
    } catch { return { exists: false }; }
  }
  const exists = async (p) => (await stat(p)).exists;

  // mkdir -p: create each segment below the root in order (idempotent; ignore "exists").
  async function mkdirp(dirPath) {
    const full = typeof dirPath === 'string' && dirPath[0] === '/' && dirPath.startsWith(root) ? normPath(dirPath) : resolve(dirPath);
    if (full === root || full.length <= root.length) return { ok: true };
    const tail = full.slice(root.length + 1).split('/');
    let cur = root;
    for (const seg of tail) { cur += '/' + seg; try { await api('mkdir', { method: 'POST', path: cur }); } catch {} }
    return { ok: true };
  }

  async function list(dir = '.') {
    let full; try { full = resolve(dir); } catch (e) { return { ok: false, error: e.message }; }
    try {
      const r = await api('list', { path: full });
      if (!r.ok) return { ok: false, error: r.status === 404 ? 'not-found' : 'http-' + r.status, path: rel(full) };
      const j = await r.json();
      const entries = (j.entries || []).filter((e) => e.name !== '.' && e.name !== '..')
        .sort((a, b) => (a.type === b.type ? a.name.localeCompare(b.name) : a.type === 'dir' ? -1 : 1));
      return { ok: true, path: rel(full), abs: full, entries };
    } catch (e) { return { ok: false, error: String(e.message || e) }; }
  }

  async function read(p, { maxBytes = 256 * 1024 } = {}) {
    let full; try { full = resolve(p); } catch (e) { return { ok: false, error: e.message }; }
    try {
      const r = await fetch('/api/fs/read?path=' + enc(full), { cache: 'no-store' });
      if (!r.ok) return { ok: false, error: r.status === 404 ? 'not-found' : 'http-' + r.status, path: rel(full) };
      const buf = await r.arrayBuffer();
      const bytes = buf.byteLength;
      const slice = bytes > maxBytes ? buf.slice(0, maxBytes) : buf;
      const content = new TextDecoder('utf-8', { fatal: false }).decode(slice);
      return { ok: true, path: rel(full), abs: full, content, bytes, truncated: bytes > maxBytes,
        lines: content ? content.split('\n').length : 0 };
    } catch (e) { return { ok: false, error: String(e.message || e) }; }
  }

  async function write(p, content = '', { overwrite = true, mkdir = true } = {}) {
    let full; try { full = resolve(p); } catch (e) { return { ok: false, error: e.message }; }
    const had = await exists(full);
    if (had && !overwrite) return { ok: false, error: 'exists', path: rel(full), abs: full };
    if (mkdir) { const d = parentOf(full); if (d && d !== root) await mkdirp(d); }
    try {
      const r = await api('write', { method: 'POST', path: full, body: content });
      if (!r.ok) return { ok: false, error: 'http-' + r.status, path: rel(full) };
      return { ok: true, path: rel(full), abs: full, bytes: byteLen(content), created: !had };
    } catch (e) { return { ok: false, error: String(e.message || e) }; }
  }

  async function append(p, content = '') {
    const cur = await read(p, {});
    const base = cur.ok ? cur.content : '';
    const joiner = base && !base.endsWith('\n') ? '\n' : '';
    const w = await write(p, base + joiner + content, { overwrite: true });
    if (w.ok) { w.appended = byteLen(content); w.created = !cur.ok; }
    return w;
  }

  // Exact-string replace, Claude-Code style: must be present, and unique unless all=true.
  async function edit(p, oldStr, newStr, { all = false } = {}) {
    if (!oldStr) return { ok: false, error: 'empty-old' };
    const cur = await read(p, {});
    if (!cur.ok) return { ok: false, error: cur.error, path: cur.path };
    const text = cur.content;
    const count = text.split(oldStr).length - 1;
    if (count === 0) return { ok: false, error: 'not-found', path: cur.path };
    if (count > 1 && !all) return { ok: false, error: 'not-unique', count, path: cur.path };
    const next = all ? text.split(oldStr).join(newStr) : text.replace(oldStr, newStr);
    const w = await write(p, next, { overwrite: true });
    if (!w.ok) return w;
    return { ok: true, path: cur.path, abs: cur.abs, replaced: all ? count : 1, ...diffStat(text, next), before: text, after: next };
  }

  async function del(p) {
    let full; try { full = resolve(p); } catch (e) { return { ok: false, error: e.message }; }
    try {
      const r = await api('delete', { method: 'POST', path: full });
      if (!r.ok) return { ok: false, error: r.status === 404 ? 'not-found' : 'http-' + r.status, path: rel(full) };
      return { ok: true, path: rel(full), abs: full };
    } catch (e) { return { ok: false, error: String(e.message || e) }; }
  }

  async function move(from, to, { overwrite = false } = {}) {
    let f, t; try { f = resolve(from); t = resolve(to); } catch (e) { return { ok: false, error: e.message }; }
    const d = parentOf(t); if (d && d !== root) await mkdirp(d);
    try {
      const r = await api('move', { method: 'POST', qs: 'from=' + enc(f) + '&to=' + enc(t) + (overwrite ? '&overwrite=1' : '') });
      if (!r.ok) { const txt = await r.text().catch(() => ''); return { ok: false, error: /exist/i.test(txt) ? 'dest-exists' : 'http-' + r.status, from: rel(f), to: rel(t) }; }
      return { ok: true, from: rel(f), to: rel(t), absTo: t };
    } catch (e) { return { ok: false, error: String(e.message || e) }; }
  }

  async function mkdir(p) {
    try { const full = resolve(p); await mkdirp(full); return { ok: true, path: rel(full), abs: full }; }
    catch (e) { return { ok: false, error: e.message }; }
  }

  // Breadth-first recursive listing to a depth, bounded by maxEntries.
  async function tree(dir = '.', { depth = 3, maxEntries = 400, dirsOnly = false } = {}) {
    let start; try { start = resolve(dir); } catch (e) { return { ok: false, error: e.message }; }
    const out = [], queue = [{ abs: start, d: 0 }];
    while (queue.length && out.length < maxEntries) {
      const { abs, d } = queue.shift();
      const j = await api('list', { path: abs }).then((x) => (x.ok ? x.json() : null)).catch(() => null);
      if (!j) continue;
      const entries = (j.entries || []).filter((e) => e.name !== '.' && e.name !== '..')
        .sort((a, b) => (a.type === b.type ? a.name.localeCompare(b.name) : a.type === 'dir' ? -1 : 1));
      for (const e of entries) {
        if (out.length >= maxEntries) break;
        const childAbs = abs + '/' + e.name;
        if (dirsOnly && e.type !== 'dir') continue;
        out.push({ path: rel(childAbs), abs: childAbs, name: e.name, type: e.type, size: e.size || 0, depth: d });
        if (e.type === 'dir' && d + 1 < depth) queue.push({ abs: childAbs, d: d + 1 });
      }
    }
    return { ok: true, root: rel(start), entries: out, truncated: out.length >= maxEntries };
  }

  // grep-like search across the workspace's text files.
  async function search(query, { glob = '', maxFiles = 300, maxMatches = 200, maxFileBytes = 512 * 1024, regex = false, caseSensitive = false } = {}) {
    if (!query) return { ok: false, error: 'empty-query' };
    let rx = null;
    if (regex) { try { rx = new RegExp(query, caseSensitive ? '' : 'i'); } catch { return { ok: false, error: 'bad-regex' }; } }
    const needle = caseSensitive ? query : query.toLowerCase();
    const globRx = glob ? globToRegExp(glob) : null;
    const t = await tree('.', { depth: 8, maxEntries: 4000 });
    const files = (t.entries || []).filter((e) => e.type === 'file' && isTextName(e.name) && (!globRx || globRx.test(e.path)));
    const matches = []; let scanned = 0;
    for (const f of files) {
      if (scanned >= maxFiles || matches.length >= maxMatches) break;
      if (f.size > maxFileBytes) continue;
      scanned++;
      const r = await read(f.path, { maxBytes: maxFileBytes });
      if (!r.ok) continue;
      const lines = r.content.split('\n');
      for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        const hit = rx ? rx.test(line) : (caseSensitive ? line : line.toLowerCase()).includes(needle);
        if (hit) { matches.push({ path: f.path, line: i + 1, text: line.trim().slice(0, 200) }); if (matches.length >= maxMatches) break; }
      }
    }
    return { ok: true, query, matches, filesScanned: scanned, truncated: matches.length >= maxMatches };
  }

  async function glob(pattern, { dir = '.' } = {}) {
    const rx = globToRegExp(pattern);
    const t = await tree(dir, { depth: 8, maxEntries: 4000 });
    const files = (t.entries || []).filter((e) => e.type === 'file' && rx.test(e.path)).map((e) => e.path);
    return { ok: true, pattern, files };
  }

  return {
    setRoot(p) { root = p ? normPath(p) : ''; cwd = ''; return root; },
    getRoot() { return root; },
    hasRoot() { return !!root; },
    setCwd(p) { try { const full = resolve(p); cwd = full === root ? '' : full.slice(root.length + 1); } catch {} return cwd; },
    getCwd() { return cwd; },
    resolve, rel, stat, exists, list, read, write, append, edit, del, move, mkdir, tree, search, glob,
  };
}

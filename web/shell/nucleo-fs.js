/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-fs.js — the ONE way the web OS touches the device's disk.
//
// Every edition answers the same /api/fs contract, but each in its own dialect: the Cardputer refuses to move
// onto an existing file, the P4 replaces it, the Handheld answers 400; only the Cardputer appends by offset;
// the browser edition keeps files in IndexedDB. Forty apps each wrote their own fetch('/api/fs/...') helper
// around that, each with its own idea of a 404, a half-written file or a full card. This module is the
// shared answer, used by the shell, by the broker (for apps without an origin) and by every system app:
//
//   import fs from '/nucleo-fs.js';
//   const text = await fs.readText('/data/Documents/note.txt');      // null when the file does not exist
//   await fs.write('/data/Documents/note.txt', text);                // atomic: the old file survives a failure
//   await fs.writeJSON('/data/apps/notepad/state.json', state);      // folders are made as needed
//   for (const e of await fs.list('/data/Pictures')) …               // [] when the folder does not exist
//
// What it guarantees, on every edition:
//   • ATOMIC replace: the bytes go to a hidden temporary file, its size is checked, then it takes the
//     name; the previous file is kept until then (and put back if the swap fails). A dropped Wi-Fi or a
//     full card never leaves half a document where the whole one was.
//   • SPACE first: a write that cannot fit is refused before a byte is sent ('no-space').
//   • RESUMABLE big writes where the device can append by offset (fs.append): a cut connection resumes
//     from what the device has, instead of starting over.
//   • GENTLE on small devices: at most `max_inflight` requests at a time (from /api/system/caps), reads
//     retried once when the device is momentarily busy.
//   • ONE error vocabulary: FsError.code is not-found, exists, no-space, denied, offline, too-large, bad-path,
//     unsupported or failed — what an app shows the user, in its own words.
// Apps without an origin cannot import this module: they get the same behaviour through NucleoApp.files.

import Caps from './nucleo-caps.js';

// code: what happened, in the vocabulary below; status: the device's HTTP answer when there was one (0 when the
// request never got an answer), for apps that already show "failed (507)" to the user.
// reason: what the device itself said, when it said something short and specific — its JSON {error} or a
// one-line message ("r2_locked", "protected system file", "destination exists"): an edition's own cases
// (the Web edition's encrypted cloud drive, a protected file) reach the app without a route of its own.
export class FsError extends Error {
  constructor(code, detail, status = 0, reason = '') {
    super(code + (detail ? ': ' + detail : '')); this.code = code; this.detail = detail || ''; this.status = status; this.reason = reason;
  }
}

// Full literal routes (the browser edition's build rewrites absolute "/api/" strings under its base path).
const R = {
  read: '/api/fs/read?path=', write: '/api/fs/write?path=', list: '/api/fs/list?path=', mkdir: '/api/fs/mkdir?path=',
  remove: '/api/fs/delete?path=', move: '/api/fs/move?from=', stat: '/api/fs/stat?path=', copy: '/api/fs/copy?from=',
  status: '/api/status', mounts: '/api/fs/mounts',
};
const enc = new TextEncoder();
const dec = new TextDecoder();
const ABSENT = new Set([404, 405, 501]);        // a route this backend does not have
const BUSY = new Set([502, 503, 504]);
const TRASH_DB = '/system/config/trash.json';
const TRASH_CAP = 20;

// '/data//a/./b/../c' → '/data/a/c'; never climbs above the root. Paths are always absolute.
export function normPath(p) {
  const out = [];
  for (const seg of String(p == null ? '' : p).split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') { out.pop(); continue; }
    out.push(seg);
  }
  return '/' + out.join('/');
}
export const dirOf = (p) => normPath(p).replace(/\/[^/]*$/, '') || '/';
export const nameOf = (p) => normPath(p).split('/').pop();
export const joinPath = (...parts) => normPath(parts.join('/'));
// The hidden names used while a file is being replaced. Short and fixed (10 characters), whatever the file is
// called: the oldest Cardputer firmware takes at most 199 encoded characters of path, and a temporary name built
// from the real one would push a long save name over it. Listings from this module never show them.
const TMP_RE = /^\.~nfs[a-z0-9]{5}$/;
const tmpName = () => '.~nfs' + Math.random().toString(36).slice(2).padEnd(5, '0').slice(0, 5);

function toBytes(data) {
  if (data == null) return new Uint8Array(0);
  if (typeof data === 'string') return enc.encode(data);
  if (data instanceof Uint8Array) return data;
  if (data instanceof ArrayBuffer) return new Uint8Array(data);
  if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  throw new FsError('failed', 'data must be text or bytes');
}
const isBlob = (d) => typeof Blob !== 'undefined' && d instanceof Blob;

// drives(path) → a drive object or null: a path served by code of the web OS instead of the device's /api/fs (the
// encrypted cloud archive's /r2, nucleo-archive.js). The default instance below wires it.
export function createFs({ fetchFn, caps = () => Caps.info(), onChange = null,
  timeoutMs = 20000, chunk = 256 * 1024, drives = null } = {}) {
  // Upload progress byte by byte needs XMLHttpRequest (fetch has none): used for whole-file writes when this
  // instance talks to the real network, never when a test injects its own fetch.
  const useXhr = !fetchFn && typeof XMLHttpRequest !== 'undefined';
  fetchFn = fetchFn || ((...a) => fetch(...a));
  // ── device discipline: a small queue in front of every request ──
  let inflight = 0, limit = 0;
  const waiting = [];
  async function capInfo() { try { return (await caps()) || { caps: [], limits: {} }; } catch { return { caps: [], limits: {} }; } }
  async function slot() {
    if (!limit) { const l = (await capInfo()).limits || {}; limit = Math.max(1, Math.min(8, l.max_inflight || 2)); }
    if (inflight < limit) { inflight++; return; }
    await new Promise((r) => waiting.push(r));
    inflight++;
  }
  function release() { inflight--; const n = waiting.shift(); if (n) n(); }
  async function req(url, opts = {}, { retry = false, ms = timeoutMs } = {}) {
    await slot();
    try {
      for (let attempt = 0; ; attempt++) {
        const o = { cache: 'no-store', ...opts };
        if (!o.signal && typeof AbortSignal !== 'undefined' && AbortSignal.timeout && ms) o.signal = AbortSignal.timeout(ms);
        let r;
        try { r = await fetchFn(url, o); }
        catch (e) {
          if (retry && attempt === 0) { await new Promise((ok) => setTimeout(ok, 400)); continue; }
          throw new FsError('offline', String((e && e.message) || e));
        }
        if (retry && attempt === 0 && BUSY.has(r.status)) { await new Promise((ok) => setTimeout(ok, 600)); continue; }
        return r;
      }
    } finally { release(); }
  }
  async function reasonOf(r) {
    try {
      const t = (await (r.clone ? r.clone() : r).text()).trim();
      if (!t || t.length > 300) return '';
      try { const j = JSON.parse(t); if (j && typeof j.error === 'string') return j.error.slice(0, 80); } catch {}
      return /^[^<{\n]{1,120}$/.test(t) ? t : '';
    } catch { return ''; }
  }
  async function fail(r, what) {
    const st = r.status;
    const why = r.text ? await reasonOf(r) : '';
    if (st === 404) throw new FsError('not-found', what, st, why);
    if (st === 401 || st === 403) throw new FsError('denied', what, st, why);
    if (st === 409) throw new FsError('exists', what, st, why);
    if (st === 413) throw new FsError('too-large', what, st, why);
    if (st === 507) throw new FsError('no-space', what, st, why);
    if (BUSY.has(st)) throw new FsError('offline', what, st, why);
    throw new FsError('failed', what, st, why);
  }
  const changed = (p) => { if (onChange) { try { onChange(p); } catch {} } };
  // Absolute paths only; the root itself only where reading it makes sense (listing, stat).
  const checkPath = (p, root = false) => {
    const n = normPath(p);
    if ((n === '/' && !root) || typeof p !== 'string' || !p.startsWith('/')) throw new FsError('bad-path', String(p));
    return n;
  };
  const drive = async (p) => (drives ? drives(normPath(p)) : null);
  const onChanged = (p) => { if (onChange) try { onChange(p); } catch {} };

  // ── reading ──
  async function read(path) {
    const p = checkPath(path);
    const d = await drive(p); if (d) return d.read(p);
    const r = await req(R.read + encodeURIComponent(p), {}, { retry: true });
    if (r.status === 404) return null;
    if (!r.ok) await fail(r, p);
    return new Uint8Array(await r.arrayBuffer());
  }
  // Bytes [start, end] (inclusive) of a file: a Range read where the backend honours it, else a slice.
  async function readRange(path, start, end) {
    const p = checkPath(path);
    const d = await drive(p); if (d) return d.readRange(p, start, end);
    const r = await req(R.read + encodeURIComponent(p), { headers: { Range: 'bytes=' + start + '-' + end } }, { retry: true });
    if (r.status === 404) return null;
    if (!r.ok) await fail(r, p);
    const b = new Uint8Array(await r.arrayBuffer());
    return r.status === 206 ? b : b.subarray(start, end + 1);
  }
  // The URL that shows a device file in <img>, <video>, <audio> or a download link (streamed by the device).
  // Segments encoded one by one, "/" kept: players read the file name from the URL (EmulatorJS does), and the
  // same file always gets the same URL, so the browser cache is shared between apps. A relative path (a
  // backend's own library path, "roms/…") stays relative.
  const url = (path) => R.read + (String(path).startsWith('/') ? normPath(path) : String(path)).split('/').map(encodeURIComponent).join('/');
  async function readText(path) { const b = await read(path); return b == null ? null : dec.decode(b); }
  async function readJSON(path, fallback = null) {
    const t = await readText(path);
    if (t == null || t === '') return fallback;
    try { return JSON.parse(t); } catch { return fallback; }          // a damaged file is not a crash
  }
  async function listAll(dir) {
    const p = checkPath(dir, true);
    const d = await drive(p); if (d) return d.list(p);
    const r = await req(R.list + encodeURIComponent(p), {}, { retry: true });
    if (r.status === 404) return [];
    if (!r.ok) await fail(r, p);
    const j = await r.json().catch(() => null);
    return ((j && j.entries) || []).filter((e) => e && typeof e.name === 'string')
      // name/type/size normalised; whatever else the device says (mtime, protected, has_subdirs, trash) passes through
      .map((e) => ({ ...e, name: e.name, type: e.type === 'dir' ? 'dir' : 'file', size: Number(e.size) || 0 }));
  }
  // The listing apps see: without this module's temporary files.
  const list = async (dir) => (await listAll(dir)).filter((e) => !TMP_RE.test(e.name));
  // { type, size } or null. The listing of the parent is the one answer every edition gives.
  // Where the device has /api/fs/stat (capability fs.stat) it answers with more (protected, read_only, trash…).
  async function stat(path) {
    const p = checkPath(path, true);
    const d = await drive(p); if (d) return d.stat(p);
    if (p === '/' || p === '/data') return { type: 'dir', size: 0 };
    if (!TMP_RE.test(nameOf(p)) && (await capInfo()).caps.includes('fs.stat')) {
      const r = await req(R.stat + encodeURIComponent(p), {}, { retry: true });
      if (r.status === 404) return null;
      if (r.ok) {
        const j = await r.json().catch(() => null);
        if (j && (j.type === 'file' || j.type === 'dir')) {
          const { ok, exists, ...rest } = j;
          return { ...rest, size: Number(j.size) || 0 };
        }
      } else if (!ABSENT.has(r.status)) await fail(r, p);
    }
    const e = (await listAll(dirOf(p))).find((x) => x.name === nameOf(p));
    if (!e) return null;
    const { name, ...rest } = e;                          // { type, size, and the device's own fields }
    return rest;
  }
  const exists = async (path) => !!(await stat(path));
  // The device's storage (capability fs.mounts): { mounts: [{ name, writable, … }], media_alt, … } — the whole
  // answer, media_alt included (where media lived before a second card arrived); { mounts: [] } elsewhere.
  async function mounts() {
    const info = await capInfo();
    if (!info.caps.includes('fs.mounts')) return { mounts: [] };
    const r = await req(R.mounts, {}, { retry: true });
    if (!r.ok) return { mounts: [] };
    const j = await r.json().catch(() => null);
    if (Array.isArray(j)) return { mounts: j };
    return j && Array.isArray(j.mounts) ? j : { mounts: [] };
  }
  // Free bytes on the device's storage, or null when it does not say.
  async function free() {
    try {
      const r = await req(R.status, {}, { retry: true, ms: 8000 });
      if (!r.ok) return null;
      const j = await r.json();
      const s = j && j.storage;
      return s && Number.isFinite(Number(s.free_bytes)) && Number(s.total_bytes) > 0 ? Number(s.free_bytes) : null;
    } catch { return null; }
  }

  // ── folders ──
  // mkdir is not recursive on every backend, and "already there" is success.
  const madeDirs = new Set();
  // A backend found without /api/fs/move (an old firmware): writes go straight to the file from then on, rather
  // than leave a temporary file behind on every save.
  let noMove = false;
  // A folder removed or moved away is no longer "made": the next mkdirp under it really asks again.
  const forgetDirs = (p) => { for (const d of [...madeDirs]) if (d === p || d.startsWith(p + '/')) madeDirs.delete(d); };
  async function mkdirp(dir) {
    const p = checkPath(dir);
    const dv = await drive(p); if (dv) return dv.mkdirp(p);
    const segs = p.split('/').filter(Boolean);
    let acc = '';
    for (const s of segs) {
      acc += '/' + s;
      if (madeDirs.has(acc)) continue;
      const r = await req(R.mkdir + encodeURIComponent(acc), { method: 'POST' });
      if (!r.ok && r.status !== 409) {
        if (r.status === 401 || r.status === 403) await fail(r, acc);
        if (!(await exists(acc).catch(() => false))) await fail(r, acc);
      }
      madeDirs.add(acc);
    }
  }

  // ── writing ──
  // A full card says so in words on the ESP32 editions (esp_http_server has no 507): read the answer before naming it.
  async function failWrite(r, what) {
    if (r.status === 500) {
      const t = await r.text().catch(() => '');
      if (/free space|no space|disk full|ENOSPC/i.test(t)) throw new FsError('no-space', what, 500);
    }
    await fail(r, what);
  }
  async function post(url, body) {
    const r = await req(url, { method: 'POST', body }, { ms: Math.max(timeoutMs, 120000) });
    if (!r.ok) await failWrite(r, url.split('?path=')[1] || url);
    return r;
  }
  // The bytes into one file, whole or by resumable appends. `src` is a Blob (a File the user dropped: the
  // browser streams it, it is never loaded whole into memory) or bytes. Returns the size written.
  async function upload(path, src, onProgress, info) {
    const wurl = R.write + encodeURIComponent(path);
    const blob = isBlob(src) ? src : new Blob([src]);
    const total = blob.size;
    const step = Math.max(16 * 1024, (info.limits && info.limits.fs_write_max) || chunk);
    const canAppend = info.caps.includes('fs.append');
    if (!canAppend || total <= step) {
      if (useXhr && onProgress && total > 256 * 1024) await xhrPost(wurl, blob, onProgress);
      else await post(wurl, blob);
      if (onProgress) onProgress(total, total);
      return total;
    }
    let at = 0, misses = 0;
    while (at < total) {
      const part = blob.slice(at, Math.min(total, at + step));
      const r = await req(wurl + (at === 0 ? '' : '&append=1&at=' + at), { method: 'POST', body: part }, { ms: 120000 }).catch((e) => e);
      if (r instanceof Error || !r.ok) {
        if (++misses > 8) { if (r instanceof Error) throw r; await failWrite(r, path); }   // a weak Wi-Fi gets patience
        // 409 tells how much the device really has: resume from there (first chunk: start again).
        if (!(r instanceof Error) && r.status === 409) { const j = await r.json().catch(() => null); at = j && Number.isInteger(j.size) && j.size <= total ? j.size : 0; }
        await new Promise((ok) => setTimeout(ok, Math.min(6000, 750 * misses)));
        continue;
      }
      misses = 0;
      at += part.size;
      if (onProgress) onProgress(at, total);
    }
    return total;
  }
  async function xhrPost(url, blob, onProgress) {
    await slot();
    try {
      const status = await new Promise((resolve, reject) => {
        const x = new XMLHttpRequest();
        x.open('POST', url);
        x.timeout = 0;                                   // a big file on a slow link takes what it takes
        x.upload.onprogress = (e) => { if (e.lengthComputable) onProgress(e.loaded, e.total); };
        x.onload = () => resolve(x.status);
        x.onerror = () => reject(new FsError('offline', url));
        x.send(blob);
      });
      if (status < 200 || status >= 300) await failWrite({ status, ok: false, text: async () => '' }, url.split('?path=')[1] || url);
    } finally { release(); }
  }
  async function moveRaw(from, to) {
    const r = await req(R.move + encodeURIComponent(from) + '&to=' + encodeURIComponent(to), { method: 'POST' });
    return r;
  }
  // Writes to the SAME path wait for each other (an autosave and a Ctrl+S at once): two replaces interleaved
  // would each move the other's file away. Different paths still go in parallel.
  const pathLocks = new Map();
  function locked(p, fn) {
    const prev = pathLocks.get(p) || Promise.resolve();
    const run = prev.then(fn, fn);
    const tail = run.then(() => {}, () => {});
    pathLocks.set(p, tail);
    tail.then(() => { if (pathLocks.get(p) === tail) pathLocks.delete(p); });
    return run;
  }
  // write(path, data, { onProgress, mkdir = true, atomic = true, overwrite = true, keepalive = false })
  function write(path, data, opts = {}) {
    let p;
    try { p = checkPath(path); } catch (e) { return Promise.reject(e); }
    if (opts.keepalive) return writeNow(p, data, opts);
    return locked(p, () => writeNow(p, data, opts));
  }
  async function writeNow(p, data, opts) {
    const dv = await drive(p);
    if (dv) { const out = await dv.write(p, data, opts); onChanged(p); return out; }
    // keepalive: the one request that survives the page closing (history, last state on exit). Small (the
    // browser caps it at 64 KB), straight to the file, not atomic, never waited for by an unloading page.
    if (opts.keepalive) {
      const body = isBlob(data) ? data : new Blob([toBytes(data)]);
      // over the browser's 64 KB keepalive budget the request is a plain one: it may not outlive the page, but it is sent
      fetchFn(R.write + encodeURIComponent(p), { method: 'POST', body, keepalive: body.size < 60000 }).catch(() => {});
      changed(p);
      return { path: p, bytes: body.size };
    }
    const bytes = isBlob(data) ? data : toBytes(data);                    // a Blob stays a stream
    const size = isBlob(bytes) ? bytes.size : bytes.byteLength;
    const info = await capInfo();
    if (opts.mkdir !== false) await mkdirp(dirOf(p));
    // A plain write needs to know nothing about the old file (one request less per file), unless told not to replace it.
    const plain = opts.atomic === false || noMove;
    const before = plain && opts.overwrite !== false ? null : await stat(p).catch(() => null);
    if (before && before.type === 'dir') throw new FsError('exists', p + ' is a folder');
    if (before && opts.overwrite === false) throw new FsError('exists', p);
    // Space: the new bytes must fit while the old file still exists (it is kept until the swap).
    const room = size > 64 * 1024 ? await free() : null;
    if (room != null && size + 64 * 1024 > room) throw new FsError('no-space', size + ' > ' + room);
    if (plain) {
      await upload(p, bytes, opts.onProgress, info);
      changed(p);
      return { path: p, bytes: size };
    }
    const dir = dirOf(p), name = nameOf(p);
    const tmp = joinPath(dir, tmpName());
    try {
      await upload(tmp, bytes, opts.onProgress, info);
      const got = await stat(tmp);
      if (!got || got.size !== size) throw new FsError('failed', 'the device kept ' + (got ? got.size : 0) + ' of ' + size + ' bytes');
    } catch (e) {
      await remove(tmp).catch(() => {});
      throw e;
    }
    let old = null;
    if (before) {
      old = joinPath(dir, tmpName());
      const r = await moveRaw(p, old);
      if (!r.ok) {
        // A backend without move (or a protected file it will not rename): the plain write is all there is.
        await remove(tmp).catch(() => {});
        if (ABSENT.has(r.status)) noMove = true;
        if (ABSENT.has(r.status) || r.status === 403) { await upload(p, bytes, null, info); changed(p); return { path: p, bytes: size }; }
        await fail(r, p);
      }
    }
    const r2 = await moveRaw(tmp, p);
    if (!r2.ok) {
      if (old) await moveRaw(old, p).catch(() => {});       // the previous version goes back where it was
      if (ABSENT.has(r2.status) && !old) { noMove = true; await remove(tmp).catch(() => {}); await upload(p, bytes, null, info); changed(p); return { path: p, bytes: size }; }
      await remove(tmp).catch(() => {});
      await fail(r2, p);
    }
    if (old) await remove(old).catch(() => {});
    changed(p);
    return { path: p, bytes: size };
  }
  const writeText = (path, text, opts) => write(path, String(text == null ? '' : text), opts);
  const writeJSON = (path, value, opts) => write(path, JSON.stringify(value, null, opts && opts.pretty ? 2 : 0), opts);

  async function remove(path) {
    const p = checkPath(path);
    if (p === '/data') throw new FsError('denied', p);
    const dv = await drive(p);
    if (dv) { await dv.remove(p); onChanged(p); return true; }
    forgetDirs(p);
    const r = await req(R.remove + encodeURIComponent(p), { method: 'POST' });
    if (r.status === 404) return false;
    if (!r.ok) await fail(r, p);
    changed(p);
    return true;
  }
  // move(from, to, { overwrite }) — the same answer on every edition: without overwrite an existing target
  // is 'exists'; with it, the target is replaced only once the move is sure to work.
  async function move(from, to, opts = {}) {
    const a = checkPath(from), b = checkPath(to);
    if (a === b) return { path: b };
    const dv = (await drive(a)) || (await drive(b));
    if (dv) { await dv.move(a, b, opts); onChanged(a); onChanged(b); return { path: b }; }
    const there = await stat(b);
    if (there && !opts.overwrite) throw new FsError('exists', b);
    await mkdirp(dirOf(b));
    let old = null;
    if (there) {
      old = joinPath(dirOf(b), tmpName());
      const r = await moveRaw(b, old);
      if (!r.ok) await fail(r, b);
    }
    const r = await moveRaw(a, b);
    if (!r.ok) { if (old) await moveRaw(old, b).catch(() => {}); await fail(r, a); }
    if (old) await remove(old).catch(() => {});
    forgetDirs(a);
    changed(a); changed(b);
    return { path: b };
  }
  // copy: the device's own copy where it has one (fs.copy), else read and write here.
  async function copy(from, to, opts = {}) {
    const a = checkPath(from), b = checkPath(to);
    const dv = (await drive(a)) || (await drive(b));
    if (dv) { await dv.copy(a, b, opts); onChanged(b); return { path: b }; }
    if (!opts.overwrite && await exists(b)) throw new FsError('exists', b);
    const info = await capInfo();
    if (info.caps.includes('fs.copy') && !opts.overwrite) {
      await mkdirp(dirOf(b));
      const r = await req(R.copy + encodeURIComponent(a) + '&to=' + encodeURIComponent(b), { method: 'POST' }, { ms: 120000 });
      if (r.ok) { changed(b); return { path: b }; }
      if (!ABSENT.has(r.status)) await fail(r, a);
    }
    const bytes = await read(a);
    if (bytes == null) throw new FsError('not-found', a);
    return write(b, bytes, { onProgress: opts.onProgress });
  }
  // A free name in `dir` for `name`: "file.txt" → "file (2).txt".
  async function uniquePath(dir, name) {
    const d = checkPath(dir);
    const have = new Set((await list(d)).map((e) => e.name));
    if (!have.has(name)) return joinPath(d, name);
    const dot = name.lastIndexOf('.'), base = dot > 0 ? name.slice(0, dot) : name, ext = dot > 0 ? name.slice(dot) : '';
    for (let i = 2; ; i++) { const c = `${base} (${i})${ext}`; if (!have.has(c)) return joinPath(d, c); }
  }

  // ── the Recycle Bin: one for every app (the format File Commander and the Recycle Bin app have always used) ──
  // trash(path) moves a file or folder to the bin and records it in /system/config/trash.json ({orig, trash, name, ts}),
  // so the Recycle Bin app lists and restores it. Every step is checked and the original goes LAST; any failure leaves
  // it where it was. Where the device trashes by itself (/api/fs/stat answers trash:true for the path, the Handheld's
  // cards) a delete is that move, and the device writes the entry. With accounts the bin is the user's own
  // (/system/config is theirs); without, it stays /system/.trash as it always was.
  async function trashIndex() {
    let txt;
    try { txt = await readText(TRASH_DB); } catch { return null; }      // a read that broke off is not an index
    if (txt == null || !txt.trim()) return { items: [] };
    try {
      const d = JSON.parse(txt);
      if (d && typeof d === 'object' && !Array.isArray(d)) {
        if (d.items === undefined) return { ...d, items: [] };
        if (Array.isArray(d.items)) return d;
      }
    } catch {}
    // A readable but damaged index: kept aside byte for byte, and a fresh one started (never an "empty bin" saved
    // over the record of every trashed file).
    try { await write(TRASH_DB.replace(/\.json$/, '.corrupt.json'), txt); return { items: [] }; } catch { return null; }
  }
  async function trash(path) {
    const p = checkPath(path);
    if (p === '/data' || p.split('/').length < 3) throw new FsError('denied', p);
    const info = await capInfo();
    if (info.caps.includes('fs.stat')) {
      const st = await stat(p);
      if (!st) throw new FsError('not-found', p);
      if (st.trash === true) {
        if (!(await remove(p))) throw new FsError('not-found', p);
        const db = await trashIndex();
        return ((db && db.items) || []).find((i) => i.orig === p) || { orig: p, name: nameOf(p), ts: Date.now() };
      }
    }
    const db = await trashIndex();
    if (!db) throw new FsError('failed', p, 0, 'trash_index');
    const bin = info.caps.includes('users.multi') ? '/system/config/.trash' : '/system/.trash';
    const name = nameOf(p);
    const entry = { orig: p, trash: joinPath(bin, Date.now() + '_' + Math.floor(Math.random() * 9999) + '_' + name), name, ts: Date.now() };
    db.items.unshift(entry);
    await write(TRASH_DB, JSON.stringify(db));      // the entry first: a file in the bin is never unlisted
    const unlist = async () => { db.items = db.items.filter((i) => i !== entry); await write(TRASH_DB, JSON.stringify(db)).catch(() => {}); };
    // A move is one rename on the device: files and folders alike, no bytes through the browser. Across two disks it
    // cannot be one, so the bytes are copied and the original removed only once the copy is there.
    let moved = false;
    try { await move(p, entry.trash); moved = true; } catch {}
    if (!moved) {
      try { await copy(p, entry.trash); } catch (e) { await remove(entry.trash).catch(() => {}); await unlist(); throw e; }
      let gone = false;
      try { gone = await remove(p); } catch {}
      if (!gone) { await remove(entry.trash).catch(() => {}); await unlist(); throw new FsError('failed', p, 0, 'trash_failed'); }
    }
    if (db.items.length > TRASH_CAP) {              // the oldest go only once this one is safely in
      while (db.items.length > TRASH_CAP) { const old = db.items.pop(); if (old.trash) await remove(old.trash).catch(() => {}); }
      await write(TRASH_DB, JSON.stringify(db)).catch(() => {});
    }
    return entry;
  }

  return { read, readRange, readText, readJSON, url, list, stat, exists, free, mounts, mkdirp, write, writeText, writeJSON, remove, move,
    copy, uniquePath, trash, trashIndex, normPath, dirOf, nameOf, joinPath, FsError };
}

// The instance every page of this origin shares. Shell and apps that want the file index refreshed after their
// writes listen with fs.onChange(fn).
const listeners = new Set();
// The encrypted cloud archive's drive (/r2), where the device declares cloud.archive: loaded on first use only.
let r2 = null;
async function cloudDrive(p) {
  if (p !== '/r2' && !p.startsWith('/r2/')) return null;
  try { if (!(await Caps.has('cloud.archive'))) return null; } catch { return null; }
  if (!r2) r2 = import('./nucleo-archive.js').then((m) => m.createDrive(m.archive, fs));
  return r2;
}
const fs = createFs({ onChange: (p) => { for (const fn of listeners) { try { fn(p); } catch {} } }, drives: cloudDrive });
fs.onChange = (fn) => { if (typeof fn === 'function') listeners.add(fn); return () => listeners.delete(fn); };
export default fs;

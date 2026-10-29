/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-archive.js — the encrypted cloud archive (Cloudflare R2 or any S3-compatible bucket), as a drive of the core
// file layer. Design and threat model: docs/cloud-archive.md.
//
// Everything is encrypted HERE, in the browser, before it leaves; the device and the bucket only ever see opaque
// pieces with random names. Each user of a device with accounts has an archive of their own (the server keeps it under
// that user's prefix), unlocked with their own phrase.
//
//   import { archive } from '/nucleo-archive.js';
//   await archive.status();                  // {configured, mode, used, cap, unlocked, ...}
//   const code = await archive.create(phrase); // first time: the recovery code, shown once
//   await archive.unlock(phraseOrCode);       // in this browser, until lock() or a change of person
//   // the drive: every app reaches /r2 through /nucleo-fs.js like any folder
//
// Wire format, identical to the archives the browser edition already created (so none needs migrating):
//   chiave, recupero   the archive key K wrapped with PBKDF2-SHA-256 (600 000 iterations) + AES-256-GCM
//   b/<32 hex>         a piece of a file: IV(12) | AES-GCM(K, piece, AAD = its own name)
//   indice             the encrypted list {v:1, seq, file:{path:{size, mtime, blocchi:[{n, h, l}]}}, dir:{path: mtime}}
// `seq` grows at every save; this browser refuses a list older than the last one it saw (a server replaying an old
// list to make it delete newer pieces).

import { FsError, normPath } from './nucleo-fs.js';

export const ROOT = '/r2';
export const PIECE = 8 * 1024 * 1024;
export const ITER = 600000;
export const MAX_FILE = 4 * 1024 * 1024 * 1024;
const WRAP_AAD = 'nucleoos-archivio-chiave';
const NAME_RE = /^(b\/[0-9a-f]{32}|indice|chiave|recupero)$/;
const RECOVERY = 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';
const enc = new TextEncoder();
const dec = new TextDecoder();
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, '0')).join('');
const b64 = (u8) => { let s = ''; for (const b of u8) s += String.fromCharCode(b); return btoa(s); };
const unb64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
const rand = (n) => crypto.getRandomValues(new Uint8Array(n));
const pieceName = () => 'b/' + hex(rand(16));
const nowS = () => Math.floor(Date.now() / 1000);

export class ArchiveError extends Error {
  constructor(code, status = 0) { super(code); this.code = code; this.status = status; }
}

// ── transports: where the opaque pieces go ──────────────────────────────────────────────────────────────────────
// relay: the device or server stores them on the bucket (/api/archive/object/<name>).
// sign:  the device only signs a short-lived URL for one object (/api/archive/sign) and the browser talks to the bucket
//        itself: a small device never carries 8 MiB pieces, and its credentials never leave it.
function statusError(r) {
  if (r.status === 409) return new ArchiveError('not_configured', 409);
  if (r.status === 507) return new ArchiveError('full', 507);
  if (r.status === 404) return new ArchiveError('not_found', 404);
  if (r.status === 401 || r.status === 403) return new ArchiveError('denied', r.status);
  if (r.status === 413) return new ArchiveError('too_large', 413);
  return new ArchiveError('offline', r.status);
}

export function createTransport({ base = '/api/archive/', fetchFn = (...a) => fetch(...a), directFetch = null } = {}) {
  const go = async (path, opts = {}) => {
    let r;
    try { r = await fetchFn(base + path, { credentials: 'same-origin', cache: 'no-store', ...opts }); }
    catch { throw new ArchiveError('offline'); }
    if (!r.ok) throw statusError(r);
    return r;
  };
  let mode = null;
  const direct = directFetch || ((u, o) => fetch(u, o));
  async function signed(method, name, body) {
    const r = await go('sign', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ method, name }) });
    const s = await r.json();
    if (!s || typeof s.url !== 'string' || !/^https:\/\//.test(s.url)) throw new ArchiveError('offline');
    let res;
    try { res = await direct(s.url, { method, body, headers: s.headers || {}, cache: 'no-store', credentials: 'omit' }); }
    catch { throw new ArchiveError('offline'); }
    if (!res.ok && !(method === 'DELETE' && res.status === 404)) throw statusError(res);
    return res;
  }
  return {
    async status() {
      const s = await (await go('status')).json();
      mode = s.mode === 'sign' ? 'sign' : 'relay';
      return s;
    },
    async get(name) {
      if (!NAME_RE.test(name)) throw new ArchiveError('bad_name');
      if (mode === null) await this.status();
      const r = mode === 'sign' ? await signed('GET', name) : await go('object/' + name);
      return new Uint8Array(await r.arrayBuffer());
    },
    async put(name, bytes) {
      if (!NAME_RE.test(name)) throw new ArchiveError('bad_name');
      if (mode === null) await this.status();
      if (mode === 'sign') await signed('PUT', name, bytes);
      else await go('object/' + name, { method: 'POST', headers: { 'content-type': 'application/octet-stream' }, body: bytes });
    },
    async del(names) {
      if (mode === null) await this.status();
      for (let i = 0; i < names.length; i += 200) {
        const part = names.slice(i, i + 200).filter((n) => NAME_RE.test(n));
        if (!part.length) continue;
        if (mode === 'sign') { for (const n of part) await signed('DELETE', n); }
        else await go('delete', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ names: part }) });
      }
    },
  };
}

// ── where this browser keeps an unlocked key: a NON-extractable CryptoKey (scripts can use it, never read it) ─────
export function idbKeyStore(dbName = 'nucleo.archive') {
  const open = () => new Promise((ok, ko) => {
    const r = indexedDB.open(dbName, 1);
    r.onupgradeneeded = () => r.result.createObjectStore('keys');
    r.onsuccess = () => ok(r.result);
    r.onerror = () => ko(r.error);
  });
  const run = async (mode, fn) => {
    const db = await open();
    try {
      return await new Promise((ok, ko) => {
        const tx = db.transaction('keys', mode), rq = fn(tx.objectStore('keys'));
        tx.oncomplete = () => ok(rq && rq.result);
        tx.onerror = () => ko(tx.error);
      });
    } finally { db.close(); }
  };
  return {
    get: (id) => run('readonly', (s) => s.get(id)).catch(() => null),
    set: (id, key) => run('readwrite', (s) => s.put(key, id)),
    del: (id) => run('readwrite', (s) => s.delete(id)).catch(() => {}),
  };
}
export function memoryKeyStore() {
  const m = new Map();
  return { get: async (id) => m.get(id) || null, set: async (id, k) => { m.set(id, k); }, del: async (id) => { m.delete(id); } };
}

// ── keys ─────────────────────────────────────────────────────────────────────────────────────────────────────────
async function derive(secret, salt, iter) {
  const base = await crypto.subtle.importKey('raw', enc.encode(secret.normalize('NFKC')), 'PBKDF2', false, ['deriveKey']);
  return crypto.subtle.deriveKey({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations: iter }, base,
    { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}
export async function wrapKey(raw, secret, iter = ITER) {
  const salt = rand(16), iv = rand(12);
  const kek = await derive(secret, salt, iter);
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: enc.encode(WRAP_AAD) }, kek, raw));
  return enc.encode(JSON.stringify({ v: 1, kdf: 'pbkdf2-sha256', iter, sale: b64(salt), iv: b64(iv), ct: b64(ct) }));
}
export async function unwrapKey(bytes, secret) {
  let d;
  try { d = JSON.parse(dec.decode(bytes)); } catch { throw new ArchiveError('format'); }
  if (!d || d.v !== 1 || d.kdf !== 'pbkdf2-sha256' || !(d.iter >= 100000 && d.iter <= 5000000)) throw new ArchiveError('format');
  const kek = await derive(secret, unb64(d.sale), d.iter);
  let raw;
  try { raw = new Uint8Array(await crypto.subtle.decrypt({ name: 'AES-GCM', iv: unb64(d.iv), additionalData: enc.encode(WRAP_AAD) }, kek, unb64(d.ct))); }
  catch { throw new ArchiveError('wrong_secret', 403); }
  try { return await crypto.subtle.importKey('raw', raw, 'AES-GCM', false, ['encrypt', 'decrypt']); }
  finally { raw.fill(0); }
}
export function recoveryCode() {
  const r = rand(25);
  return Array.from(r, (b) => RECOVERY[b % RECOVERY.length]).join('').match(/.{5}/g).join('-');
}
const recoveryForm = (s) => String(s || '').toUpperCase().replace(/[^A-Z0-9]/g, '');
export async function seal(k, name, data) {
  const iv = rand(12);
  const ct = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: enc.encode(name) }, k, data));
  const out = new Uint8Array(12 + ct.length); out.set(iv); out.set(ct, 12);
  return out;
}
export async function open(k, name, bytes) {
  try { return new Uint8Array(await crypto.subtle.decrypt({ name: 'AES-GCM', iv: bytes.slice(0, 12), additionalData: enc.encode(name) }, k, bytes.slice(12))); }
  catch { throw new ArchiveError('tampered'); }       // wrong key, or a piece that was changed or swapped
}
const sha = async (u8) => hex(new Uint8Array(await crypto.subtle.digest('SHA-256', u8)));
async function toBlob(data) {
  if (data instanceof Blob) return data;
  if (typeof data === 'string') return new Blob([enc.encode(data)]);
  if (data instanceof ArrayBuffer || ArrayBuffer.isView(data)) return new Blob([data]);
  throw new ArchiveError('bad_data');
}

// ── the archive of one user ─────────────────────────────────────────────────────────────────────────────────────
export function createArchive({ transport = createTransport(), keys = idbKeyStore(), who = async () => 'owner',
  seq = { get: (id) => +(safeLS(() => localStorage.getItem('nucleo.archive.seq.' + id)) || 0),
    set: (id, n) => safeLS(() => localStorage.setItem('nucleo.archive.seq.' + id, String(n))) } } = {}) {
  let queue = Promise.resolve();
  let view = null;                       // {index, t} for reads within a few seconds
  // Every change of the list goes through ONE queue and starts from the server's copy: two changes made from the
  // same browser can never overwrite each other.
  const inQueue = (fn) => { const r = queue.then(fn, fn); queue = r.catch(() => {}); return r; };

  async function key() {
    const k = await keys.get(await who());
    if (!k) throw new ArchiveError('locked', 423);
    return k;
  }
  async function loadIndex(k, fresh) {
    if (!fresh && view && Date.now() - view.t < 5000) return view.index;
    let index;
    try { index = JSON.parse(dec.decode(await open(k, 'indice', await transport.get('indice')))); }
    catch (e) { if (e.code === 'not_found') index = { v: 1, seq: 0, file: {} }; else throw e; }
    if (!index || typeof index.file !== 'object') throw new ArchiveError('format');
    index.dir = index.dir || {};
    const id = await who(), seen = seq.get(id), got = +index.seq || 0;
    if (got < seen) throw new ArchiveError('rolled_back');
    if (got > seen) seq.set(id, got);
    view = { index, t: Date.now() };
    return index;
  }
  async function saveIndex(k, index) {
    index.seq = (+index.seq || 0) + 1;
    await transport.put('indice', await seal(k, 'indice', enc.encode(JSON.stringify(index))));
    seq.set(await who(), index.seq);
    view = { index, t: Date.now() };
  }
  async function dropOrphans(index, before) {
    const used = new Set();
    for (const v of Object.values(index.file)) for (const b of v.blocchi) used.add(b.n);
    const gone = [...new Set(before)].filter((n) => !used.has(n));
    if (gone.length) await transport.del(gone).catch(() => {});
  }

  const api = {
    async status() {
      let server;
      try { server = await transport.status(); }
      catch (e) { server = { configured: e.code !== 'not_configured', error: e.code }; }
      return { configured: !!server.configured, mode: server.mode || 'relay', used: server.used, cap: server.cap,
        error: server.error || '', unlocked: !!(await keys.get(await who())) };
    },
    async create(phrase) {
      if (typeof phrase !== 'string' || [...phrase].length < 12 || phrase.length > 512) throw new ArchiveError('weak_phrase');
      try { await transport.get('chiave'); throw new ArchiveError('exists', 409); }
      catch (e) { if (e.code !== 'not_found') throw e; }
      const raw = rand(32), code = recoveryCode();
      try {
        await transport.put('chiave', await wrapKey(raw, phrase));
        await transport.put('recupero', await wrapKey(raw, recoveryForm(code)));
        await keys.set(await who(), await crypto.subtle.importKey('raw', raw, 'AES-GCM', false, ['encrypt', 'decrypt']));
      } finally { raw.fill(0); }
      return code;
    },
    async unlock(secret) {
      let k;
      try { k = await unwrapKey(await transport.get('chiave'), String(secret || '')); }
      catch (e) {
        if (e.code !== 'wrong_secret') throw e;
        k = await unwrapKey(await transport.get('recupero'), recoveryForm(secret));     // or the recovery code
      }
      await keys.set(await who(), k);
      view = null;
      return true;
    },
    async lock() { await keys.del(await who()); view = null; },
    async isUnlocked() { return !!(await keys.get(await who())); },

    // ── the drive: paths under /r2 ──
    async list(p) {
      const index = await loadIndex(await key());
      const t = typeOf(index, p);
      if (!t) throw new ArchiveError('not_found', 404);
      if (t !== 'dir') throw new ArchiveError('not_a_dir', 400);
      return children(index, p);
    },
    async stat(p) {
      const index = await loadIndex(await key());
      const t = typeOf(index, p);
      if (!t) return null;
      const v = index.file[p];
      return { type: t, size: v ? v.size : 0, mtime: v ? v.mtime : (index.dir[p] || 0) };
    },
    async read(p, start = 0, end = Infinity) {
      const k = await key();
      const v = (await loadIndex(k)).file[p];
      if (!v) return null;
      const last = Math.min(end, v.size - 1);
      const parts = [];
      let pos = 0;
      for (const b of v.blocchi) {
        const fin = pos + b.l;
        if (fin > start && pos <= last) {
          const piece = await open(k, b.n, await transport.get(b.n));
          if (await sha(piece) !== b.h) throw new ArchiveError('tampered');
          parts.push(piece.subarray(Math.max(0, start - pos), Math.min(b.l, last - pos + 1)));
        }
        pos = fin;
        if (pos > last) break;
      }
      const out = new Uint8Array(parts.reduce((s, x) => s + x.length, 0));
      let o = 0;
      for (const x of parts) { out.set(x, o); o += x.length; }
      return out;
    },
    async write(p, data, mtime) {
      if (p === ROOT) throw new ArchiveError('bad_path', 400);
      const blob = await toBlob(data);
      if (blob.size > MAX_FILE) throw new ArchiveError('too_large', 413);
      const k = await key();
      await inQueue(async () => {
        const index = await loadIndex(k, true);
        if (typeOf(index, p) === 'dir') throw new ArchiveError('is_a_dir', 400);
        const before = index.file[p];
        const reuse = new Map((before ? before.blocchi : []).map((b) => [b.h, b.n]));
        const blocchi = [];
        for (let i = 0; i < Math.max(1, Math.ceil(blob.size / PIECE)); i++) {
          const piece = new Uint8Array(await blob.slice(i * PIECE, (i + 1) * PIECE).arrayBuffer());
          const h = await sha(piece);
          let n = reuse.get(h);
          if (!n) { n = pieceName(); await transport.put(n, await seal(k, n, piece)); }
          blocchi.push({ n, h, l: piece.length });
        }
        index.file[p] = { size: blob.size, mtime: mtime || nowS(), blocchi };
        await saveIndex(k, index);
        if (before) await dropOrphans(index, before.blocchi.map((b) => b.n));
      });
      return blob.size;
    },
    async mkdir(p) {
      if (p === ROOT) return;
      const k = await key();
      await inQueue(async () => {
        const index = await loadIndex(k, true);
        const t = typeOf(index, p);
        if (t === 'dir') return;
        if (t) throw new ArchiveError('exists', 409);
        index.dir[p] = nowS();
        await saveIndex(k, index);
      });
    },
    async remove(p) {
      if (p === ROOT) throw new ArchiveError('protected', 403);
      const k = await key();
      const n = await inQueue(async () => {
        const index = await loadIndex(k, true);
        const files = under(Object.keys(index.file), p), dirs = under(Object.keys(index.dir), p);
        if (!files.length && !dirs.length) return 0;
        const before = [];
        for (const q of files) { index.file[q].blocchi.forEach((b) => before.push(b.n)); delete index.file[q]; }
        for (const q of dirs) delete index.dir[q];
        await saveIndex(k, index);
        await dropOrphans(index, before);
        return files.length + dirs.length;
      });
      if (!n) throw new ArchiveError('not_found', 404);
    },
    // Inside the drive only the list changes: the pieces are shared by reference.
    async relocate(from, to, { overwrite = false, keep = false } = {}) {
      if (from === ROOT || to === ROOT) throw new ArchiveError('protected', 403);
      if (to === from || to.startsWith(from + '/')) throw new ArchiveError('into_itself', 400);
      const k = await key();
      await inQueue(async () => {
        const index = await loadIndex(k, true);
        const t = typeOf(index, from);
        if (!t) throw new ArchiveError('not_found', 404);
        const before = [];
        if (typeOf(index, to)) {
          if (!overwrite) throw new ArchiveError('exists', 409);
          for (const q of under(Object.keys(index.file), to)) { index.file[q].blocchi.forEach((b) => before.push(b.n)); delete index.file[q]; }
        }
        for (const q of under(Object.keys(index.file), from)) {
          index.file[to + q.slice(from.length)] = { ...index.file[q], mtime: keep ? nowS() : index.file[q].mtime };
          if (!keep) delete index.file[q];
        }
        for (const q of under(Object.keys(index.dir), from)) {
          index.dir[to + q.slice(from.length)] = index.dir[q];
          if (!keep) delete index.dir[q];
        }
        if (t === 'dir' && !index.dir[to]) index.dir[to] = nowS();
        await saveIndex(k, index);
        await dropOrphans(index, before);
      });
    },
  };
  return api;
}

function safeLS(fn) { try { return fn(); } catch { return null; } }
const under = (paths, p) => paths.filter((q) => q === p || q.startsWith(p + '/'));
function typeOf(index, p) {
  if (p === ROOT) return 'dir';
  if (index.file[p]) return 'file';
  if (index.dir[p]) return 'dir';
  const pre = p + '/';
  if (Object.keys(index.file).some((q) => q.startsWith(pre)) || Object.keys(index.dir).some((q) => q.startsWith(pre))) return 'dir';
  return null;
}
function children(index, p) {
  const pre = p + '/', out = new Map();
  const see = (q, file) => {
    if (!q.startsWith(pre)) return;
    const rest = q.slice(pre.length), i = rest.indexOf('/');
    if (i < 0 && file) out.set(rest, { name: rest, type: 'file', size: file.size, mtime: file.mtime });
    else { const n = i < 0 ? rest : rest.slice(0, i); if (!out.has(n)) out.set(n, { name: n, type: 'dir', size: 0, mtime: 0 }); }
  };
  for (const [q, v] of Object.entries(index.file)) see(q, v);
  for (const q of Object.keys(index.dir)) see(q, null);
  return [...out.values()];
}

// ── the drive as nucleo-fs.js sees it ────────────────────────────────────────────────────────────────────────────
export const onDrive = (p) => typeof p === 'string' && (p === ROOT || p.startsWith(ROOT + '/'));
// Archive errors in the file layer's one vocabulary; `reason` keeps the precise word for the app (r2_locked...).
export function asFsError(e, what) {
  if (e instanceof FsError) return e;
  const c = e && e.code;
  const map = { locked: ['denied', 'r2_locked'], denied: ['denied', 'r2_session'], not_configured: ['unsupported', 'r2_not_configured'],
    full: ['no-space', 'no_space'], too_large: ['too-large', ''], not_found: ['not-found', ''], exists: ['exists', ''],
    offline: ['offline', 'r2_offline'], tampered: ['failed', 'r2_tampered'], rolled_back: ['failed', 'r2_rolled_back'],
    protected: ['denied', 'protected'], bad_path: ['bad-path', ''], into_itself: ['bad-path', 'into_itself'],
    is_a_dir: ['failed', 'is_a_directory'], not_a_dir: ['failed', 'not_a_directory'] };
  const [code, reason] = map[c] || ['failed', c || ''];
  return new FsError(code, what, e && e.status || 0, reason);
}
export function createDrive(arc, localFs) {
  const wrap = (fn) => async (...a) => { try { return await fn(...a); } catch (e) { throw asFsError(e, a[0]); } };
  const n = (p) => normPath(p);
  return {
    read: wrap(async (p) => arc.read(n(p))),
    readRange: wrap(async (p, s, e) => arc.read(n(p), s, e)),
    list: wrap(async (p) => { try { return await arc.list(n(p)); } catch (e) { if (e.code === 'not_found') return []; throw e; } }),
    stat: wrap(async (p) => arc.stat(n(p))),
    mkdirp: wrap(async (p) => arc.mkdir(n(p))),
    write: wrap(async (p, data, opts = {}) => ({ path: n(p), bytes: await arc.write(n(p), data, opts.mtime) })),
    remove: wrap(async (p) => arc.remove(n(p))),
    // Between the drive and the device, in either direction, file by file through the two layers.
    copy: wrap(async (from, to, opts = {}) => transfer(arc, localFs, n(from), n(to), { ...opts, keep: true })),
    move: wrap(async (from, to, opts = {}) => transfer(arc, localFs, n(from), n(to), { ...opts, keep: false })),
  };
}
async function transfer(arc, local, from, to, { overwrite = false, keep }) {
  if (onDrive(from) && onDrive(to)) return arc.relocate(from, to, { overwrite, keep });
  if (onDrive(to)) {                                     // device → drive
    const st = await local.stat(from);
    if (!st) throw new ArchiveError('not_found', 404);
    if ((await arc.stat(to)) && !overwrite) throw new ArchiveError('exists', 409);
    const walk = async (src, dst) => {
      if ((await local.stat(src)).type === 'dir') {
        await arc.mkdir(dst);
        for (const e of await local.list(src)) await walk(src + '/' + e.name, dst + '/' + e.name);
      } else await arc.write(dst, await local.read(src));
    };
    await walk(from, to);
    if (!keep) await local.remove(from);
    return;
  }
  const st = await arc.stat(from);                       // drive → device
  if (!st) throw new ArchiveError('not_found', 404);
  if ((await local.exists(to)) && !overwrite) throw new ArchiveError('exists', 409);
  const walk = async (src, dst) => {
    if ((await arc.stat(src)).type === 'dir') {
      await local.mkdirp(dst);
      for (const e of await arc.list(src)) await walk(src + '/' + e.name, dst + '/' + e.name);
    } else await local.write(dst, await arc.read(src));
  };
  await walk(from, to);
  if (!keep) await arc.remove(from);
}

// The archive of whoever is using this browser (nucleo-user.js decides who; single-user devices: the owner).
// Asked again every 10 seconds at most: a folder listing must not become a burst of status requests to a small device.
let whoCache = { id: null, t: 0 };
export const archive = createArchive({
  who: async () => {
    if (whoCache.id && Date.now() - whoCache.t < 10000) return whoCache.id;
    let id;
    try { const { default: User } = await import('./nucleo-user.js'); const me = await User.current(); id = (me && me.id) || 'nobody'; }
    catch { id = 'owner'; }
    whoCache = { id, t: Date.now() };
    return id;
  },
});
export default archive;

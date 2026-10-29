/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-pack.js — every archive format, for every app, on every device. Guide: docs/files.md.
//
//   import pack from '/nucleo-pack.js';
//   pack.isArchive('foto.7z')                                       // true: the OS opens it (pack.formats.read)
//   await pack.list(bytes, { name, password })                      // { format, encrypted, entries: [{ name, size, dir, mtime, encrypted, unsafe }] }
//   await pack.extractTo(fs, '/data/x.rar', '/data/x', { onProgress, overwrite, password, signal })   // → { files, folders, skipped }
//   await pack.compressTo(fs, ['/data/Foto'], '/data/Foto.7z', { onProgress, password })   // format from the name
//   await pack.create([{ name: 'a.txt', data }], { format: 'tar.xz' })                     // → Uint8Array
//
// ZIP goes through nucleo-zip.js (no WebAssembly, the smallest device too). Everything else — 7z, RAR (4 and 5), tar
// and its compressed forms, gz/bz2/xz/zst/lz4 files, ISO, CAB, cpio, ar/deb, RPM, XAR, LHA — goes through libarchive
// compiled for the OS (tools/libarchive-wasm), in a worker (nucleo-pack-worker.js) so the page never freezes,
// loaded the first time an archive needs it. Encrypted ZIP and 7z/RAR passwords use the same engine.
//
// The same rules as nucleo-zip.js for every format: names that would leave the destination, links, devices are never
// written; output is capped while it is produced (bombs); nothing is overwritten unless asked.
import zip, { safeName, ZipError, placeFor } from './nucleo-zip.js';
import { LIMITS } from './nucleo-pack-engine.js';

// What the OS opens as an archive (by extension; the content decides in the end) and what it writes.
export const READ = ['zip', '7z', 'rar', 'tar', 'gz', 'tgz', 'bz2', 'tbz', 'tbz2', 'xz', 'txz', 'zst', 'tzst', 'lz4', 'lzma',
  'tlz', 'z', 'iso', 'cab', 'cpio', 'ar', 'deb', 'rpm', 'xar', 'lha', 'lzh', 'warc'];
export const WRITE = {
  zip: { format: 'zip' },
  '7z': { format: '7zip', options: '7zip:compression=lzma2' },
  tar: { format: 'paxr' },
  'tar.gz': { format: 'paxr', filter: 'gzip' },
  tgz: { format: 'paxr', filter: 'gzip' },
  'tar.xz': { format: 'paxr', filter: 'xz' },
  'tar.bz2': { format: 'paxr', filter: 'bzip2' },
  'tar.zst': { format: 'paxr', filter: 'zstd' },
};
const COMPRESSED_ONLY = /\.(gz|bz2|xz|zst|lz4|lzma|z)$/i;          // one file, not an archive: x.txt.gz → x.txt

// "a.tar.gz" → "tar.gz", "a.7z" → "7z", "a" → ""
export function kindOf(name) {
  const n = String(name || '').toLowerCase();
  const two = n.match(/\.(tar\.(?:gz|xz|bz2|zst|lz4|lzma))$/);
  if (two) return two[1];
  const m = n.match(/\.([a-z0-9]{1,5})$/);
  return m ? m[1] : '';
}
export const isArchive = (name) => { const k = kindOf(name); return READ.includes(k) || k.startsWith('tar.'); };
export const canWrite = (name) => kindOf(name) in WRITE;

// The archive's own signature, so a renamed file still opens with the right reader.
export function detect(b) {
  const at = (o, ...s) => s.every((x, i) => b[o + i] === x);
  if (!b || b.length < 4) return '';
  if (at(0, 0x50, 0x4b, 3, 4) || at(0, 0x50, 0x4b, 5, 6) || at(0, 0x50, 0x4b, 7, 8)) return 'zip';
  if (at(0, 0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c)) return '7z';
  if (at(0, 0x52, 0x61, 0x72, 0x21, 0x1a, 0x07)) return 'rar';
  if (at(0, 0x1f, 0x8b)) return 'gz';
  if (at(0, 0x42, 0x5a, 0x68)) return 'bz2';
  if (at(0, 0xfd, 0x37, 0x7a, 0x58, 0x5a, 0)) return 'xz';
  if (at(0, 0x28, 0xb5, 0x2f, 0xfd)) return 'zst';
  if (at(0, 0x04, 0x22, 0x4d, 0x18)) return 'lz4';
  if (b.length > 262 && at(257, 0x75, 0x73, 0x74, 0x61, 0x72)) return 'tar';
  if (at(0, 0x4d, 0x53, 0x43, 0x46)) return 'cab';
  if (at(0, 0x21, 0x3c, 0x61, 0x72, 0x63, 0x68, 0x3e)) return 'ar';
  if (b.length > 0x8006 && at(0x8001, 0x43, 0x44, 0x30, 0x30, 0x31)) return 'iso';
  return '';
}

// The shared catalog key (core namespace) for an archive problem, or '' when it is the device's (the caller says it).
const KEYS = { 'not-zip': 'not_archive', 'not-archive': 'not_archive', corrupt: 'corrupt', encrypted: 'password', password: 'password',
  unsupported: 'unsupported', bomb: 'bomb', 'too-large': 'too_large', 'too-many': 'too_many', 'bad-name': 'bad_name',
  'not-found': 'not_found', aborted: 'aborted', exists: 'exists', 'no-engine': 'no_engine', memory: 'too_large', 'bad-input': 'corrupt' };
export function messageKey(err) {
  const k = err && KEYS[err.code];
  if (!k || (err.code === 'exists' && !(err instanceof Error))) return '';
  return 'zip_err_' + k;
}

const toBytes = async (x) => (x instanceof Uint8Array ? x : x instanceof ArrayBuffer ? new Uint8Array(x)
  : ArrayBuffer.isView(x) ? new Uint8Array(x.buffer, x.byteOffset, x.byteLength)
  : x && typeof x.arrayBuffer === 'function' ? new Uint8Array(await x.arrayBuffer()) : null);
const join = (a, b) => (a.replace(/\/+$/, '') + '/' + b).replace(/\/{2,}/g, '/');
const err = (code, detail) => Object.assign(new ZipError(code, detail), { code });
const msOf = (t) => { const n = Number(t); return n > 0 ? (n < 1e12 ? n * 1000 : n) : undefined; };

// ── the engine: a worker where the browser can, the page otherwise, anything with the same calls in the tests ──
// The same calls over a createEngine() object, for a page without module workers and for Node.
export function localEngine(eng) {
  const open = new Map();
  let seq = 0;
  return {
    list: async (bytes, opts) => eng.list(bytes, opts),
    open: async (bytes, opts) => { const rd = eng.reader(bytes, opts); open.set(++seq, rd); return { handle: seq, info: rd.info }; },
    next: async (h) => { const rd = open.get(h); if (!rd) throw err('aborted', 'reader gone'); const e = rd.next(); if (!e) open.delete(h); return e; },
    close: async (h) => { const rd = open.get(h); if (rd) rd.close(); open.delete(h); },
    write: async (files, opts) => eng.write(files, opts),
  };
}
function workerEngine() {
  let w = null, idle = 0, seq = 0, readers = 0;
  const waiting = new Map();
  const start = () => {
    if (w) return w;
    w = new Worker(new URL('./nucleo-pack-worker.js', import.meta.url), { type: 'module' });
    w.onmessage = (ev) => {
      const { id, ok, result, code, detail } = ev.data || {};
      const p = waiting.get(id); if (!p) return;
      waiting.delete(id);
      if (ok) p.resolve(result); else p.reject(err(code, detail));
    };
    w.onerror = (e) => { e.preventDefault && e.preventDefault(); stop(err('no-engine', String(e.message || 'worker failed'))); };
    return w;
  };
  // The worker holds the whole engine and the archive: gone a minute after the last call.
  const stop = (why) => { if (w) { w.terminate(); w = null; } readers = 0; for (const p of waiting.values()) p.reject(why || err('aborted')); waiting.clear(); };
  const call = (msg, transfer) => new Promise((resolve, reject) => {
    clearTimeout(idle);
    const id = ++seq;
    waiting.set(id, { resolve: (v) => { arm(); resolve(v); }, reject: (e) => { arm(); reject(e); } });
    start().postMessage({ id, ...msg }, transfer || []);
  });
  const arm = () => { clearTimeout(idle); idle = setTimeout(() => { if (!waiting.size && !readers) stop(); }, 60000); };
  return {
    list: (bytes, opts) => call({ op: 'list', bytes, opts }),
    open: async (bytes, opts) => { const r = await call({ op: 'open', bytes, opts }); readers++; return r; },
    next: (handle) => call({ op: 'next', handle }),
    close: async (handle) => { try { return await call({ op: 'close', handle }); } finally { readers = Math.max(0, readers - 1); arm(); } },
    write: (files, opts) => call({ op: 'write', files, opts }),
  };
}
async function pageEngine() {
  const [{ default: createLibarchive }, { createEngine }] = await Promise.all([
    import('./vendor/libarchive/libarchive.mjs'), import('./nucleo-pack-engine.js')]);
  return localEngine(createEngine(await createLibarchive()));
}
function defaultEngine() {
  let chosen = null;
  const pick = async () => {
    if (chosen) return chosen;
    try { if (typeof Worker === 'function') { const e = workerEngine(); await e.list(new Uint8Array(0)).catch((x) => { if (x.code === 'no-engine') throw x; }); return (chosen = e); } }
    catch { /* no module workers here: the page runs it */ }
    try { return (chosen = await pageEngine()); } catch (e) { throw err('no-engine', String(e && e.message || e)); }
  };
  const via = (fn) => async (...a) => fn(await pick(), ...a);
  return {
    list: via((e, ...a) => e.list(...a)), open: via((e, ...a) => e.open(...a)), next: via((e, ...a) => e.next(...a)),
    close: via((e, ...a) => e.close(...a)), write: via((e, ...a) => e.write(...a)),
  };
}

export function createPack({ engine } = {}) {
  const eng = engine || defaultEngine();
  const rawName = (name) => String(name || 'data').split('/').pop().replace(COMPRESSED_ONLY, '') || 'data';

  async function load(source, fs) {
    if (typeof source === 'string') {
      const b = await fs.read(source);
      if (!b) throw err('not-found', source);
      return { bytes: b, name: source.split('/').pop() };
    }
    const bytes = await toBytes(source);
    if (!bytes) throw err('bad-input');
    return { bytes, name: '' };
  }
  // ZIP without passwords stays in plain JavaScript.
  const viaZip = async (bytes, password) => detect(bytes) === 'zip' && !password && !(await zip.list(bytes).catch(() => [])).some((e) => e.encrypted);

  async function list(input, { name = '', password } = {}) {
    const bytes = await toBytes(input);
    if (!bytes) throw err('bad-input');
    if (detect(bytes) === 'zip' && !password) {
      try {
        const z = await zip.list(bytes);
        if (z.some((e) => e.encrypted)) throw Object.assign(new Error('encrypted'), { code: 'unsupported' });
        return { format: 'zip', encrypted: z.some((e) => e.encrypted),
          entries: z.map((e) => ({ name: e.name, path: safeName(e.name), size: e.size, dir: e.dir, mtime: e.mtime, encrypted: e.encrypted, unsafe: !safeName(e.name) })) };
      } catch (e) { if (e.code !== 'unsupported') throw e; }        // a method only libarchive knows (bzip2, lzma, AES…)
    }
    const r = await eng.list(bytes, { password });
    return { format: r.format, encrypted: r.encrypted,
      entries: r.entries.map((e) => {
        const n = r.raw ? rawName(name) : e.name;
        return { name: n, path: safeName(n), size: e.size, dir: e.type === 'dir', mtime: e.mtime, encrypted: e.encrypted, unsafe: !safeName(n) || (e.type !== 'file' && e.type !== 'dir'), type: e.type };
      }) };
  }

  // Entries one by one, through the engine; fn(entry) is awaited before the next is decompressed.
  // fn(entry) is awaited before the next is decompressed; it returns false to stop. The reader is always closed.
  async function walk(bytes, name, { password, limits = LIMITS, signal } = {}, fn) {
    const { handle, info } = await eng.open(bytes, { password, limits });
    try {
      for (;;) {
        if (signal && signal.aborted) throw err('aborted');
        const e = await eng.next(handle);
        if (!e) return info;
        if (info.raw) e.name = rawName(name);
        if ((await fn(e)) === false) return info;
      }
    } finally { await eng.close(handle).catch(() => {}); }
  }

  return {
    formats: { read: READ.slice(), write: Object.keys(WRITE) },
    kindOf, isArchive, canWrite, detect, messageKey, list,

    async *entries(input, { name = '', password, limits, signal } = {}) {
      const bytes = await toBytes(input);
      if (await viaZip(bytes, password)) { yield* zip.entries(bytes, { limits }); return; }
      const got = [];
      let done = false, fail = null, wake = null, stopped = false, held = null;
      // the engine pushes, the generator pulls: one entry waits at a time
      const pump = walk(bytes, name, { password, limits, signal }, (e) => (stopped ? false : new Promise((res) => { got.push([e, res]); if (wake) wake(); })))
        .then(() => { done = true; if (wake) wake(); }, (x) => { fail = x; done = true; if (wake) wake(); });
      try {
        for (;;) {
          if (!got.length && !done) await new Promise((r) => { wake = r; });
          wake = null;
          if (got.length) {
            const [e, res] = got.shift();
            held = res;                                               // released below, or in finally if the reader stops here
            const n = safeName(e.name);
            yield !n || (e.type !== 'file' && e.type !== 'dir') ? { name: e.name, skipped: 'unsafe' } : e.type === 'dir' ? { name: n, dir: true } : { name: n, dir: false, mtime: e.mtime, data: e.data };
            held = null;
            res(true);
            continue;
          }
          if (fail) throw fail;
          if (done) break;
        }
      } finally {
        // the consumer left early (break, return, a throw): let the engine stop and close its reader
        stopped = true;
        if (held) held(false);
        for (const [, res] of got.splice(0)) res(false);
        await pump;
      }
    },

    async read(input, entryName, opts = {}) {
      for await (const e of this.entries(input, opts)) if (!e.skipped && !e.dir && e.name === entryName) return e.data;
      throw err('not-found', entryName);
    },

    async extractTo(fs, source, destDir, { onProgress, overwrite = false, password, signal, limits = LIMITS, name } = {}) {
      const src = await load(source, fs);
      const label = name || src.name;
      if (await viaZip(src.bytes, password)) return zip.extractTo(fs, src.bytes, destDir, { onProgress, overwrite, signal, limits });
      const listed = await list(src.bytes, { name: label, password });
      if (listed.encrypted && !password) throw err('password');
      const total = listed.entries.reduce((t, e) => t + (e.dir || e.size < 0 ? 0 : e.size), 0);
      let done = 0, files = 0, folders = 0;
      const skipped = [];
      await fs.mkdirp(destDir);
      await walk(src.bytes, label, { password, limits, signal }, async (e) => {
        const n = safeName(e.name);
        if (!n || (e.type !== 'file' && e.type !== 'dir')) { skipped.push(e.name); return; }
        if (e.type === 'dir') { await fs.mkdirp(join(destDir, n)); folders++; return; }
        const target = await placeFor(fs, destDir, n, overwrite);
        await fs.write(target, e.data);
        files++; done += e.data.length;
        if (onProgress) onProgress(done, Math.max(total, done), n);
      });
      return { files, folders, skipped };
    },

    // [{ name, data, mtime?, dir? }] → the archive's bytes in `format` (a key of WRITE: 'zip', '7z', 'tar.xz'…)
    async create(files, { format = 'zip', password } = {}) {
      const w = WRITE[format];
      if (!w) throw err('unsupported', format);
      for (const f of files) if (!safeName(f.name)) throw err('bad-name', String(f.name));
      if (format === 'zip' && !password) return zip.create(files.map((f) => ({ ...f, name: f.dir && !f.name.endsWith('/') ? f.name + '/' : f.name })));
      const items = [];
      for (const f of files) {
        const dir = f.dir || f.name.endsWith('/');
        items.push({ name: f.name.replace(/\/+$/, ''), dir, mtime: f.mtime,
          data: dir ? null : f.data == null ? new Uint8Array(0) : typeof f.data === 'string' ? new TextEncoder().encode(f.data) : await toBytes(f.data) });
      }
      const opts = { ...w, password: password || '' };
      if (password && format === 'zip') opts.options = 'zip:encryption=aes256';
      // libarchive encrypts ZIP only (AES-256); a 7z or tar with a password would be written in clear
      if (password && format !== 'zip') throw err('unsupported', 'password for ' + format);
      return eng.write(items, opts);
    },

    // Pack device paths (files and folders, recursively) into destPath; the format comes from its name.
    async compressTo(fs, paths, destPath, { onProgress, signal, password, format } = {}) {
      const fmt = format || kindOf(destPath);
      if (!(fmt in WRITE)) throw err('unsupported', fmt);
      if (fmt === 'zip' && !password) return zip.compressTo(fs, paths, destPath, { onProgress, signal });
      const items = [];
      const walkFs = async (abs, rel) => {
        if (signal && signal.aborted) throw err('aborted');
        const st = await fs.stat(abs);
        if (!st) return;
        if (st.type === 'dir') {
          items.push({ name: rel, dir: true });
          for (const e of await fs.list(abs)) await walkFs(join(abs, e.name), rel + '/' + e.name);
        } else items.push({ name: rel, abs, mtime: msOf(st.mtime), size: st.size });
      };
      for (const p of paths) await walkFs(p, p.split('/').filter(Boolean).pop());
      const total = items.reduce((t, i) => t + (i.size || 0), 0);
      let done = 0;
      for (const i of items) {
        if (!i.abs) continue;
        if (signal && signal.aborted) throw err('aborted');
        i.data = await fs.read(i.abs);
        done += i.size || 0;
        if (onProgress) onProgress(done, total, i.name);
      }
      const bytes = await this.create(items, { format: fmt, password });
      await fs.write(destPath, bytes, { overwrite: false });
      return { path: destPath, files: items.filter((i) => i.abs).length, bytes: bytes.length };
    },
  };
}

const pack = createPack();
export default pack;

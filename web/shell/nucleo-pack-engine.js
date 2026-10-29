/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-pack-engine.js — libarchive (shell/vendor/libarchive, built by tools/libarchive-wasm) behind a few calls.
// It runs wherever the WebAssembly module is: in nucleo-pack-worker.js for the OS, in Node for the tests. Apps never
// import it: they use nucleo-pack.js, which adds the device (nucleo-fs.js) and the rules for names.
//
//   const eng = createEngine(await createLibarchive());
//   eng.list(bytes, { password })     → { format, filter, raw, encrypted, entries: [{ name, size, type, mtime, encrypted }] }
//   const rd = eng.reader(bytes, { password, limits });   rd.info → { format, filter, raw }
//   rd.next()                         → { name, size, type, mtime, encrypted, data } or null at the end; rd.close()
//   eng.write(files, { format, filter, password, options }) → Uint8Array
//
// One entry is in memory at a time, and the caller asks for the next one when it has dealt with this one. Bombs are
// stopped while reading, never trusted from headers: an entry may not grow past limits.entry, the archive past
// limits.total, and past 64 MB the output may not exceed limits.ratio times the input.

export const LIMITS = { entries: 65535, total: 4 * 1024 * 1024 * 1024, entry: 2 * 1024 * 1024 * 1024, ratio: 1000 };
const CHUNK = 1 << 20;
const TYPE = { 0o100000: 'file', 0o040000: 'dir', 0o120000: 'link' };
const FATAL = -20;                                   // below ARCHIVE_WARN: ARCHIVE_FAILED (-25), ARCHIVE_FATAL (-30)

export class PackError extends Error {
  constructor(code, detail) { super(code + (detail ? ': ' + detail : '')); this.code = code; this.detail = detail || ''; }
}

// libarchive's words → the OS's codes (the same vocabulary as nucleo-zip.js, so one catalog explains both)
function codeOf(msg, encrypted) {
  const m = String(msg || '');
  if (/passphrase|password|decrypt/i.test(m) || encrypted) return 'password';
  if (/unrecognized archive format/i.test(m)) return 'not-archive';
  if (/unsupported|not supported/i.test(m)) return 'unsupported';
  return 'corrupt';
}

export function createEngine(M) {
  // Memory may grow but is finite: a failed allocation is an error, never a write at address 0.
  const alloc = (n) => { const p = M._malloc(Math.max(1, n)); if (!p) throw new PackError('memory'); return p; };
  const str = (s) => { const n = M.lengthBytesUTF8(s) + 1; const p = alloc(n); M.stringToUTF8(s, p, n); return p; };
  // a password leaves no copy behind in the module's memory
  const wipe = (p) => { if (p) { let n = 0; while (M.HEAPU8[p + n]) n++; M.HEAPU8.fill(0, p, p + n); } M._free(p); };
  const text = (p) => (p ? M.UTF8ToString(p) : '');
  const heap = () => M.HEAPU8;
  const fail = (r, encrypted) => { const msg = text(M._na_error(r)); return new PackError(codeOf(msg, encrypted), msg); };

  // The archive opened on its first entry: every archive format first, then (raw) one compressed file such as
  // .gz or .xz, read as a single entry — never plain bytes, which would "open" as anything.
  function start(bytes, password) {
    const buf = alloc(bytes.length);
    heap().set(bytes, buf);
    let pass;
    try { pass = str(password || ''); } catch (e) { M._free(buf); throw e; }
    const free = () => { wipe(pass); M._free(buf); };
    try {
      for (const raw of [0, 1]) {
        const r = M._na_open(buf, bytes.length, pass, raw);
        if (!r) throw new PackError('memory');
        let keep = false;
        try {
          if (M._na_open_status(r) < FATAL) { if (raw) throw fail(r); continue; }
          const first = M._na_next(r);
          if (raw && (first !== 1 || text(M._na_filter(r)) === 'none')) throw new PackError('not-archive');
          if (first < 0 && !raw) {
            const e = fail(r);
            if (e.code === 'not-archive') continue;
            throw e;
          }
          keep = true;
          return { r, raw: !!raw, first, close: () => { M._na_close(r); free(); } };
        } finally { if (!keep) M._na_close(r); }
      }
      throw new PackError('not-archive');
    } catch (e) { free(); throw e; }
  }
  const meta = (r) => ({ name: text(M._na_path(r)), size: M._na_size(r), type: TYPE[M._na_type(r)] || 'other',
    mtime: M._na_mtime(r) * 1000 || 0, encrypted: !!M._na_encrypted(r) });
  const info = (h) => ({ format: text(M._na_format(h.r)), filter: text(M._na_filter(h.r)), raw: h.raw });

  function reader(bytes, { password, limits = LIMITS } = {}) {
    const h = start(bytes, password);
    let out;
    try { out = alloc(CHUNK); } catch (e) { h.close(); throw e; }
    let state = h.first, count = 0, total = 0, closed = false;
    const close = () => { if (!closed) { closed = true; M._free(out); h.close(); } };
    return {
      info: info(h),
      next() {
        if (closed) return null;
        try {
          if (state === 0) { close(); return null; }
          if (state < 0) throw fail(h.r);
          if (++count > limits.entries) throw new PackError('too-many', String(count));
          const e = meta(h.r);
          let data = null;
          if (e.type !== 'file') M._na_skip(h.r);
          else {
            const parts = [];
            let size = 0;
            for (;;) {
              const n = M._na_read(h.r, out, CHUNK);
              if (n === 0) break;
              if (n < 0) throw fail(h.r, e.encrypted);
              size += n; total += n;
              if (size > limits.entry) throw new PackError('too-large', e.name);
              if (total > limits.total) throw new PackError('too-large', 'total');
              if (total > 64 * 1024 * 1024 && total > bytes.length * limits.ratio) throw new PackError('bomb', e.name);
              parts.push(heap().slice(out, out + n));
            }
            data = parts.length === 1 ? parts[0] : new Uint8Array(size);
            if (parts.length !== 1) { let p = 0; for (const c of parts) { data.set(c, p); p += c.length; } }
            e.size = size;
          }
          state = M._na_next(h.r);
          return { ...e, data };
        } catch (err) { close(); throw err; }
      },
      close,
    };
  }

  return {
    version: () => text(M._na_version()),
    reader,
    list(bytes, { password } = {}) {
      const h = start(bytes, password);
      try {
        const entries = [];
        for (let s = h.first; s !== 0; s = M._na_next(h.r)) {
          if (s < 0) throw fail(h.r);
          if (entries.length >= LIMITS.entries) throw new PackError('too-many');
          entries.push(meta(h.r));
          M._na_skip(h.r);
        }
        return { ...info(h), encrypted: entries.some((e) => e.encrypted), entries };
      } finally { h.close(); }
    },
    write(files, { format, filter = 'none', password = '', options = '' } = {}) {
      const args = [format, filter, password || '', options || ''].map(str);
      const w = M._nw_open(...args);
      args.forEach(wipe);
      if (!w) throw new PackError('memory');
      try {
        if (M._nw_status(w) < FATAL) throw new PackError('unsupported', text(M._nw_error(w)));
        for (const file of files) {
          const p = str(file.name);
          const len = file.data ? file.data.length : 0;
          let d = 0;
          try { d = len ? alloc(len) : 0; } catch (e) { M._free(p); throw e; }
          if (len) heap().set(file.data, d);
          const s = M._nw_add(w, p, d, len, (file.mtime || 0) / 1000, file.dir ? 1 : 0);
          M._free(p); if (d) M._free(d);
          if (s < 0) throw new PackError('corrupt', text(M._nw_error(w)) || file.name);
        }
        if (M._nw_close(w) < 0) throw new PackError('corrupt', text(M._nw_error(w)));
        const ptr = M._nw_data(w), n = M._nw_len(w);
        return heap().slice(ptr, ptr + n);
      } finally { M._nw_free(w); }
    },
  };
}

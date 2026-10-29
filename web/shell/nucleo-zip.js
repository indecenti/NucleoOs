/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-zip.js — ZIP archives for the whole OS: read, extract, create. No dependency, every browser. Guide: docs/files.md.
//
//   import zip from '/nucleo-zip.js';
//   const entries = await zip.list(bytes);                         // [{ name, size, csize, dir, mtime, encrypted }]
//   for await (const f of zip.entries(bytes)) …                    // { name, dir, data: Uint8Array }
//   const bytes = await zip.create([{ name: 'a/b.txt', data }]);    // Uint8Array, deflated where it helps
//   await zip.extractTo(fs, '/data/x.zip', '/data/x', { onProgress, overwrite: false });   // straight onto the device
//   await zip.compressTo(fs, ['/data/a.txt', '/data/Photos'], '/data/out.zip', { onProgress });
//
// Why in the core: Archive Manager carried a 100 KB library, DOS Importer its own unzip that needed
// DecompressionStream (absent on older Safari and Firefox), File Commander sent the user to another app. One module
// now does it for everyone, the same on every edition: it runs in the browser and reaches the device only through
// nucleo-fs.js. Deflate uses the browser's own streams where they exist and a small inflater of its own where they
// do not; writing falls back to stored entries when the browser cannot deflate.
//
// Safe by construction: a name that would land outside the destination (absolute, "..", a drive letter, a
// backslash trick) is refused; extraction stops past a total size or a compression ratio no real archive has
// (zip bombs); encrypted entries are reported, never written as garbage; every entry's CRC-32 is checked.

export class ZipError extends Error {
  constructor(code, detail) { super(code + (detail ? ': ' + detail : '')); this.code = code; this.detail = detail || ''; }
}
export const LIMITS = { entries: 65535, total: 4 * 1024 * 1024 * 1024, ratio: 1000, entry: 2 * 1024 * 1024 * 1024 };

// ── CRC-32 ──
const CRC = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
export function crc32(u8, crc = 0) {
  let c = ~crc >>> 0;
  for (let i = 0; i < u8.length; i++) c = CRC[(c ^ u8[i]) & 0xff] ^ (c >>> 8);
  return ~c >>> 0;
}

// ── inflate (RFC 1951), for browsers without DecompressionStream('deflate-raw') ──
const LBASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
const LEXT = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
const DBASE = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577];
const DEXT = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13];
const CLORDER = [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15];
function huffman(lengths) {                       // canonical code → { counts, symbols }
  const counts = new Uint16Array(16), offs = new Uint16Array(16), symbols = new Uint16Array(lengths.length);
  for (const l of lengths) counts[l]++;
  counts[0] = 0;
  for (let i = 1; i < 16; i++) offs[i] = offs[i - 1] + counts[i - 1];
  for (let s = 0; s < lengths.length; s++) if (lengths[s]) symbols[offs[lengths[s]]++] = s;
  return { counts, symbols };
}
export function inflateRaw(src, expected = 0, maxOut = LIMITS.entry) {
  let pos = 0, bit = 0, bits = 0;
  // grown as the data comes: a header may claim 2 GB for a few bytes
  let out = new Uint8Array(Math.max(1024, Math.min(expected > 0 ? expected : Infinity, src.length * 4, 1 << 24)));
  let len = 0;
  const need = (n) => {
    while (bits < n) { if (pos >= src.length) throw new ZipError('corrupt', 'inflate past the end'); bit |= src[pos++] << bits; bits += 8; }
  };
  const take = (n) => { need(n); const v = bit & ((1 << n) - 1); bit >>>= n; bits -= n; return v; };
  const grow = (n) => {
    if (len + n <= out.length) return;
    if (len + n > maxOut) throw new ZipError(maxOut === expected ? 'corrupt' : 'too-large', 'entry larger than ' + maxOut);
    let size = out.length * 2; while (size < len + n) size *= 2;
    const o = new Uint8Array(Math.min(size, maxOut)); o.set(out.subarray(0, len)); out = o;
  };
  const decode = (h) => {
    let code = 0, first = 0, index = 0;
    for (let l = 1; l < 16; l++) {
      code |= take(1);
      const c = h.counts[l];
      if (code - c < first) return h.symbols[index + (code - first)];
      index += c; first += c; first <<= 1; code <<= 1;
    }
    throw new ZipError('corrupt', 'bad huffman code');
  };
  let fixedL = null, fixedD = null;
  for (let last = 0; !last;) {
    last = take(1);
    const type = take(2);
    if (type === 0) {
      bit = 0; bits = 0;                                      // to a byte boundary
      if (pos + 4 > src.length) throw new ZipError('corrupt', 'stored header');
      const n = src[pos] | (src[pos + 1] << 8);
      pos += 4;
      if (pos + n > src.length) throw new ZipError('corrupt', 'stored block');
      grow(n); out.set(src.subarray(pos, pos + n), len); len += n; pos += n;
      continue;
    }
    let lh, dh;
    if (type === 1) {
      if (!fixedL) {
        const l = new Uint8Array(288); l.fill(8, 0, 144); l.fill(9, 144, 256); l.fill(7, 256, 280); l.fill(8, 280, 288);
        fixedL = huffman(l); fixedD = huffman(new Uint8Array(30).fill(5));
      }
      lh = fixedL; dh = fixedD;
    } else if (type === 2) {
      const hlit = take(5) + 257, hdist = take(5) + 1, hclen = take(4) + 4;
      const cl = new Uint8Array(19);
      for (let i = 0; i < hclen; i++) cl[CLORDER[i]] = take(3);
      const ch = huffman(cl);
      const lens = new Uint8Array(hlit + hdist);
      for (let i = 0; i < hlit + hdist;) {
        const sym = decode(ch);
        if (sym < 16) lens[i++] = sym;
        else if (sym === 16) { if (!i) throw new ZipError('corrupt', 'repeat at start'); const p = lens[i - 1]; for (let r = 3 + take(2); r--;) lens[i++] = p; }
        else if (sym === 17) i += 3 + take(3);
        else i += 11 + take(7);
      }
      lh = huffman(lens.subarray(0, hlit)); dh = huffman(lens.subarray(hlit));
    } else throw new ZipError('corrupt', 'block type 3');
    for (;;) {
      const sym = decode(lh);
      if (sym < 256) { grow(1); out[len++] = sym; continue; }
      if (sym === 256) break;
      const li = sym - 257;
      if (li >= 29) throw new ZipError('corrupt', 'length code');
      const n = LBASE[li] + take(LEXT[li]);
      const di = decode(dh);
      if (di >= 30) throw new ZipError('corrupt', 'distance code');
      const d = DBASE[di] + take(DEXT[di]);
      if (d > len) throw new ZipError('corrupt', 'distance too far');
      grow(n);
      for (let k = 0; k < n; k++, len++) out[len] = out[len - d];
    }
  }
  return out.subarray(0, len);
}

// ── deflate/inflate through the browser's streams when it has them ──
const streams = {};
const hasStream = (kind) => (kind in streams ? streams[kind] : (streams[kind] = typeof globalThis[kind] === 'function' &&
  (() => { try { new globalThis[kind]('deflate-raw'); return true; } catch { return false; } })()));
// The whole output of `stream` over `u8`, failing past `max` bytes: an entry that lies about its size in the header
// cannot fill the memory before the checksum would catch it.
async function pipe(u8, stream, max = Infinity) {
  const reader = new Blob([u8]).stream().pipeThrough(stream).getReader();
  const chunks = [];
  let n = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    n += value.length;
    if (n > max) { try { await reader.cancel(); } catch {} throw new ZipError('corrupt', 'entry larger than declared'); }
    chunks.push(value);
  }
  if (chunks.length === 1) return chunks[0];
  const out = new Uint8Array(n);
  let p = 0; for (const c of chunks) { out.set(c, p); p += c.length; }
  return out;
}
async function inflate(u8, expected) {
  if (hasStream('DecompressionStream')) {
    try { return await pipe(u8, new DecompressionStream('deflate-raw'), expected); }
    catch (e) { if (e instanceof ZipError) throw e; throw new ZipError('corrupt', 'deflate data'); }
  }
  return inflateRaw(u8, expected, expected);
}
async function deflate(u8) {
  if (!hasStream('CompressionStream')) return null;            // stored instead: always readable, just bigger
  try { return await pipe(u8, new CompressionStream('deflate-raw')); } catch { return null; }
}

// ── reading ──
const dv = (u8) => new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
const dec = new TextDecoder();
const enc = new TextEncoder();
function cp437(bytes) { let s = ''; for (const b of bytes) s += b < 128 ? String.fromCharCode(b) : '�'; return s; }
function dosTime(time, date) {
  return new Date(1980 + (date >> 9), ((date >> 5) & 15) - 1, date & 31, time >> 11, (time >> 5) & 63, (time & 31) * 2).getTime();
}
async function toBytes(input) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  if (input && typeof input.arrayBuffer === 'function') return new Uint8Array(await input.arrayBuffer());
  throw new ZipError('bad-input', 'bytes or a Blob');
}

// A name inside the archive, made safe to join under a destination folder; null when it cannot be.
export function safeName(name) {
  const n = String(name).replace(/\\/g, '/');
  if (!n || n.startsWith('/') || /^[A-Za-z]:/.test(n) || n.includes('\0')) return null;
  const parts = [];
  for (const seg of n.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') return null;
    if (/[<>:"|?*\u0000-\u001f]/.test(seg)) return null;
    const clean = seg.replace(/[. ]+$/, '');            // FAT and Windows drop them: "a.txt." IS "a.txt"
    if (!clean || clean === '..') return null;
    parts.push(clean);
  }
  return parts.length ? parts.join('/') + (n.endsWith('/') ? '/' : '') : null;
}

function directory(u8) {
  const v = dv(u8);
  let eocd = -1;
  for (let i = u8.length - 22; i >= Math.max(0, u8.length - 22 - 65535); i--) if (v.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new ZipError('not-zip', 'no end of central directory');
  let count = v.getUint16(eocd + 10, true), cdSize = v.getUint32(eocd + 12, true), cdOff = v.getUint32(eocd + 16, true);
  if (cdOff === 0xffffffff || count === 0xffff) {              // ZIP64
    const loc = eocd - 20;
    if (loc < 0 || v.getUint32(loc, true) !== 0x07064b50) throw new ZipError('corrupt', 'zip64 locator');
    const z = Number(v.getBigUint64(loc + 8, true));
    if (v.getUint32(z, true) !== 0x06064b50) throw new ZipError('corrupt', 'zip64 record');
    count = Number(v.getBigUint64(z + 32, true)); cdSize = Number(v.getBigUint64(z + 40, true)); cdOff = Number(v.getBigUint64(z + 48, true));
  }
  if (count > LIMITS.entries) throw new ZipError('too-many', String(count));
  if (cdOff + cdSize > u8.length) throw new ZipError('corrupt', 'central directory outside the file');
  const out = [];
  let p = cdOff;
  for (let i = 0; i < count; i++) {
    if (v.getUint32(p, true) !== 0x02014b50) throw new ZipError('corrupt', 'central entry');
    const flags = v.getUint16(p + 8, true), method = v.getUint16(p + 10, true);
    const time = v.getUint16(p + 12, true), date = v.getUint16(p + 14, true);
    let crc = v.getUint32(p + 16, true), csize = v.getUint32(p + 20, true), size = v.getUint32(p + 24, true);
    const nl = v.getUint16(p + 28, true), el = v.getUint16(p + 30, true), cl = v.getUint16(p + 32, true);
    let off = v.getUint32(p + 42, true);
    const raw = u8.subarray(p + 46, p + 46 + nl);
    const name = flags & 0x800 ? dec.decode(raw) : (raw.every((b) => b < 128) ? dec.decode(raw) : cp437(raw));
    let e = p + 46 + nl;
    const eend = e + el;
    while (e + 4 <= eend) {                                    // the ZIP64 extra field carries the real sizes
      const id = v.getUint16(e, true), sz = v.getUint16(e + 2, true);
      if (id === 0x0001) {
        let q = e + 4;
        if (size === 0xffffffff) { size = Number(v.getBigUint64(q, true)); q += 8; }
        if (csize === 0xffffffff) { csize = Number(v.getBigUint64(q, true)); q += 8; }
        if (off === 0xffffffff) { off = Number(v.getBigUint64(q, true)); }
      }
      e += 4 + sz;
    }
    out.push({ name, size, csize, method, crc, off, flags, dir: name.endsWith('/'), mtime: dosTime(time, date), encrypted: !!(flags & 1) });
    p = eend + cl;
  }
  return out;
}

async function dataOf(u8, ent) {
  const v = dv(u8);
  if (v.getUint32(ent.off, true) !== 0x04034b50) throw new ZipError('corrupt', 'local header of ' + ent.name);
  const nl = v.getUint16(ent.off + 26, true);
  const local = u8.subarray(ent.off + 30, ent.off + 30 + nl);
  const want = enc.encode(ent.name);
  const same = local.length === want.length ? local.every((b, i) => b === want[i]) : cp437(local) === ent.name || dec.decode(local) === ent.name;
  if (!same) throw new ZipError('corrupt', 'local name differs from the directory: ' + ent.name);
  const start = ent.off + 30 + nl + v.getUint16(ent.off + 28, true);
  if (start + ent.csize > u8.length) throw new ZipError('corrupt', 'data past the end: ' + ent.name);
  const comp = u8.subarray(start, start + ent.csize);
  if (ent.encrypted) throw new ZipError('encrypted', ent.name);
  let data;
  if (ent.method === 0) data = comp;
  else if (ent.method === 8) data = await inflate(comp, ent.size);
  else throw new ZipError('unsupported', 'method ' + ent.method + ' (' + ent.name + ')');
  if (data.length !== ent.size || crc32(data) !== ent.crc) throw new ZipError('corrupt', 'checksum of ' + ent.name);
  return data;
}

// Bombs: an archive whose entries would unpack beyond the total, or squeeze more than `ratio`:1, is refused
// before a byte is written.
function guard(list, limits) {
  let total = 0;
  for (const e of list) {
    if (e.dir) continue;
    total += e.size;
    if (e.size > limits.entry) throw new ZipError('too-large', e.name);
    if (e.size > 0 && e.csize === 0) throw new ZipError('bomb', e.name);
    if (e.csize > 0 && e.size / e.csize > limits.ratio && e.size > 1024 * 1024) throw new ZipError('bomb', e.name);
  }
  if (total > limits.total) throw new ZipError('too-large', 'total ' + total);
  return total;
}

// ── writing ──
function dosStamp(ms) {
  const d = new Date(ms || Date.now());
  const y = Math.max(1980, d.getFullYear());
  return { time: (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1), date: ((y - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate() };
}
async function build(files, { level = 'auto' } = {}) {
  const parts = [], central = [];
  let offset = 0;
  for (const f of files) {
    const name = safeName(f.name);
    if (!name) throw new ZipError('bad-name', String(f.name));
    const isDir = name.endsWith('/');
    const data = isDir ? new Uint8Array(0) : await toBytes(f.data == null ? new Uint8Array(0) : (typeof f.data === 'string' ? enc.encode(f.data) : f.data));
    const crc = crc32(data);
    let method = 0, body = data;
    if (!isDir && level !== 'store' && data.length > 64) {
      const z = await deflate(data);
      if (z && z.length < data.length) { method = 8; body = z; }
    }
    const nameBytes = enc.encode(name);
    const { time, date } = dosStamp(f.mtime);
    const big = data.length >= 0xffffffff || body.length >= 0xffffffff || offset >= 0xffffffff;
    const h = new Uint8Array(30 + nameBytes.length); const hv = dv(h);
    hv.setUint32(0, 0x04034b50, true); hv.setUint16(4, big ? 45 : 20, true); hv.setUint16(6, 0x800, true); hv.setUint16(8, method, true);
    hv.setUint16(10, time, true); hv.setUint16(12, date, true); hv.setUint32(14, crc, true);
    hv.setUint32(18, big ? 0xffffffff : body.length, true); hv.setUint32(22, big ? 0xffffffff : data.length, true);
    hv.setUint16(26, nameBytes.length, true); h.set(nameBytes, 30);
    if (big) throw new ZipError('too-large', 'archives over 4 GB are read, not written, here');
    parts.push(h, body);
    central.push({ nameBytes, method, time, date, crc, csize: body.length, size: data.length, offset, isDir });
    offset += h.length + body.length;
  }
  const cdStart = offset;
  for (const c of central) {
    const h = new Uint8Array(46 + c.nameBytes.length); const hv = dv(h);
    hv.setUint32(0, 0x02014b50, true); hv.setUint16(4, 0x0314, true); hv.setUint16(6, 20, true); hv.setUint16(8, 0x800, true);
    hv.setUint16(10, c.method, true); hv.setUint16(12, c.time, true); hv.setUint16(14, c.date, true); hv.setUint32(16, c.crc, true);
    hv.setUint32(20, c.csize, true); hv.setUint32(24, c.size, true); hv.setUint16(28, c.nameBytes.length, true);
    hv.setUint32(38, c.isDir ? (0o40755 << 16) | 0x10 : (0o100644 << 16) >>> 0, true); hv.setUint32(42, c.offset, true);
    h.set(c.nameBytes, 46);
    parts.push(h); offset += h.length;
  }
  const e = new Uint8Array(22); const ev = dv(e);
  ev.setUint32(0, 0x06054b50, true); ev.setUint16(8, central.length, true); ev.setUint16(10, central.length, true);
  ev.setUint32(12, offset - cdStart, true); ev.setUint32(16, cdStart, true);
  parts.push(e);
  const out = new Uint8Array(offset + 22);
  let p = 0; for (const x of parts) { out.set(x, p); p += x.length; }
  return out;
}

// Devices report mtime in seconds or milliseconds, or not at all.
const msOf = (t) => { const n = Number(t); return n > 0 ? (n < 1e12 ? n * 1000 : n) : undefined; };
// Where an extracted file goes. `overwrite`: false (never replace: "x (2).txt"), true, or a Set of the names the
// user confirmed (clashKey of each). An existing file is found whatever its case: FAT cards are case-blind.
export const clashKey = (name) => String(name).toLowerCase();
export async function placeFor(fs, destDir, name, overwrite) {
  const target = join(destDir, name);
  if (overwrite === true) return target;
  const dir = target.replace(/\/[^/]*$/, '') || '/';
  const base = target.split('/').pop();
  const have = new Set((await fs.list(dir).catch(() => [])).map((e) => clashKey(e.name)));
  if (!have.has(clashKey(base))) return target;
  if (overwrite instanceof Set && overwrite.has(clashKey(name))) {
    const real = (await fs.list(dir)).find((e) => clashKey(e.name) === clashKey(base));
    return join(dir, real ? real.name : base);                      // replace the file that is there, not a twin
  }
  return fs.uniquePath(dir, base);
}

const join = (a, b) => (a.replace(/\/+$/, '') + '/' + b).replace(/\/{2,}/g, '/');

// The shared catalog key (core namespace, every app has it) that explains a failure to the user, or '' when the
// error is not the archive's (a device error: the caller says it its own way).
export function messageKey(err) {
  const code = err && err.code;
  if (!(err instanceof ZipError) && code !== 'exists') return '';
  return 'zip_err_' + ({ 'not-zip': 'not_zip', 'too-large': 'too_large', 'too-many': 'too_many', 'bad-name': 'bad_name',
    'not-found': 'not_found', 'bad-input': 'corrupt' }[code] || code);
}

const zip = {
  ZipError, LIMITS, crc32, inflateRaw, safeName, messageKey, clashKey,
  // The archive's table of contents. Never decompresses anything.
  async list(input) {
    const u8 = await toBytes(input);
    return directory(u8).map(({ name, size, csize, dir, mtime, encrypted, method }) => ({ name, size, csize, dir, mtime, encrypted, method }));
  },
  // Every entry with its bytes, one at a time (CRC checked). Unsafe names are skipped and reported in `skipped`.
  async *entries(input, { limits = LIMITS } = {}) {
    const u8 = await toBytes(input);
    const list = directory(u8);
    guard(list, limits);
    for (const e of list) {
      const name = safeName(e.name);
      if (!name) { yield { name: e.name, skipped: 'unsafe' }; continue; }
      if (e.dir) { yield { name, dir: true }; continue; }
      yield { name, dir: false, mtime: e.mtime, data: await dataOf(u8, e) };
    }
  },
  // One entry's bytes, by name.
  async read(input, name) {
    const u8 = await toBytes(input);
    const e = directory(u8).find((x) => x.name === name);
    if (!e) throw new ZipError('not-found', name);
    guard([e], LIMITS);
    return dataOf(u8, e);
  },
  // [{ name, data: bytes|string|Blob, mtime? }] → the archive's bytes. Folders: a name ending in "/".
  create: (files, opts) => build(files, opts),
  // Unpack an archive (a device path or bytes) into destDir, through the core file layer. Existing files are kept
  // (renamed "x (2).txt") unless overwrite. → { files, folders, skipped: [names] }
  async extractTo(fs, source, destDir, { onProgress, overwrite = false, signal, limits = LIMITS } = {}) {
    const u8 = typeof source === 'string' ? await fs.read(source) : await toBytes(source);
    if (!u8) throw new ZipError('not-found', String(source));
    const list = directory(u8);
    const total = guard(list, limits);
    let done = 0, files = 0, folders = 0;
    const skipped = [];
    await fs.mkdirp(destDir);
    for (const e of list) {
      if (signal && signal.aborted) throw new ZipError('aborted');
      const name = safeName(e.name);
      if (!name) { skipped.push(e.name); continue; }
      if (e.dir) { await fs.mkdirp(join(destDir, name)); folders++; continue; }
      if (e.encrypted) { skipped.push(e.name); continue; }
      const data = await dataOf(u8, e);
      const target = await placeFor(fs, destDir, name, overwrite);
      await fs.write(target, data);
      files++; done += e.size;
      if (onProgress) onProgress(done, total, name);
    }
    return { files, folders, skipped };
  },
  // Pack device paths (files and whole folders, recursively) into one archive on the device.
  async compressTo(fs, paths, zipPath, { onProgress, signal } = {}) {
    const items = [];
    const walk = async (abs, rel) => {
      if (signal && signal.aborted) throw new ZipError('aborted');
      const st = await fs.stat(abs);
      if (!st) return;
      if (st.type === 'dir') {
        items.push({ name: rel + '/', data: null });
        for (const e of await fs.list(abs)) await walk(join(abs, e.name), rel + '/' + e.name);
      } else items.push({ name: rel, abs, mtime: msOf(st.mtime), size: st.size });
    };
    for (const p of paths) await walk(p, p.split('/').filter(Boolean).pop());
    const total = items.reduce((t, i) => t + (i.size || 0), 0);
    let done = 0;
    for (const i of items) {
      if (!i.abs) continue;
      if (signal && signal.aborted) throw new ZipError('aborted');
      i.data = await fs.read(i.abs);
      done += i.size || 0;
      if (onProgress) onProgress(done, total, i.name);
    }
    const bytes = await build(items);
    await fs.write(zipPath, bytes, { overwrite: false });
    return { path: zipPath, files: items.filter((i) => i.abs).length, bytes: bytes.length };
  },
};
export default zip;

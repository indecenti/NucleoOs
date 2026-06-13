// Minimal, zero-dependency .jsdos bundle builder.
//
// A .jsdos bundle is just a ZIP whose root holds the game files plus a
// `.jsdos/dosbox.conf` describing what to run. js-dos (DOSBox WASM) loads it in the
// browser; the NucleoOS device only stores and streams the bytes from SD.
//
// This writer uses the ZIP "store" method (no compression) and forward-slash entry
// names, so the bundles work cross-platform (PowerShell 5.1's Compress-Archive emits
// backslash entry names, which js-dos mis-reads — hence this tool).
//
// Usage:  node tools/make-jsdos.mjs <sourceDir> <output.jsdos>
import { readdirSync, statSync, readFileSync, writeFileSync } from 'node:fs';
import { join, relative, sep } from 'node:path';

// CRC-32 (IEEE 802.3) — table built once.
const CRC = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

function walk(dir, base = dir, out = []) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    const st = statSync(p);
    if (st.isDirectory()) walk(p, base, out);
    else out.push(relative(base, p).split(sep).join('/'));   // always forward slashes
  }
  return out;
}

// Low-level ZIP "store" writer. `entries` = [{ name, data:Buffer }] with forward-slash
// names. Shared by makeJsdos() (packs a folder) and add-dos.mjs (packs files + a
// generated .jsdos/dosbox.conf). Deterministic order so identical inputs => identical bytes.
export function packStoreToFile(outFile, entries) {
  const sorted = [...entries].sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
  const locals = [], central = [];
  let offset = 0;

  for (const { name, data } of sorted) {
    const nameBuf = Buffer.from(name, 'utf8');
    const crc = crc32(data);

    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0);     // local file header signature
    lh.writeUInt16LE(20, 4);             // version needed
    lh.writeUInt16LE(0, 6);              // flags
    lh.writeUInt16LE(0, 8);              // method: store
    lh.writeUInt16LE(0, 10);             // mod time
    lh.writeUInt16LE(0x21, 12);          // mod date (1980-01-01)
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(data.length, 18);   // compressed size
    lh.writeUInt32LE(data.length, 22);   // uncompressed size
    lh.writeUInt16LE(nameBuf.length, 26);
    lh.writeUInt16LE(0, 28);             // extra len
    locals.push(lh, nameBuf, data);

    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0);     // central dir header signature
    ch.writeUInt16LE(20, 4); ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(0, 8); ch.writeUInt16LE(0, 10);
    ch.writeUInt16LE(0, 12); ch.writeUInt16LE(0x21, 14);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(data.length, 20); ch.writeUInt32LE(data.length, 24);
    ch.writeUInt16LE(nameBuf.length, 28);
    ch.writeUInt32LE(offset, 42);        // local header offset
    central.push(ch, nameBuf);

    offset += lh.length + nameBuf.length + data.length;
  }

  const localBuf = Buffer.concat(locals);
  const centralBuf = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(sorted.length, 8);
  end.writeUInt16LE(sorted.length, 10);
  end.writeUInt32LE(centralBuf.length, 12);
  end.writeUInt32LE(localBuf.length, 16);    // central dir offset
  writeFileSync(outFile, Buffer.concat([localBuf, centralBuf, end]));
  return { files: sorted.length, bytes: localBuf.length + centralBuf.length + end.length };
}

export function makeJsdos(srcDir, outFile) {
  const names = walk(srcDir).sort();
  const locals = [], central = [];
  let offset = 0;

  for (const name of names) {
    const data = readFileSync(join(srcDir, name));
    const nameBuf = Buffer.from(name, 'utf8');
    const crc = crc32(data);

    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0);     // local file header signature
    lh.writeUInt16LE(20, 4);             // version needed
    lh.writeUInt16LE(0, 6);              // flags
    lh.writeUInt16LE(0, 8);              // method: store
    lh.writeUInt16LE(0, 10);             // mod time
    lh.writeUInt16LE(0x21, 12);          // mod date (1980-01-01)
    lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(data.length, 18);   // compressed size
    lh.writeUInt32LE(data.length, 22);   // uncompressed size
    lh.writeUInt16LE(nameBuf.length, 26);
    lh.writeUInt16LE(0, 28);             // extra len
    locals.push(lh, nameBuf, data);

    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0);     // central dir header signature
    ch.writeUInt16LE(20, 4); ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(0, 8); ch.writeUInt16LE(0, 10);
    ch.writeUInt16LE(0, 12); ch.writeUInt16LE(0x21, 14);
    ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(data.length, 20); ch.writeUInt32LE(data.length, 24);
    ch.writeUInt16LE(nameBuf.length, 28);
    ch.writeUInt32LE(offset, 42);        // local header offset
    central.push(ch, nameBuf);

    offset += lh.length + nameBuf.length + data.length;
  }

  const localBuf = Buffer.concat(locals);
  const centralBuf = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(names.length, 8);
  end.writeUInt16LE(names.length, 10);
  end.writeUInt32LE(centralBuf.length, 12);
  end.writeUInt32LE(localBuf.length, 16);    // central dir offset
  writeFileSync(outFile, Buffer.concat([localBuf, centralBuf, end]));
  return { files: names.length, bytes: localBuf.length + centralBuf.length + end.length };
}

// CLI
if (import.meta.url === `file://${process.argv[1].split(sep).join('/')}` || process.argv[1].endsWith('make-jsdos.mjs')) {
  const [src, out] = process.argv.slice(2);
  if (!src || !out) { console.error('usage: node tools/make-jsdos.mjs <sourceDir> <output.jsdos>'); process.exit(1); }
  const r = makeJsdos(src, out);
  console.log(`${out}: ${r.files} files, ${r.bytes} bytes`);
}

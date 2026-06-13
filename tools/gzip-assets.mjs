// Pre-compress web UI assets into "<file>.gz" siblings so the firmware webfs can stream them
// gzip-encoded. Critical on the Cardputer: large uncompressed files (e.g. 83KB shell.js) fail to
// transfer under low heap; gzipped they shrink ~3-4x and serve reliably.
//
// Scope: only served UI trees (www/ and apps/). NOT /system or /data — those config/state files
// are read through /api/fs/read (raw), never the static handler, and are rewritten at runtime.
//
// Usage: node tools/gzip-assets.mjs <root> [<root> ...]
import { readdirSync, statSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { join, extname } from 'node:path';
import { gzipSync } from 'node:zlib';

const roots = process.argv.slice(2);
if (!roots.length) { console.error('usage: node tools/gzip-assets.mjs <dir> [dir...]'); process.exit(1); }

const EXT = new Set(['.js', '.mjs', '.css', '.html', '.json', '.webmanifest', '.svg']);
let made = 0, savedKB = 0;

function walk(dir) {
  let names;
  try { names = readdirSync(dir); } catch { return; }
  for (const name of names) {
    const p = join(dir, name);
    const st = statSync(p);
    if (st.isDirectory()) { walk(p); continue; }
    if (name.endsWith('.gz')) continue;                 // never gzip a gzip
    if (!EXT.has(extname(name).toLowerCase())) continue;
    const raw = readFileSync(p);
    const gz = gzipSync(raw, { level: 9 });
    if (gz.length >= raw.length) { try { rmSync(p + '.gz'); } catch {} continue; }  // no win → drop any stale .gz
    writeFileSync(p + '.gz', gz);
    made++; savedKB += (raw.length - gz.length) / 1024;
  }
}

for (const r of roots) walk(r);
console.log(`gzipped ${made} files, saved ${savedKB.toFixed(0)} KB`);

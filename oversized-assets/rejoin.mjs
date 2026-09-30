// Fetches the oversized binaries and verifies their integrity (SHA-256).
// Usage: node oversized-assets/rejoin.mjs            (from the repo root — all assets)
//        node oversized-assets/rejoin.mjs <id> ...   (only some assets)
//
// For each asset it tries, in order:
//   1. the file already on disk (skip if the SHA-256 matches),
//   2. local split parts under oversized-assets/parts/ (offline fallback),
//   3. the declared source — a GitHub Release asset or a Hugging Face CDN URL.
// Everything is SHA-256-verified before it is written into place.
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname } from 'node:path';

const ROOT = process.cwd();
const manifest = JSON.parse(readFileSync(join(ROOT, 'oversized-assets', 'manifest.json'), 'utf8'));
const only = process.argv.slice(2);
const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

function partsBuffer(a) {
  const files = Array.from({ length: a.parts }, (_, i) =>
    join(ROOT, `${a.partPrefix}${String(i).padStart(3, '0')}`));
  if (files.some((p) => !existsSync(p))) return null;
  return Buffer.concat(files.map((p) => readFileSync(p)));
}

async function downloadBuffer(url) {
  const res = await fetch(url, { redirect: 'follow' });
  if (!res.ok) throw new Error(`HTTP ${res.status} for ${url}`);
  return Buffer.from(await res.arrayBuffer());
}

async function fetchAsset(a) {
  const dest = join(ROOT, a.path);
  // 1. already present and correct
  if (existsSync(dest) && a.sha256 && sha256(readFileSync(dest)) === a.sha256) {
    console.log(`= ${a.id}: already present (sha ok)`);
    return true;
  }
  // 2. local split parts
  let buf = a.parts ? partsBuffer(a) : null;
  let via = 'parts';
  // 3. declared source (release / cdn)
  if (!buf && a.source && (a.source.url || (a.source.kind === 'release' && a.source.asset))) {
    const url = a.source.url || `${manifest.releaseBase}${a.source.asset}`;
    console.log(`↓ ${a.id}: downloading ${url}`);
    buf = await downloadBuffer(url);
    via = 'download';
  }
  if (!buf) {
    console.error(`✗ ${a.id}: no local parts and no reachable source — see OVERSIZED-ASSETS.md`);
    return false;
  }
  const got = sha256(buf);
  if ((a.bytes && buf.length !== a.bytes) || (a.sha256 && got !== a.sha256)) {
    console.error(`✗ ${a.id}: integrity FAILED (expected ${(a.sha256 || '').slice(0, 12)}…/${a.bytes}B, got ${got.slice(0, 12)}…/${buf.length}B)`);
    return false;
  }
  mkdirSync(dirname(dest), { recursive: true });
  writeFileSync(dest, buf);
  console.log(`✓ ${a.id} -> ${a.path}  (${(buf.length / 1048576).toFixed(0)} MiB, via ${via}, sha ok)`);
  return true;
}

let ok = 0, fail = 0;
for (const a of manifest.assets) {
  if (only.length && !only.includes(a.id)) continue;
  try {
    if (await fetchAsset(a)) ok++; else fail++;
  } catch (e) {
    console.error(`✗ ${a.id}: ${e.message}`);
    fail++;
  }
}
// External assets are informational: the browser streams the GGUF from Hugging Face and the
// teacher cache is regenerated on demand. List how to get any the caller asked for by id.
for (const a of (manifest.external || [])) {
  if (only.length && !only.includes(a.id)) continue;
  const how = a.source.kind === 'regen' ? `regenerate: ${a.source.cmd}` : `download: ${a.source.url}`;
  console.log(`ℹ ${a.id}: not stored in the repo — ${how}`);
}
console.log(`\nDone: ${ok} rebuilt, ${fail} failed.`);
process.exit(fail ? 1 : 0);

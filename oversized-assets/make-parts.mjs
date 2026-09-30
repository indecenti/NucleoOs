// Regenerates the <100 MiB split parts + refreshes SHA-256/bytes in manifest.json.
// Usage: node oversized-assets/make-parts.mjs   (from the repo root)
//
// The manifest is the source of truth: this reads its `assets` list, splits each file that is
// present on disk into parts, and updates that asset's bytes/sha256/parts in place. It never
// touches `external` assets (streamed from a CDN or regenerated) and never rewrites `source`.
// Parts are the offline fallback; the primary delivery is each asset's `source` (a GitHub
// Release). After running this, upload both clips.pcm to the release named by `releaseBase`.
import { readFileSync, writeFileSync, mkdirSync, rmSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';

const CHUNK = 90 * 1024 * 1024; // 90 MiB < GitHub's 100 MiB limit
const ROOT = process.cwd();
const PARTS = join(ROOT, 'oversized-assets', 'parts');
const MANIFEST = join(ROOT, 'oversized-assets', 'manifest.json');
const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

const manifest = JSON.parse(readFileSync(MANIFEST, 'utf8'));
let done = 0;
for (const a of manifest.assets) {
  const abs = join(ROOT, a.path);
  if (!existsSync(abs)) { console.error(`SKIP (missing): ${a.path}`); continue; }
  const buf = readFileSync(abs);
  const dir = join(PARTS, a.id);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const nParts = Math.ceil(buf.length / CHUNK);
  for (let i = 0; i < nParts; i++) {
    writeFileSync(join(dir, `${a.id}.${String(i).padStart(3, '0')}`),
      buf.subarray(i * CHUNK, Math.min((i + 1) * CHUNK, buf.length)));
  }
  a.bytes = buf.length;
  a.sha256 = sha256(buf);
  a.parts = nParts;
  a.partPrefix = `oversized-assets/parts/${a.id}/${a.id}.`;
  console.log(`OK ${a.id}: ${(buf.length / 1048576).toFixed(0)} MiB -> ${nParts} parts  sha=${a.sha256.slice(0, 12)}…`);
  done++;
}

writeFileSync(MANIFEST, JSON.stringify(manifest, null, 2) + '\n');
console.log(`\nmanifest.json updated: ${done} asset(s) refreshed.`);
console.log(`Next: upload each clips.pcm to the release at ${manifest.releaseBase}`);

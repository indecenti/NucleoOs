// Anti-regression gate for the project's recurring ".gz shadowing" gotcha.
//
// The firmware webfs serves "<file>.gz" with precedence whenever the client accepts gzip, so a
// STALE .gz (its source edited but the sibling not regenerated) silently ships OLD code to the
// device — and an ORPHAN .gz (source deleted, .gz left behind) ships a file that no longer exists.
// Both have bitten this repo (the v75-era settings/log-viewer drift). gzip-assets.mjs regenerates
// them; this verifies they're actually in sync, so a missed `node tools/gzip-assets.mjs` fails the
// lint instead of the device.
//
// Scope: the SERVED UI trees (web/shell, apps/*/www). Vendored bundles and non-text archives are
// skipped (they have no plaintext source here). Run: `node tools/check-gz.mjs` (or via `npm run validate`).
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { createHash } from 'node:crypto';
import { join, dirname, extname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const ROOTS = ['web/shell', 'apps'];
// Only these source types are gzipped by gzip-assets.mjs (+ .wasm, which we gzip by hand for the
// big WASM engines). A ".tar.gz" model bundle is NOT one of these → skipped as a vendored archive.
const EXT = new Set(['.js', '.mjs', '.css', '.html', '.json', '.webmanifest', '.svg', '.wasm']);
const SKIP_DIR = new Set(['node_modules', 'vendor', '.git']);

const sha = (buf) => createHash('sha256').update(buf).digest('hex');

export function checkGz() {
  const stale = [];
  const orphan = [];
  let checked = 0;

  function walk(dir) {
    let entries;
    try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return; }
    for (const e of entries) {
      if (e.isDirectory()) { if (!SKIP_DIR.has(e.name)) walk(join(dir, e.name)); continue; }
      if (!e.name.endsWith('.gz')) continue;
      const gzPath = join(dir, e.name);
      const srcPath = gzPath.slice(0, -3);                 // strip ".gz"
      if (!EXT.has(extname(srcPath).toLowerCase())) continue;   // e.g. .tar.gz vendored bundle → not ours
      const rel = gzPath.slice(ROOT.length + 1).replace(/\\/g, '/');
      let src;
      try { src = readFileSync(srcPath); } catch { orphan.push(rel); continue; }   // .gz with no source
      try {
        const decoded = gunzipSync(readFileSync(gzPath));
        if (sha(decoded) !== sha(src)) stale.push(rel);
      } catch { stale.push(rel + ' (corrupt gzip)'); }
      checked++;
    }
  }
  for (const r of ROOTS) walk(join(ROOT, r));
  return { checked, stale, orphan };
}

// Run standalone: `node tools/check-gz.mjs`
if (import.meta.url === `file://${process.argv[1].replace(/\\/g, '/')}` || process.argv[1].endsWith('check-gz.mjs')) {
  const { checked, stale, orphan } = checkGz();
  const problems = [...stale.map((s) => `stale .gz (run: node tools/gzip-assets.mjs): ${s}`),
                    ...orphan.map((o) => `orphan .gz (source deleted, remove it): ${o}`)];
  if (problems.length) {
    console.error(`✗ ${problems.length} .gz problem(s) of ${checked} pairs:\n` + problems.map((p) => '  - ' + p).join('\n'));
    process.exit(1);
  }
  console.log(`✓ .gz fresh: ${checked} pairs in sync (web/shell + apps)`);
}

// Build the one-fetch app-icon bundle for the web shell.
//
// WHY: every app icon used to be its own <img src="/apps/<id>/icon.svg"> GET. On the device that is
// ~32 separate requests against a single-task, 4-socket, PSRAM-less httpd — each one paying RTT +
// SD fopen + chunked stream — so a cold desktop paint trickled in for many seconds (the client-side
// NucleoIcon pool rightly caps concurrency at 3 to protect the server, which makes the trickle
// LONGER, not shorter). All the SVGs together are ~27 KB raw / ~4 KB gzipped: one bundle fetch
// replaces the whole burst, and the shell persists it in localStorage so repeat boots paint icons
// instantly without any network at all (see IconBundle in web/shell/shell.js).
//
// Output: web/shell/icons.json  { "_v": "<sha1-12>", "<app-id>": "<svg source>", ... }  (+ .gz)
// Also mirrored into deploy/sd/www/shell/ so the SD payload ships it.
//
// Icon path resolution mirrors glyph() in shell.js:
//   - relative               -> apps/<id>/www/<icon>
//   - /apps/<x>/<rest>       -> apps/<x>/www/<rest>   (the served /apps/ tree has no /www/ segment)
//   - data:/http(s) icons    -> skipped (nothing to bundle)
//
// Usage: node tools/gen-icon-bundle.mjs [--check]
//   --check: exit 1 if the committed bundle differs from what would be generated (validate-wired).
import { readdirSync, readFileSync, writeFileSync, existsSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { gzipSync } from 'node:zlib';
import { createHash } from 'node:crypto';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const APPS = join(REPO, 'apps');
const OUT = join(REPO, 'web', 'shell', 'icons.json');
const MIRROR = join(REPO, 'deploy', 'sd', 'www', 'shell', 'icons.json');
const PER_ICON_CAP = 64 * 1024;   // an "icon" this large is a bug, not an asset

function buildJson(warn) {
  const bundle = {};
  for (const id of readdirSync(APPS).sort()) {
    const manifestPath = join(APPS, id, 'manifest.json');
    if (!existsSync(manifestPath)) continue;
    let icon;
    try { icon = JSON.parse(readFileSync(manifestPath, 'utf8')).icon; } catch { continue; }
    if (!icon || /^(data:|https?:)/.test(icon)) continue;

    let file;
    const m = icon.match(/^\/apps\/([^/]+)\/(.+)$/);
    if (m) file = join(APPS, m[1], 'www', m[2]);
    else if (!icon.startsWith('/')) file = join(APPS, id, 'www', icon);
    else continue;                            // /data|/system icons live on the card, not in the repo

    if (!existsSync(file)) { if (warn) console.warn(`  ! ${id}: icon not found: ${file}`); continue; }
    if (statSync(file).size > PER_ICON_CAP) { if (warn) console.warn(`  ! ${id}: icon over ${PER_ICON_CAP} B, skipped`); continue; }
    if (!file.toLowerCase().endsWith('.svg')) continue;   // raster icons stay per-file <img>

    bundle[id] = readFileSync(file, 'utf8');
  }
  const body = { _v: createHash('sha1').update(JSON.stringify(bundle)).digest('hex').slice(0, 12), ...bundle };
  return { json: JSON.stringify(body), count: Object.keys(bundle).length };
}

// validate.mjs wiring: returns [] when fresh, otherwise one message per out-of-date output file.
export function checkIconBundle() {
  const { json } = buildJson(false);
  const drift = [];
  for (const out of [OUT, MIRROR])
    if (!existsSync(out) || readFileSync(out, 'utf8') !== json)
      drift.push(out.slice(REPO.length + 1).replaceAll('\\', '/'));
  return drift;
}

if (process.argv[1] && process.argv[1].endsWith('gen-icon-bundle.mjs')) {
  if (process.argv.includes('--check')) {
    const drift = checkIconBundle();
    if (drift.length) { console.error('STALE icon bundle (run: node tools/gen-icon-bundle.mjs): ' + drift.join(', ')); process.exit(1); }
    console.log('icon bundle up to date');
  } else {
    const { json, count } = buildJson(true);
    for (const out of [OUT, MIRROR]) {
      writeFileSync(out, json);
      writeFileSync(out + '.gz', gzipSync(Buffer.from(json), { level: 9 }));
    }
    console.log(`icons.json: ${count} icons, ${(json.length / 1024).toFixed(1)} KB raw, ${(gzipSync(Buffer.from(json), { level: 9 }).length / 1024).toFixed(1)} KB gz`);
  }
}

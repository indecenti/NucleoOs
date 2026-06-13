// audit-icons-sd — check, on a given SD/deploy root, whether each installed app's manifest icon
// is actually present (as icon.svg OR icon.svg.gz, since the firmware serves the gzipped sibling).
// Usage: node tools/audit-icons-sd.mjs <root>   e.g.  H:\   or  deploy/sd-safe
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const root = process.argv[2];
if (!root) { console.error('usage: node tools/audit-icons-sd.mjs <root>'); process.exit(1); }
const reg = JSON.parse(readFileSync('registry/apps.json', 'utf8'));
const installed = (reg.installed || []).map((a) => a.id);
const missing = [], present = [];

for (const id of installed) {
  // read the manifest from the SD root if present, else the repo (the device uses the SD copy)
  let m = {};
  const sdMf = join(root, 'apps', id, 'manifest.json');
  const repoMf = join('apps', id, 'manifest.json');
  try { m = JSON.parse(readFileSync(existsSync(sdMf) ? sdMf : repoMf, 'utf8')); } catch {}
  const icon = m.icon;
  if (!icon) continue;                       // emoji glyph, no file needed
  if (/^(data:|https?:)/.test(icon)) continue;  // inline data-URI / remote: never a 404'ing SD file
  let rel = icon;
  const pfx = '/apps/' + id + '/';
  if (rel.startsWith(pfx)) rel = rel.slice(pfx.length);
  else rel = rel.replace(/^\//, '');
  const base = join(root, 'apps', id, 'www', rel);
  const ok = existsSync(base) || existsSync(base + '.gz');
  (ok ? present : missing).push(`${id} (${icon}${existsSync(base + '.gz') && !existsSync(base) ? ' [gz only]' : ''})`);
}

console.log(`root: ${root}`);
console.log(`\n=== MISSING on SD (${missing.length}) -> these 404 on the device ===\n  ${missing.join('\n  ') || '(none)'}`);
console.log(`\n=== present (${present.length}) ===\n  ${present.join('\n  ')}`);

// audit-icons — for each installed app, resolve its manifest `icon` to the www file the
// firmware would serve (/apps/<id>/x -> apps/<id>/www/x) and report any that are MISSING
// (the cause of the shell's repeated `icon.svg 404`). Apps with no icon use the emoji glyph (fine).
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';

const reg = JSON.parse(readFileSync('registry/apps.json', 'utf8'));
const installed = (reg.installed || []).map((a) => a.id);
const broken = [], ok = [], noicon = [];

for (const id of installed) {
  const mf = join('apps', id, 'manifest.json');
  if (!existsSync(mf)) { broken.push({ id, icon: '(NO MANIFEST)', expected: mf }); continue; }
  const m = JSON.parse(readFileSync(mf, 'utf8'));
  const icon = m.icon;
  if (!icon) { noicon.push(id); continue; }
  let rel = icon;
  const pfx = '/apps/' + id + '/';
  if (rel.startsWith(pfx)) rel = rel.slice(pfx.length);
  else rel = rel.replace(/^\//, '');
  const f = join('apps', id, 'www', rel);
  if (existsSync(f)) ok.push(`${id}  (${icon})`);
  else broken.push({ id, icon, expected: f.split('\\').join('/') });
}

console.log('=== BROKEN (manifest icon -> missing www file) ===');
broken.forEach((b) => console.log(`  ${b.id}  icon=${b.icon}  -> MISSING ${b.expected}`));
console.log(`\n=== OK (${ok.length}) ===\n  ${ok.join('\n  ')}`);
console.log(`\n=== NO ICON / emoji glyph (${noicon.length}) ===\n  ${noicon.join(', ')}`);

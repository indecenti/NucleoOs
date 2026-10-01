// gen-app-catalog.mjs — web/shell/app-catalog.json: what each installed app IS (category + one-line description),
// generated from the apps' own manifests. The device's /api/apps carries only id + name, so a model asked
// "which apps are about music?" guessed ("Calculator / Notes use the speech synthesiser") and missed the
// recorder, dictation and the metronome. The agent's `apps` shell command and list_apps merge this in.
//
//   node tools/gen-app-catalog.mjs           write web/shell/app-catalog.json (+ .gz)
//   node tools/gen-app-catalog.mjs --check   exit 1 if it is out of sync (npm run catalog:check, the gate)
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..');
const OUT = join(repo, 'web', 'shell', 'app-catalog.json');
const DESC_MAX = 140;   // one line each: ~48 apps stay a few KB on the SD and in a model's context

export function buildCatalog() {
  const installed = JSON.parse(readFileSync(join(repo, 'registry', 'apps.json'), 'utf8')).installed || [];
  const apps = {};
  for (const a of installed) {
    const mf = join(repo, 'apps', a.id, 'manifest.json');
    if (!existsSync(mf)) continue;
    const m = JSON.parse(readFileSync(mf, 'utf8'));
    let d = String(m.description || '').replace(/\s+/g, ' ').trim();
    if (d.length > DESC_MAX) d = d.slice(0, d.lastIndexOf(' ', DESC_MAX - 1)).replace(/[,;:—–-]+$/, '') + '…';
    apps[a.id] = { name: m.name || a.id, category: m.category || '', description: d };
  }
  return JSON.stringify({ generated: 'tools/gen-app-catalog.mjs', apps }, null, 1) + '\n';
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const text = buildCatalog();
  const same = existsSync(OUT) && readFileSync(OUT, 'utf8') === text;
  if (process.argv.includes('--check')) {
    if (!same) { console.log('catalog:check: web/shell/app-catalog.json is OUT OF SYNC with the app manifests — run node tools/gen-app-catalog.mjs'); process.exit(1); }
    console.log('catalog:check: app-catalog.json matches the app manifests');
  } else if (!same) {
    writeFileSync(OUT, text);
    writeFileSync(OUT + '.gz', gzipSync(Buffer.from(text), { level: 9 }));
    console.log('wrote web/shell/app-catalog.json');
  } else console.log('app-catalog.json already current');
}

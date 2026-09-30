// staging-check — is the committed SD staging tree (deploy/sd) current with the sources it is built from?
// push-ota --sync, sd-sync.ps1 and sd-net-sync push deploy/sd AS IS; only tools/deploy.ps1 (also step 2 of
// release.ps1) rebuilds it. A stale tree ships old web code, so the pushers warn when this finds drift.
// Compares the sources deploy.ps1 stages 1:1 — registry/ -> system/registry, apps/ -> apps, web/shell ->
// www/shell — content-wise with line endings normalized (the tree is git-tracked; checkouts may differ in
// EOL only). Device/user state is excluded (tools/lib/sd-policy.json), like deploy.ps1 does.
//   node tools/staging-check.mjs            (npm run staging:check)  exit 1 = stale
import { readdirSync, readFileSync, existsSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';
import { isDeviceState } from './lib/sd-policy.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const SD = join(ROOT, 'deploy', 'sd');
const MAP = [['registry', 'system/registry'], ['apps', 'apps'], ['web/shell', 'www/shell']];
const norm = (b) => Buffer.from(b.toString('latin1').replace(/\r\n/g, '\n'), 'latin1');

function walk(dir, out = []) {
  for (const n of readdirSync(dir)) {
    const p = join(dir, n);
    if (statSync(p).isDirectory()) walk(p, out); else out.push(p);
  }
  return out;
}

export function stagingDrift() {
  const missing = [], changed = [];
  for (const [src, dest] of MAP) {
    const root = join(ROOT, src);
    if (!existsSync(root)) continue;
    for (const f of walk(root)) {
      const rel = `${dest}/${relative(root, f).split(/[\\/]/).join('/')}`;
      if (isDeviceState(rel)) continue;
      const staged = join(SD, ...rel.split('/'));
      if (!existsSync(staged)) { missing.push(rel); continue; }
      if (!norm(readFileSync(f)).equals(norm(readFileSync(staged)))) changed.push(rel);
    }
  }
  return { missing, changed };
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const { missing, changed } = stagingDrift();
  const n = missing.length + changed.length;
  if (!n) { console.log('staging-check: deploy/sd is current with registry/, apps/, web/shell'); process.exit(0); }
  console.log(`staging-check: deploy/sd is STALE — ${changed.length} changed, ${missing.length} missing vs the sources`);
  for (const r of [...changed.slice(0, 8).map((x) => `  changed  ${x}`), ...missing.slice(0, 8).map((x) => `  missing  ${x}`)]) console.log(r);
  console.log('  rebuild it with: powershell -ExecutionPolicy Bypass -File tools\\deploy.ps1');
  process.exit(1);
}

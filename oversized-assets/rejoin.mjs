// Ricostruisce i file oversize dai loro pezzi e ne verifica l'integrità (SHA-256).
// Uso: node oversized-assets/rejoin.mjs            (dalla root del repo)
//      node oversized-assets/rejoin.mjs <id> ...   (solo alcuni asset)
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname } from 'node:path';

const ROOT = process.cwd();
const manifest = JSON.parse(readFileSync(join(ROOT, 'oversized-assets', 'manifest.json'), 'utf8'));
const only = process.argv.slice(2);
const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

let ok = 0, fail = 0;
for (const a of manifest.assets) {
  if (only.length && !only.includes(a.id)) continue;
  const partFiles = Array.from({ length: a.parts }, (_, i) =>
    join(ROOT, `${a.partPrefix}${String(i).padStart(3, '0')}`));
  const missing = partFiles.filter((p) => !existsSync(p));
  if (missing.length) { console.error(`✗ ${a.id}: parti mancanti (${missing.length})`); fail++; continue; }

  const buf = Buffer.concat(partFiles.map((p) => readFileSync(p)));
  const gotSha = sha256(buf);
  if (buf.length !== a.bytes || gotSha !== a.sha256) {
    console.error(`✗ ${a.id}: integrità FALLITA (atteso ${a.sha256.slice(0,12)}…/${a.bytes}B, ottenuto ${gotSha.slice(0,12)}…/${buf.length}B)`);
    fail++; continue;
  }
  const dest = join(ROOT, a.path);
  mkdirSync(dirname(dest), { recursive: true });
  writeFileSync(dest, buf);
  console.log(`✓ ${a.id} -> ${a.path}  (${(a.bytes/1048576).toFixed(0)} MiB, sha ok)`);
  ok++;
}
console.log(`\nFatto: ${ok} ricostruiti, ${fail} falliti.`);
process.exit(fail ? 1 : 0);

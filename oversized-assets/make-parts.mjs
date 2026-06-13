// Genera le parti <100 MiB dei file oversize + manifest.json (con SHA-256).
// Uso: node oversized-assets/make-parts.mjs   (dalla root del repo)
// I file originali NON sono versionati (sono in .git/info/exclude): qui produciamo
// solo le parti committabili. Per ricostruirli: node oversized-assets/rejoin.mjs
import { readFileSync, writeFileSync, mkdirSync, rmSync, existsSync, statSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';

const CHUNK = 90 * 1024 * 1024; // 90 MiB < limite 100 MiB di GitHub
const ROOT = process.cwd();
const PARTS = join(ROOT, 'oversized-assets', 'parts');

const assets = [
  { id: 'qwen-coder-gguf',
    path: 'deploy/sd-safe/apps/anima/www/forge/models/Qwen2.5-Coder-0.5B-Instruct-GGUF/qwen2.5-coder-0.5b-instruct-q4_k_m.gguf',
    what: 'Modello Qwen2.5-Coder 0.5B Instruct (GGUF q4_k_m) usato da ANIMA Forge (path wllama/llama.cpp).' },
  { id: 'teacher-npy',
    path: 'tools/anima/.cache/teacher_200000_192.npy',
    what: 'Cache NumPy degli embedding "teacher" (200k campioni x 192 dim) della pipeline encoder ANIMA.' },
  { id: 'tts-it-clips',
    path: 'deploy/sd-safe/data/tts/it/clips.pcm',
    what: 'Banco clip audio del TTS concatenativo italiano (nucleo_tts).' },
  { id: 'tts-en-clips',
    path: 'deploy/sd-safe/data/tts/en/clips.pcm',
    what: 'Banco clip audio del TTS concatenativo inglese (nucleo_tts).' },
];

const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

const out = { chunkBytes: CHUNK, generated: 'oversized-assets/make-parts.mjs', assets: [] };

for (const a of assets) {
  const abs = join(ROOT, a.path);
  if (!existsSync(abs)) { console.error(`SALTO (assente): ${a.path}`); continue; }
  const buf = readFileSync(abs);
  const dir = join(PARTS, a.id);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const nParts = Math.ceil(buf.length / CHUNK);
  for (let i = 0; i < nParts; i++) {
    const part = buf.subarray(i * CHUNK, Math.min((i + 1) * CHUNK, buf.length));
    const name = `${a.id}.${String(i).padStart(3, '0')}`;
    writeFileSync(join(dir, name), part);
  }
  out.assets.push({
    id: a.id, path: a.path, what: a.what,
    bytes: buf.length, sha256: sha256(buf),
    parts: nParts, partPrefix: `oversized-assets/parts/${a.id}/${a.id}.`,
  });
  console.log(`OK ${a.id}: ${(buf.length/1048576).toFixed(0)} MiB -> ${nParts} parti  sha=${sha256(buf).slice(0,12)}…`);
}

writeFileSync(join(ROOT, 'oversized-assets', 'manifest.json'), JSON.stringify(out, null, 2) + '\n');
console.log(`\nmanifest.json scritto: ${out.assets.length} asset.`);

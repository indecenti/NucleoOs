// Generates the <100 MiB parts of the oversized files + manifest.json (with SHA-256).
// Usage: node oversized-assets/make-parts.mjs   (from the repo root)
// The original files are NOT versioned (they live in .git/info/exclude): here we produce
// only the committable parts. To rebuild the originals: node oversized-assets/rejoin.mjs
import { readFileSync, writeFileSync, mkdirSync, rmSync, existsSync, statSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';

const CHUNK = 90 * 1024 * 1024; // 90 MiB < GitHub's 100 MiB limit
const ROOT = process.cwd();
const PARTS = join(ROOT, 'oversized-assets', 'parts');

const assets = [
  { id: 'qwen-coder-gguf',
    path: 'deploy/sd-safe/apps/anima/www/forge/models/Qwen2.5-Coder-0.5B-Instruct-GGUF/qwen2.5-coder-0.5b-instruct-q4_k_m.gguf',
    what: 'Qwen2.5-Coder 0.5B Instruct model (GGUF q4_k_m) used by ANIMA Forge (wllama/llama.cpp path).' },
  { id: 'teacher-npy',
    path: 'tools/anima/.cache/teacher_200000_192.npy',
    what: 'NumPy cache of the "teacher" embeddings (200k samples x 192 dims) of the ANIMA encoder pipeline.' },
  { id: 'tts-it-clips',
    path: 'deploy/sd-safe/data/tts/it/clips.pcm',
    what: 'Audio clip bank of the Italian concatenative TTS (nucleo_tts).' },
  { id: 'tts-en-clips',
    path: 'deploy/sd-safe/data/tts/en/clips.pcm',
    what: 'Audio clip bank of the English concatenative TTS (nucleo_tts).' },
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

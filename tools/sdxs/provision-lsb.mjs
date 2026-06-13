#!/usr/bin/env node
// provision-lsb.mjs — download lsb/sdxs-controlnet-sketch ONNX, rechunk to 7 MB for SD serving.
//
// The lsb repo (github.com/lsb/sdxs-controlnet-sketch) ships the model as TWO pre-built ONNX files,
// already split into ~50 MB parts:
//   sd512q68.onnx        — fused ControlNet+UNet+VAE, 6/8-bit mixed quant  (9 parts, ~429 MB)
//   sd_text_encoder_q8.onnx — CLIP text encoder, 8-bit quant              (3 parts, ~124 MB)
// We reassemble them then rechunk to 7 MiB pieces so the Cardputer httpd can serve them safely.
// tokenizer.json is fetched from HuggingFace IDKiro/sdxs-512-dreamshaper.
//
// Usage: node tools/sdxs/provision-lsb.mjs [--out <dir>]
// Default --out: deploy/sd-safe/apps/paint/www/models/sdxs-512-dreamshaper-sketch

import { createHash }                               from 'node:crypto';
import { writeFileSync, mkdirSync }                 from 'node:fs';
import { join }                                     from 'node:path';
import process                                      from 'node:process';

const CHUNK   = 7 * 1024 * 1024;
const LSB     = 'https://raw.githubusercontent.com/lsb/sdxs-controlnet-sketch/trunk';
const HF_BASE = 'https://huggingface.co/IDKiro/sdxs-512-dreamshaper/resolve/main/tokenizer';

function arg(k, d) { const i = process.argv.indexOf(k); return i >= 0 && process.argv[i+1] ? process.argv[i+1] : d; }
const OUT = arg('--out', join('deploy', 'sd-safe', 'apps', 'paint', 'www', 'models', 'sdxs-512-dreamshaper-sketch'));
mkdirSync(OUT, { recursive: true });

const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');
const pad3   = (n)   => String(n).padStart(3, '0');

async function fetchBuf(url, label) {
  process.stdout.write(`  ↓ ${label} `);
  const r = await fetch(url);
  if (!r.ok) throw new Error(`HTTP ${r.status}: ${url}`);
  const buf = Buffer.from(await r.arrayBuffer());
  console.log(`(${(buf.length / 1e6).toFixed(1)} MB)`);
  return buf;
}

// Download lsb-style split file (name.0.db … name.N-1.db) and concatenate.
async function downloadAssemble(name, parts) {
  console.log(`\n[ ${name} — ${parts} parts ]`);
  const chunks = [];
  for (let i = 0; i < parts; i++) chunks.push(await fetchBuf(`${LSB}/${name}.${pad3(i)}.db`, `${name}.${pad3(i)}.db`));
  return Buffer.concat(chunks);
}

// Rechunk a buffer into CHUNK-sized pieces, write to OUT, return component descriptor.
function rechunk(role, file, buf) {
  const n = Math.ceil(buf.length / CHUNK);
  const partSha = [];
  for (let i = 0; i < n; i++) {
    const sl = buf.subarray(i * CHUNK, Math.min(buf.length, (i + 1) * CHUNK));
    writeFileSync(join(OUT, `${file}.${pad3(i)}`), sl);
    partSha.push(sha256(sl));
  }
  console.log(`  → ${role.padEnd(14)} ${file.padEnd(22)} ${(buf.length / 1e6).toFixed(1)} MB → ${n} chunks (7 MB)`);
  return { role, file, bytes: buf.length, parts: n, sha256: sha256(buf), partSha };
}

// ─── main ────────────────────────────────────────────────────────────────────
console.log('provision-lsb: SDXS-512 fused (lsb/sdxs-controlnet-sketch)');
console.log(`Output: ${OUT}\n`);

const components = [];
let totalBytes = 0;

const fusedBuf = await downloadAssemble('sd512q68.onnx', 9);
components.push(rechunk('fused', 'fused.onnx', fusedBuf));
totalBytes += fusedBuf.length;

const textBuf = await downloadAssemble('sd_text_encoder_q8.onnx', 3);
components.push(rechunk('text-encoder', 'text_encoder.onnx', textBuf));
totalBytes += textBuf.length;

// Assemble tokenizer.json from vocab.json + merges.txt (no pre-built tokenizer.json in this HF repo)
console.log('\n[ tokenizer ]');
const vocabBuf  = await fetchBuf(`${HF_BASE}/vocab.json`, 'vocab.json');
const mergesBuf = await fetchBuf(`${HF_BASE}/merges.txt`, 'merges.txt');
const vocab  = JSON.parse(vocabBuf.toString('utf8'));
const merges = mergesBuf.toString('utf8').split('\n').filter(l => l && !l.startsWith('#'));
const tokJson = JSON.stringify({ vocab, merges });
writeFileSync(join(OUT, 'tokenizer.json'), tokJson);
components.unshift({ role: 'tokenizer', file: 'tokenizer.json', bytes: Buffer.byteLength(tokJson), parts: 0 });

const manifest = {
  model:      'sdxs-512-dreamshaper-sketch',
  revision:   '2024-12-lsb',
  format:     'lsb-fused',   // signals diffusion-engine to use makeFusedEngine
  chunkBytes: CHUNK,
  resolution: 512,
  steps:      1,
  totalBytes,
  components,
  // io names as exported by lsb/sdxs-controlnet-sketch export-sdxs-512.py
  io: {
    fused: {
      image:             'image',             // uint8  [512, 512]  sketch (dark stroke = edge)
      promptEmbeds:      'prompt_embeds',     // f32    [1, 77, 768]
      conditioningScale: 'conditioning_scale',// f32    [1]
      latents:           'latents',           // f32    [1, 4, 64, 64]
      output:            'output_image',      // uint8  [512, 512, 3]  RGB
    },
    text: {
      input:  'input_ids',        // int64  [1, 77]
      output: 'output_embeddings',// f32    [1, 77, 768]
    },
  },
};

writeFileSync(join(OUT, 'manifest.json'), JSON.stringify(manifest, null, 2));

console.log(`\n✓  manifest.json written`);
console.log(`   ${components.length} components, ${(totalBytes / 1e6).toFixed(0)} MB on-disk`);
console.log(`\nNext steps:`);
console.log(`  tools/sd-sync.ps1               → copy deploy/sd-safe → H: (SD card)`);
console.log(`  open Paint → ✨ Atelier          → engine loads on first open`);

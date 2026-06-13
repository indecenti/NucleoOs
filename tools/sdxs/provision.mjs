#!/usr/bin/env node
// provision.mjs — turn the Stable Diffusion XS ONNX export into a Cardputer-ready model tree: split every
// component into 7 MB parts (the SD/httpd chunk size), SHA-256 each part, and emit the manifest.json that
// Paint's Atelier (sd-model-loader + diffusion-engine) and the ANIMA Model Manager (model-fetch) consume.
// Weights NEVER live in git — you run this on your PC against locally-exported ONNX, then sd-sync to the
// device. Pure Node (fs + crypto), deterministic.
//
// 1) Get the ONNX (one of):
//      a) download lsb's pre-exported files, OR
//      b) run the export:  python tools/sdxs/export_sdxs_onnx.py  (needs torch+optimum; see that file)
//    You need: text_encoder.onnx  controlnet.onnx  unet.onnx  vae_decoder.onnx  tokenizer.json
// 2) Provision:
//      node tools/sdxs/provision.mjs --in <dir-with-onnx> [--out <model-dir>] [--rev 2025-03]
//    default --out: deploy/sd-safe/apps/paint/www/models/sdxs-512-dreamshaper-sketch
// 3) Ship:  tools/sd-sync.ps1   (copies deploy/sd-safe → SD; never deletes user state)
// 4) On a WebGPU browser: open Paint → ✨ Atelier → Genera.  (No model? the Atelier runs the deterministic
//    procedural preview and tells you to download from the Model Manager in ANIMA.)

import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync, readdirSync, mkdirSync, existsSync, statSync, copyFileSync } from 'node:fs';
import { join, basename } from 'node:path';

const CHUNK = 7 * 1024 * 1024;     // 7 MiB — proven safe vs the single-task httpd reset ceiling
const COMPONENTS = [               // role → expected ONNX filename (the diffusion-engine reads roles)
  { role: 'text-encoder', file: 'text_encoder.onnx' },
  { role: 'controlnet',   file: 'controlnet.onnx' },
  { role: 'unet',         file: 'unet.onnx' },
  { role: 'vae-decoder',  file: 'vae_decoder.onnx' },
];
// Default ONNX io spec for the IDKiro/sdxs-512-dreamshaper-sketch export. ADJUST if your export uses other
// tensor names (run `python -c "import onnx;print([i.name for i in onnx.load('unet.onnx').graph.input])"`).
const DEFAULT_IO = {
  resolution: 512, latentChannels: 4, latentScale: 8, controlChannels: 3, timestep: 999, vaeScale: 0.18215,
  text: { input: 'input_ids', output: 'last_hidden_state' },
  control: { sample: 'sample', timestep: 'timestep', encoder: 'encoder_hidden_states', hint: 'controlnet_cond', outDown: 'down_block_res_samples', outMid: 'mid_block_res_sample' },
  unet: { sample: 'sample', timestep: 'timestep', encoder: 'encoder_hidden_states', down: 'down_block_additional_residuals', mid: 'mid_block_additional_residual', output: 'out_sample' },
  vae: { input: 'latent_sample', output: 'sample' },
};

function arg(name, def) { const i = process.argv.indexOf(name); return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : def; }
const sha = (buf) => createHash('sha256').update(buf).digest('hex');
const pad3 = (i) => String(i).padStart(3, '0');

const IN = arg('--in', null);
const OUT = arg('--out', join('deploy', 'sd-safe', 'apps', 'paint', 'www', 'models', 'sdxs-512-dreamshaper-sketch'));
const REV = arg('--rev', '2025-03');
if (!IN || !existsSync(IN)) { console.error('usage: node tools/sdxs/provision.mjs --in <dir-with-onnx> [--out <dir>] [--rev <rev>]'); process.exit(2); }

mkdirSync(OUT, { recursive: true });
const components = [];
const shards = [];
let total = 0;

for (const c of COMPONENTS) {
  const src = join(IN, c.file);
  if (!existsSync(src)) { console.error(`MISSING ${c.file} in ${IN} — provide all of: ${COMPONENTS.map((x) => x.file).join(', ')}`); process.exit(1); }
  const bytes = readFileSync(src);
  const parts = Math.ceil(bytes.length / CHUNK);
  const partSha = [];
  for (let i = 0; i < parts; i++) {
    const slice = bytes.subarray(i * CHUNK, Math.min(bytes.length, (i + 1) * CHUNK));
    const name = `${c.file}.${pad3(i)}`;
    writeFileSync(join(OUT, name), slice);
    const h = sha(slice);
    partSha.push(h);
    shards.push({ name, bytes: slice.length, sha256: h });
  }
  components.push({ role: c.role, file: c.file, bytes: bytes.length, parts, sha256: sha(bytes), partSha });
  total += bytes.length;
  console.log(`  ${c.role.padEnd(13)} ${c.file.padEnd(20)} ${(bytes.length / 1e6).toFixed(1)} MB → ${parts} parts`);
}

// tokenizer.json (CLIP vocab+merges) — small, copied whole (the engine builds the tokenizer from it).
const tokSrc = join(IN, 'tokenizer.json');
if (existsSync(tokSrc)) { copyFileSync(tokSrc, join(OUT, 'tokenizer.json')); components.unshift({ role: 'tokenizer', file: 'tokenizer.json', bytes: statSync(tokSrc).size, parts: 0 }); }
else console.warn('  ! tokenizer.json not found — Atelier real engine needs it (CLIP ViT-L/14).');

const manifest = { model: 'sdxs-512-dreamshaper-sketch', revision: REV, chunkBytes: CHUNK, resolution: 512, steps: 1,
  totalBytes: total, io: DEFAULT_IO, components, shards };
writeFileSync(join(OUT, 'manifest.json'), JSON.stringify(manifest, null, 2));

console.log(`\nOK → ${OUT}`);
console.log(`  manifest.json: ${components.length} components, ${shards.length} shards, ${(total / 1e6).toFixed(0)} MB total.`);
console.log(`\nNext: tools/sd-sync.ps1  (ship deploy/sd-safe → SD), then open Paint → ✨ Atelier on a WebGPU browser.`);
console.log(`If you adjusted tensor names, edit "io" in ${join(OUT, 'manifest.json')} (or DEFAULT_IO here) to match your export.`);

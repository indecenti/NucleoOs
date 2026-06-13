#!/usr/bin/env node
// chunk-ort-wasm.mjs — split the ONNX Runtime WebGPU .wasm into 7 MiB pieces so the Cardputer httpd
// can serve it. The whole 22.5 MB blob trips the httpd's safe-serve limit (→ 503); chunked like the
// model, Atelier fetches the pieces via /api/fs/read, reassembles them and hands the bytes to ORT
// through ort.env.wasm.wasmBinary (no .wasm fetch at all).
//
// Writes <name>.000 … <name>.NNN next to the original in BOTH the repo and the SD staging tree,
// plus a tiny ort-wasm.manifest.json {file, bytes, parts} the loader reads to know how many parts.
//
// Usage: node tools/sdxs/chunk-ort-wasm.mjs

import { readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const CHUNK = 7 * 1024 * 1024;
const WASM  = 'ort-wasm-simd-threaded.asyncify.wasm';
const REL   = join('apps', 'paint', 'www', 'vendor', 'onnxruntime-web');
const ROOTS = ['.', join('deploy', 'sd-safe')];          // repo tree + SD staging
const pad3  = (n) => String(n).padStart(3, '0');

const srcPath = join('.', REL, WASM);
if (!existsSync(srcPath)) { console.error(`missing: ${srcPath}`); process.exit(1); }
const buf = readFileSync(srcPath);
const parts = Math.ceil(buf.length / CHUNK);

console.log(`chunk-ort-wasm: ${WASM}`);
console.log(`  ${(buf.length / 1e6).toFixed(1)} MB → ${parts} chunks (7 MiB)\n`);

const manifest = JSON.stringify({ file: WASM, bytes: buf.length, parts });

for (const root of ROOTS) {
  const dir = join(root, REL);
  if (!existsSync(dir)) { mkdirSync(dir, { recursive: true }); }
  for (let i = 0; i < parts; i++) {
    const slice = buf.subarray(i * CHUNK, Math.min(buf.length, (i + 1) * CHUNK));
    writeFileSync(join(dir, `${WASM}.${pad3(i)}`), slice);
  }
  writeFileSync(join(dir, 'ort-wasm.manifest.json'), manifest);
  console.log(`  → ${dir}  (${parts} parts + manifest)`);
}

console.log(`\n✓  done. parts=${parts} bytes=${buf.length}`);
console.log(`   Next: tools/deploy.ps1  then additive push to SD (vendor/ chunks).`);

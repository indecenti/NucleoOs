// prep-lib-manifest.mjs — generate vendor/lib-manifest.json: ground-truth {bytes, sha256} for each
// vendored M4 RUNTIME lib (the WebLLM ESM + per-model model_lib .wasm, and the wllama ESM + its two
// runtime .wasm). This is the integrity + air-gap-readiness ground truth, analogous to the per-model
// manifest.json that prep-llm-models.mjs writes for the weights. readiness.js / lib-resolver.js read
// it to (a) classify whether an engine can run with no network, and (b) reject a TRUNCATED SD lib
// (incomplete sync) instead of silently handing a corrupt file to the runtime.
//
//   Usage: node tools/anima/prep-lib-manifest.mjs        (npm run anima:prep-libs)
import { readFileSync, writeFileSync, existsSync, statSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const VENDOR = join(HERE, '..', '..', 'apps', 'anima', 'www', 'forge', 'vendor');

// The runtime-lib closure, per engine. `path` is relative to vendor/ (and to the served
// /apps/anima/forge/vendor/ URL). `role` distinguishes ESM module vs wasm artifact.
const LIBS = [
  { name: 'webllm-esm', engine: 'webgpu', role: 'esm', path: 'web-llm.js' },
  { name: 'webllm-model-lib', engine: 'webgpu', role: 'model_lib', path: 'Qwen2-0.5B-Instruct-q4f16_1_cs1k-webgpu.wasm' },
  { name: 'wllama-esm', engine: 'wasm', role: 'esm', path: 'wllama.mjs' },
  { name: 'wllama-wasm-st', engine: 'wasm', role: 'wasm-st', path: 'wllama/single-thread/wllama.wasm' },
  { name: 'wllama-wasm-mt', engine: 'wasm', role: 'wasm-mt', path: 'wllama/multi-thread/wllama.wasm', needsCrossOriginIsolation: true },
];

const libs = [];
let missing = 0;
for (const l of LIBS) {
  const abs = join(VENDOR, l.path);
  if (!existsSync(abs)) { console.warn('  MISSING (skipped): ' + l.path); missing++; continue; }
  const buf = readFileSync(abs);
  const sha256 = createHash('sha256').update(buf).digest('hex');
  libs.push({ name: l.name, engine: l.engine, role: l.role, path: l.path, bytes: statSync(abs).size, sha256, ...(l.needsCrossOriginIsolation ? { needsCrossOriginIsolation: true } : {}) });
  console.log(`  ${l.path}  ${buf.length} bytes  ${sha256.slice(0, 12)}…`);
}

const manifest = { schema: 'forge-lib-manifest@1', generator: 'prep-lib-manifest.mjs', libs };
const outPath = join(VENDOR, 'lib-manifest.json');
writeFileSync(outPath, JSON.stringify(manifest, null, 2) + '\n');
console.log(`\nwrote ${outPath}  (${libs.length} libs${missing ? `, ${missing} missing` : ''})`);
if (missing) process.exitCode = 0; // missing libs are a provisioning state, not a build error

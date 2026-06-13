// prep-llm-models.mjs — provision an LLM model directory for ANIMA Forge: scan the staged weight
// files, compute a SHA-256 per file, and write a client-verifiable manifest.json. The manifest pins
// the revision and lets download.js (verifyManifest / pendingShards) stream + integrity-check the
// weights from the device SD or a CDN. The big params_shard_*.bin go under `shards` (the
// download-manager streams them with BOUNDED Range windows); small config/tokenizer files under `aux`.
//
//   node tools/anima/prep-llm-models.mjs <modelDir> [--revision <sha>] [--format mlc|gguf]
//   (re-run after re-downloading; the manifest is regenerated deterministically from the files.)
import { readdirSync, statSync, readFileSync, writeFileSync, createReadStream } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, basename } from 'node:path';

const args = process.argv.slice(2);
const dir = args.find((a) => !a.startsWith('--'));
if (!dir) { console.error('usage: node tools/anima/prep-llm-models.mjs <modelDir> [--revision <sha>] [--format mlc|gguf]'); process.exit(1); }
const revision = (args[args.indexOf('--revision') + 1] && !args[args.indexOf('--revision') + 1].startsWith('--')) ? args[args.indexOf('--revision') + 1] : 'local';
const format = (args.includes('--format') ? args[args.indexOf('--format') + 1] : null) || (existsShard(dir) ? 'mlc' : 'gguf');

function existsShard(d) { try { return readdirSync(d).some((f) => /params_shard_\d+\.bin$/.test(f)); } catch { return false; } }
function sha256(path) {
  return new Promise((res, rej) => {
    const h = createHash('sha256'); const s = createReadStream(path);
    s.on('data', (c) => h.update(c)); s.on('end', () => res(h.digest('hex'))); s.on('error', rej);
  });
}

const SKIP = new Set(['.gitattributes', 'README.md', 'manifest.json']);
const isShard = (n) => /params_shard_\d+\.bin$/.test(n) || /\.gguf(\.\d+)?$/i.test(n);

const all = readdirSync(dir).filter((f) => { try { return statSync(join(dir, f)).isFile() && !SKIP.has(f); } catch { return false; } });
const shards = [], aux = [];
let total = 0;
for (const f of all.sort()) {
  const bytes = statSync(join(dir, f)).size;
  const sha = await sha256(join(dir, f));
  total += bytes;
  (isShard(f) ? shards : aux).push({ name: f, bytes, sha256: sha });
}

let model = basename(dir), ctx = null, modelType = null;
try { const c = JSON.parse(readFileSync(join(dir, 'mlc-chat-config.json'), 'utf8')); modelType = c.model_type; ctx = c.context_window_size || c.max_window_size; } catch { /* gguf: no mlc config */ }

const manifest = { model, format, revision, modelType, contextWindow: ctx, totalBytes: total, shards, aux };
writeFileSync(join(dir, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n');
console.log(`[prep-llm-models] ${model} (${format}) rev=${revision}`);
console.log(`  ${shards.length} shard(s) + ${aux.length} aux file(s), ${(total / 1e6).toFixed(1)} MB`);
console.log(`  -> ${join(dir, 'manifest.json')}`);

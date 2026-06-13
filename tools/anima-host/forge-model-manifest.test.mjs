// Gate: ANIMA Forge — staged LLM model integrity. Verifies any model provisioned under the SD
// payload (deploy/sd-safe/apps/anima/www/forge/models/<id>/manifest.json): the manifest is well-formed
// (download.verifyManifest), every shard/aux file exists with the declared byte size, the mlc config is
// present, and one file's SHA-256 matches (proves the manifest hashes are real without rehashing ~290 MB).
// SKIPS cleanly when no model is staged (the weights are a large optional artifact, not always present).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readdirSync, statSync, existsSync, createReadStream, readFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { verifyManifest } from '../../apps/anima/www/forge/download.js';

const here = dirname(fileURLToPath(import.meta.url));
const modelsDir = join(here, '..', '..', 'deploy', 'sd-safe', 'apps', 'anima', 'www', 'forge', 'models');

function sha256(path) {
  return new Promise((res, rej) => { const h = createHash('sha256'); const s = createReadStream(path); s.on('data', (c) => h.update(c)); s.on('end', () => res(h.digest('hex'))); s.on('error', rej); });
}
const staged = existsSync(modelsDir)
  ? readdirSync(modelsDir).filter((d) => existsSync(join(modelsDir, d, 'manifest.json')))
  : [];

test('staged LLM models: manifest + files are consistent (or none staged → skip)', { skip: staged.length === 0 ? 'no model staged under deploy/sd-safe (optional large artifact)' : false }, async () => {
  for (const id of staged) {
    const dir = join(modelsDir, id);
    const manifest = JSON.parse(readFileSync(join(dir, 'manifest.json'), 'utf8'));
    // shards must satisfy the download manifest contract (download.js streams these with bounded Range)
    const vm = verifyManifest({ model: manifest.model, revision: manifest.revision, shards: manifest.shards });
    assert.ok(vm.ok, `${id}: bad manifest: ${vm.errors.join(', ')}`);
    assert.match(manifest.revision, /^[0-9a-f]{7,}$/i, `${id}: revision should be a pinned commit sha`);

    const files = [...manifest.shards, ...manifest.aux];
    assert.ok(files.length >= 1 && manifest.shards.length >= 1, `${id}: no weight files`);   // GGUF = a single shard; MLC = many
    for (const f of files) {
      const p = join(dir, f.name);
      assert.ok(existsSync(p), `${id}: missing ${f.name}`);
      assert.equal(statSync(p).size, f.bytes, `${id}: size mismatch ${f.name}`);
      assert.match(f.sha256, /^[0-9a-f]{64}$/i, `${id}: bad sha for ${f.name}`);
    }
    // MLC weights need the runtime config + tokenizer to load in WebLLM
    if (manifest.format === 'mlc') {
      for (const need of ['mlc-chat-config.json', 'ndarray-cache.json', 'tokenizer.json'])
        assert.ok(existsSync(join(dir, need)), `${id}: MLC needs ${need}`);
    }
    // prove the recorded hashes are REAL: rehash the smallest file and compare
    const smallest = files.slice().sort((a, b) => a.bytes - b.bytes)[0];
    assert.equal(await sha256(join(dir, smallest.name)), smallest.sha256, `${id}: sha mismatch on ${smallest.name}`);
  }
  console.log(`[model-manifest] verified ${staged.length} staged model(s): ${staged.join(', ')}`);
});

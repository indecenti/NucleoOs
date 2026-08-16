// Shared guard for the ANIMA Forge weight tests: which models are ACTUALLY present?
// The model weights are "a large optional artifact, not always present" — under Git LFS /
// oversized-assets, so a fresh clone (or CI, or this dev machine) ships only the manifest.json
// and 133-byte LFS pointer stubs, never the multi-MB shards. A test that keys "staged" off the
// mere existence of manifest.json then runs against absent/stub files and fails with a confusing
// length/size mismatch instead of skipping. So: a model counts as staged only when every
// declared shard+aux file exists on disk AT its declared byte size (which also rejects LFS stubs).
import { readdirSync, existsSync, statSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

// Returns the ids under modelsDir whose weights are fully present (safe to test end-to-end).
export function listStagedModels(modelsDir) {
  if (!existsSync(modelsDir)) return [];
  return readdirSync(modelsDir).filter((id) => {
    const dir = join(modelsDir, id);
    const mp = join(dir, 'manifest.json');
    if (!existsSync(mp)) return false;
    let manifest;
    try { manifest = JSON.parse(readFileSync(mp, 'utf8')); } catch { return false; }
    const files = [...(manifest.shards || []), ...(manifest.aux || [])];
    if (files.length === 0) return false;
    return files.every((f) => {
      const p = join(dir, f.name);
      return existsSync(p) && statSync(p).size === f.bytes;   // real weight, not an LFS pointer stub
    });
  });
}

// Gate: ANIMA Forge — vendored runtime-lib manifest is HONEST. The air-gap readiness + integrity
// checks trust vendor/lib-manifest.json as ground truth; if someone re-vendors a lib without re-running
// the generator, the manifest goes stale and readiness would (correctly) flag a present file as corrupt
// — or, worse, bless a wrong file by matching a stale size. This gate catches that drift in CI before
// a flash: every manifest entry must point at a real file whose size + sha256 still match.
//   Regenerate with:  npm run anima:prep-libs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync, statSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const VENDOR = join(HERE, '..', '..', 'apps', 'anima', 'www', 'forge', 'vendor');
const MANIFEST = join(VENDOR, 'lib-manifest.json');

test('lib-manifest.json exists and has the expected schema', () => {
  assert.ok(existsSync(MANIFEST), 'missing vendor/lib-manifest.json — run npm run anima:prep-libs');
  const m = JSON.parse(readFileSync(MANIFEST, 'utf8'));
  assert.equal(m.schema, 'forge-lib-manifest@1');
  assert.ok(Array.isArray(m.libs) && m.libs.length >= 1);
  for (const l of m.libs) {
    assert.ok(l.name && l.path && l.engine && l.role, 'lib entry missing fields: ' + JSON.stringify(l));
    assert.ok(Number.isInteger(l.bytes) && l.bytes > 0, 'bad bytes for ' + l.path);
    assert.match(l.sha256, /^[0-9a-f]{64}$/, 'bad sha256 for ' + l.path);
  }
});

test('every vendored lib present on disk matches its manifest size + sha256 (no drift)', () => {
  const m = JSON.parse(readFileSync(MANIFEST, 'utf8'));
  let checked = 0;
  for (const l of m.libs) {
    const abs = join(VENDOR, l.path);
    if (!existsSync(abs)) continue;                 // a not-yet-synced lib is a provisioning state, not drift
    assert.equal(statSync(abs).size, l.bytes, 'SIZE DRIFT on ' + l.path + ' — re-run npm run anima:prep-libs');
    const sha = createHash('sha256').update(readFileSync(abs)).digest('hex');
    assert.equal(sha, l.sha256, 'SHA DRIFT on ' + l.path + ' — re-run npm run anima:prep-libs');
    checked++;
  }
  assert.ok(checked >= 1, 'no vendored libs found to verify');
});

test('manifest covers both local engines (webgpu + wasm) so readiness has ground truth for each', () => {
  const m = JSON.parse(readFileSync(MANIFEST, 'utf8'));
  const engines = new Set(m.libs.map((l) => l.engine));
  assert.ok(engines.has('webgpu'), 'no webgpu libs in manifest');
  assert.ok(engines.has('wasm'), 'no wasm libs in manifest');
  // the wasm multi-thread binary must be flagged as needing cross-origin isolation
  const mt = m.libs.find((l) => l.role === 'wasm-mt');
  if (mt) assert.equal(mt.needsCrossOriginIsolation, true);
});

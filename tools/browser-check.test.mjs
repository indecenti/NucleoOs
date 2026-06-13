// Guards the removal of the Ultraviolet in-iframe web proxy from the Browser app.
//
// The Cardputer can't be a "bare server" (no PSRAM, ~18KB runtime heap, TLS already barely fits —
// see the ANIMA online heap saga), and pointing UV at a public bare server (tomp.app) just trades
// the OOM for a CORS wall. So the Browser app now hands external URLs off to a real browser tab via
// window.open(): it works, and costs the device nothing. This test makes sure UV stays gone and that
// the gzipped sibling the firmware actually serves was regenerated (the classic "edited the .html,
// forgot the .gz" deploy trap).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');

const SRC = join(REPO, 'apps', 'browser', 'www');
// Locations the firmware/SD actually serve from. H: is the live SD — only present when mounted.
const COPIES = [
  join(REPO, 'deploy', 'sd', 'apps', 'browser', 'www'),
  join(REPO, 'deploy', 'sd-safe', 'apps', 'browser', 'www'),
  ...(existsSync('H:\\apps\\browser\\www') ? ['H:\\apps\\browser\\www'] : []),
];

// Markers that only exist if the Ultraviolet proxy is still wired in.
const FORBIDDEN = ['uv.bundle.js', 'uv.config.js', 'uv.client.js', 'uv.handler.js', 'uv.sw.js',
  '__uv', 'UVServiceWorker', 'tomp.app', 'encodeUrl', 'serviceWorker.register'];
// Proof the honest new-tab handoff replaced it.
const REQUIRED = ['window.open(', 'createHandoffHtml'];

const isUvFile = (n) => /^uv\./i.test(n) || /^sw\.js(\.gz)?$/i.test(n);

test('source www has no Ultraviolet / proxy SW files', () => {
  const files = readdirSync(SRC);
  const leftover = files.filter(isUvFile);
  assert.deepEqual(leftover, [], `UV/SW files still in source: ${leftover.join(', ')}`);
});

test('source index.html dropped UV, added the new-tab handoff', () => {
  const html = readFileSync(join(SRC, 'index.html'), 'utf8');
  for (const bad of FORBIDDEN) assert.ok(!html.includes(bad), `index.html still references "${bad}"`);
  for (const need of REQUIRED) assert.ok(html.includes(need), `index.html missing "${need}"`);
});

const srcHtml = readFileSync(join(SRC, 'index.html'));

for (const dir of COPIES) {
  test(`deploy copy clean + gz in sync: ${dir}`, () => {
    // No UV files survive in the served tree.
    const leftover = readdirSync(dir).filter(isUvFile);
    assert.deepEqual(leftover, [], `UV/SW files in ${dir}: ${leftover.join(', ')}`);

    // The raw index.html matches source exactly.
    const raw = readFileSync(join(dir, 'index.html'));
    assert.ok(raw.equals(srcHtml), `index.html in ${dir} differs from source`);

    // The .gz the firmware serves must exist AND decompress to the same bytes (the deploy trap).
    const gzPath = join(dir, 'index.html.gz');
    assert.ok(existsSync(gzPath), `missing ${gzPath} — firmware serves the .gz, regenerate it`);
    const ungz = gunzipSync(readFileSync(gzPath));
    assert.ok(ungz.equals(srcHtml), `index.html.gz in ${dir} is stale — re-run tools/gzip-assets.mjs`);
  });
}

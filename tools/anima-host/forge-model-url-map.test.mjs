// Gate: ANIMA offline-installer — the service-worker URL→install-cache mapping. The shell SW closes the
// "install → runs offline" loop by serving model weights from caches['anima-forge-models'] at '/fc/<id>/
// <file>'. That only works if the mapping from the URLs WebLLM/wllama ACTUALLY request (the device SD path
// and the HF CDN, both built from the registry's sdBase/cdnBase) lands on the exact key the installer wrote.
// This pins forgeModelKey against EVERY real registry URL — so a base/pattern drift fails the build, not a
// user's offline session. (web/shell/sw.js inlines a byte-identical copy of forgeModelKey.)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { forgeModelKey } from '../../apps/anima/www/forge/model-url-map.js';
import { REGISTRY } from '../../apps/anima/www/forge/model-store.js';

// Extract the ACTUAL forgeModelKey the shell service worker ships (a classic SW can't import the ES module,
// so it inlines a copy). Evaluating the real source lets us assert byte-for-byte BEHAVIOURAL parity below —
// the only thing that matters — instead of a brittle text compare that trips on a comment.
const repo = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const swSrc = readFileSync(join(repo, 'web', 'shell', 'sw.js'), 'utf8');
const swFnSrc = (swSrc.match(/function forgeModelKey\(url\)\s*\{[\s\S]*?\n\}/) || [])[0];
const swForgeModelKey = swFnSrc ? new Function('return (' + swFnSrc + ')')() : null;

test('every registry shard+aux maps from BOTH its SD url and its CDN url to the exact /fc/<id>/<file> key', () => {
  let checked = 0;
  for (const m of REGISTRY) {
    const files = [...m.manifest.shards, ...(m.manifest.aux || [])];
    assert.ok(files.length, m.id + ' has files');
    for (const f of files) {
      const want = '/fc/' + m.id + '/' + f.name;
      // SD: an absolute request to the device path (sdBase = /apps/anima/www/forge/models/<id>/).
      assert.equal(forgeModelKey('https://192.168.0.166' + m.sdBase + f.name), want, 'SD ' + m.id + '/' + f.name);
      // CDN: WebLLM requests cdnBase + file verbatim (huggingface.co/<org>/<id>/resolve/<rev>/<file>).
      assert.equal(forgeModelKey(m.cdnBase + f.name), want, 'CDN ' + m.id + '/' + f.name);
      checked++;
    }
  }
  assert.ok(checked >= 9, 'covered all registry files (got ' + checked + ')');
});

test('non-model URLs are left alone (null → SW passthrough), so nothing else is intercepted', () => {
  for (const u of [
    '/api/fs/read?path=/data/x.json',
    'https://192.168.0.166/apps/anima/www/index.html',
    'https://192.168.0.166/apps/anima/www/forge/vendor/web-llm.js',   // the model_lib/vendor is NOT a weight
    'https://192.168.0.166/dlgate.js',
    'https://huggingface.co/mlc-ai/SomeModel',                        // a bare model page, not a /resolve/ file
    'https://example.com/forge/somethingelse/file.bin',
  ]) assert.equal(forgeModelKey(u), null, 'ignored: ' + u);
});

test('cache-busting query/hash on a model URL is stripped before mapping', () => {
  assert.equal(forgeModelKey('https://h/apps/anima/www/forge/models/MyModel/params_shard_0.bin?t=42'), '/fc/MyModel/params_shard_0.bin');
  assert.equal(forgeModelKey('https://huggingface.co/Qwen/MyGGUF/resolve/main/m.gguf#frag'), '/fc/MyGGUF/m.gguf');
});

test('the installer copy in apps/anima/www/index.html (_fmCache/_fmHas/_fmPut) uses the SAME cache name and key shape', () => {
  // The production installer WRITES through _fmPut only; the SW READS through MODEL_CACHE + forgeModelKey.
  // If either the cache name or the '/fc/<id>/<file>' key shape drifts, installs silently stop being served
  // offline (both sides of the app drift together, so only a cross-file pin can catch it).
  const appSrc = readFileSync(join(repo, 'apps', 'anima', 'www', 'index.html'), 'utf8');
  const cacheName = (appSrc.match(/const _fmCache\s*=\s*'([^']+)'/) || [])[1];
  assert.equal(cacheName, 'anima-forge-models', '_fmCache matches the SW MODEL_CACHE name');
  const swCacheName = (swSrc.match(/const MODEL_CACHE\s*=\s*'([^']+)'/) || [])[1];
  assert.equal(cacheName, swCacheName, '_fmCache vs sw.js MODEL_CACHE');
  // Key shape: both _fmHas and _fmPut must build keys exactly as '/fc/'+id+'/'+n.
  const keyUses = appSrc.match(/'\/fc\/'\s*\+\s*id\s*\+\s*'\/'\s*\+\s*n/g) || [];
  assert.ok(keyUses.length >= 2, "_fmHas and _fmPut both use '/fc/'+id+'/'+n (got " + keyUses.length + ')');
  // And that shape must be exactly what forgeModelKey produces for a registry file.
  const m = REGISTRY[0], f = m.manifest.shards[0];
  assert.equal(forgeModelKey(m.cdnBase + f.name), '/fc/' + m.id + '/' + f.name, 'installer key == SW lookup key');
});

test('the inlined copy in web/shell/sw.js is BEHAVIOURALLY identical (no drift between SW and module)', () => {
  assert.ok(swForgeModelKey, 'forgeModelKey found in web/shell/sw.js');
  const urls = [];
  for (const m of REGISTRY) for (const f of [...m.manifest.shards, ...(m.manifest.aux || [])]) {
    urls.push('https://192.168.0.166' + m.sdBase + f.name, m.cdnBase + f.name);
  }
  urls.push('/api/fs/read?path=x', 'https://192.168.0.166/apps/anima/www/index.html',
    'https://192.168.0.166/apps/anima/www/forge/vendor/web-llm.js', 'https://huggingface.co/mlc-ai/Bare',
    'https://h/apps/anima/www/forge/models/M/params_shard_0.bin?t=42');
  for (const u of urls) assert.equal(swForgeModelKey(u), forgeModelKey(u), 'SW vs module parity for ' + u);
});

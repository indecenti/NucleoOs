// The apps.json merge rule (tools/lib/registry-merge.mjs + its Python twin in sd_deploy.py): both held to
// the shared cases in tools/lib/registry-merge-vectors.json, byte for byte, plus the CLI the PowerShell
// tools call. Run: node --test tools/registry-merge.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, writeFileSync, mkdtempSync, rmSync, existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { mergeRegistryText } from './lib/registry-merge.mjs';

const { cases } = JSON.parse(readFileSync('tools/lib/registry-merge-vectors.json', 'utf8'));
const PY = ['python', 'python3'].find((b) => spawnSync(b, ['--version']).status === 0);

function check(name, got, exp, releaseText) {
  const ids = JSON.parse(got.text.replace(/^﻿/, '')).installed.map((e) => e.id);
  assert.deepEqual(ids, exp.installed, `${name}: installed ids`);
  assert.deepEqual(got.kept, exp.kept, `${name}: kept`);
  assert.deepEqual(got.shadowed, exp.shadowed, `${name}: shadowed`);
  assert.equal(got.deviceReadable, exp.deviceReadable, `${name}: deviceReadable`);
  if (exp.same_as_release) assert.equal(got.text, releaseText, `${name}: must be the release text byte-exact`);
  else assert.notEqual(got.text, releaseText, `${name}: must differ from the release`);
  if (exp.text) assert.equal(got.text, exp.text, `${name}: exact text`);
  if (exp.entry) {
    const e = JSON.parse(got.text.replace(/^﻿/, '')).installed.find((x) => x.id === exp.entry.id);
    assert.deepEqual(e, exp.entry, `${name}: carried entry unchanged`);
  }
}

test('JS merge satisfies every shared case', () => {
  for (const c of cases) check(c.name, mergeRegistryText(c.release, c.device), c.expect, c.release);
});

test('Python twin satisfies every shared case, byte-identical to JS', () => {
  assert.ok(PY, 'Python 3 is required');
  const code = `import sys, json; sys.path.insert(0, 'tools/nucleo-sd-deploy'); import sd_deploy as S
cases = json.load(open('tools/lib/registry-merge-vectors.json', encoding='utf-8'))['cases']
out = []
for c in cases:
    t, k, s, r = S.merge_registry_text(c['release'], c['device'])
    out.append({'text': t, 'kept': k, 'shadowed': s, 'deviceReadable': r})
sys.stdout.buffer.write(json.dumps(out).encode('utf-8'))`;
  const r = spawnSync(PY, ['-c', code], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  const got = JSON.parse(r.stdout);
  cases.forEach((c, i) => {
    check(`py: ${c.name}`, got[i], c.expect, c.release);
    assert.equal(got[i].text, mergeRegistryText(c.release, c.device).text, `py/js differ: ${c.name}`);
  });
});

test('malformed RELEASE is an error, never a silent overwrite', () => {
  assert.throws(() => mergeRegistryText('{"installed": 5}', null));
  assert.throws(() => mergeRegistryText('nope', '{"installed":[]}'));
});

test('CLI: merges in place, reports, leaves an unchanged card untouched', () => {
  const d = mkdtempSync(join(tmpdir(), 'regmerge-'));
  try {
    const rel = join(d, 'release.json'), dev = join(d, 'device.json');
    const c = cases.find((x) => x.name.startsWith('agent app carried'));
    writeFileSync(rel, c.release); writeFileSync(dev, c.device);
    let r = spawnSync(process.execPath, ['tools/lib/registry-merge.mjs', rel, dev, dev], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    assert.deepEqual(JSON.parse(r.stdout), { changed: true, kept: ['mine'], shadowed: [], deviceReadable: true });
    assert.equal(readFileSync(dev, 'utf8'), c.expect.text);
    r = spawnSync(process.execPath, ['tools/lib/registry-merge.mjs', rel, dev, dev], { encoding: 'utf8' });
    assert.equal(JSON.parse(r.stdout).changed, false, 'second run is a no-op');
    const out = join(d, 'fresh.json');
    r = spawnSync(process.execPath, ['tools/lib/registry-merge.mjs', rel, '-', out], { encoding: 'utf8' });
    assert.equal(r.status, 0); assert.equal(readFileSync(out, 'utf8'), c.release, 'no device file -> the release copy');
    writeFileSync(join(d, 'bad.json'), 'x');
    r = spawnSync(process.execPath, ['tools/lib/registry-merge.mjs', join(d, 'bad.json'), dev, join(d, 'o2.json')], { encoding: 'utf8' });
    assert.equal(r.status, 1, 'malformed release exits 1'); assert.ok(!existsSync(join(d, 'o2.json')));
  } finally { rmSync(d, { recursive: true, force: true }); }
});

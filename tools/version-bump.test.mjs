// tools/version-bump.ps1 -Bump patch is THE documented way to cut a release (docs/versioning.md, the
// nucleo-release skill). It moved only firmware/version/{VERSION,BUILD}, so the very next CI run failed
// tools/version-consistency.test.mjs. Run the real script on a scratch copy and check that a release bump
// carries the version everywhere, and that a plain build bump touches nothing else. Skips without PowerShell.
import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, mkdirSync, copyFileSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const repo = join(dirname(fileURLToPath(import.meta.url)), '..');
const shell = ['powershell', 'pwsh'].find((s) => spawnSync(s, ['-NoProfile', '-Command', 'exit 0']).status === 0);

function scratch() {
  const d = mkdtempSync(join(tmpdir(), 'nucleo-vbump-'));
  mkdirSync(join(d, 'tools')); mkdirSync(join(d, 'firmware', 'version'), { recursive: true });
  copyFileSync(join(repo, 'tools', 'version-bump.ps1'), join(d, 'tools', 'version-bump.ps1'));
  writeFileSync(join(d, 'firmware', 'version', 'VERSION'), '0.6.0');
  writeFileSync(join(d, 'firmware', 'version', 'BUILD'), '41');
  writeFileSync(join(d, 'package.json'), '{\n  "name": "nucleoos",\n  "version": "0.6.0",\n  "dependencies": { "x": { "version": "9.9.9" } }\n}\n');
  writeFileSync(join(d, 'CITATION.cff'), 'cff-version: 1.2.0\nversion: 0.6.0\ndate-released: "2026-10-04"\n');
  writeFileSync(join(d, 'CHANGELOG.md'), '# Changelog\n\n## [Unreleased]\n\n### Fixed\n- a thing\n\n## [0.6.0] — 2026-10-04\n');
  return d;
}
const run = (d, ...args) => spawnSync(shell, ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', join(d, 'tools', 'version-bump.ps1'), ...args], { encoding: 'utf8' });
const rd = (d, p) => readFileSync(join(d, p), 'utf8');

test('a release bump carries the new version to package.json, CITATION.cff and the CHANGELOG', { skip: !shell && 'no PowerShell' }, () => {
  const d = scratch();
  try {
    const r = run(d, '-Bump', 'patch');
    assert.equal(r.status, 0, r.stderr);
    assert.equal(rd(d, 'firmware/version/VERSION'), '0.6.1');
    assert.equal(rd(d, 'firmware/version/BUILD'), '0');
    const pkg = JSON.parse(rd(d, 'package.json'));
    assert.equal(pkg.version, '0.6.1');
    assert.equal(pkg.dependencies.x.version, '9.9.9', 'only the top-level version moves');
    assert.match(rd(d, 'CITATION.cff'), /^version: 0\.6\.1$/m);
    const heads = [...rd(d, 'CHANGELOG.md').matchAll(/^## \[([^\]]+)\](.*)$/gm)].map((m) => m[1] + m[2]);
    assert.equal(heads[0], 'Unreleased');
    assert.match(heads[1], /^0\.6\.1 — \d{4}-\d{2}-\d{2}$/, 'a dated section with a real em dash: ' + heads[1]);
    assert.match(rd(d, 'CHANGELOG.md'), /## \[0\.6\.1\][^\n]*\n\n### Fixed\n- a thing/, 'the unreleased notes now sit under the release');
  } finally { rmSync(d, { recursive: true, force: true }); }
});

test('a build bump moves only the build counter', { skip: !shell && 'no PowerShell' }, () => {
  const d = scratch();
  try {
    const before = ['package.json', 'CITATION.cff', 'CHANGELOG.md'].map((p) => rd(d, p));
    const r = run(d);
    assert.equal(r.status, 0, r.stderr);
    assert.equal(rd(d, 'firmware/version/VERSION'), '0.6.0');
    assert.equal(rd(d, 'firmware/version/BUILD'), '42');
    assert.deepEqual(['package.json', 'CITATION.cff', 'CHANGELOG.md'].map((p) => rd(d, p)), before);
  } finally { rmSync(d, { recursive: true, force: true }); }
});

// ONE device/user-state table for every SD tool (tools/lib/sd-policy.json). Proves that every reader —
// tools/lib/sd-policy.mjs (push-ota, sd-net-sync), sd_deploy.py is_state, tools/lib/sd-policy.ps1 (deploy.ps1,
// sd-sync.ps1) — gives the same verdict on every row of tools/lib/sd-policy-vectors.json, and that no tool
// still carries its own hand-kept list (the drift this table replaced). Run: node --test tools/sd-policy.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { isDeviceState, globToRegExp } from './lib/sd-policy.mjs';

const vec = JSON.parse(readFileSync('tools/lib/sd-policy-vectors.json', 'utf8')).state;
const paths = Object.keys(vec);
const PY = ['python', 'python3'].find((b) => spawnSync(b, ['--version']).status === 0);

test('JS reader matches every vector', () => {
  for (const p of paths) assert.equal(isDeviceState(p), vec[p], `js ${JSON.stringify(p)}`);
});

test('Python reader (sd_deploy.is_state) matches every vector', () => {
  assert.ok(PY, 'Python 3 is required');
  const r = spawnSync(PY, ['-c', "import sys,json; sys.path.insert(0,'tools/nucleo-sd-deploy'); import sd_deploy as S; " +
    "print(json.dumps([S.is_state(p) for p in json.load(open('tools/lib/sd-policy-vectors.json',encoding='utf-8'))['state']]))"],
    { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  JSON.parse(r.stdout).forEach((got, i) => assert.equal(got, vec[paths[i]], `python ${JSON.stringify(paths[i])}`));
});

test('PowerShell reader (deploy.ps1 / sd-sync.ps1) matches every vector', { skip: process.platform !== 'win32' }, () => {
  const r = spawnSync('powershell', ['-NoProfile', '-Command',
    ". ./tools/lib/sd-policy.ps1; $input | ForEach-Object { if (Is-DeviceState $_) { '1' } else { '0' } }"],
    { encoding: 'utf8', input: paths.join('\n') + '\n' });
  assert.equal(r.status, 0, r.stderr);
  const out = r.stdout.trim().split(/\r?\n/);
  assert.equal(out.length, paths.length);
  out.forEach((l, i) => assert.equal(l.trim() === '1', vec[paths[i]], `powershell ${JSON.stringify(paths[i])}`));
});

test('every SD tool reads the shared table and carries no private list', () => {
  const uses = {
    'tools/push-ota.mjs': "./lib/sd-policy.mjs", 'tools/sd-net-sync.mjs': "./lib/sd-policy.mjs",
    'tools/deploy.ps1': 'lib/sd-policy.ps1', 'tools/sd-sync.ps1': 'lib/sd-policy.ps1',
    'tools/nucleo-sd-deploy/sd_deploy.py': 'sd-policy.json',
  };
  for (const [f, needle] of Object.entries(uses)) assert.ok(readFileSync(f, 'utf8').includes(needle), `${f} must use ${needle}`);
  // the hand-kept lists this table replaced must not come back
  const gone = { 'tools/push-ota.mjs': [/STATE_EXACT/, /STATE_DIRS/], 'tools/sd-net-sync.mjs': [/PROT_FILES/, /PROT_DIRS/],
    'tools/deploy.ps1': [/^\$STATE = @\(/m], 'tools/sd-sync.ps1': [/'teacher\.json','telemetry/],
    'tools/nucleo-sd-deploy/sd_deploy.py': [/^DEVICE_STATE = \[/m] };
  for (const [f, res] of Object.entries(gone)) for (const re of res) assert.ok(!re.test(readFileSync(f, 'utf8')), `${f} still has ${re}`);
});

test('pattern semantics: * stays in a segment, ** crosses, case-insensitive', () => {
  assert.ok(globToRegExp('data/tts/*.cfg').test('data/tts/speak.cfg'));
  assert.ok(!globToRegExp('data/tts/*.cfg').test('data/tts/it/speak.cfg'));
  assert.ok(globToRegExp('**/*.vec').test('a.vec') && globToRegExp('**/*.vec').test('x/y/a.VEC'));
  assert.ok(globToRegExp('system/config/**').test('System/Config/a/b'));
  assert.ok(!globToRegExp('auth.json').test('x/auth.json'), 'a root file pattern is root-only');
});

test('the release payload holds no device state under the shared table', () => {
  // sd_deploy.py release refuses state (release_path_allowed -> is_state), and its allow-list is covered by
  // tools/sd-payload.test.mjs; here: every "allow" vector the release accepts is NOT state.
  const allow = JSON.parse(readFileSync('tools/nucleo-sd-deploy/allowlist-vectors.json', 'utf8')).allow;
  for (const [p, ok] of Object.entries(allow)) if (ok) assert.equal(isDeviceState(p), false, `release-allowed path is state: ${p}`);
});

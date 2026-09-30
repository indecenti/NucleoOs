// Release SD payload (tools/nucleo-sd-deploy/sd_deploy.py `release`): what the release zip carries and what
// the device's SD-content installer downloads file by file (docs/sd-content-install.md). Builds the real
// payload from the sources into a temp dir and proves: the manifest is exact (every line's size + SHA-256
// matches the tree, header counts add up), nothing outside the device write allow-list ships (shared
// vectors in allowlist-vectors.json — the firmware policy is gated on the same table), no device state /
// heavy models / orphan knowledge shards, every .gz twin is byte-exact, apps.json is marked merge-only,
// and the core pack stays within budget. Run: node --test tools/sd-payload.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, existsSync, statSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { gunzipSync } from 'node:zlib';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const ROOT = process.cwd();
const TOOL = 'tools/nucleo-sd-deploy/sd_deploy.py';
const PY = ['python', 'python3'].find((b) => spawnSync(b, ['--version']).status === 0);
const CORE_BUDGET = 64 * 1048576;          // a regression alarm, not a target: core is ~51 MB today
const TOTAL_BUDGET = 150 * 1048576;

function py(code) {
  const r = spawnSync(PY, ['-c', `import sys; sys.path.insert(0, 'tools/nucleo-sd-deploy'); import sd_deploy as S\n${code}`],
    { cwd: ROOT, encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  return r.stdout;
}

const tmp = mkdtempSync(join(tmpdir(), 'sdpayload-'));
const out = join(tmp, 'sd');
const man = join(tmp, 'sd-manifest.txt');
let build;
test.before(() => {
  build = spawnSync(PY, [TOOL, 'release', out, '--tag', 'v9.9.9', '--manifest', man], { cwd: ROOT, encoding: 'utf8' });
});
test.after(() => rmSync(tmp, { recursive: true, force: true }));

function parse() {
  const text = readFileSync(man, 'ascii');
  assert.ok(text.endsWith('\n'), 'manifest ends with a newline');
  const lines = text.split('\n').filter(Boolean);
  const head = /^#nucleoos-sd 1 (\S+) (\d+) (\d+)$/.exec(lines[0]);
  const packs = {}, entries = [];
  for (const l of lines.slice(1)) {
    const p = /^#pack ([a-z0-9-]+) (\d+) (\d+)$/.exec(l);
    if (p) { packs[p[1]] = { files: +p[2], bytes: +p[3] }; continue; }
    const m = /^([0-9a-f]{64}) (\d+) ([a-z0-9-]+) ([wcm]) (.+)$/.exec(l);
    assert.ok(m, `malformed manifest line: ${l}`);
    entries.push({ sha: m[1], size: +m[2], pack: m[3], mode: m[4], path: m[5] });
  }
  return { head, packs, entries };
}

test('builds cleanly from the sources', () => {
  assert.ok(PY, 'Python 3 is required');
  assert.equal(build.status, 0, `sd_deploy.py release failed:\n${build.stdout}\n${build.stderr}`);
});

test('manifest header, pack totals and every line match the tree exactly', () => {
  const { head, packs, entries } = parse();
  assert.ok(head, 'header line');
  assert.equal(head[1], 'v9.9.9');
  assert.equal(+head[2], entries.length);
  assert.equal(+head[3], entries.reduce((a, e) => a + e.size, 0));
  for (const [k, v] of Object.entries(packs)) {
    const es = entries.filter((e) => e.pack === k);
    assert.equal(v.files, es.length, `pack ${k} file count`);
    assert.equal(v.bytes, es.reduce((a, e) => a + e.size, 0), `pack ${k} bytes`);
  }
  const paths = entries.map((e) => e.path);
  assert.deepEqual(paths, [...paths].sort(), 'lines sorted by path');
  assert.equal(new Set(paths).size, paths.length, 'no duplicate paths');
  for (const e of entries) {
    const f = join(out, e.path);
    assert.ok(existsSync(f), `listed but missing: ${e.path}`);
    assert.equal(statSync(f).size, e.size, `size ${e.path}`);
    assert.equal(createHash('sha256').update(readFileSync(f)).digest('hex'), e.sha, `sha ${e.path}`);
  }
});

test('shared allow-list vectors agree with sd_deploy.py', () => {
  const vec = JSON.parse(readFileSync('tools/nucleo-sd-deploy/allowlist-vectors.json', 'utf8')).allow;
  const got = JSON.parse(py(`import json\nv=json.load(open('tools/nucleo-sd-deploy/allowlist-vectors.json',encoding='utf-8'))['allow']\n`
    + `print(json.dumps({k: S.release_path_allowed(k) == '' for k in v}))`));
  for (const [p, want] of Object.entries(vec)) assert.equal(got[p], want, `allow-list vector ${JSON.stringify(p)}`);
});

test('every shipped path is allowed; no device state, heavy model or runtime cache', () => {
  const { entries } = parse();
  const verdict = JSON.parse(py(`import json\n`
    + `ps=[l.split(' ',4)[4] for l in open(${JSON.stringify(man)},encoding='ascii').read().splitlines() if not l.startswith('#')]\n`
    + `print(json.dumps({p: [S.release_path_allowed(p), S.is_state(p)] for p in ps}))`));
  for (const e of entries) {
    const [why, state] = verdict[e.path];
    assert.equal(why, '', `outside the allow-list (${why}): ${e.path}`);
    assert.equal(state, false, `device state shipped: ${e.path}`);
    assert.ok(!/\/models\/|\/vendor\/(onnxruntime-web|ffmpeg|wllama)\/|\/forge\/vendor\//.test('/' + e.path), `heavy: ${e.path}`);
    assert.ok(!/\.(pcm|gguf|npy|onnx)$/i.test(e.path), `heavy ext: ${e.path}`);
  }
});

test('AKB5: exactly the shards the shipped manifest routes to', () => {
  const { entries } = parse();
  const listed = JSON.parse(py(`import json\nprint(json.dumps(S._akb5_shards(${JSON.stringify(join(out, 'data/anima/anima-it-akb5.bin'))})))`));
  const shipped = entries.filter((e) => e.path.startsWith('data/anima/akb5/')).map((e) => e.path.split('/').pop());
  assert.deepEqual([...shipped].sort(), [...listed].sort());
  assert.ok(listed.length >= 40, `akb5 shard count ${listed.length}`);
});

test('.gz twins are byte-exact gzip of their sibling', () => {
  const { entries } = parse();
  const set = new Set(entries.map((e) => e.path));
  let n = 0;
  for (const e of entries) {
    if (!e.path.endsWith('.gz') || !set.has(e.path.slice(0, -3))) continue;
    const raw = readFileSync(join(out, e.path.slice(0, -3)));
    assert.ok(gunzipSync(readFileSync(join(out, e.path))).equals(raw), `stale twin ${e.path}`);
    n++;
  }
  assert.ok(n > 100, `expected many twins, saw ${n}`);
});

test('modes: apps.json is merge-only, nothing else is', () => {
  const { entries } = parse();
  const merge = entries.filter((e) => e.mode === 'm').map((e) => e.path);
  assert.deepEqual(merge, ['system/registry/apps.json']);
});

test('required files present, packs and size budget', () => {
  const { packs, entries } = parse();
  const set = new Set(entries.map((e) => e.path));
  for (const r of JSON.parse(py('import json\nprint(json.dumps(S.RELEASE_REQUIRED))'))) assert.ok(set.has(r), `required ${r}`);
  assert.ok(packs.core, 'core pack');
  assert.ok(packs.core.bytes <= CORE_BUDGET, `core pack ${(packs.core.bytes / 1048576).toFixed(1)} MB over budget`);
  const total = Object.values(packs).reduce((a, p) => a + p.bytes, 0);
  assert.ok(total <= TOTAL_BUDGET, `total ${(total / 1048576).toFixed(1)} MB over budget`);
  for (const e of entries) {
    if (e.path.startsWith('www/shell/downloads/')) assert.equal(e.pack, 'downloads', e.path);
    else if (e.path.startsWith('apps/arcade/www/emulatorjs/')) assert.equal(e.pack, 'arcade', e.path);
    else assert.equal(e.pack, 'core', e.path);
  }
});

test('gzip is deterministic (unchanged file -> byte-identical twin)', () => {
  const r = py(`import tempfile,os,hashlib\nd=tempfile.mkdtemp()\nsrc=os.path.join(d,'a.js')\nopen(src,'w').write('x'*5000)\n`
    + `h=[]\nfor i in range(2):\n  dst=os.path.join(d,f'o{i}.gz'); S.gz_file(src,dst); h.append(hashlib.sha256(open(dst,'rb').read()).hexdigest())\n`
    + `print(h[0]==h[1])`);
  assert.equal(r.trim(), 'True');
});

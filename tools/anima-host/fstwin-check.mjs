// Host gate for the firmware's stale-.gz-twin rule (firmware/components/nucleo_fsapi/fstwin.c): compile the
// REAL C with the ui-host shims (NUCLEO_SD_MOUNT = "sd") and run it against real files in a fresh temp dir.
// Wired as `npm run fstwin:test`, the ANIMA gate and CI.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, rmSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { twinScope } from '../lib/twin-scope.mjs';

// The regex deploy.ps1 and sd-sync.ps1 use (PowerShell -match is case-insensitive). Keep in step with them.
export const PS_TWIN_RE = '^(www/shell|apps/[^/]+/www)/.';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
const exe = join(BUILD, process.platform === 'win32' ? 'fstwinctest.exe' : 'fstwinctest');
const cc = spawnSync(GCC, ['-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation',
  '-I', 'firmware/components/nucleo_fsapi/include', '-I', 'tools/ui-host/shim',
  'tools/anima-host/fstwin-ctest.c', 'firmware/components/nucleo_fsapi/fstwin.c', '-o', exe],
  { cwd: ROOT, env, encoding: 'utf8' });
if (cc.status !== 0) { console.log('fstwin: COMPILE FAILED'); process.stdout.write((cc.stdout || '') + (cc.stderr || '')); process.exit(1); }
// 1) the shared scope table: firmware C, the JS helper the network tools use, the Python twin (sd_deploy), and
//    the PowerShell regex deploy.ps1 / sd-sync.ps1 use — all must agree on every row.
const vec = JSON.parse(readFileSync('tools/lib/twin-scope-vectors.json', 'utf8')).scope;
const paths = Object.keys(vec);
let fail = 0;
const agree = (who, got) => paths.forEach((p, i) => { if (got[i] !== vec[p]) { fail++; console.log(`FAIL ${who} scope(${JSON.stringify(p)}) = ${got[i]}, want ${vec[p]}`); } });
const c = spawnSync(exe, ['scope', ...paths], { env, encoding: 'utf8' });
agree('firmware', (c.stdout || '').trim().split(/\r?\n/).map((l) => l === '1'));
agree('js', paths.map((p) => twinScope(p)));
const PY = ['python', 'python3'].find((b) => spawnSync(b, ['--version']).status === 0);
if (PY) {
  const r = spawnSync(PY, ['-c', "import sys,json; sys.path.insert(0,'tools/nucleo-sd-deploy'); import sd_deploy as S; " +
    "print(json.dumps([S.twin_scope(p) for p in json.load(open('tools/lib/twin-scope-vectors.json',encoding='utf-8'))['scope']]))"], { encoding: 'utf8' });
  if (r.status !== 0) { fail++; console.log(`FAIL python: ${r.stderr}`); } else agree('python', JSON.parse(r.stdout));
}
if (process.platform === 'win32') {
  // paths on stdin, one per line (PS 5.1 ConvertFrom-Json folds key case, so the table can't be parsed there)
  const ps = `$input | ForEach-Object { if ($_ -match '${PS_TWIN_RE}') { '1' } else { '0' } }`;
  const r = spawnSync('powershell', ['-NoProfile', '-Command', ps], { encoding: 'utf8', input: paths.join('\n') + '\n' });
  agree('powershell', (r.stdout || '').trim().split(/\r?\n/).map((l) => l.trim() === '1'));
}
for (const ps1 of ['tools/deploy.ps1', 'tools/sd-sync.ps1'])     // the PS tools must use exactly this regex
  if (!readFileSync(ps1, 'utf8').includes(`'${PS_TWIN_RE}'`)) { fail++; console.log(`FAIL ${ps1} does not use the shared twin regex '${PS_TWIN_RE}'`); }
console.log(`twin-scope vectors: ${paths.length} paths x ${2 + (PY ? 1 : 0) + (process.platform === 'win32' ? 1 : 0)} implementations`);

// 2) the drop behaviour against real files
const dir = mkdtempSync(join(tmpdir(), 'fstwin-'));
try {
  const r = spawnSync(exe, [dir], { env, encoding: 'utf8' });
  process.stdout.write(r.stdout || ''); if (r.stderr) process.stderr.write(r.stderr);
  process.exit(fail ? 1 : (r.status === null ? 1 : r.status));
} finally { rmSync(dir, { recursive: true, force: true }); }

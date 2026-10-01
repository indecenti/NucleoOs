// Host gate for the registry loader's per-app scanner: compile the REAL
// firmware/components/nucleo_registry/registry_scan.c with regscan-ctest.c and run it against the real
// registry/apps.json. Mirrors authslot-check.mjs. Wired as `npm run regscan:test`.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, process.platform === 'win32' ? 'regscanctest.exe' : 'regscanctest');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
  '-I', 'firmware/components/nucleo_registry/include',
  'tools/anima-host/regscan-ctest.c',
  'firmware/components/nucleo_registry/registry_scan.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('regscan: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);

const reg = join(ROOT, 'registry', 'apps.json');
const n = JSON.parse(readFileSync(reg, 'utf8')).installed.length;
const run = spawnSync(exe, [reg, String(n)], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

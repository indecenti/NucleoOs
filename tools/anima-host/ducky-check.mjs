// Host gate for nucleo_ducky: compile the pure DuckyScript engine (firmware/components/nucleo_ducky/
// nucleo_ducky.c) with ducky-ctest.c and run the parser / layout / dry-run assertions. Mirrors
// eth-check.mjs / ble-check.mjs — prove the payload engine on the PC before it drives a real host.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, 'duckyctest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra',
  '-I', 'firmware/components/nucleo_ducky/include',
  'tools/anima-host/ducky-ctest.c',
  'firmware/components/nucleo_ducky/nucleo_ducky.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('nucleo_ducky: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);

const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

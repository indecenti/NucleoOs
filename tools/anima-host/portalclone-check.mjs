// Host gate for the Evil Portal page-cloner asset rewriter: compile the pure HTML rewriter
// (firmware/components/nucleo_evilportal/nucleo_evilportal_clone.c) with portalclone-ctest.c and run
// the same-origin / URL-resolution / shrink-rewrite assertions on the PC before any live clone runs.
// Mirrors ble-check.mjs. Wired as `npm run portalclone:test` + the gate.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, 'portalclonectest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra',
  '-I', 'firmware/components/nucleo_evilportal/include',
  'tools/anima-host/portalclone-ctest.c',
  'firmware/components/nucleo_evilportal/nucleo_evilportal_clone.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('nucleo_evilportal_clone: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);

const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

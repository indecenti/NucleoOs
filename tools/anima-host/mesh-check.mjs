// Host gate for nucleo_mesh: compile the pure gossip core (nucleo_mesh.c) with mesh-ctest.c and
// run the gating / loop-prevention / de-dup / lossy-channel assertions. Mirrors link-check.mjs —
// prove the firmware's pure logic on the PC before it touches a Cardputer. Wired as `npm run mesh:test`.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, 'meshctest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra',
  '-I', 'firmware/components/nucleo_mesh/include',
  'tools/anima-host/mesh-ctest.c',
  'firmware/components/nucleo_mesh/nucleo_mesh.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('nucleo_mesh: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);   // surface warnings even on success

const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

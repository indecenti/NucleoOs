// Host gate for the Wi-Fi supervisor policy: compile the REAL decision core
// (firmware/components/nucleo_sdcontent/content_policy.c) with sdcontent-ctest.c and prove the four
// anti-flap invariants (never disturb a hotspot in use, APSTA joins, AP always restored,
// failed manual join never arms the retry loop) on the PC. Mirrors eth-check.mjs.
// Wired as `npm run sdcontent:test` + the ANIMA gate (sdcontent).
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, 'sdcontentctest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
  '-I', 'firmware/components/nucleo_sdcontent/include',
  'tools/anima-host/sdcontent-ctest.c',
  'firmware/components/nucleo_sdcontent/content_policy.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('sdcontent: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);   // surface warnings even on success

const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

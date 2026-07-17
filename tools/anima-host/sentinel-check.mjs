// Host gate for nucleo_sentinel: compile the pure defensive detection core
// (firmware/components/nucleo_sentinel/core/*.c) with sentinel-ctest.c and run
// the BLE-tracker / persistence / 802.11 / anomaly assertions on the PC. Mirrors
// ducky-check.mjs / fido-check.mjs — prove the detectors before any device build.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const SENT = 'firmware/components/nucleo_sentinel';
const exe = join(BUILD, 'sentinelctest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra',
  '-I', `${SENT}/include`,
  'tools/anima-host/sentinel-ctest.c',
  `${SENT}/core/sentinel_ble.c`,
  `${SENT}/core/sentinel_track.c`,
  `${SENT}/core/sentinel_wifi.c`,
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('nucleo_sentinel: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);

const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

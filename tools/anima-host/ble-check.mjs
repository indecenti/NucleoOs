// Host gate for nucleo_ble: compile the pure BLE advertisement core (firmware/components/nucleo_ble/
// nucleo_ble_adv.c) with ble-ctest.c and run the Apple/Swift Pair/Fast Pair/iBeacon framing + <=31-byte
// assertions. Mirrors eth-check.mjs — prove the spam/beacon payload byte-layouts on the PC before they
// ever hit the air (or a Cardputer). Wired as `npm run ble:test` + the gate.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, 'blectest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra',
  '-I', 'firmware/components/nucleo_ble/include',
  'tools/anima-host/ble-ctest.c',
  'firmware/components/nucleo_ble/nucleo_ble_adv.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('nucleo_ble: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);   // surface warnings even on success

const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

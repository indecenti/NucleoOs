// Host gate for the pure IR protocol encoder: compile nucleo_ir_proto.c + ir-ctest.c with gcc
// and run the timing-vector assertions. Mirrors the ANIMA host-harness philosophy — prove the
// firmware's pure logic on the PC before it ever touches the Cardputer. Wired as `npm run ir:test`.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

// Prefer the MSYS2 MinGW gcc the ANIMA harness uses; fall back to whatever `gcc` is on PATH.
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
// Putting the MinGW bin on PATH lets the produced exe find libwinpthread etc. at run time.
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, 'irctest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O0', '-Wall',
  '-I', 'firmware/components/nucleo_ir/include',
  'tools/anima-host/ir-ctest.c',
  'firmware/components/nucleo_ir/nucleo_ir_proto.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('IR encoder: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}

const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

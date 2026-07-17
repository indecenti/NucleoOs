// Host gate for nucleo_fido: compile the pure FIDO2 core (firmware/components/
// nucleo_fido/core/*.c + vendored tinycbor) with fido-ctest.c and run the CTAP2 /
// U2F / key-wrap / CTAPHID assertions on the PC. Mirrors ducky-check.mjs /
// eth-check.mjs — prove the security-key protocol layer before any device build.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const FIDO = 'firmware/components/nucleo_fido';
const exe = join(BUILD, 'fidoctest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra',
  '-I', `${FIDO}/include`,
  '-I', `${FIDO}/core`,
  '-I', `${FIDO}/tinycbor`,
  'tools/anima-host/fido-ctest.c',
  `${FIDO}/core/fido_cbor.c`,
  `${FIDO}/core/fido_cose.c`,
  `${FIDO}/core/fido_authdata.c`,
  `${FIDO}/core/fido_keywrap.c`,
  `${FIDO}/core/fido_cred_store_ram.c`,
  `${FIDO}/core/fido_u2f.c`,
  `${FIDO}/core/fido_ctap2.c`,
  `${FIDO}/core/fido_ctaphid.c`,
  `${FIDO}/core/fido_pin.c`,
  `${FIDO}/tinycbor/cborencoder.c`,
  `${FIDO}/tinycbor/cborparser.c`,
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('nucleo_fido: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);

const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

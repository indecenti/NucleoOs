// Host gate for the SD-content installer ENGINE, end to end: compile the REAL sdc_engine.c + content_policy.c
// + sdc_sha256.c (exactly what the device runs) with sdcontent-e2e-ctest.c, which drives it against a fake
// "GitHub Pages" that models the Cardputer's 8 KB TLS record cap and injects faults (dropped connections,
// corrupted windows, 404s, hostile manifests, unwritable card), with a temp dir as the SD. Also checks the
// SHA-256 against FIPS vectors and against hashlib on the real release in dist/ (when present).
// Wired as `npm run sdcontent:e2e` + the ANIMA gate.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
const C = 'firmware/components/nucleo_sdcontent';

const exe = join(BUILD, 'sdcontente2e.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-result',
  '-I', `${C}/include`,
  'tools/anima-host/sdcontent-e2e-ctest.c', `${C}/sdc_engine.c`, `${C}/content_policy.c`, `${C}/sdc_sha256.c`,
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });
if (cc.status !== 0) {
  console.error('sdcontent-e2e: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);
const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

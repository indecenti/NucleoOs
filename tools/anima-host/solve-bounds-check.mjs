#!/usr/bin/env node
// SOLVE-BOUNDS gate — memory-safety unit test for anima_solve.c helpers (a_subst_regs output bound,
// a_from_base overflow, a_solve_base truncated tokens). The test #includes the real anima_solve.c, so it
// links against the same host sources as anima.exe minus that file and host_main.c (mirrors
// stitch-dedup.mjs).
//
//   node tools/anima-host/solve-bounds-check.mjs
import { spawnSync } from 'node:child_process';
import { existsSync, readdirSync, mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here  = dirname(fileURLToPath(import.meta.url));
const repo  = join(here, '..', '..');
const anima = join(repo, 'firmware', 'components', 'nucleo_anima');
const build = join(here, 'build');

const skip = new Set(['nucleo_anima_online.c', 'nucleo_anima_bench.c', 'anima_solve.c']);
const srcs = readdirSync(anima).filter((f) => f.endsWith('.c') && !skip.has(f)).map((f) => join(anima, f));
srcs.push(join(here, 'esp_timer_host.c'), join(here, 'anima_online_stub.c'), join(here, 'solve-bounds-ctest.c'));

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

mkdirSync(build, { recursive: true });
const exe = join(build, 'solve-bounds-ctest');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O0', '-g', '-DANIMA_HOST',
  '-I', join(here, 'shim'), '-I', join(anima, 'include'), '-I', anima,
  '-include', join(here, 'shim', 'host_compat.h'),
  ...srcs, '-o', exe, '-lm',
], { encoding: 'utf8', env });
if (cc.status !== 0) {
  console.error('[solve-bounds] unit test failed to COMPILE:\n' + (cc.stderr || cc.stdout || (cc.error && cc.error.message) || ''));
  process.exit(2);
}
const run = spawnSync(exe, [], { encoding: 'utf8', env });
process.stdout.write((run.stdout || '') + (run.stderr || ''));
process.exit(run.status === 0 ? 0 : 1);

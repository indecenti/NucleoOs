// Host gate for the three-tier config store: compile the REAL firmware/components/nucleo_setup/
// setup_store.c (setup.json + networks.json persistence) with setup-store-ctest.c and an in-memory NVS
// (nvs_host.c), and prove fan-out, read order, and the factory-reset contract behind Settings ▸ Reset
// (erase every tier, stay honest about failures, seal against a racing save). Mirrors wifi-check.mjs.
// Wired as `npm run setupstore:test` + the test registry (gate-setup-store-reset).
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });
rmSync(join(BUILD, 'setupstore'), { recursive: true, force: true });   // the test's fake /cfg + /sd trees

const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, 'setupstorectest.exe');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation',   // as the component's CMakeLists
  '-include', 'tools/anima-host/setup-store-shim/host_compat.h',
  '-I', 'tools/anima-host/setup-store-shim',     // nvs.h (in-memory NVS)
  '-I', 'tools/anima-host/shim',                 // esp_err.h / esp_log.h
  '-I', 'firmware/components/nucleo_setup/include',
  'tools/anima-host/setup-store-ctest.c',
  'tools/anima-host/nvs_host.c',
  'firmware/components/nucleo_setup/setup_store.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('setup-store: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);   // surface warnings even on success

const run = spawnSync(exe, [], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr && run.status !== 0) process.stderr.write(run.stderr);   // ESP_LOG lines only on failure
process.exit(run.status === null ? 1 : run.status);

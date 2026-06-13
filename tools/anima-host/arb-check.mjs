#!/usr/bin/env node
// arb-check — gate for the heavy-work arbiter (firmware/components/nucleo_arb). Builds the host
// concurrency test if any source changed, runs it, and passes iff it proves: mutual exclusion
// under real thread contention, FG-preempts-BG yielding, the never-block (timeout=0) guarantee,
// idempotent release, and the teardown heap-floor sentinel. Wired into `npm run anima:gate`.
import { spawnSync } from 'node:child_process';
import { existsSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const hostDir = join(repo, 'tools', 'arb-host');
const comp = join(repo, 'firmware', 'components', 'nucleo_arb');
const exe = join(hostDir, 'build', 'arb_test.exe');

// Rebuild if the exe is missing or any source is newer (don't test a stale binary -> false green).
const srcs = [
  join(comp, 'nucleo_arb.c'),
  join(comp, 'include', 'nucleo_arb.h'),
  join(comp, 'arb_plat.h'),
  join(hostDir, 'arb_plat_host.c'),
  join(hostDir, 'arb_test.c'),
  join(hostDir, 'arb-build.ps1'),
];
const stale = !existsSync(exe) ||
  srcs.some((s) => existsSync(s) && statSync(s).mtimeMs > statSync(exe).mtimeMs);

if (stale) {
  const b = spawnSync('powershell', ['-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', join(hostDir, 'arb-build.ps1')], { cwd: repo, encoding: 'utf8' });
  process.stdout.write((b.stdout || '') + (b.stderr || ''));
  if (b.status !== 0 || !existsSync(exe)) {
    console.error('arb-check: build FAILED');
    process.exit(1);
  }
}

const r = spawnSync(exe, [], { cwd: repo, encoding: 'utf8' });
process.stdout.write(r.stdout || '');
if (r.stderr) process.stderr.write(r.stderr);
process.exit(r.status === 0 ? 0 : 1);

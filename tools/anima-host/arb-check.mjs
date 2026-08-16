#!/usr/bin/env node
// arb-check — gate for the heavy-work arbiter (firmware/components/nucleo_arb). Builds the host
// concurrency test if any source changed, runs it, and passes iff it proves: mutual exclusion
// under real thread contention, FG-preempts-BG yielding, the never-block (timeout=0) guarantee,
// idempotent release, and the teardown heap-floor sentinel. Wired into `npm run anima:gate`.
import { spawnSync } from 'node:child_process';
import { existsSync, statSync, openSync, readSync, closeSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

// A committed .exe from the OTHER OS (this repo tracks the Windows build) must be rebuilt, not run:
// on a fresh clone every file shares one mtime, so the mtime staleness check alone would run a Windows
// PE on Linux (or vice-versa). Rebuild when the on-disk exe isn't this platform's format.
function wrongPlatformExe(p) {
  try {
    const fd = openSync(p, 'r'); const b = Buffer.alloc(4); readSync(fd, b, 0, 4, 0); closeSync(fd);
    const isELF = b[0] === 0x7f && b[1] === 0x45 && b[2] === 0x4c && b[3] === 0x46;   // \x7fELF
    const isPE  = b[0] === 0x4d && b[1] === 0x5a;                                     // MZ
    return process.platform === 'win32' ? !isPE : !isELF;
  } catch { return true; }
}

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
  join(hostDir, 'arb_host_compat.h'),
  join(hostDir, 'arb-build.ps1'),
  join(hostDir, 'build.sh'),
];
const stale = !existsSync(exe) || wrongPlatformExe(exe) ||
  srcs.some((s) => existsSync(s) && statSync(s).mtimeMs > statSync(exe).mtimeMs);

if (stale) {
  // Windows builds via arb-build.ps1 (MinGW), every other platform via build.sh (system gcc).
  const win = process.platform === 'win32';
  const cmd = win ? 'powershell' : 'bash';
  const args = win
    ? ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', join(hostDir, 'arb-build.ps1')]
    : [join(hostDir, 'build.sh')];
  const b = spawnSync(cmd, args, { cwd: repo, encoding: 'utf8' });
  process.stdout.write((b.stdout || '') + (b.stderr || ''));
  if (b.error) { console.error(`arb-check: cannot run ${cmd}: ${b.error.message}`); process.exit(1); }
  if (b.status !== 0 || !existsSync(exe)) {
    console.error('arb-check: build FAILED');
    process.exit(1);
  }
}

const r = spawnSync(exe, [], { cwd: repo, encoding: 'utf8' });
process.stdout.write(r.stdout || '');
if (r.stderr) process.stderr.write(r.stderr);
process.exit(r.status === 0 ? 0 : 1);

#!/usr/bin/env node
// Default ANIMA dev loop: build the host harness if anything changed, then run it.
//   npm run anima -- "che ore sono"      one-shot
//   npm run anima                        interactive REPL
//   npm run anima:build                  force a rebuild only
// The harness compiles the REAL firmware cascade on the PC (no flash/Wi-Fi). See
// docs/debugging.md and tools/anima-host/README.md.
import { spawnSync } from 'node:child_process';
import { existsSync, statSync, readdirSync, readFileSync, openSync, readSync, closeSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

// A committed .exe from the OTHER OS (this repo tracks the Windows build) must be rebuilt, not run: on a
// fresh clone every file shares one mtime, so the mtime check alone would run a Windows PE on Linux.
function wrongPlatformExe(p) {
  try {
    const fd = openSync(p, 'r'); const b = Buffer.alloc(4); readSync(fd, b, 0, 4, 0); closeSync(fd);
    const isELF = b[0] === 0x7f && b[1] === 0x45 && b[2] === 0x4c && b[3] === 0x46;   // \x7fELF
    const isPE  = b[0] === 0x4d && b[1] === 0x5a;                                     // MZ
    return process.platform === 'win32' ? !isPE : !isELF;
  } catch { return true; }
}

const here  = dirname(fileURLToPath(import.meta.url));
const repo  = join(here, '..', '..');
const anima = join(repo, 'firmware', 'components', 'nucleo_anima');
const exe   = join(here, 'build', 'anima.exe');

// Anything the build depends on; if newer than the exe, rebuild. Watch the WHOLE nucleo_anima
// component (every .c/.h) + its include/ + the host shims, so a new firmware file (e.g.
// nucleo_anima_facet.c) can never be silently left out of the staleness check -> the gate would
// otherwise test a STALE exe after a firmware edit (a false green). Glob, don't hand-list.
const inc = join(anima, 'include');
const sources = [
  ...readdirSync(anima).filter(f => f.endsWith('.c') || f.endsWith('.h')).map(f => join(anima, f)),
  ...(existsSync(inc) ? readdirSync(inc).map(f => join(inc, f)) : []),
  join(here, 'esp_timer_host.c'), join(here, 'anima_online_stub.c'), join(here, 'host_main.c'),
  join(here, 'build.ps1'), join(here, 'build.sh'),
  ...readdirSync(join(here, 'shim')).map(f => join(here, 'shim', f)),
];
const mtime = p => { try { return statSync(p).mtimeMs; } catch { return 0; } };
const stale = () => !existsSync(exe) || wrongPlatformExe(exe) || sources.some(s => mtime(s) > mtime(exe));

const argv = process.argv.slice(2);
const forceBuild = argv[0] === '--build';   // always recompile
const ensure = argv[0] === '--ensure';      // recompile ONLY if stale, then exit (used by the gate)
if (forceBuild || ensure) argv.shift();

// --file <path>: feed a query list to the harness as raw UTF-8 bytes (node controls stdin,
// so accented Italian is not mangled by the shell's code page). Used by `npm run anima:sweep`.
let inputBuf = null;
const fi = argv.indexOf('--file');
if (fi !== -1) { inputBuf = readFileSync(argv[fi + 1]); argv.splice(fi, 2); }

if (forceBuild || stale()) {
  // Windows builds via build.ps1 (MinGW), every other platform via build.sh (system gcc).
  const win = process.platform === 'win32';
  const cmd = win ? 'powershell' : 'bash';
  const args = win
    ? ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', join(here, 'build.ps1')]
    : [join(here, 'build.sh')];
  const b = spawnSync(cmd, args, { stdio: 'inherit' });
  if (b.error) {                      // e.g. the shell itself is missing — say so, don't die silently
    console.error(`anima host build: cannot run ${cmd}: ${b.error.message}`);
    process.exit(1);
  }
  if (b.status !== 0) process.exit(b.status ?? 1);
}
if ((forceBuild || ensure) && argv.length === 0) process.exit(0);

// The exe is built -static, so it runs with no MinGW DLLs on PATH.
const opts = inputBuf
  ? { input: inputBuf, stdio: ['pipe', 'inherit', 'inherit'] }
  : { stdio: 'inherit' };
const r = spawnSync(exe, argv, opts);
process.exit(r.status ?? 0);

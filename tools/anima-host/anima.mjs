#!/usr/bin/env node
// Default ANIMA dev loop: build the host harness if anything changed, then run it.
//   npm run anima -- "che ore sono"      one-shot
//   npm run anima                        interactive REPL
//   npm run anima:build                  force a rebuild only
// The harness compiles the REAL firmware cascade on the PC (no flash/Wi-Fi). See
// docs/debugging.md and tools/anima-host/README.md.
import { spawnSync } from 'node:child_process';
import { existsSync, statSync, readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

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
  join(here, 'build.ps1'),
  ...readdirSync(join(here, 'shim')).map(f => join(here, 'shim', f)),
];
const mtime = p => { try { return statSync(p).mtimeMs; } catch { return 0; } };
const stale = () => !existsSync(exe) || sources.some(s => mtime(s) > mtime(exe));

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
  const win = process.platform === 'win32';
  const cmd = win ? 'powershell' : 'pwsh';
  const args = (win ? ['-NoProfile', '-ExecutionPolicy', 'Bypass'] : ['-NoProfile'])
    .concat(['-File', join(here, 'build.ps1')]);
  const b = spawnSync(cmd, args, { stdio: 'inherit' });
  if (b.status !== 0) process.exit(b.status ?? 1);
}
if ((forceBuild || ensure) && argv.length === 0) process.exit(0);

// The exe is built -static, so it runs with no MinGW DLLs on PATH.
const opts = inputBuf
  ? { input: inputBuf, stdio: ['pipe', 'inherit', 'inherit'] }
  : { stdio: 'inherit' };
const r = spawnSync(exe, argv, opts);
process.exit(r.status ?? 0);

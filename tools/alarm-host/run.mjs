#!/usr/bin/env node
// Build + run the host harness for the native Allarme app.
//   node tools/alarm-host/run.mjs        (npm run alarm:test)
//
// It compiles a byte-identical COPY of firmware/components/nucleo_app/app_alarm.cpp against the
// stubs in ./stubs (the copy exists only so those stubs win the "" include search over the real
// firmware headers next to the source) and drives it on a simulated clock. No device involved.
import { execFileSync } from 'node:child_process';
import { mkdirSync, rmSync, copyFileSync, readFileSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const app = join(here, '..', '..', 'firmware', 'components', 'nucleo_app', 'app_alarm.cpp');
const build = join(here, 'build');
const sandbox = join(here, 'sdroot');

const GPP = [
  'C:/msys64/mingw64/bin/g++.exe',
  'C:/msys64/ucrt64/bin/g++.exe',
  'g++',
].find((p) => p === 'g++' || existsSync(p));

rmSync(build, { recursive: true, force: true });
rmSync(sandbox, { recursive: true, force: true });
mkdirSync(build, { recursive: true });

const copy = join(build, 'app_under_test.cpp');
copyFileSync(app, copy);
if (Buffer.compare(readFileSync(app), readFileSync(copy)) !== 0) {
  console.error('the copy under test differs from the firmware source');
  process.exit(1);
}

const common = ['-std=gnu++17', '-O0', '-I', 'stubs', '-I', '.'];
const run = (args) =>
  execFileSync(GPP, args, {
    cwd: here,
    stdio: 'inherit',
    env: { ...process.env, PATH: `C:/msys64/mingw64/bin;${process.env.PATH}` },
  });

run([...common, '-include', 'stubs/hostsd.h', '-c', 'build/app_under_test.cpp', '-o', 'build/app_alarm.o']);
run([...common, '-c', 'host_stubs.cpp', '-o', 'build/host_stubs.o']);
run([...common, '-c', 'test_alarm.cpp', '-o', 'build/test_alarm.o']);
run(['build/app_alarm.o', 'build/host_stubs.o', 'build/test_alarm.o', '-o', 'build/alarm_test.exe']);

try {
  execFileSync(join(build, 'alarm_test.exe'), ['sdroot'], { cwd: here, stdio: 'inherit' });
} catch {
  process.exit(1);
}

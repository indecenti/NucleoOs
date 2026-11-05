// rt-check.mjs: Nucleo Code runtime gate. Regenerates the host tables, compiles code_rt.c + MicroQuickJS
// (the sources the device module uses) with MinGW and runs tools/code/rt-test.c. Usage: npm run code:test
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const comp = join(root, 'firmware', 'components', 'nucleo_code');
const vend = join(comp, 'vendor', 'mquickjs');
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const g = spawnSync(process.execPath, [join(root, 'tools', 'code', 'gen-tables.mjs')], { encoding: 'utf8' });
if (g.status !== 0) { console.error(g.stderr || g.stdout); process.exit(1); }
const out = join(root, 'build', 'code-rt');
mkdirSync(out, { recursive: true });
const exe = join(out, 'rt-test' + (process.platform === 'win32' ? '.exe' : ''));
const cc = spawnSync(GCC, ['-std=gnu11', '-Os', '-D_GNU_SOURCE', '-fno-math-errno', '-fno-trapping-math', '-Wall', '-Wno-unused-function',
  '-I', join(root, 'build', 'code-host'), '-I', join(comp, 'include'), '-I', vend,
  join(root, 'tools', 'code', 'rt-test.c'), join(comp, 'code_rt.c'),
  join(vend, 'mquickjs.c'), join(vend, 'dtoa.c'), join(vend, 'libm.c'), join(vend, 'cutils.c'), '-lm', '-o', exe], { encoding: 'utf8', env });
if (cc.status !== 0) { console.error('[code-rt] compile failed:\n' + (cc.stderr || cc.stdout)); process.exit(1); }
const r = spawnSync(exe, [], { encoding: 'utf8', env, cwd: root, timeout: 120000 });
process.stdout.write(r.stdout || ''); if (r.stderr) process.stderr.write(r.stderr);
console.log(`[code-rt] ${r.status === 0 ? 'OK' : 'FAIL'}`);
process.exit(r.status === 0 ? 0 : 1);

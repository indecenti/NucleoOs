// gen-tables.mjs: builds MicroQuickJS's table generator from Nucleo Code's stdlib definition
// (firmware/components/nucleo_code/code_stdlib.c + vendor/mquickjs/mquickjs_build.c) with MinGW gcc and
// writes the ROM tables:
//   firmware/components/nucleo_code/gen/{mquickjs_atom.h,code_stdlib.h}   32-bit (-m32): the ESP32-S3
//   build/code-host/{mquickjs_atom.h,code_stdlib.h}                       the host's word size: host tests
// The atom table and the stdlib table must come from the SAME definition. Usage: node tools/code/gen-tables.mjs [--check]
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const comp = join(root, 'firmware', 'components', 'nucleo_code');
const vend = join(comp, 'vendor', 'mquickjs');
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
const check = process.argv.includes('--check');

const out = join(root, 'build', 'code-gen');
mkdirSync(out, { recursive: true });
const exe = join(out, 'code_stdlib_gen' + (process.platform === 'win32' ? '.exe' : ''));
const cc = spawnSync(GCC, ['-O2', '-D_GNU_SOURCE', '-I', vend, join(comp, 'code_stdlib.c'), join(vend, 'mquickjs_build.c'), '-o', exe],
  { encoding: 'utf8', env });
if (cc.status !== 0) { console.error('[code-gen] generator build failed:\n' + (cc.stderr || cc.stdout)); process.exit(1); }

function gen(args) {
  const r = spawnSync(exe, args, { encoding: 'utf8', env, maxBuffer: 64 << 20 });
  if (r.status !== 0) { console.error('[code-gen] generator failed: ' + r.stderr); process.exit(1); }
  return r.stdout.replace(/\r\n/g, '\n');
}
const targets = [
  { dir: join(comp, 'gen'), flags: ['-m32'], committed: true },
  { dir: join(root, 'build', 'code-host'), flags: [], committed: false },
];
let stale = 0;
for (const t of targets) {
  mkdirSync(t.dir, { recursive: true });
  const files = { 'mquickjs_atom.h': gen(['-a', ...t.flags]), 'code_stdlib.h': gen(t.flags) };
  for (const [name, text] of Object.entries(files)) {
    const p = join(t.dir, name);
    if (check && t.committed) { if (!existsSync(p) || readFileSync(p, 'utf8').replace(/\r\n/g, '\n') !== text) { stale++; console.error(`[code-gen] stale: ${p}`); } }
    else writeFileSync(p, text);
  }
}
if (check) { console.log(stale ? `[code-gen] FAIL: ${stale} table(s) out of date, run node tools/code/gen-tables.mjs` : '[code-gen] OK: tables match code_stdlib.c'); process.exit(stale ? 1 : 0); }
console.log('[code-gen] tables written (32-bit to gen/, host to build/code-host/)');

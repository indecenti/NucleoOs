// rt-check.mjs: Nucleo Code runtime gate. Regenerates the host tables, compiles code_rt.c + MicroQuickJS
// (the sources the device module uses) with MinGW and runs tools/code/rt-test.c. Usage: npm run code:test
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { inflateSync, crc32 } from 'node:zlib';
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
  join(root, 'tools', 'code', 'rt-test.c'), join(comp, 'code_rt.c'), join(comp, 'code_gfx.c'),
  join(vend, 'mquickjs.c'), join(vend, 'dtoa.c'), join(vend, 'libm.c'), join(vend, 'cutils.c'), '-lm', '-o', exe], { encoding: 'utf8', env });
if (cc.status !== 0) { console.error('[code-rt] compile failed:\n' + (cc.stderr || cc.stdout)); process.exit(1); }
const r = spawnSync(exe, [], { encoding: 'utf8', env, cwd: root, timeout: 120000 });
process.stdout.write(r.stdout || ''); if (r.stderr) process.stderr.write(r.stderr);
// The screenshots the runtime wrote: valid PNGs (every chunk CRC, the zlib stream and its Adler-32), with the pixels drawn.
let pngFail = 0;
function png(file) {
  const b = readFileSync(file); let o = 8, ihdr = null; const idat = [];
  if (b.subarray(0, 8).toString('hex') !== '89504e470d0a1a0a') throw new Error('signature');
  while (o < b.length) {
    const n = b.readUInt32BE(o), type = b.subarray(o + 4, o + 8).toString('latin1'), data = b.subarray(o + 8, o + 8 + n);
    if (b.readUInt32BE(o + 8 + n) !== crc32(b.subarray(o + 4, o + 8 + n))) throw new Error(type + ' CRC');
    if (type === 'IHDR') ihdr = { w: data.readUInt32BE(0), h: data.readUInt32BE(4), depth: data[8], type: data[9] };
    if (type === 'PLTE') ihdr.plte = data;
    if (type === 'IDAT') idat.push(data);
    o += 12 + n;
  }
  const raw = inflateSync(Buffer.concat(idat)), rb = Math.ceil(ihdr.w / 2) + 1;
  if (raw.length !== rb * ihdr.h) throw new Error(`raw ${raw.length} != ${rb * ihdr.h}`);
  const px = (x, y) => { const v = raw[y * rb + 1 + (x >> 1)]; return x & 1 ? v & 15 : v >> 4; };
  return { ...ihdr, px, size: b.length };
}
const check = (c, m) => { if (!c) { pngFail++; console.log('FAIL png: ' + m); } };
try {
  const p = png(join(root, 'build', 'code-rt', 'shot.png'));
  check(p.w === 480 && p.h === 270 && p.depth === 4 && p.type === 3, `shot.png header ${p.w}x${p.h}`);
  check(p.px(10, 10) === 4 && p.px(479, 269) === 13 && p.px(100, 200) === 11, 'shot.png pixels (red corner, pink pixel, dark blue)');
  check(p.px(240, 140) === 7, 'shot.png: the green disc');
  console.log(`png: shot.png ${p.w}x${p.h} ${p.size} B`);
  const q = png(join(root, 'build', 'code-rt', 'shot-large.png'));
  check(q.w === 640 && q.h === 480, `shot-large.png ${q.w}x${q.h}`);
  console.log(`png: shot-large.png ${q.w}x${q.h} ${q.size} B`);
} catch (e) { check(false, e.message); }
const ok = r.status === 0 && !pngFail;
console.log(`[code-rt] ${ok ? 'OK' : 'FAIL'}`);
process.exit(ok ? 0 : 1);

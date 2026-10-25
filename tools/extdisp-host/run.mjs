#!/usr/bin/env node
// External display host gate (npm run extdisp:test).
//
// Compiles the REAL firmware sources of the external-display backend — extdisp_probe.c (pin rules,
// XPT2046 fingerprint, attach decision) and extdisp_mirror.cpp (the panel that tees the internal
// display onto the ILI9341) — against the REAL LovyanGFX core the firmware links, with MinGW, and runs
// tools/extdisp-host/main.cpp. No device, no flash. Writes build/extdisp-host/{internal,external}.png.
//   node tools/extdisp-host/run.mjs
import { spawn, execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, existsSync, statSync, readdirSync } from 'node:fs';
import { join, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const UI = join(root, 'firmware', 'components', 'nucleo_ui');
const GFX = join(root, 'firmware', 'managed_components', 'm5stack__m5gfx', 'src');
const UIHOST = join(root, 'tools', 'ui-host');
const OUT = join(root, 'build', 'extdisp-host');
const OBJ = join(OUT, 'obj');
const LGFX_OBJ = join(OUT, 'lgfx');

const MINGW = 'C:/msys64/mingw64/bin';
const GPP = existsSync(`${MINGW}/g++.exe`) ? `${MINGW}/g++.exe` : 'g++';
const GCC = existsSync(`${MINGW}/gcc.exe`) ? `${MINGW}/gcc.exe` : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };
for (const d of [OBJ, LGFX_OBJ]) mkdirSync(d, { recursive: true });

function compile(cc, src, obj, flags) {
  return new Promise((resolve, reject) => {
    const p = spawn(cc, [...flags, '-c', src, '-o', obj], { env });
    let err = '';
    p.stderr.on('data', (b) => { err += b; });
    p.on('close', (code) => (code === 0 ? resolve(obj) : reject(new Error(`${basename(src)}\n${err}`))));
  });
}
const stale = (src, obj) => !existsSync(obj) || statSync(obj).mtimeMs < statSync(src).mtimeMs;
async function pool(jobs, n = 8) {
  const out = []; let i = 0;
  await Promise.all(Array.from({ length: n }, async () => { while (i < jobs.length) { const j = jobs[i++]; out.push(await j()); } }));
  return out;
}

// LovyanGFX core + the ui-host platform layer (same set ui:shots links), compiled once and cached.
const lgfxSrc = [
  join(GFX, 'lgfx', 'v1', 'LGFXBase.cpp'), join(GFX, 'lgfx', 'v1', 'LGFX_Sprite.cpp'),
  join(GFX, 'lgfx', 'v1', 'lgfx_fonts.cpp'), join(GFX, 'lgfx', 'v1', 'panel', 'Panel_Device.cpp'),
  ...readdirSync(join(GFX, 'lgfx', 'v1', 'misc')).filter((f) => f.endsWith('.cpp')).map((f) => join(GFX, 'lgfx', 'v1', 'misc', f)),
  ...readdirSync(join(GFX, 'lgfx', 'utility')).filter((f) => f.endsWith('.c')).map((f) => join(GFX, 'lgfx', 'utility', f)),
  join(UIHOST, 'host_platform.cpp'),
];
const fontSyms = new Set(readFileSync(join(GFX, 'lgfx', 'v1', 'lgfx_fonts.cpp'), 'utf8').match(/\blgfx_(?:efont|font_japan)_\w+/g));
const fontStub = join(LGFX_OBJ, 'font_stubs.c');
const fontStubSrc = ['#include <stdint.h>', ...[...fontSyms].map((s) => `const uint8_t ${s}[64] = {0};`), ''].join('\n');
if (!existsSync(fontStub) || readFileSync(fontStub, 'utf8') !== fontStubSrc) writeFileSync(fontStub, fontStubSrc);
lgfxSrc.push(fontStub);

const testSrc = [join(UI, 'extdisp_probe.c'), join(UI, 'extdisp_mirror.cpp'), join(here, 'main.cpp')];
const INC = ['-I', join(UIHOST, 'shim'), '-I', GFX, '-I', UI];

try {
  const lgfxObjs = await pool(lgfxSrc.map((src) => () => {
    const obj = join(LGFX_OBJ, basename(src) + '.o');
    if (!stale(src, obj)) return obj;
    const cpp = src.endsWith('.cpp');
    return compile(cpp ? GPP : GCC, src, obj, [cpp ? '-std=gnu++17' : '-std=gnu11', '-O1', '-w', '-I', join(UIHOST, 'shim'), '-I', GFX]);
  }));
  const objs = await pool(testSrc.map((src) => () => {
    const obj = join(OBJ, basename(src) + '.o');
    const cpp = src.endsWith('.cpp');
    return compile(cpp ? GPP : GCC, src, obj, [cpp ? '-std=gnu++17' : '-std=gnu11', '-O0', '-g', '-Wall', '-Wextra', '-Wno-unused-parameter', ...INC]);
  }));
  execFileSync(GPP, [...objs, ...lgfxObjs, '-o', join(OUT, 'extdisp_host.exe')], { env, stdio: 'inherit' });
} catch (e) {
  console.error('extdisp-host: BUILD FAILED\n' + e.message);
  process.exit(1);
}

let code = 0;
try {
  process.stdout.write(execFileSync(join(OUT, 'extdisp_host.exe'), [OUT], { env, encoding: 'utf8' }));
} catch (e) {
  process.stdout.write(e.stdout || '');
  code = e.status || 1;
}

function png(rgb, w, h, scale) {
  const W = w * scale, H = h * scale;
  const raw = Buffer.alloc((W * 3 + 1) * H);
  for (let y = 0; y < H; y++) {
    raw[y * (W * 3 + 1)] = 0;
    for (let x = 0; x < W; x++) rgb.copy(raw, y * (W * 3 + 1) + 1 + x * 3, ((Math.floor(y / scale) * w) + Math.floor(x / scale)) * 3, ((Math.floor(y / scale) * w) + Math.floor(x / scale)) * 3 + 3);
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
for (const [name, w, h, s] of [['internal', 240, 135, 3], ['external', 320, 240, 2]]) {
  const f = join(OUT, `${name}.rgb`);
  if (existsSync(f)) writeFileSync(join(OUT, `${name}.png`), png(readFileSync(f), w, h, s));
}
console.log(code ? 'extdisp-host: FAILED' : `extdisp-host: OK (build/extdisp-host/external.png)`);
process.exit(code);

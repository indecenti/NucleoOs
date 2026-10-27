#!/usr/bin/env node
// Host harness for the native Presence app (npm run presence:test).
//
// Compiles the REAL firmware sources presence_detect.c (the CSI motion detector) and presence_view.cpp
// (every pixel of the app) with the real LovyanGFX core that tools/ui-host builds, then:
//   1. drives the detector on synthetic CSI (calibration, still room, walking, the 10 s hold, events);
//   2. renders the app's screens — the Cardputer canvas (8 bpp, as on the device) and the external
//      320x240 panel composited in 101-row strips (the shared 32 KB canvas seen 320 wide) — to
//      build/presence-host/*.png, plus a contact sheet index.html. Look at them after any UI change.
import { execFileSync } from 'node:child_process';
import { mkdirSync, readdirSync, readFileSync, writeFileSync, rmSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const FW = join(root, 'firmware', 'components');
const GFX = join(root, 'firmware', 'managed_components', 'm5stack__m5gfx', 'src');
const CJSON = 'C:/esp/esp-idf/components/json/cJSON';
const LGFX_OBJ = join(root, 'build', 'ui-host', 'lgfx');
const OUT = join(root, 'build', 'presence-host');
const MINGW = 'C:/msys64/mingw64/bin';
const GPP = existsSync(`${MINGW}/g++.exe`) ? `${MINGW}/g++.exe` : 'g++';
const GCC = existsSync(`${MINGW}/gcc.exe`) ? `${MINGW}/gcc.exe` : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };

if (!existsSync(join(LGFX_OBJ, 'LGFXBase.cpp.o'))) {
  execFileSync(process.execPath, [join(root, 'tools', 'ui-host', 'run.mjs'), '--only', 'launcher.home.en.classic'], { cwd: root, stdio: 'inherit' });
}
mkdirSync(join(OUT, 'obj'), { recursive: true });
for (const f of readdirSync(OUT)) if (f !== 'obj') rmSync(join(OUT, f), { recursive: true, force: true });   // empty it; the dir itself may be held open

const INC = ['-I', join(root, 'tools', 'ui-host', 'shim'), '-I', GFX, '-I', join(FW, 'nucleo_app'), '-I', join(FW, 'nucleo_app', 'include'),
  '-I', join(FW, 'nucleo_ui', 'include'), '-I', join(FW, 'nucleo_storage', 'include'), '-I', join(FW, 'nucleo_kbd', 'include'), '-I', CJSON,
  '-include', join(root, 'tools', 'ui-host', 'shim', 'host_compat.h')];
const srcs = [
  join(FW, 'nucleo_app', 'presence_detect.c'), join(FW, 'nucleo_app', 'presence_alarm.c'), join(FW, 'nucleo_app', 'presence_view.cpp'),
  join(FW, 'nucleo_ui', 'nucleo_theme.cpp'), join(FW, 'nucleo_storage', 'nucleo_i18n.c'), join(CJSON, 'cJSON.c'),
  join(here, 'host_stubs.cpp'), join(here, 'test_presence.cpp'),
];
const objs = srcs.map((src) => {
  const obj = join(OUT, 'obj', src.split(/[\\/]/).pop() + '.o');
  const cpp = src.endsWith('.cpp');
  execFileSync(cpp ? GPP : GCC, [cpp ? '-std=gnu++17' : '-std=gnu11', '-O1', '-Wall', '-Wno-unused-function', '-DPV_LAYOUT_PROBE', ...INC, '-c', src, '-o', obj], { env, stdio: 'inherit' });
  return obj;
});
const lgfx = readdirSync(LGFX_OBJ).filter((f) => f.endsWith('.o')).map((f) => join(LGFX_OBJ, f));
const exe = join(OUT, 'presence_test.exe');
execFileSync(GPP, [...objs, ...lgfx, '-o', exe], { env, stdio: 'inherit' });

let failed = false;
try { execFileSync(exe, [OUT], { env, stdio: 'inherit', cwd: OUT }); } catch { failed = true; }

// ---- PNGs (x3, nearest) + contact sheet ---------------------------------------------------------------
function png(rgb, W, H, S = 3) {
  const w = W * S, h = H * S, raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) rgb.copy(raw, y * (w * 3 + 1) + 1 + x * 3, ((Math.floor(y / S) * W) + Math.floor(x / S)) * 3, ((Math.floor(y / S) * W) + Math.floor(x / S)) * 3 + 3);
  const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(zlib.crc32(td) >>> 0); return Buffer.concat([l, td, c]); };
  const ih = Buffer.alloc(13); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const shots = [];
for (const f of readdirSync(OUT).filter((f) => f.endsWith('.rgb')).sort()) {
  const [, name, W, H] = f.match(/^(.*)\.(\d+)x(\d+)\.rgb$/);
  writeFileSync(join(OUT, name + '.png'), png(readFileSync(join(OUT, f)), +W, +H));
  rmSync(join(OUT, f));
  shots.push(name);
}
writeFileSync(join(OUT, 'index.html'), `<!doctype html><meta charset="utf-8"><title>Presence shots</title>
<body style="background:#222;color:#ddd;font:13px sans-serif">${shots.map((n) => `<figure style="display:inline-block;margin:6px"><img src="${n}.png"><figcaption>${n}</figcaption></figure>`).join('')}`);
console.log(`presence-host: ${shots.length} screens -> ${OUT}`);
process.exit(failed ? 1 : 0);

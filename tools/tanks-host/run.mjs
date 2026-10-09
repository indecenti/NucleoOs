#!/usr/bin/env node
// Nucleo Tanks host gate (npm run tanks:test).
//
// Compiles the REAL firmware/components/nucleo_app/app_tanks.cpp with the REAL LovyanGFX core (the objects
// tools/ui-host builds and caches), runs harness.cpp's scenarios — aiming precision, every weapon at every
// kind of aim, whole best-of series vs the CPU, menus/edges — and writes the review frames as PNGs to
// build/tanks-host/shots/.
//   node tools/tanks-host/run.mjs [scenario] [seed]      scenario: aim | weapons | series | edges | physics | cpu | frames | mp | all
import { spawnSync, execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, existsSync, readdirSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import zlib from 'node:zlib';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const FW = join(root, 'firmware', 'components');
const GFX = join(root, 'firmware', 'managed_components', 'm5stack__m5gfx', 'src');
const OUT = join(root, 'build', 'tanks-host');
const LGFX_OBJ = join(root, 'build', 'ui-host', 'lgfx');
const MINGW = 'C:/msys64/mingw64/bin';
const GPP = existsSync(`${MINGW}/g++.exe`) ? `${MINGW}/g++.exe` : 'g++';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };
const [scenario = 'all', seed = '7'] = process.argv.slice(2);

// The LovyanGFX core is compiled and cached by the native UI harness: build it once if it is missing.
if (!existsSync(LGFX_OBJ) || !readdirSync(LGFX_OBJ).some((f) => f.endsWith('.o'))) {
  console.log('tanks-host: building the LovyanGFX core via tools/ui-host (one time)...');
  const r = spawnSync(process.execPath, [join(root, 'tools', 'ui-host', 'run.mjs'), '--only', 'launcher'], { stdio: 'inherit', env });
  if (r.status !== 0 && !existsSync(LGFX_OBJ)) process.exit(1);
}
const lgfxObjs = readdirSync(LGFX_OBJ).filter((f) => f.endsWith('.o')).map((f) => join(LGFX_OBJ, f));

mkdirSync(OUT, { recursive: true });
const EXE = join(OUT, 'tanks_host.exe');
const INC = [
  '-I', join(here, 'shim'), '-I', join(root, 'tools', 'ui-host', 'shim'), '-I', GFX,
  '-I', join(FW, 'nucleo_app', 'include'), '-I', join(FW, 'nucleo_app'), '-I', join(FW, 'nucleo_kbd', 'include'),
  '-I', join(FW, 'nucleo_ui', 'include'), '-I', join(FW, 'nucleo_audio', 'include'), '-I', join(FW, 'nucleo_pnet', 'include'),
];
const t0 = Date.now();
try {
  execFileSync(GPP, ['-std=gnu++17', '-O1', '-g', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
    '-include', join(here, 'shim', 'tanks_host_fs.h'), ...INC, join(here, 'harness.cpp'), ...lgfxObjs, '-o', EXE], { env, stdio: 'inherit' });
} catch {
  console.error('tanks-host: BUILD FAILED');
  process.exit(1);
}
const MP_EXE = join(OUT, 'tanks_mp.exe');
if (scenario === 'all' || scenario === 'mp') {
  try {
    execFileSync(GPP, ['-std=gnu++17', '-O1', '-g', '-w', '-include', join(here, 'shim', 'tanks_host_fs.h'), ...INC,
      join(here, 'mp.cpp'), ...lgfxObjs, '-o', MP_EXE], { env, stdio: 'inherit' });
  } catch {
    console.error('tanks-host: MP BUILD FAILED');
    process.exit(1);
  }
}
const built = ((Date.now() - t0) / 1000).toFixed(1);

// Each run gets a fresh sandbox: the game's /sd/data/tanks lands in it, never on the host's drive root.
const box = join(OUT, 'box');
rmSync(box, { recursive: true, force: true });
mkdirSync(box, { recursive: true });
let status = 0;
if (scenario !== 'mp') {
  const run = spawnSync(EXE, [scenario, seed], { cwd: box, env, encoding: 'utf8', timeout: 1800000 });
  process.stdout.write(run.stdout || '');
  if (run.stderr) process.stderr.write(run.stderr);
  if (run.error || run.status === null) { console.error(`tanks-host: harness crashed (${run.error?.message || run.signal})`); process.exit(1); }
  status = run.status;
}
if (scenario === 'all' || scenario === 'mp') {                 // two devices over a simulated ESP-NOW link
  const mp = spawnSync(MP_EXE, [seed], { cwd: box, env, encoding: 'utf8', timeout: 900000 });
  process.stdout.write(mp.stdout || '');
  if (mp.stderr) process.stderr.write(mp.stderr);
  if (mp.error || mp.status === null) { console.error(`tanks-host: MP harness crashed (${mp.error?.message || mp.signal})`); process.exit(1); }
  status = status || mp.status;
}

// ---- review frames -> PNG (x3, nearest) --------------------------------------------------------------
const W = 240, H = 135, S = 3;
function png(rgb) {
  const w = W * S, h = H * S, raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const s = (Math.floor(y / S) * W + Math.floor(x / S)) * 3;
    rgb.copy(raw, y * (w * 3 + 1) + 1 + x * 3, s, s + 3);
  }
  const crcT = new Int32Array(256).map((_, n) => { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; return c; });
  const crc = (b) => { let c = -1; for (const x of b) c = crcT[(c ^ x) & 255] ^ (c >>> 8); return (c ^ -1) >>> 0; };
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]); const c = Buffer.alloc(4); c.writeUInt32BE(crc(td));
    return Buffer.concat([len, td, c]);
  };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]);
}
const shotsIn = join(box, 'shots'), shotsOut = join(OUT, 'shots');
if (existsSync(shotsIn)) {
  rmSync(shotsOut, { recursive: true, force: true }); mkdirSync(shotsOut, { recursive: true });
  for (const f of readdirSync(shotsIn).filter((f) => f.endsWith('.rgb')))
    writeFileSync(join(shotsOut, f.replace(/\.rgb$/, '.png')), png(readFileSync(join(shotsIn, f))));
}
console.log(`tanks-host: build ${built}s${existsSync(shotsOut) ? `, frames in build/tanks-host/shots/` : ''}`);
process.exit(status);

#!/usr/bin/env node
// Native app gallery (npm run ui:apps): every native app rendered on the PC in its three looks, from its REAL
// source — compact (the Cardputer alone), companion (the Cardputer while the app owns the external panel) and
// large (on_draw_large on the 320x240 panel, plus a copy with its touch targets outlined).
//
// One executable per app (gallery.cpp + app_<key>.cpp + the shared UI sources + host_stubs.cpp). Whatever else
// the app links is generated: every undefined symbol becomes a stub returning 0, so an app renders without
// hand-written stubs. Data an app reads from the SD card comes from its scene file.
//
// Scene file tools/ui-host/apps/<key>.txt (key = the source name, app_<key>.cpp). One scene per line:
//   <name> [key tokens...]                     tokens: U D L R ENTER BACK TAB DEL SPACE c:<ch> t:<text> row:<n> tap:<x>,<y> touch:<x>,<y> view:<n> tick:<n> adv
//   @src extra.cpp comp/x.c ...                more firmware sources (nucleo_app/, or <component>/<file>) the app needs for real
//   @host x_stubs.cpp ...                      hand-written host stubs from tools/ui-host/ (data a stub of 0 cannot give)
//   @reg nucleo_register_x                     the register function (default nucleo_register_<key>)
//   @app <id>                                  which registered app (a file may register two)
//   @sd <path under sd/> <content>             a file the scenes find on the card (\n = newline)
// No scene file: one scene, "home".
//
// Output: build/ui-host/gallery/shots/<key>.<scene>.<variant>.png (x2), sheets/<key>.png (all scenes, 1:1).
// Golden: tools/ui-host/apps-golden.json. A changed shot fails the run; accept with --update.
//   node tools/ui-host/apps.mjs [--app a,b] [--update] [--lang en]
import { spawn, execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, existsSync, statSync, readdirSync, rmSync } from 'node:fs';
import { join, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import zlib from 'node:zlib';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const FW = join(root, 'firmware', 'components');
const APPS = join(FW, 'nucleo_app');
const GFX = join(root, 'firmware', 'managed_components', 'm5stack__m5gfx', 'src');
const CJSON = 'C:/esp/esp-idf/components/json/cJSON';
const OUT = join(root, 'build', 'ui-host', 'gallery');
const LGFX_OBJ = join(root, 'build', 'ui-host', 'lgfx');
const COMMON_OBJ = join(OUT, 'common');
const SHOTS = join(OUT, 'shots');
const SHEETS = join(OUT, 'sheets');
const GOLDEN = join(here, 'apps-golden.json');

const args = process.argv.slice(2);
const opt = (k) => (args.includes(k) ? args[args.indexOf(k) + 1] : null);
const update = args.includes('--update');
const lang = opt('--lang') || 'it';
const MINGW = 'C:/msys64/mingw64/bin';
const GPP = existsSync(`${MINGW}/g++.exe`) ? `${MINGW}/g++.exe` : 'g++';
const GCC = existsSync(`${MINGW}/gcc.exe`) ? `${MINGW}/gcc.exe` : 'gcc';
const AS = existsSync(`${MINGW}/as.exe`) ? `${MINGW}/as.exe` : 'as';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };

const sceneDir = join(here, 'apps');
const allKeys = readdirSync(sceneDir).filter((f) => f.endsWith('.txt')).map((f) => f.slice(0, -4)).sort();
const keys = opt('--app') ? opt('--app').split(',') : allKeys;
for (const d of [OUT, COMMON_OBJ, SHOTS, SHEETS, LGFX_OBJ]) mkdirSync(d, { recursive: true });

// ---- scene files -------------------------------------------------------------------------------------------
function readScenes(key) {
  const f = join(sceneDir, key + '.txt');
  const spec = { key, src: [], host: [], reg: `nucleo_register_${key}`, app: null, sd: [], scenes: [] };
  const text = existsSync(f) ? readFileSync(f, 'utf8') : 'home\n';
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line || line.startsWith('#')) continue;
    const [w, ...rest] = line.split(/\s+/);
    if (w === '@src') spec.src.push(...rest);
    else if (w === '@host') spec.host.push(...rest);
    else if (w === '@reg') spec.reg = rest[0];
    else if (w === '@app') spec.app = rest[0];
    else if (w === '@sd') spec.sd.push([rest[0], line.slice(line.indexOf(rest[0]) + rest[0].length + 1).replace(/\\n/g, '\n')]);
    else spec.scenes.push({ name: w, keys: rest });
  }
  if (!spec.scenes.length) spec.scenes.push({ name: 'home', keys: [] });
  return spec;
}

// ---- compile ------------------------------------------------------------------------------------------------
const compIncludes = readdirSync(FW).flatMap((c) => (existsSync(join(FW, c, 'include')) ? ['-I', join(FW, c, 'include')] : []));
const INC = [
  '-I', join(here, 'shim'), '-I', join(here, 'gshim'), '-I', GFX, '-I', join(root, 'build', 'ui-host'),
  '-I', APPS, ...compIncludes, '-I', CJSON, '-I', join(FW, 'libssh2', 'upstream', 'include'),
  '-I', join(root, 'firmware', 'managed_components', 'espressif__esp_new_jpeg', 'include'),
];
const COMPAT = ['-include', join(here, 'shim', 'host_compat.h'), '-include', 'esp_timer.h', '-include', 'esp_heap_caps.h'];
function run(cmd, argv) {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, argv, { env });
    let err = '';
    p.stdout.on('data', (b) => { err += b; });
    p.stderr.on('data', (b) => { err += b; });
    p.on('close', (code) => (code === 0 ? resolve(err) : reject(Object.assign(new Error(err), { out: err }))));
  });
}
const stale = (srcs, obj) => !existsSync(obj) || srcs.some((s) => statSync(obj).mtimeMs < statSync(s).mtimeMs);
// strict: the app's own source, with the warnings the firmware build turns into errors; the rest compiles quiet.
const warnings = [];
function compile(src, obj, extra = [], strict = false) {
  const cpp = /\.cpp$/.test(src);
  const w = strict ? ['-Wall', '-Wno-unused-function', '-Wno-format-truncation', '-Wno-missing-field-initializers', '-Wno-char-subscripts']
                   : ['-w', '-fpermissive'];
  return run(cpp ? GPP : GCC, [cpp ? '-std=gnu++17' : '-std=gnu11', '-O0', '-g', ...w, ...extra, ...COMPAT, ...INC, '-c', src, '-o', obj])
    .then((out) => {
      if (strict && /warning:/.test(out)) warnings.push(`${basename(src)}: warnings\n${out.split('\n').filter((l) => /warning:/.test(l)).slice(0, 12).join('\n')}`);
      return obj;
    }, (e) => { throw new Error(`${basename(src)}\n${e.message}`); });
}
async function pool(jobs, n = 8) {
  const out = []; let i = 0;
  await Promise.all(Array.from({ length: n }, async () => { while (i < jobs.length) { const j = jobs[i++]; out.push(await j()); } }));
  return out;
}

// LovyanGFX: the same cached objects as run.mjs (built there; rebuilt here only when missing or stale).
const lgfxSrc = [
  join(GFX, 'lgfx', 'v1', 'LGFXBase.cpp'), join(GFX, 'lgfx', 'v1', 'LGFX_Sprite.cpp'),
  join(GFX, 'lgfx', 'v1', 'lgfx_fonts.cpp'), join(GFX, 'lgfx', 'v1', 'panel', 'Panel_Device.cpp'),
  ...readdirSync(join(GFX, 'lgfx', 'v1', 'misc')).filter((f) => f.endsWith('.cpp')).map((f) => join(GFX, 'lgfx', 'v1', 'misc', f)),
  ...readdirSync(join(GFX, 'lgfx', 'utility')).filter((f) => f.endsWith('.c')).map((f) => join(GFX, 'lgfx', 'utility', f)),
  join(here, 'host_platform.cpp'),
];
const fontSyms = new Set(readFileSync(join(GFX, 'lgfx', 'v1', 'lgfx_fonts.cpp'), 'utf8').match(/\blgfx_(?:efont|font_japan)_\w+/g));
const fontStub = join(LGFX_OBJ, 'font_stubs.c');
const fontStubSrc = ['#include <stdint.h>', ...[...fontSyms].map((s) => `const uint8_t ${s}[64] = {0};`), ''].join('\n');
if (!existsSync(fontStub) || readFileSync(fontStub, 'utf8') !== fontStubSrc) writeFileSync(fontStub, fontStubSrc);
lgfxSrc.push(fontStub);
const lgfxObjs = await pool(lgfxSrc.map((src) => () => {
  const obj = join(LGFX_OBJ, basename(src) + '.o');
  if (!stale([src], obj)) return obj;
  const cpp = src.endsWith('.cpp');
  return run(cpp ? GPP : GCC, [cpp ? '-std=gnu++17' : '-std=gnu11', '-O1', '-w', '-I', join(here, 'shim'), '-I', GFX, '-c', src, '-o', obj]).then(() => obj);
}));

// The shared UI sources every app links (the real ones) + the host's display/clock stubs.
const commonSrc = [
  join(APPS, 'launcher_render.cpp'), join(APPS, 'app_ui.cpp'), join(APPS, 'app_large.cpp'), join(APPS, 'game_ui.cpp'), join(APPS, 'game_text.cpp'),
  join(FW, 'nucleo_ui', 'nucleo_theme.cpp'), join(FW, 'nucleo_ui', 'nucleo_ui_modal.cpp'),
  join(FW, 'nucleo_storage', 'nucleo_i18n.c'), join(CJSON, 'cJSON.c'), join(here, 'gallery_stubs.cpp'), join(here, 'host_stubs.cpp'),
];
const hdrs = [join(here, 'host_state.h'), join(here, 'shim', 'host_compat.h')];
let commonObjs;
try {
  commonObjs = await pool(commonSrc.map((src) => () => {
    const obj = join(COMMON_OBJ, basename(src) + '.o');
    return stale([src, ...hdrs], obj) ? compile(src, obj) : obj;
  }));
} catch (e) { console.error('apps: COMMON BUILD FAILED\n' + e.message); process.exit(1); }

// Undefined symbols -> "return 0" stubs (xor eax/xmm0; ret), in a writable section so a stubbed DATA symbol
// reads as zeros past its first bytes instead of faulting.
function stubAsm(syms) {
  return ['\t.section .gstub,"dwx"', ...[...syms].sort().flatMap((s) => [`\t.globl ${s}`, '\t.balign 16', `${s}:`,
    '\txorl %eax, %eax', '\txorps %xmm0, %xmm0', '\tret', '\t.zero 250']), ''].join('\n');
}
async function buildApp(spec) {
  const dir = join(OUT, 'obj', spec.key);
  mkdirSync(dir, { recursive: true });
  const srcs = [join(APPS, `app_${spec.key}.cpp`), ...spec.src.map((s) => (s.includes('/') ? join(FW, s) : join(APPS, s))), ...spec.host.map((s) => join(here, s))];
  const objs = await pool(srcs.map((src, i) => () => {
    const obj = join(dir, basename(src) + '.o');
    return stale([src, join(APPS, 'app_large.h')], obj) ? compile(src, obj, [], i === 0) : obj;
  }), 4);
  const gal = join(dir, 'gallery.o');
  await compile(join(here, 'gallery.cpp'), gal, [`-DGALLERY_REG=${spec.reg}`]);
  const exe = join(dir, `gallery_${spec.key}.exe`);
  const stubS = join(dir, 'stubs.s'), stubO = join(dir, 'stubs.o');
  const syms = new Set();
  for (let pass = 0; pass < 6; pass++) {
    writeFileSync(stubS, stubAsm(syms));
    await run(AS, [stubS, '-o', stubO]);
    try {
      await run(GPP, [gal, ...objs, ...commonObjs, ...lgfxObjs, stubO, '-Wl,--allow-multiple-definition', '-Wl,--no-demangle', '-o', exe]);
      return { exe, stubbed: syms.size };
    } catch (e) {
      const found = [...e.message.matchAll(/undefined reference to `([^']+)'/g)].map((m) => m[1]);
      if (!found.length) throw new Error(`link ${spec.key}\n${e.message.slice(0, 4000)}`);
      for (const s of found) syms.add(s);
    }
  }
  throw new Error(`link ${spec.key}: stubs did not converge`);
}

// ---- PNG -----------------------------------------------------------------------------------------------------
function pngRaw(rgb, w, h) {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0; rgb.copy(raw, y * (w * 3 + 1) + 1, y * w * 3, (y + 1) * w * 3); }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0);
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk('IHDR', ihdr),
    chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}
const dims = (rgb) => (rgb.length === 320 * 240 * 3 ? [320, 240] : [240, 135]);
function scaled(rgb, s) {
  const [W, H] = dims(rgb), w = W * s, h = H * s, big = Buffer.alloc(w * h * 3);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) rgb.copy(big, (y * w + x) * 3, ((Math.floor(y / s) * W) + Math.floor(x / s)) * 3, ((Math.floor(y / s) * W) + Math.floor(x / s)) * 3 + 3);
  return pngRaw(big, w, h);
}

// ---- render ----------------------------------------------------------------------------------------------------
const VARIANTS = ['compact', 'companion', 'large'];
const golden = existsSync(GOLDEN) ? JSON.parse(readFileSync(GOLDEN, 'utf8')) : {};
const hashes = {}, problems = [], built = [];
const BOX = join(OUT, 'box');
for (const key of keys) {
  const spec = readScenes(key);
  let b;
  try { b = await buildApp(spec); } catch (e) { problems.push(`${key}: BUILD FAILED\n${e.message.slice(0, 3000)}`); continue; }
  built.push(`${key} (${b.stubbed} stubs)`);
  for (const f of readdirSync(SHOTS)) if (f.startsWith(key + '.')) rmSync(join(SHOTS, f));
  const frames = {};
  await pool(spec.scenes.flatMap((sc) => VARIANTS.map((v) => () => new Promise((resolve) => {
    const box = join(BOX, `${key}.${sc.name}.${v}`);
    rmSync(box, { recursive: true, force: true });
    for (const d of ['sd/system/config', 'cfg/config', 'sd/data']) mkdirSync(join(box, d), { recursive: true });
    for (const [p, c] of spec.sd) { mkdirSync(dirname(join(box, 'sd', p)), { recursive: true }); writeFileSync(join(box, 'sd', p), c); }
    const p = spawn(b.exe, [SHOTS, `${key}.${sc.name}`, v, lang, ...sc.keys], { cwd: box, env: { ...env, ...(spec.app ? { GALLERY_APP: spec.app } : {}) } });
    let out = '';
    p.stdout.on('data', (x) => { out += x; });
    p.stderr.on('data', (x) => { out += x; });
    p.on('close', (code) => {
      for (const l of out.split('\n')) if (/^gallery-(check|note)/.test(l) && !(l.startsWith('gallery-note') && v === 'large')) problems.push(l.trim());
      if (code !== 0) problems.push(`${key}.${sc.name}.${v}: exit ${code} ${out.trim().slice(0, 300)}`);
      resolve();
    });
  }))), 8);
  for (const f of readdirSync(SHOTS).filter((f) => f.startsWith(key + '.') && f.endsWith('.rgb'))) {
    const rgb = readFileSync(join(SHOTS, f)), name = f.slice(0, -4);
    frames[name] = rgb;
    if (!name.endsWith('.touch')) hashes[name] = createHash('sha1').update(rgb).digest('hex').slice(0, 16);
    writeFileSync(join(SHOTS, name + '.png'), scaled(rgb, 2));
    rmSync(join(SHOTS, f));
  }
  // The app's sheet, 1:1: a row per scene — compact over companion, then large, then large with touch targets.
  const G = 6, RW = 240 + G + 320 + G + 320, RH = 135 + G + 135;
  const sh = spec.scenes.length * (RH + G), sheet = Buffer.alloc(RW * sh * 3, 0x30);
  const put = (rgb, ox, oy) => { if (!rgb) return; const [W, H] = dims(rgb); for (let y = 0; y < H; y++) rgb.copy(sheet, ((oy + y) * RW + ox) * 3, y * W * 3, (y + 1) * W * 3); };
  spec.scenes.forEach((sc, i) => {
    const n = `${key}.${sc.name}`, oy = i * (RH + G);
    put(frames[`${n}.compact`], 0, oy); put(frames[`${n}.companion`], 0, oy + 135 + G);
    put(frames[`${n}.large`], 240 + G, oy); put(frames[`${n}.touch`], 240 + G + 320 + G, oy);
  });
  writeFileSync(join(SHEETS, key + '.png'), pngRaw(sheet, RW, sh));
}

// ---- golden gate ------------------------------------------------------------------------------------------------
const mine = (n) => keys.includes(n.split('.')[0]);
const changed = Object.keys(hashes).filter((n) => n in golden && golden[n] !== hashes[n]);
const added = Object.keys(hashes).filter((n) => !(n in golden));
const missing = Object.keys(golden).filter((n) => mine(n) && !(n in hashes));
for (const w of warnings) problems.push(w);
for (const p of problems) console.log('  ' + p);
console.log(`apps: built ${built.join(', ')}`);
if (update) {
  const next = { ...Object.fromEntries(Object.entries(golden).filter(([n]) => !mine(n))), ...hashes };
  writeFileSync(GOLDEN, JSON.stringify(Object.fromEntries(Object.entries(next).sort()), null, 1) + '\n');
  console.log(`apps: ${Object.keys(hashes).length} shots, golden updated`);
  process.exit(problems.some((p) => /FAILED|exit/.test(p)) ? 1 : 0);
}
for (const n of changed) console.log(`  changed  build/ui-host/gallery/shots/${n}.png`);
for (const n of added) console.log(`  new      build/ui-host/gallery/shots/${n}.png`);
for (const n of missing) console.log(`  missing  ${n}`);
console.log(`apps: ${Object.keys(hashes).length} shots, ${changed.length} changed, ${added.length} new, ${missing.length} missing, ${problems.length} problems; sheets in build/ui-host/gallery/sheets/`);
process.exit(problems.length || changed.length || missing.length ? 1 : 0);

#!/usr/bin/env node
// The Games gallery's bench: builds tools/ui-host/gamefront_bench.cpp against the REAL gamefront.cpp (and the objects
// `node tools/ui-host/apps.mjs --app gamefront` leaves), runs the browse on the card of apps/gamefront.txt (plus a few
// more screenshots, so a console mixes titles with and without one) on the panel and on the Cardputer alone, and
// prints what each phase cost: card accesses, frames and pixels pushed, pictures from the RAM cache.
//   node tools/ui-host/gamefront-bench.mjs [--heap nocanvas|tight] [--json out.json]
import { spawn, spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, existsSync, readdirSync, rmSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const FW = join(root, 'firmware', 'components');
const APPS = join(FW, 'nucleo_app');
const GFX = join(root, 'firmware', 'managed_components', 'm5stack__m5gfx', 'src');
const GAL = join(root, 'build', 'ui-host', 'gallery');
const OBJ = join(GAL, 'obj', 'gamefront');
const OUT = join(GAL, 'bench');
const MINGW = 'C:/msys64/mingw64/bin';
const GPP = existsSync(`${MINGW}/g++.exe`) ? `${MINGW}/g++.exe` : 'g++';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };
const args = process.argv.slice(2);

if (!existsSync(join(OBJ, 'stubs.o'))) {                // the gallery's build first (common objects, stubs)
  const r = spawnSync(process.execPath, [join(here, 'apps.mjs'), '--app', 'gamefront'], { stdio: 'inherit' });
  if (!existsSync(join(OBJ, 'stubs.o'))) process.exit(r.status || 1);
}
mkdirSync(OUT, { recursive: true });
const compIncludes = readdirSync(FW).flatMap((c) => (existsSync(join(FW, c, 'include')) ? ['-I', join(FW, c, 'include')] : []));
const INC = ['-I', join(here, 'shim'), '-I', join(here, 'gshim'), '-I', GFX, '-I', join(root, 'build', 'ui-host'), '-I', APPS, ...compIncludes];
const COMPAT = ['-include', join(here, 'shim', 'host_compat.h'), '-include', 'esp_timer.h', '-include', 'esp_heap_caps.h'];
const cc = (src, obj, extra = []) => {
  const r = spawnSync(GPP, ['-std=gnu++17', '-O0', '-g', '-DNUCLEO_UI_HOST=1', '-w', '-fpermissive', ...extra, ...COMPAT, ...INC, '-c', src, '-o', obj], { env, encoding: 'utf8' });
  if (r.status) { console.error(r.stdout + r.stderr); process.exit(1); }
  return obj;
};
const objs = [
  cc(join(APPS, 'gamefront.cpp'), join(OUT, 'gamefront.o')),
  cc(join(here, 'gamefront_bench.cpp'), join(OUT, 'bench.o')),
  cc(join(here, 'gallery.cpp'), join(OUT, 'gallery.o'), ['-DGALLERY_REG=nucleo_register_gamefront', '-Dmain=gallery_main_unused']),
  join(OBJ, 'gamefront_host.cpp.o'), join(OBJ, 'tinfl_zlib.cpp.o'), join(OBJ, 'stubs.o'),
  ...readdirSync(join(GAL, 'common')).map((f) => join(GAL, 'common', f)),
  ...readdirSync(join(root, 'build', 'ui-host', 'lgfx')).filter((f) => f.endsWith('.o')).map((f) => join(root, 'build', 'ui-host', 'lgfx', f)),
];
const wraps = ['fopen', 'fread', 'fgets', 'stat64i32', 'opendir', 'readdir', '_ZN2lg5frameEPFvPvES0_iib', '_ZN2lg10frame_rectEPFvPvES0_iiiib'];
const exe = join(OUT, 'gamefront_bench.exe');
const ld = spawnSync(GPP, [...objs, '-lz', '-Wl,--allow-multiple-definition', '-Wl,--no-demangle', ...wraps.map((w) => `-Wl,--wrap=${w}`), '-o', exe], { env, encoding: 'utf8' });
if (ld.status) { console.error(ld.stdout + ld.stderr); process.exit(1); }

// The card: the gallery scenes' own, and screenshots for half of the Game Boy titles.
const sd = [];
for (const raw of readFileSync(join(here, 'apps', 'gamefront.txt'), 'utf8').split(/\r?\n/)) {
  const [w, p, f] = raw.trim().split(/\s+/);
  if (w === '@sd') sd.push([decodeURIComponent(p), 'x']);
  if (w === '@sdfile') sd.push([decodeURIComponent(p), readFileSync(join(here, f))]);
}
const snp = readFileSync(join(here, 'apps', 'fixtures', 'test.snp'));
for (const t of ['Tetris (World)', 'Kirby\'s Dream Land (USA, Europe)', 'GB Wordyl', 'Tobu Tobu Girl Deluxe'])
  sd.push([`data/ROMs/gb/.snaps/${t}.gb.snp`, snp]);

const run = (variant, heap) => new Promise((resolve) => {
  const box = join(OUT, variant + heap);
  rmSync(box, { recursive: true, force: true });
  for (const d of ['sd/system/config', 'cfg/config', 'sd/data']) mkdirSync(join(box, d), { recursive: true });
  for (const [p, c] of sd) { mkdirSync(dirname(join(box, 'sd', p)), { recursive: true }); writeFileSync(join(box, 'sd', p), c); }
  const p = spawn(exe, [variant, heap], { cwd: box, env });
  let out = '';
  p.stdout.on('data', (x) => { out += x; });
  p.stderr.on('data', (x) => { out += x; });
  p.on('close', (code) => {
    const rows = []; let ram = 0;
    for (const l of out.split(/\r?\n/)) {
      if (/picture \d+ from RAM/.test(l)) ram++;
      const m = l.match(/^PHASE (\S+) (.*)$/);
      if (!m) continue;
      const [opens, misses, writes, bytes, stats, dirs, whole, rects, px, cpu] = m[2].trim().split(' ').map(Number);
      rows.push({ phase: m[1], opens, misses, writes, kb: +(bytes / 1024).toFixed(1), stats, dirs, whole, rects, kpx: Math.round(px / 1000), ram, cpu });
      ram = 0;
    }
    writeFileSync(join(OUT, `${variant}${heap}.log`), out);   // BENCH_TRACE=1: every open, stat and listing, by phase
    if (code) console.error(`${variant}: exit ${code}\n${out.slice(-1500)}`);
    resolve(rows);
  });
});

// --heap nocanvas | tight: the ADV without its 32 KB canvas, or a fragmented heap (largest free block 9 KB).
const heap = args.includes('--heap') ? args[args.indexOf('--heap') + 1] : '';
const result = {};
for (const v of ['large', 'compact']) {
  const rows = await run(v, heap);
  result[v] = rows;
  console.log(`\n${v === 'large' ? 'panel 320x240 (two screens)' : 'Cardputer 240x135'}${heap ? ` (${heap})` : ''}`);
  console.log('phase          opens miss  KB read stats dirs whole rects    Kpx  ram  cpu ms');
  const tot = { opens: 0, misses: 0, kb: 0, stats: 0, dirs: 0, whole: 0, rects: 0, kpx: 0, ram: 0, cpu: 0 };
  for (const r of rows) {
    console.log(`${r.phase.padEnd(14)} ${String(r.opens).padStart(5)} ${String(r.misses).padStart(4)} ${r.kb.toFixed(1).padStart(8)} ${String(r.stats).padStart(5)} ${String(r.dirs).padStart(4)} ${String(r.whole).padStart(5)} ${String(r.rects).padStart(5)} ${String(r.kpx).padStart(6)} ${String(r.ram).padStart(4)} ${r.cpu.toFixed(0).padStart(7)}`);
    if (r.phase !== 'open') for (const k of Object.keys(tot)) tot[k] += r[k];
  }
  console.log(`${'browse total'.padEnd(14)} ${String(tot.opens).padStart(5)} ${String(tot.misses).padStart(4)} ${tot.kb.toFixed(1).padStart(8)} ${String(tot.stats).padStart(5)} ${String(tot.dirs).padStart(4)} ${String(tot.whole).padStart(5)} ${String(tot.rects).padStart(5)} ${String(tot.kpx).padStart(6)} ${String(tot.ram).padStart(4)} ${tot.cpu.toFixed(0).padStart(7)}`);
}
const j = args.indexOf('--json');
if (j >= 0) writeFileSync(args[j + 1], JSON.stringify(result, null, 1));

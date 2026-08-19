// gb-check — host gate for the NATIVE Game Boy emulator.
//
// Compiles the VENDORED core (firmware/components/nucleo_emu/vendor/peanut_gb.h) with the same build
// switches the firmware uses, then boots every ROM it can find in the SD-sim library and runs real
// frames. Host-first, per CLAUDE.md: the core and our host callbacks are proven on the PC before
// anything reaches a board.
//
// What it actually guards:
//   * sizeof(struct gb_s) — the number the whole design rests on (~16.9 KB). A core update that
//     doubles it must fail HERE, not on a device with ~60 KB of contiguous heap.
//   * the PPU really emits 144 scanlines per frame, and they are not all blank. "It compiled" and
//     "it loads" both pass while rendering nothing; this does not.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
// gcc needs MinGW on PATH to find its own DLLs — without this it exits non-zero with no diagnostic.
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

if (!existsSync(BUILD)) mkdirSync(BUILD, { recursive: true });
const exe = join(BUILD, 'gbtest.exe');

const cc = spawnSync(GCC, [
  '-std=gnu11', '-O2', '-Wall',
  '-DMINIGB_APU_AUDIO_FORMAT_S16SYS=1',   // the APU refuses to build without an explicit format
  join(ROOT, 'tools', 'emu-host', 'gb_test.c'),
  // The APU is a separate translation unit in the firmware too — compile it the same way here, so
  // the gate links the exact code the device runs rather than a header-only approximation.
  join(ROOT, 'firmware', 'components', 'nucleo_emu', 'vendor', 'minigb_apu.c'),
  '-o', exe,
], { env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('gb-check: compile FAILED');
  console.error(cc.stderr || cc.stdout || '(no compiler output)');
  process.exit(1);
}

// Real cartridges from the simulator's own library — the same files the device would read.
const romDirs = [
  join(ROOT, 'tools', 'sd-sim', 'data', 'ROMs', 'gb'),
  join(ROOT, 'tools', 'sd-sim', 'data', 'ROMs', 'gbc'),
];
// Collect the whole library, then sample deliberately. The RENDER gate wants a spread of titles and
// the CACHE gate wants the BIGGEST cartridges, because a 32 KB ROM stays resident in RAM and so
// proves nothing at all about a cache. Taking the first six filenames alphabetically served neither.
const all = [];
for (const dir of romDirs) {
  if (!existsSync(dir)) continue;
  for (const f of readdirSync(dir)) {
    if (/\.gbc?$/i.test(f)) all.push(join(dir, f));
  }
}
const roms = all.slice(0, 6);                    // a representative sample; the gate is a guard, not a soak

if (!all.length) {
  console.error('gb-check: no .gb/.gbc ROMs in tools/sd-sim/data/ROMs — cannot verify rendering.');
  console.error('          (ROMs are deliberately not committed; drop a few in to run this gate.)');
  process.exit(1);
}

const run = spawnSync(exe, roms, { env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
let failed = run.status !== 0;

// ── part two: the ROM page cache, against the REAL firmware source ───────────────────────────────
// The gate above supplies its own callbacks, so it never touches nucleo_gb.c — the file that decides
// where a cartridge lives. That gap once let this gate stay green while a device answered "invalid
// ROM". This half compiles nucleo_gb.c itself and drives its public API, with NUCLEO_HOST_HEAP set
// to the ~60 KB largest block a Solo boot actually offers, so the SD-paged path is what runs.
console.log('\n— ROM page cache (real nucleo_gb.c, device-sized heap) —');
const cacheExe = join(BUILD, 'gbcachetest.exe');
const cc2 = spawnSync(GCC, [
  '-std=gnu11', '-O2', '-w',
  '-DMINIGB_APU_AUDIO_FORMAT_S16SYS=1',
  '-I' + join(ROOT, 'tools', 'anima-host', 'shim'),
  '-I' + join(ROOT, 'firmware', 'components', 'nucleo_emu', 'include'),
  '-I' + join(ROOT, 'firmware', 'components', 'nucleo_audio', 'include'),
  join(ROOT, 'tools', 'emu-host', 'gb_cache_test.c'),
  join(ROOT, 'firmware', 'components', 'nucleo_emu', 'nucleo_gb.c'),
  join(ROOT, 'firmware', 'components', 'nucleo_emu', 'vendor', 'minigb_apu.c'),
  '-o', cacheExe,
], { env, encoding: 'utf8' });

if (cc2.status !== 0) {
  console.error('gb-check: cache gate compile FAILED');
  console.error(cc2.stderr || cc2.stdout || '(no compiler output)');
  process.exit(1);
}

// Prefer the BIGGEST cartridges available: a 32 KB ROM stays resident and proves nothing about the
// cache, and it is the 256-512 KB games that thrash a badly-sized one.
const bySize = all
  .map((r) => ({ r, size: statSync(r).size }))
  .sort((a, b) => b.size - a.size)
  .slice(0, 4)
  .map((x) => x.r);

const run2 = spawnSync(cacheExe, bySize, {
  env: { ...env, NUCLEO_HOST_HEAP: String(240 * 1024) },   // -> largest block 60 KB, as on a Solo boot
  encoding: 'utf8',
});
process.stdout.write(run2.stdout || '');
if (run2.stderr) process.stderr.write(run2.stderr);
if (run2.status !== 0) failed = true;

console.log(failed ? '\ngb-check: FAILED' : '\ngb-check: all green');
process.exit(failed ? 1 : 0);

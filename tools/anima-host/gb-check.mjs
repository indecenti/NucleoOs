// gb-check — host gate for the NATIVE Game Boy emulator.
//
// Compiles the VENDORED core (firmware/components/nucleo_emu/vendor/peanut_gb.h) with the same build
// switches the firmware uses, then boots every ROM it can find in the SD-sim library and runs real
// frames. Host-first, per CLAUDE.md: the core and our host callbacks are proven on the PC before
// anything reaches a board.
//
// What it actually guards:
//   * sizeof(struct gb_s) — the number the whole design rests on (~16.9 KB). A core update that
//     doubles it must fail HERE, not on a device whose largest free block is 32 KB.
//   * the PPU really emits 144 scanlines per frame, and they are not all blank. "It compiled" and
//     "it loads" both pass while rendering nothing; this does not.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
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
// ROM". This half compiles nucleo_gb.c itself and drives its public API against a MODEL of the
// device heap (free blocks, best fit — the Solo-boot shape, largest block 32 KB), so the SD-paged path
// is what runs and an allocation that could not succeed on the device cannot succeed here either.
console.log('\n— ROM page cache (real nucleo_gb.c, device-sized heap) —');
const cacheExe = join(BUILD, 'gbcachetest.exe');
const EMU = join(ROOT, 'tools', 'emu-host');
const shimInc = [
  '-DMINIGB_APU_AUDIO_FORMAT_S16SYS=1',
  // newlib (the device) has localtime_r unconditionally; MinGW only exposes it under this macro.
  '-D_POSIX_THREAD_SAFE_FUNCTIONS',
  // heap_caps_* answer from the device-heap MODEL in gb_cache_test.c, not from a PC-sized pool.
  '-DNUCLEO_HOST_HEAP_MODEL',
  '-I' + join(ROOT, 'tools', 'anima-host', 'shim'),
  '-I' + join(ROOT, 'firmware', 'components', 'nucleo_emu', 'include'),
  '-I' + join(ROOT, 'firmware', 'components', 'nucleo_audio', 'include'),
];
// nucleo_gb.c alone gets heap_model.h force-included, so ITS malloc/calloc/free go through the model
// while the harness keeps the real allocator.
const gbObj = join(BUILD, 'nucleo_gb_host.o');
let cc2 = spawnSync(GCC, ['-std=gnu11', '-O2', '-w', ...shimInc, '-include', join(EMU, 'heap_model.h'),
  '-c', join(ROOT, 'firmware', 'components', 'nucleo_emu', 'nucleo_gb.c'), '-o', gbObj], { env, encoding: 'utf8' });
if (cc2.status === 0) cc2 = spawnSync(GCC, [
  '-std=gnu11', '-O2', '-w', ...shimInc,
  join(EMU, 'gb_cache_test.c'),
  gbObj,
  join(ROOT, 'firmware', 'components', 'nucleo_emu', 'vendor', 'minigb_apu.c'),
  // nucleo_gb.c times its own frames, so the harness needs the same monotonic clock the rest of the
  // host gates use (QueryPerformanceCounter behind the ESP-IDF name).
  join(ROOT, 'tools', 'anima-host', 'esp_timer_host.c'),
  '-o', cacheExe,
], { env, encoding: 'utf8' });

if (cc2.status !== 0) {
  console.error('gb-check: cache gate compile FAILED');
  console.error(cc2.stderr || cc2.stdout || '(no compiler output)');
  process.exit(1);
}

// Prefer the BIGGEST cartridges available: a 32 KB ROM stays resident and proves nothing about the
// cache, and it is the 256-512 KB games that thrash a badly-sized one.
// Game Boy Color-ONLY carts (header 0x143 == 0xC0) are refused by design (a DMG core would draw
// garbage), so they cannot be the cache sample — but ONE of them rides along to prove the refusal.
const cgbOnly = (r) => { try { const b = readFileSync(r); return b.length > 0x143 && b[0x143] === 0xC0; } catch { return false; } };
const bySize = all
  .filter((r) => !cgbOnly(r))
  .map((r) => ({ r, size: statSync(r).size }))
  .sort((a, b) => b.size - a.size)
  .slice(0, 4)
  .map((x) => x.r);
const refuse = all.find(cgbOnly);
if (refuse) bySize.push(refuse);

// A 32 KB-SRAM cartridge (Pokemon Red/Blue/Yellow, and most late MBC3/MBC5 RPGs). The library here may
// have none, so one is SYNTHESISED from the biggest MBC1+RAM cart: RAM-size byte -> 3 (32 KB), header
// checksum recomputed. It exists to prove the battery RAM still finds room on the device-shaped heap —
// allocated as one 32 KB block it could not (the heap's largest block IS 32 KB, and the core wants it).
const donor = bySize.find((r) => { const b = readFileSync(r); return b[0x147] === 0x03; });
if (donor) {
  const b = Buffer.from(readFileSync(donor));
  b[0x149] = 3;
  let x = 0; for (let i = 0x134; i <= 0x14C; i++) x = (x - b[i] - 1) & 0xFF;
  b[0x14D] = x;
  // Into the gitignored test-ROM cache, never build/: it is a derivative of a commercial cartridge.
  const synthDir = join(ROOT, 'tools', 'emu-host', 'testroms');
  mkdirSync(synthDir, { recursive: true });
  const synth = join(synthDir, 'gb-sram32-synthetic.gb');
  writeFileSync(synth, b);
  bySize.push(synth);
}

// mooneye's MBC1 RAM tests, through the real module: ram_256kb switches across all four SRAM banks,
// which on this heap means two resident and two swapped to the card — every swap path, verified by the
// test ROM itself. Present once gb-accuracy has fetched the suites (it runs as part three below).
const mtsRoot = join(ROOT, 'tools', 'emu-host', 'testroms', 'mts');
if (existsSync(mtsRoot)) {
  for (const d of readdirSync(mtsRoot)) {
    for (const t of ['ram_64kb.gb', 'ram_256kb.gb']) {
      const p = join(mtsRoot, d, 'emulator-only', 'mbc1', t);
      if (existsSync(p)) bySize.push('mooneye:' + p);
    }
  }
}

const run2 = spawnSync(cacheExe, bySize, {
  // The emulator's Solo boot as /gbemu_trace.txt measures it once the canvas is released: ~90 KB free,
  // largest block 32 KB (sum of the first nine pieces: 90,112 B). The last two are what play() now also
  // hands back before it opens a cartridge, both of which were still allocated when that was measured:
  // the shelf's name block (swapped to the card, 6,628 B) and the title-scroller sprite (6,336 B). The
  // harness takes the app's two band buffers out of it before each open, exactly as play() does.
  env: { ...env, NUCLEO_HOST_HEAP_BLOCKS: '32768,20480,12288,8192,6144,4096,3072,2048,1024,6628,6336' },
  encoding: 'utf8',
});
process.stdout.write(run2.stdout || '');
if (run2.stderr) process.stderr.write(run2.stderr);
if (run2.status !== 0) failed = true;

// ── part three: accuracy ratchet against the reference suites (blargg, mooneye, dmg-acid2) ─────────
// Parts one and two prove it renders and pages; this proves it is RIGHT, and that no core update or
// patch quietly un-fixes something (see tools/emu-host/gb-accuracy.mjs). SKIPs itself when offline.
console.log('\n— Accuracy ratchet (reference test ROMs) —');
const acc = spawnSync(process.execPath, [join(ROOT, 'tools', 'emu-host', 'gb-accuracy.mjs')], { env, encoding: 'utf8' });
process.stdout.write(acc.stdout || '');
if (acc.stderr) process.stderr.write(acc.stderr);
if (acc.status !== 0) failed = true;

console.log(failed ? '\ngb-check: FAILED' : '\ngb-check: all green');
process.exit(failed ? 1 : 0);

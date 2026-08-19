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
import { existsSync, mkdirSync, readdirSync } from 'node:fs';
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
const roms = [];
for (const dir of romDirs) {
  if (!existsSync(dir)) continue;
  for (const f of readdirSync(dir)) {
    if (/\.gbc?$/i.test(f)) roms.push(join(dir, f));
    if (roms.length >= 6) break;                 // a representative sample; the gate is a guard, not a soak
  }
}

if (!roms.length) {
  console.error('gb-check: no .gb/.gbc ROMs in tools/sd-sim/data/ROMs — cannot verify rendering.');
  console.error('          (ROMs are deliberately not committed; drop a few in to run this gate.)');
  process.exit(1);
}

const run = spawnSync(exe, roms, { env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === 0 ? 0 : 1);

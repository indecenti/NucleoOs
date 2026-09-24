// gg-check — host gate for the NATIVE Game Gear emulator (npm run gg:test).
//
// Compiles firmware/components/nucleo_emu/nucleo_gg.c ITSELF (with the vendored z80emu inside it) and
// drives its public API against a MODEL of the device heap — the emulator's Solo boot as
// /gbemu_trace.txt measured it, the app's two DMA bands and the speaker's DMA channel taken out first —
// so the cartridge cache it measures is the one the device would get. See tools/emu-host/gg_test.c.
//
// What it guards, per cartridge: every frame emits 144 lines; the picture is colour, not a flat field;
// the page cache stays under the SD budget (1.4 misses/frame); open/close returns every byte; audio
// is produced every frame; a save state replays bit-exactly.
//
// The sample is deliberate: the biggest cartridges (the cache), the Codemasters mapper, and the
// cartridges that once hung on the power-on state or the status-poll race (docs/native-emulation.md §7).
// `node tools/anima-host/gg-check.mjs --sweep` runs the whole library instead and writes contact
// sheets to build/gg-sweep/ (a human look at every game, one PNG per 25). Last, the Z80 itself is held
// to zexall (tools/emu-host/zex_test.c).
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
if (!existsSync(BUILD)) mkdirSync(BUILD, { recursive: true });

const EMU = join(ROOT, 'tools', 'emu-host');
const inc = [
  '-D_POSIX_THREAD_SAFE_FUNCTIONS', '-DNUCLEO_HOST_HEAP_MODEL', '-DNUCLEO_HOST_ZLIB',
  '-I' + join(ROOT, 'tools', 'anima-host', 'shim'),
  '-I' + join(ROOT, 'firmware', 'components', 'nucleo_emu', 'include'),
  '-I' + join(ROOT, 'firmware', 'components', 'nucleo_audio', 'include'),
];
const obj = join(BUILD, 'nucleo_gg_host.o');
const exe = join(BUILD, 'ggtest.exe');
let cc = spawnSync(GCC, ['-std=gnu11', '-O2', '-Wall', '-Wextra', ...inc, '-include', join(EMU, 'heap_model.h'),
  '-c', join(ROOT, 'firmware', 'components', 'nucleo_emu', 'nucleo_gg.c'), '-o', obj], { env, encoding: 'utf8' });
if (cc.status === 0 && /warning:/.test(cc.stderr || '')) {
  console.error('gg-check: nucleo_gg.c compiles with warnings:\n' + cc.stderr);
  process.exit(1);
}
if (cc.status === 0) cc = spawnSync(GCC, ['-std=gnu11', '-O2', '-Wall', ...inc, join(EMU, 'gg_test.c'), obj,
  join(ROOT, 'tools', 'anima-host', 'esp_timer_host.c'), '-lz', '-o', exe], { env, encoding: 'utf8' });
if (cc.status !== 0) {
  console.error('gg-check: compile FAILED');
  console.error(cc.stderr || cc.stdout || '(no compiler output)');
  process.exit(1);
}

const lib = join(ROOT, 'tools', 'sd-sim', 'data', 'ROMs', 'gg');
const libSms = join(ROOT, 'tools', 'sd-sim', 'data', 'ROMs', 'sms');
const list = (dir, re) => (existsSync(dir) ? readdirSync(dir).filter((f) => re.test(f)).map((f) => join(dir, f)) : []);
const all = [...list(lib, /\.(gg|zip)$/i), ...list(libSms, /\.(sms|zip)$/i)];   // zipped like the Arcade's
if (!all.length) {
  console.error('gg-check: no .gg ROMs in tools/sd-sim/data/ROMs/gg — cannot verify.');
  console.error('          (ROMs are deliberately not committed; drop a few in to run this gate.)');
  process.exit(1);
}

// The device's Solo-boot heap after the canvas and the shelf are released (same profile as gb-check).
const heap = { ...env, NUCLEO_HOST_HEAP_BLOCKS: '32768,20480,12288,8192,6144,4096,3072,2048,1024,6628,6336' };

if (process.argv.includes('--sweep')) {
  const out = join(BUILD, 'gg-sweep');
  mkdirSync(out, { recursive: true });
  const r = spawnSync(exe, ['--sweep', out, ...all], { env: heap, encoding: 'utf8', maxBuffer: 64 << 20 });
  process.stdout.write(r.stdout || '');
  process.exit(r.status === 0 ? 0 : 1);
}

const bySize = all.map((r) => ({ r, size: statSync(r).size })).sort((a, b) => b.size - a.size).map((x) => x.r);
const named = (re) => all.filter((r) => re.test(r));
const sample = [...new Set([
  ...bySize.slice(0, 3),                                            // the cache under the heaviest load
  ...named(/Sonic the Hedgehog 2|Sonic Chaos|Earthworm Jim/i),      // measured thrashers at 44-50 pages
  ...named(/Micro Machines 2|Cosmic Spacehead/i),                   // Codemasters mapper
  ...named(/Evander Holyfield|Monster Truck Wars|Chicago Syndicate|Pac-Attack/i),   // fixed hangs (§7)
  ...named(/Castle of Illusion|Prince of Persia/i),                 // Game Gear carts in Master System mode
  ...named(/World Series Baseball \(/i),                            // 93C46 EEPROM
  // Master System cartridges (zipped here). Ace of Aces is left out: it never initialises the VDP and
  // crashes within a second in SMS Plus exactly as it does here — a dump/BIOS question, not ours.
  ...all.filter((r) => r.startsWith(libSms) && !/Ace of Aces/i.test(r)).slice(0, 2),
])].slice(0, 18);

const run = spawnSync(exe, sample, { env: heap, encoding: 'utf8', maxBuffer: 64 << 20 });
const quiet = (t) => (t || '').split('\n').filter((l) => !/^[IW] \(gg\)/.test(l)).join('\n');
process.stdout.write(quiet(run.stdout));
process.stderr.write(quiet(run.stderr));
let failed = run.status !== 0;

// ── the CPU: zexall, against the vendored z80emu built with OUR z80config.h (HALT/EI caught) ─────────
// Fetched once into the gitignored test-ROM folder, SHA-256 pinned; skipped (not failed) offline.
console.log('\n— Z80 instruction exerciser (zexall) —');
const ZEX = { file: 'zexall.com', sha: '6e2da55147a04f28d303d5da6a1e6b771557ac244653590a0f24a2d39c8537e8',
  url: 'https://raw.githubusercontent.com/anotherlin/z80emu/1c418fa0d719abab9273131113defbe276101d95/testfiles/zexall.com' };
const zexDir = join(EMU, 'testroms', 'zex');
const zexPath = join(zexDir, ZEX.file);
const sha256 = (b) => createHash('sha256').update(b).digest('hex');
if (!existsSync(zexPath)) {
  try {
    const r = await fetch(ZEX.url, { redirect: 'follow' });
    const buf = Buffer.from(await r.arrayBuffer());
    if (r.ok && sha256(buf) === ZEX.sha) { mkdirSync(zexDir, { recursive: true }); writeFileSync(zexPath, buf); }
  } catch { /* offline */ }
}
if (existsSync(zexPath) && sha256(readFileSync(zexPath)) === ZEX.sha) {
  const zexe = join(BUILD, 'zextest.exe');
  const zc = spawnSync(GCC, ['-std=gnu11', '-O2', '-Wall', '-Wno-unused-function', join(EMU, 'zex_test.c'), '-o', zexe], { env, encoding: 'utf8' });
  const zr = zc.status === 0 ? spawnSync(zexe, [zexPath], { env, encoding: 'utf8', maxBuffer: 16 << 20 }) : zc;
  const ok = (zr.stdout || '').match(/OK/g)?.length || 0;
  console.log(zr.status === 0 ? `zexall: ${ok} instruction groups OK` : 'zexall: FAILED\n' + (zr.stdout || zr.stderr || ''));
  if (zr.status !== 0) failed = true;
} else {
  console.log('zexall: SKIP (offline, not cached)');
}

console.log(failed ? '\ngg-check: FAILED' : `\ngg-check: all green (${sample.length} cartridges + zexall)`);
process.exit(failed ? 1 : 0);

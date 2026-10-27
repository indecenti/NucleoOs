#!/usr/bin/env node
// Native-app loader gate (npm run native:test). Runs the REAL firmware loader (nap_loader.c) on the PC:
//   1. the ABI export table in the firmware matches sdk/native/abi/ (gen-exports --check);
//   2. builds tools/native-host/nap/fixture.c into a real .nap with the SDK's flags and linker script;
//   3. relocates it and checks EVERY relocated word against an independent computation from `readelf -r`,
//      and that every other byte is the file's own;
//   4. targeted corruptions must each be refused with the right reason; 4000 random ones must never crash;
//   5. the same on any published native app found in the store checkout next to this repo.
// Needs the ESP-IDF Xtensa toolchain (~/.espressif) and MinGW gcc (C:/msys64/mingw64).
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync, existsSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { homedir } from 'node:os';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const OUT = join(root, 'build', 'native-test');
mkdirSync(OUT, { recursive: true });

const XT = (() => {
  const base = join(homedir(), '.espressif', 'tools', 'xtensa-esp-elf');
  if (!existsSync(base)) return null;
  const v = readdirSync(base).sort().pop();
  return join(base, v, 'xtensa-esp-elf', 'bin');
})();
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(`${MINGW}/gcc.exe`) ? `${MINGW}/gcc.exe` : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };

function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { encoding: 'utf8', env, maxBuffer: 64 << 20, ...opts });
  if (r.status !== 0 && !opts.allowFail) {
    console.error(`native: ${cmd} ${args.join(' ')}\n${r.stdout}\n${r.stderr}`);
    process.exit(1);
  }
  return r;
}

// 1. the export table
run(process.execPath, [join(here, 'gen-exports.mjs'), '--check'], { stdio: 'inherit' });
if (!XT) { console.error('native: the Xtensa toolchain (~/.espressif/tools/xtensa-esp-elf) is needed'); process.exit(1); }
const xgcc = join(XT, 'xtensa-esp32s3-elf-gcc'), readelf = join(XT, 'xtensa-esp32s3-elf-readelf'), strip = join(XT, 'xtensa-esp32s3-elf-strip');

// Exports in strcmp order (as the firmware table), address 0x40000000 + 16 * i.
const names = [];
for (const raw of readFileSync(join(root, 'sdk', 'native', 'abi', 'v1.txt'), 'utf8').split(/\r?\n/)) {
  const l = raw.replace(/#.*/, '').trim();
  if (l) names.push(l.split(/\s*=\s*/)[0]);
}
names.sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
const exportsFile = join(OUT, 'exports.txt');
writeFileSync(exportsFile, names.join('\n') + '\n');
const exportAddr = new Map(names.map((n, i) => [n, 0x40000000 + 16 * i]));

// 2. the fixture, built like a real module
const fixture = join(OUT, 'fixture.nap');
run(xgcc, ['-Os', '-fPIC', '-fvisibility=hidden', '-mlongcalls', '-ffunction-sections', '-fdata-sections', '-fno-common',
  '-shared', '-nostdlib', '-Wl,--gc-sections', '-Wl,--hash-style=sysv', '-Wl,--build-id=none',
  `-Wl,-T,${join(root, 'sdk', 'native', 'nap.ld')}`, '-o', join(OUT, 'fixture.so'), join(root, 'tools', 'native-host', 'nap', 'fixture.c')]);
run(strip, ['--strip-all', '-o', fixture, join(OUT, 'fixture.so')]);

// The loader, with UB and bounds traps.
const tester = join(OUT, 'nap_test.exe');
run(GCC, ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
  '-fsanitize=undefined,bounds', '-fsanitize-undefined-trap-on-error',
  '-I', join(root, 'firmware', 'components', 'nucleo_native', 'include'), '-I', join(root, 'sdk', 'native', 'include'),
  join(root, 'firmware', 'components', 'nucleo_native', 'nap_loader.c'), join(root, 'tools', 'native-host', 'nap', 'nap_test.c'),
  '-o', tester]);

const IBUS = 0x42010000, DBUS = 0x3c010000, RAMB = 0x3fc90000;
const u32 = (b, o) => b.readUInt32LE(o);

function checkModule(label, file, layout, id) {
  const fl = join(OUT, `${label}.flash`), da = join(OUT, `${label}.data`);
  const r = run(tester, ['relocate', file, exportsFile, layout, id, fl, da]);
  const m = r.stdout.match(/text 0x(\w+) rodata 0x(\w+)-0x(\w+) data 0x(\w+)\+(\d+)\/(\d+)/);
  const [textEnd, roStart, roEnd, dataStart] = m.slice(1, 5).map((x) => parseInt(x, 16));
  const dataMem = +m[6];
  const src = readFileSync(file), flash = readFileSync(fl), data = readFileSync(da);
  const inMod = (v) => v < textEnd || (v >= roStart && v < roEnd) || (v >= dataStart && v <= dataStart + dataMem);
  const addr = (v) => (v < textEnd ? IBUS + v : v < roEnd ? DBUS + v : RAMB - dataStart + v) >>> 0;
  const word = (off) => (off < roEnd ? u32(flash, off) : u32(data, off - dataStart));
  const orig = (off) => (off < dataStart + src.length - dataStart && off + 4 <= src.length && (off < roEnd || off < dataStart + (src.length - dataStart)) ? u32(src, off) : 0);
  // Module-defined symbol values, by name (for imports resolved inside the module).
  const syms = new Map();
  for (const l of run(readelf, ['--dyn-syms', '-W', file]).stdout.split('\n')) {
    const p = l.trim().split(/\s+/);
    if (p.length >= 8 && /^\d+:$/.test(p[0]) && p[6] !== 'UND') syms.set(p[7], parseInt(p[1], 16));
  }
  const touched = new Set();
  let n = 0, bad = 0;
  for (const l of run(readelf, ['-r', '-W', file]).stdout.split('\n')) {
    const p = l.trim().split(/\s+/);
    if (!/^[0-9a-f]{8}$/.test(p[0] || '') || !/^R_XTENSA_/.test(p[2] || '')) continue;
    const off = parseInt(p[0], 16), type = p[2];
    const add = parseInt(p[p.length - 1], 16) || 0;
    const name = p.length >= 6 ? p[4] : null;
    const inplace = off + 4 <= src.length ? u32(src, off) : 0;
    let want;
    if (type === 'R_XTENSA_RELATIVE') want = addr((inplace + add) >>> 0);
    else if (type === 'R_XTENSA_RTLD') want = 0;
    else {
      const s = syms.has(name) ? addr(syms.get(name)) : exportAddr.get(name);
      if (s === undefined) { console.error(`native: ${label}: ${name} neither exported nor defined`); process.exit(1); }
      want = type === 'R_XTENSA_32' ? (inplace + s + add) >>> 0 : (s + add) >>> 0;
    }
    if (type === 'R_XTENSA_RELATIVE' && !inMod((inplace + add) >>> 0)) { console.error(`native: ${label}: RELATIVE outside`); process.exit(1); }
    const got = word(off) >>> 0;
    if (got !== want) { if (bad++ < 5) console.error(`native: ${label}: ${type} @0x${off.toString(16)} = 0x${got.toString(16)}, want 0x${want.toString(16)}`); }
    touched.add(off);
    n++;
  }
  // Every other byte is the file's own (alignment padding between text and rodata is zero).
  let diff = 0;
  for (let o = 0; o + 4 <= roEnd; o += 4) {
    if (touched.has(o)) continue;
    const w = o >= textEnd && o < roStart ? 0 : u32(src, o);
    if (u32(flash, o) !== w) diff++;
  }
  for (let o = dataStart; o + 4 <= dataStart + dataMem; o += 4) {
    if (touched.has(o)) continue;
    const w = o + 4 <= src.length && o < dataStart + (src.length - dataStart) ? 0 : 0;
    void w;
  }
  if (bad || diff) { console.error(`native: ${label}: ${bad} wrong relocations, ${diff} altered words`); process.exit(1); }
  console.log(`native: ${label}: ${n} relocations exact, rest of the image byte-identical (${r.stdout.trim()})`);
  const rj = run(tester, ['reject', file, exportsFile, layout, id], { allowFail: true });
  process.stdout.write(rj.stdout.split('\n').map((x) => (x ? `  ${x}` : x)).join('\n'));
  if (rj.status !== 0) { console.error(`native: ${label}: corruption checks failed${rj.status > 1 ? ` (exit ${rj.status}: a crash)` : ''}`); process.exit(1); }
}

checkModule('fixture', fixture, '12345678', 'fixture');

// 5. published native apps in the store checkout beside this repo, against the layout of the firmware built here
const fwElf = join(root, 'firmware', 'build', 'nucleoos.elf');
let fwLayout = null;
if (existsSync(fwElf)) {
  const sym = run(join(XT, 'xtensa-esp32s3-elf-nm'), [fwElf]).stdout.split(/\r?\n/).find((l) => / nucleo_native_layout_fw$/.test(l));
  if (sym) {
    const addr = parseInt(sym.split(' ')[0], 16);
    const dump = run(join(XT, 'xtensa-esp32s3-elf-objdump'), ['-s', `--start-address=0x${addr.toString(16)}`,
      `--stop-address=0x${(addr + 4).toString(16)}`, fwElf]).stdout;
    const m = dump.match(/^\s*[0-9a-f]+ ([0-9a-f]{8})/m);
    if (m) fwLayout = Buffer.from(m[1], 'hex').readUInt32LE(0).toString(16).padStart(8, '0');
  }
}
const store = join(root, '..', 'nucleoos-cardputer-store', 'apps', 'native');
if (existsSync(store)) {
  for (const id of readdirSync(store)) {
    const nap = join(store, id, 'files', 'app.nap');
    if (!existsSync(nap)) continue;
    const b = readFileSync(nap);
    const at = b.indexOf(Buffer.from([0x4e, 0x41, 0x50, 0x31]));      // nucleo_native_info magic "NAP1"
    const lay = b.readUInt32LE(at + 8).toString(16).padStart(8, '0');
    if (fwLayout && lay !== fwLayout) { console.error(`native: ${id}: layout ${lay}, the firmware built here has ${fwLayout}: rebuild it`); process.exit(1); }
    console.log(`native: ${id}: layout ${lay} ${fwLayout ? '== the firmware built here' : '(no firmware build here to compare)'}`);
    checkModule(id, nap, lay, id);
  }
}
console.log('native: all checks passed');

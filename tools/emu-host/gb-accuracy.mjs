// gb-accuracy — the Game Boy core against the community's reference test suites, as a RATCHET.
//
// gb-check proves the emulator renders and pages; this proves it is RIGHT. It runs the vendored core,
// built with the firmware's own switches (tools/emu-host/gb_accuracy.c), through:
//   * blargg      cpu_instrs, instr_timing (serial "Passed"), halt_bug (screen hash)
//   * mooneye     acceptance/ + emulator-only/mbc1,mbc2,mbc5 — every DMG-compatible test
//   * dmg-acid2   PPU, pixel-exact against the reference image (hash derived from reference-dmg.png)
//
// Peanut-GB is a line-based, instruction-granular core, so many cycle-exact tests are EXPECTED to
// fail and always will. The gate therefore does not demand 100%: it demands that nothing in
// gb-accuracy-expected.json regresses. A test that starts passing is reported, and `--update`
// ratchets it into the list so it can never silently break again.
//
// The ROMs are not committed (licences vary, the MBC tests are megabytes). They are fetched once,
// SHA-256-pinned, into tools/emu-host/testroms/ (gitignored). Offline with no cache = SKIP, not fail.
//
//   npm run gb:accuracy              run the gate
//   npm run gb:accuracy -- --update  accept newly passing tests into the ratchet
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { join, relative, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { cpus } from 'node:os';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..');
const CACHE = join(HERE, 'testroms');
const EXPECTED = join(HERE, 'gb-accuracy-expected.json');
const BUILD = join(ROOT, 'build');
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
const UPDATE = process.argv.includes('--update');

// Pinned sources. A changed file upstream is a changed test: refuse it rather than drift.
const BLARGG = 'https://raw.githubusercontent.com/retrio/gb-test-roms/c240dd7d700e5c0b00a7bbba52b53e4ee67b5f15';
const FETCH = [
  { file: 'blargg/cpu_instrs.gb',   url: `${BLARGG}/cpu_instrs/cpu_instrs.gb`,
    sha: '8c5e12f41e0ba5bbca796944f92ffe6de28809198682c4332e38d1b3cf56fcf2' },
  { file: 'blargg/instr_timing.gb', url: `${BLARGG}/instr_timing/instr_timing.gb`,
    sha: '646067b3d6c79fda810e9c3f1cb7c0efd5abb0a7ac06437c54e65720c15d9925' },
  { file: 'blargg/halt_bug.gb',     url: `${BLARGG}/halt_bug.gb`,
    sha: 'ddebfb81f992132954a5580e16cfb97346a4e061c762634d7701328e23fea7c6' },
  { file: 'dmg-acid2.gb',           url: 'https://github.com/mattcurrie/dmg-acid2/releases/download/v1.0/dmg-acid2.gb',
    sha: '464e14b7d42e7feea0b7ede42be7071dc88913f75b9ffa444299424b63d1dff1' },
  { file: 'mts.zip', unzip: 'mts',
    url: 'https://gekkio.fi/files/mooneye-test-suite/mts-20240926-1737-443f6e1/mts-20240926-1737-443f6e1.zip',
    sha: '5cf50314cd3d42ec8f423b0a71b9ab6dbfad2386d8d88f03687af1b5b5baba71' },
];

// Screen-judged tests: the FNV-1a of the final frame's shade indices when the test PASSES.
//   dmg-acid2  computed from mattcurrie/dmg-acid2 img/reference-dmg.png (NOT from our own output)
//   halt_bug   the frame that reads "Passed" (blargg writes no serial verdict for this one)
const FB_PASS = { 'dmg-acid2.gb': 'B312E6F6', 'blargg/halt_bug.gb': '1A4E5E56' };

const sha256 = (buf) => createHash('sha256').update(buf).digest('hex');

async function fetchAll() {
  mkdirSync(join(CACHE, 'blargg'), { recursive: true });
  for (const f of FETCH) {
    const dst = join(CACHE, f.file);
    const done = f.unzip ? existsSync(join(CACHE, f.unzip)) : existsSync(dst);
    if (done) continue;
    let buf;
    try {
      const r = await fetch(f.url, { redirect: 'follow' });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      buf = Buffer.from(await r.arrayBuffer());
    } catch (e) {
      return `cannot fetch ${f.file} (${e.message})`;
    }
    if (sha256(buf) !== f.sha) return `${f.file}: SHA-256 mismatch — upstream changed, refusing it`;
    writeFileSync(dst, buf);
    if (f.unzip) {
      const out = join(CACHE, f.unzip);
      mkdirSync(out, { recursive: true });
      // bsdtar (Windows 10+ ships it as System32\tar.exe) reads zip; GNU tar does not.
      const tar = process.platform === 'win32' ? 'C:/Windows/System32/tar.exe' : 'bsdtar';
      const x = spawnSync(tar, ['-xf', dst, '-C', out], { encoding: 'utf8' });
      if (x.status !== 0) return `cannot unpack ${f.file}: ${x.stderr || x.error}`;
    }
  }
  return null;
}

function walk(d, out = []) {
  for (const f of readdirSync(d)) {
    const p = join(d, f);
    if (statSync(p).isDirectory()) walk(p, out);
    else if (/\.gb$/i.test(f)) out.push(p);
  }
  return out;
}

function tests() {
  const list = [
    { name: 'blargg/cpu_instrs.gb', frames: 3600 },
    { name: 'blargg/instr_timing.gb', frames: 600 },
    { name: 'blargg/halt_bug.gb', frames: 300 },
    { name: 'dmg-acid2.gb', frames: 120 },
  ];
  const mts = join(CACHE, 'mts');
  if (existsSync(mts)) {
    for (const p of walk(mts)) {
      const r = relative(CACHE, p).replace(/\\/g, '/');
      if (!/\/(acceptance|emulator-only\/(mbc1|mbc2|mbc5))\//.test(r)) continue;
      // DMG-compatible only: tests suffixed for other models (and the dmg0 boot ROM) are not ours.
      const base = r.split('/').pop();
      if (/-(mgb|sgb|sgb2|cgb|agb|ags|S|A|C|dmg0)\.gb$/.test(base) && !/dmgABC/.test(base)) continue;
      list.push({ name: r.replace(/^mts\/[^/]+\//, 'mooneye/'), path: p, frames: 600 });
    }
  }
  return list;
}

function judge(t, out) {
  if (/init-error/.test(out)) return false;
  if (t.name.startsWith('mooneye/')) return /regs=0305080D/.test(out);
  if (FB_PASS[t.name]) return out.includes(`fb=${FB_PASS[t.name]}`);
  return /serial=.*Passed/.test(out);
}

function run(exe, t) {
  return new Promise((resolve) => {
    const p = spawn(exe, [t.path || join(CACHE, t.name), String(t.frames)], { env });
    let out = '';
    p.stdout.on('data', (d) => (out += d));
    const timer = setTimeout(() => p.kill(), 60000);
    p.on('close', () => { clearTimeout(timer); resolve(judge(t, out)); });
  });
}

const why = await fetchAll();
if (why) {
  console.log(`gb-accuracy: SKIPPED — ${why}`);
  console.log('             (the ratchet needs the pinned test ROMs once; run it with network access)');
  process.exit(0);
}

if (!existsSync(BUILD)) mkdirSync(BUILD, { recursive: true });
const exe = join(BUILD, 'gbaccuracy.exe');
const cc = spawnSync(GCC, ['-std=gnu11', '-O2', '-w', join(HERE, 'gb_accuracy.c'), '-o', exe], { env, encoding: 'utf8' });
if (cc.status !== 0) { console.error('gb-accuracy: compile FAILED\n' + (cc.stderr || cc.stdout)); process.exit(1); }

const all = tests();
const results = new Map();
const queue = [...all];
await Promise.all(Array.from({ length: Math.max(2, cpus().length - 1) }, async () => {
  while (queue.length) { const t = queue.shift(); results.set(t.name, await run(exe, t)); }
}));

const expected = existsSync(EXPECTED) ? JSON.parse(readFileSync(EXPECTED, 'utf8')) : [];
const pass = all.filter((t) => results.get(t.name)).map((t) => t.name).sort();
const regressed = expected.filter((n) => results.has(n) && !results.get(n));
const gained = pass.filter((n) => !expected.includes(n));

console.log(`gb-accuracy: ${pass.length}/${all.length} reference tests pass (ratchet holds ${expected.length})`);
for (const n of regressed) console.log(`  REGRESSED  ${n}`);
for (const n of gained) console.log(`  new pass   ${n}`);

if (UPDATE && (gained.length || regressed.length === 0)) {
  writeFileSync(EXPECTED, JSON.stringify([...new Set([...expected, ...pass])].sort(), null, 2) + '\n');
  if (gained.length) console.log(`gb-accuracy: ratchet updated (+${gained.length})`);
}
if (regressed.length) { console.log('gb-accuracy: FAILED — a test that used to pass no longer does'); process.exit(1); }
if (gained.length && !UPDATE) console.log('gb-accuracy: green — run with --update to lock the new passes in');
else console.log('gb-accuracy: green');

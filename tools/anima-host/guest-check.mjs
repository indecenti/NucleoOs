// Host gate for M5Launcher guest mode: encode firmware/partitions.csv into a binary ESP-IDF partition
// table (same 32-byte rows gen_esp32part.py writes; no IDF needed, so it runs in CI), then compile the
// REAL decision core (firmware/components/nucleo_guest/guest_policy.c) with guest-ctest.c and run it
// against that table. Mirrors update-check.mjs. Wired as `npm run guest:test`. See docs/m5launcher.md.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
mkdirSync(BUILD, { recursive: true });

// ── partitions.csv -> binary table ──────────────────────────────────────────────────────────────
const TYPES = { app: 0x00, data: 0x01 };
const SUBTYPES = {
  app: { factory: 0x00, test: 0x20, ...Object.fromEntries([...Array(16)].map((_, i) => [`ota_${i}`, 0x10 + i])) },
  data: { ota: 0x00, phy: 0x01, nvs: 0x02, coredump: 0x03, nvs_keys: 0x04, efuse: 0x05, undefined: 0x06,
          fat: 0x81, spiffs: 0x82, littlefs: 0x83 },
};
const num = (s) => {
  const t = s.trim();
  const m = /^(0x[0-9a-f]+|\d+)([km])?$/i.exec(t);
  if (!m) throw new Error(`bad number: ${s}`);
  const v = Number(m[1]);
  return m[2] ? v * (m[2].toLowerCase() === 'k' ? 1024 : 1024 * 1024) : v;
};

function encodeCsv(csv) {
  const rows = [];
  for (const raw of csv.split(/\r?\n/)) {
    const line = raw.replace(/#.*$/, '').trim();
    if (!line) continue;
    const [name, type, sub, off, size, flags = ''] = line.split(',').map((c) => c.trim());
    const t = TYPES[type];
    const st = SUBTYPES[type]?.[sub];
    if (t === undefined || st === undefined) throw new Error(`unknown type/subtype: ${type}/${sub}`);
    const e = Buffer.alloc(32, 0);
    e[0] = 0xaa; e[1] = 0x50; e[2] = t; e[3] = st;
    e.writeUInt32LE(num(off), 4);
    e.writeUInt32LE(num(size), 8);
    e.write(name.slice(0, 16), 12, 'latin1');
    e.writeUInt32LE(flags.includes('encrypted') ? 1 : 0, 28);
    rows.push(e);
  }
  const md5 = Buffer.alloc(32, 0xff); md5[0] = 0xeb; md5[1] = 0xeb;   // terminator row (digest not checked here)
  return Buffer.concat([...rows, md5]);
}

const tablePath = join(BUILD, 'guest-partitions.bin');
try {
  writeFileSync(tablePath, encodeCsv(readFileSync(join(ROOT, 'firmware/partitions.csv'), 'utf8')));
} catch (err) {
  console.error(`guest-policy: partitions.csv encode FAILED: ${err.message}`);
  process.exit(1);
}

// ── compile + run the real C ────────────────────────────────────────────────────────────────────
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

const exe = join(BUILD, process.platform === 'win32' ? 'guestctest.exe' : 'guestctest');
const cc = spawnSync(GCC, [
  '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
  '-I', 'firmware/components/nucleo_guest/include',
  'tools/anima-host/guest-ctest.c',
  'firmware/components/nucleo_guest/guest_policy.c',
  '-o', exe,
], { cwd: ROOT, env, encoding: 'utf8' });

if (cc.status !== 0) {
  console.error('guest-policy: COMPILE FAILED');
  if (cc.stdout) process.stdout.write(cc.stdout);
  if (cc.stderr) process.stderr.write(cc.stderr);
  process.exit(1);
}
if (cc.stderr) process.stderr.write(cc.stderr);

const run = spawnSync(exe, [tablePath], { cwd: ROOT, env, encoding: 'utf8' });
process.stdout.write(run.stdout || '');
if (run.stderr) process.stderr.write(run.stderr);
process.exit(run.status === null ? 1 : run.status);

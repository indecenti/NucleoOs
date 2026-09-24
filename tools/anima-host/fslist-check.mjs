#!/usr/bin/env node
// FSLIST-CHECK — the streaming GET /api/fs/list body (firmware/components/nucleo_fsapi/fslist.c),
// host-compiled with MinGW: the SAME C the device runs. fslist-ctest.c builds a fixture SD and
// self-checks chunk boundaries, sink failure and cJSON-identical escaping; this driver then parses
// every listing and checks the contract the clients rely on: {"entries":[{name,type,size,
// protected?,has_subdirs?}]}, the protected-tree and ".factory" lock flags, and a 300-entry folder
// (the size that answered 503 "oom" with the old cJSON build) listed in full. Built twice — with the
// factory hash set and with its zero-heap per-entry fallback (FSLIST_FACTORY_MAX=0) — and the two
// must produce byte-identical listings.
//
// The fs-policy headers include "nucleo_board.h" by quote, which would find the REAL header next
// to them (SD at "/sd"); they are copied beside the host shim (SD at "./sd") so the policy is
// evaluated on the fixture tree.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, copyFileSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build');
const INC = join(BUILD, 'fslist-inc');
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

rmSync(INC, { recursive: true, force: true });
mkdirSync(INC, { recursive: true });
for (const h of ['nucleo_fsprotect.h', 'nucleo_fsfactory.h']) copyFileSync(join(ROOT, 'firmware/components/nucleo_board/include', h), join(INC, h));
copyFileSync(join(ROOT, 'tools/anima-host/shim/nucleo_board.h'), join(INC, 'nucleo_board.h'));

let fails = 0, passes = 0;
const check = (cond, msg) => { if (cond) passes++; else { fails++; console.log(`  ✗ ${msg}`); } };

function build(tag, defs) {
  const exe = join(BUILD, `fslistctest-${tag}.exe`);
  const cc = spawnSync(GCC, ['-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation', ...defs,   // as the component's CMakeLists
    '-I', INC, '-I', 'firmware/components/nucleo_fsapi', 'tools/anima-host/fslist-ctest.c', '-o', exe],
    { cwd: ROOT, env, encoding: 'utf8' });
  if (cc.status !== 0) { console.error(`fslist: COMPILE FAILED (${tag})\n${cc.stdout || ''}${cc.stderr || ''}`); process.exit(1); }
  const dir = join(BUILD, `fslist-test-${tag}`);
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const run = spawnSync(exe, [], { cwd: dir, env, encoding: 'utf8' });
  const m = (run.stdout || '').match(/fslist: (\d+) passed, (\d+) failed/);
  if (!m || run.status !== 0) { console.log((run.stdout || '') + (run.stderr || '')); fails += m ? Number(m[2]) : 1; }
  else passes += Number(m[1]);
  const out = {};
  for (const n of ['roms', 'apps', 'music']) out[n] = readFileSync(join(dir, 'out', `${n}.json`), 'utf8');
  rmSync(dir, { recursive: true, force: true });
  return out;
}

function entries(raw, where) {
  let j;
  try { j = JSON.parse(raw); } catch (e) { check(false, `${where}: invalid JSON (${e.message})`); return new Map(); }
  check(Array.isArray(j.entries) && Object.keys(j).length === 1, `${where}: shape {"entries":[...]}`);
  const m = new Map();
  for (const e of j.entries || []) {
    if (e.name === '.' || e.name === '..') continue;               // MinGW's readdir lists them; FATFS doesn't
    const keys = Object.keys(e).join(',');
    const okKeys = /^name,type,size(,protected)?(,has_subdirs)?$/.test(keys);
    check(okKeys && (e.type === 'dir' || e.type === 'file') && Number.isInteger(e.size) && e.size >= 0 &&
          (e.protected === undefined || e.protected === true) && ((e.type === 'dir') === (typeof e.has_subdirs === 'boolean')),
          `${where}: entry ${JSON.stringify(e)} fields`);
    m.set(e.name, e);
  }
  return m;
}

const hashed = build('hash', []);
const scan = build('scan', ['-DFSLIST_FACTORY_MAX=0']);
for (const n of Object.keys(hashed)) check(hashed[n] === scan[n], `${n}: hash-set and per-entry-scan listings identical`);

const roms = entries(hashed.roms, 'roms');
const want = {
  '.factory': { type: 'file', protected: true },
  'Tetris.gb': { type: 'file', size: 10, protected: true },
  'zelda.gb': { type: 'file', size: 20, protected: true },            // listed as "  Zelda.GB \r\n"
  'User.gb': { type: 'file', size: 5, protected: undefined },          // a user import stays free
  'Sub': { type: 'dir', protected: true, has_subdirs: true },
  'Empty': { type: 'dir', protected: undefined, has_subdirs: false },
};
check(roms.size === Object.keys(want).length, `roms: ${roms.size} entries, want ${Object.keys(want).length}`);
for (const [name, w] of Object.entries(want)) {
  const e = roms.get(name);
  check(!!e && Object.entries(w).every(([k, v]) => e[k] === v), `roms: ${name} -> ${JSON.stringify(e)} want ${JSON.stringify(w)}`);
}

const apps = entries(hashed.apps, 'apps');
check(apps.size === 300, `apps: 300-entry folder listed in full (${apps.size})`);
check([...apps.values()].every((e) => e.protected === true && e.type === 'file' && e.size === Number(e.name.slice(1, 4))),
      'apps: every entry protected with its exact size');

const music = entries(hashed.music, 'music');
check(music.has('canzone è bella.mp3') && music.get('canzone è bella.mp3').size === 7, 'music: UTF-8 name round-trips');
check(music.has("a&b 'x' [1].txt") && !music.get("a&b 'x' [1].txt").protected, 'music: punctuation name, user file unprotected');

console.log(`fslist: ${passes} passed, ${fails} failed`);
process.exit(fails ? 1 : 0);

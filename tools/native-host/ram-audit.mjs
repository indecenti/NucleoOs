#!/usr/bin/env node
// Native games: the device-side RAM / stack / flash audit (npm run games:ram).
//
// Recompiles each game's CURRENT sources with the real xtensa toolchain and the exact flags of the last
// firmware configure (firmware/build/compile_commands.json, so run a firmware build once first), into a
// temp folder — the firmware build dir is never touched — with -fstack-usage, and reports per game:
//   static RAM  .bss + .data: resident ALL THE TIME, even with the game closed (the "zero RAM until used"
//               rule wants this tiny: working memory belongs in the game's APP_RAM table)
//   flash       .text + .rodata (+ literals): what the game costs in the image / M5Launcher slot
//   stack       the biggest function frames: the games run on the MAIN task (8 KB, shared with the launcher's
//               frames below), so one big frame or a deep chain of them overflows it
// The runtime heap (peak, largest block, leaks) is measured by run.mjs in the harness.
// Fails when a game's static RAM is over STATIC_MAX or a single frame is over FRAME_MAX.
//   node tools/native-host/ram-audit.mjs [id,...|all]
import { execFileSync } from 'node:child_process';
import { readFileSync, mkdirSync, readdirSync, rmSync, existsSync } from 'node:fs';
import { join, dirname, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { GAMES } from './games.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const BUILD = join(root, 'firmware', 'build');
const APPS = join(root, 'firmware', 'components', 'nucleo_app');
const STATIC_MAX = 2048;          // B of .bss+.data a game may keep resident (APP_RAM is the place for more)
const FRAME_MAX = 1536;           // B: one function frame on the 8 KB main task

const ccPath = join(BUILD, 'compile_commands.json');
if (!existsSync(ccPath)) { console.error('ram-audit: no firmware/build/compile_commands.json — build the firmware once'); process.exit(2); }
const cc = JSON.parse(readFileSync(ccPath, 'utf8'));
const tool = (cmd) => cmd.match(/^(\S+?xtensa-esp32s3-elf-)(g\+\+|gcc)(\.exe)?/);
const which = process.argv[2] && process.argv[2] !== 'all' ? process.argv[2].split(',') : Object.keys(GAMES);
const tmp = join(tmpdir(), `ram-audit-${process.pid}`);
mkdirSync(tmp, { recursive: true });

// Split a compile command into argv (quotes as the CMake Ninja generator writes them on Windows).
function argv(cmd) {
  const out = []; let cur = '', q = false;
  for (let i = 0; i < cmd.length; i++) {
    const c = cmd[i];
    if (c === '\\' && cmd[i + 1] === '"') { cur += '"'; i++; continue; }
    if (c === '"') { q = !q; continue; }
    if (c === ' ' && !q) { if (cur) out.push(cur); cur = ''; continue; }
    cur += c;
  }
  if (cur) out.push(cur);
  return out;
}

let bad = 0;
const rows = [];
for (const id of which) {
  const g = GAMES[id];
  if (!g) { console.error(`ram-audit: unknown game ${id}`); process.exit(2); }
  const srcs = [g.src, ...(g.extra || [])];
  let bss = 0, data = 0, text = 0, rodata = 0, broken = false;
  const frames = [];
  for (const s of srcs) {
    const e = cc.find((x) => x.file.replace(/\\/g, '/').endsWith(`/nucleo_app/${s}`));
    if (!e) { console.error(`ram-audit: ${s} not in compile_commands.json (re-run the firmware configure)`); process.exit(2); }
    const a = argv(e.command);
    const oi = a.indexOf('-o');
    const obj = join(tmp, `${id}_${s}.obj`);
    a[oi + 1] = obj;
    a.splice(1, 0, '-fstack-usage');
    try { execFileSync(a[0], a.slice(1), { cwd: e.directory, stdio: 'pipe' }); }
    catch (err) { console.error(`ram-audit: ${s} does not compile for the device:\n${String(err.stderr).split('\n').filter((l) => /error/.test(l)).slice(0, 8).join('\n')}`); broken = true; continue; }
    const m = tool(a[0]);
    const size = execFileSync(`${m[1]}size${m[3] || ''}`, ['-A', obj], { encoding: 'utf8' });
    for (const line of size.split('\n')) {
      const [sec, sz] = line.trim().split(/\s+/); const n = +sz;
      if (!sec || !Number.isFinite(n)) continue;
      if (/^\.(s?bss)/.test(sec)) bss += n;
      else if (/^\.(s?data)/.test(sec)) data += n;
      else if (/^\.(text|literal)/.test(sec)) text += n;
      else if (/^\.rodata/.test(sec)) rodata += n;
    }
    const su = obj.replace(/\.obj$/, '.su');
    if (existsSync(su)) for (const l of readFileSync(su, 'utf8').split('\n')) {
      const p = l.split('\t'); if (p.length < 3) continue;
      frames.push({ fn: p[0].split(':').pop(), file: s, bytes: +p[1], kind: p[2] });
    }
  }
  frames.sort((x, y) => y.bytes - x.bytes);
  const top = frames.slice(0, 3).map((f) => `${f.fn} ${f.bytes}${f.kind.includes('dynamic') ? '+dyn' : ''}`).join(', ');
  const stat = bss + data, big = frames.filter((f) => f.bytes > FRAME_MAX);
  if (broken) { bad++; rows.push(`FAIL ${id.padEnd(9)} does not compile for the device (see above)`); continue; }
  const fail = stat > STATIC_MAX || big.length;
  if (fail) bad++;
  rows.push(`${fail ? 'FAIL' : 'ok  '} ${id.padEnd(9)} static ${String(stat).padStart(6)} B (bss ${bss}, data ${data})  flash ${String(text + rodata).padStart(7)} B  stack top: ${top}`);
  if (stat > STATIC_MAX) rows.push(`       ${id}: ${stat} B resident even when closed (max ${STATIC_MAX}) — move working memory to APP_RAM`);
  for (const f of big) rows.push(`       ${id}: ${f.fn} (${f.file}) needs a ${f.bytes} B frame on the 8 KB main task (max ${FRAME_MAX})`);
}
rmSync(tmp, { recursive: true, force: true });
console.log(rows.join('\n'));
console.log(`ram-audit: ${which.length - bad}/${which.length} games within static ${STATIC_MAX} B and frame ${FRAME_MAX} B`);
process.exit(bad ? 1 : 0);

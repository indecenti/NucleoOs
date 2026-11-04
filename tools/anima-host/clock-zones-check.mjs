#!/usr/bin/env node
// CLOCK-ZONES-CHECK — the device time-zone table (firmware/components/nucleo_setup/clock_zones.c), host-compiled
// with MinGW: the SAME C the device runs. Every zone's POSIX rule (what newlib's tzset() applies) is evaluated
// here by a small POSIX-TZ interpreter and compared with the IANA database bundled with Node (Intl) over
// 2026-2027 — a wrong offset, a missing or extra DST, or a DST switch at the wrong moment fails. Also: unique
// ids, ASCII labels that fit the Settings row, every alias resolves to a zone with the alias's own offsets,
// unknown ids are refused, the default is Europe/Rome, zones run west to east.
// Wired as `npm run clockzones:test`.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const SRC = join(ROOT, 'firmware/components/nucleo_setup');
const BUILD = join(ROOT, 'build', 'clock-zones');
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
const EXE = join(BUILD, 'clock-zones' + (process.platform === 'win32' ? '.exe' : ''));

mkdirSync(BUILD, { recursive: true });
const cc = spawnSync(GCC, ['-std=c11', '-Wall', '-Wextra', '-Werror', '-I', SRC, join(SRC, 'clock_zones.c'),
  join(ROOT, 'tools/anima-host/clock-zones-ctest.c'), '-o', EXE], { env, encoding: 'utf8' });
if (cc.status !== 0) { console.error(cc.stderr || cc.error); process.exit(1); }

const aliases = [...readFileSync(join(SRC, 'clock_zones.c'), 'utf8').matchAll(/\{\s*"([^"]+)",\s*"([^"]+)"\s*\}/g)]
  .map((m) => ({ alias: m[1], id: m[2] }));
const UNKNOWN = ['Mars/Olympus', 'europe/madrid', 'Europe/Madrid '];
const run = spawnSync(EXE, [...aliases.map((a) => a.alias), ...UNKNOWN], { env, encoding: 'utf8' });
if (run.status !== 0) { console.error(run.stdout, run.stderr); process.exit(1); }
const lines = run.stdout.trim().split(/\r?\n/).map((l) => l.split('\t'));
const zones = lines.filter((l) => l[0] === 'zone').map(([, id, label, posix]) => ({ id, label, posix }));
const found = Object.fromEntries(lines.filter((l) => l[0] === 'find').map(([, id, i]) => [id, +i]));
const def = +lines.find((l) => l[0] === 'default')[1];

let fail = 0, pass = 0;
const check = (cond, msg) => { if (cond) pass++; else { fail++; console.log(`  ✗ ${msg}`); } };

// ---- POSIX TZ interpreter (the subset used here: <name>/alpha names, [+-]hh[:mm], Mm.w.d[/time]) ----------
function parsePosix(s) {
  let i = 0;
  const name = () => {
    if (s[i] === '<') { const j = s.indexOf('>', i); const n = s.slice(i + 1, j); i = j + 1; return n; }
    const m = /^[A-Za-z]{3,}/.exec(s.slice(i)); if (!m) throw new Error(`bad name at ${i}`); i += m[0].length; return m[0];
  };
  const time = () => {   // [+-]hh[:mm[:ss]] -> seconds
    const m = /^([+-]?)(\d{1,3})(?::(\d{2}))?(?::(\d{2}))?/.exec(s.slice(i)); if (!m) throw new Error(`bad time at ${i}`);
    i += m[0].length; const v = (+m[2]) * 3600 + (+(m[3] || 0)) * 60 + (+(m[4] || 0)); return m[1] === '-' ? -v : v;
  };
  const rule = () => {
    const m = /^M(\d{1,2})\.(\d)\.(\d)/.exec(s.slice(i)); if (!m) throw new Error(`bad rule at ${i}`); i += m[0].length;
    let at = 7200; if (s[i] === '/') { i++; at = time(); }
    return { mon: +m[1], week: +m[2], dow: +m[3], at };
  };
  const std = name(); const stdOff = -time();          // POSIX offsets are positive WEST of Greenwich
  if (i === s.length) return { std, stdOff };
  const dst = name(); let dstOff = stdOff + 3600;
  if (s[i] !== ',') dstOff = -time();
  if (s[i++] !== ',') throw new Error('missing DST rules');
  const start = rule(); if (s[i++] !== ',') throw new Error('missing DST end'); const end = rule();
  if (i !== s.length) throw new Error(`trailing "${s.slice(i)}"`);
  return { std, stdOff, dst, dstOff, start, end };
}
// UTC epoch (s) of a rule's transition in `year`; the rule's wall time is local time at `offBefore`.
function transition(r, year, offBefore) {
  const first = new Date(Date.UTC(year, r.mon - 1, 1)).getUTCDay();
  let day = 1 + ((r.dow - first + 7) % 7) + (r.week - 1) * 7;
  const dim = new Date(Date.UTC(year, r.mon, 0)).getUTCDate();
  if (r.week === 5) while (day > dim) day -= 7;
  return Date.UTC(year, r.mon - 1, day) / 1000 + r.at - offBefore;
}
function posixOffset(z, t) {   // seconds east of UTC at epoch t (s)
  if (!z.dst) return z.stdOff;
  const y = new Date(t * 1000).getUTCFullYear();
  const on = transition(z.start, y, z.stdOff), off = transition(z.end, y, z.dstOff);
  const inDst = on < off ? (t >= on && t < off) : (t >= on || t < off);   // southern hemisphere wraps the year
  return inDst ? z.dstOff : z.stdOff;
}
function ianaOffset(id, t) {
  const p = Object.fromEntries(new Intl.DateTimeFormat('en-US', { timeZone: id, hourCycle: 'h23', year: 'numeric', month: 'numeric',
    day: 'numeric', hour: 'numeric', minute: 'numeric', second: 'numeric' }).formatToParts(new Date(t * 1000)).map((x) => [x.type, x.value]));
  return Date.UTC(+p.year, +p.month - 1, +p.day, +p.hour, +p.minute, +p.second) / 1000 - t;
}
const T0 = Date.UTC(2026, 0, 1) / 1000, T1 = Date.UTC(2028, 0, 1) / 1000, STEP = 3 * 3600 + 17 * 60;   // ~5.7k samples per zone
// Morocco suspends its +01 during Ramadan (dates move every year): the device keeps +01 all year (see clock_zones.c).
const SEASONAL = new Set(['Africa/Casablanca']);

function compare(name, z, iana) {
  let bad = null, n = 0;
  for (let t = T0; t < T1; t += STEP) {
    const a = posixOffset(z, t), b = ianaOffset(iana, t);
    if (a !== b && !bad) bad = `${new Date(t * 1000).toISOString()}: rule ${a / 60} min vs IANA ${b / 60} min`;
    if (a !== b) n++;
  }
  check(!bad, `${name}: ${bad} (${n} samples differ)`);
}

// ---- checks -----------------------------------------------------------------------------------------------
check(zones.length >= 40, `at least 40 zones (got ${zones.length})`);
check(zones[def] && zones[def].id === 'Europe/Rome', 'default zone is Europe/Rome (what every device ran before)');
const ids = zones.map((z) => z.id);
check(new Set(ids).size === ids.length, `unique ids (${ids.filter((x, i) => ids.indexOf(x) !== i).join(', ') || '-'})`);
const parsed = [];
for (const z of zones) {
  check(/^[\x20-\x7e]{1,20}$/.test(z.label), `${z.id}: label "${z.label}" is printable ASCII, <= 20 chars (device font, Settings row)`);
  check(z.label.split(',')[0].length <= 12, `${z.id}: first city "${z.label.split(',')[0]}" <= 12 chars (the Settings row value)`);
  let p = null;
  try { p = parsePosix(z.posix); } catch (e) { check(false, `${z.id}: POSIX "${z.posix}" — ${e.message}`); }
  parsed.push(p);
  if (p && !SEASONAL.has(z.id)) compare(z.id, p, z.id);
}
for (const a of aliases) {
  const i = found[a.alias];
  check(i >= 0 && zones[i] && zones[i].id === a.id, `alias ${a.alias} -> ${a.id} (got ${i >= 0 ? zones[i]?.id : 'unknown'})`);
  check(!ids.includes(a.alias), `alias ${a.alias} is not also a zone id`);
  if (i >= 0 && parsed[i] && !SEASONAL.has(a.id)) compare(`alias ${a.alias}`, parsed[i], a.alias);
}
for (const bad of UNKNOWN) check(found[bad] === -1, `unknown id "${bad}" is refused`);
check(parsed.every((p, i) => i === 0 || !p || !parsed[i - 1] || parsed[i - 1].stdOff <= p.stdOff),
  'zones are ordered west to east (standard offset) so cycling walks the globe');

console.log(`clock-zones: ${zones.length} zones, ${aliases.length} aliases — ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);

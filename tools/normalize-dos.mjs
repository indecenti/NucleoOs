// normalize-dos — rebuild every .jsdos in the repo SD library to a clean, dosbox-backend-friendly
// bundle: the SAME game files, a store-method ZIP (forward-slash names), and a compact dosbox.conf
// whose [autoexec] reliably mounts C: and launches the right program.
//
// It is non-destructive of TUNING: each bundle's existing machine / memsize / cpu / sound settings
// are parsed and preserved (sensible defaults fill any gaps). Only the structure is normalized and
// cosmetic autoexec noise (e.g. a leftover `type README.TXT`) is dropped. Run command is detected
// from the current autoexec, else from the lone root executable — with the historical `cd`-prefix
// bug fixed (so `CD-MAN.EXE` is no longer mistaken for a `cd` command).
//
// Usage: node tools/normalize-dos.mjs [--dir deploy/sd/data/DOS] [--dry] [--also tools/sd-sim/data/DOS]
import zlib from 'node:zlib';
import { readdirSync, readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { packStoreToFile } from './make-jsdos.mjs';

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const DIR = opt('--dir', 'deploy/sd/data/DOS');
const DRY = args.includes('--dry');
// Mirror destinations the rebuilt bundle is also copied to (default: the simulator's SD + sd-safe).
const MIRRORS = [opt('--also', 'tools/sd-sim/data/DOS')].filter(Boolean).filter((d) => existsSync(d));
const RUNNABLE = /\.(exe|com|bat)$/i;

function unzip(u8) {
  const dv = (o) => u8[o] | (u8[o + 1] << 8);
  const dw = (o) => (u8[o] | (u8[o + 1] << 8) | (u8[o + 2] << 16) | (u8[o + 3] << 24)) >>> 0;
  let p = u8.length - 22;
  while (p >= 0 && dw(p) !== 0x06054b50) p--;
  if (p < 0) throw new Error('no EOCD (not a zip)');
  const count = dv(p + 10); let cd = dw(p + 16);
  const out = [];
  for (let i = 0; i < count; i++) {
    if (dw(cd) !== 0x02014b50) break;
    const method = dv(cd + 10), csize = dw(cd + 20), nlen = dv(cd + 28), elen = dv(cd + 30), clen = dv(cd + 32), lho = dw(cd + 42);
    const name = Buffer.from(u8.subarray(cd + 46, cd + 46 + nlen)).toString('latin1');
    const lnlen = dv(lho + 26), lelen = dv(lho + 28);
    const start = lho + 30 + lnlen + lelen;
    const comp = Buffer.from(u8.subarray(start, start + csize));
    if (!name.endsWith('/')) {
      const data = method === 0 ? comp : method === 8 ? zlib.inflateRawSync(comp) : null;
      if (!data) throw new Error('unsupported method ' + method + ' in ' + name);
      out.push({ name, data });
    }
    cd += 46 + nlen + elen + clen;
  }
  return out;
}

const confOf = (entries) => { const c = entries.find((e) => e.name.toLowerCase() === '.jsdos/dosbox.conf'); return c ? c.data.toString('latin1') : ''; };

// Pull a single `key=value` from a conf (first match wins), trimmed.
function val(conf, key) {
  const m = conf.match(new RegExp('^\\s*' + key + '\\s*=\\s*(.*)$', 'im'));
  return m ? m[1].trim() : null;
}

// Decide what to launch. Prefer the last REAL command in the current [autoexec]; ignore
// echo/mount/cd/drive-letter/exit/type/pause/comments. Else the lone root executable.
function detectRun(entries) {
  const conf = confOf(entries);
  const tail = conf.replace(/[\s\S]*?\[autoexec\]/i, '');
  const cmds = tail.split(/\r?\n/).map((s) => s.trim()).filter((l) => {
    if (!l || /^[#;]/.test(l)) return false;
    if (/^@?echo\b/i.test(l) || /^exit\b/i.test(l) || /^mount\b/i.test(l)) return false;
    if (/^cd\s/i.test(l) || /^cd$/i.test(l)) return false;     // a real `cd` command (note the space) — NOT "CD-MAN.EXE"
    if (/^[a-z]:\\?$/i.test(l)) return false;                  // drive letter "c:"
    if (/^(type|pause|rem|cls|set|ver|menu|choice|goto|if|for|call\s+(type|echo))\b/i.test(l)) return false;
    if (/^imgmount\b|^boot\b/i.test(l)) return false;
    return true;
  });
  let full = null;
  if (cmds.length) full = cmds[cmds.length - 1].replace(/^call\s+/i, '').split(/\s+/)[0];
  if (!full) {
    full = entries.map((e) => e.name).filter((n) => RUNNABLE.test(n))
      .sort((a, b) => a.split('/').length - b.split('/').length)[0];
  }
  if (!full) throw new Error('no executable found');
  full = full.replace(/\\/g, '/');
  const slash = full.lastIndexOf('/');
  const dir = slash >= 0 ? full.slice(0, slash) : '';
  const exe = slash >= 0 ? full.slice(slash + 1) : full;
  // Confirm the target exists in the bundle (case-insensitive); otherwise fall back to lone exe.
  const lc = full.toLowerCase();
  if (!entries.some((e) => e.name.toLowerCase().replace(/\\/g, '/') === lc)) {
    const lone = entries.map((e) => e.name).filter((n) => RUNNABLE.test(n))[0];
    if (lone) return detectRunFromName(lone);
  }
  return { dir, exe, bat: /\.bat$/i.test(exe) };
}
function detectRunFromName(name) {
  const n = name.replace(/\\/g, '/'); const s = n.lastIndexOf('/');
  return { dir: s >= 0 ? n.slice(0, s) : '', exe: s >= 0 ? n.slice(s + 1) : n, bat: /\.bat$/i.test(n) };
}

function genConf(old, run) {
  const machine = val(old, 'machine') || 'svga_s3';
  const memsize = val(old, 'memsize') || '16';
  const core = val(old, 'core') || 'auto';
  const cputype = val(old, 'cputype') || 'auto';
  const cycles = val(old, 'cycles') || 'auto';
  const nosound = /nosound\s*=\s*true/i.test(old);
  const sbtype = val(old, 'sbtype') || 'sb16';
  const ax = ['@echo off', 'mount c .', 'c:'];
  if (run.dir) ax.push('cd ' + run.dir.replace(/\//g, '\\'));
  ax.push((run.bat ? 'call ' : '') + run.exe);
  const L = [
    '[sdl]', 'autolock=false', '',
    '[dosbox]', 'machine=' + machine, 'memsize=' + memsize, '',
    '[cpu]', 'core=' + core, 'cputype=' + cputype, 'cycles=' + cycles, '',
  ];
  if (!nosound) L.push('[mixer]', 'nosound=false', 'rate=44100', '',
    '[sblaster]', 'sbtype=' + sbtype, 'irq=7', 'dma=1', 'hdma=5', 'oplmode=auto', 'oplrate=44100', '');
  else L.push('[mixer]', 'nosound=true', '');
  L.push('[dos]', 'xms=true', 'ems=true', 'umb=true', '');
  L.push('[autoexec]', ...ax, '');
  return Buffer.from(L.join('\n'), 'latin1');
}

const files = readdirSync(DIR).filter((n) => n.toLowerCase().endsWith('.jsdos')).sort();
let changed = 0;
for (const name of files) {
  const path = join(DIR, name);
  try {
    const entries = unzip(readFileSync(path));
    const run = detectRun(entries);
    const keep = entries.filter((e) => !e.name.toLowerCase().startsWith('.jsdos/'))
      .map((e) => ({ name: e.name, data: e.data }));
    keep.push({ name: '.jsdos/dosbox.conf', data: genConf(confOf(entries), run) });
    const runStr = (run.dir ? run.dir + '/' : '') + (run.bat ? 'call ' : '') + run.exe;
    if (DRY) { console.log(`${name}: would run ${runStr} (${keep.length} entries)`); continue; }
    const res = packStoreToFile(path, keep);
    for (const dst of MIRRORS) { mkdirSync(dst, { recursive: true }); packStoreToFile(join(dst, name), keep); }
    console.log(`✓ ${name}: ${res.files} files, ${res.bytes} bytes, runs ${runStr}`);
    changed++;
  } catch (e) { console.error(`✗ ${name}: ${e.message}`); }
}
console.log(`\n${files.length} bundles, ${changed} rebuilt${DRY ? ' (dry-run)' : ''}, mirrored to: ${MIRRORS.join(', ') || '(none)'}`);

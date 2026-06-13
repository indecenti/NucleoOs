// add-dos — turn a DOS program into a js-dos-ready bundle and (optionally) drop it on the device SD.
//
// This is the missing end-to-end tool: it builds a CORRECT .jsdos bundle (the previous
// make-jsdos only zipped a folder and relied on a hand-written conf — the cause of the
// "game opens then closes instantly" bug, where the autoexec never mounted C:). It:
//   1. collects the program's files (a folder, or a single .EXE/.COM/.BAT),
//   2. auto-detects the executable to run (override with --run),
//   3. generates .jsdos/dosbox.conf with `mount c .` + `c:` + the run command,
//   4. packs a valid js-dos v8 ZIP bundle (store method, forward-slash names),
//   5. stages it into deploy/sd/data/DOS (the repo SD image), and
//   6. with --host/--pin, copies it onto the running device over HTTP (/api/fs), creating
//      /data/DOS if needed — no SD removal.
//
// Usage:
//   node tools/add-dos.mjs <source> [--run CMD] [--name NAME] [--out FILE]
//                          [--host http://IP --pin 123456]
//                          [--machine svga_s3] [--cycles auto] [--no-sound] [--no-stage]
//   <source>   a folder of DOS files, or a single .exe/.com/.bat
// Examples:
//   node tools/add-dos.mjs ./games/keen           --host http://192.168.0.166 --pin 095323
//   node tools/add-dos.mjs ./PRINCE.EXE --name prince --cycles "fixed 20000"
import { readdirSync, statSync, readFileSync, mkdirSync } from 'node:fs';
import { join, relative, sep, basename, extname, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { packStoreToFile } from './make-jsdos.mjs';

const REPO = join(fileURLToPath(import.meta.url), '..', '..');

function parseArgs(argv) {
  const a = { src: null, run: null, name: null, out: null, host: null, pin: null,
              machine: 'svga_s3', cycles: 'auto', memsize: '16', sound: true, stage: true };
  for (let i = 0; i < argv.length; i++) {
    const v = argv[i];
    if (v === '--run') a.run = argv[++i];
    else if (v === '--name') a.name = argv[++i];
    else if (v === '--out') a.out = argv[++i];
    else if (v === '--host') a.host = argv[++i];
    else if (v === '--pin') a.pin = String(argv[++i] || '').trim();
    else if (v === '--machine') a.machine = argv[++i];
    else if (v === '--cycles') a.cycles = argv[++i];
    else if (v === '--memsize') a.memsize = String(argv[++i] || '16');
    else if (v === '--no-sound') a.sound = false;
    else if (v === '--no-stage') a.stage = false;
    else if (!v.startsWith('--') && !a.src) a.src = v;
  }
  return a;
}

// Gather the program files into [{ name, data }] with forward-slash, root-relative names.
// Skips any pre-existing .jsdos/ control dir so we always regenerate a clean conf.
function collect(src) {
  const st = statSync(src);
  if (st.isFile()) return [{ name: basename(src), data: readFileSync(src) }];
  const out = [];
  const walk = (dir) => {
    for (const n of readdirSync(dir)) {
      const p = join(dir, n);
      if (statSync(p).isDirectory()) walk(p);
      else {
        const rel = relative(src, p).split(sep).join('/');
        if (rel.toLowerCase().startsWith('.jsdos/')) continue;
        out.push({ name: rel, data: readFileSync(p) });
      }
    }
  };
  walk(src);
  if (!out.length) throw new Error('no files found under ' + src);
  return out;
}

const RUNNABLE = /\.(exe|com|bat)$/i;

// Decide what to launch: explicit --run, else the lone executable, else ask.
function pickRun(entries, override) {
  if (override) {
    const dir = dirname(override).replace(/\\/g, '/'); const d = dir === '.' ? '' : dir;
    return { dir: d, exe: basename(override), bat: /\.bat$/i.test(override) };
  }
  const exes = entries.filter((e) => RUNNABLE.test(e.name));
  if (exes.length === 0) throw new Error('no .exe/.com/.bat found — pass --run <CMD>');
  // Prefer a shallow executable; if several at the same depth, it is ambiguous → ask.
  exes.sort((a, b) => a.name.split('/').length - b.name.split('/').length);
  const top = exes.filter((e) => e.name.split('/').length === exes[0].name.split('/').length);
  if (top.length > 1) {
    throw new Error('multiple executables found — pick one with --run <CMD>:\n  ' +
      exes.map((e) => e.name).join('\n  '));
  }
  const full = top[0].name;
  const dir = dirname(full).replace(/\\/g, '/'); const d = dir === '.' ? '' : dir;
  return { dir: d, exe: basename(full), bat: /\.bat$/i.test(full) };
}

function genConf({ machine, cycles, memsize, sound, run }) {
  const ax = ['@echo off', 'mount c .', 'c:'];
  if (run.dir) ax.push('cd ' + run.dir.replace(/\//g, '\\'));      // DOS-style path
  ax.push((run.bat ? 'call ' : '') + run.exe);
  const L = [
    '[sdl]', 'autolock=false', '',
    // memsize feeds XMS/EMS — raise it for 386/486-era and DOS4GW protected-mode apps.
    '[dosbox]', 'machine=' + machine, 'memsize=' + memsize, '',
    '[cpu]', 'core=auto', 'cputype=auto', 'cycles=' + cycles, '',
  ];
  if (sound) L.push('[mixer]', 'nosound=false', 'rate=44100', '',
                    '[sblaster]', 'sbtype=sb16', 'irq=7', 'dma=1', 'hdma=5', 'oplmode=auto', 'oplrate=44100', '');
  // Extended/expanded memory + UMBs: required by most post-1990 DOS software.
  L.push('[dos]', 'xms=true', 'ems=true', 'umb=true', '');
  L.push('[autoexec]', ...ax, '');
  return Buffer.from(L.join('\n'), 'utf8');
}

// ---- device upload (mirrors push-ota: pair → session cookie → /api/fs) ----
let sessionCookie = null;
const authHeaders = () => (sessionCookie ? { cookie: sessionCookie } : {});
async function pair(host, pin) {
  const r = await fetch(host + '/api/pair', { method: 'POST',
    headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin }) });
  if (!r.ok) throw new Error('pairing rejected (check the PIN on the Connection screen): HTTP ' + r.status);
  const sc = r.headers.get('set-cookie');
  const m = sc && sc.match(/nucleo_session=[^;]+/);
  if (!m) throw new Error('paired but no session cookie returned');
  sessionCookie = m[0];
}
async function ensureDir(host, dir) {
  const parts = dir.split('/').filter(Boolean); let cur = '';
  for (const p of parts) { cur += '/' + p;
    try { await fetch(host + '/api/fs/mkdir?path=' + encodeURIComponent(cur), { method: 'POST', headers: authHeaders() }); } catch {}
  }
}
async function uploadFile(host, devPath, buf) {
  const r = await fetch(host + '/api/fs/write?path=' + encodeURIComponent(devPath),
    { method: 'POST', headers: authHeaders(), body: buf });
  if (!r.ok) throw new Error('write failed: HTTP ' + r.status + ' ' + (await r.text().catch(() => '')));
  return r.text();
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.src) {
    console.error('usage: node tools/add-dos.mjs <folder|exe> [--run CMD] [--name NAME] [--host URL --pin PIN] [--cycles auto] [--no-sound]');
    process.exit(1);
  }
  const name = (args.name || basename(args.src, extname(args.src))).replace(/[^A-Za-z0-9._-]/g, '_');
  const entries = collect(args.src);
  const run = pickRun(entries, args.run);
  entries.push({ name: '.jsdos/dosbox.conf', data: genConf({ machine: args.machine, cycles: args.cycles, memsize: args.memsize, sound: args.sound, run }) });

  const out = args.out || join(REPO, 'deploy', 'sd', 'data', 'DOS', name + '.jsdos');
  if (args.stage || args.out) { mkdirSync(dirname(out), { recursive: true }); }
  const res = packStoreToFile(out, entries);
  const runStr = (run.dir ? run.dir + '/' : '') + (run.bat ? 'call ' : '') + run.exe;
  console.log(`✓ built ${name}.jsdos  (${res.files} files, ${res.bytes} bytes)  → runs: ${runStr}`);
  console.log(`  ${out}`);

  if (args.host) {
    if (!args.pin) { console.error('✗ --host requires --pin (6-digit code on the Cardputer Connection screen)'); process.exit(1); }
    const host = args.host.replace(/\/$/, '');
    await pair(host, args.pin);
    console.log('✓ paired with device');
    await ensureDir(host, '/data/DOS');
    const devPath = '/data/DOS/' + name + '.jsdos';
    const r = await uploadFile(host, devPath, readFileSync(out));
    console.log(`✓ copied to device: ${devPath}  ${r}`);
    console.log('  Open the DOS Box app (hard-refresh) and pick it.');
  } else {
    console.log('  (no --host given: staged only. Add --host http://IP --pin 123456 to copy it to the device.)');
  }
}

main().catch((e) => { console.error('✗ ' + (e && e.message ? e.message : e)); process.exit(1); });

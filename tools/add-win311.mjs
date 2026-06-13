// add-win311 — package a DOS hard-drive folder that boots a GUEST OS (Windows 3.x) into a
// .jsdos bundle for the DOS Box app. Same "Commodore trick": the device only streams the bytes,
// the browser (js-dos / DOSBox WASM) runs the emulation.
//
// Unlike add-dos.mjs (single game, single executable), a guest OS is a whole C: drive with many
// executables. This tool:
//   1. collects the C:-drive folder (preserving WINDOWS/… paths — the .INI/.GRP files hold
//      absolute C:\WINDOWS\… references, so the tree MUST be preserved, not flattened),
//   2. auto-detects the launcher (WIN.COM by default) and the directory to cd into,
//   3. writes a Windows-tuned .jsdos/dosbox.conf (svga_s3, 32 MB, XMS/EMS for 386 enhanced mode,
//      Sound Blaster 16) whose [autoexec] mounts C:, cd's to the Windows dir and runs `win`,
//   4. packs a store-method ZIP bundle, stages it into deploy/sd/data/DOS (+ mirrors to the sim),
//   5. registers it in /data/DOS/library.json under the "Systems" category so the DOS Box app
//      shows it with a real title + 🪟 icon.
//
// Usage:
//   node tools/add-win311.mjs --src <C-drive folder> [--name win311] [--launch WINDOWS/WIN.COM]
//        [--title "Windows 3.11"] [--category Systems] [--memsize 32] [--cycles max] [--no-mirror]
//   <C-drive folder> must contain WINDOWS/WIN.COM (or pass --launch).
import { readdirSync, statSync, readFileSync, writeFileSync, existsSync, mkdirSync } from 'node:fs';
import { join, relative, sep, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { packStoreToFile } from './make-jsdos.mjs';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
const opt = (k, d = null) => { const i = args.indexOf(k); return i >= 0 ? args[i + 1] : d; };
const flag = (k) => args.includes(k);

const src = opt('--src');
if (!src) { console.error('usage: node tools/add-win311.mjs --src <C-drive folder> [--name win311] [--launch WINDOWS/WIN.COM]'); process.exit(1); }
const name = (opt('--name') || 'win311').replace(/[^A-Za-z0-9._-]/g, '_');
const title = opt('--title') || 'Windows 3.11';
const category = opt('--category') || 'Systems';
const memsize = opt('--memsize') || '32';
// Windows 3.11 in DOSBox: the DYNAMIC core (core=auto/dynamic) hangs Win386 in 386-enhanced mode,
// and cycles=max starves the single-threaded WASM render loop during Windows' CPU-heavy boot, so the
// canvas never paints a frame. core=normal + a fixed, moderate cycle count is the proven-stable combo.
const core = opt('--core') || 'normal';
const cycles = opt('--cycles') || '20000';
const machine = opt('--machine') || 'svga_s3';
const doMirror = !flag('--no-mirror');

// ---- collect the C:-drive tree, forward-slash root-relative names ----
function collect(root) {
  const out = [];
  const walk = (dir) => {
    for (const n of readdirSync(dir)) {
      const p = join(dir, n);
      if (statSync(p).isDirectory()) walk(p);
      else {
        const rel = relative(root, p).split(sep).join('/');
        if (rel.toLowerCase().startsWith('.jsdos/')) continue;     // regenerate our own control dir
        out.push({ name: rel, data: readFileSync(p) });
      }
    }
  };
  walk(root);
  if (!out.length) throw new Error('no files found under ' + root);
  return out;
}

// ---- locate the launcher (WIN.COM) → the dir to cd into + the command ----
function detectLaunch(entries, override) {
  let full = override ? override.replace(/\\/g, '/') : null;
  if (!full) {
    const win = entries.find((e) => /(^|\/)win\.com$/i.test(e.name));
    if (!win) throw new Error('WIN.COM not found — pass --launch <path/to/launcher.com>');
    full = win.name;
  } else if (!entries.some((e) => e.name.toLowerCase() === full.toLowerCase())) {
    throw new Error('--launch target not in the source tree: ' + full);
  }
  const slash = full.lastIndexOf('/');
  return { dir: slash >= 0 ? full.slice(0, slash) : '', exe: slash >= 0 ? full.slice(slash + 1) : full };
}

const launchArgs = opt('--launch-args', '');   // e.g. "/S" to force Windows STANDARD mode (skips the
                                               // [386Enh] section — swap file, WIN386.EXE, third-party
                                               // VxDs like DVA.386 — which can hang 386-enhanced boot
                                               // under DOSBox-WASM; standard mode is the reliable default).

function genConf(launch) {
  const ax = ['@echo off', 'mount c .', 'c:'];
  if (launch.dir) ax.push('cd ' + launch.dir.replace(/\//g, '\\'));
  // `win` (not WIN.COM) so DOSBox runs the .COM launcher from the cwd; Windows then loads itself.
  const cmd = /\.bat$/i.test(launch.exe) ? 'call ' + launch.exe : launch.exe.replace(/\.com$/i, '');
  ax.push(cmd + (launchArgs ? ' ' + launchArgs : ''));
  return [
    '[sdl]', 'autolock=false', '',
    // svga_s3 covers the VGA/SVGA Windows display drivers; memsize feeds XMS for 386 enhanced mode.
    '[dosbox]', 'machine=' + machine, 'memsize=' + memsize, '',
    // core=normal: stable for Windows' protected-mode (386 enhanced) code under DOSBox-WASM.
    '[cpu]', 'core=' + core, 'cputype=auto', 'cycles=' + cycles, '',
    '[mixer]', 'nosound=false', 'rate=44100', '',
    '[sblaster]', 'sbtype=sb16', 'irq=7', 'dma=1', 'hdma=5', 'oplmode=auto', 'oplrate=44100', '',
    // XMS/EMS/UMB: Windows 3.11 386 enhanced mode needs XMS (DOSBox provides it without HIMEM.SYS,
    // which a mounted CONFIG.SYS would otherwise load — CONFIG.SYS is not processed under `mount`).
    '[dos]', 'xms=true', 'ems=true', 'umb=true', '',
    '[autoexec]', ...ax, '',
  ].join('\n');
}

// ---- library.json registration (read-modify-write, schema { "<file>": {title,category,...} }) ----
function registerLibrary(dirs, file, meta) {
  for (const dir of dirs) {
    if (!existsSync(dir)) continue;
    const lp = join(dir, 'library.json');
    let lib = {};
    try { lib = JSON.parse(readFileSync(lp, 'utf8')); } catch {}
    lib[file] = meta;
    writeFileSync(lp, JSON.stringify(lib, null, 2) + '\n');
  }
}

const entries = collect(src);
const launch = detectLaunch(entries, opt('--launch'));
entries.push({ name: '.jsdos/dosbox.conf', data: Buffer.from(genConf(launch), 'latin1') });

const DOS_DIRS = [join(REPO, 'deploy', 'sd', 'data', 'DOS')];
if (doMirror && existsSync(join(REPO, 'tools', 'sd-sim', 'data', 'DOS'))) DOS_DIRS.push(join(REPO, 'tools', 'sd-sim', 'data', 'DOS'));

let res;
for (const dir of DOS_DIRS) { mkdirSync(dir, { recursive: true }); res = packStoreToFile(join(dir, name + '.jsdos'), entries); }
registerLibrary(DOS_DIRS, name + '.jsdos', { title, category, note: 'boots to the Program Manager - give it a few seconds' });

const runStr = (launch.dir ? launch.dir + '\\' : '') + launch.exe;
console.log(`✓ built ${name}.jsdos  (${res.files} files, ${(res.bytes / 1048576).toFixed(1)} MB)  → mount c: + run ${runStr}`);
console.log(`  staged to: ${DOS_DIRS.join('  +  ')}`);
console.log(`  registered in library.json as "${title}" (${category}). Open DOS Box and pick it.`);

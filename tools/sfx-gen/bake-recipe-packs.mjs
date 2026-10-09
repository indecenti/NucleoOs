#!/usr/bin/env node
// Bake + check the native games' SFX WAV packs (deploy/sd/data/<dir>/pack/<cue>.wav).
//
// WHY: the games never synthesize on the device any more. Bulk-synthesizing a cue table in on_enter (two
// float passes + an SD write per cue) ran past the 8 s Task WDT the app task is subscribed to and rebooted
// the Cardputer on any SD without the cache; synth-on-miss froze the game mid-frame. The device now plays
// the deployed pack, else beeps a short tone for important cues (firmware/components/nucleo_app/game_sfx.h).
// So every game needs a pack that covers every cue its firmware can play.
//
// HOW: the cue recipes stay where they are — in the game's C++ (sfx_name + build_voices). This tool cuts
// those two functions out of the REAL firmware source, compiles them on the PC against the REAL
// notify_synth.h (MinGW g++), and runs the same additive synth the device used to run, writing each cue's
// WAV. Same recipes, same math -> the same sound players already know; at 22050 Hz instead of the
// device's 12000, so high partials no longer alias. Hand-designed packs are never overwritten: pinball,
// slots and tanks come from gen_arcade_sfx.py (+ CC0 samples, see tanks-real-sounds.md), tankduel from its
// own pack, Orde from the CC0 set in assets/. A baked game graduates to a hand-designed pack by adding a
// builder to gen_arcade_sfx.py and flipping it to `kind: 'hand'` here.
//
//   node tools/sfx-gen/bake-recipe-packs.mjs              # CHECK every pack covers every cue (gate; exit 1 on a gap)
//   node tools/sfx-gen/bake-recipe-packs.mjs --bake       # (re)bake the recipe packs + copy the asset packs, then check
//   node tools/sfx-gen/bake-recipe-packs.mjs --bake pong  # just one game (dir name)
//   --rate <hz>  bake sample rate (default 22050)
//
// Wired as `npm run sfx:check` / `npm run sfx:bake`. Re-bake after changing a recipe (and bump the game's
// SFX_VER so a stale on-device legacy cache is wiped).
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync, readdirSync, statSync, copyFileSync, openSync, readSync, closeSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const NA = join(ROOT, 'firmware', 'components', 'nucleo_app');
const PACKS = join(ROOT, 'deploy', 'sd', 'data');
const BUILD = join(ROOT, 'build', 'sfx-bake');

// One row per game that plays SFX. dir = the game's folder under /sd/data on the device (DIRR / DIRG /
// DIR / game_sfx_t.dir). kind: 'bake' = baked here from the recipes; 'copy' = copied from `from`;
// 'hand' = a hand-made pack, only checked. deps = enums the recipes name, cut from their header.
const GAMES = [
  { dir: 'brawler',       src: 'brawler_sfx.cpp',        name: 'bsfx_name',   recipe: 'build_voices',  kind: 'bake',
    deps: [['brawler.h', /enum\s*\{\s*BSFX_NAV\b[^}]*\}\s*;/]] },
  { dir: 'poker',         src: 'app_poker.cpp',          name: 'sfx_name',    recipe: 'build_voices',  kind: 'bake' },
  { dir: 'pong',          src: 'app_pong.cpp',           name: 'sfx_name',    recipe: 'build_voices',  kind: 'bake' },
  { dir: 'giardino',      src: 'app_sandgarden.cpp',     name: 'sfx_name',    recipe: 'build_voices',  kind: 'bake' },
  { dir: 'reattore',      src: 'app_reactor.cpp',        name: 'sfx_name',    recipe: 'build_voices',  kind: 'bake' },
  { dir: 'costellazioni', src: 'app_constellations.cpp', name: 'sfx_name',    recipe: 'build_voices',  kind: 'bake',
    deps: [['constellations_content.h', /enum\s*\{\s*SFX_NONE\b[^}]*\}\s*;/]] },
  { dir: 'yahtzee',       src: 'app_yahtzee.cpp',        name: 'sfx_name',    recipe: 'build_voices',  kind: 'bake' },
  { dir: 'snake',         src: 'app_snake.cpp',          name: 'sn_sfx_name', recipe: 'sn_sfx_recipe', kind: 'bake',
    deps: [['app_snake.cpp', /enum\s*\{\s*SX_EAT\b[^}]*\}\s*;/]] },
  { dir: 'Orde',          src: 'app_vs.cpp',             name: 'sfx_name',    recipe: 'sfx_recipe',    kind: 'copy',
    from: 'assets/coop-rpg/orde_pack', deps: [['app_vs.cpp', /enum\s*\{\s*SFX_START\b[^}]*\}\s*;/]] },
  { dir: 'pinball',       src: 'app_pinball.cpp',        name: 'sfx_name',    recipe: 'build_voices',  kind: 'hand' },
  { dir: 'slots',         src: 'app_slots.cpp',          name: 'sfx_name',    recipe: 'build_voices',  kind: 'hand' },
  { dir: 'tanks',         src: 'app_tanks.cpp',          name: 'sfx_name',    recipe: 'build_voices',  kind: 'hand' },
  { dir: 'tankduel',      src: 'app_tankduel.cpp',       name: 'sfx_td_name', recipe: 'sfx_td_recipe', kind: 'hand' },
];

// ---------------------------------------------------------------- args
const argv = process.argv.slice(2);
const bake = argv.includes('--bake');
const ri = argv.indexOf('--rate');
const RATE = ri >= 0 ? parseInt(argv[ri + 1], 10) : 22050;
const only = argv.filter((a, i) => !a.startsWith('--') && argv[i - 1] !== '--rate');
for (const o of only) if (!GAMES.some(g => g.dir === o)) { console.error(`unknown game '${o}' (one of: ${GAMES.map(g => g.dir).join(', ')})`); process.exit(2); }
const games = only.length ? GAMES.filter(g => only.includes(g.dir)) : GAMES;

const MINGW = 'C:/msys64/mingw64/bin';
const GXX = existsSync(join(MINGW, 'g++.exe')) ? join(MINGW, 'g++.exe') : 'g++';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };

// ---------------------------------------------------------------- C++ function extraction
// From the start of the definition's line to its matching brace, skipping comments and literals.
function cutFunction(src, re, what) {
  const m = re.exec(src);
  if (!m) throw new Error(`${what}: definition not found`);
  const start = src.lastIndexOf('\n', m.index) + 1;
  let depth = 0;
  for (let i = m.index + m[0].length - 1; i < src.length; i++) {
    const c = src[i], n = src[i + 1];
    if (c === '/' && n === '/') { i = src.indexOf('\n', i); if (i < 0) break; continue; }
    if (c === '/' && n === '*') { i = src.indexOf('*/', i + 2) + 1; if (i <= 0) break; continue; }
    if (c === '"' || c === "'") { for (i++; i < src.length && src[i] !== c; i++) if (src[i] === '\\') i++; continue; }
    if (c === '{') depth++;
    else if (c === '}' && --depth === 0) return src.slice(start, i + 1);
  }
  throw new Error(`${what}: unbalanced braces`);
}
const TAIL = String.raw`\)\s*(?:\/\/[^\n]*\s*)*\{`;   // ')' + optional trailing // comments + '{'
const nameRe = fn => new RegExp(String.raw`\b${fn}\s*\(\s*int\s+\w+\s*` + TAIL);
const recipeRe = fn => new RegExp(String.raw`\b${fn}\s*\(\s*int\s+\w+\s*,\s*notify_voice_t\s*\*\s*\w+\s*` + TAIL);

// Compile the game's cue table; returns the exe path. Program: `list` prints "id name voices" per cue;
// `bake <dir> <rate>` also writes <dir>/<name>.wav through the real notify_synth_voices_wav.
function buildCueTool(g) {
  const src = readFileSync(join(NA, g.src), 'utf8');
  const deps = (g.deps || []).map(([f, re]) => {
    const m = re.exec(readFileSync(join(NA, f), 'utf8'));
    if (!m) throw new Error(`${g.dir}: dependency ${re} not found in ${f}`);
    return m[0];
  });
  const cpp = [
    '// GENERATED by tools/sfx-gen/bake-recipe-packs.mjs from ' + g.src + ' — do not edit.',
    '#include <cstdio>', '#include <cstdlib>', '#include <cstring>', '#include "notify_synth.h"',
    ...deps,
    cutFunction(src, nameRe(g.name), `${g.dir}: ${g.name}()`),
    cutFunction(src, recipeRe(g.recipe), `${g.dir}: ${g.recipe}()`),
    `int main(int argc, char **argv) {
    const bool bake = argc >= 4 && !strcmp(argv[1], "bake");
    const int rate = bake ? atoi(argv[3]) : 0;
    static notify_voice_t v[256];
    const char *seen[128]; int ns = 0;
    for (int id = 1; id < 128; id++) {
        memset(v, 0, sizeof v);
        const int nv = ${g.recipe}(id, v);
        const char *nm = ${g.name}(id);
        if (nv <= 0 || !nm || !*nm || !strcmp(nm, "x") || !strcmp(nm, "?")) continue;   // no such cue
        for (int k = 0; k < ns; k++) if (!strcmp(seen[k], nm)) { fprintf(stderr, "cue name '%s' used twice (id %d)\\n", nm, id); return 3; }
        seen[ns++] = nm;
        printf("%d %s %d\\n", id, nm, nv);
        if (bake) {
            char p[1024]; snprintf(p, sizeof p, "%s/%s.wav", argv[2], nm);
            if (notify_synth_voices_wav(v, nv, p, rate) != 0) { fprintf(stderr, "write failed: %s\\n", p); return 2; }
        }
    }
    return 0;
}`,
  ].join('\n');
  mkdirSync(BUILD, { recursive: true });
  const cppPath = join(BUILD, `${g.dir}.cpp`);
  const exe = join(BUILD, process.platform === 'win32' ? `${g.dir}.exe` : g.dir);
  writeFileSync(cppPath, cpp);
  const cc = spawnSync(GXX, ['-std=gnu++17', '-O2', '-w', '-I', NA, cppPath, '-o', exe], { env, encoding: 'utf8' });
  if (cc.status !== 0) throw new Error(`${g.dir}: compile failed\n${cc.stdout || ''}${cc.stderr || ''}`);
  return exe;
}
function runCueTool(exe, args) {
  const r = spawnSync(exe, args, { env, encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`${exe} ${args.join(' ')} failed (${r.status})\n${r.stderr || ''}`);
  return r.stdout.trim().split(/\r?\n/).filter(Boolean).map(l => { const [id, name, nv] = l.split(' '); return { id: +id, name, nv: +nv }; });
}

// ---------------------------------------------------------------- WAV check (what nucleo_audio_play reads)
function wavProblem(path) {
  if (!existsSync(path)) return 'missing';
  const size = statSync(path).size;
  if (size <= 44) return `${size} bytes (header only / truncated)`;
  const b = Buffer.alloc(44); const fd = openSync(path, 'r'); readSync(fd, b, 0, 44, 0); closeSync(fd);
  if (b.toString('ascii', 0, 4) !== 'RIFF' || b.toString('ascii', 8, 12) !== 'WAVE') return 'not a RIFF/WAVE file';
  const fmt = b.readUInt16LE(20), ch = b.readUInt16LE(22), rate = b.readUInt32LE(24), bits = b.readUInt16LE(34);
  if (fmt !== 1 || ch !== 1 || bits !== 16) return `not 16-bit mono PCM (fmt ${fmt}, ${ch} ch, ${bits} bit)`;
  if (rate < 8000 || rate > 48000) return `odd sample rate ${rate}`;
  return null;
}

// ---------------------------------------------------------------- main
let gaps = 0, wrote = 0;
for (const g of games) {
  const pack = join(PACKS, g.dir, 'pack');
  let cues;
  try {
    const exe = buildCueTool(g);
    if (bake && g.kind === 'bake') {
      mkdirSync(pack, { recursive: true });
      cues = runCueTool(exe, ['bake', pack, String(RATE)]);
      wrote += cues.length;
    } else {
      cues = runCueTool(exe, ['list']);
      if (bake && g.kind === 'copy') {
        mkdirSync(pack, { recursive: true });
        for (const c of cues) {
          const s = join(ROOT, g.from, `${c.name}.wav`);
          if (existsSync(s)) { copyFileSync(s, join(pack, `${c.name}.wav`)); wrote++; }
        }
      }
    }
  } catch (e) { console.error(`  FAIL ${e.message}`); gaps++; continue; }

  const bad = cues.map(c => [c.name, wavProblem(join(pack, `${c.name}.wav`))]).filter(([, p]) => p);
  const names = new Set(cues.map(c => `${c.name}.wav`.toLowerCase()));
  const extra = existsSync(pack) ? readdirSync(pack).filter(f => !names.has(f.toLowerCase())) : [];
  const tag = `${g.dir.padEnd(14)} ${String(cues.length).padStart(2)} cues  (${g.kind})`;
  if (bad.length) {
    gaps += bad.length;
    console.log(`  FAIL ${tag}: ${bad.map(([n, p]) => `${n} ${p}`).join('; ')}`);
  } else console.log(`  ok   ${tag}`);
  if (extra.length) console.log(`       ${g.dir}: not a cue (unused): ${extra.join(', ')}`);
}
if (bake) console.log(`sfx: wrote ${wrote} WAV(s) at ${RATE} Hz (baked) / asset rate (copied)`);
if (gaps) {
  console.log(`sfx: ${gaps} gap(s) — those cues fall back to a tone (important) or silence on the device. Fix: npm run sfx:bake`);
  process.exit(1);
}
console.log(`sfx: every pack covers every cue (${games.length} games)`);

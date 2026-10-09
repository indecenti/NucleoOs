#!/usr/bin/env node
// Every native game speaks the five OS languages (firmware/components/nucleo_app/game_text.h).
// For each game (tools/native-host/games.mjs) whose sources use GT(), this fails when:
//   - a GT("it", "en") English literal has no es/fr/de entry in tools/game-i18n/<game>.json
//   - a GT() argument is not a plain string literal (the pack is keyed by the literal English text)
//   - a string is not ASCII (the TFT fonts have no accented glyphs)
//   - a built pack in tools/sd-sim/system/i18n/games is stale (run tools/game-i18n/build.mjs)
//   - the game's sources still pick a language by themselves (a private it/en toggle instead of the OS one)
// Unused JSON entries are reported, not failed.
//   node tools/game-i18n/check.mjs [game,...]
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { GAMES } from '../native-host/games.mjs';
import { pack, LANGS, OUT, isAscii } from './build.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const APPS = join(root, 'firmware', 'components', 'nucleo_app');

const unC = (s) => s.replace(/\\(x[0-9a-fA-F]{2}|.)/g, (_, e) => e[0] === 'x' ? String.fromCharCode(parseInt(e.slice(1), 16)) : e === 'n' ? '\n' : e === 't' ? '\t' : e);
const LIT = '"((?:[^"\\\\\\n]|\\\\.)*)"';
const GT_OK = new RegExp(`\\bGTK?\\(\\s*${LIT}\\s*,\\s*${LIT}\\s*\\)`, 'g');   // GT() calls and GTK() table pairs

const want = process.argv[2] ? process.argv[2].split(',') : Object.keys(GAMES);
let bad = 0, games = 0;
const out = [];
for (const id of want) {
  const g = GAMES[id];
  if (!g) { console.error(`game-i18n: unknown game ${id}`); process.exit(2); }
  const files = [g.src, ...(g.extra || [])].map((f) => join(APPS, f));
  const src = files.map((f) => readFileSync(f, 'utf8')).join('\n');
  const uses = [...src.matchAll(/\bGTK?\(/g)].length - [...src.matchAll(/#define GTK?\(/g)].length;
  if (!uses) { if (process.argv[2]) { out.push(`FAIL ${id}: no GT() strings yet`); bad++; } continue; }
  games++;
  const lits = [...src.matchAll(GT_OK)].map((m) => ({ it: unC(m[1]), en: unC(m[2]) }));
  if (process.argv.includes('--missing')) {          // print a JSON skeleton of the strings still lacking es/fr/de
    const jf0 = join(here, `${id}.json`), have = existsSync(jf0) ? JSON.parse(readFileSync(jf0, 'utf8')).strings : {};
    const miss = {}; for (const { it, en } of lits) if (!have[en]) miss[en] = { it, es: '', fr: '', de: '' };
    console.log(JSON.stringify(miss, null, 2)); continue;
  }
  const problems = [];
  if (lits.length !== uses) problems.push(`${uses - lits.length} GT() call(s) whose arguments are not two plain string literals`);
  const jf = join(here, `${id}.json`);
  const j = existsSync(jf) ? JSON.parse(readFileSync(jf, 'utf8')) : { game: id, strings: {} };
  const used = new Set();
  for (const { it, en } of lits) {
    used.add(en);
    if (!isAscii(it) || !isAscii(en)) problems.push(`non-ASCII: "${it}" / "${en}"`);
    const tr = j.strings[en];
    const miss = LANGS.filter((l) => !tr || typeof tr[l] !== 'string' || !tr[l].length);
    if (miss.length) problems.push(`"${en}" lacks ${miss.join('/')}`);
  }
  // a private language switch left behind (the game must follow the OS language through GT)
  if (/\bg_lang\b|\bs_lang\b|\btx\(\s*"/.test(src)) problems.push('still has its own language switch (g_lang / s_lang / tx("..."))');
  if (!problems.length) {
    try {
      for (const l of LANGS) {
        const f = join(OUT, `${id}.${l}`);
        if (!existsSync(f) || !readFileSync(f).equals(pack(j, l))) { problems.push(`pack ${id}.${l} is stale — run node tools/game-i18n/build.mjs`); break; }
      }
    } catch (e) { problems.push(e.message); }
  }
  const unused = Object.keys(j.strings).filter((k) => !used.has(k));
  if (problems.length) { bad++; out.push(`FAIL ${id}: ${problems.length} problem(s)\n  - ${[...new Set(problems)].slice(0, 25).join('\n  - ')}`); }
  else out.push(`ok   ${id}: ${lits.length} strings in 5 languages${unused.length ? ` (${unused.length} unused JSON entries)` : ''}`);
}
console.log(out.join('\n'));
console.log(`game-i18n: ${games - (bad > games ? games : bad)}/${games} games speak 5 languages${bad ? `, ${bad} failing` : ''}`);
process.exit(bad ? 1 : 0);

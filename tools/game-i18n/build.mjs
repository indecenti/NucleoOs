#!/usr/bin/env node
// Build the native games' language packs (firmware/components/nucleo_app/game_text.h).
//
// Source of truth: tools/game-i18n/<game>.json
//   { "game": "<registry id>", "strings": { "<English string exactly as passed to GT()>": { "es": "...", "fr": "...", "de": "..." } } }
// Output: tools/sd-sim/system/i18n/games/<game>.<es|fr|de> — staged to /sd/system/i18n/games/ by deploy.ps1 and
// sd_deploy.py. Format (little endian): "GTX1", u16 count, count x { u32 fnv1a(english), u16 offset } sorted
// by hash, then the NUL-terminated translations. Deterministic; refuses non-ASCII (the TFT fonts have no
// accented glyphs), empty translations, hash collisions and packs over the firmware's 12 KB limit.
//   node tools/game-i18n/build.mjs [--check]     --check: fail if a pack on disk is stale, write nothing
import { readFileSync, writeFileSync, readdirSync, mkdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
export const OUT = join(root, 'tools', 'sd-sim', 'system', 'i18n', 'games');
export const LANGS = ['es', 'fr', 'de'];
const MAX_PACK = 12288;

export function fnv1a(s) {
  let h = 0x811c9dc5;
  for (const b of Buffer.from(s, 'latin1')) { h ^= b; h = Math.imul(h, 16777619) >>> 0; }
  return h >>> 0;
}
export const isAscii = (s) => /^[\x20-\x7e]*$/.test(s);

export function loadSources() {
  return readdirSync(here).filter((f) => f.endsWith('.json')).sort().map((f) => {
    const j = JSON.parse(readFileSync(join(here, f), 'utf8'));
    if (!j.game || j.game + '.json' !== f) throw new Error(`${f}: "game" must equal the file name`);
    return j;
  });
}

export function pack(j, lang) {
  const rows = [];
  const seen = new Map();
  for (const [en, tr] of Object.entries(j.strings || {})) {
    const t = tr && tr[lang];
    if (typeof t !== 'string' || !t.length) throw new Error(`${j.game}: "${en}" has no ${lang} translation`);
    if (!isAscii(en) || !isAscii(t)) throw new Error(`${j.game}: non-ASCII in "${en}" / ${lang} "${t}" (the TFT fonts have no accented glyphs)`);
    const h = fnv1a(en);
    if (seen.has(h)) throw new Error(`${j.game}: hash collision between "${seen.get(h)}" and "${en}"`);
    seen.set(h, en);
    rows.push({ h, t });
  }
  rows.sort((a, b) => a.h - b.h);
  const head = 6 + rows.length * 6;
  const strs = rows.map((r) => Buffer.concat([Buffer.from(r.t, 'latin1'), Buffer.from([0])]));
  const total = head + strs.reduce((n, b) => n + b.length, 0);
  if (total > MAX_PACK) throw new Error(`${j.game}.${lang}: ${total} B, over the firmware's ${MAX_PACK} B pack limit`);
  const out = Buffer.alloc(total);
  out.write('GTX1', 0, 'latin1'); out.writeUInt16LE(rows.length, 4);
  let off = head;
  rows.forEach((r, i) => { out.writeUInt32LE(r.h, 6 + i * 6); out.writeUInt16LE(off, 6 + i * 6 + 4); strs[i].copy(out, off); off += strs[i].length; });
  return out;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  const checkOnly = process.argv.includes('--check');
  mkdirSync(OUT, { recursive: true });
  let stale = 0, written = 0;
  try {
    for (const j of loadSources()) for (const lang of LANGS) {
      const buf = pack(j, lang), file = join(OUT, `${j.game}.${lang}`);
      const same = existsSync(file) && readFileSync(file).equals(buf);
      if (same) continue;
      if (checkOnly) { console.log(`STALE ${j.game}.${lang}`); stale++; } else { writeFileSync(file, buf); written++; }
    }
  } catch (e) { console.error(`game-i18n: ${e.message}`); process.exit(1); }
  if (checkOnly && stale) { console.error(`game-i18n: ${stale} pack(s) stale — run node tools/game-i18n/build.mjs`); process.exit(1); }
  console.log(`game-i18n: packs ${checkOnly ? 'up to date' : `built (${written} written)`} in tools/sd-sim/system/i18n/games/`);
}

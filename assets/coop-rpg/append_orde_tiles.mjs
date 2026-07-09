// append_orde_tiles.mjs — adds NEW world tiles to the existing orde_atlas.bin WITHOUT touching a
// single existing byte. The original 23 Orde tiles were cropped ad-hoc (no committed build script —
// see SPRITES.md's "Bash recipe in git history" note) and re-deriving their exact (col,row) crops from
// scratch risks silently swapping sprite art for the wrong one. So: never re-crop what's already
// there — only ever crop BRAND NEW tiles and append them past the current end of the file. Same
// crop/pack pipeline as Cardler's assets/cardler/build_atlas.mjs (ffmpeg PNG->RGBA, RGB332 pack, same
// magenta key 0xE3 — matches repack_orde_atlas8.mjs's already-repacked 8bpp atlas).
//   node append_orde_tiles.mjs
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, appendFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const ROOT = new URL('./', import.meta.url).pathname.replace(/^\/([A-Z]:)/, '$1');
const DIR = { T: ROOT + 'tiny-town/Tiles/', D: ROOT + 'tiny-dungeon/Tiles/' };
const COLS = 12;
const ATLAS = new URL('./orde_atlas.bin', import.meta.url);
const TKEY = 0xE3;   // color332(255,0,255) — matches repack_orde_atlas8.mjs / Cardler's key

// New tiles, in the exact order app_vs.cpp's T_N enum appends them (index = current tile count + i).
// Coords verified against assets/cardler/build_atlas.mjs's TILES list (same source sheets).
const NEW_TILES = [
  ['GRASSCLOVER', 'T', 1, 0],   // extra ground-texture variant for meadow variety
  ['GRASSPEBBLE',  'T', 7, 3],   // extra ground-texture variant for meadow variety
  ['SAND',         'T', 3, 3],   // rare sandy clearing zone
  ['SIGN',         'T', 11, 6],  // sparse camp-debris landmark
  ['BARREL',       'T', 11, 8],  // sparse camp-debris landmark
  ['CRATE',        'D', 6, 5],   // sparse camp-debris landmark
  ['CHEST',        'D', 5, 7],   // sparse camp-debris landmark
];

function crop(pk, col, row, tmp) {
  const num = row * COLS + col;
  const src = DIR[pk] + 'tile_' + String(num).padStart(4, '0') + '.png';
  const out = join(tmp, `${pk}_${num}.rgba`);
  execFileSync('ffmpeg', ['-y', '-i', src, '-f', 'rawvideo', '-pix_fmt', 'rgba', out, '-loglevel', 'error']);
  return readFileSync(out);
}
function pack(rgba) {
  const o = Buffer.alloc(256);
  for (let i = 0; i < 256; i++) {
    const r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
    if (a < 128) { o[i] = TKEY; continue; }
    let c = ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6);   // RRRGGGBB, same truncation as Cardler's pack()
    if (c === TKEY) c = 0xE7;                               // nudge a real-magenta opaque pixel off the key
    o[i] = c;
  }
  return o;
}

const before = readFileSync(ATLAS);
if (before.length % 256 !== 0) throw new Error('orde_atlas.bin: not a whole number of 256B tiles — repack first');
const tileCountBefore = before.length / 256;

const tmp = mkdtempSync(join(tmpdir(), 'orde-append-'));
const chunks = [];
for (const [name, pk, col, row] of NEW_TILES) {
  chunks.push(pack(crop(pk, col, row, tmp)));
  console.log(`  + ${name} (${pk} ${col},${row}) -> tile #${tileCountBefore + chunks.length - 1}`);
}
appendFileSync(ATLAS, Buffer.concat(chunks));
console.log(`orde_atlas.bin: ${tileCountBefore} -> ${tileCountBefore + chunks.length} tiles ` +
            `(+${chunks.length * 256} B), existing bytes untouched.`);

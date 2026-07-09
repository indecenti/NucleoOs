// build_atlas.mjs — deterministic Cardler atlas builder. Crops named 16x16 tiles from the CC0 Kenney
// tiny-town / tiny-dungeon sheets and packs them to raw RGB565LE with an exact magenta colour-key.
// Output: cardler_atlas.bin (device) + atlas_v.rgba (RGBA, for host preview). Re-run to reshape the set.
//   node build_atlas.mjs
// TILES order == the C enum in worldgen.mjs (keep both in sync). Coords are (col,row), stride 17px.
import { execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const ROOT = new URL('../coop-rpg/', import.meta.url).pathname.replace(/^\/([A-Z]:)/, '$1');
// crop from the individual Tiles/tile_NNNN.png — those are TRUE RGBA (the packed tilemap.png is a
// palette PNG with NO per-pixel alpha, so cropping it yields opaque black backgrounds). tile# = row*12+col.
const DIR = { T: ROOT + 'tiny-town/Tiles/', D: ROOT + 'tiny-dungeon/Tiles/' };
const COLS = 12;

// name, pack, col, row  — index = position in this list
const TILES = [
  // ground / nature
  ['GRASS',   'T', 0, 0], ['GRASS2', 'T', 2, 0], ['SAND', 'T', 3, 3], ['FLOWER', 'T', 5, 1],
  ['GRASSCLOVER', 'T', 1, 0], ['GRASSPEBBLE', 'T', 7, 3],   // extra ground-texture variants for meadow variety
  // dirt/path 9-slice against grass (corners + edges + fill) — for paths & bare-earth clearings
  ['DTL', 'T', 0, 1], ['DT', 'T', 1, 1], ['DTR', 'T', 2, 1],
  ['DL',  'T', 0, 2], ['PATH', 'T', 1, 2], ['DR', 'T', 2, 2],
  ['DBL', 'T', 0, 3], ['DB', 'T', 1, 3], ['DBR', 'T', 2, 3],
  // single-tile SHORT trees (row2 = a complete little tree, canopy+trunk in one tile)
  ['PINE',    'T', 4, 2], ['AUTUMN', 'T', 3, 2], ['BUSH', 'T', 5, 0], ['MUSH', 'T', 5, 2],
  // TALL trees are 2 tiles: canopy TOP (row0, tapered point) stacked over canopy BOTTOM+trunk (row1).
  // Using only the bottom half (my old "round tree") left the canopy sheared flat at the top.
  ['PINETOP', 'T', 4, 0], ['PINEBOT', 'T', 4, 1], ['AUTTOP', 'T', 3, 0], ['AUTBOT', 'T', 3, 1],
  // roofs are flat-top 2-row shingle blocks (upper = ridge cap, lower = eave meeting the wall) — NOT a
  // floating peak. Each row has a real LEFT-EDGE / FILL / RIGHT-EDGE triplet (dark trim on the outer
  // side, verified via a left/right-column brightness scan — a byte-exact compare had wrongly suggested
  // these were just noise-variant fill, which is what caused the roof to look "cut off" flat at the sides
  // with no finishing trim). red + grey/blue variants.
  ['ROOFUL', 'T', 4, 4], ['ROOFUF', 'T', 5, 4], ['ROOFUR', 'T', 6, 4],
  ['ROOFDL', 'T', 4, 5], ['ROOFDF', 'T', 5, 5], ['ROOFDR', 'T', 6, 5],
  ['GROOFUL', 'T', 0, 4], ['GROOFUF', 'T', 1, 4], ['GROOFUR', 'T', 2, 4],
  ['GROOFDL', 'T', 0, 5], ['GROOFDF', 'T', 1, 5], ['GROOFDR', 'T', 2, 5],
  // openings: Kenney door/window tiles ALREADY include the wall around them. grey-stone set (row7) +
  // tan/wood set for adobe houses.
  ['WINDOW', 'T', 4, 7], ['DOOR', 'T', 5, 7], ['WINDOWT', 'T', 0, 7], ['DOORT', 'T', 1, 7],
  // house side walls: dedicated EDGE tile (dashed corner trim, for the building's left side) + plain FILL
  // (for a 3-wide house: edge / window-or-door / edge-mirrored — see MIRROR below for the right side).
  ['WALLE', 'T', 4, 6], ['WALLF', 'T', 5, 6], ['WWALLE', 'T', 0, 6], ['WWALLF', 'T', 1, 6],
  // dormer window: a 2nd-storey window set INTO the roof band (verified via edge-connectivity: only
  // this pair in the roof rows has a transparent frame, i.e. an overlay, not a roof-fill texture).
  // Used for a taller "manor" house variant. grey-wall context (51) + red/tan-wall context (55).
  ['DORMER', 'T', 3, 4], ['DORMERR', 'T', 7, 4],
  // faro (lighthouse) + corner-turret 9-slice pieces: CT is the merlon/battlement cornice strip — same
  // light, self-consistent family as CTL/CL/etc (no arch/gate mixed in, so no shade-mismatch risk).
  // CF/CB/GARCHL/GARCHR/GATEL/GATER/GCAP stay dropped (unused, reclaimed RAM).
  ['CTL', 'T', 0, 8], ['CT', 'T', 1, 8], ['CTR', 'T', 2, 8], ['CL', 'T', 0, 9], ['CR', 'T', 2, 9], ['CBL', 'T', 0, 10], ['CBR', 'T', 2, 10],
  // fortress wall accents (same light family as CT/CL, safe): a barred arrow-slit + a barred wall vault.
  ['ARROWSLIT', 'T', 6, 8],
  // LADDER (7,8) dropped: cataloged but never placeable (leans on the ground, not a wall fixture like
  // the door/window/arrow-slit family) -- same reclaimed-RAM call as CF/CB/GARCHL/GARCHR/GATEL/GATER/GCAP.
  // props
  ['SIGN', 'T', 11, 6], ['BARREL', 'T', 11, 8], ['CRATE', 'D', 6, 5],
  // fence autotile: straight rails (H + a TRUE seamless V, not the end-post cap) + two DISTINCT corner
  // sprites (top pair / bottom pair aren't vertical mirrors of each other in the source art — verified
  // pixel-exact — so top-left/top-right share one tile via runtime H-flip, ditto bottom-left/right).
  ['FENCEH', 'T', 9, 6], ['FENCEV', 'T', 8, 4], ['FENCE_TC', 'T', 8, 3], ['FENCE_BC', 'T', 8, 5],
  // minecart-track autotile: horizontal, vertical, and ONE corner (E+S) flipped for the other 3 corners
  ['RAILH', 'D', 8, 6], ['RAILV', 'D', 7, 5], ['RAILC', 'D', 9, 5],
  // characters
  ['HERO', 'D', 0, 8], ['WIZ', 'D', 0, 7], ['VILL', 'D', 1, 7], ['KID', 'T', 8, 8], ['PRIN', 'D', 3, 8], ['OLD', 'D', 2, 7], ['VIKING', 'D', 3, 7], ['WITCH', 'D', 4, 8],
  // beasts
  ['WOLF', 'D', 4, 10], ['SPIDER', 'D', 2, 10], ['GHOST', 'D', 0, 9], ['WRAITH', 'D', 1, 10], ['DEMON', 'D', 2, 9], ['BARB', 'D', 1, 9],
  // misc
  ['BARGATE', 'D', 7, 5], ['CHEST', 'D', 5, 7],
  // haunted-biome + fauna tiles (appended at the END so existing indices never shift). CRYPT/CRYPT2 are
  // opaque dark dungeon-stone floors (the graveyard ground); GRAVE + FLAME are keyed props (headstone,
  // torch); RAT is a small critter sprite for wandering fauna.
  ['CRYPT', 'D', 0, 0], ['CRYPT2', 'D', 0, 1], ['GRAVE', 'D', 5, 5], ['FLAME', 'D', 5, 2], ['RAT', 'D', 3, 10],
];

const tmp = mkdtempSync(join(tmpdir(), 'cardler-'));
// 8bpp RGB332 to MATCH the device canvas (also 8bpp) — half the atlas RAM (256 B/tile vs 512) and a
// straight memcpy blit instead of a per-pixel 565->332 convert every frame. TKEY = RGB332 magenta.
const TKEY = 0xE3;   // color332(255,0,255) = ((7<<3)+0)<<2 + 3 = 0xE3
function crop(pk, col, row) {
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
    let c = ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6);   // RRRGGGBB
    if (c === TKEY) c = 0xE7;                               // nudge a real-magenta opaque pixel off the key
    o[i] = c;
  }
  return o;
}

// opaque tiles that need a pre-flipped mirror baked in (the runtime FLP flip only works for keyed
// overlays via the per-pixel blit_key; opaque base tiles use the fast pushImage blit_op, no runtime
// flip). Building both orientations at atlas-build time keeps blit_op untouched.
const MIRROR = new Set(['WALLE', 'WWALLE']);
function mirrorX(rgba) { const o = Buffer.alloc(1024); for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) rgba.copy(o, (y * 16 + x) * 4, (y * 16 + (15 - x)) * 4, (y * 16 + (15 - x)) * 4 + 4); return o; }

const names = [], bins = [], raws = [];
for (const [name, pk, col, row] of TILES) {
  const rgba = crop(pk, col, row);
  names.push('C_' + name); bins.push(pack(rgba)); raws.push(rgba);
  if (MIRROR.has(name)) { const m = mirrorX(rgba); names.push('C_' + name + '_R'); bins.push(pack(m)); raws.push(m); }
}
writeFileSync(new URL('./cardler_atlas.bin', import.meta.url), Buffer.concat(bins));
writeFileSync(new URL('./atlas_v.rgba', import.meta.url), Buffer.concat(raws));
writeFileSync(new URL('./atlas_enum.json', import.meta.url), JSON.stringify(names));
console.log(`atlas: ${names.length} tiles (incl. mirrors), ${names.length * 256} bytes (8bpp RGB332)`);
console.log('enum:', names.join(' '));

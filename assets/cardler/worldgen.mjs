// worldgen.mjs — procedural, data-driven world generator for Cardler.
// Emits firmware/components/nucleo_app/cardler_world.h: the atlas enum, per-char lookup tables, TWO tile
// layers (BASE terrain + OVR props/nature), and an ENT[] table (NPCs + beasts).
//   node worldgen.mjs [seed] [W] [H]
//
// TWO-LAYER MODEL (fixes the "green-backed barrel on brown ground" bug): a prop tile (barrel, crate,
// tree, fence, rail…) is a TRANSPARENT sprite — its cell keeps whatever terrain is underneath. So base
// and overlay are separate grids: put a barrel on a dirt apron and the apron shows through, not grass.
// BASE chars are opaque ground/building tiles; OVR chars are keyed props drawn on top of the base.
import { readFileSync, writeFileSync } from 'node:fs';

const NAMES = JSON.parse(readFileSync(new URL('./atlas_enum.json', import.meta.url)));   // ['C_GRASS',...]
const IDX = {}; NAMES.forEach((n, i) => IDX[n.replace('C_', '')] = i);
const SEED = (process.argv[2] | 0) || 1337, W = (process.argv[3] | 0) || 144, H = (process.argv[4] | 0) || 96;

// per-tile transparency (from the RGBA atlas) — used to guarantee a BASE tile is always opaque.
const ATL = readFileSync(new URL('./atlas_v.rgba', import.meta.url));
const transp = (idx) => { let n = 0; for (let p = 0; p < 256; p++) if (ATL[idx * 1024 + p * 4 + 3] < 128) n++; return n; };
const OPAQUE = NAMES.map((_, i) => transp(i) <= 4);   // a tile safe to use as an opaque base layer

// ---- BASE chars: opaque ground + building tiles (the terrain layer) ----
const CB = {
  '.': 'GRASS', ',': 'GRASS2', 'p': 'PATH', 's': 'SAND', '@': 'GRASS',
  'c': 'GRASSCLOVER', 'A': 'GRASSPEBBLE',
  'g': 'CRYPT', 'G': 'CRYPT2',                                // dark dungeon-stone ground for the haunted zone
  // dirt/path 9-slice edge chars (grass fringe): corners + sides
  'Q': 'DTL', 'U': 'DT', 'E': 'DTR', 'N': 'DL', 'M': 'DR', 'Z': 'DBL', 'X': 'DB', 'B': 'DBR',
  // roof: real left-edge / fill / right-edge triplets (dark trim on the outer side), ridge + eave, red/grey
  '<': 'ROOFUL', '-': 'ROOFUF', '>': 'ROOFUR', '^': 'ROOFDL', '%': 'ROOFDF', '&': 'ROOFDR',
  '[': 'GROOFUL', '_': 'GROOFUF', ']': 'GROOFUR', '~': 'GROOFDL', '`': 'GROOFDF', '!': 'GROOFDR',
  'i': 'WINDOW', 'd': 'DOOR', 'n': 'WINDOWT', 'e': 'DOORT',           // door/window (wall baked in)
  'r': 'WALLE', 'a': 'WALLE_R', 'q': 'WALLF', 'y': 'WWALLE', 'u': 'WWALLE_R', 'w': 'WWALLF',
  'D': 'DORMER', 'v': 'DORMERR',                                       // attic windows (roof-shingle bg baked)
  '1': 'CTL', '3': 'CTR', 'l': 'CL', 'j': 'CR', '7': 'CBL', '9': 'CBR', '2': 'CT', '4': 'ARROWSLIT',
};
// ---- OVR chars: transparent props/nature drawn on top of whatever base the cell has ----
// { o: tile, s: solid?, fl: flip bits (bit0=H, bit1=V) }
const CO = {
  // FLOWER is a flowers-on-grass sprite with TRANSPARENT gaps — it must be a keyed overlay (drawn with
  // blit_key), never an opaque base (blit_op would copy the magenta key straight through as pink pixels).
  '*': { o: 'FLOWER' },
  'T': { o: 'PINE', s: 1 }, 'Y': { o: 'AUTUMN', s: 1 }, 'b': { o: 'BUSH', s: 1 }, 'm': { o: 'MUSH' },
  // tall 2-tile trees: BOTTOM (trunk, solid) + TOP (canopy point, walkable — the cell above, over grass)
  'o': { o: 'PINEBOT', s: 1 }, 'O': { o: 'PINETOP' }, '5': { o: 'AUTBOT', s: 1 }, '6': { o: 'AUTTOP' },
  'S': { o: 'SIGN', s: 1 }, 'k': { o: 'BARREL', s: 1 }, 'x': { o: 'CRATE', s: 1 },
  'C': { o: 'CHEST', s: 1 }, 'J': { o: 'BARGATE', s: 1 },
  // fence autotile: H rail + a TRUE seamless V rail. Top corners share one sprite via H-flip; bottom
  // corners share a DIFFERENT sprite via H-flip (top/bottom pairs aren't vertical mirrors in the art).
  '=': { o: 'FENCEH', s: 1 }, 'I': { o: 'FENCEV', s: 1 },
  'F': { o: 'FENCE_TC', s: 1, fl: 0 }, 'P': { o: 'FENCE_TC', s: 1, fl: 1 },
  'L': { o: 'FENCE_BC', s: 1, fl: 0 }, 'K': { o: 'FENCE_BC', s: 1, fl: 1 },
  // minecart track: straights + 4 corners (ONE corner tile, flipped). base underneath is sand, walkable.
  'R': { o: 'RAILH' }, 'V': { o: 'RAILV' },
  '(': { o: 'RAILC', fl: 0 }, ')': { o: 'RAILC', fl: 1 }, '{': { o: 'RAILC', fl: 2 }, '}': { o: 'RAILC', fl: 3 },
  't': { o: 'GRAVE', s: 1 }, 'f': { o: 'FLAME' },             // haunted props: headstone (solid) + torch flame
};
for (const [c, b] of Object.entries(CB)) if (IDX[b] === undefined) throw new Error(`base char '${c}': unknown tile '${b}'`);
for (const [c, v] of Object.entries(CO)) if (IDX[v.o] === undefined) throw new Error(`ovr char '${c}': unknown tile '${v.o}'`);
// any base tile that turns out to have transparency is unsafe as a ground fill — flag it loudly.
for (const [c, b] of Object.entries(CB)) if (!OPAQUE[IDX[b]] && c !== '@') console.error(`WARN base '${c}'->${b} is transparent (may show through)`);

// ---- seeded PRNG ----
let _s = SEED >>> 0;
const rnd = () => { _s |= 0; _s = (_s + 0x6D2B79F5) | 0; let t = Math.imul(_s ^ (_s >>> 15), 1 | _s); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
const ri = (a, b) => a + Math.floor(rnd() * (b - a + 1));
const chance = (p) => rnd() < p, pick = (a) => a[Math.floor(rnd() * a.length)];
function makeBag(pool) {
  let bag = [];
  return () => {
    if (bag.length === 0) { bag = pool.slice(); for (let i = bag.length - 1; i > 0; i--) { const j = Math.floor(rnd() * (i + 1)); [bag[i], bag[j]] = [bag[j], bag[i]]; } }
    return bag.pop();
  };
}

// ---- two grids: base (terrain, opaque) + ovr (props, transparent, ' '=none) ----
const base = Array.from({ length: H }, () => Array(W).fill('.'));
const ovr  = Array.from({ length: H }, () => Array(W).fill(' '));
const occ  = Array.from({ length: H }, () => Array(W).fill(0));
const inb = (x, y) => x >= 0 && x < W && y >= 0 && y < H;
const setB = (x, y, c) => { if (inb(x, y)) base[y][x] = c; };
const setO = (x, y, c) => { if (inb(x, y)) ovr[y][x] = c; };
const clrO = (x, y) => { if (inb(x, y)) ovr[y][x] = ' '; };
// a cell is "grass" (plantable/decoratable) when its base is plain grass, it has no prop, and it's unreserved.
const isGrass = (x, y) => inb(x, y) && base[y][x] === '.' && ovr[y][x] === ' ';
const free = (x, y) => inb(x, y) && occ[y][x] === 0 && base[y][x] === '.' && ovr[y][x] === ' ';
const areaFree = (x, y, w, h, m = 1) => { for (let yy = y - m; yy < y + h + m; yy++) for (let xx = x - m; xx < x + w + m; xx++) if (!inb(xx, yy) || occ[yy][xx]) return false; return true; };
const reserve = (x, y, w, h) => { for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) if (inb(xx, yy)) occ[yy][xx] = 1; };
const doors = [];
const start = { x: 4, y: (H >> 1) };

// ---- structures (correct multi-tile autotiling; roofs/walls are BASE, props are OVR) ----
function roofBand(x, y, w, red, dormer = false) {
  const rL = red ? '<' : '[', rF = red ? '-' : '_', rR = red ? '>' : ']';
  const eL = red ? '^' : '~', eF = red ? '%' : '`', eR = red ? '&' : '!';
  for (let i = 0; i < w; i++) {
    setB(x + i, y, i === 0 ? rL : i === w - 1 ? rR : rF);
    setB(x + i, y + 1, i === 0 ? eL : i === w - 1 ? eR : eF);
  }
  if (dormer && w >= 3) setB(x + (w >> 1), y, red ? 'v' : 'D');
}
// tall 2-tile tree (canopy top over trunk). Only where the cell ABOVE is free grass. Trunk+canopy = OVR.
function tallTree(x, y, pine) {
  if (!inb(x, y - 1) || !free(x, y - 1) || !free(x, y)) return false;
  setO(x, y - 1, pine ? 'O' : '6'); setO(x, y, pine ? 'o' : '5');
  return true;
}
function house(x, y, w = 3, wallRows = 1) {
  const red = chance(0.5), wood = chance(0.35);
  const le = wood ? 'y' : 'r', re = wood ? 'u' : 'a', win = wood ? 'n' : 'i', dr = wood ? 'e' : 'd', fill = wood ? 'w' : 'q';
  roofBand(x, y, w, red, chance(0.35));
  for (let row = 0; row < wallRows; row++) {
    const wy = y + 2 + row;
    for (let i = 0; i < w; i++) setB(x + i, wy, i === 0 ? le : i === w - 1 ? re : fill);
    for (let i = 2; i < w - 2; i += 3) setB(x + i, wy, win);
  }
  const doorX = x + (w >> 1);
  setB(doorX, y + 2 + wallRows - 1, dr);
  reserve(x, y, w, wallRows + 2); doors.push({ x: doorX, y: y + 2 + wallRows });
  // props by the wall are OVR — they keep the ground (dirt apron) underneath, no green halo.
  if (chance(0.5)) setO(x - 1, y + 2 + wallRows - 1, 'k');
  if (chance(0.4)) setO(x + w, y + 2 + wallRows - 1, chance(0.5) ? 'x' : 'S');
}
function turret(x, y, h) {
  setB(x, y, '1'); setB(x + 1, y, '3');
  for (let r = 1; r < h - 1; r++) { setB(x, y + r, 'l'); setB(x + 1, y + r, 'j'); }
  setB(x, y + h - 1, '7'); setB(x + 1, y + h - 1, '9');
}
function castle(x, y, w, h) {
  const tH = 4;
  turret(x, y - tH, tH); turret(x + w - 2, y - tH, tH);
  for (let i = 2; i < w - 2; i++) setB(x + i, y - 1, i === 2 ? '1' : i === w - 3 ? '3' : '2');
  roofBand(x, y, w, false, true);
  for (let row = 0; row < h; row++) {
    const wy = y + 2 + row;
    for (let i = 0; i < w; i++) setB(x + i, wy, i === 0 ? 'r' : i === w - 1 ? 'a' : 'q');
    // fortress openings on EVERY wall row use the grey-stone arrow-slit ('4') — the WINDOW tile is a
    // red-brick/wood frame that clashes on the castle's grey stone (looked like brown boxes, not windows).
    for (let i = 2; i < w - 2; i += 3) setB(x + i, wy, '4');
  }
  const doorX = x + (w >> 1);
  setB(doorX, y + 2 + h - 1, 'd');
  setO(x - 1, y + 2 + h - 1, 'C');                            // treasure on the ground outside the wall
  reserve(x, y - tH, w, tH + h + 2); doors.push({ x: doorX, y: y + 2 + h });
}
function faro(x, y) { turret(x, y, 3); reserve(x, y, 2, 3); doors.push({ x, y: y + 3 }); }
// fenced flower bed: fence corners/rails are OVR over a flower-grass BASE; gate = a grass gap in the rail.
function garden(x, y, w, h) {
  for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) setB(xx, yy, ',');   // flower-grass bed
  setO(x, y, 'F'); setO(x + w - 1, y, 'P'); setO(x, y + h - 1, 'L'); setO(x + w - 1, y + h - 1, 'K');
  for (let xx = x + 1; xx < x + w - 1; xx++) { setO(xx, y, '='); setO(xx, y + h - 1, '='); }
  for (let yy = y + 1; yy < y + h - 1; yy++) { setO(x, yy, 'I'); setO(x + w - 1, yy, 'I'); }
  clrO(x + (w >> 1), y + h - 1);                              // gate opening in the bottom rail
  reserve(x, y, w, h); doors.push({ x: x + (w >> 1), y: y + h });
}
// open plowed field: tilled-earth BASE ('p', grass-fringed later) with alternating crop rows (bush OVR).
function field(x, y, w, h) {
  for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) if (free(xx, yy)) setB(xx, yy, 'p');
  for (let yy = y + 1; yy < y + h - 1; yy += 2) for (let xx = x + 1; xx < x + w - 1; xx++) if (base[yy][xx] === 'p') setO(xx, yy, 'b');
  reserve(x, y, w, h);
  if (chance(0.7)) setO(x, y, chance(0.5) ? 'k' : 'x');
}
function meadow(cx, cy, rad) {
  for (let y = cy - rad; y <= cy + rad; y++) for (let x = cx - rad; x <= cx + rad; x++) {
    if (Math.hypot(x - cx, y - cy) > rad || !free(x, y)) continue;
    const r = rnd();
    if (r < 0.5) setO(x, y, '*');                 // flowers = overlay on the default grass base
    else setB(x, y, r < 0.75 ? ',' : 'c');
  }
}
// a woodland grove: a radial DENSITY GRADIENT (dense trees at the core, thinning to shrub/sprout/mushroom
// understory at the rim). Clustered groves read as real copses — far more natural than uniform per-tile
// tree scatter, which always looks like evenly-spaced dots. Specialised worldgen: place a few of these.
function grove(cx, cy, rad) {
  for (let y = cy - rad; y <= cy + rad; y++) for (let x = cx - rad; x <= cx + rad; x++) {
    const d = Math.hypot(x - cx, y - cy);
    if (d > rad || !free(x, y)) continue;
    const dens = 1 - d / rad, r = rnd();
    if (r < dens * 0.5) { if (!tallTree(x, y, chance(0.6))) setO(x, y, 'T'); }   // dense canopy at the core
    else if (r < dens * 0.72) setO(x, y, chance(0.5) ? 'T' : 'Y');               // short trees
    else if (r < dens * 0.88) setO(x, y, 'b');                                   // bush understory
    else if (r < dens) setO(x, y, 'm');                                          // mushroom
  }
}
function tryPlace(fn, w, h, z, tries = 40) { for (let t = 0; t < tries; t++) { const x = ri(z.x, z.x + z.w - w), y = ri(z.y + 1, z.y + z.h - h); if (areaFree(x, y - 1, w, h + 1, 1)) { fn(x, y); return true; } } return false; }

// HAUNTED graveyard: paints the zone's open grass to dark crypt-stone, plants rows of headstones, dots
// a few torches, and scatters dead/creepy bits. Ghosts/wraiths/rats are spawned here (see entities). The
// app layers fog + drifting spirits over this zone for the spectral mood.
function graveyard(z) {
  for (let y = z.y; y < z.y + z.h; y++) for (let x = z.x; x < z.x + z.w; x++) {
    if (!inb(x, y) || occ[y][x] || base[y][x] !== '.') continue;
    setB(x, y, chance(0.75) ? 'g' : 'G');                         // mostly smooth crypt (G2 detritus is busy)
  }
  const inCrypt = (x, y) => inb(x, y) && (base[y][x] === 'g' || base[y][x] === 'G') && ovr[y][x] === ' ';
  for (let gy = z.y + 2; gy < z.y + z.h - 2; gy += 3) {           // loose rows of headstones
    let gx = z.x + 2;
    while (gx < z.x + z.w - 2) { if (inCrypt(gx, gy)) setO(gx, gy, 't'); gx += ri(3, 5); }
  }
  // torches ring the perimeter (mark the haunted ground at a glance) + a few inside
  for (let x = z.x; x < z.x + z.w; x += ri(4, 6)) { if (inCrypt(x, z.y)) setO(x, z.y, 'f'); if (inCrypt(x, z.y + z.h - 1)) setO(x, z.y + z.h - 1, 'f'); }
  for (let i = 0; i < 4; i++) { const x = ri(z.x + 1, z.x + z.w - 2), y = ri(z.y + 1, z.y + z.h - 2); if (inCrypt(x, y)) setO(x, y, 'f'); }
  for (let i = 0; i < 8; i++) { const x = ri(z.x + 1, z.x + z.w - 2), y = ri(z.y + 1, z.y + z.h - 2); if (inCrypt(x, y)) setO(x, y, chance(0.5) ? 'm' : 'b'); }
}

// ---- zones (proportional -> scale with the bigger map) ----
const forest = { x: 0, y: 0, w: W, h: Math.floor(H * 0.22) };
const village = { x: 3, y: Math.floor(H * 0.27), w: Math.floor(W * 0.32), h: Math.floor(H * 0.30) };
const castleZ = { x: Math.floor(W * 0.40), y: Math.floor(H * 0.27), w: Math.floor(W * 0.24), h: Math.floor(H * 0.28) };
const faroZ = { x: Math.floor(W * 0.72), y: Math.floor(H * 0.22), w: Math.floor(W * 0.28), h: Math.floor(H * 0.40) };
const hauntZ = { x: 2, y: Math.floor(H * 0.70), w: Math.floor(W * 0.30), h: Math.floor(H * 0.28) };   // cimitero infestato
const farm = { x: Math.floor(W * 0.34), y: Math.floor(H * 0.72), w: Math.floor(W * 0.64), h: Math.floor(H * 0.26) };

// sand beach — the faro peninsula's east coast (a coherent per-row-jittered coastline).
{
  let coast = faroZ.x + Math.floor(faroZ.w * 0.35);
  for (let y = faroZ.y; y < faroZ.y + faroZ.h; y++) {
    coast += ri(-1, 1);
    coast = Math.max(faroZ.x + 3, Math.min(faroZ.x + faroZ.w - 2, coast));
    for (let x = coast; x < W; x++) if (isGrass(x, y)) setB(x, y, chance(0.06) ? '.' : (chance(0.28) ? 's' : 'p'));
  }
}

// place structures
const nHouse = Math.max(5, Math.floor((village.w * village.h) / 80));
for (let i = 0; i < nHouse; i++) {
  const big = chance(0.3), w = big ? 5 : 3, rows = big && chance(0.35) ? 2 : 1;
  tryPlace((x, y) => house(x, y, w, rows), w, rows + 2, village);
}
tryPlace((x, y) => garden(x, y, 6, 5), 6, 5, village);
tryPlace((x, y) => house(x, y, 3, 1), 3, 3, { x: faroZ.x + 2, y: faroZ.y + 2, w: faroZ.w - 4, h: faroZ.h - 6 });
tryPlace((x, y) => castle(x, y + 4, ri(7, 9), ri(2, 3)), 9, 9, castleZ);
tryPlace((x, y) => faro(x, y), 2, 3, { x: faroZ.x + 4, y: faroZ.y + 2, w: faroZ.w - 7, h: faroZ.h - 6 });
tryPlace((x, y) => garden(x, y, 6, 5), 6, 5, farm);
tryPlace((x, y) => garden(x, y, 7, 5), 7, 5, farm);
for (let i = 0; i < 4; i++) { const fw = ri(5, 8), fh = ri(4, 6); tryPlace((x, y) => field(x, y, fw, fh), fw, fh, farm); }

// the haunted graveyard (dark crypt ground + headstones + torches), with a gate the road links to
graveyard(hauntZ);
doors.push({ x: hauntZ.x + (hauntZ.w >> 1), y: hauntZ.y });

// ---- roads: horizontal spine + L-paths from every door + a few bare-earth plazas ----
const spineY = Math.floor(H * 0.5);
const carve1 = (x, y) => { if (free(x, y)) setB(x, y, 'p'); };
const carve = (x, y) => { carve1(x, y); carve1(x + 1, y); carve1(x, y + 1); carve1(x + 1, y + 1); };
for (let x = 2; x < W - 2; x++) carve(x, spineY);
function road(x0, y0, x1, y1) { let x = x0, y = y0; while (y !== y1) { carve(x, y); y += Math.sign(y1 - y); } while (x !== x1) { carve(x, y); x += Math.sign(x1 - x); } carve(x, y); }
for (const d of doors) road(d.x, d.y, d.x, spineY);
carve(start.x, start.y); road(start.x, start.y, start.x, spineY);
const plaza = (x, y, w, h) => { for (let yy = y; yy < y + h; yy++) for (let xx = x; xx < x + w; xx++) carve(xx, yy); };
plaza(village.x + 3, spineY - 5, 6, 4);
plaza(Math.floor(W * 0.30), spineY + 2, 5, 4);
plaza(farm.x + 4, farm.y + 1, 6, 4);
for (const d of doors) plaza(d.x - 1, d.y, 3, 2);

// ---- minecart track: a winding line with real CORNERS (rail is OVR over a sand BASE) ----
const CORNER = { ES: '(', WS: ')', EN: '{', WN: '}' };
function railPath(pts) {
  const lay = (x, y, c) => { if (free(x, y)) { setB(x, y, 's'); setO(x, y, c); } };
  for (let i = 0; i < pts.length - 1; i++) {
    let [x0, y0] = pts[i]; const [x1, y1] = pts[i + 1];
    const dx = Math.sign(x1 - x0), dy = Math.sign(y1 - y0);
    let x = x0, y = y0;
    while (x !== x1 || y !== y1) { if (!(i > 0 && x === x0 && y === y0)) lay(x, y, dx ? 'R' : 'V'); x += dx; y += dy; }
  }
  for (let i = 1; i < pts.length - 1; i++) {
    const [px, py] = pts[i - 1], [cx, cy] = pts[i], [nx, ny] = pts[i + 1];
    const inx = Math.sign(cx - px), iny = Math.sign(cy - py), outx = Math.sign(nx - cx), outy = Math.sign(ny - cy);
    const open = new Set();
    open.add(inx === 1 ? 'W' : inx === -1 ? 'E' : iny === 1 ? 'N' : 'S');
    open.add(outx === 1 ? 'E' : outx === -1 ? 'W' : outy === 1 ? 'S' : 'N');
    const key = (open.has('E') ? 'E' : 'W') + (open.has('S') ? 'S' : 'N');
    if (free(cx, cy)) { setB(cx, cy, 's'); setO(cx, cy, CORNER[key]); }
  }
}
const ry = Math.floor(H * 0.86), rx0 = 4, rx1 = W - 5, dip = 5;
railPath([[rx0, ry], [Math.floor(W * 0.28), ry], [Math.floor(W * 0.28), ry + dip], [Math.floor(W * 0.55), ry + dip], [Math.floor(W * 0.55), ry], [rx1, ry]]);

// ---- autotile the dirt: grass-edge fringe (BASE layer only; overlays untouched) ----
const GRASSY = new Set(['.', ',', '@', 'c', 'A']);   // grass-family BASE tiles (flowers are an overlay now)
const grassy = (x, y) => inb(x, y) && GRASSY.has(base[y][x]);
function fringeTan(fill) {
  const cells = [];
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (base[y][x] === fill) cells.push([x, y]);
  for (const [x, y] of cells) {
    const n = grassy(x, y - 1), s = grassy(x, y + 1), w = grassy(x - 1, y), e = grassy(x + 1, y);
    let c = fill;
    if ((n && s) || (e && w)) c = fill;
    else if (n && w) c = 'Q'; else if (n && e) c = 'E'; else if (s && w) c = 'Z'; else if (s && e) c = 'B';
    else if (n) c = 'U'; else if (s) c = 'X'; else if (w) c = 'N'; else if (e) c = 'M';
    base[y][x] = c;
  }
}
fringeTan('p');
fringeTan('s');

// ---- decoration (only free grass) ----
for (let y = forest.y; y < forest.y + forest.h; y++) {
  const depth = 1 - (y - forest.y) / forest.h;
  const dens = 0.10 + 0.22 * depth;
  for (let x = 0; x < W; x++) if (free(x, y)) {
    const r = rnd();
    if (r < dens * 0.35) tallTree(x, y, true);
    else if (r < dens * 0.6) tallTree(x, y, false);
    else if (r < dens * 0.75) setO(x, y, 'T'); else if (r < dens * 0.85) setO(x, y, 'Y');
    else if (r < dens) setO(x, y, 'b');
    else if (r < dens + 0.05) setO(x, y, 'b');                       // bush understory
    else if (r < dens + 0.07) setO(x, y, 'm');
  }
}
function clearing(cx, cy, rad) {
  for (let y = cy - rad; y <= cy + rad; y++) for (let x = cx - rad; x <= cx + rad; x++) {
    const d = Math.hypot(x - cx, y - cy);
    if (d > rad || !inb(x, y) || occ[y][x]) continue;
    if (d > rad - 1.1) { setB(x, y, '.'); if (chance(0.6)) setO(x, y, 'm'); else clrO(x, y); }   // mushroom ring
    else if (chance(0.35)) { setB(x, y, '.'); setO(x, y, '*'); }                                  // flowery centre
    else { setB(x, y, ','); clrO(x, y); }
  }
}
tryPlace((x, y) => clearing(x + 3, y + 3, 3), 7, 7, forest);
const openCountry = { x: 4, y: forest.h + 2, w: W - 8, h: farm.y - forest.h - 4 };
for (let i = 0; i < 3; i++) { const r = ri(2, 4); tryPlace((x, y) => meadow(x + r, y + r, r), r * 2 + 1, r * 2 + 1, openCountry); }
// scattered COPSES across the open country — clustered woodland instead of lone evenly-spaced trees.
for (let i = 0; i < 5; i++) { const r = ri(3, 6); tryPlace((x, y) => grove(x + r, y + r, r), r * 2 + 1, r * 2 + 1, openCountry); }
const scatterB = (c, p) => { for (let y = forest.h; y < H; y++) for (let x = 0; x < W; x++) if (free(x, y) && chance(p)) setB(x, y, c); };
const scatterO = (c, p) => { for (let y = forest.h; y < H; y++) for (let x = 0; x < W; x++) if (free(x, y) && chance(p)) setO(x, y, c); };
scatterB(',', 0.05); scatterB('c', 0.02); scatterB('A', 0.015);
scatterO('*', 0.02); scatterO('m', 0.014); scatterO('b', 0.028); scatterO('T', 0.012); scatterO('Y', 0.009);
for (let y = forest.h; y < H; y++) for (let x = 0; x < W; x++) if (chance(0.01) && free(x, y)) tallTree(x, y, chance(0.5));
let chests = 0, guard = 0; while (chests < Math.floor((W * H) / 900) + 4 && guard++ < 5000) { const x = ri(1, W - 2), y = ri(1, H - 2); if (free(x, y)) { setO(x, y, 'C'); chests++; } }
setB(start.x, start.y, '@'); setO(start.x + 1, start.y, 'S');

// ---- entities ----
const ENT = [];
// solid BASE chars: buildings (roofs/walls/windows/doors) block movement; grounds don't. Single source
// of truth, reused by both the walkable-spawn check and the emitted SOLb table.
const SOLID_BASE = new Set(['<','-','>','^','%','&','[','_',']','~','`','!','i','n','r','a','q','y','u','w','D','v','1','3','l','j','7','9','2','4']);
const isWalkCell = (x, y) => inb(x, y) && ovr[y][x] === ' ' && !SOLID_BASE.has(base[y][x]);
const spawnCell = (z) => { for (let t = 0; t < 80; t++) { const x = ri(z.x, z.x + z.w - 1), y = ri(z.y, z.y + z.h - 1); if (isWalkCell(x, y) && base[y][x] !== '@') return { x, y }; } return null; };
function addEnt(draw, z, kind, lines) { const p = spawnCell(z); if (!p) return; const sprite = typeof draw === 'function' ? draw() : pick(draw); ENT.push({ tx: p.x, ty: p.y, axis: chance(0.5) ? 0 : 1, span: ri(3, 7), sprite: IDX[sprite], kind, line: pick(lines) }); }
const npcLines = ["Benvenuto a Cardler!\nEsplora il villaggio.", "Il castello nasconde un\ntesoro, ma occhio ai mostri.", "Compro pelli di lupo.\nTorna con dell'oro.", "Hai visto il mio gatto?\nEra qui un attimo fa...", "Accendi il faro a est,\nle orde premono!", "Riposati, poi parti\nall'avventura.", "A sud c'e' un cimitero...\nnon andarci da solo.", "I ratti rubano il grano,\nma sono innocui."];
const beastLines = ["Grrr...", "Un ringhio nell'ombra.", "Sssss...", "Occhi rossi nel buio."];
const ghostLines = ["Uuuuuh...", "Un freddo spettrale\nti attraversa.", "...riposa con noi...", "Ossa e silenzio."];
const animLines  = ["Squit!", "*annusa*", "Squit squit!"];
const villageBag = makeBag(['VILL', 'KID', 'PRIN', 'OLD', 'WIZ', 'VIKING', 'WITCH']);
for (let i = 0; i < 9; i++) addEnt(villageBag, village, 0, npcLines);
addEnt(['OLD', 'WIZ', 'VIKING'], faroZ, 0, npcLines);
const forestBag = makeBag(['WOLF', 'SPIDER', 'GHOST']);
for (let i = 0; i < 5; i++) addEnt(forestBag, forest, 1, beastLines);
const castleBag = makeBag(['WRAITH', 'DEMON', 'BARB']);
for (let i = 0; i < 4; i++) addEnt(castleBag, castleZ, 1, beastLines);
const farmBag = makeBag(['WOLF', 'SPIDER']);
for (let i = 0; i < 3; i++) addEnt(farmBag, farm, 1, beastLines);
// HAUNTED zone: dense ghosts + wraiths (spectral infestation) — kind=1 (they harm)
const hauntBag = makeBag(['GHOST', 'WRAITH', 'GHOST', 'WRAITH', 'DEMON']);
for (let i = 0; i < 8; i++) addEnt(hauntBag, hauntZ, 1, ghostLines);
// wandering FAUNA: rats (kind=2 = neutral animal — wanders, doesn't harm, doesn't block)
for (let i = 0; i < 3; i++) addEnt(['RAT'], village, 2, animLines);
for (let i = 0; i < 3; i++) addEnt(['RAT'], farm, 2, animLines);
addEnt(['RAT'], hauntZ, 2, animLines);

// ---- validate + emit ----
for (let y = 0; y < H; y++) {
  if (base[y].length !== W || ovr[y].length !== W) throw new Error(`row ${y} len`);
  for (const c of base[y]) if (!CB[c]) throw new Error(`unknown base char '${c}' at row ${y}`);
  for (const c of ovr[y]) if (c !== ' ' && !CO[c]) throw new Error(`unknown ovr char '${c}' at row ${y}`);
}
const TIDb = Array(128).fill(IDX.GRASS), SOLb = Array(128).fill(0);
const TIDo = Array(128).fill(255), SOLo = Array(128).fill(0), FLPo = Array(128).fill(0);
for (const [c, t] of Object.entries(CB)) { const cc = c.charCodeAt(0); TIDb[cc] = IDX[t]; if (SOLID_BASE.has(c)) SOLb[cc] = 1; }
for (const [c, v] of Object.entries(CO)) { const cc = c.charCodeAt(0); TIDo[cc] = IDX[v.o]; if (v.s) SOLo[cc] = 1; if (v.fl) FLPo[cc] = v.fl; }
const esc = (s) => s.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n');
const row8 = (a) => { let o = ''; for (let i = 0; i < 128; i += 16) o += '    ' + a.slice(i, i + 16).join(',') + ',\n'; return o; };

let out = `// AUTO-GENERATED by assets/cardler/worldgen.mjs  (seed=${SEED}, ${W}x${H}).  Do not edit by hand.
// Re-generate:  node assets/cardler/build_atlas.mjs && node assets/cardler/worldgen.mjs ${SEED} ${W} ${H}
// TWO LAYERS: BASE (opaque terrain) + OVR (transparent props, ' '=none, drawn keyed on top of BASE).
#pragma once
#include <stdint.h>

enum { ${NAMES.join(', ')}, C_N };

#define MAP_W ${W}
#define MAP_H ${H}
static const char *const BASE[MAP_H] = {
`;
for (let y = 0; y < H; y++) out += `    "${base[y].join('')}",\n`;
out += `};
static const char *const OVR[MAP_H] = {
`;
for (let y = 0; y < H; y++) out += `    "${esc(ovr[y].join(''))}",\n`;
out += `};

// base char -> opaque ground tile + solidity (buildings block).
static const uint8_t TIDb[128] = {\n${row8(TIDb)}};
static const uint8_t SOLb[128] = {\n${row8(SOLb)}};
// overlay char -> keyed prop tile (255=none) + solidity + flip (bit0=H mirror, bit1=V mirror).
static const uint8_t TIDo[128] = {\n${row8(TIDo)}};
static const uint8_t SOLo[128] = {\n${row8(SOLo)}};
static const uint8_t FLPo[128] = {\n${row8(FLPo)}};

typedef struct { int16_t tx, ty; uint8_t axis, span, sprite, kind; const char *line; } CardlerEnt;
#define NENT ${ENT.length}
static const CardlerEnt ENT[NENT] = {
`;
for (const e of ENT) out += `    { ${e.tx}, ${e.ty}, ${e.axis}, ${e.span}, ${e.sprite}, ${e.kind}, "${esc(e.line)}" },\n`;
out += `};\n`;

writeFileSync(new URL('../../firmware/components/nucleo_app/cardler_world.h', import.meta.url), out);
console.log(`cardler_world.h  ${W}x${H} seed=${SEED}  houses~${nHouse}  chests=${chests}  entities=${ENT.length}`);

// --preview: composite the whole map (base + keyed overlay, exactly as the device draws) to a raw RGBA
// dump for a host-side visual check — the fast way to confirm props keep their terrain (no green halos).
if (process.argv.includes('--preview')) {
  const IMGW = W * 16, IMGH = H * 16, img = Buffer.alloc(IMGW * IMGH * 4);
  const blit = (tid, px, py, keyed) => {
    for (let ty = 0; ty < 16; ty++) for (let tx = 0; tx < 16; tx++) {
      const s = tid * 1024 + (ty * 16 + tx) * 4;
      if (keyed && ATL[s + 3] < 128) continue;
      const d = ((py + ty) * IMGW + (px + tx)) * 4;
      img[d] = ATL[s]; img[d + 1] = ATL[s + 1]; img[d + 2] = ATL[s + 2]; img[d + 3] = 255;
    }
  };
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    blit(TIDb[base[y][x].charCodeAt(0)], x * 16, y * 16, false);
    const o = ovr[y][x]; if (o !== ' ') { const ot = TIDo[o.charCodeAt(0)]; if (ot !== 255) blit(ot, x * 16, y * 16, true); }
  }
  writeFileSync(new URL('./preview.rgba', import.meta.url), img);
  console.log(`preview.rgba ${IMGW}x${IMGH}`);
}

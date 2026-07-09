// test_scene.mjs — compose a test scene from the atlas to eyeball house/castle/tree autotiling.
import { readFileSync, writeFileSync } from 'node:fs';
const names = JSON.parse(readFileSync(new URL('./atlas_enum.json', import.meta.url)));
const I = {}; names.forEach((n, i) => I[n.replace('C_', '')] = i);
const atlas = readFileSync(new URL('./atlas_v.rgba', import.meta.url));
const tile = i => atlas.subarray(i * 1024, i * 1024 + 1024);
const GW = 26, GH = 16, IW = GW * 16, IH = GH * 16;
const img = Buffer.alloc(IW * IH * 4);
// grass base
for (let p = 0; p < GW * GH; p++) { const tx = p % GW, ty = (p / GW) | 0; blit(I.GRASS, tx, ty, false); }
function blit(tid, tx, ty, keyed) {
  const s = tile(tid), px = tx * 16, py = ty * 16;
  for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) {
    const a = s[(y * 16 + x) * 4 + 3]; if (keyed && a < 128) continue;
    const di = ((py + y) * IW + (px + x)) * 4;
    img[di] = s[(y * 16 + x) * 4]; img[di + 1] = s[(y * 16 + x) * 4 + 1]; img[di + 2] = s[(y * 16 + x) * 4 + 2]; img[di + 3] = 255;
  }
}
// house 3x3 at (x,y): grey or red roof
function house(x, y, grey) {
  const rl = grey ? I.GROOFL : I.ROOFL, rm = grey ? I.GROOFM : I.ROOFM, rr = grey ? I.GROOFR : I.ROOFR, pk = grey ? I.GPEAK : I.PEAK;
  blit(pk, x + 1, y - 1, true);                                    // gable peak on top-centre
  blit(rl, x, y, false); blit(rm, x + 1, y, false); blit(rr, x + 2, y, false);
  blit(I.CL, x, y + 1, false); blit(I.WINDOW, x + 1, y + 1, false); blit(I.CR, x + 2, y + 1, false);
  blit(I.CL, x, y + 2, false); blit(I.DOOR, x + 1, y + 2, false); blit(I.CR, x + 2, y + 2, false);
}
// castle w x h at (x,y): 9-slice ring + 2-wide gate centred on bottom
function castle(x, y, w, h) {
  for (let yy = 0; yy < h; yy++) for (let xx = 0; xx < w; xx++) {
    const L = xx === 0, R = xx === w - 1, Tp = yy === 0, B = yy === h - 1;
    let t = I.CF;
    if (Tp && L) t = I.CTL; else if (Tp && R) t = I.CTR; else if (Tp) t = I.CT;
    else if (B && L) t = I.CBL; else if (B && R) t = I.CBR; else if (B) t = I.CB;
    else if (L) t = I.CL; else if (R) t = I.CR;
    blit(t, x + xx, y + yy, false);
  }
  const gx = x + ((w >> 1) - 1);                                   // 2-wide gate
  blit(I.GCAP, gx, y + h - 3, false); blit(I.GCAP, gx + 1, y + h - 3, false);
  blit(I.GARCHL, gx, y + h - 2, false); blit(I.GARCHR, gx + 1, y + h - 2, false);
  blit(I.GATEL, gx, y + h - 1, false); blit(I.GATER, gx + 1, y + h - 1, false);
}
function faro(x, y) { blit(I.CTL, x, y, false); blit(I.CTR, x + 1, y, false); blit(I.CL, x, y + 1, false); blit(I.CR, x + 1, y + 1, false); blit(I.CBL, x, y + 2, false); blit(I.CBR, x + 1, y + 2, false); }

house(1, 2, true);
house(6, 2, false);
castle(11, 2, 6, 5);
faro(19, 2);
// trees + props row
blit(I.PINE, 1, 8, true); blit(I.AUTUMN, 3, 8, true); blit(I.BUSH, 5, 8, true); blit(I.MUSH, 7, 8, true);
blit(I.SIGN, 9, 8, true); blit(I.BARREL, 11, 8, true); blit(I.CRATE, 13, 8, true); blit(I.BARGATE, 15, 8, true);
blit(I.FENCE, 17, 8, true); blit(I.FPOST, 19, 8, true);
// characters row
['HERO', 'WIZ', 'VILL', 'KID', 'PRIN', 'OLD', 'WOLF', 'SPIDER', 'GHOST', 'WRAITH', 'DEMON', 'BARB'].forEach((n, i) => blit(I[n], 1 + i * 2, 11, true));
// rail run
for (let i = 0; i < 10; i++) { blit(I.SAND, 1 + i, 14, false); blit(I.RAIL, 1 + i, 14, true); }

writeFileSync(new URL('./_scene.rgba', import.meta.url), img);
console.log(`${IW}x${IH}`);

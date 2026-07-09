// Render cardler_world.h to raw RGBA using atlas_v.rgba + the emitted TID/OVL tables. node render_preview.mjs
import { readFileSync, writeFileSync } from 'node:fs';
const hdr = readFileSync(new URL('../../firmware/components/nucleo_app/cardler_world.h', import.meta.url), 'utf8');
const W = +hdr.match(/#define MAP_W (\d+)/)[1], H = +hdr.match(/#define MAP_H (\d+)/)[1];
const MAP = [...hdr.matchAll(/^ {4}"(.*)",$/gm)].map(m => m[1]).slice(0, H);
const arr = (name) => { const body = hdr.match(new RegExp(`${name}\\[128\\] = \\{([\\s\\S]*?)\\};`))[1]; return body.split(',').map(s => s.trim()).filter(s => s.length).map(Number); };
const TID = arr('TID'), OVL = arr('OVL'), FLP = arr('FLP');
const atlas = readFileSync(new URL('./atlas_v.rgba', import.meta.url));
const tile = i => atlas.subarray(i * 1024, i * 1024 + 1024);
const IW = W * 16, IH = H * 16, img = Buffer.alloc(IW * IH * 4);
const put = (tid, px, py, keyed, fl = 0) => { const s = tile(tid); for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) { const sx = (fl & 1) ? 15 - x : x, sy = (fl & 2) ? 15 - y : y; const a = s[(sy*16+sx)*4+3]; if (keyed && a < 128) continue; const di = ((py+y)*IW+(px+x))*4; img[di]=s[(sy*16+sx)*4]; img[di+1]=s[(sy*16+sx)*4+1]; img[di+2]=s[(sy*16+sx)*4+2]; img[di+3]=255; } };
for (let ty = 0; ty < H; ty++) for (let tx = 0; tx < W; tx++) {
  const cc = MAP[ty].charCodeAt(tx), px = tx*16, py = ty*16;
  put(TID[cc], px, py, false);
  if (OVL[cc] !== 255) put(OVL[cc], px, py, true, FLP[cc]);
}
writeFileSync(new URL('./_worldmap.rgba', import.meta.url), img);
console.log(`${IW}x${IH}`);

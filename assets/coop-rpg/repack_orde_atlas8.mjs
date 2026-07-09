// repack_orde_atlas8.mjs — converts the existing RGB565LE Orde atlas to 8bpp RGB332, matching the
// Cardler atlas format (assets/cardler/build_atlas.mjs) so both tile-based games share the device
// canvas's own bit depth: half the atlas RAM (256 B/tile vs 512) and a straight pushImage memcpy
// blit instead of a per-pixel 565->332 convert every frame.
//
// Repacks the ALREADY-CROPPED, already-correct art in place — no re-crop from the source Kenney
// sheets, so there's no risk of picking the wrong (col,row) and silently swapping sprite art. Each
// 565 pixel is truncated to its top RRR/GGG/BB bits (same precision-truncation style as Cardler's
// pack(), not rounded) and 565-magenta (0xF81F) maps to the exact same 332-magenta key Cardler uses
// (0xE3), so app_vs.cpp only needs a type/constant swap, not new key-detection logic.
//   node repack_orde_atlas8.mjs
import { readFileSync, writeFileSync } from 'node:fs';

const PATH = new URL('./orde_atlas.bin', import.meta.url);
const src = readFileSync(PATH);
const TKEY = 0xE3;                              // color332(255,0,255) — same key as Cardler

if (src.length % 2 !== 0) throw new Error('orde_atlas.bin: odd length, not RGB565LE');
const n = src.length / 2;                       // total pixels (tiles * 256)
const out = Buffer.alloc(n);
for (let i = 0; i < n; i++) {
  const px = src.readUInt16LE(i * 2);
  if (px === 0xF81F) { out[i] = TKEY; continue; }             // 565 magenta -> 332 magenta
  let c = (((px >> 13) & 7) << 5) | (((px >> 8) & 7) << 2) | ((px >> 2) & 3);
  if (c === TKEY) c = 0xE7;                                    // nudge a real-magenta opaque pixel off the key
  out[i] = c;
}
writeFileSync(PATH, out);
console.log(`orde_atlas.bin repacked: ${src.length} B (RGB565) -> ${out.length} B (RGB332 8bpp), ${n / 256} tiles`);

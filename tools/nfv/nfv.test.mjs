// Tests for the browser .nfv v4 writer (apps/video-studio/www/nfv.js) and the browser reader
// (apps/video-player/www/nfv4.js). Pure Node (Blob is global in Node 18+).
//   node tools/nfv/nfv.test.mjs [clip.nfv]
// With a clip made by tools/nfv/nfv4.py, the writer must rebuild it BYTE FOR BYTE from its own frames, masks
// and audio blocks: the two encoders then cannot drift apart. Without one, a synthetic clip is round-tripped.
import { readFileSync, existsSync } from 'node:fs';
import { buildNfv4, splitMjpeg, wavAdpcmBlocks, stripMask } from '../../apps/video-studio/www/nfv.js';
import { parseHeader, imaDecodeBlock } from '../../apps/video-player/www/nfv4.js';

let failed = 0;
const ok = (c, m) => { if (!c) { console.error('  ✗ ' + m); failed++; } else console.log('  ✓ ' + m); };

function chunks(buf, h) {
  const out = [];
  for (let p = 64; p + 12 <= h.indexOff;) {
    const dv = new DataView(buf.buffer, buf.byteOffset + p, 12);
    const len = dv.getUint32(4, true);
    out.push({ type: buf[p], mask: dv.getUint16(2, true), stamp: dv.getUint32(8, true), off: p, data: buf.subarray(p + 12, p + 12 + len) });
    p += 12 + len;
  }
  return out;
}

// ---- 1. byte-exact against the Python reference encoder ----
const ref = process.argv[2] || 'tools/nfv/out/Test NFV4 - Una Fantastica Gita.nfv';
if (existsSync(ref)) {
  const f = new Uint8Array(readFileSync(ref));
  const h = parseHeader(f.subarray(0, 64));
  const cs = chunks(f, h);
  const v = cs.filter((c) => c.type === 1), a = cs.filter((c) => c.type === 2);
  const { blob } = buildNfv4(v.map((c) => c.data), {
    fps: h.fps, width: h.w, height: h.h, visW: h.visW, visH: h.visH, masks: v.map((c) => c.mask),
    audio: a.length ? { blocks: a.map((c) => c.data), block: h.block, spb: h.spb, rate: h.rate } : null,
  });
  const out = new Uint8Array(await blob.arrayBuffer());
  let same = out.length === f.length;
  for (let i = 0; same && i < f.length; i++) if (out[i] !== f[i]) { same = false; console.error('    first difference at byte ' + i); }
  ok(same, `rebuilds ${ref.split('/').pop()} byte for byte (${v.length} frames, ${a.length} audio blocks)`);
} else console.log('  - no reference clip, byte-exact check skipped');

// ---- 2. synthetic round trip ----
function jpeg(n, pad) { const b = [0xff, 0xd8]; for (let i = 0; i < pad; i++) b.push((n + i) & 0x7f); b.push(0xff, 0xd9); return new Uint8Array(b); }
const FPS = 15, N = 47;
const frames = Array.from({ length: N }, (_, i) => jpeg(i, 20 + (i % 9)));
frames[3] = new Uint8Array([...jpeg(3, 5), 0xaa, 0xbb]);                 // trailing bytes after EOI are trimmed
// a 16 kHz ADPCM track: 4-byte preamble + 252 data bytes = 505 samples a block
const blocks = Array.from({ length: Math.ceil(N / FPS * 16000 / 505) }, (_, i) => { const b = new Uint8Array(256); b[0] = i; b[2] = 10; b.fill(0x17, 4); return b; });
const { blob } = buildNfv4(frames, { fps: FPS, width: 320, height: 240, audio: { blocks, block: 256, spb: 505, rate: 16000 } });
const buf = new Uint8Array(await blob.arrayBuffer());
const h = parseHeader(buf.subarray(0, 64));
ok(h.version === 4 && h.flags === 3, 'v4, audio + strip-mask flags');
ok(h.w === 320 && h.h === 240 && h.visW === 320 && h.visH === 240 && h.fps === FPS, 'geometry');
ok(h.frames === N && h.durationMs === Math.floor(N * 1000 / FPS), 'frame count and duration');
ok(h.rate === 16000 && h.codec === 1 && h.block === 256 && h.spb === 505, 'audio fields');
const cs = chunks(buf, h);
const vs = cs.filter((c) => c.type === 1), as = cs.filter((c) => c.type === 2);
ok(vs.length === N && as.length === blocks.length, 'every frame and every audio block written once');
ok(vs[3].data.length === 9 && vs[3].data[8] === 0xd9, 'trailing bytes after EOI trimmed');
ok(h.maxJpeg === Math.max(...vs.map((c) => c.data.length)), 'max_jpeg is the largest frame');
ok(vs.every((c, i) => c.stamp === i && c.mask === 0xffff), 'video stamps; no masks given = every strip');
// audio leads the picture: the block for time t is in the file before the frame at t - LEAD
let lead = true;
for (const c of vs) { const t = c.stamp / FPS + 0.25; const need = as.filter((x) => x.stamp < t * 16000); if (need.some((x) => x.off > c.off)) lead = false; }
ok(lead, 'audio written LEAD ahead of its frames');
const dv = new DataView(buf.buffer, buf.byteOffset + h.indexOff);
const idx = Array.from({ length: h.indexCount }, (_, i) => [dv.getUint32(i * 8, true), dv.getUint32(i * 8 + 4, true)]);
ok(idx.length === Math.ceil(N / FPS) && idx.every(([fi], i) => fi === i * FPS), 'one index point a second');
ok(idx.every(([fi, off]) => { const fr = vs[fi]; const a = as.filter((x) => x.stamp <= fi / FPS * 16000).pop(); return off <= fr.off && (!a || off <= a.off); }),
  'index points start early enough for the frame AND its audio');

// ---- 3. pieces ----
const concat = new Uint8Array(frames.slice(4).reduce((n, f) => n + f.length, 0));
let o = 0; for (const f of frames.slice(4)) { concat.set(f, o); o += f.length; }
ok(splitMjpeg(concat).length === N - 4, 'splitMjpeg recovers every frame');
const wav = (() => {
  const data = new Uint8Array(512).fill(0x21), w = new Uint8Array(12 + 28 + 8 + 512), d = new DataView(w.buffer);
  w.set([82, 73, 70, 70], 0); d.setUint32(4, w.length - 8, true); w.set([87, 65, 86, 69, 102, 109, 116, 32], 8);
  d.setUint32(16, 20, true); d.setUint16(20, 0x11, true); d.setUint16(22, 1, true); d.setUint32(24, 16000, true);
  d.setUint32(28, 8110, true); d.setUint16(32, 256, true); d.setUint16(34, 4, true); d.setUint16(36, 2, true); d.setUint16(38, 505, true);
  w.set([100, 97, 116, 97], 40); d.setUint32(44, 512, true); w.set(data, 48); return w;
})();
const wb = wavAdpcmBlocks(wav);
ok(wb.blocks.length === 2 && wb.block === 256 && wb.spb === 505 && wb.rate === 16000, 'WAV ADPCM blocks parsed');
ok(imaDecodeBlock(blocks[0]).length === 505, 'ADPCM block decodes to 505 samples');
const W = 32, H = 32, still = new Uint8ClampedArray(W * H * 4).fill(90), shown = [null, null];
ok(stripMask(shown, still, W, H, 0, 30) === 3, 'first frame sends every strip');
ok(stripMask(shown, still, W, H, 1, 30) === 0, 'an unchanged frame sends nothing');
const moved = still.slice(); for (let i = 16 * W * 4; i < 16 * W * 4 + 40 * 4; i++) moved[i] = 200;
ok(stripMask(shown, moved, W, H, 2, 30) === 2, 'local motion in strip 1 sends strip 1 only');

console.log(failed ? `\n${failed} FAILED` : '\nall passed');
process.exit(failed ? 1 : 0);

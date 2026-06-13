// Round-trip test for the browser .nfv writer (apps/video-studio/www/nfv.js): build an .nfv
// from synthetic JPEG frames, parse the bytes back, and assert the on-disk layout matches what
// the device player / studio.mjs expect. Pure Node (Blob is global in Node 18+).
//   node tools/nfv/nfv.test.mjs
import { buildNfv, splitMjpeg } from '../../apps/video-studio/www/nfv.js';

let failed = 0;
const ok = (c, m) => { if (!c) { console.error('  ✗ ' + m); failed++; } else console.log('  ✓ ' + m); };

// Synthetic JPEG-ish frames: SOI (FF D8) … EOI (FF D9), varying sizes.
function frame(n, pad) {
  const a = [0xff, 0xd8];
  for (let i = 0; i < pad; i++) a.push((n + i) & 0xff);
  a.push(0xff, 0xd9);
  return new Uint8Array(a);
}
const FPS = 20;
const frames = Array.from({ length: 130 }, (_, i) => frame(i, 10 + (i % 7)));
// One frame with trailing garbage after EOI → must be trimmed by buildNfv.
const dirty = new Uint8Array([...frame(99, 5), 0xaa, 0xbb, 0xcc]);
const clean = frame(99, 5);
frames[5] = dirty;

const blob = buildNfv(frames, { fps: FPS, hasAudio: true });
const buf = new Uint8Array(await blob.arrayBuffer());
const dv = new DataView(buf.buffer);

// ---- header ----
ok(String.fromCharCode(buf[0], buf[1], buf[2], buf[3]) === 'NFV1', "magic 'NFV1'");
ok(dv.getUint8(4) === 2, 'version 2');
ok(dv.getUint8(5) === (1 | 2), 'flags = audio|indexed (3)');
ok(dv.getUint16(6, true) === 240, 'width 240');
ok(dv.getUint16(8, true) === 136, 'height 136');
ok(dv.getUint16(10, true) === FPS, 'fps 20');
const stride = dv.getUint16(12, true);
ok(stride === 60, 'index_stride = fps*3 = 60');
ok(dv.getUint32(14, true) === frames.length, `frame_count = ${frames.length}`);
ok(dv.getUint32(18, true) === Math.round((frames.length * 1000) / FPS), 'duration_ms');
const idxOff = dv.getUint32(26, true);

// ---- walk the frame chain from offset 32, collect offsets, compare frames ----
let pos = 32, maxFrame = 0;
const seenOffsets = [];
const expected = frames.map((f, i) => (i === 5 ? clean : f));   // frame 5 trimmed
for (let i = 0; i < frames.length; i++) {
  if (i % stride === 0) seenOffsets.push(pos);
  const sz = dv.getUint32(pos, true); pos += 4;
  const jpeg = buf.subarray(pos, pos + sz); pos += sz;
  if (sz > maxFrame) maxFrame = sz;
  const exp = expected[i];
  let same = jpeg.length === exp.length;
  for (let k = 0; same && k < sz; k++) same = jpeg[k] === exp[k];
  if (i === 5 || i === 0 || i === frames.length - 1) ok(same, `frame ${i} bytes match (sz ${sz})`);
  else if (!same) ok(false, `frame ${i} bytes mismatch`);
}
ok(pos === idxOff, `chain ends exactly at index_offset (${pos} === ${idxOff})`);
ok(dv.getUint32(22, true) === maxFrame, `max_frame = ${maxFrame}`);

// ---- index at EOF ----
const idxCount = dv.getUint32(idxOff, true);
ok(idxCount === seenOffsets.length, `index has ${seenOffsets.length} offsets`);
let idxOnDisk = [];
for (let i = 0; i < idxCount; i++) idxOnDisk.push(dv.getUint32(idxOff + 4 + i * 4, true));
ok(JSON.stringify(idxOnDisk) === JSON.stringify(seenOffsets), 'index offsets match walked positions');
ok(idxOff + 4 + idxCount * 4 === buf.length, 'index is the last thing in the file (no trailing bytes)');

// ---- splitMjpeg round-trip ----
const concat = new Uint8Array(frames.reduce((a, f) => a + f.length, 0));
let o = 0; for (const f of frames) { concat.set(f, o); o += f.length; }
const split = splitMjpeg(concat);
ok(split.length === frames.length, `splitMjpeg recovered ${frames.length} frames`);
let splitOk = true;
for (let i = 0; i < frames.length; i++) {
  if (split[i].length !== frames[i].length) { splitOk = false; break; }
  for (let k = 0; k < frames[i].length; k++) if (split[i][k] !== frames[i][k]) { splitOk = false; break; }
}
ok(splitOk, 'splitMjpeg frames are byte-identical to inputs');

console.log(failed ? `\nFAILED: ${failed} assertion(s)` : '\nAll assertions passed.');
process.exit(failed ? 1 : 0);

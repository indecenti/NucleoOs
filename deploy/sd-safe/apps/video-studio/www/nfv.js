// Build a NucleoOS .nfv (v2, indexed) in the browser — the same on-disk format the device
// player and tools/nfv/studio.mjs (makeNfvWriter) produce. Header layout (32 bytes, LE):
//   'NFV1'@0 · version u8@4=2 · flags u8@5=(audio?1:0)|2 · w u16@6 · h u16@8 · fps u16@10 ·
//   index_stride u16@12 · frame_count u32@14 · duration_ms u32@18 · max_frame u32@22 · index_off u32@26
// then [u32 size][JPEG] frames, then at EOF [u32 count][u32 offsets…] (sparse, every ~3 s).
// Pure data, no DOM — runs in the browser AND in Node (for tests).

// Split a buffer of concatenated JPEGs (ffmpeg image2pipe output) into individual frames at
// SOI (FF D8) boundaries. ffmpeg MJPEG carries no EXIF thumbnails, so SOI is unambiguous.
export function splitMjpeg(buf) {
  const frames = [];
  let start = -1;
  for (let i = 0; i + 1 < buf.length; i++) {
    if (buf[i] === 0xff && buf[i + 1] === 0xd8) {
      if (start >= 0) frames.push(buf.subarray(start, i));
      start = i; i++;
    }
  }
  if (start >= 0 && start < buf.length) frames.push(buf.subarray(start));
  return frames;
}

// frames: array of Uint8Array (each a complete JPEG). Returns a Blob (the .nfv).
export function buildNfv(frames, { fps, hasAudio = false, width = 240, height = 136 } = {}) {
  const stride = Math.max(1, Math.round(fps * 3));
  const offsets = [];
  const body = [];
  let pos = 32, count = 0, maxFrame = 0;
  for (let f of frames) {
    const e = f.lastIndexOf(0xd9);                       // trim trailing bytes after final EOI (FF D9)
    if (e > 0 && f[e - 1] === 0xff) f = f.subarray(0, e + 1);
    if (count % stride === 0) offsets.push(pos);
    const sz = new Uint8Array(4);
    new DataView(sz.buffer).setUint32(0, f.length, true);
    body.push(sz, f);
    count++; pos += 4 + f.length;
    if (f.length > maxFrame) maxFrame = f.length;
  }
  const idxOff = pos;
  const idx = new Uint8Array(4 + offsets.length * 4);
  const idv = new DataView(idx.buffer);
  idv.setUint32(0, offsets.length, true);
  offsets.forEach((o, i) => idv.setUint32(4 + i * 4, o, true));

  const h = new Uint8Array(32);
  const hv = new DataView(h.buffer);
  h.set([0x4e, 0x46, 0x56, 0x31], 0);                    // 'NFV1'
  hv.setUint8(4, 2);                                     // version 2
  hv.setUint8(5, (hasAudio ? 1 : 0) | 2);               // flags: audio + indexed
  hv.setUint16(6, width, true);
  hv.setUint16(8, height, true);
  hv.setUint16(10, fps, true);
  hv.setUint16(12, stride, true);
  hv.setUint32(14, count, true);
  hv.setUint32(18, Math.round((count * 1000) / fps), true);
  hv.setUint32(22, maxFrame, true);
  hv.setUint32(26, idxOff, true);

  return new Blob([h, ...body, idx], { type: 'application/octet-stream' });
}

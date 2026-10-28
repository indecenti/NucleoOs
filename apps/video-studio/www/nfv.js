/* SPDX-License-Identifier: AGPL-3.0-or-later */
// Build a NucleoOS .nfv v4 in the browser: the format the Cardputer's native Video app plays (spec and the
// reference encoder: tools/nfv/nfv4.py — this file writes byte-for-byte the same layout).
//   64-byte header · chunks [u8 type][u8 0][u16 mask][u32 len][u32 stamp][payload] · index {u32 frame, u32 off}/s
// Video chunks are whole baseline 4:2:0 JPEGs; audio chunks are IMA ADPCM blocks (WAV layout, mono), written
// LEAD_S ahead of the frame they play under, so the device's one reader never lets the sound run dry.
// Pure data, no DOM — runs in the browser AND in Node (for tests).

export const LEAD_S = 0.25;            // audio queued ahead of the picture
export const REFRESH_S = 2.0;          // every strip re-sent at least this often
const STRIP_MAD = 2.0;                 // mean abs difference (0..255) that makes a strip "changed"
const STRIP_PX = 18, STRIP_FRAC = 0.01; // ...or this share of its pixels moving more than STRIP_PX (local motion)

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

// IMA ADPCM blocks out of the WAV ffmpeg writes with -c:a adpcm_ima_wav -block_size N.
export function wavAdpcmBlocks(wav) {
  const dv = new DataView(wav.buffer, wav.byteOffset, wav.byteLength);
  const tag = (o) => String.fromCharCode(wav[o], wav[o + 1], wav[o + 2], wav[o + 3]);
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') throw new Error('not a WAV');
  let p = 12, block = 0, spb = 0, rate = 0, data = null;
  while (p + 8 <= wav.length) {
    const id = tag(p), len = dv.getUint32(p + 4, true), body = p + 8;
    if (id === 'fmt ') {
      if (dv.getUint16(body, true) !== 0x11 || dv.getUint16(body + 2, true) !== 1) throw new Error('not mono IMA ADPCM');
      rate = dv.getUint32(body + 4, true);
      block = dv.getUint16(body + 12, true);
      spb = len >= 20 ? dv.getUint16(body + 18, true) : (block - 4) * 2 + 1;
    } else if (id === 'data') data = wav.subarray(body, Math.min(wav.length, body + len));
    p = body + len + (len & 1);
  }
  if (!data || !block) throw new Error('WAV without ADPCM data');
  const blocks = [];
  for (let o = 0; o + 4 < data.length; o += block) blocks.push(data.subarray(o, Math.min(data.length, o + block)));
  return { blocks, block, spb, rate };
}

// The strip mask of one frame (RGBA pixels, as a canvas returns them): bit k = 16-row strip k changed since
// it was last sent. `shown` (one entry per strip, null at start) is updated in place. Same rules as nfv4.py.
export function stripMask(shown, rgba, w, h, nfr, refresh) {
  const n = h >> 4, rowBytes = w * 4;
  let mask = 0;
  for (let k = 0; k < n; k++) {
    const a = k * 16 * rowBytes, b = a + 16 * rowBytes;
    let changed = !shown[k] || (nfr - Math.floor(k * refresh / n)) % refresh === 0;
    if (!changed) {
      const prev = shown[k];
      let sum = 0, moving = 0;
      for (let i = a, j = 0; i < b; i += 4, j += 4) {
        const dr = Math.abs(rgba[i] - prev[j]), dg = Math.abs(rgba[i + 1] - prev[j + 1]), db = Math.abs(rgba[i + 2] - prev[j + 2]);
        sum += dr + dg + db;
        if (dr > STRIP_PX || dg > STRIP_PX || db > STRIP_PX) moving++;
      }
      const px = 16 * w;
      changed = sum / (px * 3) > STRIP_MAD || moving / px > STRIP_FRAC;
    }
    if (changed) { mask |= 1 << k; shown[k] = rgba.slice(a, b); }
  }
  return mask;
}

// frames: Uint8Array[] (complete JPEGs) · masks: number[] (one per frame) · audio: wavAdpcmBlocks() result or null.
// Returns a Blob (the .nfv).
export function buildNfv4(frames, { fps, width, height, visW = width, visH = height, masks = null, audio = null }) {
  if (width % 16 || height % 16) throw new Error('coded size must be a multiple of 16');
  const parts = [], index = [], aoffs = [];
  let pos = 64, maxJpeg = 0, ai = 0;
  const hdr = new Uint8Array(64);
  parts.push(hdr);
  const chunk = (type, stamp, payload, mask = 0) => {
    const h = new Uint8Array(12), dv = new DataView(h.buffer);
    h[0] = type; dv.setUint16(2, mask, true); dv.setUint32(4, payload.length, true); dv.setUint32(8, stamp >>> 0, true);
    parts.push(h, payload);
    const off = pos; pos += 12 + payload.length; return off;
  };
  const ab = audio ? audio.blocks : [], spb = audio ? audio.spb : 0, rate = audio ? audio.rate : 0;
  const audioUntil = (t) => { while (ai < ab.length && ai * spb < t * rate) { aoffs.push(chunk(2, ai * spb, ab[ai])); ai++; } };
  frames.forEach((f, nfr) => {
    const e = f.lastIndexOf(0xd9);                       // trim trailing bytes after the final EOI (FF D9)
    if (e > 0 && f[e - 1] === 0xff) f = f.subarray(0, e + 1);
    audioUntil(nfr / fps + LEAD_S);
    const off = chunk(1, nfr, f, masks ? masks[nfr] : 0xffff);
    if (nfr % fps === 0) {
      // A seek point must start early enough to meet the audio of this instant (written up to LEAD earlier).
      const k = aoffs.length ? Math.min(Math.floor(Math.floor(nfr / fps * rate) / spb), aoffs.length - 1) : -1;
      index.push([nfr, k >= 0 ? Math.min(off, aoffs[k]) : off]);
    }
    if (f.length > maxJpeg) maxJpeg = f.length;
  });
  audioUntil(1e9);
  const indexOff = pos;
  const idx = new Uint8Array(index.length * 8), iv = new DataView(idx.buffer);
  index.forEach(([fi, off], i) => { iv.setUint32(i * 8, fi, true); iv.setUint32(i * 8 + 4, off, true); });
  parts.push(idx);

  const hv = new DataView(hdr.buffer);
  hdr.set([0x4e, 0x46, 0x56, 0x31], 0);                  // 'NFV1' (shared magic; the version byte decides)
  hdr[4] = 4;
  hdr[5] = (ab.length ? 1 : 0) | 2;                      // audio present · video chunks carry a strip mask
  hv.setUint16(6, width, true); hv.setUint16(8, height, true); hv.setUint16(10, fps, true);
  hv.setUint16(12, ab.length ? rate : 0, true);
  hdr[14] = ab.length ? 1 : 0;                           // 1 = IMA ADPCM
  hv.setUint32(16, frames.length, true);
  hv.setUint32(20, Math.floor(frames.length * 1000 / fps), true);
  hv.setUint32(24, maxJpeg, true);
  hv.setUint16(28, audio ? audio.block : 0, true); hv.setUint16(30, spb, true);
  hv.setUint32(32, indexOff, true); hv.setUint32(36, index.length, true);
  hv.setUint32(40, visW, true); hv.setUint32(44, visH, true);
  return { blob: new Blob(parts, { type: 'application/octet-stream' }), maxJpeg, size: pos + idx.length };
}

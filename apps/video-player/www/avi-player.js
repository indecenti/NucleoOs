/* SPDX-License-Identifier: AGPL-3.0-or-later */
// avi-player.js — motore AVI (RIFF) con video Motion-JPEG e audio PCM, per le registrazioni della
// fotocamera della board (/DCIM/VID_*.avi) e qualunque AVI MJPEG (ffmpeg, webcam, fotocamere).
//
// • Indice: idx1 (offset relativi al fourcc 'movi' o assoluti: si prova quale base fa cadere la
//   prima voce su un id di chunk) oppure — registrazione interrotta, niente idx1, dimensioni a 0 —
//   scansione lineare delle intestazioni dei chunk man mano che i byte arrivano.
// • Video: ogni fotogramma è un JPEG completo → decodifica con createImageBitmap (fuori dal thread
//   principale), concorrenza limitata, finestra di pre-decodifica; seek esatto al fotogramma.
//   JPEG "AVI1" senza tabelle di Huffman (webcam) → si inseriscono quelle standard.
// • Audio: PCM 8/16/24/32 bit, float, A-law/µ-law → WebAudio (orologio master). Altri codec
//   (MP3, AC3, ADPCM…) → video muto + avviso.

import { SoftEngine, MediaError } from './soft-engine.js';
import { BLOCK } from './byte-source.js';

const PREFETCH_S = 3.0;        // secondi di byte scaricati in anticipo
const DECODE_AHEAD_S = 0.6;    // secondi di fotogrammi decodificati in anticipo
const MAX_DECODE_AHEAD = 8;
const MAX_INFLIGHT_DECODE = 3;
const AUDIO_AHEAD = 0.8;

const MJPEG_FOURCC = /^(MJPG|AVRN|AVDJ|ADJV|JPEG|MJPA|DMB1|IJPG|QIVG|SLMJ|CJPG|JPGL|MMJP|LJPG|MJLS)$/i;
const AUDIO_NAMES = { 0x50: 'MP2', 0x55: 'MP3', 0x2000: 'AC3', 0x2001: 'DTS', 0x11: 'IMA ADPCM', 0x02: 'MS ADPCM', 0xFF: 'AAC', 0x1610: 'AAC', 0x161: 'WMA', 0x162: 'WMA Pro', 0x31: 'GSM', 0x674f: 'Vorbis' };

const fcc = (u, o) => String.fromCharCode(u[o], u[o + 1], u[o + 2], u[o + 3]);
const u32 = (u, o) => (u[o] | (u[o + 1] << 8) | (u[o + 2] << 16) | (u[o + 3] << 24)) >>> 0;
const u16 = (u, o) => u[o] | (u[o + 1] << 8);
const i32 = (u, o) => u[o] | (u[o + 1] << 8) | (u[o + 2] << 16) | (u[o + 3] << 24);
const isFcc = (u, o) => { for (let k = 0; k < 4; k++) { const c = u[o + k]; if (c < 0x20 || c > 0x7e) return false; } return true; };

// ---------------------------------------------------------------- JPEG senza DHT (MJPEG "AVI1")

const STD_DHT = new Uint8Array([
  0xFF, 0xC4, 0x01, 0xA2,
  0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
  0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
  0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D,
  0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
  0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
  0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
  0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
  0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
  0xF9, 0xFA,
  0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77,
  0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
  0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
  0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
  0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
  0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
  0xF9, 0xFA,
]);

// Posizione del marker SOS se il JPEG non ha tabelle di Huffman (va inserita la DHT standard),
// -1 se le ha (o se non è un JPEG riconoscibile).
function sosWithoutDht(u) {
  if (u.length < 4 || u[0] !== 0xFF || u[1] !== 0xD8) return -1;
  let p = 2;
  while (p + 4 <= u.length) {
    if (u[p] !== 0xFF) return -1;
    const m = u[p + 1];
    if (m === 0xFF) { p++; continue; }
    if (m === 0xC4) return -1;
    if (m === 0xDA) return p;
    if (m === 0xD8 || (m >= 0xD0 && m <= 0xD7) || m === 0x01) { p += 2; continue; }
    p += 2 + ((u[p + 2] << 8) | u[p + 3]);
  }
  return -1;
}

// ---------------------------------------------------------------- PCM → float

const ALAW = new Float32Array(256), ULAW = new Float32Array(256);
for (let i = 0; i < 256; i++) {
  let a = i ^ 0x55, t = (a & 0x0f) << 4, seg = (a & 0x70) >> 4;
  t = seg === 0 ? t + 8 : seg === 1 ? t + 0x108 : (t + 0x108) << (seg - 1);
  ALAW[i] = ((a & 0x80) ? t : -t) / 32768;
  let u = ~i & 0xff; let tt = ((u & 0x0f) << 3) + 0x84; tt <<= (u & 0x70) >> 4;
  ULAW[i] = ((u & 0x80) ? 0x84 - tt : tt - 0x84) / 32768;
}

function pcmPlanes(bytes, fmt) {
  const ch = fmt.channels, bps = fmt.bytesPerSample, frame = ch * bps;
  const n = Math.floor(bytes.length / frame);
  const planes = [];
  for (let c = 0; c < ch; c++) planes.push(new Float32Array(n));
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let o = 0;
  for (let i = 0; i < n; i++) {
    for (let c = 0; c < ch; c++, o += bps) {
      let v;
      switch (fmt.kind) {
        case 'u8': v = (bytes[o] - 128) / 128; break;
        case 's16': v = dv.getInt16(o, true) / 32768; break;
        case 's24': v = ((bytes[o] | (bytes[o + 1] << 8) | (bytes[o + 2] << 16)) << 8 >> 8) / 8388608; break;
        case 's32': v = dv.getInt32(o, true) / 2147483648; break;
        case 'f32': v = dv.getFloat32(o, true); break;
        case 'f64': v = dv.getFloat64(o, true); break;
        case 'alaw': v = ALAW[bytes[o]]; break;
        case 'ulaw': v = ULAW[bytes[o]]; break;
        default: v = 0;
      }
      planes[c][i] = v;
    }
  }
  return planes;
}

// ---------------------------------------------------------------- motore

export class AviEngine extends SoftEngine {
  constructor(opts) {
    super(opts);
    this.info.engine = 'AVI (soft)';
    this.info.container = 'AVI';
  }

  async _open() {
    const src = this.src;
    const size = src.size;
    const head = await src.read(0, Math.min(size != null ? size : BLOCK, BLOCK));
    if (head.length < 12 || fcc(head, 0) !== 'RIFF' || fcc(head, 8) !== 'AVI ') throw new MediaError('errNotAvi');
    const riffSize = u32(head, 4);
    const fileEnd = size != null ? size : Infinity;
    const riffEnd = riffSize ? Math.min(8 + riffSize, fileEnd) : fileEnd;
    const bytesAt = async (off, len) => (off + len <= head.length ? head.subarray(off, off + len) : await src.read(off, len));

    let p = 12, hdrl = null, moviFcc = -1, moviEnd = fileEnd, openMovi = false;
    while (p + 8 <= riffEnd) {
      const h = await bytesAt(p, 12);
      if (h.length < 8) break;
      const id = fcc(h, 0), sz = u32(h, 4);
      if (id === 'LIST' && h.length >= 12) {
        const type = fcc(h, 8);
        if (type === 'hdrl') hdrl = await bytesAt(p + 12, Math.max(0, sz - 4));
        else if (type === 'movi') {
          moviFcc = p + 8;
          if (!sz || p + 8 + sz > fileEnd) { openMovi = true; moviEnd = fileEnd; }
          else moviEnd = p + 8 + sz;
          break;
        }
      }
      if (!isFcc(h, 0)) break;
      p += 8 + sz + (sz & 1);
    }
    if (!hdrl) throw new MediaError('errCorrupt', 'hdrl');
    if (moviFcc < 0) throw new MediaError('errNoFrames');
    this._parseHdrl(hdrl);

    // ---- indice
    this.fOff = []; this.fSize = [];            // fotogrammi video: offset del payload, byte
    this.aOff = []; this.aSize = []; this.aStart = [];   // chunk audio + primo campione
    this.aSamples = 0;
    this.moviStart = moviFcc + 4;
    this.moviEnd = moviEnd;
    this.scan = { pos: this.moviStart, done: false };

    if (!openMovi && src.ranged && moviEnd + 8 <= fileEnd) {
      try { await this._loadIdx1(moviEnd + (moviEnd & 1), fileEnd, moviFcc); } catch (e) { console.warn('[avi] idx1 non valido, scansione', e); this._resetIndex(); }
    }
    if (!this.scan.done) this._scanAvailable(4096);
    // un file con solo l'intestazione (registrazione vuota): nessun fotogramma
    if (this.scan.done && !this.fOff.length) throw new MediaError('errNoFrames');
    this._updateDuration();

    this._setSize(this.vWidth, this.vHeight);
    this.decoded = new Map();       // n -> {state, bmp, t0}
    this.inflight = 0;
    this.shownIdx = -1;
    this.aNext = 0;
    this.gen = 0;
    // primo fotogramma subito (poster)
    await this._showFrameAt(0, true);
  }

  _parseHdrl(u) {
    let fps = 0, usec = 0, total = 0, w = 0, h = 0, dml = 0;
    const streams = [];
    const walk = (buf, off, end, cur) => {
      while (off + 8 <= end) {
        const id = fcc(buf, off), sz = u32(buf, off + 4), d = off + 8;
        if (id === 'LIST') {
          const type = fcc(buf, d);
          if (type === 'strl') { const s = {}; streams.push(s); walk(buf, d + 4, Math.min(end, d + sz), s); }
          else walk(buf, d + 4, Math.min(end, d + sz), cur);
        }
        else if (id === 'avih' && sz >= 40) { usec = u32(buf, d); total = u32(buf, d + 16); w = u32(buf, d + 32); h = u32(buf, d + 36); }
        else if (id === 'dmlh' && sz >= 4) dml = u32(buf, d);
        else if (id === 'strh' && cur && sz >= 36) {
          cur.type = fcc(buf, d); cur.handler = fcc(buf, d + 4);
          cur.scale = u32(buf, d + 20); cur.rate = u32(buf, d + 24); cur.start = u32(buf, d + 28); cur.length = u32(buf, d + 32);
        }
        else if (id === 'strf' && cur) cur.strf = buf.subarray(d, Math.min(end, d + sz));
        off = d + sz + (sz & 1);
      }
    };
    walk(u, 0, u.length, null);
    const vi = streams.findIndex((s) => s.type === 'vids');
    if (vi < 0) throw new MediaError('errNoVideo');
    const vs = streams[vi];
    let comp = vs.handler || '';
    if (vs.strf && vs.strf.length >= 20) {
      w = Math.abs(i32(vs.strf, 4)) || w; h = Math.abs(i32(vs.strf, 8)) || h;
      const c = fcc(vs.strf, 16);
      if (isFcc(vs.strf, 16)) comp = c;
    }
    const codec = comp.replace(/\0/g, '').trim();
    if (!MJPEG_FOURCC.test(codec) && !MJPEG_FOURCC.test((vs.handler || '').trim())) {
      throw new MediaError('errUnsupportedCodec', codec, { codec: codec || '?' });
    }
    if (vs.scale > 0 && vs.rate > 0) fps = vs.rate / vs.scale;
    if (!(fps > 0.01 && fps < 1000) && usec > 0) fps = 1e6 / usec;
    if (!(fps > 0.01 && fps < 1000)) fps = 12;
    this.fps = fps;
    this.frameDur = 1 / fps;
    this.vStream = vi;
    this.vWidth = w || 640; this.vHeight = h || 480;
    this.hdrFrames = Math.max(dml, total, vs.length || 0);
    this.info.vcodec = 'Motion JPEG (' + codec + ')';
    this.info.fpsNominal = fps;

    const ai = streams.findIndex((s) => s.type === 'auds');
    this.aStream = ai;
    this.afmt = null;
    if (ai >= 0) {
      const f = streams[ai].strf;
      if (f && f.length >= 16) {
        let tag = u16(f, 0);
        const ch = u16(f, 2), rate = u32(f, 4), align = u16(f, 12), bits = u16(f, 14);
        if (tag === 0xFFFE && f.length >= 26) tag = u16(f, 24);   // WAVE_FORMAT_EXTENSIBLE → sottoformato
        let kind = null;
        if (tag === 1) kind = bits === 8 ? 'u8' : bits === 16 ? 's16' : bits === 24 ? 's24' : bits === 32 ? 's32' : null;
        else if (tag === 3) kind = bits === 32 ? 'f32' : bits === 64 ? 'f64' : null;
        else if (tag === 6 && bits === 8) kind = 'alaw';
        else if (tag === 7 && bits === 8) kind = 'ulaw';
        if (kind && ch > 0 && ch <= 8 && rate >= 1000 && rate <= 384000) {
          const bps = kind === 'u8' || kind === 'alaw' || kind === 'ulaw' ? 1 : kind === 's16' ? 2 : kind === 's24' ? 3 : kind === 'f64' ? 8 : 4;
          this.afmt = { kind, channels: ch, rate, bytesPerSample: bps, blockAlign: align || ch * bps };
          this.hasAudio = !!this.audio;
          this.info.acodec = (tag === 3 ? 'PCM float' : tag === 6 ? 'A-law' : tag === 7 ? 'µ-law' : 'PCM ' + bits + ' bit') + ' · ' + rate + ' Hz · ' + ch + ' ch';
        } else {
          const name = AUDIO_NAMES[tag] || ('0x' + tag.toString(16));
          this.audioNotice = name;
          this.info.acodec = name + ' (non supportato)';
        }
      }
    }
  }

  _resetIndex() {
    this.fOff = []; this.fSize = []; this.aOff = []; this.aSize = []; this.aStart = []; this.aSamples = 0;
    this.scan = { pos: this.moviStart, done: false };
  }

  _chunkKind(id) {
    const s = parseInt(id.slice(0, 2), 10);
    if (!(s >= 0)) return 0;
    const t = id.slice(2);
    if (s === this.vStream && (t === 'dc' || t === 'db')) return 1;
    if (s === this.aStream && t === 'wb') return 2;
    return 0;
  }
  _addAudio(off, sz) {
    this.aOff.push(off); this.aSize.push(sz); this.aStart.push(this.aSamples);
    if (this.afmt) this.aSamples += Math.floor(sz / this.afmt.blockAlign);
  }

  async _loadIdx1(off, fileEnd, moviFcc) {
    // idx1 è di solito l'ultimo chunk: una sola richiesta per esattamente quei byte (16 per voce)
    let buf = await this.src.fetchRange(off, fileEnd - off <= 4 * 1024 * 1024 ? fileEnd - off : 8);
    if (buf.length < 8 || fcc(buf, 0) !== 'idx1') return;
    const sz = u32(buf, 4);
    if (!sz || off + 8 + sz > fileEnd) return;
    if (buf.length < 8 + sz) buf = await this.src.fetchRange(off, 8 + sz);
    const raw = buf.subarray(8, 8 + sz);
    const n = Math.floor(raw.length / 16);
    if (!n) return;
    // base degli offset: 'movi' (standard) o assoluti — vince quella che cade su un id di chunk
    let first = -1;
    for (let k = 0; k < n && first < 0; k++) if (!(u32(raw, k * 16 + 4) & 1) && fcc(raw, k * 16) !== 'rec ') first = k;
    if (first < 0) return;
    const id0 = fcc(raw, first * 16), o0 = u32(raw, first * 16 + 8);
    let base = null;
    for (const b of [moviFcc, 0, moviFcc + 4, moviFcc - 4]) {
      const probe = await this.src.read(b + o0, 4);
      if (probe.length === 4 && fcc(probe, 0) === id0) { base = b; break; }
    }
    if (base == null) throw new Error('idx1 base');
    for (let k = 0; k < n; k++) {
      const o = k * 16;
      const id = fcc(raw, o);
      const kind = this._chunkKind(id);
      if (!kind) continue;
      const cs = u32(raw, o + 12), co = base + u32(raw, o + 8) + 8;
      if (kind === 1) { this.fOff.push(co); this.fSize.push(cs); }
      else this._addAudio(co, cs);
    }
    if (!this.fOff.length) { this._resetIndex(); return; }
    this.scan = { pos: this.moviEnd, done: true };
    this.indexKind = 'idx1';
  }

  // Scansione lineare dei chunk su quanto già scaricato. Ritorna true se ha fatto progressi.
  _scanAvailable(maxChunks = 512) {
    if (this.scan.done) return false;
    const src = this.src;
    const end = Math.min(this.moviEnd, src.size != null ? src.size : Infinity);
    let pos = this.scan.pos, count = 0, progressed = false;
    this.indexKind = 'scan';
    while (count++ < maxChunks) {
      if (pos + 8 > end) { this.scan.done = true; break; }
      const h = src.readSync(pos, 12);
      if (!h || h.length < 8) break;
      const id = fcc(h, 0), sz = u32(h, 4);
      if (!isFcc(h, 0)) {
        const r = this._resync(pos, end);
        if (r < 0) break;          // mancano dati per cercare: si riprova più tardi
        if (r === Infinity) { this.scan.done = true; break; }
        pos = r; progressed = true; continue;
      }
      if (id === 'LIST' || id === 'RIFF') { pos += 12; progressed = true; continue; }   // 'rec ', AVIX/movi
      if (id === 'idx1' && this.moviEnd === end && pos + 8 + sz >= end) { this.scan.done = true; break; }
      const kind = this._chunkKind(id);
      if (kind && pos + 8 + sz > end) { this.scan.done = true; break; }   // ultimo chunk troncato
      if (kind === 1) { this.fOff.push(pos + 8); this.fSize.push(sz); }
      else if (kind === 2) this._addAudio(pos + 8, sz);
      pos += 8 + sz + (sz & 1);
      progressed = true;
    }
    this.scan.pos = pos;
    if (progressed) this._updateDuration();
    return progressed;
  }
  // Dati non validi nel movi (scrittura interrotta, chunk dispari non allineati): cerca il
  // prossimo id di chunk plausibile. -1 = servono altri byte, Infinity = niente fino alla fine.
  _resync(pos, end) {
    const win = this.src.readSync(Math.max(0, pos - 1), Math.min(256 * 1024, end - pos + 1));
    if (!win || win.length < 16) return -1;
    const vid = String(this.vStream).padStart(2, '0');
    for (let k = 0; k + 8 <= win.length; k++) {
      if (win[k] === vid.charCodeAt(0) && win[k + 1] === vid.charCodeAt(1) && (win[k + 2] === 0x64) && (win[k + 3] === 0x63 || win[k + 3] === 0x62)) {
        const sz = u32(win, k + 4);
        if (sz < 64 * 1024 * 1024 && (k + 8 >= win.length || win[k + 8] === 0xFF || sz === 0)) return Math.max(0, pos - 1) + k;
      }
    }
    return win.length >= end - pos ? Infinity : Math.max(0, pos - 1) + win.length - 8;
  }

  _updateDuration() {
    let frames = this.fOff.length;
    const exact = this.scan.done;
    if (!exact) {
      let est = frames;
      if (frames > 0 && this.src.size != null) {
        const endB = Math.min(this.moviEnd, this.src.size);
        const per = (this.scan.pos - this.moviStart) / frames;   // byte per fotogramma (audio incluso)
        if (per > 0) est = frames + Math.max(0, endB - this.scan.pos) / per;
      }
      frames = this.hdrFrames > 0 ? Math.max(this.hdrFrames, frames) : Math.round(est);
    }
    let d = frames * this.frameDur;
    if (this.afmt && exact) d = Math.max(d, this.aSamples / this.afmt.rate);
    if (d > 0) this._setDuration(d, exact);
  }

  // ---- fotogrammi
  _frameIndexAt(t) {
    const n = Math.floor(t * this.fps + 1e-6);
    return Math.max(0, Math.min(n, this.fOff.length - 1));
  }
  _frameReadyData(n) { return this.src.has(this.fOff[n], this.fSize[n]); }

  _startDecode(n) {
    const rec = { state: 'pending', bmp: null, gen: this.gen };
    this.decoded.set(n, rec);
    const size = this.fSize[n];
    if (!size) { rec.state = 'hold'; return; }     // chunk vuoto = ripeti il fotogramma precedente
    let bytes = this.src.readSync(this.fOff[n], size);
    if (!bytes) { this.decoded.delete(n); return; }
    if (this.needDht == null) this.needDht = sosWithoutDht(bytes) > 0;
    if (this.needDht) {
      const s = sosWithoutDht(bytes);
      if (s > 0) { const out = new Uint8Array(bytes.length + STD_DHT.length); out.set(bytes.subarray(0, s)); out.set(STD_DHT, s); out.set(bytes.subarray(s), s + STD_DHT.length); bytes = out; }
    }
    this.inflight++;
    createImageBitmap(new Blob([bytes], { type: 'image/jpeg' })).then((bmp) => {
      this.inflight--;
      if (this._destroyed || rec.gen !== this.gen || this.decoded.get(n) !== rec) { bmp.close(); return; }
      rec.state = 'ready'; rec.bmp = bmp;
      this.stats.decoded++;
      if (bmp.width !== this.canvas.width || bmp.height !== this.canvas.height) this._setSize(bmp.width, bmp.height);
    }, () => {
      this.inflight--;
      rec.state = 'error';
      this.stats.corrupt++;
    });
  }

  _display(n) {
    const rec = this.decoded.get(n);
    if (!rec) return false;
    if (rec.state === 'ready') {
      const g = this.g || (this.g = this.canvas.getContext('2d', { alpha: false }));
      g.drawImage(rec.bmp, 0, 0, this.canvas.width, this.canvas.height);
    } else if (rec.state !== 'hold' && rec.state !== 'error') return false;
    if (this.shownIdx >= 0 && n > this.shownIdx + 1 && !this._paused) this.stats.dropped += n - this.shownIdx - 1;
    this.shownIdx = n;
    this._markShown();
    // libera i fotogrammi ormai passati
    for (const [k, r] of this.decoded) if (k < n) { if (r.bmp) r.bmp.close(); this.decoded.delete(k); }
    return true;
  }

  _clearDecoded() {
    this.gen++;
    for (const [, r] of this.decoded) if (r.bmp) r.bmp.close();
    this.decoded.clear();
  }

  // Porta a schermo il fotogramma del tempo t (seek / poster). Aspetta i dati se serve.
  async _showFrameAt(t, first) {
    const target = this._seekTarget;
    let n = Math.floor(t * this.fps + 1e-6);
    // scansione: servono i chunk fino al fotogramma n (si scaricano i blocchi in sequenza)
    while (!this.scan.done && this.fOff.length <= n) {
      const blk = Math.floor(this.scan.pos / BLOCK);
      this.src.prefetch(blk + 1, blk + 4);
      await this.src.read(this.scan.pos, 12);
      if (!first && target !== this._seekTarget) return;
      if (!this._scanAvailable(100000) && !this.scan.done) {
        // dati corrotti: serve una finestra più ampia per ritrovare un chunk valido
        await this.src.read(this.scan.pos, 256 * 1024);
        if (!this._scanAvailable(100000) && !this.scan.done) { this.scan.done = true; this._updateDuration(); }
      }
    }
    if (!this.fOff.length) throw new MediaError('errNoFrames');
    n = Math.max(0, Math.min(n, this.fOff.length - 1));
    this._clearDecoded();
    this.shownIdx = -1;
    // i chunk vuoti ripetono il precedente: cerca l'ultimo fotogramma con dati
    let k = n;
    while (k > 0 && !this.fSize[k]) k--;
    await this.src.read(this.fOff[k], this.fSize[k]);
    if (!first && target !== this._seekTarget) return;
    this._startDecode(k);
    const rec = this.decoded.get(k);
    const t0 = performance.now();
    while (rec && rec.state === 'pending' && performance.now() - t0 < 5000) await new Promise((r) => setTimeout(r, 8));
    if (rec && rec.state === 'error' && first) throw new MediaError('errDecode', 'JPEG');
    this._display(k);
    this.shownIdx = n;
    this.aNext = this._audioIndexAt(t);
  }

  async _seek(t) {
    this.src.cancelPrefetch();
    if (this.audio) this.audio.clear();
    await this._showFrameAt(t, false);
  }

  _audioIndexAt(t) {
    if (!this.afmt || !this.aOff.length) return 0;
    const s = t * this.afmt.rate;
    let lo = 0, hi = this.aStart.length - 1;
    while (lo < hi) { const m = (lo + hi + 1) >> 1; if (this.aStart[m] <= s) lo = m; else hi = m - 1; }
    return lo;
  }

  _tick(now) {
    const src = this.src;
    if (!this.scan.done) this._scanAvailable();
    const total = this.fOff.length;
    if (!total) return;
    const n = this._frameIndexAt(now);

    // ---- prefetch dei byte: da n a n + PREFETCH_S (e oltre la scansione se l'indice è parziale)
    const aheadFrames = Math.ceil(Math.max(PREFETCH_S, this._resumeSecs() + 0.5) * this.fps) + 1;
    const last = Math.min(total - 1, n + aheadFrames);
    let from = this.fOff[n], to = this.fOff[last] + this.fSize[last];
    if (!this.scan.done && last === total - 1) {
      const per = total > 1 ? (this.fOff[total - 1] - this.fOff[0]) / (total - 1) : BLOCK;
      to = Math.max(to, this.scan.pos + Math.max(BLOCK, per * aheadFrames));
    }
    if (typeof src.setPosition === 'function') src.setPosition(from);
    src.prefetch(Math.floor(from / BLOCK), Math.floor((to - 1) / BLOCK));
    // blocco del fotogramma corrente: urgente
    if (!src.has(this.fOff[n], this.fSize[n])) {
      const b0 = Math.floor(this.fOff[n] / BLOCK), b1 = Math.floor((this.fOff[n] + Math.max(1, this.fSize[n]) - 1) / BLOCK);
      for (let b = b0; b <= b1; b++) src.getBlock(b, true).catch(() => {});
    }

    // ---- decodifica in anticipo
    const ahead = Math.min(MAX_DECODE_AHEAD, Math.max(2, Math.ceil(DECODE_AHEAD_S * this.fps)));
    for (let k = n; k <= Math.min(total - 1, n + ahead) && this.inflight < MAX_INFLIGHT_DECODE; k++) {
      if (k <= this.shownIdx && k !== n) continue;
      if (this.decoded.has(k)) continue;
      if (!this._frameReadyData(k)) break;
      this._startDecode(k);
    }
    // i fotogrammi in ritardo non ancora decodificati si saltano (frame drop)
    for (const [k, r] of this.decoded) if (k < n && r.state === 'pending') { this.decoded.delete(k); }

    // ---- a schermo: il fotogramma più recente pronto che non superi n
    if (n !== this.shownIdx) {
      for (let k = n; k > this.shownIdx; k--) {
        const r = this.decoded.get(k);
        if (r && r.state !== 'pending') { this._display(k); break; }
      }
    }

    // ---- audio PCM
    if (this.afmt && this.audio) this._pumpAudio(now);
  }

  _pumpAudio(now) {
    const until = now + AUDIO_AHEAD;
    const fmt = this.afmt, rate = fmt.rate;
    const minBatch = Math.round(rate * 0.1);   // ~100 ms per AudioBuffer: meno nodi, niente giunture
    let budget = 16;
    // coda vuota dopo un seek/attesa: riparti dal chunk che contiene "now"
    if (this.audio.chunks.length === 0 && this.aNext < this.aOff.length && this.aStart[this.aNext] / rate > now + 0.05) this.aNext = this._audioIndexAt(now);
    while (budget-- > 0 && this.aNext < this.aOff.length) {
      const t0 = this.aStart[this.aNext] / rate;
      if (t0 >= until) break;
      // salta i chunk già passati
      if ((this.aStart[this.aNext] + this.aSize[this.aNext] / fmt.blockAlign) / rate < now - 0.05) { this.aNext++; continue; }
      // unisce chunk consecutivi (già scaricati) fino a ~100 ms
      const parts = [];
      let k = this.aNext, samples = 0;
      while (k < this.aOff.length && samples < minBatch) {
        if (k > this.aNext && this.aStart[k] !== this.aStart[k - 1] + Math.floor(this.aSize[k - 1] / fmt.blockAlign)) break;
        const bytes = this.src.readSync(this.aOff[k], this.aSize[k]);
        if (!bytes) { if (k === this.aNext) this.src.getBlock(Math.floor(this.aOff[k] / BLOCK), true).catch(() => {}); break; }
        parts.push(bytes);
        samples += Math.floor(this.aSize[k] / fmt.blockAlign);
        k++;
      }
      if (!parts.length) break;
      if (samples < minBatch && k < this.aOff.length && this.aStart[k] / rate < until) {
        // il resto del lotto non è ancora arrivato: aspetta, a meno che serva subito
        if (t0 > now + 0.25) break;
      }
      let joined = parts[0];
      if (parts.length > 1) {
        joined = new Uint8Array(parts.reduce((a, p) => a + p.length, 0));
        let o = 0; for (const p of parts) { joined.set(p, o); o += p.length; }
      }
      if (samples) this.audio.push(t0, rate, pcmPlanes(joined, fmt));
      this.aNext = k;
    }
  }

  _starved(now) {
    const total = this.fOff.length;
    if (!total) return !this.scan.done;
    const n = this._frameIndexAt(now);
    if (!this.scan.done && now * this.fps >= total - 1) return true;   // oltre la parte già indicizzata
    const r = this.decoded.get(n);
    if (n === this.shownIdx || (r && r.state !== 'pending')) return false;
    return !this._frameReadyData(n);
  }
  _canResume(now) {
    const total = this.fOff.length;
    if (!total) return false;
    const n = this._frameIndexAt(now);
    if (!this.scan.done && now * this.fps >= total - 1) return false;
    // il fotogramma corrente deve essere già decodificato, poi servono _resumeSecs() di dati
    const r = this.decoded.get(n);
    if (n !== this.shownIdx && (!r || r.state === 'pending')) return false;
    const last = Math.min(total - 1, n + Math.ceil(this.fps * this._resumeSecs()));
    return this.src.has(this.fOff[n], this.fOff[last] + this.fSize[last] - this.fOff[n]);
  }
  _freezeTime() { return this.shownIdx >= 0 ? (this.shownIdx + 1) / this.fps - 1e-3 : NaN; }
  _atEnd(now) {
    if (!this.scan.done) return false;
    return now >= this._duration - 1e-4;
  }
  _bufferedAhead() {
    const total = this.fOff.length;
    if (!total) return 0;
    const n = this._frameIndexAt(this._now());
    let k = n;
    while (k < total && this.src.has(this.fOff[k], this.fSize[k])) k++;
    return (k - n) / this.fps;
  }
  _bufferedRanges() {
    const total = this.fOff.length;
    if (!total) return [];
    const out = [];
    const fo = this.fOff;
    const findFirst = (x) => { let lo = 0, hi = total; while (lo < hi) { const m = (lo + hi) >> 1; if (fo[m] < x) lo = m + 1; else hi = m; } return lo; };
    for (const [s, e] of this.src.cachedRanges()) {
      const a = findFirst(s);
      let b = findFirst(e) - 1;
      while (b >= a && fo[b] + this.fSize[b] > e) b--;
      if (b >= a) out.push([a / this.fps, Math.min(this._duration || Infinity, (b + 1) / this.fps)]);
    }
    return out;
  }
  _extraStats() {
    return { index: this.indexKind + (this.scan.done ? '' : ' …'), frames: this.fOff.length, audioChunks: this.aOff.length };
  }
  _destroy() { this._clearDecoded(); }
}

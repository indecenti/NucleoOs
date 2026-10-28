/* SPDX-License-Identifier: AGPL-3.0-or-later */
// NFV v4 in the browser: the Cardputer's own video format (tools/nfv/nfv4.py holds the spec and the reference
// reader). Frames are whole baseline JPEGs, audio is 16 kHz mono IMA ADPCM, both interleaved in one file with a
// one-per-second seek index at the end. The browser cannot hand such a file to <video>, so NfvMedia plays it
// itself: JPEGs onto a <canvas> (createImageBitmap), audio through Web Audio, read from the SD in sequential
// HTTP Range parts (a local File is sliced instead). It mimics the slice of HTMLMediaElement the player uses
// (currentTime, duration, paused, play/pause, volume, playbackRate, on<event> handlers), so the page swaps it
// in for the <video> without knowing the difference. The strip mask the device uses to spare its SPI bus is
// ignored here: every chunk is a complete frame.

const IMA_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
  80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
  724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
  4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
  22385, 24623, 27086, 29794, 32767];
const IMA_IDX = [-1, -1, -1, -1, 2, 4, 6, 8];

// One IMA ADPCM block (WAV/Microsoft layout, mono): 4-byte preamble with the first sample, then 2 nibbles a byte.
export function imaDecodeBlock(b, out = new Float32Array(1 + (b.length - 4) * 2)) {
  let pred = (b[0] | (b[1] << 8)) << 16 >> 16, idx = Math.min(88, b[2]);
  out[0] = pred / 32768;
  let o = 1;
  for (let i = 4; i < b.length; i++) {
    for (let s = 0; s < 8; s += 4) {
      const nib = (b[i] >> s) & 15, step = IMA_STEP[idx];
      let diff = step >> 3;
      if (nib & 1) diff += step >> 2;
      if (nib & 2) diff += step >> 1;
      if (nib & 4) diff += step;
      pred = nib & 8 ? pred - diff : pred + diff;
      if (pred > 32767) pred = 32767; else if (pred < -32768) pred = -32768;
      idx += IMA_IDX[nib & 7]; if (idx < 0) idx = 0; else if (idx > 88) idx = 88;
      out[o++] = pred / 32768;
    }
  }
  return out;
}

export function parseHeader(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const magic = String.fromCharCode(buf[0], buf[1], buf[2], buf[3]);
  if (magic !== 'NFV1') throw new Error('not an .nfv file');
  const version = buf[4];
  if (version !== 4) { const e = new Error('nfv v' + version); e.code = 'NFV_OLD'; e.version = version; throw e; }
  return {
    version, flags: buf[5], w: dv.getUint16(6, true), h: dv.getUint16(8, true), fps: dv.getUint16(10, true),
    rate: dv.getUint16(12, true), codec: buf[14], frames: dv.getUint32(16, true), durationMs: dv.getUint32(20, true),
    maxJpeg: dv.getUint32(24, true), block: dv.getUint16(28, true), spb: dv.getUint16(30, true),
    indexOff: dv.getUint32(32, true), indexCount: dv.getUint32(36, true),
    visW: dv.getUint32(40, true) || dv.getUint16(6, true), visH: dv.getUint32(44, true) || dv.getUint16(8, true),
  };
}

const PART = 256 * 1024;         // one Range request: big enough to keep the device's few sockets idle
const AHEAD_S = 4;               // read this far ahead of the playhead
const SEG = 8000;                // audio is scheduled in 0.5 s segments (at 16 kHz) on exact sample boundaries
const SCHED_AHEAD_S = 1.0;       // audio handed to Web Audio this far ahead

// Byte source: an SD path (HTTP Range on /api/fs/read) or a local File/Blob.
function makeSource(src) {
  if (src instanceof Blob) return { size: src.size, read: async (a, b) => new Uint8Array(await src.slice(a, b).arrayBuffer()) };
  let size = 0;
  return {
    get size() { return size; },
    async read(a, b, signal) {
      const r = await fetch(src, { headers: { Range: `bytes=${a}-${b - 1}` }, signal, cache: 'no-store' });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const cr = /\/(\d+)$/.exec(r.headers.get('content-range') || '');
      if (cr) size = +cr[1];
      const data = new Uint8Array(await r.arrayBuffer());
      return r.status === 206 ? data : data.subarray(a, b);     // a server without Range sends it all
    },
  };
}

export class NfvMedia extends EventTarget {
  constructor(canvas) {
    super();
    this.canvas = canvas;
    this.g = canvas.getContext('2d', { alpha: false });
    this.dataset = {};
    this.textTracks = [];
    this.audioTracks = undefined;
    this._vol = 1; this._muted = false; this._rate = 1;
    this.paused = true; this.ended = false; this.readyState = 0;
    this.h = null; this._gen = 0; this._raf = 0; this._ctx = null;
    this._reset(0);
  }

  // ── HTMLMediaElement look-alike ────────────────────────────────────────────────────────────────────────
  get duration() { return this.h ? this.h.durationMs / 1000 : NaN; }
  get videoWidth() { return this.h ? this.h.visW : 0; }
  get videoHeight() { return this.h ? this.h.visH : 0; }
  get currentTime() { return this._now(); }
  set currentTime(t) { if (this.h) this._seek(Math.max(0, Math.min(this.duration, +t || 0))); }
  get volume() { return this._vol; }
  set volume(x) { this._vol = Math.max(0, Math.min(1, +x)); this._applyGain(); this._emit('volumechange'); }
  get muted() { return this._muted; }
  set muted(m) { this._muted = !!m; this._applyGain(); this._emit('volumechange'); }
  get playbackRate() { return this._rate; }
  set playbackRate(r) {
    r = +r || 1; if (r === this._rate) return;
    const t = this._now(); this._rate = r;
    if (!this.paused) { this._stopAudio(); this._anchor(t); }
    this._emit('ratechange');
  }
  get buffered() {
    const end = Math.min(this.duration || 0, this._readT);
    const s = this._baseT;
    return { length: end > s ? 1 : 0, start: () => s, end: () => end };
  }
  querySelectorAll() { return []; }
  appendChild() {}
  removeAttribute() { this.close(); }
  load() {}
  requestPictureInPicture() { return Promise.reject(new Error('Picture-in-Picture is not available for .nfv')); }

  async open(src) {
    this.close();
    const gen = ++this._gen;
    this._src = makeSource(src);
    try {
      const head = await this._src.read(0, 64);
      const h = parseHeader(head);
      const idx = h.indexCount ? await this._src.read(h.indexOff, h.indexOff + h.indexCount * 8) : new Uint8Array(0);
      if (gen !== this._gen) return;
      const dv = new DataView(idx.buffer, idx.byteOffset, idx.byteLength);
      this.index = [];
      for (let i = 0; i < h.indexCount; i++) this.index.push({ frame: dv.getUint32(i * 8, true), off: dv.getUint32(i * 8 + 4, true) });
      this.h = h;
      this.hasAudio = (h.flags & 1) && h.codec === 1 && h.rate > 0;
      this.canvas.width = h.visW; this.canvas.height = h.visH;
      this.g.fillStyle = '#000'; this.g.fillRect(0, 0, h.visW, h.visH);
      this.readyState = 1;
      this._emit('loadedmetadata'); this._emit('durationchange');
      this._startAt(0, gen);
      this.readyState = 4;
      this._emit('canplay');
    } catch (e) {
      if (gen !== this._gen) return;
      this.error = e; this._emit('error');
    }
  }

  play() {
    if (!this.h) return Promise.resolve();
    if (this.ended) { this.ended = false; this._seek(0); }
    if (!this.paused) return Promise.resolve();
    this.paused = false;
    this._audioCtx();
    const t = this._pausedT;
    const go = () => { if (!this.paused) { this._anchor(t); this._loop(); } };
    this._emit('play');
    // Resume Web Audio before anchoring: a suspended context's clock stands still.
    if (this._ctx && this._ctx.state !== 'running') return this._ctx.resume().catch(() => {}).then(go);
    go(); return Promise.resolve();
  }

  pause() {
    if (this.paused) return;
    this._pausedT = this._now();
    this.paused = true;
    this._stopAudio();
    cancelAnimationFrame(this._raf); this._raf = 0; clearInterval(this._iv);
    this._emit('pause'); this._emit('timeupdate');
  }

  close() {
    this._gen++;
    this.pause();
    if (this._abort) this._abort.abort();
    this.h = null; this.readyState = 0; this.ended = false; this.error = null;
    this._reset(0);
  }

  // ── internals ───────────────────────────────────────────────────────────────────────────────────────────
  _emit(type) {
    const ev = new Event(type);
    this.dispatchEvent(ev);
    const fn = this['on' + type];
    if (typeof fn === 'function') fn.call(this, ev);
  }

  _reset(t) {
    this._pausedT = t; this._baseT = t; this._readT = t;
    this._frames = [];                 // [{t, jpg}] in presentation order, not yet shown
    this._segs = new Map();            // audio segment index -> {pcm: Float32Array(SEG), got}
    this._schedSeg = -1;               // last segment handed to Web Audio
    this._sources = [];
    this._eof = false; this._reading = false; this._pos = -1;   // no read until _startAt places it
    this._shownT = -1; this._pending = null;
    this._wall0 = 0; this._media0 = t; this._clock = null;
  }

  _audioCtx() {
    if (this._ctx || !this.hasAudio) return;
    try {
      this._ctx = new AudioContext({ latencyHint: 'playback' });
      this._gain = this._ctx.createGain(); this._gain.connect(this._ctx.destination);
      this._applyGain();
    } catch { this._ctx = null; }
  }
  _applyGain() { if (this._gain) this._gain.gain.value = this._muted ? 0 : this._vol; }

  // The playhead clock: Web Audio's when it runs (so picture and sound share one clock), else the page's.
  _wall() { return this._clock === 'audio' ? this._ctx.currentTime : performance.now() / 1000; }
  _anchor(t) {
    this._clock = this._ctx && this._ctx.state === 'running' ? 'audio' : 'perf';
    this._wall0 = this._wall() + (this._clock === 'audio' ? 0.08 : 0);   // a little lead for the first segment
    this._media0 = t;
    this._schedSeg = Math.floor(t * (this.h.rate || 1) / SEG) - 1;
    this._waiting = false;
  }
  _now() {
    if (!this.h) return 0;
    if (this.paused || this._waiting || !this._clock) return this._pausedT;
    return Math.max(this._media0, Math.min(this.duration, this._media0 + (this._wall() - this._wall0) * this._rate));
  }

  _stopAudio() {
    for (const s of this._sources) { try { s.stop(); } catch {} }
    this._sources = [];
  }

  _seek(t) {
    const wasPlaying = !this.paused;
    this._stopAudio();
    if (this._abort) this._abort.abort();
    const gen = ++this._gen;
    this.ended = false;
    this._reset(t);
    this._emit('seeking');
    // Dragging the bar fires a seek per mouse move: only the last one within 120 ms opens a read on the device.
    clearTimeout(this._seekTimer);
    this._seekTimer = setTimeout(() => { if (gen === this._gen) this._startAt(t, gen); }, 120);
    if (wasPlaying) this._anchor(t);
    else this._showFirstAt(t, gen);
    this._emit('timeupdate'); this._emit('seeked');
  }

  // Start reading at the index point at or before t (it already points far enough back for the audio).
  _startAt(t, gen) {
    const fi = Math.floor(t * this.h.fps);
    let off = 64;
    for (const e of this.index) { if (e.frame <= fi) off = e.off; else break; }
    this._pos = off;
    this._buf = new Uint8Array(0);
    this._pump(gen);
  }

  async _showFirstAt(t, gen) {
    // Paused seek: wait for the first frame at/after t and paint it, so the scrub shows where it landed.
    for (let i = 0; i < 100 && gen === this._gen; i++) {
      const f = this._frames.find((x) => x.t + 1e-6 >= t) || (this._eof && this._frames[this._frames.length - 1]);
      if (f) { await this._paint(f); return; }
      await new Promise((r) => setTimeout(r, 50));
    }
  }

  async _pump(gen) {
    if (this._reading || this._pos < 0) return;
    this._reading = true;
    try {
      const h = this.h;
      while (gen === this._gen && !this._eof && this._readT - this._now() < AHEAD_S) {
        const end = Math.min(this._pos + PART, h.indexOff);
        if (end <= this._pos) { this._eof = true; break; }
        this._abort = new AbortController();
        let part;
        try { part = await this._src.read(this._pos, end, this._abort.signal); }
        catch (e) { if (gen !== this._gen) return; await new Promise((r) => setTimeout(r, 400)); continue; }  // retry the same part
        if (gen !== this._gen) return;
        this._pos = end;
        const buf = new Uint8Array(this._buf.length + part.length);
        buf.set(this._buf); buf.set(part, this._buf.length);
        let p = 0;
        while (p + 12 <= buf.length) {
          const len = buf[p + 4] | (buf[p + 5] << 8) | (buf[p + 6] << 16) | (buf[p + 7] << 24);
          if (p + 12 + len > buf.length) break;
          const type = buf[p], stamp = (buf[p + 8] | (buf[p + 9] << 8) | (buf[p + 10] << 16) | (buf[p + 11] << 24)) >>> 0;
          const payload = buf.slice(p + 12, p + 12 + len);
          if (type === 1) {
            const ft = stamp / h.fps;
            if (ft + 1 / h.fps >= this._baseT) this._frames.push({ t: ft, jpg: payload });
            this._readT = Math.max(this._readT, ft);
          } else if (type === 2 && this.hasAudio) this._addAudio(stamp, payload);
          p += 12 + len;
        }
        this._buf = buf.subarray(p);
        if (end >= h.indexOff) this._eof = true;
        this._emit('progress');
      }
    } finally { if (gen === this._gen) this._reading = false; }
  }

  _addAudio(stamp, block) {
    const pcm = imaDecodeBlock(block);
    for (let i = 0; i < pcm.length;) {
      const s = stamp + i, k = Math.floor(s / SEG), o = s - k * SEG, n = Math.min(pcm.length - i, SEG - o);
      let seg = this._segs.get(k);
      if (!seg) { seg = { pcm: new Float32Array(SEG), got: 0 }; this._segs.set(k, seg); }
      seg.pcm.set(pcm.subarray(i, i + n), o); seg.got += n;
      i += n;
    }
  }

  _scheduleAudio(now) {
    if (!this._ctx || this._clock !== 'audio') return;
    const rate = this.h.rate, total = Math.ceil(this.h.durationMs / 1000 * rate / SEG);
    for (;;) {
      const k = this._schedSeg + 1;
      const t0 = k * SEG / rate;
      if (k >= total || t0 > now + SCHED_AHEAD_S) break;
      const seg = this._segs.get(k);
      if (!seg || (seg.got < SEG && !this._eof)) break;           // not all of it read yet
      this._schedSeg = k;
      const skip = Math.max(0, this._media0 - t0);                 // first segment after a seek starts part-way
      if (skip >= SEG / rate) continue;
      const buf = this._ctx.createBuffer(1, SEG, rate);
      buf.copyToChannel(seg.pcm, 0);
      const src = this._ctx.createBufferSource();
      src.buffer = buf; src.playbackRate.value = this._rate;
      src.connect(this._gain);
      const when = this._wall0 + (t0 - this._media0) / this._rate;
      src.start(Math.max(when, this._ctx.currentTime), skip);
      src.onended = () => { this._sources = this._sources.filter((x) => x !== src); };
      this._sources.push(src);
    }
    // Drop what is behind the playhead (a whole film of PCM would be ~90 MB).
    for (const k of this._segs.keys()) if ((k + 1) * SEG / rate < now - 1) this._segs.delete(k);
  }

  async _paint(f) {
    const bmp = await createImageBitmap(new Blob([f.jpg], { type: 'image/jpeg' }));
    this.g.drawImage(bmp, 0, 0, this.h.visW, this.h.visH, 0, 0, this.h.visW, this.h.visH);
    bmp.close && bmp.close();
    this._shownT = f.t;
  }

  // One decode at a time; a frame that falls due meanwhile replaces any other still waiting.
  _show(f) {
    if (this._decoding) { this._pending = f; return; }
    this._decoding = true;
    const gen = this._gen;
    this._paint(f).catch(() => {}).finally(() => {
      this._decoding = false;
      const n = this._pending; this._pending = null;
      if (n && gen === this._gen) this._show(n);
    });
  }

  _loop() {
    cancelAnimationFrame(this._raf);
    clearInterval(this._iv);
    // rAF stops in a hidden tab; this keeps the reader and the sound going there.
    this._iv = setInterval(() => {
      if (this.paused || !this.h || this._waiting) return;
      if (!this._reading) this._pump(this._gen);
      if (this.hasAudio) this._scheduleAudio(this._now());
    }, 250);
    let lastTu = 0;
    const tick = () => {
      if (this.paused || !this.h) return;
      this._raf = requestAnimationFrame(tick);
      const gen = this._gen;
      let now = this._now();
      // Starved (the SD/Wi-Fi fell behind): hold the clock until a second is buffered, like <video> does.
      if (!this._waiting && !this._eof && this._readT < now) {
        this._pausedT = now; this._waiting = true; this._stopAudio(); this._emit('waiting');
      } else if (this._waiting && (this._eof || this._readT >= this._pausedT + 1)) {
        this._anchor(this._pausedT); this._emit('playing'); now = this._now();
      }
      if (!this._reading) this._pump(gen);
      if (this._waiting) return;
      if (this.hasAudio) this._scheduleAudio(now);
      // The newest frame that is due; anything older is late and skipped.
      let f = null;
      while (this._frames.length && this._frames[0].t <= now + 0.001) f = this._frames.shift();
      if (f) this._show(f);
      if (now - lastTu > 0.25 || now < lastTu) { lastTu = now; this._emit('timeupdate'); }
      if (this._eof && !this._frames.length && now >= this.duration - 0.05) {
        this._pausedT = this.duration; this.paused = true; this.ended = true;
        this._stopAudio(); cancelAnimationFrame(this._raf); this._raf = 0; clearInterval(this._iv);
        this._emit('timeupdate'); this._emit('pause'); this._emit('ended');
      }
    };
    this._raf = requestAnimationFrame(tick);
  }
}

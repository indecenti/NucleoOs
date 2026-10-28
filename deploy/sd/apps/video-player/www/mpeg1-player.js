/* SPDX-License-Identifier: AGPL-3.0-or-later */
// mpeg1-player.js — motore MPEG-1 (.mpg/.mpeg program stream, .m1v elementary stream) per il
// Video Player web. Il browser non decodifica MPEG-1: lo fa pl_mpeg compilato in WebAssembly
// (mpeg1.wasm, sorgenti e script di build in ./wasm/). Qui: alimentazione a blocchi dalla rete,
// orologio (audio master), rendering YCbCr → RGB (BT.601 limited range) con WebGL e ripiego 2D,
// audio MP2 → WebAudio, seek per stima di byte + aggancio a un I-frame + decodifica fino al
// fotogramma esatto, durata dai PTS di testa e coda (Range) o stimata (server senza Range).

import { SoftEngine, MediaError } from './soft-engine.js';
import { BLOCK } from './byte-source.js';

const PROBE = 256 * 1024;         // testa/coda lette per sonde e durata (un blocco)
const FEED_MIN = 1024 * 1024;     // byte non ancora demultiplexati da tenere nel decoder
const PREFETCH_S = 3.0;
const AUDIO_AHEAD = 0.7;
const AUDIO_BATCH = 4;            // blocchi MP2 (1152 campioni) per AudioBuffer
const VIDEO = 0xE0, AUDIO1 = 0xC0;

let modulePromise = null;
function loadModule() {
  if (!modulePromise) {
    modulePromise = fetch(new URL('./mpeg1.wasm', import.meta.url))
      .then((r) => { if (!r.ok) throw new MediaError('errDecoderLoad', 'HTTP ' + r.status); return r.arrayBuffer(); })
      .then((b) => WebAssembly.compile(b))
      .catch((e) => { modulePromise = null; throw e instanceof MediaError ? e : new MediaError('errDecoderLoad', e && e.message); });
  }
  return modulePromise;
}

// ---------------------------------------------------------------- rendering YCbCr

const VS = `attribute vec2 p; varying vec2 uv; uniform vec2 crop;
void main(){ uv = vec2((p.x+1.0)*0.5, (1.0-p.y)*0.5) * crop; gl_Position = vec4(p, 0.0, 1.0); }`;
const FS = `precision mediump float; varying vec2 uv; uniform sampler2D ty, tcb, tcr;
const mat4 bt601 = mat4(
  1.16438,  0.00000,  1.59603, -0.87079,
  1.16438, -0.39176, -0.81297,  0.52959,
  1.16438,  2.01723,  0.00000, -1.08139,
  0.0, 0.0, 0.0, 1.0);
void main(){
  float y = texture2D(ty, uv).r, cb = texture2D(tcb, uv).r, cr = texture2D(tcr, uv).r;
  gl_FragColor = vec4(y, cb, cr, 1.0) * bt601;
}`;

class GLRenderer {
  constructor(canvas) {
    const gl = canvas.getContext('webgl', { alpha: false, antialias: false, depth: false, stencil: false, preserveDrawingBuffer: false, powerPreference: 'low-power' })
      || canvas.getContext('experimental-webgl');
    if (!gl) throw new Error('no webgl');
    this.gl = gl;
    const sh = (type, src) => { const s = gl.createShader(type); gl.shaderSource(s, src); gl.compileShader(s); if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s)); return s; };
    const prog = gl.createProgram();
    gl.attachShader(prog, sh(gl.VERTEX_SHADER, VS));
    gl.attachShader(prog, sh(gl.FRAGMENT_SHADER, FS));
    gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(prog));
    gl.useProgram(prog);
    this.prog = prog;
    const buf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(prog, 'p');
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    this.crop = gl.getUniformLocation(prog, 'crop');
    this.tex = ['ty', 'tcb', 'tcr'].map((name, i) => {
      const t = gl.createTexture();
      gl.activeTexture(gl.TEXTURE0 + i);
      gl.bindTexture(gl.TEXTURE_2D, t);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.uniform1i(gl.getUniformLocation(prog, name), i);
      return { t, w: 0, h: 0 };
    });
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    this.lost = false;
    canvas.addEventListener('webglcontextlost', (e) => { e.preventDefault(); this.lost = true; });
  }
  draw(canvas, planes, dispW, dispH) {
    const gl = this.gl;
    if (this.lost) return false;
    planes.forEach((pl, i) => {
      const tx = this.tex[i];
      gl.activeTexture(gl.TEXTURE0 + i);
      gl.bindTexture(gl.TEXTURE_2D, tx.t);
      if (tx.w !== pl.w || tx.h !== pl.h) {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, pl.w, pl.h, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, pl.data);
        tx.w = pl.w; tx.h = pl.h;
      } else gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, pl.w, pl.h, gl.LUMINANCE, gl.UNSIGNED_BYTE, pl.data);
    });
    gl.viewport(0, 0, canvas.width, canvas.height);
    gl.uniform2f(this.crop, dispW / planes[0].w, dispH / planes[0].h);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    return true;
  }
}

// ---------------------------------------------------------------- motore

export class Mpeg1Engine extends SoftEngine {
  constructor(opts) {
    loadModule().catch(() => {});   // il decoder parte subito, prima della prima lettura del file
    super(opts);
    this.info.engine = 'MPEG-1 (pl_mpeg/WASM)';
  }

  async _open() {
    const src = this.src;
    let [mod, head] = await Promise.all([loadModule(), src.read(0, Math.min(src.size != null ? src.size : PROBE, PROBE))]);
    if (head.length < 16) throw new MediaError('errNotMpeg');
    const inst = await WebAssembly.instantiate(mod, {});
    this.X = inst.exports;
    if (this.X._initialize) this.X._initialize();

    // ---- tipo di flusso
    const ps = this._findCode(head, 0xBA, 4096);
    const seq = this._findCode(head, 0xB3, 65536);
    if (ps >= 0) {
      if ((head[ps + 4] & 0xC0) === 0x40) throw new MediaError('errMpeg2');   // pack header MPEG-2
      this.es = false;
      this.info.container = 'MPEG-1 PS';
    } else if (seq >= 0 && seq < 1024) {
      this.es = true;
      this.info.container = 'MPEG-1 ES';
    } else throw new MediaError('errNotMpeg');
    if (seq >= 0 && this._isMpeg2Video(head, seq)) throw new MediaError('errMpeg2');

    const X = this.X;
    this.dec = X.mp_create(this.es ? 1 : 0);
    if (!this.dec) throw new MediaError('errDecode', 'oom');

    if (!this.es) {
      let mask = this._scan(head, 0, 0);
      if (!(mask & 2) && (src.size == null || src.size > head.length)) {
        // il system header dichiara un audio che nei primi 256 KB non c'è ancora (bitrate alto):
        // si guarda un po' più avanti. Se non ne dichiara, niente letture in più.
        this._write(head);
        const decl = X.mp_declared_streams(this.dec);
        X.mp_reset(this.dec, 0);
        if (decl < 0 || (decl & 0xff) > 0) {
          head = await src.read(0, 3 * PROBE);
          mask = this._scan(head, 0, 0);
        }
      }
      if (!(mask & 1)) throw new MediaError('errNoVideo');
      this.atype = 0;
      for (let k = 0; k < 4; k++) if (mask & (2 << k)) { this.atype = AUDIO1 + k; break; }
      X.mp_set_streams(this.dec, VIDEO, this.atype);
      this.startPts = this._scan(head, 1, VIDEO);
      if (this.startPts < 0) throw new MediaError('errCorrupt', 'pts');
      this.dataStart = Math.max(0, ps);
    } else {
      this.atype = 0;
      this.startPts = 0;
      this.dataStart = Math.max(0, seq);
    }

    // ---- intestazioni: si alimenta la testa e si leggono le dimensioni/fps/audio
    X.mp_reset(this.dec, this.startPts);
    this.feedPos = 0;
    this.endSignaled = false;
    this._write(head);
    this.feedPos = head.length;
    if (src.size != null && this.feedPos >= src.size) { X.mp_end(this.dec); this.endSignaled = true; }
    if (!X.mp_has_video_header(this.dec)) throw new MediaError('errCorrupt', 'sequence header');
    const w = X.mp_width(this.dec), h = X.mp_height(this.dec);
    this.fps = X.mp_framerate(this.dec) || 25;
    let par = X.mp_pixel_aspect(this.dec) || 1;
    if (!(par > 0.3 && par < 3)) par = 1;
    this.info.vcodec = 'MPEG-1 video';
    this.info.fpsNominal = this.fps;
    this._setSize(w, h, w / par);   // pel_aspect_ratio MPEG-1 = altezza/larghezza del pixel
    this.hasAudio = !!(this.atype && this.audio);
    if (this.atype) {
      this.sampleRate = X.mp_samplerate(this.dec);
      if (!this.sampleRate) { this.hasAudio = false; this.atype = 0; X.mp_set_streams(this.dec, VIDEO, 0); }
      else this.info.acodec = 'MPEG-1 Layer II · ' + this.sampleRate + ' Hz · ' + (X.mp_audio_channels(this.dec) === 1 ? 'mono' : 'stereo') + ' · ' + X.mp_audio_bitrate(this.dec) + ' kbps';
    }
    if (!this.hasAudio && this.atype) X.mp_set_streams(this.dec, VIDEO, 0);

    // ---- durata
    await this._probeDuration(head);

    // ---- renderer
    try { this.gl = new GLRenderer(this.canvas); this.renderer = 'WebGL'; }
    catch (e) { this.gl = null; this.renderer = 'Canvas 2D'; }

    // ---- primo fotogramma
    X.mp_reset(this.dec, this.startPts);
    this.feedPos = this.dataStart;
    this.endSignaled = false;
    this._abatch = null;
    await this._decodeUntil(0, true);
    this._openedAt = performance.now();
  }

  // ---- utilità wasm
  _mem() { return new Uint8Array(this.X.memory.buffer); }
  _write(bytes) {
    const X = this.X;
    for (let o = 0; o < bytes.length; o += 1024 * 1024) {
      const part = bytes.subarray(o, Math.min(bytes.length, o + 1024 * 1024));
      const p = X.mp_input(this.dec, part.length);
      if (!p) throw new MediaError('errDecode', 'oom');
      this._mem().set(part, p);
      X.mp_write(this.dec, part.length);
    }
  }
  _scan(bytes, what, type) {
    const X = this.X;
    const p = X.mp_malloc(bytes.length || 1);
    if (!p) return -1;
    this._mem().set(bytes, p);
    const r = X.mp_scan(p, bytes.length, what, type);
    X.mp_free(p);
    return r;
  }
  _findCode(u, code, limit) {
    const n = Math.min(u.length - 4, limit);
    for (let i = 0; i < n; i++) if (u[i] === 0 && u[i + 1] === 0 && u[i + 2] === 1 && u[i + 3] === code) return i;
    return -1;
  }
  // Un'estensione di sequenza (00 00 01 B5, id 1) dopo l'header = MPEG-2 video (non supportato).
  _isMpeg2Video(u, seq) {
    const n = Math.min(u.length - 5, seq + 4096);
    for (let i = seq + 4; i < n; i++) {
      if (u[i] === 0 && u[i + 1] === 0 && u[i + 2] === 1) {
        if (u[i + 3] === 0xB5) return (u[i + 4] >> 4) === 1;
        if (u[i + 3] === 0x00 || u[i + 3] === 0xB8) return false;
      }
    }
    return false;
  }

  async _probeDuration(head) {
    const src = this.src, X = this.X;
    const size = src.size;
    this.durationExact = false;
    if (this.es) {
      // niente PTS: time_code degli header di GOP (esatti con ffmpeg), altrimenti byte medi per fotogramma
      let pics = 0;
      for (let i = 0; i + 4 < head.length; i++) if (head[i] === 0 && head[i + 1] === 0 && head[i + 2] === 1 && head[i + 3] === 0) pics++;
      const per = pics > 1 ? head.length / pics : 8192;
      this.byterate = per * this.fps;
      const g0 = this._esGops(head);
      this.gop0 = g0.length ? g0[0].t : 0;
      if (size != null && size <= head.length) { const d = this._esDurationFromTail(head); if (d > 0) { this._setDuration(d, true); return; } }
      if (src.ranged && size != null) {
        const from = Math.max(0, size - PROBE);
        const d = this._esDurationFromTail(await src.read(from, size - from, true));
        if (d > 0) { this._setDuration(d, true); this.byterate = (size - this.dataStart) / d; return; }
      }
      if (size != null) this._setDuration(size / per / this.fps, false);
      return;
    }
    const lastHead = this._scan(head, 2, VIDEO);
    if (size != null && size <= head.length) {
      this._setDuration(lastHead - this.startPts + 1 / this.fps, true);
    } else if (src.ranged && size != null) {
      // durata esatta dalla coda, letta DOPO il primo fotogramma: la riproduzione parte subito con
      // una stima (sulla board ogni blocco in più all'apertura costa secondi)
      this._tailProbe = async () => {
        for (const span of [64 * 1024, PROBE, 4 * PROBE]) {
          if (this._destroyed || this._durationExact) return;
          const from = Math.max(0, size - span);
          const tail = await src.fetchRange(from, size - from);
          if (this._destroyed || !this.X) return;
          if (this._durationFromTail(tail)) return;
        }
      };
    }
    if (!this._durationExact && size != null) {
      // stima dal bitrate dei primi blocchi (senza Range si affina mentre il flusso scorre e diventa
      // esatta quando il download arriva alla fine; con Range la sostituisce la sonda della coda)
      let buf = head, last = lastHead;
      if (last - this.startPts < 1.0 && size > 4 * PROBE && !src.ranged) {
        buf = await src.read(0, 4 * PROBE, true);
        last = this._scan(buf, 2, VIDEO);
      }
      const spanT = last - this.startPts;
      if (spanT > 0.2) this._setDuration(spanT * (size - this.dataStart) / (buf.length - this.dataStart), false);
      else this._setDuration(size / (1150 * 1000 / 8), false);
    }
    this.byterate = size != null && this._duration > 0 ? (size - this.dataStart) / this._duration : 150000;
  }

  // Header di GOP (00 00 01 B8) di un blocco ES con il loro time_code in secondi.
  _esGops(u) {
    const out = [];
    for (let i = 0; i + 8 < u.length; i++) {
      if (u[i] !== 0 || u[i + 1] !== 0 || u[i + 2] !== 1 || u[i + 3] !== 0xB8) continue;
      const tc = (u[i + 4] << 17) | (u[i + 5] << 9) | (u[i + 6] << 1) | (u[i + 7] >> 7);   // 25 bit
      const hh = (tc >> 19) & 0x1f, mm = (tc >> 13) & 0x3f, ss = (tc >> 6) & 0x3f, pp = tc & 0x3f;
      out.push({ pos: i, t: hh * 3600 + mm * 60 + ss + pp / this.fps });
    }
    return out;
  }
  _esDurationFromTail(tail) {
    const g = this._esGops(tail);
    if (!g.length) return 0;
    const last = g[g.length - 1];
    let pics = 0;
    for (let i = last.pos; i + 4 < tail.length; i++) if (tail[i] === 0 && tail[i + 1] === 0 && tail[i + 2] === 1 && tail[i + 3] === 0) pics++;
    return last.t - (this.gop0 || 0) + pics / this.fps;
  }

  // Durata esatta dagli ultimi PTS (video, e audio se più lungo) di un blocco di coda.
  _durationFromTail(tail) {
    const last = this._scan(tail, 2, VIDEO);
    if (last < 0) return false;
    let d = last - this.startPts + 1 / this.fps;
    if (this.hasAudio && this.atype) {
      const la = this._scan(tail, 2, this.atype);
      if (la >= 0) d = Math.max(d, la - this.startPts + 1152 / this.sampleRate);
    }
    this._setDuration(d, true);
    this.byterate = this.src.size ? (this.src.size - this.dataStart) / d : this.byterate;
    return true;
  }

  // ---- alimentazione
  _feed(maxBlocks = 8) {
    const src = this.src, X = this.X;
    let fed = false;
    while (maxBlocks-- > 0 && X.mp_remaining(this.dec) < Math.max(FEED_MIN, this.byterate * 1.5)) {
      if (src.size != null && this.feedPos >= src.size) break;
      const i = Math.floor(this.feedPos / BLOCK);
      const b = src.peek(i);
      if (!b) { src.getBlock(i, true).catch(() => {}); break; }
      const s = this.feedPos - i * BLOCK;
      if (s >= b.length) { if (src.size == null && src.eof) break; this.feedPos = (i + 1) * BLOCK; continue; }
      this._write(b.subarray(s));
      this.feedPos += b.length - s;
      fed = true;
    }
    if (src.size != null && this.feedPos >= src.size && !this.endSignaled) {
      X.mp_end(this.dec);
      this.endSignaled = true;
    }
    // server senza Range: appena il download è completo la coda è in cache → durata esatta
    if (!this._durationExact && !this.es && src.eof && src.size && !this._tailProbed) {
      this._tailProbed = true;
      const from = Math.max(0, src.size - PROBE);
      const tail = src.readSync(from, src.size - from);
      if (tail) this._durationFromTail(tail);
    }
    if (typeof src.setPosition === 'function') src.setPosition(this.feedPos);
    const i = Math.floor(this.feedPos / BLOCK);
    src.prefetch(i, i + Math.max(1, Math.ceil(this.byterate * Math.max(PREFETCH_S, this._resumeSecs() + 0.5) / BLOCK)));
    return fed;
  }

  // ---- video
  _planes() {
    const X = this.X, c = this.dec, mem = this.X.memory.buffer;
    const lw = X.mp_luma_width(c), lh = X.mp_luma_height(c), cw = X.mp_chroma_width(c), ch = X.mp_chroma_height(c);
    return [
      { w: lw, h: lh, data: new Uint8Array(mem, X.mp_frame_y(c), lw * lh) },
      { w: cw, h: ch, data: new Uint8Array(mem, X.mp_frame_cb(c), cw * ch) },
      { w: cw, h: ch, data: new Uint8Array(mem, X.mp_frame_cr(c), cw * ch) },
    ];
  }
  _render() {
    if (document.hidden && !this._paused) return;   // finestra nascosta: si decodifica senza disegnare
    const planes = this._planes();
    if (this.gl && this.gl.draw(this.canvas, planes, this.videoWidth, this.videoHeight)) { this._markShown(); return; }
    // ripiego 2D (niente WebGL)
    const X = this.X, w = this.videoWidth, h = this.videoHeight;
    if (!this.rgba || this.rgbaLen !== w * h * 4) { if (this.rgba) X.mp_free(this.rgba); this.rgbaLen = w * h * 4; this.rgba = X.mp_malloc(this.rgbaLen); }
    X.mp_frame_to_rgba(this.dec, this.rgba, w * 4);
    const img = new ImageData(new Uint8ClampedArray(this.X.memory.buffer.slice(this.rgba, this.rgba + this.rgbaLen)), w, h);
    if (!this.off) { this.off = document.createElement('canvas'); }
    if (this.off.width !== w || this.off.height !== h) { this.off.width = w; this.off.height = h; }
    this.off.getContext('2d').putImageData(img, 0, 0);
    if (!this.g2) this.g2 = this.canvas.getContext('2d');
    if (this.g2) this.g2.drawImage(this.off, 0, 0, this.canvas.width, this.canvas.height);
    this._markShown();
  }

  // Decodifica (senza pacing) fino al primo fotogramma valido con tempo ≥ t - mezzo fotogramma,
  // poi lo disegna. Usato per il poster e per il seek esatto.
  async _decodeUntil(t, first) {
    const X = this.X, c = this.dec;
    const target = this._seekTarget;
    const half = 0.5 / this.fps;
    let have = false, guard = 0;
    for (;;) {
      if (!first && target !== this._seekTarget) return false;
      const nt = X.mp_video_next_time(c);
      if (have && nt >= 0 && nt > t + half) break;
      const r = X.mp_decode_video(c);
      if (r === 1) {
        this.stats.decoded++;
        if (X.mp_frame_valid(c)) { have = true; this._lastVT = X.mp_frame_time(c); if (this._lastVT >= t - half) break; }
        continue;
      }
      if (r === -1) break;
      // servono dati
      if (!this._feed()) {
        if (this.src.size != null && this.feedPos >= this.src.size) { if (!this.endSignaled) { X.mp_end(c); this.endSignaled = true; } continue; }
        await this.src.getBlock(Math.floor(this.feedPos / BLOCK), true);
        if (++guard > 4000) break;
      }
    }
    if (have) this._render();
    else if (first) throw new MediaError('errNoFrames');
    this.videoDone = false;
    return have;
  }

  _decodeAudio(until) {
    const X = this.X, c = this.dec;
    const n = X.mp_samples_count();
    let budget = 48;
    while (budget-- > 0) {
      const next = X.mp_audio_next_time(c);
      if (next >= 0 && next >= until) break;
      const r = X.mp_decode_audio(c);
      if (r !== 1) { if (r === 0) this._feed(2); break; }
      if (!X.mp_audio_valid(c)) continue;
      const t0 = X.mp_samples_time(c);
      const src = new Float32Array(X.memory.buffer, X.mp_samples_ptr(c), n * 2);
      let b = this._abatch;
      if (!b || b.fill === b.cap || Math.abs(b.t0 + b.fill / this.sampleRate - t0) > 0.002) {
        if (b && b.fill) this._flushBatch();
        b = this._abatch = { t0, fill: 0, cap: n * AUDIO_BATCH, L: new Float32Array(n * AUDIO_BATCH), R: new Float32Array(n * AUDIO_BATCH) };
      }
      for (let i = 0, j = b.fill; i < n; i++, j++) { b.L[j] = src[2 * i]; b.R[j] = src[2 * i + 1]; }
      b.fill += n;
      if (b.fill === b.cap) this._flushBatch();
    }
    // non lasciare un batch parziale troppo a lungo vicino all'istante di riproduzione
    const b = this._abatch;
    if (b && b.fill && b.t0 < this._now() + 0.25) this._flushBatch();
  }
  _flushBatch() {
    const b = this._abatch;
    if (!b || !b.fill) return;
    this.audio.push(b.t0, this.sampleRate, [b.L.subarray(0, b.fill), b.R.subarray(0, b.fill)]);
    this._abatch = null;
  }

  // Senza Range: ogni ~2 s la stima della durata si ricalcola sull'ultimo blocco arrivato.
  _refineDuration() {
    const src = this.src;
    if (this._durationExact || this.es || src.ranged || !src.size) return;
    const t = performance.now();
    if (t - (this._lastRefine || 0) < 2000) return;
    this._lastRefine = t;
    const i = Math.floor((src.streamPos || 0) / BLOCK) - 1;
    const b = i >= 1 ? src.peek(i) : null;
    if (!b) return;
    const last = this._scan(b, 2, VIDEO);
    const end = (i + 1) * BLOCK - this.dataStart;
    if (last > this.startPts + 1 && end > 0) {
      const d = (last - this.startPts) * (src.size - this.dataStart) / end;
      if (Math.abs(d - this._duration) / d > 0.005) { this._setDuration(d, false); this.byterate = (src.size - this.dataStart) / d; }
    }
  }

  _tick(now) {
    const X = this.X, c = this.dec;
    // sonda della coda (durata esatta) quando c'è già un po' di margine, per non rubare banda
    // alla partenza della riproduzione
    if (this._tailProbe && (this._bufferedAhead() >= 2 || this._paused || performance.now() - this._openedAt > 6000)) {
      const p = this._tailProbe; this._tailProbe = null; p().catch(() => {});
    }
    this._refineDuration();
    this._feed();
    if (this.hasAudio) this._decodeAudio(now + AUDIO_AHEAD);

    // video: decodifica i fotogrammi scaduti; se si è in ritardo si disegna solo l'ultimo
    const t0 = performance.now();
    let pending = false;
    this.videoStarved = false;
    for (;;) {
      const nt = X.mp_video_next_time(c);
      if (nt >= 0 && nt > now) break;
      const r = X.mp_decode_video(c);
      if (r === 1) {
        this.stats.decoded++;
        if (X.mp_frame_valid(c)) {
          if (pending) this.stats.dropped++;
          pending = true;
          this._lastVT = X.mp_frame_time(c);
        }
        if (performance.now() - t0 > 24) break;   // non bloccare l'interfaccia
        continue;
      }
      if (r === 0) {
        if (this._feed()) continue;
        this.videoStarved = !(this.src.size != null && this.feedPos >= this.src.size);
        break;
      }
      this.videoDone = true;
      break;
    }
    if (pending) this._render();
  }
  async _seek(t) {
    const X = this.X, c = this.dec, src = this.src;
    const target = this._seekTarget;
    if (this.audio) this.audio.clear();
    this._abatch = null;
    const es = this.es ? await this._seekOffsetES(t) : null;
    const off = es ? es.off : await this._seekOffsetPS(t);
    if (target !== this._seekTarget) return;
    X.mp_reset(c, this.startPts);
    if (es) X.mp_set_video_time(c, Math.max(0, es.t));
    this.feedPos = off;
    this.endSignaled = false;
    this.videoDone = false;
    const i = Math.floor(off / BLOCK);
    src.cancelPrefetch(i, i + 8);
    await this._decodeUntil(t, false);
  }

  // Offset di un pacchetto video con I-frame poco prima di t (PTS verificati su blocchi letti).
  async _seekOffsetPS(t) {
    const src = this.src;
    const size = src.size != null ? src.size : this.feedPos + BLOCK;
    const target = this._seekTarget;
    if (t < 0.3) return this.dataStart;
    let br = this.byterate || 150000;
    let est = this.dataStart + t * br - 256 * 1024;
    let best = null;
    const SPAN = 256 * 1024;
    for (let iter = 0; iter < 8; iter++) {
      est = Math.max(this.dataStart, Math.min(est, size - 4096));
      const at = Math.floor(est);
      const buf = await src.read(at, Math.min(SPAN, size - at));
      if (target !== this._seekTarget) return best != null ? best : this.dataStart;
      // tutti gli I-frame del blocco con il loro PTS
      const found = [];
      let o = 0;
      while (o < buf.length - 16) {
        const rel = this._scan(buf.subarray(o), 4, VIDEO);
        if (rel < 0) break;
        const pos = o + rel;
        const pts = this._scan(buf.subarray(pos), 1, VIDEO);
        if (pts >= 0) found.push({ pos: at + pos, t: pts - this.startPts });
        o = pos + 8;
      }
      if (!found.length) { est += SPAN; if (est >= size - 4096) break; continue; }
      // bitrate medio misurato dall'inizio del file fino a questo punto (secante): converge in 2-3 salti
      const lastF = found[found.length - 1];
      if (lastF.t > 2) br = Math.max(4000, (lastF.pos - this.dataStart) / lastF.t);
      const before = found.filter((f) => f.t <= t + 0.02);
      if (before.length) {
        const f = before[before.length - 1];
        best = f.pos;
        if (t - f.t <= 3.0 || found.length > before.length) return f.pos;   // abbastanza vicino
        // troppo presto: salta avanti usando il bitrate locale
        est = f.pos + (t - f.t - 1.0) * br;
        continue;
      }
      // tutti dopo t: torna indietro
      const f = found[0];
      if (at <= this.dataStart) return this.dataStart;
      br = Math.max(20000, br);
      est = at - (f.t - t + 1.5) * br;
    }
    return best != null ? best : this.dataStart;
  }

  // ES: si aggancia all'header di GOP con time_code ≤ t (tempo esatto), altrimenti stima dai byte.
  async _seekOffsetES(t) {
    const src = this.src;
    const size = src.size || 0;
    if (t < 0.3) return { off: this.dataStart, t: 0 };
    let est = Math.floor(this.dataStart + t * (this.byterate || 150000)) - 128 * 1024;
    let best = null;
    for (let iter = 0; iter < 6; iter++) {
      est = Math.max(this.dataStart, Math.min(est, size - 4096));
      const buf = await src.read(est, 256 * 1024);
      const gops = this._esGops(buf).map((g) => ({ off: est + g.pos, t: g.t - (this.gop0 || 0) }));
      if (!gops.length) break;
      const before = gops.filter((g) => g.t <= t + 0.001);
      if (before.length) {
        best = before[before.length - 1];
        if (t - best.t <= 3 || before.length < gops.length) return best;
        est = best.off + (t - best.t - 1) * this.byterate;
        continue;
      }
      if (est <= this.dataStart) return { off: this.dataStart, t: 0 };
      est -= (gops[0].t - t + 1.5) * this.byterate;
    }
    if (best) return best;
    // nessun time_code: stima proporzionale e primo start code utile
    est = Math.max(this.dataStart, Math.min(Math.floor(this.dataStart + t * (this.byterate || 150000)), size - 4096));
    const buf = await src.read(est, 256 * 1024);
    for (let i = 0; i + 4 < buf.length; i++) if (buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 1 && (buf[i + 3] === 0xB3 || buf[i + 3] === 0xB8)) return { off: est + i, t: (est + i - this.dataStart) / Math.max(1, this.byterate) };
    return { off: est, t: (est - this.dataStart) / Math.max(1, this.byterate) };
  }

  _starved(now) {
    if (this.videoDone) return false;
    if (!this.videoStarved) return false;
    const nt = this.X.mp_video_next_time(this.dec);
    return nt < 0 || nt <= now;
  }
  _freezeTime() { const nt = this.X.mp_video_next_time(this.dec); return nt >= 0 ? nt - 1e-3 : NaN; }
  _canResume(now) {
    if (this.src.size != null && this.feedPos >= this.src.size) return true;
    const need = Math.min(this.byterate * this._resumeSecs(), (this.src.size || Infinity) - this.feedPos);
    const i = Math.floor(this.feedPos / BLOCK);
    return this.src.has(this.feedPos, Math.max(1, need)) || (this.X.mp_remaining(this.dec) > this.byterate * 0.8 && !!this.src.peek(i));
  }
  _atEnd(now) {
    if (this._durationExact && now >= this._duration - 1e-3) return true;
    if (!this.videoDone) return false;
    if (this.hasAudio && this.audio && this.audio.end > now + 0.02) return false;
    if (!this._durationExact) this._setDuration(Math.max(now, (this._lastVT || 0) + 1 / this.fps), true);
    return true;
  }
  _bufferedAhead() {
    const now = this._now();
    const aheadBytes = Math.max(0, this.feedPos - this._consumedPos());
    let ahead = aheadBytes / Math.max(1, this.byterate);
    let p = this.feedPos;
    while (this.src.has(p, 1) && (this.src.size == null || p < this.src.size) && ahead < 600) { const nb = (Math.floor(p / BLOCK) + 1) * BLOCK; ahead += (nb - p) / this.byterate; p = nb; }
    return Math.max(0, ahead - Math.max(0, now - (this._lastVT || now)));
  }
  _consumedPos() { return this.feedPos - this.X.mp_remaining(this.dec) - this.X.mp_queued(this.dec); }
  _bufferedRanges() {
    const d = this._duration;
    if (!(d > 0) || !this.src.size) return [];
    const span = this.src.size - this.dataStart;
    return this.src.cachedRanges().map(([s, e]) => [Math.max(0, (s - this.dataStart) / span * d), Math.min(d, (e - this.dataStart) / span * d)]);
  }
  _extraStats() {
    return { renderer: this.renderer, seekIndex: 'PTS', startPts: this.startPts };
  }
  _destroy() {
    try { if (this.X && this.dec) { this.X.mp_destroy(this.dec); this.dec = 0; } } catch {}
    this.X = null;
  }
}

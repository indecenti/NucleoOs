/* SPDX-License-Identifier: AGPL-3.0-or-later */
// soft-engine.js — base comune dei player "software" (AVI/MJPEG e MPEG-1) del Video Player.
//
// Espone lo stesso sottoinsieme di API di HTMLVideoElement che usa l'app (paused, currentTime,
// duration, volume, muted, playbackRate, play()/pause(), ended, buffered, seeking, readyState,
// videoWidth/videoHeight, error, dataset + eventi play/pause/playing/waiting/seeking/seeked/
// timeupdate/progress/loadedmetadata/durationchange/ended/error/volumechange/ratechange), così
// tutti i controlli dell'app funzionano identici col <video> nativo e con questi motori.
//
// Orologio: se il file ha audio e l'AudioContext è attivo, il tempo è quello dell'hardware audio
// (ctx.currentTime) e i buffer PCM sono programmati esattamente ai loro istanti: audio e video
// non possono derivare. Senza audio (o con l'audio bloccato dalla policy di autoplay) si usa
// performance.now(); appena l'utente interagisce l'audio si aggancia senza salti.

import { SourceError } from './byte-source.js';

export class MediaError extends Error {
  constructor(key, detail, vars) { super(key + (detail ? ': ' + detail : '')); this.key = key; this.detail = detail || ''; this.vars = vars || null; }
}

const AUDIO_LEAD = 0.6;       // secondi di audio programmati in anticipo
const STALL_MS = 180;         // dati mancanti per più di così → stato "waiting"
const TIMEUPDATE_MS = 250;

// ---------------------------------------------------------------- AudioContext condiviso

let sharedCtx = null;
export function audioContext() {
  if (sharedCtx) return sharedCtx;
  const AC = window.AudioContext || window.webkitAudioContext;
  if (!AC) return null;
  try { sharedCtx = new AC({ latencyHint: 'playback' }); } catch { try { sharedCtx = new AC(); } catch { return null; } }
  const tryResume = () => { if (sharedCtx.state === 'suspended') sharedCtx.resume().catch(() => {}); };
  tryResume();
  // Policy di autoplay: il primo gesto dell'utente sblocca l'audio (listener permanenti e passivi:
  // il contesto può tornare "suspended" se il sistema lo interrompe).
  for (const ev of ['pointerdown', 'keydown', 'touchend', 'click']) window.addEventListener(ev, tryResume, { capture: true, passive: true });
  return sharedCtx;
}

// ---------------------------------------------------------------- orologio

class Clock {
  constructor() { this.ctx = null; this.audio = false; this.rate = 1; this.running = false; this.base = 0; this.anchor = 0; }
  wall() { return this.audio ? this.ctx.currentTime : performance.now() / 1000; }
  now() { return this.running ? this.base + (this.wall() - this.anchor) * this.rate : this.base; }
  start() { if (!this.running) { this.anchor = this.wall(); this.running = true; } }
  stop() { if (this.running) { this.base = this.now(); this.running = false; } }
  set(t) { this.base = t; this.anchor = this.wall(); }
  setRate(r) { const t = this.now(); this.rate = r; this.set(t); }
  useAudio(ctx, on) { const t = this.now(); this.ctx = ctx; this.audio = !!on; this.set(t); }
}

// ---------------------------------------------------------------- uscita audio

export class AudioOut {
  constructor(ctx) {
    this.ctx = ctx;
    this.gain = ctx.createGain();
    this.gain.connect(ctx.destination);
    this.chunks = [];          // [{t0, t1, buf, node}] in ordine di tempo
    this.live = new Set();
    this.trace = [];
  }
  // planes: un Float32Array per canale, tutti lunghi uguali.
  push(t0, sampleRate, planes) {
    const n = planes[0].length;
    if (!n) return;
    let buf;
    try { buf = this.ctx.createBuffer(planes.length, n, sampleRate); } catch { return; }
    for (let c = 0; c < planes.length; c++) buf.copyToChannel(planes[c], c);
    this.chunks.push({ t0, t1: t0 + n / sampleRate, buf, node: null });
  }
  get end() { return this.chunks.length ? this.chunks[this.chunks.length - 1].t1 : -Infinity; }
  // Programma i pezzi che iniziano prima di `until` (tempo media). `clock` deve usare l'audio.
  pump(clock, until, schedule) {
    const mediaNow = clock.now();
    while (this.chunks.length && this.chunks[0].t1 < mediaNow - 0.25 && !this.chunks[0].node) this.chunks.shift();
    while (this.chunks.length && this.chunks[0].node && this.chunks[0].t1 < mediaNow - 1) this.chunks.shift();
    if (!schedule) return;
    const earliest = this.ctx.currentTime + 0.012;
    for (const c of this.chunks) {
      if (c.node) continue;
      if (c.t0 >= until) break;
      let when = clock.anchor + (c.t0 - clock.base) / clock.rate;
      let offset = 0;
      if (when < earliest) {
        offset = (earliest - when) * clock.rate;
        when = earliest;
        if (offset >= c.buf.duration - 0.001) { c.node = 'skip'; continue; }
      }
      const s = this.ctx.createBufferSource();
      s.buffer = c.buf;
      s.playbackRate.value = clock.rate;
      s.connect(this.gain);
      s.onended = () => { this.live.delete(s); try { s.disconnect(); } catch {} };
      s.start(when, offset);
      c.node = s;
      this.live.add(s);
      // traccia degli ultimi pezzi programmati (diagnostica: continuità tra un buffer e l'altro)
      this.trace.push([c.t0, when, offset, c.buf.duration]);
      if (this.trace.length > 64) this.trace.shift();
    }
  }
  // Ferma ciò che è programmato (pausa, cambio velocità, attesa): i pezzi restano in coda.
  flush() {
    for (const s of this.live) { try { s.onended = null; s.stop(); s.disconnect(); } catch {} }
    this.live.clear();
    for (const c of this.chunks) c.node = null;
  }
  clear() { this.flush(); this.chunks = []; }
  setVolume(v) { try { this.gain.gain.setTargetAtTime(v, this.ctx.currentTime, 0.015); } catch { this.gain.gain.value = v; } }
  destroy() { this.clear(); try { this.gain.disconnect(); } catch {} }
}

// ---------------------------------------------------------------- TimeRanges minimale

export function timeRanges(list) {
  const r = (list || []).filter((x) => x[1] > x[0]);
  return { length: r.length, start: (i) => r[i][0], end: (i) => r[i][1] };
}

// ---------------------------------------------------------------- motore base

export class SoftEngine extends EventTarget {
  constructor(opts) {
    super();
    this.opts = opts;
    this.src = opts.source;
    this.container = opts.container;
    this.dataset = {};
    this.canvas = document.createElement('canvas');
    this.canvas.className = 'soft-canvas';
    this.canvas.width = 16; this.canvas.height = 9;
    this.container.appendChild(this.canvas);

    this.clock = new Clock();
    this._paused = true;
    this._ended = false;
    this._duration = NaN;
    this._durationExact = false;
    this._volume = opts.volume != null ? opts.volume : 1;
    this._muted = !!opts.muted;
    this._rate = opts.playbackRate || 1;
    this.clock.rate = this._rate;
    this._seeking = false;
    this._seekTarget = 0;
    this._waiting = false;
    this._floor = 0;
    this._starveSince = 0;
    this._stalls = 0;               // attese per mancanza di dati (la soglia di ripartenza cresce)
    this._lastTU = 0;
    this._destroyed = false;
    this.readyState = 0;
    this.error = null;
    this.videoWidth = 0;
    this.videoHeight = 0;
    this.hasAudio = false;          // il file ha una traccia audio riproducibile
    this.audioNotice = '';          // codec audio presente ma non supportato
    this.info = { container: '', vcodec: '', acodec: '', fpsNominal: 0 };
    this.stats = { decoded: 0, shown: 0, dropped: 0, corrupt: 0 };
    this._shownTimes = [];

    this.ctx = audioContext();
    this.audio = this.ctx ? new AudioOut(this.ctx) : null;
    if (this.audio) this.audio.setVolume(this._muted ? 0 : this._volume);
    this._onCtxState = () => this._syncAudioClock();
    if (this.ctx) this.ctx.addEventListener('statechange', this._onCtxState);
    this._onSrcProgress = () => { this._progressDirty = true; };
    this.src.addEventListener('progress', this._onSrcProgress);

    this._loop = this._loop.bind(this);
    this._timer = 0; this._raf = 0;
    this._ready = this._openInternal();
  }

  // ---- API compatibile con HTMLVideoElement
  get paused() { return this._paused; }
  get ended() { return this._ended; }
  get seeking() { return this._seeking; }
  get duration() { return this._duration; }
  get currentTime() {
    if (this._seeking) return this._seekTarget;
    return this._now();
  }
  set currentTime(t) { this._requestSeek(Number(t) || 0); }
  get volume() { return this._volume; }
  set volume(v) { this._volume = Math.max(0, Math.min(1, Number(v) || 0)); this._applyVolume(); this._emit('volumechange'); }
  get muted() { return this._muted; }
  set muted(m) { this._muted = !!m; this._applyVolume(); this._emit('volumechange'); }
  get playbackRate() { return this._rate; }
  set playbackRate(r) {
    r = Math.max(0.25, Math.min(4, Number(r) || 1));
    if (r === this._rate) return;
    this._rate = r;
    this.clock.setRate(r);
    if (this.audio) this.audio.flush();
    this._emit('ratechange');
  }
  get defaultPlaybackRate() { return 1; }
  set defaultPlaybackRate(_) {}
  get buffered() { return timeRanges(this._safe(() => this._bufferedRanges(), [])); }
  // Il file ha audio ma l'autoplay lo tiene bloccato finché l'utente non interagisce.
  get audioBlocked() { return !!(this.hasAudio && this.ctx && this.ctx.state !== 'running'); }

  play() {
    if (this.error) return Promise.reject(this.error);
    if (this._destroyed) return Promise.resolve();
    if (this.ctx && this.ctx.state === 'suspended') this.ctx.resume().catch(() => {});
    if (this._ended) { this._ended = false; this._requestSeek(0); }
    if (!this._paused) return Promise.resolve();
    this._paused = false;
    this._emit('play');
    this._startClockIfReady();
    this._schedule(true);
    return Promise.resolve();
  }
  pause() {
    if (this._paused) return;
    this._paused = true;
    this.clock.stop();
    if (this.audio) this.audio.flush();
    this._emit('timeupdate');
    this._emit('pause');
  }
  load() {}
  getVideoPlaybackQuality() { return { totalVideoFrames: this.stats.decoded, droppedVideoFrames: this.stats.dropped, corruptedVideoFrames: this.stats.corrupt }; }

  destroy() {
    if (this._destroyed) return;
    this._destroyed = true;
    cancelAnimationFrame(this._raf); clearTimeout(this._timer);
    try { this.src.removeEventListener('progress', this._onSrcProgress); this.src.close(); } catch {}
    if (this.ctx) this.ctx.removeEventListener('statechange', this._onCtxState);
    if (this.audio) this.audio.destroy();
    try { this._destroy(); } catch {}
    this.canvas.remove();
  }

  // Statistiche per l'overlay "Statistiche".
  getStats() {
    const now = performance.now();
    const st = this._shownTimes.filter((x) => now - x < 2000);
    let fps = 0;
    if (st.length >= 2) fps = (st.length - 1) / ((st[st.length - 1] - st[0]) / 1000);
    const s = this.src.stats || {};
    return {
      engine: this.info.engine || 'soft',
      container: this.info.container,
      vcodec: this.info.vcodec,
      acodec: this.info.acodec || '',
      width: this.videoWidth, height: this.videoHeight,
      fpsNominal: this.info.fpsNominal,
      fpsReal: this._paused ? 0 : fps,
      decoded: this.stats.decoded, shown: this.stats.shown, dropped: this.stats.dropped, corrupt: this.stats.corrupt,
      clock: this.clock.audio ? 'audio' : 'wall',
      audioState: this.hasAudio ? (this.ctx ? this.ctx.state : 'none') : 'none',
      transport: s.mode || '', bytes: s.bytes || 0, requests: s.requests || 0, retries: s.retries || 0,
      size: this.src.size, cached: this.src.cachedBytes || 0,
      ahead: this._safe(() => this._bufferedAhead(), 0),
      extra: this._safe(() => this._extraStats(), null),
    };
  }

  // chiamate diagnostiche: mai eccezioni (motore ancora in apertura o già distrutto)
  _safe(fn, dflt) {
    if (this.readyState < 1 || this._destroyed) return dflt;
    try { const r = fn(); return r == null ? dflt : r; } catch { return dflt; }
  }

  // ---- da implementare nelle sottoclassi
  async _open() {}
  async _seek(t) {}
  _tick(now) {}
  _starved(now) { return false; }
  _canResume(now) { return true; }
  _atEnd(now) { return now >= this._duration; }
  _bufferedRanges() { return []; }
  _bufferedAhead() { return 0; }
  _extraStats() { return null; }
  _freezeTime() { return NaN; }
  _destroy() {}

  // Secondi di dati pronti richiesti per ripartire dopo un'attesa: 1 s, poi 2, 4, 8 se la rete
  // non regge il bitrate (meno interruzioni, più lunghe, invece di uno scatto ogni pochi secondi).
  _resumeSecs() { return Math.min(8, Math.pow(2, Math.max(0, this._stalls - 1))); }

  // ---- utilità per le sottoclassi
  _emit(type, detail) { this.dispatchEvent(detail ? new CustomEvent(type, { detail }) : new Event(type)); }
  _setDuration(d, exact) {
    if (!(d > 0) || !isFinite(d)) return;
    const changed = !(Math.abs(d - this._duration) < 1e-3);
    this._duration = d;
    if (exact) this._durationExact = true;
    if (changed && this.readyState >= 1) this._emit('durationchange');
  }
  _setSize(w, h, displayW) {
    this.videoWidth = w; this.videoHeight = h;
    const cw = Math.max(1, Math.round(displayW || w)), ch = Math.max(1, h);
    if (this.canvas.width !== cw || this.canvas.height !== ch) { this.canvas.width = cw; this.canvas.height = ch; }
  }
  _markShown() {
    this.stats.shown++;
    const t = performance.now();
    this._shownTimes.push(t);
    if (this._shownTimes.length > 120) this._shownTimes.splice(0, this._shownTimes.length - 120);
  }
  _fail(e) {
    if (this._destroyed || this.error) return;
    console.warn('[video-player]', e);
    const key = e && e.key ? e.key : (e instanceof WebAssembly.RuntimeError ? 'errCorrupt' : 'errDecode');
    this.error = { code: 4, key, detail: (e && e.detail) || (e && e.message) || '', vars: (e && e.vars) || null, message: String(e && e.message || e) };
    this._paused = true;
    this.clock.stop();
    if (this.audio) this.audio.clear();
    this._emit('error');
  }
  _latency() {
    if (!this.clock.audio || !this.ctx) return 0;
    return (this.ctx.outputLatency || 0) + (this.ctx.baseLatency || 0);
  }
  // Tempo "visibile": con l'orologio audio si sottrae la latenza d'uscita (Bluetooth incluso),
  // senza mai tornare indietro rispetto al punto di ripartenza.
  _now() {
    let t = this.clock.now();
    if (this.clock.audio && this.clock.running) t = Math.max(this._floor, t - this._latency());
    if (this._durationExact && t > this._duration) t = this._duration;
    return Math.max(0, t);
  }

  // ---- interni
  async _openInternal() {
    try {
      await this.src.open();
      if (this._destroyed) return;
      await this._open();
      if (this._destroyed) return;
      this._syncAudioClock();
      this.readyState = 1;
      this._emit('loadedmetadata');
      this._emit('durationchange');
      this.readyState = 4;
      this._emit('loadeddata');
      this._emit('canplay');
      if (this.audioNotice) this._emit('audionotice', { codec: this.audioNotice });
      if (!this._paused) { this._startClockIfReady(); if (this.audioBlocked) this._emit('audioblocked'); }
      this._schedule(true);
    } catch (e) {
      if (e instanceof SourceError && e.key === 'aborted') return;
      this._fail(e);
    }
  }
  _startClockIfReady() {
    if (this._paused || this._seeking || this._waiting || this.readyState < 1 || this._destroyed) return;
    if (this.hasAudio && this.ctx && this.ctx.state !== 'running') this._emit('audioblocked');
    this._floor = this.clock.now();
    this.clock.start();
    this._emit('playing');
  }
  // Passa all'orologio audio quando l'AudioContext diventa attivo (o torna al tempo di sistema).
  _syncAudioClock() {
    if (this._destroyed) return;
    const want = !!(this.hasAudio && this.ctx && this.ctx.state === 'running');
    if (want !== this.clock.audio) {
      this.clock.useAudio(this.ctx, want);
      this._floor = this.clock.now();
      if (this.audio) this.audio.flush();
      if (want) this._emit('audiounlocked');
    }
  }
  _applyVolume() { if (this.audio) this.audio.setVolume(this._muted ? 0 : this._volume); }

  _requestSeek(t) {
    if (this.error || this._destroyed) return;
    const d = this._duration;
    if (isFinite(d) && d > 0) t = Math.min(t, this._durationExact ? d : d * 1.5);
    t = Math.max(0, t);
    this._seekTarget = t;
    this._ended = false;
    this.clock.stop();
    this.clock.set(t);
    if (this.audio) this.audio.clear();
    if (this._seeking) return;       // il seek in corso raccoglie il nuovo obiettivo
    this._seeking = true;
    this._emit('seeking');
    this._emit('timeupdate');
    this._runSeek();
  }
  async _runSeek() {
    try {
      if (this.readyState < 1) await this._ready;
      let t;
      do {
        t = this._seekTarget;
        await this._seek(t);
      } while (t !== this._seekTarget && !this._destroyed && !this.error);
      if (this._destroyed || this.error) return;
      this._seeking = false;
      this._waiting = false;
      this._starveSince = 0;
      this._stalls = 0;
      this.clock.set(t);
      if (this.audio) this.audio.clear();
      this._emit('seeked');
      this._emit('timeupdate');
      this._startClockIfReady();
      this._schedule(true);
    } catch (e) {
      if (e instanceof SourceError && e.key === 'aborted') return;
      this._seeking = false;
      this._fail(e);
    }
  }

  // rAF per disegnare in sincrono con lo schermo + timer di riserva: in un iframe nascosto (finestra
  // ridotta a icona nella Web OS) rAF si ferma ma document.hidden resta false, e l'audio deve
  // continuare. Il primo dei due che scatta esegue il passo e annulla l'altro.
  _schedule(soon) {
    if (this._destroyed) return;
    if (soon) { cancelAnimationFrame(this._raf); clearTimeout(this._timer); this._raf = this._timer = 0; }
    if (this._raf || this._timer) return;
    const idle = this._paused && !this._seeking;
    if (idle) { this._timer = setTimeout(this._loop, 200); return; }
    if (!document.hidden) this._raf = requestAnimationFrame(this._loop);
    this._timer = setTimeout(this._loop, document.hidden ? 25 : 50);
  }
  _loop() {
    cancelAnimationFrame(this._raf); clearTimeout(this._timer);
    this._raf = 0; this._timer = 0;
    if (this._destroyed || this.error) return;
    try { this._step(); } catch (e) { this._fail(e); return; }
    this._schedule(false);
  }
  _step() {
    if (this.readyState < 1) return;
    const now = this._now();
    if (!this._seeking) this._tick(now);
    const playing = !this._paused && !this._seeking && !this._waiting;
    if (this.audio) this.audio.pump(this.clock, this.clock.now() + AUDIO_LEAD, playing && this.clock.audio && this.ctx.state === 'running');

    if (playing) {
      if (this._starved(now)) {
        if (!this._starveSince) this._starveSince = performance.now();
        else if (performance.now() - this._starveSince > STALL_MS) {
          this._waiting = true;
          this._stalls++;
          this.clock.stop();
          // si riparte dall'ultimo fotogramma mostrato: niente fotogrammi saltati dopo l'attesa
          const ft = this._safe(() => this._freezeTime(), NaN);
          if (ft >= 0 && ft < this.clock.base) this.clock.set(ft);
          if (this.audio) this.audio.flush();
          this._emit('waiting');
        }
      } else this._starveSince = 0;
      if (!this._waiting && this._atEnd(now)) { this._finish(); return; }
    }
    else if (this._waiting && !this._paused && !this._seeking && this._canResume(now)) {
      this._waiting = false;
      this._starveSince = 0;
      this._startClockIfReady();
    }
    const t = performance.now();
    if ((playing && t - this._lastTU >= TIMEUPDATE_MS)) { this._lastTU = t; this._emit('timeupdate'); }
    if (this._progressDirty && t - (this._lastProgress || 0) > 500) { this._progressDirty = false; this._lastProgress = t; this._emit('progress'); }
  }
  _finish() {
    this.clock.stop();
    if (isFinite(this._duration)) this.clock.set(this._duration);
    this._paused = true;
    this._ended = true;
    if (this.audio) this.audio.flush();
    this._emit('timeupdate');
    this._emit('pause');
    this._emit('ended');
  }
}

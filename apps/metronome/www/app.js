// app.js — Metronome + Tuner engine for NucleoOS.
//
// Everything here runs in the BROWSER (operator console): Web Audio clicks and pitch detection are
// pure client-side DSP, so the Cardputer spends zero heap/CPU on it (device-load discipline). The
// tuner prefers the browser microphone (getUserMedia — best quality, zero device traffic) and falls
// back to the Cardputer's PDM mic via the OS-wide mic gate when the page has no secure context
// (http://LAN-IP serves no getUserMedia at all).

// ── Metronome ────────────────────────────────────────────────────────────────────────────────────
// Lookahead scheduler (short setInterval + Web Audio timeline) — setInterval alone drifts and
// stutters under load; scheduling clicks ~120ms ahead on the audio clock keeps them sample-accurate.
export class Metronome {
  constructor(onBeat) {
    this.bpm = 120; this.beats = 4; this.subdiv = 1;
    this.onBeat = onBeat;               // (beatIndex, subIndex, when) → UI flash
    this.ctx = null; this.timer = null;
    this._next = 0; this._beat = 0; this._sub = 0;
  }
  get running() { return !!this.timer; }
  start() {
    if (this.timer) return;
    if (!this.ctx) this.ctx = new (window.AudioContext || window.webkitAudioContext)();
    this.ctx.resume();
    this._next = this.ctx.currentTime + 0.06; this._beat = 0; this._sub = 0;
    this.timer = setInterval(() => this._schedule(), 25);
  }
  stop() { clearInterval(this.timer); this.timer = null; }
  _schedule() {
    while (this._next < this.ctx.currentTime + 0.12) {
      this._click(this._next, this._beat, this._sub);
      const beatDur = 60 / this.bpm;
      this._next += beatDur / this.subdiv;
      if (++this._sub >= this.subdiv) { this._sub = 0; this._beat = (this._beat + 1) % this.beats; }
    }
  }
  _click(when, beat, sub) {
    const accent = beat === 0 && sub === 0, main = sub === 0;
    const osc = this.ctx.createOscillator(), g = this.ctx.createGain();
    osc.frequency.value = accent ? 1568 : main ? 1047 : 784;
    g.gain.setValueAtTime(accent ? 0.5 : main ? 0.35 : 0.18, when);
    g.gain.exponentialRampToValueAtTime(0.001, when + 0.05);
    osc.connect(g).connect(this.ctx.destination);
    osc.start(when); osc.stop(when + 0.06);
    const ms = Math.max(0, (when - this.ctx.currentTime) * 1000);
    setTimeout(() => this.onBeat && this.onBeat(beat, sub), ms);
  }
}

// Tap tempo: average the last few intervals; a pause >2s starts a fresh phrase.
export function makeTapTempo() {
  let taps = [];
  return () => {
    const now = performance.now();
    if (taps.length && now - taps[taps.length - 1] > 2000) taps = [];
    taps.push(now); if (taps.length > 5) taps.shift();
    if (taps.length < 2) return null;
    const avg = (taps[taps.length - 1] - taps[0]) / (taps.length - 1);
    return Math.round(60000 / avg);
  };
}

// ── Pitch detection (YIN) ────────────────────────────────────────────────────────────────────────
// YIN, the standard monophonic pitch estimator. A plain raw-autocorrelation peak is biased toward
// small lags (fewer product terms at large lag → the sum shrinks with lag), so it reports too-high a
// frequency; YIN's cumulative-mean-normalised difference removes that bias and its octave errors.
// Range 60–1200 Hz covers a bass low E (~41 Hz is out — instruments here start at bass B) up past a
// violin's top; the threshold 0.12 is YIN's usual clarity gate.
export function detectPitch(buf, sampleRate) {
  const N = buf.length;
  let rms = 0;
  for (let i = 0; i < N; i++) rms += buf[i] * buf[i];
  if (Math.sqrt(rms / N) < 0.006) return null;        // silence — don't chase noise
  const minLag = Math.max(2, Math.floor(sampleRate / 1200));
  const maxLag = Math.min(Math.floor(sampleRate / 60), N >> 1);
  const d = new Float32Array(maxLag + 1);             // difference function
  for (let lag = minLag; lag <= maxLag; lag++) {
    let sum = 0;
    for (let i = 0, e = N - maxLag; i < e; i++) { const diff = buf[i] - buf[i + lag]; sum += diff * diff; }
    d[lag] = sum;
  }
  // Cumulative mean normalised difference d'[lag] = d[lag] / (mean of d[1..lag]).
  const cmnd = new Float32Array(maxLag + 1);
  let running = 0;
  for (let lag = minLag; lag <= maxLag; lag++) { running += d[lag]; cmnd[lag] = d[lag] * (lag - minLag + 1) / (running || 1); }
  const THRESH = 0.12;
  let tau = -1;
  for (let lag = minLag; lag <= maxLag; lag++) {
    if (cmnd[lag] < THRESH) {                          // first dip below threshold → walk to its local min
      while (lag + 1 <= maxLag && cmnd[lag + 1] < cmnd[lag]) lag++;
      tau = lag; break;
    }
  }
  if (tau < 0) {                                        // nothing crossed the gate → take the global min, but only if it's clearly periodic
    let best = minLag;
    for (let lag = minLag; lag <= maxLag; lag++) if (cmnd[lag] < cmnd[best]) best = lag;
    if (cmnd[best] > 0.4) return null;                  // no clear pitch
    tau = best;
  }
  let t = tau;                                          // parabolic interpolation on the difference function
  if (tau > minLag && tau < maxLag) {
    const a = d[tau - 1], b = d[tau], c = d[tau + 1], den = a - 2 * b + c;
    if (den !== 0) t = tau + 0.5 * (a - c) / den;
  }
  return { freq: sampleRate / t, clarity: 1 - cmnd[tau] };
}

const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const SOLFEGE = ['Do', 'Do#', 'Re', 'Re#', 'Mi', 'Fa', 'Fa#', 'Sol', 'Sol#', 'La', 'La#', 'Si'];
export function noteFromFreq(freq, a4 = 440) {
  const n = 12 * Math.log2(freq / a4) + 69;
  const nearest = Math.round(n);
  const idx = ((nearest % 12) + 12) % 12;
  return { name: NAMES[idx], solfege: SOLFEGE[idx], octave: Math.floor(nearest / 12) - 1,
           cents: Math.round((n - nearest) * 100) };
}

// ── Tuner audio sources ──────────────────────────────────────────────────────────────────────────
// Both sources feed the same consumer: onbuf(Float32Array, sampleRate) every ~100ms. Returns a stop().
export async function startBrowserMic(onbuf) {
  const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } });
  const ctx = new (window.AudioContext || window.webkitAudioContext)();
  const src = ctx.createMediaStreamSource(stream);
  const an = ctx.createAnalyser(); an.fftSize = 2048;
  src.connect(an);
  const buf = new Float32Array(an.fftSize);
  const timer = setInterval(() => { an.getFloatTimeDomainData(buf); onbuf(buf, ctx.sampleRate); }, 100);
  return () => { clearInterval(timer); stream.getTracks().forEach((t) => t.stop()); ctx.close(); };
}

// Cardputer PDM mic through the OS-wide gate (micgate owns exclusivity + reconnects). 16kHz Int16
// frames are accumulated into a 2048-sample ring; detection runs on each delivery past the fill.
export async function startDeviceMic(onbuf, onerror, label) {
  const { streamMic } = await import('/micgate.js');
  const abort = new AbortController();
  const ring = new Float32Array(2048); let fill = 0, rate = 16000;
  streamMic({
    label, signal: abort.signal,
    onframe(samples, info) {
      rate = (info && info.sampleRate) || 16000;
      for (let i = 0; i < samples.length; i++) {
        if (fill < ring.length) ring[fill++] = samples[i] / 32768;
        else { ring.copyWithin(0, 1); ring[ring.length - 1] = samples[i] / 32768; }
      }
      if (fill >= ring.length) onbuf(ring, rate);
    },
    onerror(kind, who) { onerror && onerror(kind, who); },
  });
  return () => abort.abort();
}

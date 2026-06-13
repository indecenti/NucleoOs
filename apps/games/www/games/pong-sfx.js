// pong-sfx.js — procedural sound for Pong. 100% synthesized via the Web Audio API: no asset files on
// SD, nothing to download. Each event (paddle hit, wall, score, power-up, shield, win) is a tiny burst
// of oscillators/noise through a shared master bus with a soft limiter. There's also an optional ambient
// synth pad. Autoplay policy: the context is created lazily and resumed on the first user gesture.

export class SFX {
  constructor() {
    this.ac = null; this.master = null; this.musicGain = null; this.music = null; this.muted = false;
    const unlock = () => this._ensure() && this.ac.resume && this.ac.resume();
    ['pointerdown', 'keydown', 'touchstart'].forEach(e => window.addEventListener(e, unlock, { once: true, passive: true }));
  }

  _ensure() {
    if (this.ac) return true;
    const AC = window.AudioContext || window.webkitAudioContext; if (!AC) return false;
    this.ac = new AC();
    this.master = this.ac.createGain(); this.master.gain.value = 0.9;
    const lim = this.ac.createDynamicsCompressor();
    lim.threshold.value = -8; lim.knee.value = 6; lim.ratio.value = 12; lim.attack.value = 0.003; lim.release.value = 0.2;
    this.master.connect(lim); lim.connect(this.ac.destination);
    return true;
  }
  resume() { if (this._ensure() && this.ac.state === 'suspended') this.ac.resume(); }
  setMuted(m) { this.muted = m; if (this.master) this.master.gain.value = m ? 0 : 0.9; }

  // low-level voice: an oscillator with an ADSR-ish gain envelope.
  _tone(freq, t0, dur, { type = 'sine', gain = 0.3, glide = 0, dest = null } = {}) {
    const ac = this.ac, o = ac.createOscillator(), g = ac.createGain();
    o.type = type; o.frequency.setValueAtTime(freq, t0);
    if (glide) o.frequency.exponentialRampToValueAtTime(Math.max(20, freq * glide), t0 + dur);
    g.gain.setValueAtTime(0.0001, t0); g.gain.exponentialRampToValueAtTime(gain, t0 + 0.006);
    g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    o.connect(g); g.connect(dest || this.master); o.start(t0); o.stop(t0 + dur + 0.02);
    return o;
  }
  _noise(t0, dur, { gain = 0.25, freq = 1200, q = 0.7, type = 'bandpass' } = {}) {
    const ac = this.ac, n = Math.floor(ac.sampleRate * dur), buf = ac.createBuffer(1, n, ac.sampleRate), d = buf.getChannelData(0);
    for (let i = 0; i < n; i++) d[i] = Math.random() * 2 - 1;
    const src = ac.createBufferSource(); src.buffer = buf;
    const f = ac.createBiquadFilter(); f.type = type; f.frequency.value = freq; f.Q.value = q;
    const g = ac.createGain(); g.gain.setValueAtTime(gain, t0); g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    src.connect(f); f.connect(g); g.connect(this.master); src.start(t0); src.stop(t0 + dur);
  }

  // ── game events ──────────────────────────────────────────────────────────────────────────
  hit(speed = 130, combo = 1) {
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    const f = 220 + Math.min(1, speed / 270) * 520 + combo * 14;
    this._tone(f, t, 0.10, { type: 'triangle', gain: 0.32, glide: 1.5 });
    this._tone(f * 1.5, t, 0.06, { type: 'square', gain: 0.10 });
    this._noise(t, 0.05, { gain: 0.10, freq: f * 2, q: 1.2 });
  }
  wall() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime; this._tone(150, t, 0.08, { type: 'sine', gain: 0.22, glide: 0.6 }); this._noise(t, 0.04, { gain: 0.06, freq: 400 }); }
  shield() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime; [660, 880, 1320].forEach((f, i) => this._tone(f, t + i * 0.02, 0.18, { type: 'sine', gain: 0.16 })); this._noise(t, 0.2, { gain: 0.05, freq: 2400, type: 'highpass' }); }
  orb(kind = 'grow') {
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    const seq = kind === 'shrink' ? [880, 740, 620] : kind === 'fast' ? [520, 700, 1040, 1400] : kind === 'shield' ? [523, 784, 1046] : [523, 659, 880, 1175];
    seq.forEach((f, i) => this._tone(f, t + i * 0.05, 0.16, { type: 'triangle', gain: 0.18 }));
  }
  score(win = true) {
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    const seq = win ? [523, 659, 784, 1046] : [392, 330, 262];
    seq.forEach((f, i) => this._tone(f, t + i * 0.07, 0.22, { type: 'sawtooth', gain: 0.16 }));
  }
  // disc landing in Connect Four — a satisfying descending thunk (used by forza4-3d.js).
  drop() {
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    this._tone(330, t, 0.14, { type: 'triangle', gain: 0.30, glide: 0.5 });
    this._tone(140, t + 0.02, 0.18, { type: 'sine', gain: 0.22, glide: 0.7 });
    this._noise(t + 0.01, 0.05, { gain: 0.10, freq: 600, q: 0.8 });
  }
  count() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime; this._tone(440, t, 0.12, { type: 'square', gain: 0.2 }); }
  go() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime; this._tone(880, t, 0.3, { type: 'sawtooth', gain: 0.24, glide: 1.3 }); }
  win() {
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    [523, 659, 784, 1046, 1318].forEach((f, i) => { this._tone(f, t + i * 0.09, 0.5, { type: 'sawtooth', gain: 0.16 }); this._tone(f * 0.5, t + i * 0.09, 0.5, { type: 'triangle', gain: 0.08 }); });
  }
  lose() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime; [440, 392, 330, 262].forEach((f, i) => this._tone(f, t + i * 0.12, 0.4, { type: 'sawtooth', gain: 0.14, glide: 0.92 })); }

  // ── ambient pad (optional, default off) ────────────────────────────────────────────────────
  toggleMusic() { if (this.music) { this.stopMusic(); return false; } this.startMusic(); return true; }
  startMusic() {
    if (!this._ensure() || this.music) return; const ac = this.ac, t = ac.currentTime;
    const g = ac.createGain(); g.gain.value = 0.0; g.gain.linearRampToValueAtTime(0.10, t + 2); g.connect(this.master);
    const lp = ac.createBiquadFilter(); lp.type = 'lowpass'; lp.frequency.value = 700; lp.Q.value = 0.6; lp.connect(g);
    const lfo = ac.createOscillator(), lfoG = ac.createGain(); lfo.frequency.value = 0.07; lfoG.gain.value = 380; lfo.connect(lfoG); lfoG.connect(lp.frequency); lfo.start();
    const voices = [];
    [110, 164.81, 220, 277.18].forEach((f, i) => { const o = ac.createOscillator(); o.type = i % 2 ? 'sawtooth' : 'triangle'; o.frequency.value = f; o.detune.value = (i - 1.5) * 6; o.connect(lp); o.start(); voices.push(o); });
    this.music = { g, lp, lfo, voices };
  }
  stopMusic() {
    if (!this.music) return; const { g, lfo, voices } = this.music, t = this.ac.currentTime;
    g.gain.cancelScheduledValues(t); g.gain.setValueAtTime(g.gain.value, t); g.gain.linearRampToValueAtTime(0.0001, t + 0.8);
    setTimeout(() => { try { lfo.stop(); voices.forEach(o => o.stop()); } catch {} }, 900);
    this.music = null;
  }
}

// constellations-sfx.js — procedural space-combat sound for Costellazioni 3D. 100% Web Audio synthesis
// (no asset files on the tiny SD): every event is a short burst of oscillators/noise through a shared
// limiter bus, with a cheap feedback-delay "reverb" and a dynamic minor-key music bed. Lazy context,
// resumed on the first user gesture.

export class SFX {
  constructor() {
    this.ac = null; this.master = null; this.muted = false; this.music = null; this._fx = null; this._lastLaser = 0;
    const unlock = () => { if (this._ensure() && this.ac.resume) this.ac.resume(); };
    ['pointerdown', 'keydown', 'touchstart'].forEach(e => window.addEventListener(e, unlock, { passive: true }));
  }
  _ensure() {
    if (this.ac) return true;
    const AC = window.AudioContext || window.webkitAudioContext; if (!AC) return false;
    this.ac = new AC();
    this.master = this.ac.createGain(); this.master.gain.value = 0.85;
    const lim = this.ac.createDynamicsCompressor();
    lim.threshold.value = -10; lim.knee.value = 8; lim.ratio.value = 14; lim.attack.value = 0.003; lim.release.value = 0.18;
    this.master.connect(lim); lim.connect(this.ac.destination);
    return true;
  }
  setMuted(m) { this.muted = m; if (this.master) this.master.gain.value = m ? 0 : 0.85; }

  _tone(freq, t0, dur, { type = 'sine', gain = 0.3, glide = 0, dest = null } = {}) {
    const ac = this.ac, o = ac.createOscillator(), g = ac.createGain();
    o.type = type; o.frequency.setValueAtTime(freq, t0);
    if (glide) o.frequency.exponentialRampToValueAtTime(Math.max(20, freq * glide), t0 + dur);
    g.gain.setValueAtTime(0.0001, t0); g.gain.exponentialRampToValueAtTime(gain, t0 + 0.005);
    g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    o.connect(g); g.connect(dest || this.master); o.start(t0); o.stop(t0 + dur + 0.02);
    return o;
  }
  _noise(t0, dur, { gain = 0.25, freq = 1200, q = 0.7, type = 'bandpass' } = {}) {
    const ac = this.ac, n = Math.max(1, Math.floor(ac.sampleRate * dur)), buf = ac.createBuffer(1, n, ac.sampleRate), d = buf.getChannelData(0);
    for (let i = 0; i < n; i++) d[i] = Math.random() * 2 - 1;
    const src = ac.createBufferSource(); src.buffer = buf;
    const f = ac.createBiquadFilter(); f.type = type; f.frequency.value = freq; f.Q.value = q;
    const g = ac.createGain(); g.gain.setValueAtTime(gain, t0); g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    src.connect(f); f.connect(g); g.connect(this.master); src.start(t0); src.stop(t0 + dur);
  }
  // Shared FX bus: one lazy feedback DelayNode = cheap "tail/reverb" (no convolver). Pass as a _tone dest.
  _delayBus() {
    if (this._fx) return this._fx.input;
    const ac = this.ac;
    const input = ac.createGain(); input.gain.value = 1;
    const wet = ac.createGain(); wet.gain.value = 0.30;
    const dly = ac.createDelay(0.5); dly.delayTime.value = 0.16;
    const fb = ac.createGain(); fb.gain.value = 0.32;
    const tone = ac.createBiquadFilter(); tone.type = 'lowpass'; tone.frequency.value = 2600;
    input.connect(dly); dly.connect(tone); tone.connect(wet); wet.connect(this.master);
    tone.connect(fb); fb.connect(dly);
    input.connect(this.master);
    this._fx = { input, wet, dly, fb, tone };
    return input;
  }

  // ── combat events ───────────────────────────────────────────────────────────────────────────
  laser() {  // twin-cannon zap — two detuned pews + muzzle snap, punchy + short
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    if (t - this._lastLaser < 0.04) return; this._lastLaser = t;
    this._tone(2100, t, 0.11, { type: 'sawtooth', gain: 0.20, glide: 0.10 });
    this._tone(3100, t, 0.04, { type: 'square', gain: 0.08, glide: 0.45 });
    this._tone(1850, t + 0.012, 0.12, { type: 'sawtooth', gain: 0.16, glide: 0.11 });   // detuned 2nd barrel = fat
    this._tone(620, t, 0.06, { type: 'square', gain: 0.07, glide: 0.55 });              // low body
    this._noise(t, 0.025, { gain: 0.07, freq: 3600, q: 0.8, type: 'highpass' });        // muzzle click
  }
  hit() {    // bolt connects — crisp metallic tick + tiny clang + body
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    this._noise(t, 0.04, { gain: 0.18, freq: 3000, q: 1.4, type: 'bandpass' });
    this._tone(540, t, 0.05, { type: 'square', gain: 0.13, glide: 0.45 });
    this._tone(810, t + 0.004, 0.04, { type: 'square', gain: 0.07, glide: 0.5 });       // inharmonic = metallic
    this._tone(190, t, 0.05, { type: 'sine', gain: 0.11, glide: 0.6 });
  }
  boom(big) {  // explosion — crack + sub-bass + crackle tail; big adds inharmonic hull-ring & longer decay
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    this._noise(t, 0.045, { gain: big ? 0.34 : 0.24, freq: 4200, q: 0.6, type: 'highpass' });
    this._noise(t + 0.002, big ? 0.6 : 0.4, { gain: big ? 0.30 : 0.22, freq: big ? 380 : 600, q: 0.5, type: 'lowpass' });
    this._tone(big ? 54 : 84, t, big ? 0.55 : 0.36, { type: 'sine', gain: 0.36, glide: 0.42 });
    this._tone(big ? 108 : 168, t, 0.22, { type: 'triangle', gain: 0.16, glide: 0.4 });
    this._noise(t + 0.06, 0.18, { gain: big ? 0.12 : 0.08, freq: 1800, q: 0.7, type: 'bandpass' });
    this._noise(t + 0.14, 0.16, { gain: big ? 0.09 : 0.05, freq: 2600, q: 0.8, type: 'bandpass' });
    if (big) {
      this._tone(430, t + 0.03, 0.5, { type: 'sawtooth', gain: 0.07, glide: 0.32 });
      this._tone(631, t + 0.05, 0.44, { type: 'sine', gain: 0.05, glide: 0.34 });
      this._tone(947, t + 0.07, 0.38, { type: 'sine', gain: 0.04, glide: 0.36 });
    }
  }
  lock() {   // target lock — three crystalline rising pips, last an octave = "confirmed"
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    this._tone(1175, t, 0.045, { type: 'square', gain: 0.09 });
    this._tone(1568, t + 0.05, 0.045, { type: 'square', gain: 0.10 });
    this._tone(2349, t + 0.10, 0.08, { type: 'square', gain: 0.11 });
    this._tone(2349, t + 0.10, 0.08, { type: 'sine', gain: 0.05 });
  }
  hurt() {   // hull/shield damage — gut-punch drop + alarm edge + shield-rupture rumble
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    this._tone(150, t, 0.2, { type: 'sawtooth', gain: 0.24, glide: 0.65 });
    this._tone(75, t, 0.22, { type: 'sine', gain: 0.18, glide: 0.7 });
    this._tone(330, t + 0.04, 0.16, { type: 'square', gain: 0.10, glide: 0.6 });
    this._noise(t, 0.12, { gain: 0.14, freq: 280, q: 0.8, type: 'lowpass' });
  }
  strafe() { // enemy screams past — Doppler whoosh rises then falls, airy noise
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    this._tone(900, t, 0.14, { type: 'sawtooth', gain: 0.13, glide: 1.9 });
    this._tone(1700, t + 0.13, 0.22, { type: 'sawtooth', gain: 0.14, glide: 0.18 });
    this._noise(t, 0.34, { gain: 0.09, freq: 2200, q: 0.4, type: 'bandpass' });
  }
  missile() { // launch — ignition thump + sub kick + long climbing whoosh + exhaust hiss
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    this._tone(110, t, 0.09, { type: 'square', gain: 0.24, glide: 0.5 });
    this._tone(70, t, 0.12, { type: 'sine', gain: 0.16, glide: 0.6 });
    this._tone(240, t + 0.03, 0.46, { type: 'sawtooth', gain: 0.18, glide: 4.6 });
    this._tone(360, t + 0.05, 0.42, { type: 'triangle', gain: 0.08, glide: 4.0 });
    this._noise(t + 0.02, 0.46, { gain: 0.16, freq: 1100, q: 0.4, type: 'highpass' });
  }
  // power-up collected — distinct, satisfying cue per kind ('missile' | 'shield' | 'repair')
  powerup(kind) {
    if (!this._ensure() || this.muted) return;
    const t = this.ac.currentTime, echo = this._delayBus();
    if (kind === 'missile') {
      const arp = [392, 523, 659, 784, 1046];
      arp.forEach((f, i) => { const tt = t + i * 0.055; this._tone(f, tt, 0.12, { type: 'square', gain: 0.10, dest: echo }); this._tone(f * 2, tt, 0.06, { type: 'triangle', gain: 0.05 }); });
      this._tone(140, t, 0.10, { type: 'square', gain: 0.16, glide: 0.6 });
      this._tone(1568, t + arp.length * 0.055, 0.20, { type: 'square', gain: 0.10, dest: echo });
      this._noise(t, 0.03, { gain: 0.05, freq: 4200, q: 0.8, type: 'highpass' });
    } else if (kind === 'shield') {
      const cry = [659, 880, 1108, 1318, 1760];
      cry.forEach((f, i) => { const tt = t + i * 0.04; this._tone(f, tt, 0.30, { type: 'sine', gain: 0.09, dest: echo }); this._tone(f * 1.5, tt, 0.16, { type: 'triangle', gain: 0.04, dest: echo }); });
      this._tone(330, t, 0.45, { type: 'sine', gain: 0.10, glide: 2.2 });
      this._noise(t + 0.02, 0.35, { gain: 0.05, freq: 6000, q: 0.5, type: 'highpass' });
      this._tone(2637, t + 0.22, 0.22, { type: 'sine', gain: 0.07, dest: echo });
    } else {   // 'repair' — warm settle + heartbeat
      this._tone(196, t, 0.10, { type: 'sine', gain: 0.20, glide: 1.0 });
      this._tone(294, t + 0.05, 0.55, { type: 'triangle', gain: 0.14, glide: 1.0 });
      this._tone(392, t + 0.10, 0.50, { type: 'sine', gain: 0.10, dest: echo });
      this._tone(587, t + 0.20, 0.45, { type: 'sine', gain: 0.08, dest: echo });
      this._noise(t, 0.06, { gain: 0.05, freq: 500, q: 0.7, type: 'lowpass' });
      this._tone(120, t + 0.34, 0.12, { type: 'sine', gain: 0.12, glide: 0.7 });
    }
  }
  wave(n = 1) {  // a new wave warps in — rising klaxon, pitch climbs with the wave number
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    const base = 300 + n * 40;
    this._tone(base, t, 0.16, { type: 'sawtooth', gain: 0.16, glide: 1.6 });
    this._tone(base * 1.5, t + 0.1, 0.16, { type: 'sawtooth', gain: 0.12, glide: 1.5 });
  }
  victory() {
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime, echo = this._delayBus();
    [523, 659, 784, 1046, 1318].forEach((f, i) => { this._tone(f, t + i * 0.09, 0.5, { type: 'sawtooth', gain: 0.15, dest: echo }); this._tone(f / 2, t + i * 0.09, 0.5, { type: 'triangle', gain: 0.08 }); });
  }
  defeat() {
    if (!this._ensure() || this.muted) return; const t = this.ac.currentTime;
    [392, 330, 262, 196].forEach((f, i) => this._tone(f, t + i * 0.13, 0.45, { type: 'sawtooth', gain: 0.16, glide: 0.9 }));
  }
  // ── UI ──────────────────────────────────────────────────────────────────────────────────────
  blip() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime; this._tone(680, t, 0.045, { type: 'square', gain: 0.07 }); this._tone(1360, t, 0.025, { type: 'sine', gain: 0.03 }); }
  confirm() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime, echo = this._delayBus(); this._tone(587, t, 0.08, { type: 'square', gain: 0.11, dest: echo }); this._tone(880, t + 0.06, 0.12, { type: 'square', gain: 0.11, dest: echo }); this._tone(1760, t + 0.06, 0.10, { type: 'sine', gain: 0.05 }); }
  deny() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime; this._tone(220, t, 0.14, { type: 'square', gain: 0.13, glide: 0.6 }); this._tone(165, t + 0.02, 0.16, { type: 'sawtooth', gain: 0.09, glide: 0.7 }); }
  cash() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime, echo = this._delayBus(); [988, 1319, 1760].forEach((f, i) => { const tt = t + i * 0.05; this._tone(f, tt, 0.12, { type: 'triangle', gain: 0.12, dest: echo }); this._tone(f * 2, tt, 0.06, { type: 'sine', gain: 0.04 }); }); }
  jump() { if (!this._ensure() || this.muted) return; const t = this.ac.currentTime, echo = this._delayBus(); this._tone(160, t, 0.5, { type: 'sawtooth', gain: 0.2, glide: 7 }); this._tone(320, t, 0.5, { type: 'triangle', gain: 0.08, glide: 6 }); this._noise(t, 0.5, { gain: 0.1, freq: 1800, q: 0.5, type: 'highpass' }); this._tone(2200, t + 0.46, 0.10, { type: 'square', gain: 0.10, dest: echo }); }

  // ── dynamic music bed (combat) — A natural minor: pulsing bass + light arp + soft pad + tenue kick.
  // setIntensity(0..1) ramps tempo 96->160 BPM and opens the lowpass. startDrone/stopDrone are aliases.
  startMusic() {
    if (!this._ensure() || this.music || this.muted) return;
    const ac = this.ac, t = ac.currentTime;
    const bus = ac.createGain(); bus.gain.value = 0; bus.gain.linearRampToValueAtTime(0.14, t + 1.8); bus.connect(this.master);
    const delay = ac.createDelay(0.6); delay.delayTime.value = 0.30;
    const fb = ac.createGain(); fb.gain.value = 0.28;
    const wet = ac.createGain(); wet.gain.value = 0.22;
    delay.connect(fb); fb.connect(delay); delay.connect(wet); wet.connect(bus);
    const tone = ac.createBiquadFilter(); tone.type = 'lowpass'; tone.frequency.value = 900; tone.Q.value = 0.5;
    tone.connect(bus); tone.connect(delay);
    const lfo = ac.createOscillator(), lfoG = ac.createGain(); lfo.frequency.value = 0.08; lfoG.gain.value = 260; lfo.connect(lfoG); lfoG.connect(tone.frequency); lfo.start();
    const padG = ac.createGain(); padG.gain.value = 0; padG.gain.linearRampToValueAtTime(0.05, t + 2.2); padG.connect(tone);
    const pad = [110, 164.81].map((f, i) => { const o = ac.createOscillator(); o.type = i ? 'triangle' : 'sawtooth'; o.frequency.value = f; o.detune.value = (i ? 7 : -6); o.connect(padG); o.start(); return o; });
    this.music = { bus, tone, delay, fb, wet, lfo, padG, pad, step: 0, intensity: 0.0, timer: null, stopped: false,
      bassSeq: [55, 55, 82.41, 55, 65.41, 55, 73.42, 82.41], arpSeq: [220, 261.63, 329.63, 440, 392, 329.63, 261.63, 220] };
    this._musicStep();
  }
  stopMusic() {
    if (!this.music) return;
    const m = this.music, t = this.ac.currentTime;
    m.stopped = true; if (m.timer) { clearTimeout(m.timer); m.timer = null; }
    m.bus.gain.cancelScheduledValues(t); m.bus.gain.setValueAtTime(m.bus.gain.value, t); m.bus.gain.linearRampToValueAtTime(0.0001, t + 0.7);
    setTimeout(() => { try { m.lfo.stop(); m.pad.forEach(o => o.stop()); } catch {} }, 800);
    this.music = null;
  }
  setIntensity(v) { if (this.music) this.music.intensity = Math.max(0, Math.min(1, v)); }
  _musicStep() {
    const m = this.music; if (!m || m.stopped) return;
    const ac = this.ac, k = m.intensity, tempo = 96 + k * 64, dt = 30 / tempo, t = ac.currentTime + 0.04, i = m.step % 8, downbeat = (i % 2) === 0;
    m.tone.frequency.setTargetAtTime(720 + k * 1100, t, 0.2);
    const bf = m.bassSeq[i];
    this._tone(bf, t, dt * (downbeat ? 1.7 : 1.0), { type: 'square', gain: 0.16 + k * 0.05, dest: m.tone });
    this._tone(bf, t, dt * (downbeat ? 1.5 : 0.9), { type: 'triangle', gain: 0.10, dest: m.tone });
    if (downbeat) { this._tone(120, t, 0.14, { type: 'sine', gain: 0.14 + k * 0.05, glide: 0.42, dest: m.bus }); this._noise(t, 0.02, { gain: 0.05 + k * 0.04, freq: 2400, q: 0.7, type: 'highpass' }); }
    if (k > 0.12 || i % 2 === 0) { const af = m.arpSeq[i]; this._tone(af, t, dt * 0.9, { type: 'sawtooth', gain: 0.05 + k * 0.05, dest: m.tone }); this._tone(af * 2, t, dt * 0.5, { type: 'triangle', gain: 0.02 + k * 0.03, dest: m.tone }); }
    m.delay.delayTime.setTargetAtTime(0.34 - k * 0.10, t, 0.3);
    m.step++; m.timer = setTimeout(() => this._musicStep(), dt * 1000);
  }
  startDrone() { this.startMusic(); }
  stopDrone() { this.stopMusic(); }
}

// One shared instance for the whole game (renderer + UI import the same module singleton).
export const sfx = new SFX();

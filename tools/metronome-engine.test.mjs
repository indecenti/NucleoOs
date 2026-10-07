// Host tests for the Metronome + Tuner engine (apps/metronome/www/app.js): pure DSP / timing logic,
// no browser, no microphone. What a musician relies on:
//   - the tuner names the RIGHT note (no octave errors) across the whole advertised 60–1200 Hz range,
//     on the browser mic rates (44.1 / 48 kHz) AND the Cardputer PDM mic (16 kHz), for pure tones and
//     harmonic-rich ones (saw, weak fundamental) — and stays silent on silence/noise;
//   - note naming / cents / A4 reference math;
//   - tap tempo averages the last taps and restarts after a pause;
//   - the click scheduler lays beats exactly 60/bpm apart, accents the downbeat, subdivides, and picks up
//     a tempo change on the next click.
import test from 'node:test';
import assert from 'node:assert/strict';
import { Metronome, makeTapTempo, detectPitch, noteFromFreq as noteFromFreqRaw } from '../apps/metronome/www/app.js';

// ── synthetic signals ────────────────────────────────────────────────────────────────────────────
function tone(f, sr, kind = 'sine', N = 2048, amp = 0.5) {
  const b = new Float32Array(N);
  for (let i = 0; i < N; i++) {
    const t = i / sr;
    let v = 0;
    if (kind === 'sine') v = Math.sin(2 * Math.PI * f * t);
    else if (kind === 'saw') {                       // band-limited sawtooth (a bowed / plucked string's spectrum)
      for (let h = 1; h <= 20; h++) if (f * h < sr / 2) v += Math.sin(2 * Math.PI * f * h * t) / h;
      v *= 0.6;
    } else if (kind === 'weakfund') {               // weak fundamental, strong 2nd/3rd harmonics (the octave-error trap)
      v = 0.4 * (0.2 * Math.sin(2 * Math.PI * f * t) + Math.sin(2 * Math.PI * 2 * f * t) + 0.8 * Math.sin(2 * Math.PI * 3 * f * t));
    }
    b[i] = amp * v;
  }
  return b;
}
const cents = (got, want) => 1200 * Math.log2(got / want);
// Guitar low E … violin/flute high D: the range the tuner claims (60–1200 Hz).
const NOTES = [61.74, 65.41, 82.41, 98.0, 110.0, 146.83, 196.0, 246.94, 261.63, 329.63, 440.0, 659.26, 880.0, 987.77, 1174.66];

test('tuner: every note in 60–1200 Hz is found in the right octave, at 44.1k / 48k / 16k, for sine, saw and weak-fundamental tones', () => {
  const bad = [];
  for (const sr of [44100, 48000, 16000]) for (const kind of ['sine', 'saw', 'weakfund']) for (const f of NOTES) {
    const p = detectPitch(tone(f, sr, kind), sr);
    if (!p) { bad.push(`${sr} Hz ${kind} ${f}: no pitch`); continue; }
    const c = cents(p.freq, f);
    if (Math.abs(c) > 50) bad.push(`${sr} Hz ${kind} ${f}: read ${p.freq.toFixed(1)} Hz (${c.toFixed(0)} cents)`);
  }
  assert.deepEqual(bad, [], 'octave / wrong-note errors');
});

test('tuner: a pure tone reads within ±1 cent; a saw within ±2 cents on the browser mic (the in-tune gate is ±5)', () => {
  for (const sr of [44100, 48000]) for (const f of NOTES) {
    const s = detectPitch(tone(f, sr, 'sine'), sr);
    assert.ok(s && Math.abs(cents(s.freq, f)) <= 1, `${sr} sine ${f}: ${s && s.freq}`);
    const w = detectPitch(tone(f, sr, 'saw'), sr);
    assert.ok(w && Math.abs(cents(w.freq, f)) <= 2, `${sr} saw ${f}: ${w && w.freq}`);
  }
  for (const f of NOTES) {                          // the Cardputer mic: coarser lag grid, still well inside the gate for pure tones
    const s = detectPitch(tone(f, 16000, 'sine'), 16000);
    assert.ok(s && Math.abs(cents(s.freq, f)) <= 1, `16k sine ${f}: ${s && s.freq}`);
  }
});

test('tuner: silence, near-silence and white noise give no pitch (the needle must not chase noise)', () => {
  assert.equal(detectPitch(new Float32Array(2048), 44100), null, 'silence');
  assert.equal(detectPitch(tone(440, 44100, 'sine', 2048, 0.003), 44100), null, 'below the RMS gate');
  let seed = 12345;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296) - 0.5;   // deterministic noise
  for (let k = 0; k < 5; k++) {
    const n = new Float32Array(2048).map(() => rnd());
    assert.equal(detectPitch(n, 44100), null, 'white noise #' + k);
  }
});

test('tuner: a moderately noisy note still reads correctly, with a clarity score', () => {
  let seed = 7;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) >>> 0) / 4294967296) - 0.5;
  const b = tone(196, 44100, 'saw');
  for (let i = 0; i < b.length; i++) b[i] += 0.05 * rnd();
  const p = detectPitch(b, 44100);
  assert.ok(p, 'pitch found');
  assert.ok(Math.abs(cents(p.freq, 196)) < 5, 'G3 within 5 cents: ' + p.freq);
  assert.ok(p.clarity > 0.8 && p.clarity <= 1, 'clarity ' + p.clarity);
});

test('note naming: letter + solfège names, octave, cents and the A4 reference', () => {
  const noteFromFreq = (f, a4) => { const n = noteFromFreqRaw(f, a4); return { ...n, cents: n.cents + 0 }; };   // -0 and 0 print the same
  assert.deepEqual(noteFromFreq(440), { name: 'A', solfege: 'La', octave: 4, cents: 0 });
  assert.deepEqual(noteFromFreq(261.63), { name: 'C', solfege: 'Do', octave: 4, cents: 0 });
  assert.deepEqual(noteFromFreq(466.16), { name: 'A#', solfege: 'La#', octave: 4, cents: 0 });
  assert.deepEqual(noteFromFreq(82.41), { name: 'E', solfege: 'Mi', octave: 2, cents: 0 });
  assert.deepEqual(noteFromFreq(246.94), { name: 'B', solfege: 'Si', octave: 3, cents: 0 });
  const flatC = noteFromFreq(259);                   // a flat C4 is still C4, never "B3 +83"
  assert.equal(flatC.name + flatC.octave, 'C4'); assert.equal(flatC.cents, -17);
  const sharpA = noteFromFreq(445);
  assert.equal(sharpA.name + sharpA.octave, 'A4'); assert.equal(sharpA.cents, 20);
  assert.deepEqual(noteFromFreq(432, 432), { name: 'A', solfege: 'La', octave: 4, cents: 0 }, 'A4 = 432 reference');
  assert.equal(noteFromFreq(440, 432).cents, 32, '440 Hz against A4=432 is 32 cents sharp');
  assert.deepEqual(noteFromFreq(27.5), { name: 'A', solfege: 'La', octave: 0, cents: 0 });
});

// performance.now() is what tap tempo reads; drive it by hand.
function withClock(fn) {
  const real = Object.getOwnPropertyDescriptor(globalThis, 'performance');
  let now = 0;
  Object.defineProperty(globalThis, 'performance', { value: { now: () => now }, configurable: true, writable: true });
  try { return fn((ms) => { now = ms; }); } finally { Object.defineProperty(globalThis, 'performance', real); }
}

test('tap tempo: averages the taps, needs two, restarts after a 2 s pause, keeps only the last five', () => {
  withClock((at) => {
    const tap = makeTapTempo();
    at(1000); assert.equal(tap(), null, 'one tap is not a tempo');
    at(1500); assert.equal(tap(), 120);
    at(2000); assert.equal(tap(), 120);
    at(2510); assert.equal(tap(), 119, 'average of the intervals, not the last one');
    at(5000); assert.equal(tap(), null, 'a pause > 2 s starts a new phrase');
    at(5600); assert.equal(tap(), 100);
    // five more taps at 400 ms: once the 600 ms interval leaves the 5-tap window the tempo is exactly 150
    for (let k = 1; k <= 5; k++) { at(5600 + 400 * k); tap(); }
    at(5600 + 400 * 6); assert.equal(tap(), 150);
  });
});

// A Web Audio stand-in: records every scheduled click (time + pitch). The engine only uses these calls.
class FakeAudioContext {
  constructor() { this.currentTime = 0; this.clicks = []; this.destination = {}; }
  resume() { return Promise.resolve(); }
  createOscillator() {
    const ctx = this;
    const o = { frequency: { value: 0 }, connect: (g) => g, start(when) { ctx.clicks.push({ when, freq: o.frequency.value }); }, stop() {} };
    return o;
  }
  createGain() { return { gain: { setValueAtTime() {}, exponentialRampToValueAtTime() {} }, connect: (d) => d }; }
}
function run(m, seconds) {            // advance the audio clock in 10 ms steps, letting the scheduler look ahead
  for (let t = m.ctx.currentTime; t <= seconds; t += 0.01) { m.ctx.currentTime = t; m._schedule(); }
}
function newMetronome() {
  globalThis.window = { AudioContext: FakeAudioContext };
  const m = new Metronome(null);
  m.start(); m.stop();                 // create the context; the test drives _schedule() itself
  return m;
}
const gaps = (cl) => cl.slice(1).map((c, i) => c.when - cl[i].when);

test('scheduler: 120 BPM in 4/4 = a click every 0.5 s, accent on beat 1 of every bar', () => {
  const m = newMetronome();
  run(m, 4);
  const cl = m.ctx.clicks;
  assert.ok(cl.length >= 8, 'clicks scheduled: ' + cl.length);
  assert.ok(gaps(cl).every((g) => Math.abs(g - 0.5) < 1e-6), 'gaps ' + gaps(cl));
  cl.forEach((c, i) => assert.equal(c.freq, i % 4 === 0 ? 1568 : 1047, 'click ' + i));
  assert.ok(cl[cl.length - 1].when <= 4 + 0.12 + 1e-9, 'never schedules more than the lookahead');
});

test('scheduler: subdivisions, odd meters, extreme tempos and a live tempo change', () => {
  const m = newMetronome();
  m.bpm = 90; m.subdiv = 3; m.beats = 3;
  run(m, 6);
  let cl = m.ctx.clicks;
  assert.ok(gaps(cl).every((g) => Math.abs(g - 60 / 90 / 3) < 1e-6), 'triplets at 90 BPM');
  cl.forEach((c, i) => assert.equal(c.freq, i % 9 === 0 ? 1568 : i % 3 === 0 ? 1047 : 784, 'click ' + i + ' (accent / beat / sub)'));

  for (const bpm of [30, 260]) {
    const x = newMetronome(); x.bpm = bpm;
    run(x, 10);
    assert.ok(gaps(x.ctx.clicks).every((g) => Math.abs(g - 60 / bpm) < 1e-6), bpm + ' BPM');
  }

  const y = newMetronome();
  run(y, 2);
  const before = y.ctx.clicks.length;
  y.bpm = 60;
  run(y, 6);
  const g = gaps(y.ctx.clicks);
  assert.ok(g.slice(0, before - 1).every((d) => Math.abs(d - 0.5) < 1e-6), 'clicks already scheduled keep 120');
  assert.ok(g.slice(before).every((d) => Math.abs(d - 1) < 1e-6), 'every later gap is 60 BPM: ' + g);
});

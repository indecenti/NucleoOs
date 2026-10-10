// constellations-sfx.js — sound for Costellazioni. Effects are 100% Web Audio synthesis (no files):
// short bursts of oscillators / cached noise through a shared limiter bus with a cheap feedback-delay
// "reverb", stereo-panned and distance-scaled for the 6DOF flight, plus a continuous engine voice that
// follows throttle and boost. Music: the four ACE-Step tracks from stelle/assets.js when present
// (theme / islands / dimming / storm, looped, ducked under big explosions), else the procedural
// minor-key bed. Lazy context, resumed on the first user gesture.
// Planet ambience (ambStart / ambSet / ambStop / thunder / lightningCrackle): a procedural bed under the
// music — wind + gusts, re-entry roar, rain / snow / dust, surf, ground rush, a baked biome bed (insects,
// frogs, singing glass, lava bubbling, gas-giant drone) and sparse discrete events (calls, cracks, chimes).
import * as A from '/apps/games/games/stelle/assets.js';

// ── ambience building blocks (module level: nothing here runs per frame) ─────────────────────────────
const TAU = Math.PI * 2;
const cl = (v) => (v > 0 ? (v < 1 ? v : 1) : 0);          // clamp 0..1, NaN/undefined -> 0
const AMB_OUT = 0.36;   // ambience bus at k = 1: calm beds ~6-8 dB(A) under the music tracks (gain 0.12-0.15), flight ~2 dB under
// layer calibration (bed units, before presence × AMB_OUT) — measured so a calm ground bed sits well under the music
const AC_ = { rush: 0.30, whis: 1.2, roar: 0.15, crack: 0.30, low: 0.19, hiss: 0.06, drop: 0.10, snow: 0.16, grit: 0.12, surf: 0.32, bed: 0.12 };
// per-biome character: breeze at the ground, gust depth, wind lowpass / whistle (level, pitch factor, Q, 2nd-resonance
// share), biome rumble + its lowpass, how high the bed carries (m; 0 = follows air density), baked bed, echo, far hiss
const AMB_BIOME = {
  rocky:    { breeze: .45, gust: .35, wLP: 520, whis: .16, wF: 1.00, wQ: 7,  w2: .30, rum: .05, lowF: 30, reach: 500,  bed: null,       bedLvl: 0,   echo: .26, dly: .27, fb: .32 },
  desert:   { breeze: .42, gust: .40, wLP: 640, whis: .12, wF: 1.15, wQ: 4,  w2: .15, rum: .04, lowF: 25, reach: 500,  bed: null,       bedLvl: 0,   echo: .14, dly: .36, fb: .24 },
  ocean:    { breeze: .26, gust: .30, wLP: 470, whis: .07, wF: 0.90, wQ: 3,  w2: .10, rum: .08, lowF: 20, reach: 700,  bed: null,       bedLvl: 0,   echo: .10, dly: .30, fb: .20 },
  ice:      { breeze: .38, gust: .42, wLP: 760, whis: .19, wF: 1.40, wQ: 17, w2: .70, rum: .03, lowF: 20, reach: 500,  bed: null,       bedLvl: 0,   echo: .40, dly: .21, fb: .42 },
  jungle:   { breeze: .12, gust: .25, wLP: 430, whis: .04, wF: 0.90, wQ: 3,  w2: 0,   rum: .03, lowF: 20, reach: 380,  bed: 'jungle',   bedLvl: 0.9, echo: .20, dly: .17, fb: .30 },
  volcanic: { breeze: .18, gust: .30, wLP: 380, whis: .08, wF: 0.80, wQ: 4,  w2: .10, rum: .55, lowF: 110, reach: 1400, bed: 'volcanic', bedLvl: 3.0, echo: .24, dly: .31, fb: .35, hiss: .35 },
  gas:      { breeze: .75, gust: .45, wLP: 340, whis: .10, wF: 0.55, wQ: 5,  w2: .20, rum: .65, lowF: 60, reach: 0,    bed: 'gas',      bedLvl: 2.0, echo: .30, dly: .40, fb: .45 },
  crystal:  { breeze: .16, gust: .25, wLP: 560, whis: .10, wF: 1.25, wQ: 13, w2: .55, rum: .03, lowF: 20, reach: 800,  bed: 'crystal',  bedLvl: .62, echo: .44, dly: .24, fb: .45 },
};
for (const id in AMB_BIOME) AMB_BIOME[id].id = id;
const BED_IDS = { jungle: ['jday', 'jnight'], volcanic: ['volcanic'], gas: ['gas'], crystal: ['crystal'] };
// AudioParam slots the per-tick setter remembers (only a meaningful change reaches setTargetAtTime)
const K = { out: 0, pres: 1, rushG: 2, rushF: 3, w1G: 4, w1F: 5, w1Q: 6, w2G: 7, w2F: 8, w2Q: 9, modW: 10, modF: 11, roarG: 12, roarF: 13,
  crackG: 14, crackR: 15, lowG: 16, lowF: 17, hissG: 18, hissF: 19, dropG: 20, dropR: 21, snowG: 22, gritG: 23, gritF: 24, surfG: 25,
  eWet: 26, dly: 27, eFb: 28, bdG: 29, bd0: 30, bd1: 31, modH: 32 };
const NK = 33;

// Looped noise: generate N samples + a tail and fold the tail over the head with an equal-power crossfade,
// so the loop point is seamless even for correlated (pink / brown) noise. Normalised to RMS `rms`.
// Generators (yield every 32k samples): the buffers are synthesised in small chunks off the frame path.
function* loopNoise(ac, ch, sec, sr, mk, rms) {
  const b = ac.createBuffer(ch, Math.round(sec * sr), sr); let e = 0;
  for (let c = 0; c < ch; c++) {
    const d = b.getChannelData(c), N = d.length, F = Math.min(N >> 2, Math.round(sr * 0.3)), g = mk();
    for (let i = 0; i < N; i++) { d[i] = g(); if ((i & 32767) === 32767) yield; }
    for (let i = 0; i < F; i++) { const w = i / F; d[i] = d[i] * Math.sqrt(w) + g() * Math.sqrt(1 - w); }
    for (let i = 0; i < N; i++) e += d[i] * d[i];
    yield;
  }
  const s = rms / Math.sqrt(e / (b.length * ch) || 1);
  for (let c = 0; c < ch; c++) { const d = b.getChannelData(c); for (let i = 0; i < d.length; i++) d[i] *= s; yield; }
  return b;
}
const white = () => () => Math.random() * 2 - 1;
const pink = () => { let b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0; return () => {   // Kellet's refined pink
  const w = Math.random() * 2 - 1;
  b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.969 * b2 + w * 0.153852;
  b3 = 0.8665 * b3 + w * 0.3104856; b4 = 0.55 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.016898;
  const o = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362; b6 = w * 0.115926; return o; }; };
const brown = () => { let b = 0; return () => (b = (b + 0.02 * (Math.random() * 2 - 1)) / 1.02); };
// Sparse impulses (rain drops / plasma crackle), written with wrap-around so the loop has no seam. Peak 0.9.
function* impulses(ac, sec, sr, perSec, one) {
  const b = ac.createBuffer(2, Math.round(sec * sr), sr), N = b.length; let pk = 0;
  for (let c = 0; c < 2; c++) {
    const d = b.getChannelData(c);
    for (let n = Math.round(sec * perSec); n-- > 0;) { one(d, (Math.random() * N) | 0, N, sr); if ((n & 127) === 0) yield; }
    for (let i = 0; i < N; i++) { const v = Math.abs(d[i]); if (v > pk) pk = v; }
    yield;
  }
  const s = 0.9 / (pk || 1);
  for (let c = 0; c < 2; c++) { const d = b.getChannelData(c); for (let i = 0; i < N; i++) d[i] *= s; }
  return b;
}
function drop(d, p, N, sr) {      // a water drop: tiny click + a short rising "plink"; heavy-tailed loudness
  const f = 1500 + Math.pow(Math.random(), 1.5) * 4200, len = Math.round(sr * (0.005 + Math.random() * 0.02)), a = 0.04 + 0.96 * Math.pow(Math.random(), 4);
  const tc = len * 0.3, ck = sr * 0.0007; let ph = 0;
  for (let j = 0; j < len; j++) { ph += TAU * f * (1 + 0.35 * j / len) / sr; d[(p + j) % N] += a * (Math.sin(ph) * Math.exp(-j / tc) + 0.5 * (Math.random() * 2 - 1) * Math.exp(-j / ck)); }
}
function spark(d, p, N, sr) {     // a crackle: a few ms of decaying noise; heavy-tailed loudness
  const len = Math.round(sr * (0.0005 + Math.random() * 0.004)), a = Math.pow(Math.random(), 3), tc = len * 0.35;
  for (let j = 0; j < len; j++) d[(p + j) % N] += a * (Math.random() * 2 - 1) * Math.exp(-j / tc);
}
// Slow modulation loop (gust flutter): smooth random -1..1 (cosine-interpolated points every 0.5 s), seamless.
function modBuf(ac) {
  const sr = 8000, P = 34, step = sr / 2, b = ac.createBuffer(1, P * step, sr), d = b.getChannelData(0), pts = [];
  for (let i = 0; i < P; i++) pts.push(Math.random() * 2 - 1);
  for (let i = 0; i < P; i++) { const a = pts[i], z = pts[(i + 1) % P]; for (let j = 0; j < step; j++) { const w = 0.5 - 0.5 * Math.cos(Math.PI * j / step); d[i * step + j] = a + (z - a) * w; } }
  return b;
}
// every looped buffer the ambience needs, cheapest first (~35 ms of synthesis in total, spread over ~10 chunks)
function* ambBufs(ac, o) {
  o.mod = modBuf(ac); yield;
  o.brown = yield* loopNoise(ac, 1, 6, 12000, brown, 0.3);
  o.crack = yield* impulses(ac, 3, 24000, 70, spark);
  o.drops = yield* impulses(ac, 4, 24000, 150, drop);
  o.pink = yield* loopNoise(ac, 2, 6, 24000, pink, 0.3);
  o.white = yield* loopNoise(ac, 2, 4, ac.sampleRate, white, 0.3);
}
// drive a generator in ~3 ms slices from setTimeout; `done` runs once it has finished
function chunked(gen, done) {
  const step = () => {
    const t0 = performance.now();
    try { while (!gen.next().done) if (performance.now() - t0 > 3) { setTimeout(step, 0); return; } } catch { return; }   // failure: stays absent
    done();
  };
  setTimeout(step, 0);
}

// Baked biome beds: 8 s stereo loops at 22.05 kHz synthesised in JS — in small chunks off the frame path
// (a generator stepped from setTimeout), exactly periodic: every frequency / rate is a multiple of 1/8 Hz and
// discrete sounds wrap round the loop, so the seam is inaudible. One buffer source plays the whole texture.
const BR = 22050, BL = 8, BN = BR * BL, CN = 1600, CK = CN / BN;
const fq = (f) => Math.round(f * BL) / BL;
function rng(seed) { let a = seed >>> 0; return () => { a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a); t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }
function curve(R, h0, h1) {        // smooth periodic control curve 0..1 built from loop harmonics h0..h1
  const c = new Float32Array(CN + 1), a = [], p = []; let lo = 1e9, hi = -1e9;
  for (let h = h0; h <= h1; h++) { a.push((0.3 + R()) / h); p.push(R() * TAU); }
  for (let i = 0; i < CN; i++) { let v = 0; for (let h = 0; h < a.length; h++) v += a[h] * Math.cos(TAU * (h0 + h) * i / CN + p[h]); c[i] = v; if (v < lo) lo = v; if (v > hi) hi = v; }
  for (let i = 0; i < CN; i++) c[i] = (c[i] - lo) / (hi - lo || 1);
  c[CN] = c[0]; return c;
}
const cv = (c, i) => { const x = i * CK, j = x | 0; return c[j] + (c[j + 1] - c[j]) * (x - j); };
const gate = (v, th) => (v <= th - 0.08 ? 0 : v >= th + 0.08 ? 1 : (v - th + 0.08) / 0.16);
const panL = (p) => Math.sqrt((1 - p) / 2), panR = (p) => Math.sqrt((1 + p) / 2);
// pulsed insect: carrier f, pulses at `rate` (duty), switched on/off by a slow curve over threshold `th`
function* trill(L, R, f, rate, duty, c, th, lvl, pan) {
  const w = TAU * fq(f) / BR, rr = fq(rate) / BR, gl = lvl * panL(pan), gr = lvl * panR(pan);
  for (let i = 0; i < BN; i++) {
    if ((i & 8191) === 0) yield;
    const g = gate(cv(c, i), th); if (!g) continue;
    const p = (i * rr) % 1; if (p >= duty) continue;
    const e = Math.sin(Math.PI * p / duty), s = (Math.sin(w * i) + 0.18 * Math.sin(2 * w * i)) * e * e * g;
    L[i] += s * gl; R[i] += s * gr;
  }
}
// cicada: rough triple carrier gated by a fast tymbal buzz, swelling with a slow curve
function* cicada(L, R, f, buzz, c, lvl, pan) {
  const w1 = TAU * fq(f) / BR, w2 = TAU * fq(f * 1.063) / BR, w3 = TAU * fq(f * 0.52) / BR, br = fq(buzz) / BR, gl = lvl * panL(pan), gr = lvl * panR(pan);
  for (let i = 0; i < BN; i++) {
    if ((i & 8191) === 0) yield;
    let e = cv(c, i); e = e * e * e; if (e < 0.003) continue;
    const p = (i * br) % 1; if (p >= 0.38) continue;
    const s = (Math.sin(w1 * i) + 0.6 * Math.sin(w2 * i) + 0.25 * Math.sin(w3 * i)) * Math.sin(Math.PI * p / 0.38) * e;
    L[i] += s * gl; R[i] += s * gr;
  }
}
// a far shimmering wall of tiny high voices
function* chorus(L, R, rnd, n, f0, f1, lvl) {
  for (let v = 0; v < n; v++) {
    const w = TAU * fq(f0 + rnd() * (f1 - f0)) / BR, am = TAU * fq(14 + rnd() * 30) / BR, ph = rnd() * TAU, c = curve(rnd, 1, 4), pan = rnd() * 1.6 - 0.8, gl = lvl * panL(pan), gr = lvl * panR(pan);
    for (let i = 0; i < BN; i++) {
      if ((i & 8191) === 0) yield;
      const s = Math.sin(w * i) * (0.55 + 0.45 * Math.sin(am * i + ph)) * (0.2 + 0.8 * cv(c, i));
      L[i] += s * gl; R[i] += s * gr;
    }
  }
}
// cricket-like chirps: `n` pulses at `pr` Hz every P seconds (P divides the loop), rasp = in-pulse AM
function* chirper(L, R, f, P, n, pr, rasp, c, lvl, pan) {
  const w = TAU * fq(f) / BR, Pn = BN / Math.round(BL / P), on = Math.round(n / pr * BR), ra = TAU * fq(rasp) / BR, gl = lvl * panL(pan), gr = lvl * panR(pan);
  for (let i = 0; i < BN; i++) {
    if ((i & 8191) === 0) yield;
    const q = i % Pn; if (q >= on) continue;
    const p = (q * pr / BR) % 1; if (p >= 0.6) continue;
    const e = Math.sin(Math.PI * p / 0.6), s = (Math.sin(w * i) + 0.2 * Math.sin(2 * w * i)) * e * e * (0.3 + 0.7 * cv(c, i)) * (rasp ? 0.5 + 0.5 * Math.sin(ra * i) : 1);
    L[i] += s * gl; R[i] += s * gr;
  }
}
// discrete calls placed round the loop (wrapping): frogs, peepers, lava bubbles
function* events(L, R, rnd, n, one) {
  for (let e = 0; e < n; e++) { one(L, R, (rnd() * BN) | 0, rnd); if ((e & 3) === 3) yield; }
}
function frog(f0, lvl, pan) {        // pulsed throaty croak ("brr-ump"), 1-2 per call
  return (L, R, p0, rnd) => {
    const reps = 1 + ((rnd() * 2) | 0), gl = lvl * panL(pan), gr = lvl * panR(pan);
    for (let r = 0; r < reps; r++) {
      const dur = 0.16 + rnd() * 0.14, len = Math.round(dur * BR), p = p0 + Math.round(r * (dur + 0.09) * BR), pr = 28 + rnd() * 14, f = f0 * (0.92 + rnd() * 0.16); let ph = 0;
      for (let j = 0; j < len; j++) {
        const t = j / BR, x = (t * pr) % 1, pe = x < 0.5 ? Math.sin(Math.PI * x / 0.5) : 0, env = Math.pow(Math.sin(Math.PI * j / len), 0.6);
        ph += TAU * f * (1 - 0.1 * j / len) / BR;
        const s = (Math.sin(ph) + 0.7 * Math.sin(2 * ph) + 0.45 * Math.sin(3 * ph) + 0.2 * Math.sin(5 * ph)) * pe * env, k = (p + j) % BN;
        L[k] += s * gl; R[k] += s * gr;
      }
    }
  };
}
function peeper(f0, lvl, pan) {       // tiny rising "peep", a short burst of them
  return (L, R, p0, rnd) => {
    const n = 2 + ((rnd() * 4) | 0), gap = 0.25 + rnd() * 0.25, gl = lvl * panL(pan), gr = lvl * panR(pan);
    for (let r = 0; r < n; r++) {
      const len = Math.round((0.04 + rnd() * 0.03) * BR), p = p0 + Math.round(r * gap * BR); let ph = 0;
      for (let j = 0; j < len; j++) { ph += TAU * f0 * (1 + 0.28 * j / len) / BR; const e = Math.sin(Math.PI * j / len), s = Math.sin(ph) * e * e, k = (p + j) % BN; L[k] += s * gl; R[k] += s * gr; }
    }
  };
}
function bubble(lvl) {                 // lava bubble: a rising "bloop" (big ones deep and thick)
  return (L, R, p, rnd) => {
    const big = rnd() < 0.22, f0 = big ? 55 + rnd() * 55 : 110 + rnd() * 300, dur = big ? 0.12 + rnd() * 0.16 : 0.03 + rnd() * 0.08;
    const rise = big ? 1.4 + rnd() : 2.5 + rnd() * 3, a = (big ? 1 : 0.3 + 0.5 * rnd()) * lvl, pan = rnd() * 1.4 - 0.7, gl = a * panL(pan), gr = a * panR(pan), len = Math.round(dur * BR); let ph = 0;
    for (let j = 0; j < len; j++) {
      const t = j / len; ph += TAU * f0 * (1 + rise * t) / BR;
      const s = (Math.sin(ph) + (big ? 0.45 * Math.sin(2 * ph) : 0)) * (1 - Math.exp(-j / (0.002 * BR))) * Math.exp(-3.5 * t), k = (p + j) % BN;
      L[k] += s * gl; R[k] += s * gr;
    }
  };
}
function seethe(lvl) {                 // tiny crackles of cooling crust
  return (L, R, p, rnd) => {
    const len = Math.round(BR * (0.001 + rnd() * 0.004)), a = Math.pow(rnd(), 2) * lvl, pan = rnd() * 1.6 - 0.8; let y = 0;
    for (let j = 0; j < len; j++) { y += 0.35 * ((rnd() * 2 - 1) - y); const s = y * a * Math.exp(-3 * j / len), k = (p + j) % BN; L[k] += s * panL(pan); R[k] += s * panR(pan); }
  };
}
// singing glass: pairs of slightly detuned partials beating slowly, each swelling on its own curve
function* glass(L, R, rnd, lvl) {
  const base = [392, 588, 784, 980, 1176, 1568, 2352], beat = [0.5, 0.375, 0.25, 0.625, 0.75, 0.125, 0.5];
  for (let v = 0; v < base.length; v++) {
    const w1 = TAU * fq(base[v]) / BR, w2 = TAU * fq(base[v] + beat[v]) / BR, a = lvl / (1 + v * 0.45), c = curve(rnd, 1, 3), pan = (v % 2 ? 1 : -1) * (0.15 + 0.55 * rnd()), gl = a * panL(pan), gr = a * panR(pan);
    for (let i = 0; i < BN; i++) {
      if ((i & 8191) === 0) yield;
      let e = cv(c, i); e = 0.15 + 0.85 * e * e; const s = (Math.sin(w1 * i) + Math.sin(w2 * i)) * e;
      L[i] += s * gl; R[i] += s * gr;
    }
  }
  for (const f of [3136, 3920]) {     // airy high shimmer with a slow tremolo
    const w = TAU * fq(f) / BR, tr = TAU * fq(5.5) / BR, c = curve(rnd, 2, 5), pan = rnd() * 1.4 - 0.7, gl = lvl * 0.12 * panL(pan), gr = lvl * 0.12 * panR(pan);
    for (let i = 0; i < BN; i++) { if ((i & 8191) === 0) yield; const s = Math.sin(w * i) * (0.6 + 0.4 * Math.sin(tr * i)) * cv(c, i); L[i] += s * gl; R[i] += s * gr; }
  }
}
// gas-giant drone: detuned low voices whose harmonics pass through a slowly wandering formant (breathing "wah")
function* drone(L, R, rnd, lvl) {
  const fs = [73, 73.75, 109.5, 146.5].map(fq), amp = [1, 0.8, 0.5, 0.3], pans = [-0.45, 0.45, -0.1, 0.2], H = 6, c = curve(rnd, 1, 3), sw = curve(rnd, 1, 2), wt = new Float64Array(H + 1);
  for (let v = 0; v < fs.length; v++) {
    const w = TAU * fs[v] / BR, gl = lvl * amp[v] * panL(pans[v]), gr = lvl * amp[v] * panR(pans[v]);
    for (let i = 0; i < BN; i++) {
      if ((i & 8191) === 0) yield;
      if ((i & 63) === 0) { const hc = 1.2 + 4.5 * cv(c, i); for (let h = 1; h <= H; h++) wt[h] = Math.exp(-((h - hc) * (h - hc)) / 2.5) / h; }
      let s = 0; for (let h = 1; h <= H; h++) s += wt[h] * Math.sin(h * w * i);
      s *= 0.55 + 0.45 * cv(sw, i); L[i] += s * gl; R[i] += s * gr;
    }
  }
}
const BAKE = {
  *jday(L, R, r) {
    yield* chorus(L, R, r, 5, 6600, 7900, 0.10);
    yield* cicada(L, R, 4100, 180, curve(r, 1, 5), 0.50, -0.55);
    yield* cicada(L, R, 4730, 212, curve(r, 1, 5), 0.38, 0.60);
    yield* trill(L, R, 5300, 14, 0.42, curve(r, 2, 12), 0.55, 0.22, -0.8);
    yield* trill(L, R, 6150, 23, 0.40, curve(r, 2, 12), 0.50, 0.18, 0.25);
    yield* trill(L, R, 3650, 9.5, 0.45, curve(r, 2, 10), 0.60, 0.25, 0.75);
  },
  *jnight(L, R, r) {
    yield* chorus(L, R, r, 4, 5200, 6400, 0.07);
    yield* chirper(L, R, 4400, 0.5, 3, 30, 0, curve(r, 1, 6), 0.45, -0.6);
    yield* chirper(L, R, 4950, 2 / 3, 4, 34, 0, curve(r, 1, 6), 0.35, 0.5);
    yield* chirper(L, R, 3800, 0.8, 3, 26, 0, curve(r, 1, 6), 0.30, 0.1);
    yield* chirper(L, R, 6800, 2, 3, 7, 160, curve(r, 1, 4), 0.20, 0.8);        // katydid rasp "ka-ty-did"
    yield* events(L, R, r, 11, frog(170, 0.30, -0.4));
    yield* events(L, R, r, 8, frog(290, 0.22, 0.55));
    yield* events(L, R, r, 12, peeper(2350, 0.14, -0.2));
  },
  *crystal(L, R, r) { yield* glass(L, R, r, 0.5); },
  *gas(L, R, r) { yield* drone(L, R, r, 0.6); },
  *volcanic(L, R, r) { yield* events(L, R, r, 52, bubble(0.6)); yield* events(L, R, r, 420, seethe(0.25)); },
};
const SEED = { jday: 11, jnight: 23, crystal: 37, gas: 41, volcanic: 53 };
const CHIME = [784, 980, 1176, 1568, 1960, 2352];

export class SFX {
  constructor() {
    this.ac = null; this.master = null; this.muted = false; this.music = null; this._fx = null; this._lastLaser = 0;
    this._amb = null; this._ambOn = false; this._ambVol = 1; this._ambLvl = 0; this._ambNext = 0; this._ambLast = 0; this._ambPz = false;
    this._ambQuiet = 0; this._ambWd = 0; this._ambStopT = 0; this._ab = null; this._abG = null; this._abO = null; this._abk = null; this._engAmb = 1; this._engLvl = 0;
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
  startDrone() { this.combatMusic(true); }
  stopDrone() { this.combatMusic(false); }

  // ── 6DOF flight layer ───────────────────────────────────────────────────────────────────────
  _nbuf() {   // one cached 2 s white-noise buffer, read at random offsets (no per-shot allocation)
    if (this._nb) return this._nb;
    const ac = this.ac, n = ac.sampleRate * 2, b = ac.createBuffer(1, n, ac.sampleRate), d = b.getChannelData(0);
    for (let i = 0; i < n; i++) d[i] = Math.random() * 2 - 1;
    return (this._nb = b);
  }
  _out(pan, dest) {
    if (!pan || !this.ac.createStereoPanner) return dest || this.master;
    const p = this.ac.createStereoPanner(); p.pan.value = Math.max(-1, Math.min(1, pan)); p.connect(dest || this.master); return p;
  }
  _n(t0, dur, { gain = 0.2, freq = 1200, q = 0.7, type = 'bandpass', pan = 0, glide = 0, dest = null } = {}) {
    const ac = this.ac, src = ac.createBufferSource(); src.buffer = this._nbuf();
    const f = ac.createBiquadFilter(); f.type = type; f.frequency.setValueAtTime(freq, t0); f.Q.value = q;
    if (glide) f.frequency.exponentialRampToValueAtTime(Math.max(30, freq * glide), t0 + dur);
    const g = ac.createGain(); g.gain.setValueAtTime(gain, t0); g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
    src.connect(f); f.connect(g); g.connect(this._out(pan, dest)); src.start(t0, Math.random() * 1.5); src.stop(t0 + dur + 0.02);
  }
  _tp(freq, t0, dur, o = {}) { const dest = this._out(o.pan || 0, o.dest); return this._tone(freq, t0, dur, { ...o, dest }); }
  _ok() { return this._ensure() && !this.muted; }
  laserP(pan = 0) {
    if (!this._ok()) return; const t = this.ac.currentTime;
    if (t - this._lastLaser < 0.03) return; this._lastLaser = t;
    this._tp(1900 + Math.random() * 120, t, 0.12, { type: 'sawtooth', gain: 0.13, glide: 0.12, pan });
    this._tp(980, t, 0.07, { type: 'square', gain: 0.06, glide: 0.4, pan });
    this._tp(140, t, 0.06, { type: 'sine', gain: 0.12, glide: 0.5 });
    this._n(t, 0.03, { gain: 0.05, freq: 4200, type: 'highpass', pan });
  }
  laserFar(vol, pan = 0, heavy = false) {
    if (!this._ok() || vol < 0.04) return; const t = this.ac.currentTime;
    if (t - (this._lastFar || 0) < 0.07) return; this._lastFar = t;
    const v = vol * vol;
    this._tp(heavy ? 700 : 1300 + Math.random() * 300, t, 0.1, { type: 'sawtooth', gain: 0.07 * v, glide: 0.25, pan });
    this._n(t, 0.05, { gain: 0.04 * v, freq: heavy ? 900 : 2500, q: 1, pan });
  }
  shieldHit() {
    if (!this._ok()) return; const t = this.ac.currentTime;
    if (t - (this._lastSh || 0) < 0.05) return; this._lastSh = t;
    this._n(t, 0.18, { gain: 0.13, freq: 3200, q: 3, glide: 0.35 });
    this._tone(1600, t, 0.14, { type: 'sine', gain: 0.07, glide: 0.5 });
    this._tone(400, t, 0.12, { type: 'triangle', gain: 0.06, glide: 0.6 });
  }
  boomAt(size = 1, vol = 1, pan = 0) {
    if (!this._ok() || vol < 0.03) return; const t = this.ac.currentTime;
    const k = Math.min(2.5, size), v = Math.min(1, vol) * (0.5 + 0.2 * k);
    this._n(t, 0.05, { gain: 0.3 * v, freq: 4000, type: 'highpass', pan });
    this._n(t, 0.35 + k * 0.25, { gain: 0.32 * v, freq: 520 - k * 120, type: 'lowpass', pan });
    this._tp(70 - k * 10, t, 0.3 + k * 0.2, { type: 'sine', gain: 0.4 * v, glide: 0.4 });
    this._n(t + 0.07, 0.25, { gain: 0.08 * v, freq: 1800, pan });
    if (k >= 1.5) { this._duck(0.35, 1.2 + k * 0.4); this._tp(43, t, 1.2, { type: 'sine', gain: 0.35 * v, glide: 0.5 }); this._n(t + 0.1, 1.3, { gain: 0.12 * v, freq: 300, type: 'lowpass' }); }
  }
  _duck(to, dur) {
    const g = this._musicGain(); if (!g) return; const t = this.ac.currentTime, base = this._musicLevel || 0.14;
    g.gain.cancelScheduledValues(t); g.gain.setValueAtTime(g.gain.value, t); g.gain.linearRampToValueAtTime(base * to, t + 0.05); g.gain.linearRampToValueAtTime(base, t + dur);
  }
  lockTone(p) { if (!this._ok()) return; const t = this.ac.currentTime; this._tone(900 + p * 900, t, 0.045, { type: 'square', gain: 0.05 }); }
  missileWarn() { if (!this._ok()) return; const t = this.ac.currentTime; for (let i = 0; i < 3; i++) { this._tone(1250, t + i * 0.12, 0.07, { type: 'square', gain: 0.08 }); this._tone(1860, t + i * 0.12 + 0.05, 0.05, { type: 'square', gain: 0.06 }); } }
  flare() { if (!this._ok()) return; const t = this.ac.currentTime; for (let i = 0; i < 4; i++) { this._n(t + i * 0.06, 0.18, { gain: 0.1, freq: 2600, q: 0.8, glide: 0.4 }); this._tone(300, t + i * 0.06, 0.05, { type: 'square', gain: 0.06, glide: 0.5 }); } }
  boost() { if (!this._ok()) return; const t = this.ac.currentTime; this._n(t, 0.7, { gain: 0.16, freq: 400, q: 0.6, glide: 4, type: 'bandpass' }); this._tone(60, t, 0.25, { type: 'sine', gain: 0.25, glide: 0.6 }); }
  pip(k = 0) { if (!this._ok()) return; const t = this.ac.currentTime; this._tone([520, 660, 800, 600, 700, 760][k % 6] || 600, t, 0.05, { type: 'square', gain: 0.06 }); this._tone(1400, t + 0.03, 0.03, { type: 'sine', gain: 0.03 }); }
  overheat() { if (!this._ok()) return; const t = this.ac.currentTime; this._tone(880, t, 0.3, { type: 'square', gain: 0.07, glide: 0.5 }); this._n(t, 0.5, { gain: 0.07, freq: 3000, type: 'highpass' }); }
  comms() { if (!this._ok()) return; const t = this.ac.currentTime; this._n(t, 0.09, { gain: 0.05, freq: 1800, q: 2 }); this._tone(1320, t + 0.02, 0.04, { type: 'sine', gain: 0.04 }); this._tone(1760, t + 0.07, 0.04, { type: 'sine', gain: 0.035 }); }
  warp(k = 1) { if (!this._ok()) return; const t = this.ac.currentTime, v = Math.max(0.2, k); this._tone(120, t, 0.7, { type: 'sawtooth', gain: 0.12 * v, glide: 9 }); this._n(t, 0.8, { gain: 0.1 * v, freq: 600, glide: 6, q: 0.6 }); this._tone(55, t + 0.6, 0.6, { type: 'sine', gain: 0.25 * v, glide: 0.5 }); }
  clang() { if (!this._ok()) return; const t = this.ac.currentTime; this._tone(320, t, 0.25, { type: 'square', gain: 0.1, glide: 0.7 }); this._tone(467, t, 0.3, { type: 'sine', gain: 0.08 }); this._n(t, 0.12, { gain: 0.14, freq: 900, q: 1.2 }); }
  cruiseSpool() { if (!this._ok()) return; const t = this.ac.currentTime; this._tone(70, t, 1.4, { type: 'sawtooth', gain: 0.1, glide: 3.5 }); this._n(t, 1.4, { gain: 0.08, freq: 300, glide: 8, q: 0.8 }); this._tone(880, t + 1.3, 0.12, { type: 'sine', gain: 0.06 }); }
  tether() { if (!this._ok()) return; const t = this.ac.currentTime; this._tone(90, t, 0.6, { type: 'sawtooth', gain: 0.12, glide: 1.6 }); this._n(t, 0.5, { gain: 0.08, freq: 5000, q: 4 }); }
  // continuous engine: two detuned oscillators + filtered noise; pitch and level follow throttle / boost
  engineStart() {
    if (!this._ok() || this._eng) return;
    const ac = this.ac, t = ac.currentTime;
    const g = ac.createGain(); g.gain.value = 0; g.connect(this.master);
    const lp = ac.createBiquadFilter(); lp.type = 'lowpass'; lp.frequency.value = 400; lp.Q.value = 2; lp.connect(g);
    const o1 = ac.createOscillator(), o2 = ac.createOscillator(); o1.type = 'sawtooth'; o2.type = 'square'; o1.frequency.value = 48; o2.frequency.value = 72; o2.detune.value = 9;
    const og = ac.createGain(); og.gain.value = 0.35; o1.connect(og); o2.connect(og); og.connect(lp);
    const ns = ac.createBufferSource(); ns.buffer = this._nbuf(); ns.loop = true; const nf = ac.createBiquadFilter(); nf.type = 'bandpass'; nf.frequency.value = 700; nf.Q.value = 0.7;
    const ng = ac.createGain(); ng.gain.value = 0.5; ns.connect(nf); nf.connect(ng); ng.connect(lp);
    o1.start(t); o2.start(t); ns.start(t);
    this._eng = { g, lp, o1, o2, ns, nf, ng };
  }
  engineSet(thr, boost, paused) {
    const e = this._eng; if (!e || !this.ac) return; const t = this.ac.currentTime;
    const lvl = paused ? 0 : 0.035 + thr * 0.05 + (boost ? 0.06 : 0);
    this._engLvl = lvl; e.g.gain.setTargetAtTime(lvl * this._engAmb, t, 0.15);   // _engAmb: quieter when landed (ambience)
    e.lp.frequency.setTargetAtTime(260 + thr * 700 + (boost ? 1400 : 0), t, 0.2);
    e.o1.frequency.setTargetAtTime(42 + thr * 30 + (boost ? 26 : 0), t, 0.3); e.o2.frequency.setTargetAtTime(63 + thr * 45 + (boost ? 40 : 0), t, 0.3);
    e.nf.frequency.setTargetAtTime(500 + thr * 900 + (boost ? 1800 : 0), t, 0.2);
  }
  engineStop() { const e = this._eng; if (!e) return; this._eng = null; const t = this.ac.currentTime; e.g.gain.setTargetAtTime(0, t, 0.1); setTimeout(() => { try { e.o1.stop(); e.o2.stop(); e.ns.stop(); } catch {} try { e.g.disconnect(); } catch {} }, 500); }
  // ── music: asset tracks when the manifest has them ─────────────────────────────────────────────
  _musicGain() { return this._track ? this._track.g : (this.music ? this.music.bus : null); }
  async playTrack(id, level = 0.16) {
    if (!this._ensure()) return false;
    if (this._track && this._track.id === id) return true;
    try { await A.loadManifest(); } catch { return false; }
    if (!A.has('music', id)) return false;
    const want = (this._wantTrack = id);
    let buf;
    try { buf = await this.ac.decodeAudioData(await A.musicBuffer(id)); } catch { return false; }
    if (this._wantTrack !== want) return false;
    this.stopTrack();
    const ac = this.ac, t = ac.currentTime, g = ac.createGain(); g.gain.value = 0; g.gain.linearRampToValueAtTime(this.muted ? 0 : level, t + 2); g.connect(this.master);
    const src = ac.createBufferSource(); src.buffer = buf; src.loop = true; src.connect(g); src.start(t);
    this._track = { id, g, src }; this._musicLevel = level;
    return true;
  }
  stopTrack() { const tk = this._track; if (!tk) return; this._track = null; const t = this.ac.currentTime; tk.g.gain.cancelScheduledValues(t); tk.g.gain.setValueAtTime(tk.g.gain.value, t); tk.g.gain.linearRampToValueAtTime(0.0001, t + 1.2); setTimeout(() => { try { tk.src.stop(); tk.g.disconnect(); } catch {} }, 1400); }
  // flight music: the combat track if present, else the procedural bed; hub: calm / deep exploration
  combatMusic(on) {
    if (on) { this._wantTrack = 'storm'; this.playTrack('storm', 0.15).then((ok) => { if (ok) this.stopMusic(); else if (this._wantTrack === 'storm' || !this._track) this.startMusic(); }); }
    else { this.stopMusic(); if (this._track && this._track.id === 'storm') this.stopTrack(); this._wantTrack = null; }
  }
  hubMusic(deep) { const id = deep ? 'dimming' : 'islands'; if (this._track && this._track.id === id) return; this.playTrack(id, 0.12); }

  // ── planet ambience ─────────────────────────────────────────────────────────────────────────
  // ambStart() when a world is near, ambSet(o) every frame (sampled internally at ~15 Hz: no nodes, only
  // setTargetAtTime on values that really moved), ambStop() when leaving. Its own bus (out -> master) sits under
  // the music. If ambSet stops arriving the bed fades by itself; after 2.5 s of silence (space, pause, mute) the
  // sources are stopped and the graph is rebuilt on demand. Before the first user gesture the graph is built on a
  // suspended context (silent until the constructor's unlock resumes it). Discrete events (calls, cracks, chimes)
  // are a few short-lived nodes, at most ~1 per second, scheduled from the 15 Hz tick.
  ambStart() {
    if (!this._ensure()) return;
    this._ambOn = true; this._ambLast = performance.now(); this._ambNext = 0;
    if (this._ambStopT) { clearTimeout(this._ambStopT); this._ambStopT = 0; if (this._amb) this._amb.lv[K.out] = NaN; }
    if (this._aPrep() && !this._amb) this._amb = this._aBuild();     // else the tick builds it once the buffers are ready
    if (!this._ambWd) this._ambWd = setInterval(() => this._ambWatch(), 400);
  }
  ambSet(o) {
    if (!this._ambOn || !o) return;
    const now = performance.now(); this._ambLast = now;
    if (now < this._ambNext && !o.paused === !this._ambPz) return;     // ~15 Hz; a pause toggle goes through at once
    this._ambNext = now + 66; this._ambPz = !!o.paused;
    this._ambTick(o);
  }
  ambStop() {
    this._ambOn = false;
    if (this._ambWd) { clearInterval(this._ambWd); this._ambWd = 0; }
    this._engScale(1);
    const a = this._amb; if (!a || this._ambStopT) return;
    a.out.gain.setTargetAtTime(0, this.ac.currentTime, 0.25); a.lv[K.out] = 0;
    this._ambStopT = setTimeout(() => { this._ambStopT = 0; if (!this._ambOn) this._aTear(); }, 1300);
  }
  ambVolume(v) { this._ambVol = Math.max(0, Math.min(2, +v || 0)); this._ambNext = 0; }   // user scale, 1 = default
  _engScale(s) { if (s === this._engAmb) return; this._engAmb = s; const e = this._eng; if (e && this.ac) e.g.gain.setTargetAtTime(this._engLvl * s, this.ac.currentTime, 0.5); }
  _ambWatch() {
    const a = this._amb, now = performance.now();
    if (now - this._ambLast > 700) {        // the renderer stopped calling ambSet (scene change, hidden tab): fade out
      this._ambLvl = 0;
      if (a && a.lv[K.out] !== 0) { a.lv[K.out] = 0; a.out.gain.setTargetAtTime(0, this.ac.currentTime, 0.3); }
    }
    if (a && this._ambLvl < 0.004) { if (!this._ambQuiet) this._ambQuiet = now; else if (now - this._ambQuiet > 2500) this._aTear(); }
    else this._ambQuiet = 0;
  }
  _aTear() {
    const a = this._amb; if (!a) return;
    this._amb = null; this._ambQuiet = 0;
    for (const s of a.srcs) { try { s.stop(); } catch {} }
    if (a.bd) this._aBedKill(a.bd);
    try { a.out.disconnect(); } catch {}
  }
  // looped noises (stereo where width matters), drops, crackle, gust modulation: synthesised once per session in
  // chunks off the frame path (true once ready); _aBufs() finishes them synchronously if a one-shot needs them first
  _aPrep() {
    if (this._ab) return true;
    if (!this._abG) { const o = (this._abO = {}); chunked((this._abG = ambBufs(this.ac, o)), () => { this._ab = o; }); }
    return false;
  }
  _aBufs() {
    if (!this._aPrep()) { const g = this._abG; while (!g.next().done); this._ab = this._abO; }
    return this._ab;
  }
  _aBuild() {
    const ac = this.ac, B = this._ab, t = ac.currentTime;
    const G = (v, dst) => { const g = ac.createGain(); g.gain.value = v; if (dst) g.connect(dst); return g; };
    const F = (type, f, q, dst) => { const n = ac.createBiquadFilter(); n.type = type; n.frequency.value = f; n.Q.value = q; try { n.frequency.automationRate = 'k-rate'; } catch {} if (dst) n.connect(dst); return n; };
    const S = (buf) => { const s = ac.createBufferSource(); s.buffer = buf; s.loop = true; return s; };
    const out = G(0, this.master), pres = G(0, out), gust = G(1, pres);
    const pk = S(B.pink), br = S(B.brown), wh = S(B.white), drops = S(B.drops), crack = S(B.crack), mod = S(B.mod);
    const a = { out, pres, gust, drops, crack, srcs: [pk, br, wh, drops, crack, mod], lv: new Float64Array(NK).fill(NaN), t,
      bd: null, bdId: null, evT: t + 1.5 + Math.random() * 2, swT: t, guT: t };
    // wind group (rush + two whistles + blown grit) -> gust gain (random gust targets + looped flutter)
    a.rushG = G(0, gust); a.rushLP = F('lowpass', 500, 0.5, a.rushG); pk.connect(a.rushLP);
    a.w1G = G(0, gust); a.w1 = F('bandpass', 900, 7, a.w1G); pk.connect(a.w1);
    a.w2G = G(0, gust); a.w2 = F('bandpass', 1320, 7, a.w2G); pk.connect(a.w2);
    a.hfl = G(1, out); a.roarG = G(0, a.hfl); a.roar = F('bandpass', 400, 0.55, a.roarG); pk.connect(a.roar);   // re-entry: driven by heat, not by k
    a.gritG = G(0, gust); a.grit = F('bandpass', 3600, 0.8, a.gritG); crack.connect(a.grit);
    a.modW = G(0.2); mod.connect(a.modW); a.modW.connect(gust.gain); a.modH = G(0); mod.connect(a.modH); a.modH.connect(a.hfl.gain);
    a.modF = G(60); mod.connect(a.modF); a.modF.connect(a.w1.frequency); a.modF.connect(a.w2.frequency);
    // the rest straight into the presence bus
    a.snowG = G(0, pres); pk.connect(F('lowpass', 650, 0.4, a.snowG));
    a.surfG = G(0, pres); a.surfE = G(0.22, a.surfG); a.surf = F('lowpass', 380, 0.4, a.surfE); pk.connect(a.surf);
    a.lowG = G(0, out); a.low = F('lowpass', 90, 0.6, a.lowG); br.connect(a.low);        // presence applied in the tick (heat part is not)
    a.hissG = G(0, pres); a.hiss = F('highpass', 3000, 0.5, a.hissG); wh.connect(a.hiss);
    a.dropG = G(0, pres); drops.connect(F('highpass', 900, 0.5, a.dropG));
    a.crackG = G(0, out); crack.connect(F('highpass', 1600, 0.6, a.crackG));
    // a cheap feedback-delay space for the discrete events (canyon / ice-field echoes), sized per biome
    a.eIn = G(1); a.dly = ac.createDelay(1); a.dly.delayTime.value = 0.25; a.eLP = F('lowpass', 2400, 0.5); a.eFb = G(0.3); a.eWet = G(0.25, pres);
    a.eIn.connect(a.dly); a.dly.connect(a.eLP); a.eLP.connect(a.eWet); a.eLP.connect(a.eFb); a.eFb.connect(a.dly);
    a.pans = [-0.75, -0.35, 0, 0.35, 0.75].map((p) => { if (!ac.createStereoPanner) return pres; const n = ac.createStereoPanner(); n.pan.value = p; n.connect(pres); return n; });
    for (const s of a.srcs) s.start(t, Math.random() * s.buffer.duration * 0.9);
    return a;
  }
  _as(a, i, p, v, tc) {   // set an AudioParam target only when it moved meaningfully (1.5 % or a hair above silence)
    const l = a.lv[i]; if (Math.abs(v - l) <= 0.015 * Math.abs(l) + 2e-5) return;
    a.lv[i] = v; p.setTargetAtTime(v, a.t, tc);
  }
  _ambTick(o) {
    const B = AMB_BIOME[o.biome] || AMB_BIOME.rocky;
    const k = cl(o.k), dens = cl(o.dens), heat = cl(o.heat), rain = cl(o.rain), snow = cl(o.snow), dust = cl(o.dust), ash = cl(o.ash);
    const storm = cl(o.storm), water = cl(o.water), night = cl(o.night), landed = !!o.landed, paused = !!o.paused;
    const spd = o.spd > 0 ? o.spd : 0, agl = landed ? 0 : o.agl >= 0 ? o.agl : o.agl < 0 ? 0 : 1e4;
    const vol = paused || this.muted ? 0 : AMB_OUT * this._ambVol, pres = Math.pow(k, 1.3) * (landed ? 1.2 : 1);
    this._ambLvl = vol * Math.max(pres, heat);
    let a = this._amb;
    if (!a) { if (this._ambLvl < 0.004 || !this._aPrep()) return; a = this._amb = this._aBuild(); }     // first build / wake from sleep
    const t = (a.t = this.ac.currentTime);
    this._as(a, K.out, a.out.gain, vol, paused ? 0.05 : 0.3);
    this._as(a, K.pres, a.pres.gain, pres, 0.5);
    const air = Math.pow(dens, 0.6), sp = Math.min(1.5, spd / 700), sp1 = sp < 1 ? sp : 1;
    const ground = cl(1 - agl / 600), reach = B.reach ? cl(1 - agl / B.reach) : air, skim = landed ? 0 : cl(1 - agl / 30) * cl(spd / 120);
    const hush = 1 - 0.35 * snow;                                   // snow muffles the bright layers
    // wind: a breeze near the ground (stronger in storms) + airflow from airspeed; whistles follow speed
    const breeze = air * (B.breeze * (0.55 + 0.45 * ground) + 0.25 * storm), flow = 0.45 * air * Math.pow(sp, 1.25), wind = breeze + flow;
    this._as(a, K.rushG, a.rushG.gain, AC_.rush * wind / (1 + 0.45 * wind) * hush, 0.25);
    this._as(a, K.rushF, a.rushLP.frequency, B.wLP * (0.6 + 0.4 * air) * (1 + 1.8 * sp + 0.6 * storm), 0.3);
    const wf = B.wF * (650 + 1500 * sp1 + 350 * storm), wl = air * (B.whis * (0.45 + 0.55 * ground + 0.4 * storm) + 0.18 * sp1 * sp1) * hush;
    const wq = AC_.whis * wl * Math.sqrt(B.wQ / 6);              // a narrower whistle passes less noise: keep its level
    this._as(a, K.w1G, a.w1G.gain, wq, 0.3);
    this._as(a, K.w1F, a.w1.frequency, wf, 0.4);
    this._as(a, K.w1Q, a.w1.Q, B.wQ, 1);
    this._as(a, K.w2G, a.w2G.gain, wq * B.w2, 0.3);
    this._as(a, K.w2F, a.w2.frequency, wf * 1.47, 0.4);
    this._as(a, K.w2Q, a.w2.Q, B.wQ * 1.2, 1);
    this._as(a, K.modW, a.modW.gain, 0.18 + 0.25 * storm, 0.5);
    this._as(a, K.modH, a.modH.gain, 0.35 * heat, 0.5);
    this._as(a, K.modF, a.modF.gain, wf * (0.08 + 0.1 * storm), 0.5);
    // re-entry (bypasses presence: heat itself says how loud): plasma roar with its own flutter + crackle + the low rumble below
    this._as(a, K.roarG, a.roarG.gain, AC_.roar * Math.pow(heat, 1.4), 0.2);
    this._as(a, K.roarF, a.roar.frequency, 280 + 700 * heat + 300 * sp1, 0.3);
    this._as(a, K.crackG, a.crackG.gain, AC_.crack * heat * heat, 0.2);
    this._as(a, K.crackR, a.crack.playbackRate, 0.7 + 0.8 * heat, 0.4);
    // low: ground rush when skimming fast, biome rumble (lava, gas-giant roar), storm body, re-entry rumble
    const low = pres * (0.9 * skim * dens + B.rum * (0.4 + 0.6 * reach) * air + 0.25 * storm * air + 0.2 * flow) + 0.9 * heat;
    this._as(a, K.lowG, a.lowG.gain, AC_.low * low, 0.25);
    this._as(a, K.lowF, a.low.frequency, 55 + B.lowF + 140 * heat + 120 * skim + 40 * sp1, 0.3);
    // weather: rain hiss + drops (louder on the ground, faster with speed), snow hush, dust / ash grit (blown by the wind)
    const hissR = rain * (0.3 + 0.5 * ground + 0.4 * sp1), hissD = (dust + 0.8 * ash) * (0.15 + 0.5 * (wind < 1 ? wind : 1)), hissV = (B.hiss || 0) * reach;   // V: far vents
    this._as(a, K.hissG, a.hissG.gain, AC_.hiss * (hissR + hissD + hissV) * hush, 0.3);
    this._as(a, K.hissF, a.hiss.frequency, (hissR * 2800 + hissD * (dust >= ash ? 4200 : 2300) + hissV * 2500) / (hissR + hissD + hissV + 1e-6) || 3000, 0.5);
    this._as(a, K.dropG, a.dropG.gain, AC_.drop * rain * (0.2 + 0.8 * ground), 0.3);
    this._as(a, K.dropR, a.drops.playbackRate, 0.8 + 0.5 * sp1 + 0.3 * rain, 0.5);
    this._as(a, K.snowG, a.snowG.gain, AC_.snow * snow * (0.4 + 0.6 * ground) * (0.5 + 0.5 * (wind < 1 ? wind : 1)), 0.4);
    this._as(a, K.gritG, a.gritG.gain, AC_.grit * (dust + 0.7 * ash) * (0.25 + 0.75 * (wind < 1 ? wind : 1)), 0.3);
    this._as(a, K.gritF, a.grit.frequency, 3800 - 1600 * (ash / (dust + ash + 1e-6)), 0.5);
    // open water: surf bed (its swell envelope and the breaking waves are scheduled below)
    const surf = water * (landed ? 1 : 0.12 + 0.88 * cl(1 - agl / 500)) * air;
    this._as(a, K.surfG, a.surfG.gain, AC_.surf * surf, 0.6);
    this._as(a, K.eWet, a.eWet.gain, B.echo, 1.5); this._as(a, K.dly, a.dly.delayTime, B.dly, 1.5); this._as(a, K.eFb, a.eFb.gain, B.fb, 1.5);
    // biome bed: crossfade on change; a new bed starts once its loop is baked (a few chunks off the frame path)
    const want = B.bed;
    if (want !== a.bdId) { if (a.bd) this._aBedFade(a.bd, t); a.bd = null; a.bdId = want; }
    if (want && !a.bd) {
      const ids = BED_IDS[want], b0 = this._aBake(ids[0]), b1 = ids.length > 1 ? this._aBake(ids[1]) : null;
      if (b0 && (ids.length < 2 || b1)) a.bd = this._aBedMake(a, b0, b1);
    }
    if (a.bd) {
      this._as(a, K.bdG, a.bd.sum.gain, AC_.bed * B.bedLvl * reach * (1 - 0.5 * (rain > storm ? rain : storm)) * (1 - 0.4 * sp1), 0.8);
      if (a.bd.v1) { this._as(a, K.bd0, a.bd.v0.g.gain, Math.sqrt(1 - night), 1.5); this._as(a, K.bd1, a.bd.v1.g.gain, Math.sqrt(night), 1.5); }
    }
    // gusts: a new random target every 0.5-2 s (faster and bigger in storms)
    if (t >= a.guT) {
      const big = Math.random() < storm * 0.25, g = big ? 1.3 + 0.3 * storm : 1 + (Math.random() * 2 - 1) * (B.gust + 0.3 * storm);
      a.gust.gain.setTargetAtTime(g > 0.15 ? g : 0.15, t, big ? 0.5 : 0.3 + Math.random() * 0.6);
      a.guT = t + (0.5 + Math.random() * 1.5) * (1 - 0.45 * storm);
    }
    // discrete events, only where they can be heard
    const ev = reach * (1 - 0.6 * sp1) * (1 - 0.5 * storm) * (1 - 0.4 * rain), evOk = this._ambLvl > 0.02 && heat < 0.25 && !paused && !this.muted;
    if (surf > 0.02 && t >= a.swT) {            // surf: a swell rises and draws back; some break into a crash
      const rise = 1 + Math.random() * 0.8;
      a.surfE.gain.setTargetAtTime(0.55 + Math.random() * 0.45, t, rise * 0.45); a.surfE.gain.setTargetAtTime(0.16 + Math.random() * 0.1, t + rise, 1.1 + Math.random() * 0.8);
      a.surf.frequency.setTargetAtTime(700 + Math.random() * 900, t, rise * 0.5); a.surf.frequency.setTargetAtTime(330, t + rise, 1.3);
      if (evOk && Math.random() < 0.45 * water) this._aCrash(a, t + rise * 0.8, AC_.surf * 0.3 * surf);
      a.swT = t + 4 + Math.random() * 5;
    }
    if (evOk && ev > 0.1 && t >= a.evT) a.evT = t + this._aEv(a, B.id, night, ev, t);
    this._engScale(landed ? 0.45 : 1);            // landed: the engine settles, the world comes forward
  }
  _aBake(id) {   // the baked loop for a bed, or null while it is being synthesised (kicked off on the first ask)
    const c = this._abk || (this._abk = {}), v = c[id];
    if (v) return v === 1 ? null : v;
    c[id] = 1;
    const buf = this.ac.createBuffer(2, BN, BR), L = buf.getChannelData(0), R = buf.getChannelData(1);
    chunked(BAKE[id](L, R, rng(SEED[id])), () => {      // normalise to RMS 0.12 (peak capped at 0.95): beds sit level
      let pk = 0, e = 0; for (let i = 0; i < BN; i++) { const l = L[i], r = R[i]; e += l * l + r * r; const m = Math.max(Math.abs(l), Math.abs(r)); if (m > pk) pk = m; }
      const s = Math.min(0.12 / (Math.sqrt(e / (2 * BN)) || 1), 0.95 / (pk || 1)); for (let i = 0; i < BN; i++) { L[i] *= s; R[i] *= s; }
      c[id] = buf;
    });
    return null;
  }
  _aBedMake(a, b0, b1) {
    const ac = this.ac, t = a.t, sum = ac.createGain(); sum.gain.value = 0; sum.connect(a.pres);
    const mk = (b) => { const s = ac.createBufferSource(), g = ac.createGain(); s.buffer = b; s.loop = true; s.connect(g); g.connect(sum); s.start(t, Math.random() * BL); return { s, g }; };
    a.lv[K.bdG] = a.lv[K.bd0] = a.lv[K.bd1] = NaN;
    return { sum, v0: mk(b0), v1: b1 ? mk(b1) : null };
  }
  _aBedFade(bd, t) { bd.sum.gain.setTargetAtTime(0, t, 0.6); setTimeout(() => this._aBedKill(bd), 3500); }
  _aBedKill(bd) { try { bd.v0.s.stop(); } catch {} if (bd.v1) { try { bd.v1.s.stop(); } catch {} } try { bd.sum.disconnect(); } catch {} }

  // ── ambience events (a few short-lived nodes each, scheduled from the 15 Hz tick) ─────────────────
  _aV(a, type, echo) {          // oscillator -> gain -> a random pan bus (+ echo send)
    const ac = this.ac, o = ac.createOscillator(), g = ac.createGain(); o.type = type; g.gain.value = 0;
    o.connect(g); g.connect(a.pans[(Math.random() * 5) | 0]); if (echo) g.connect(a.eIn);
    return { o, g };
  }
  _aN(a, buf, type, f, q, echo) {   // looped noise slice -> filter -> gain -> a random pan bus (+ echo send)
    const ac = this.ac, s = ac.createBufferSource(), fl = ac.createBiquadFilter(), g = ac.createGain();
    s.buffer = buf; s.loop = true; fl.type = type; fl.frequency.value = f; fl.Q.value = q; g.gain.value = 0;
    s.connect(fl); fl.connect(g); g.connect(a.pans[(Math.random() * 5) | 0]); if (echo) g.connect(a.eIn);
    return { s, f: fl, g };
  }
  _aEv(a, id, night, v, t) {    // fire one biome event; returns the seconds until the next
    const R = Math.random, r = R();
    t += 0.03 + R() * 0.2;
    switch (id) {
      case 'jungle':
        if (R() < night) { if (r < 0.65) this._aFrog(a, t, v * 0.3); else this._aPeep(a, t, v * 0.06); return 1 + R() * 3; }
        this._aBird(a, t, v * 0.05, false); return 1.8 + R() * 4.5;
      case 'ocean':
        if (R() < night) { if (r < 0.35) this._aWhale(a, t, v * 0.12); return 7 + R() * 9; }
        if (r < 0.45) this._aBird(a, t, v * 0.05, true); return 5 + R() * 8;
      case 'ice':
        if (r < 0.4) this._aCrack(a, t, v * 0.45); else if (r < 0.72) this._aCreak(a, t, v * 0.32); else this._aPew(a, t, v * 0.16);
        return 2.5 + R() * 4.5;
      case 'volcanic':
        if (r < 0.6) this._aPops(a, t, v * 0.25); else if (r < 0.88) this._aVent(a, t, v * 0.1); else this._aRumble(a, t, v * 0.3, 120, 4.5);
        return 1.2 + R() * 2.8;
      case 'gas':
        if (r < 0.55) this._aGroan(a, t, v * 0.32); else this._aRumble(a, t, v * 0.55, 80, 5.5);
        return 6 + R() * 8;
      case 'crystal':
        this._aChime(a, t, v * 0.15); if (r < 0.25) this._aChime(a, t + 0.3 + R() * 0.6, v * 0.1);
        return (2.5 + R() * 3.5) * (1 - 0.5 * night);            // the spires ring more at night
      case 'desert':
        if (r < 0.65) this._aTicks(a, t, v * 0.3, 1 + ((R() * 3) | 0), 0.2); else if (r < 0.82) this._aDune(a, t, v * 0.4);
        return 3 + R() * 5;
      default:                      // rocky: pebbles skitter (more at night, as the rock cools); rarely a rockfall
        if (r < 0.86) this._aTicks(a, t, v * 0.35, 2 + ((R() * 5) | 0), 0.5);
        else { this._aRumble(a, t, v * 0.35, 260, 2.6); this._aTicks(a, t + 0.15, v * 0.3, 10 + ((R() * 10) | 0), 0.5); }
        return (2 + R() * 4) * (1 - 0.4 * night);
    }
  }
  _aBird(a, t, v, gull) {       // alien bird call: one oscillator, notes by automation, sometimes an FM warble; gull = falling cries
    const R = Math.random, { o, g } = this._aV(a, gull ? 'triangle' : 'sine', true), style = (R() * 4) | 0;
    let base = gull ? 900 + R() * 700 : 1500 + R() * 2400, tt = t;
    for (let i = 0, n = gull ? 2 + ((R() * 2) | 0) : 2 + ((R() * 6) | 0); i < n; i++) {
      const d = gull ? 0.16 + R() * 0.16 : style === 1 ? 0.035 + R() * 0.03 : 0.05 + R() * 0.15, f1 = base * (0.85 + R() * 0.3);
      const f2 = gull ? f1 * (0.6 + R() * 0.12) : style === 0 ? f1 * (1.3 + R() * 0.5) : style === 1 ? f1 * (0.9 + R() * 0.2) : style === 2 ? f1 * 0.55 : f1 * (i % 2 ? 1.6 : 0.7);
      o.frequency.setValueAtTime(f1, tt); o.frequency.exponentialRampToValueAtTime(f2, tt + d);
      g.gain.setValueAtTime(0, tt); g.gain.linearRampToValueAtTime(v * (0.6 + R() * 0.4), tt + Math.min(0.015, d * 0.3)); g.gain.linearRampToValueAtTime(0, tt + d);
      tt += d + (style === 1 && !gull ? 0.02 : 0.04 + R() * 0.12);
      if (style !== 1) base *= 0.9 + R() * 0.22;
    }
    o.start(t); o.stop(tt + 0.05);
    if (!gull && R() < 0.45) {
      const m = this.ac.createOscillator(), mg = this.ac.createGain(); m.frequency.value = 25 + R() * 70; mg.gain.value = base * (0.03 + R() * 0.08);
      m.connect(mg); mg.connect(o.frequency); m.start(t); m.stop(tt + 0.05);
    }
  }
  _aFrog(a, t, v) {             // throaty pulsed croaks: a buzzy source through a formant, gated by pulses
    const ac = this.ac, R = Math.random, o = ac.createOscillator(), f = ac.createBiquadFilter(), g = ac.createGain();
    o.type = 'sawtooth'; f.type = 'bandpass'; f.frequency.value = 450 + R() * 650; f.Q.value = 3.5; g.gain.value = 0;
    o.connect(f); f.connect(g); g.connect(a.pans[(R() * 5) | 0]);
    const f0 = 110 + R() * 190, pr = 18 + R() * 22, np = 4 + ((R() * 7) | 0), d = np / pr; let tt = t;
    for (let c = 1 + ((R() * 3) | 0); c-- > 0;) {
      o.frequency.setValueAtTime(f0 * 1.08, tt); o.frequency.linearRampToValueAtTime(f0 * 0.9, tt + d);
      for (let p = 0; p < np; p++) { const pt = tt + p / pr; g.gain.setValueAtTime(0, pt); g.gain.linearRampToValueAtTime(v * (0.6 + 0.4 * R()), pt + 0.006); g.gain.linearRampToValueAtTime(0, pt + 0.65 / pr); }
      tt += d + 0.12 + R() * 0.3;
    }
    o.start(t); o.stop(tt + 0.05);
  }
  _aPeep(a, t, v) {             // tree-frog peeps
    const R = Math.random, { o, g } = this._aV(a, 'sine', false), f = 2100 + R() * 900; let tt = t;
    for (let n = 2 + ((R() * 4) | 0); n-- > 0;) {
      o.frequency.setValueAtTime(f, tt); o.frequency.exponentialRampToValueAtTime(f * 1.3, tt + 0.06);
      g.gain.setValueAtTime(0, tt); g.gain.linearRampToValueAtTime(v, tt + 0.02); g.gain.linearRampToValueAtTime(0, tt + 0.06);
      tt += 0.28 + R() * 0.2;
    }
    o.start(t); o.stop(tt);
  }
  _aCrash(a, t, v) {            // a swell breaks: a long roar of foam, darkening as it spreads
    const R = Math.random, n = this._aN(a, this._ab.white, 'lowpass', 3000, 0.3, false), pk = Math.max(1e-4, v);
    n.f.frequency.setValueAtTime(3200 + R() * 1200, t); n.f.frequency.exponentialRampToValueAtTime(450, t + 3);
    n.g.gain.setValueAtTime(1e-4, t); n.g.gain.exponentialRampToValueAtTime(pk, t + 0.3 + R() * 0.2); n.g.gain.setTargetAtTime(0, t + 0.6, 0.8);
    n.s.start(t, R() * 3); n.s.stop(t + 4.5);
  }
  _aWhale(a, t, v) {            // a far, slow, gliding moan over the night sea
    const ac = this.ac, R = Math.random, { o, g } = this._aV(a, 'triangle', true), l = ac.createOscillator(), lg = ac.createGain(), f = 110 + R() * 130, d = 2.5 + R() * 2;
    o.frequency.setValueAtTime(f, t); o.frequency.exponentialRampToValueAtTime(f * (1.25 + R() * 0.3), t + d * 0.45); o.frequency.exponentialRampToValueAtTime(f * (0.65 + R() * 0.15), t + d);
    l.frequency.value = 3.5 + R() * 2.5; lg.gain.value = f * 0.025; l.connect(lg); lg.connect(o.frequency);
    g.gain.setValueAtTime(0, t); g.gain.linearRampToValueAtTime(v, t + 0.8); g.gain.setValueAtTime(v, t + d - 0.9); g.gain.linearRampToValueAtTime(0, t + d);
    o.start(t); l.start(t); o.stop(t + d + 0.05); l.stop(t + d + 0.05);
  }
  _aPops(a, t, v) {             // lava bubbles bursting: bloop, bloop-bloop
    const R = Math.random, { o, g } = this._aV(a, 'sine', false); let tt = t;
    for (let n = 1 + ((R() * 4) | 0); n-- > 0;) {
      const f0 = 60 + R() * 180, d = 0.06 + R() * 0.14;
      o.frequency.setValueAtTime(f0, tt); o.frequency.exponentialRampToValueAtTime(f0 * (2 + R() * 2.5), tt + d);
      g.gain.setValueAtTime(0, tt); g.gain.linearRampToValueAtTime(v * (0.5 + 0.5 * R()), tt + 0.006); g.gain.setTargetAtTime(0, tt + 0.008, d * 0.35);
      tt += d + 0.04 + R() * 0.3;
    }
    o.start(t); o.stop(tt + 0.1);
  }
  _aVent(a, t, v) {             // a steam vent breathes out
    const R = Math.random, f = 2400 + R() * 2600, n = this._aN(a, this._ab.white, 'bandpass', f, 0.9, true), d = 2.5 + R() * 2.5;
    n.g.gain.setValueAtTime(0, t); n.g.gain.linearRampToValueAtTime(v, t + 0.4 + R() * 0.6); n.g.gain.setTargetAtTime(v * 0.6, t + 1.2, 0.5); n.g.gain.setTargetAtTime(0, t + d - 1, 0.45);
    n.f.frequency.setValueAtTime(f, t); n.f.frequency.linearRampToValueAtTime(f * 0.7, t + d);
    n.s.start(t, R() * 3); n.s.stop(t + d + 1);
  }
  _aRumble(a, t, v, f, dur) {   // distant rockfall / eruption / pressure boom: rolling brown-noise bumps
    const R = Math.random, n = this._aN(a, this._ab.brown, 'lowpass', f, 0.7, true), pk = Math.max(1e-4, v);
    n.g.gain.setValueAtTime(1e-4, t); n.g.gain.exponentialRampToValueAtTime(pk, t + 0.25 + R() * 0.3);
    for (let i = 1; i <= 3; i++) n.g.gain.setTargetAtTime(pk * (0.3 + 0.6 * R()), t + i * dur * 0.2, dur * 0.07);
    n.g.gain.setTargetAtTime(0, t + dur * 0.75, dur * 0.15);
    n.s.start(t, R() * 5); n.s.stop(t + dur * 1.5);
  }
  _aCrack(a, t, v) {            // a glassy crack across the ice: a sharp snap + ringing pings in the echo
    const R = Math.random, n = this._aN(a, this._ab.white, 'bandpass', 2000 + R() * 3000, 0.8, true), f = 2600 + R() * 3600;
    n.g.gain.setValueAtTime(v, t); n.g.gain.setTargetAtTime(0, t + 0.002, 0.012); n.s.start(t, R() * 3); n.s.stop(t + 0.2);
    for (let i = 0; i < 2; i++) {
      const r = i ? 1.53 : 1, { o, g } = this._aV(a, 'sine', true);
      o.frequency.setValueAtTime(f * r, t); o.frequency.exponentialRampToValueAtTime(f * r * 0.97, t + 1);
      g.gain.setValueAtTime(0, t); g.gain.linearRampToValueAtTime(v * 0.3 / r, t + 0.003); g.gain.setTargetAtTime(0, t + 0.005, 0.12 + R() * 0.2);
      o.start(t); o.stop(t + 1.4);
    }
  }
  _aCreak(a, t, v) {            // stick-slip creak of stressed ice
    const ac = this.ac, R = Math.random, o = ac.createOscillator(), f = ac.createBiquadFilter(), g = ac.createGain(), f0 = 55 + R() * 70;
    o.type = 'sawtooth'; o.frequency.setValueAtTime(f0, t); f.type = 'bandpass'; f.frequency.value = 450 + R() * 600; f.Q.value = 5; g.gain.value = 0;
    o.connect(f); f.connect(g); g.connect(a.pans[(R() * 5) | 0]); g.connect(a.eIn);
    let tt = t, dt = 0.02 + R() * 0.04;
    for (let i = 8 + ((R() * 18) | 0); i-- > 0;) { g.gain.setValueAtTime(v * (0.35 + 0.65 * R()), tt); g.gain.setTargetAtTime(0, tt + 0.002, 0.01); tt += dt * (0.6 + 0.8 * R()); dt *= 0.97; }
    o.frequency.linearRampToValueAtTime(f0 * (0.8 + R() * 0.45), tt);
    o.start(t); o.stop(tt + 0.08);
  }
  _aPew(a, t, v) {              // the ice sheet sings: dispersive descending "pew" chirps
    const R = Math.random, { o, g } = this._aV(a, 'sine', true); let tt = t;
    for (let n = 1 + ((R() * 3) | 0); n-- > 0;) {
      const d = 0.25 + R() * 0.35;
      o.frequency.setValueAtTime(2400 + R() * 2600, tt); o.frequency.exponentialRampToValueAtTime(140 + R() * 200, tt + d);
      g.gain.setValueAtTime(0, tt); g.gain.linearRampToValueAtTime(v, tt + 0.004); g.gain.setTargetAtTime(0, tt + 0.01, d * 0.35);
      tt += d + 0.12 + R() * 0.5;
    }
    o.start(t); o.stop(tt + 0.1);
  }
  _aTicks(a, t, v, n, spread) { // pebbles skittering: a bouncing cluster of ticks (one noise voice, gain spikes)
    const R = Math.random, s = this._aN(a, this._ab.white, 'bandpass', 2200 + R() * 2800, 1.6, true);
    let tt = t, dt = 0.05 + R() * 0.12, pk = v;
    for (let i = 0; i < n; i++) {
      s.g.gain.setValueAtTime(pk, tt); s.g.gain.setTargetAtTime(0, tt + 0.001, 0.003 + R() * 0.004);
      tt += dt; dt *= 0.6 + R() * 0.2; pk *= 0.6 + R() * 0.3;
      if (dt < 0.012 || R() < 0.2) { tt += R() * spread; dt = 0.04 + R() * 0.1; pk = v * (0.3 + 0.7 * R()); }
    }
    s.s.start(t, R() * 3); s.s.stop(tt + 0.1);
  }
  _aDune(a, t, v) {             // booming dune: a low sand hum swelling and wavering
    const ac = this.ac, R = Math.random, o = ac.createOscillator(), f = ac.createBiquadFilter(), g = ac.createGain(), f0 = 80 + R() * 28;
    o.type = 'triangle'; o.frequency.setValueAtTime(f0, t); o.frequency.linearRampToValueAtTime(f0 * 1.05, t + 4.5); f.type = 'lowpass'; f.frequency.value = 280; g.gain.value = 0;
    o.connect(f); f.connect(g); g.connect(a.pans[(R() * 5) | 0]); g.connect(a.eIn);
    g.gain.setValueAtTime(0, t); g.gain.linearRampToValueAtTime(v * 0.6, t + 1.4);
    for (let i = 0; i < 6; i++) g.gain.setTargetAtTime(v * (0.4 + 0.6 * R()), t + 1.4 + i * 0.5, 0.18);
    g.gain.setTargetAtTime(0, t + 4.4, 0.6);
    o.start(t); o.stop(t + 7.5);
  }
  _aChime(a, t, v) {            // a crystal spire rings: glassy inharmonic partials into the echo
    const ac = this.ac, R = Math.random, f = CHIME[(R() * CHIME.length) | 0] * (R() < 0.3 ? 2 : 1), p = a.pans[(R() * 5) | 0];
    for (let i = 0; i < 3; i++) {
      const o = ac.createOscillator(), g = ac.createGain(), r = i === 0 ? 1 : i === 1 ? 2.32 : 4.25, tc = i === 0 ? 0.75 : i === 1 ? 0.45 : 0.25;
      o.frequency.value = f * r; g.gain.value = 0; o.connect(g); g.connect(p); g.connect(a.eIn);
      g.gain.setValueAtTime(0, t); g.gain.linearRampToValueAtTime(v / (1 + i * 1.3), t + 0.004); g.gain.setTargetAtTime(0, t + 0.006, tc);
      o.start(t); o.stop(t + tc * 6);
    }
  }
  _aGroan(a, t, v) {            // the gas giant's depths groan: a resonant low sweep
    const ac = this.ac, R = Math.random, o = ac.createOscillator(), f = ac.createBiquadFilter(), g = ac.createGain(), f0 = 34 + R() * 22, d = 4 + R() * 2.5;
    o.type = 'sawtooth'; o.frequency.setValueAtTime(f0, t); o.frequency.exponentialRampToValueAtTime(f0 * (0.8 + R() * 0.45), t + d);
    f.type = 'lowpass'; f.Q.value = 7; f.frequency.setValueAtTime(90, t); f.frequency.exponentialRampToValueAtTime(220 + R() * 160, t + d * 0.45); f.frequency.exponentialRampToValueAtTime(100, t + d);
    g.gain.value = 0; o.connect(f); f.connect(g); g.connect(a.pans[(R() * 5) | 0]); g.connect(a.eIn);
    g.gain.setValueAtTime(0, t); g.gain.linearRampToValueAtTime(v, t + d * 0.35); g.gain.linearRampToValueAtTime(0, t + d);
    o.start(t); o.stop(t + d + 0.05);
  }
  // Thunder `dist` metres away: arrives dist/343 s later. Near = a multi-snap crack, the rip and a sub thump, then the
  // boom; far = only a low, slow, long rumble (the boom's lowpass closes with distance). Works with or without the bed.
  thunder(dist = 1500) {
    if (!this._ok() || this._ambPz) return;
    const ac = this.ac, B = this._aBufs(), a = this._amb, R = Math.random, dst = a ? a.out : this.master, sc = a ? 1 / AMB_OUT : this._ambVol;   // levels in master units
    const d = Math.min(20000, Math.max(0, +dist || 0)), t = ac.currentTime + d / 343 + 0.02, v = sc * 0.4 / (1 + Math.pow(d / 600, 1.1)), near = cl(1 - d / 1500), far = 1 - near;
    const N = (buf, type, f, q) => { const s = ac.createBufferSource(), fl = ac.createBiquadFilter(), g = ac.createGain(); s.buffer = buf; s.loop = true; fl.type = type; fl.frequency.value = f; fl.Q.value = q; g.gain.value = 0; s.connect(fl); fl.connect(g); g.connect(dst); return { s, g }; };
    if (near > 0) {
      const c = N(B.white, 'bandpass', 600 + 2400 * near, 0.5), pk = v * 3 * near; let tt = t;
      c.g.gain.setValueAtTime(0, t); c.g.gain.linearRampToValueAtTime(pk, t + 0.003); c.g.gain.setTargetAtTime(pk * 0.2, t + 0.004, 0.025);
      for (let i = 3 + ((R() * 4) | 0); i-- > 0;) { tt += 0.025 + R() * 0.07; c.g.gain.setValueAtTime(pk * (0.25 + 0.6 * R()), tt); c.g.gain.setTargetAtTime(pk * 0.1, tt + 0.002, 0.03); }
      c.g.gain.setTargetAtTime(0, tt + 0.01, 0.15); c.s.start(t, R() * 3); c.s.stop(tt + 1.2);
      const r = N(B.crack, 'bandpass', 500, 0.6); r.s.playbackRate.value = 0.6;
      r.g.gain.setValueAtTime(0, t); r.g.gain.linearRampToValueAtTime(v * 1.2 * near, t + 0.02); r.g.gain.setTargetAtTime(0, t + 0.1, 0.35); r.s.start(t, R() * 2); r.s.stop(t + 2);
      const o = ac.createOscillator(), og = ac.createGain(); o.frequency.setValueAtTime(52, t); o.frequency.exponentialRampToValueAtTime(28, t + 0.8);
      og.gain.setValueAtTime(0, t); og.gain.linearRampToValueAtTime(v * 0.6 * near, t + 0.02); og.gain.setTargetAtTime(0, t + 0.08, 0.25); o.connect(og); og.connect(dst); o.start(t); o.stop(t + 1.6);
      if (d < 700) setTimeout(() => this._duck(0.55, 1.8), (t - ac.currentTime) * 1000);
    }
    const b = N(B.brown, 'lowpass', 110 + 1100 * Math.exp(-d / 900), 0.6), L0 = Math.max(1e-4, v * (0.9 + 0.1 * far));
    let tt = t + 0.04 + 0.15 * far + 0.5 * far * R();
    b.g.gain.setValueAtTime(1e-4, t); b.g.gain.exponentialRampToValueAtTime(L0, tt);
    for (let i = 0, n = 4 + ((R() * 4) | 0); i < n; i++) { tt += 0.25 + R() * 0.55 * (1 + far); b.g.gain.setTargetAtTime(L0 * (0.3 + 0.7 * R()) * (1 - i / (n + 1)), tt, 0.1 + 0.25 * far); }
    b.g.gain.setTargetAtTime(0, tt + 0.2, 0.5 + 0.9 * far); b.s.start(t, R() * 5); b.s.stop(tt + 6);
    if (a) b.g.connect(a.eIn);
  }
  lightningCrackle() {          // a close strike: a sharp electric crackle + a short buzzing zap (no delay)
    if (!this._ok() || this._ambPz) return;
    const ac = this.ac, B = this._aBufs(), a = this._amb, dst = a ? a.out : this.master, sc = a ? 1 / AMB_OUT : this._ambVol, t = ac.currentTime;
    const s = ac.createBufferSource(), f = ac.createBiquadFilter(), g = ac.createGain();
    s.buffer = B.crack; s.loop = true; s.playbackRate.value = 1.8; f.type = 'highpass'; f.frequency.value = 2200; g.gain.value = 0;
    s.connect(f); f.connect(g); g.connect(dst);
    g.gain.setValueAtTime(0, t); g.gain.linearRampToValueAtTime(0.25 * sc, t + 0.004); g.gain.setTargetAtTime(0.08 * sc, t + 0.02, 0.05); g.gain.setTargetAtTime(0, t + 0.15, 0.08);
    s.start(t, Math.random() * 2); s.stop(t + 0.7);
    const o = ac.createOscillator(), bf = ac.createBiquadFilter(), og = ac.createGain();
    o.type = 'sawtooth'; o.frequency.value = 95 + Math.random() * 40; bf.type = 'bandpass'; bf.frequency.value = 1900; bf.Q.value = 2.5; og.gain.value = 0;
    o.connect(bf); bf.connect(og); og.connect(dst);
    for (let i = 0; i < 5; i++) { const tt = t + i * 0.025; og.gain.setValueAtTime(0.06 * sc * (1 - i / 6), tt); og.gain.setValueAtTime(0, tt + 0.015); }
    o.start(t); o.stop(t + 0.16);
  }
}

// One shared instance for the whole game (renderer + UI import the same module singleton).
export const sfx = new SFX();

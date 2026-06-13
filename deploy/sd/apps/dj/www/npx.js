// npx.js — NucleoOS .npx sidecar loader + feature derivation (browser/Node).
//
// Reads the binary .npx (same format the firmware + tools/npx_gen produce) and
// DERIVES the higher-level metadata the mix planner needs — the same fields
// Radio Index computes server-side (bpm, bands, key/camelot, envelope shape,
// beat grid, drop, sections), but reconstructed from the per-frame data that is
// already baked into the sidecar. Zero network beyond the one fetch.
//
// Binary layout (little-endian) — see nucleo_npx.h:
//   Header 36 B: magic[4] ver fps ch tonic scale pad[3] bpm(f) dur(f)
//                frames(u32) sr(u32) peak_rms(f) lufs(f)
//   Frame  12 B: rms(u16) bass(u16) mid(u16) high(u16) beat(u8) cue(u8)
//                brightness(u8) pad(u8)
//   beat byte: 0 = no kick; 1..255 = kick start at (beat-1)/254 into the frame.
//   cue  byte: 1 = drop.

const TONICS = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
// pitch-class + mode -> Camelot wheel code (Mixed In Key), for harmonic mixing.
const CAMELOT = {
  '0maj': '8B', '1maj': '3B', '2maj': '10B', '3maj': '5B', '4maj': '12B',
  '5maj': '7B', '6maj': '2B', '7maj': '9B', '8maj': '4B', '9maj': '11B',
  '10maj': '6B', '11maj': '1B',
  '0min': '5A', '1min': '12A', '2min': '7A', '3min': '2A', '4min': '9A',
  '5min': '4A', '6min': '11A', '7min': '6A', '8min': '1A', '9min': '8A',
  '10min': '3A', '11min': '10A',
};

export function camelotOf(tonic, scale) {
  const mode = scale === 1 ? 'min' : 'maj';
  return CAMELOT[`${tonic}${mode}`] || null;
}

// Harmonic compatibility between two Camelot codes (same as Radio Index):
//   2 identical · 1 compatible (±1 same letter / same number diff letter)
//   0 unknown · -1 dissonant clash
export function camelotCompat(ka, kb) {
  if (!ka || !kb) return 0;
  const na = parseInt(ka, 10), la = ka.slice(-1).toUpperCase();
  const nb = parseInt(kb, 10), lb = kb.slice(-1).toUpperCase();
  if (!Number.isFinite(na) || !Number.isFinite(nb)) return 0;
  if (na === nb && la === lb) return 2;
  if (na === nb && la !== lb) return 1;
  const diff = ((na - nb) % 12 + 12) % 12;
  if (la === lb && (diff === 1 || diff === 11)) return 1;
  return -1;
}

// Parse a .npx ArrayBuffer into a rich track-analysis object.
export function parseNpx(buf) {
  const dv = new DataView(buf);
  if (dv.byteLength < 36) throw new Error('npx too small');
  if (dv.getUint8(0) !== 0x4e || dv.getUint8(1) !== 0x50 ||
      dv.getUint8(2) !== 0x58 || dv.getUint8(3) !== 0x31) {
    throw new Error('bad npx magic');
  }
  const fps = dv.getUint8(5) || 10;
  const channels = dv.getUint8(6);
  const tonic = dv.getUint8(7);
  const scale = dv.getUint8(8);
  const bpmHdr = dv.getFloat32(12, true);
  const duration = dv.getFloat32(16, true);
  const frameCount = dv.getUint32(20, true);
  const sampleRate = dv.getUint32(24, true);
  const lufs = dv.getFloat32(32, true);

  const n = Math.min(frameCount, Math.floor((dv.byteLength - 36) / 12));
  const rms = new Float32Array(n), bass = new Float32Array(n);
  const mid = new Float32Array(n), high = new Float32Array(n);
  const bright = new Uint8Array(n);
  const kicks = [];              // precise kick start times (s), sub-frame decoded
  let dropS = -1;
  for (let i = 0; i < n; i++) {
    const o = 36 + i * 12;
    rms[i] = dv.getUint16(o, true) / 65535;
    bass[i] = dv.getUint16(o + 2, true) / 65535;
    mid[i] = dv.getUint16(o + 4, true) / 65535;
    high[i] = dv.getUint16(o + 6, true) / 65535;
    const beat = dv.getUint8(o + 8);
    const cue = dv.getUint8(o + 9);
    bright[i] = dv.getUint8(o + 10);
    if (beat) kicks.push((i + (beat - 1) / 254) / fps);
    if (cue && dropS < 0) dropS = i / fps;
  }

  return finalize({
    fps, channels, sampleRate, lufs,
    duration: duration || n / fps,
    bpmHdr, tonic, scale,
    camelot: camelotOf(tonic, scale),
    rms, bass, mid, high, bright, kicks, dropS,
    frameCount: n,
  });
}

// Derive beat grid, envelope shape, mean bands & sections from the raw frames.
function finalize(t) {
  // --- beat grid from the precise kick list (period + phase) ---
  let bpm = t.bpmHdr, period = bpm > 0 ? 60 / bpm : 0, phase = 0, conf = 0;
  if (t.kicks.length >= 4) {
    const d = [];
    for (let i = 1; i < t.kicks.length; i++) d.push(t.kicks[i] - t.kicks[i - 1]);
    d.sort((a, b) => a - b);
    const med = d[d.length >> 1];
    if (med > 0.1 && med < 1.5) {
      period = med;
      bpm = Math.round((60 / med) * 10) / 10;
      phase = ((t.kicks[0] % period) + period) % period;
      // confidence = how regular the spacing is (low CV -> high conf). Use only
      // intervals near the median: a breakdown leaves one big gap that would
      // otherwise blow up the variance and falsely report an unsteady beat.
      const norm = d.filter((x) => x > med * 0.5 && x < med * 1.8);
      const arr = norm.length ? norm : d;
      const mean = arr.reduce((a, b) => a + b, 0) / arr.length;
      const varr = arr.reduce((a, b) => a + (b - mean) ** 2, 0) / arr.length;
      const cv = mean > 0 ? Math.sqrt(varr) / mean : 1;
      conf = Math.max(0, Math.min(1, 1 - cv * 2));
    }
  }
  t.bpm = bpm;
  t.beat = { period_s: period, phase_s: phase, conf };
  t.bars = period > 0
    ? { bar4_period_s: period * 4, phrase16_period_s: period * 16,
        bar4_phase_s: phase % (period * 4),
        phrase16_phase_s: phase % (period * 16) }
    : { bar4_period_s: 0, phrase16_period_s: 0, bar4_phase_s: 0, phrase16_phase_s: 0 };

  // --- mean band split (bass/mid/high fractions, ~sum 1) like Radio Index ---
  const sum = (a) => a.reduce((x, y) => x + y, 0);
  const sb = sum(t.bass), sm = sum(t.mid), sh = sum(t.high);
  const tot = sb + sm + sh || 1;
  t.bands = { bass: sb / tot, mid: sm / tot, high: sh / tot };

  // --- envelope shape (intro/outro, attack punch, tail fade) ---
  t.shape = envelopeShape(t.rms, t.fps);
  t.intro_sec = t.shape.intro_sec;
  t.outro_sec = t.shape.outro_sec;
  t.drop_sec = t.dropS > 0 ? t.dropS : 0;

  // --- a coarse break after the drop (valley) for break-aware entry ---
  t.sections = detectBreak(t.rms, t.fps, t.drop_sec, t.outro_sec);
  return t;
}

function envelopeShape(e, fps) {
  const neutral = { intro_sec: 0, outro_sec: e.length / fps, head_e: 0.5,
                    tail_e: 0.5, head_punch: 0.5, tail_fade: 0.5 };
  if (!e || e.length < 8) return neutral;
  let i0 = 0, i1 = e.length;
  while (i0 < e.length && e[i0] <= 0.03) i0++;
  while (i1 > i0 && e[i1 - 1] <= 0.03) i1--;
  if (i1 - i0 < 4) return neutral;
  const fs = 1 / fps;
  const w = (sec) => Math.max(1, Math.min(i1 - i0, Math.round(sec / fs)));
  const wl = w(1.5), ws = w(0.4);
  const mean = (a, b) => {
    let s = 0; for (let k = a; k < b; k++) s += e[k]; return s / Math.max(1, b - a);
  };
  const head_e = mean(i0, i0 + wl), tail_e = mean(i1 - wl, i1);
  const head0 = mean(i0, i0 + ws), tail0 = mean(i1 - ws, i1);
  const head_punch = Math.min(1.5, head0 / (head_e + 1e-6)) / 1.5;
  const tail_fade = Math.max(0, Math.min(1, 1 - tail0 / (tail_e + 1e-6)));
  return {
    intro_sec: +(i0 * fs).toFixed(2), outro_sec: +(i1 * fs).toFixed(2),
    head_e: +head_e.toFixed(4), tail_e: +tail_e.toFixed(4),
    head_punch: +head_punch.toFixed(3), tail_fade: +tail_fade.toFixed(3),
  };
}

function detectBreak(e, fps, dropS, outroS) {
  const out = { break_start_sec: 0, break_end_sec: 0 };
  if (!e || e.length < 32 || dropS <= 0) return out;
  const fs = 1 / fps, win = Math.max(3, Math.round(fps));
  const sm = movavg(e, win);
  const iDrop = Math.round(dropS / fs);
  const iEnd = Math.min(sm.length, Math.round(outroS / fs) - Math.round(2 / fs));
  const need = Math.round(4 / fs);
  let run = 0, start = -1;
  for (let i = iDrop + Math.round(8 / fs); i < iEnd; i++) {
    if (sm[i] < 0.45) {
      if (start < 0) start = i;
      if (++run >= need) {
        out.break_start_sec = +((start) * fs).toFixed(2);
        let j = i; while (j < sm.length && sm[j] < 0.6) j++;
        if (j < sm.length) out.break_end_sec = +(j * fs).toFixed(2);
        break;
      }
    } else { run = 0; start = -1; }
  }
  return out;
}

function movavg(a, w) {
  const out = new Float32Array(a.length);
  let acc = 0;
  for (let i = 0; i < a.length; i++) {
    acc += a[i];
    if (i >= w) acc -= a[i - w];
    out[i] = acc / Math.min(i + 1, w);
  }
  return out;
}

// Fetch + parse a sidecar for an audio path served by the firmware FS API.
// audioPath e.g. "/data/Music/track.mp3" -> reads "/data/Music/track.npx".
export async function loadNpx(audioPath, fetchImpl = fetch) {
  const npxPath = audioPath.replace(/\.[^./]+$/, '') + '.npx';
  const url = '/api/fs/read?path=' + encodeURIComponent(npxPath);
  const r = await fetchImpl(url);
  if (!r.ok) return null;
  const buf = await r.arrayBuffer();
  try { return parseNpx(buf); } catch (e) { return null; }
}

export { TONICS };

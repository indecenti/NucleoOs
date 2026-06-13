// dj-planner.js — the mix brain, ported from Radio Index `plan_music_mix`.
//
// Decides HOW to mix two consecutive songs from family + BPM + bass + energy +
// key + envelope shape. Deterministic (same pair -> same plan), genre-coherent,
// never random. Songs only (no jingles/voice/carts). The fx tags it emits are
// realised by dj-engine.js as Web Audio node graphs (our "VST rack").
//
// Output fields the engine consumes:
//   type      blend | beatmatch | tempo_blend | bass_swap | filter_in |
//             filter_out | riser | downsweep | echo_tail | slam
//   xfade     overlap length (s)
//   curve     epow | punch | fast       (gain crossfade shape)
//   fx        '+'-joined effect tags (see ENGINE in dj-engine.js), or null
//   pitch     resample factor applied to the INCOMING deck (vinyl beat-lock)
//   eff_bpm_b effective BPM of B after pitch (new ref for the next pair)
//   mix_in_off start offset (s) of the incoming track (drop/break/main entry)
//   xfade_bars overlap quantised to N bars (0 = not bar-locked)
//   key_a/key_b/harm  harmonic-mixing diagnostics
//   double_drop / mix_in_drop  flags

import { camelotCompat } from './npx.js';

// ---- genre -> energy (1 chill .. 3 hot) -------------------------------------
const ENERGY_MAP = [
  [['hardcore', 'frenchcore', 'uptempo', 'gabber', 'hardstyle', 'rawstyle',
    'hard ', 'drill', 'phonk', 'hyper', 'rave', 'techno', 'tekno', 'dnb',
    'drum', 'bass', 'rage', 'psytrance', 'psy-trance', 'goa', 'schranz',
    'hardgroove', 'jungle', 'neurofunk', 'jump up', 'dubstep', 'brostep',
    'riddim', 'big room', 'bigroom', 'festival', 'mainstage', 'hardtek'], 3],
  [['trap', 'urban', 'rap', 'hip hop', 'hip-hop', 'hiphop', 'house', 'electro',
    'electronic', 'edm', 'dance', 'pop', 'club', 'remix', 'garage',
    'breakbeat', 'breaks', 'trance', 'progressive', 'minimal', 'synthwave',
    'future bass', 'glitch', 'moombahton', 'idm', 'downtempo edit'], 2],
  [['rnb', 'r&b', 'soul', 'ambient', 'lofi', 'lo-fi', 'chill', 'acoustic',
    'ballad', 'slow', 'jazz', 'blues', 'downtempo', 'piano'], 1],
];

// ---- genre -> family (sound world) ------------------------------------------
const FAMILY_MAP = [
  [['hardcore', 'frenchcore', 'uptempo', 'gabber', 'industrial hardcore',
    'speedcore', 'terrorcore'], 'hardcore'],
  [['hardstyle', 'rawstyle', 'hardtek', 'hardtekk', 'jumpstyle', 'rave',
    'rage', 'hyper', 'tekno', 'hard '], 'hard'],
  [['techno', 'tech house', 'tech-house', 'house', 'deep house', 'bass house',
    'future house', 'electro', 'electronic', 'edm', 'dance', 'club', 'trance',
    'psytrance', 'psy-trance', 'goa', 'progressive', 'big room', 'bigroom',
    'festival', 'mainstage', 'dubstep', 'brostep', 'riddim', 'garage',
    '2-step', 'speed garage', 'breakbeat', 'breaks', 'schranz', 'hardgroove',
    'minimal', 'dnb', 'drum', 'bass', 'jungle', 'neurofunk', 'liquid',
    'jump up', 'synthwave', 'retrowave', 'darksynth', 'outrun', 'glitch',
    'idm', 'moombahton', 'footwork', 'juke', 'acid', 'remix'], 'electronic'],
  [['trap', 'rap', 'hip hop', 'hip-hop', 'hiphop', 'drill', 'phonk', 'urban',
    'grime', 'reggaeton', 'afrobeat', 'amapiano'], 'urban'],
  [['rock', 'metal', 'punk', 'grunge', 'indie', 'alternative', 'hard rock',
    'classic rock', 'emo'], 'rock'],
  [['pop', 'synthpop', 'electropop', 'dance pop', 'disco', 'funk', 'k-pop'], 'pop'],
  [['rnb', 'r&b', 'soul', 'motown', 'jazz', 'blues', 'gospel'], 'soul'],
  [['ambient', 'lofi', 'lo-fi', 'lo fi', 'chill', 'downtempo', 'acoustic',
    'ballad', 'slow', 'piano', 'classical', 'orchestral', 'soundtrack',
    'new age', 'instrumental'], 'chill'],
];

export function genreEnergy(genre) {
  const g = (genre || '').toLowerCase();
  for (const [keys, val] of ENERGY_MAP) if (keys.some((k) => g.includes(k))) return val;
  return 2;
}
export function genreFamily(genre) {
  const g = (genre || '').toLowerCase();
  for (const [keys, fam] of FAMILY_MAP) if (keys.some((k) => g.includes(k))) return fam;
  return 'pop';
}

// hard + hardcore share one PROTECTED cluster, isolated from everything else
// (they beatmatch each other but never cross-mix with techno/house/etc.).
const FAMILY_CLUSTER = { hardcore: 'hard', hard: 'hard' };
export function familyCluster(f) {
  f = (f || '').toLowerCase();
  return FAMILY_CLUSTER[f] || f || 'pop';
}
const CLUB_CLUSTERS = new Set(['hard', 'electronic']);
export function isClubFamily(f) { return CLUB_CLUSTERS.has(familyCluster(f)); }

// ---- crossfade base length by energy, biased by daypart ----------------------
const XFADE_BY_ENERGY = { 3: 7.0, 2: 11.0, 1: 16.0 };
export function musicXfade(energy, bias, durA, durB) {
  const base = (XFADE_BY_ENERGY[energy] || 11.0) * bias;
  const cap = Math.min(durA, durB) * 0.42;
  return Math.round(Math.max(3.0, Math.min(base, cap)) * 100) / 100;
}

// small deterministic gate (replaces Python's zlib.crc32 % N)
let _crcT = null;
function crc32(str) {
  if (!_crcT) {
    _crcT = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      _crcT[n] = c >>> 0;
    }
  }
  let c = 0xffffffff;
  for (let i = 0; i < str.length; i++) c = _crcT[(c ^ str.charCodeAt(i)) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

const r2 = (x) => Math.round(x * 100) / 100;
const SHAPE0 = { intro_sec: 0, outro_sec: 0, head_e: 0.5, tail_e: 0.5, head_punch: 0.5, tail_fade: 0.5 };
const BEAT0 = { period_s: 0, phase_s: 0, conf: 0 };

// `a`, `b`, `c` are track objects: { file_path, family, energy, duration_sec,
//   npx } where npx is the parseNpx() result (bpm, bands, key/camelot, shape,
//   beat, bars, sections, drop_sec). `dp` = { name, xfade_bias } from the brain.
export function planMusicMix(a, b, dp = { name: 'day', xfade_bias: 1.0 },
                             refBpm = null, c = null) {
  const ma = a.npx || {}, mb = b.npx || {};
  const bpmA = +(ma.bpm || 0), bpmB = +(mb.bpm || 0);
  const bassA = +((ma.bands || {}).bass || 0), bassB = +((mb.bands || {}).bass || 0);
  const sameFam = !!a.family &&
    familyCluster(a.family) === familyCluster(b.family);
  const en = Math.min(a.energy, b.energy);
  const base = musicXfade(en, dp.xfade_bias, a.duration_sec, b.duration_sec);
  const cap = Math.min(a.duration_sec, b.duration_sec) * 0.45;

  let mtype = 'blend', curve = en === 3 ? 'punch' : 'epow', fx = null, xf = base;

  const isHard = (a.family || '').includes('hard') || (b.family || '').includes('hard');
  const bassy = isHard || (bassA > 0.40 && bassB > 0.40);

  // ── pitch fader (vinyl): lock the incoming BPM to the running set ──────────
  const famA = a.family || '';
  const club = famA.includes('hard') || famA === 'electronic' || famA === 'urban';
  const lim = club ? 0.06 : 0.035;
  const ref = refBpm && refBpm > 0 ? refBpm : bpmA;
  let pitch = 1.0, effBpmB = bpmB, locked = false;
  if (sameFam && bpmB > 0 && ref > 0) {
    const s = ref / bpmB;
    let skip = false;
    if (c && familyCluster(c.family) === familyCluster(b.family)) {
      const bpmC = +((c.npx || {}).bpm || 0);
      if (bpmC > 0) {
        const nearRefC = Math.abs(1 - bpmC / ref) <= lim;
        const nearBC = Math.abs(1 - bpmC / bpmB) <= lim;
        if (nearBC && !nearRefC) skip = true;
      }
    }
    if (!skip && s >= 1 - lim && s <= 1 + lim) {
      pitch = r2(s * 100) / 100; pitch = Math.round(s * 1e4) / 1e4;
      effBpmB = ref; locked = true;
    }
  }

  // ── harmonic mixing (Camelot) ─────────────────────────────────────────────
  const keyA = (ma.key && ma.key.camelot) || ma.camelot || null;
  const keyB = (mb.key && mb.key.camelot) || mb.camelot || null;
  const harm = sameFam ? camelotCompat(keyA, keyB) : 0;

  let dpct = 1.0;
  if (locked) {
    mtype = 'beatmatch'; curve = 'epow'; fx = null;
    const harmMult = harm >= 0 ? 1.0 + [0.0, 0.15, 0.35][Math.max(0, Math.min(2, harm))] : 0.75;
    xf = r2(Math.min(base * 1.35 * harmMult, cap));
  } else if (bpmA > 0 && bpmB > 0) {
    let ratio = bpmA / bpmB;
    while (ratio > 1.5) ratio /= 2;
    while (ratio < 0.67) ratio *= 2;
    dpct = Math.abs(1 - ratio);
    if (sameFam && dpct <= 0.035) {
      mtype = 'beatmatch'; curve = 'epow'; fx = null;
      xf = r2(Math.min(base * 1.35, cap));
    } else if (sameFam && dpct <= 0.12) {
      mtype = 'tempo_blend'; curve = 'epow'; fx = null;
      xf = r2(Math.min(base * 0.95, cap));
    } else {
      mtype = 'filter_in'; curve = 'fast'; fx = 'lp_in';
      xf = r2(Math.max(3.0, base * 0.55));
    }
  } else if (!sameFam) {
    mtype = 'filter_in'; curve = 'fast'; fx = 'lp_in';
    xf = r2(Math.max(3.0, base * 0.7));
  }

  // ── bass handling: true 2-band kick-swap on the OUTGOING deck ──────────────
  if (['blend', 'beatmatch', 'tempo_blend'].includes(mtype) && bassy) {
    if (mtype === 'beatmatch' && bpmB > 0 && sameFam && isClubFamily(a.family)) {
      fx = 'sc_eq';                          // eq_swap + sidechain pumping (pro)
    } else {
      fx = 'eq_swap';
    }
    if (mtype === 'blend') mtype = 'bass_swap';
  }

  const gate = crc32(a.file_path + '|' + b.file_path) % 3;
  const g4 = crc32('v|' + a.file_path + '|' + b.file_path) % 4;
  let de = 0;
  try { de = (b.energy | 0) - (a.energy | 0); } catch { de = 0; }

  const fxJoin = (deck, bus) => [deck, bus].filter(Boolean).join('+') || null;

  // ── envelope-shape transition (works even without BPM) ─────────────────────
  const sa = ma.shape || SHAPE0, sb = mb.shape || SHAPE0;
  const aFade = +(sa.tail_fade ?? 0.5), aTailE = +(sa.tail_e ?? 0.5), bPunch = +(sb.head_punch ?? 0.5);
  if (mtype === 'blend') {
    if (aFade >= 0.55 && bPunch <= 0.45) {
      curve = 'epow'; fx = null; xf = r2(Math.min(base * 1.4, cap));
    } else if (aFade <= 0.25 && bPunch >= 0.70) {
      curve = 'punch'; fx = null; xf = r2(Math.max(3.0, base * 0.5));
    } else if (bPunch >= 0.70) {
      mtype = 'filter_in'; curve = 'fast'; fx = 'lp_in'; xf = r2(Math.max(3.0, base * 0.6));
    } else if (aTailE >= 0.55 && sameFam && gate === 2 &&
               ['hardcore', 'hard', 'electronic'].includes(a.family)) {
      mtype = 'filter_out'; curve = 'epow'; fx = 'hp_out'; xf = r2(Math.min(base * 1.1, cap));
    }
  }

  // ── coverage by energy direction (riser / downsweep), rotated by g4 ────────
  const rising = de >= 1 || bPunch >= 0.70 || (bpmA > 0 && bpmB > bpmA * 1.05);
  const falling = de <= -1 || aFade >= 0.60 || (bpmA > 0 && bpmB > 0 && bpmB < bpmA * 0.95);
  if (mtype === 'filter_in') {
    if (rising) { mtype = 'riser'; curve = 'fast'; fx = 'lp_in+sweep_up'; xf = r2(Math.max(3.0, base * 0.6)); }
    else if (falling) { mtype = 'downsweep'; curve = 'fast'; fx = 'sweep_down'; xf = r2(Math.max(2.5, base * 0.5)); }
    else if (gate === 0) { fx = 'lp_in'; }
    else if (gate === 1) { mtype = 'downsweep'; fx = 'sweep_down'; xf = r2(Math.max(2.5, base * 0.5)); }
    else { mtype = 'echo_break'; fx = 'echo_tail+sweep_down'; xf = r2(Math.max(2.5, base * 0.55)); }
  } else if (['beatmatch', 'tempo_blend', 'bass_swap'].includes(mtype)) {
    const baseDeck = ['eq_swap', 'hp_out', 'echo_tail', 'sc_eq', 'sidechain', 'comp_glue'].includes(fx) ? fx : null;
    fx = fxJoin(baseDeck, (rising && bPunch >= 0.70 && club && (g4 === 0 || g4 === 2)) ? 'sweep_up' : null);
  } else if (mtype === 'filter_out') {
    fx = fxJoin('hp_out', (falling && g4 === 1) ? 'sweep_down' : null);
  } else if (mtype === 'blend') {
    fx = fxJoin(null, (rising && bPunch >= 0.70 && g4 === 2) ? 'sweep_up' : null);
  }

  // ── SLAM (festival drop) on hot, beat-locked club; hard cluster dares more ──
  const hardCluster = familyCluster(a.family) === 'hard';
  const slamOk = mtype === 'beatmatch' && en === 3 &&
    ['hardcore', 'hard', 'electronic', 'urban'].includes(a.family) &&
    (gate === 0 || (hardCluster && gate === 1));
  if (slamOk) {
    const isProDance = ['hardcore', 'hard', 'electronic'].includes(a.family);
    mtype = 'slam'; curve = 'punch';
    const swap = (bpmB > 0 && sameFam && isClubFamily(a.family)) ? 'sc_eq' : 'eq_swap';
    const tempo = effBpmB > 0 ? effBpmB : (bpmA || bpmB || 0);
    const bar = tempo > 0 ? 4 * 60 / tempo : 1.6;
    xf = r2(Math.min(Math.max(base * 0.22, bar), 2 * bar));
    if (hardCluster && bpmB > 0 && gate === 0 && g4 === 2) {
      fx = 'backspin+impact'; xf = r2(Math.min(Math.max(base * 0.22, 1.5 * bar), 2 * bar));
    } else if (g4 === 2) { fx = 'tape_stop+impact'; }
    else if ((g4 === 1 || g4 === 3) && isProDance && bpmB > 0) { fx = swap + '+kick_roll'; }
    else { fx = swap + '+impact'; }
  }

  // ── night/chill: long, soft, occasional dub echo ──────────────────────────
  if (dp.name === 'night' && en <= 1) {
    if (gate === 1) { mtype = 'echo_tail'; curve = 'epow'; fx = 'echo_tail'; }
    else if (g4 === 3) { mtype = 'downsweep'; curve = 'epow'; fx = 'echo_tail+sweep_down'; }
    else { mtype = 'blend'; curve = 'epow'; fx = null; }
    xf = r2(Math.min(base * 1.2, cap));
  }

  // ── glue comp on long non-club blends ─────────────────────────────────────
  if (['blend', 'filter_out', 'echo_tail'].includes(mtype) && xf >= 9.0 &&
      !isClubFamily(a.family) && !fx) {
    fx = 'comp_glue';
  }

  // ── final clamp: never cut a track before its half ────────────────────────
  const minDur = Math.max(1.0, Math.min(+a.duration_sec, +b.duration_sec));
  const hardCap = 0.45 * minDur;
  if (mtype === 'slam') xf = Math.max(1.0, Math.min(xf, hardCap));
  else { xf = Math.min(xf, hardCap); xf = Math.max(Math.min(3.0, hardCap), xf); }

  // ── anti-naked: short cut with no coverage gets a coherent one ────────────
  if (!fx && xf <= 4.0 && !['beatmatch', 'tempo_blend', 'bass_swap'].includes(mtype)) {
    fx = de >= 0 ? 'impact' : 'sweep_down';
  }

  // ── beat / phrase alignment (anti "double kick" amateur flam) ─────────────
  let mixInOff = null, dropAtEntry = false, doubleDrop = false;
  if (['beatmatch', 'tempo_blend', 'bass_swap', 'slam'].includes(mtype)) {
    const ba = ma.beat || BEAT0, bb = mb.beat || BEAT0;
    const pA = +(ba.period_s || 0), phA = +(ba.phase_s || 0), cA = +(ba.conf || 0);
    const pB = +(bb.period_s || 0), phB = +(bb.phase_s || 0), cB = +(bb.conf || 0);
    const aOut = +(sa.outro_sec || 0) || +a.duration_sec;
    const bIn = +(sb.intro_sec || 0);
    const bDrop = +(mb.drop_sec || 0);
    const bFam = b.family || '';
    const clubDrop = bDrop > 0 && (bFam.includes('hard') || bFam === 'electronic' || bFam === 'urban');
    if (pA > 0.15 && pB > 0.15 && cA >= 0.15 && cB >= 0.15) {
      // phrase-16 snap of the splice on A
      const barsA = ma.bars || {};
      const ph16P = +(barsA.phrase16_period_s || 0), ph16Ph = +(barsA.phrase16_phase_s || 0);
      let newXf = null;
      if (mtype === 'beatmatch' && ph16P > 0 && ph16P <= hardCap) {
        const splice0 = aOut - xf;
        const k16 = Math.round((splice0 - ph16Ph) / ph16P);
        const cand = aOut - (ph16Ph + k16 * ph16P);
        if (Math.min(3.0, hardCap) <= cand && cand <= hardCap) newXf = cand;
      }
      if (newXf === null) {
        const splice0 = aOut - xf;
        const k = Math.round((splice0 - phA) / pA);
        newXf = aOut - (phA + k * pA);
      }
      const loXf = mtype === 'slam' ? 1.0 : Math.min(3.0, hardCap);
      if (loXf <= newXf && newXf <= hardCap) xf = newXf;

      // double-drop / layering (hard cluster only)
      if (hardCluster && mtype === 'beatmatch' && gate === 2 && aTailE >= 0.15) {
        const tempoDD = effBpmB > 0 ? effBpmB : (bpmB || bpmA || 0);
        const barDD = tempoDD > 0 ? 4 * 60 / tempoDD : 0;
        if (barDD > 0) {
          let nb = 4;
          while (nb > 2 && nb * barDD > hardCap) nb--;
          const candXf = nb * barDD;
          if (candXf >= 3.0 && candXf <= hardCap) {
            const target = (bDrop > 0 && bDrop < b.duration_sec * 0.6) ? bDrop - 0.55 * candXf : bIn;
            const kdd = Math.round((target - phB) / pB);
            let off = phB + kdd * pB;
            while (off < bIn - 1e-6) off += pB;
            if (off >= 0 && off < b.duration_sec * 0.6) {
              xf = r2(candXf); mixInOff = Math.round(off * 1000) / 1000;
              doubleDrop = true;
              if (!['sc_eq', 'eq_swap'].includes(fx)) fx = bpmB > 0 ? 'sc_eq' : 'eq_swap';
            }
          }
        }
      }
      // drop entry of B
      if (!doubleDrop && clubDrop && ['beatmatch', 'tempo_blend'].includes(mtype) &&
          bDrop < b.duration_sec * 0.6) {
        const k2 = Math.round((bDrop - phB) / pB);
        const off = phB + k2 * pB;
        if (off > 0 && off < b.duration_sec * 0.7) { mixInOff = Math.round(off * 1000) / 1000; dropAtEntry = true; }
      }
      // break-aware entry (override drop when harmonic clash)
      if (!doubleDrop && (mixInOff === null || (harm < 0 && !dropAtEntry))) {
        const secB = mb.sections || {};
        const bBreak = +(secB.break_start_sec || 0);
        if (bBreak > 0 && bBreak < b.duration_sec * 0.5 && pB > 0) {
          const k3 = Math.round((bBreak - phB) / pB);
          const off = phB + k3 * pB;
          if (off > 0) {
            mixInOff = Math.round(off * 1000) / 1000; dropAtEntry = false;
            if (!fx && ['beatmatch', 'tempo_blend'].includes(mtype)) fx = 'reverb_tail';
          }
        }
      }
      if (mixInOff === null) {
        const m = bIn > phB ? Math.ceil((bIn - phB) / pB) : 0;
        let off = phB + m * pB;
        while (off < bIn - 1e-6) off += pB;
        if (off >= 0 && off < b.duration_sec * 0.5) mixInOff = Math.round(off * 1000) / 1000;
      }
    }
  }

  // ── phrase quantisation of the overlap length ─────────────────────────────
  let xfadeBars = 0;
  if (['beatmatch', 'tempo_blend', 'bass_swap'].includes(mtype) && !doubleDrop) {
    const beatBpm = effBpmB > 0 ? effBpmB : bpmA;
    if (beatBpm > 0) {
      const bar = 4 * 60 / beatBpm;
      let bars;
      if (mtype === 'beatmatch') {
        const pref = 4 * bar;
        bars = (pref <= cap && pref >= xf * 0.8) ? 4 : Math.max(2, Math.round(xf / bar));
      } else bars = Math.max(2, Math.round(xf / bar));
      let xfq = bars * bar;
      while (xfq > cap && bars > 2) { bars--; xfq = bars * bar; }
      if (xfq <= cap && xfq >= 2.5) { xf = r2(xfq); xfadeBars = bars; }
    }
  }

  // ── club filter style: resonant variant on hard/electronic ────────────────
  if (fx) {
    fx = fx.split('+').map((tk) => {
      if (tk === 'lp_in' && isClubFamily(b.family)) return 'lp_in_res';
      if (tk === 'hp_out' && isClubFamily(a.family)) return 'hp_out_res';
      return tk;
    }).join('+');
  }

  const out = {
    type: mtype, xfade: r2(xf), curve, fx,
    bpm_a: r2(bpmA), bpm_b: r2(bpmB), pitch: Math.round(pitch * 1e4) / 1e4,
    eff_bpm_b: r2(effBpmB), xfade_bars: xfadeBars,
    key_a: keyA, key_b: keyB, harm,
  };
  if (mixInOff !== null) out.mix_in_off = mixInOff;
  if (dropAtEntry) out.mix_in_drop = true;
  if (doubleDrop) out.double_drop = true;
  return out;
}

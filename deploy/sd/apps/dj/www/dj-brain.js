// dj-brain.js — the DJ's "consciousness", ported from Radio Index
// radio_dj_brain.py, trimmed to SONGS ONLY (no carts/voice/jingles).
//
// Rule-based, deterministic, ~free. Two jobs:
//   1. give the planner a daypart context (name + xfade_bias) and shape the
//      ENERGY ARC of the set (warm-up -> peak -> cool-down);
//   2. pick the NEXT track for Auto-DJ: same sound-world, harmonic match,
//      moves energy toward the daypart target, never repeats artist/title
//      too soon, breaks family monotony.

import { familyCluster, isClubFamily, genreEnergy, genreFamily } from './dj-planner.js';
import { camelotCompat } from './npx.js';

// Daypart profile: a real FM follows an energy curve through the day.
function daypart(hour) {
  const h = ((hour % 24) + 24) % 24;
  if (h >= 6 && h < 10) return { name: 'morning', target: 2, xfade_bias: 1.0 };
  if (h >= 10 && h < 14) return { name: 'midday', target: 2, xfade_bias: 1.0 };
  if (h >= 14 && h < 19) return { name: 'drive', target: 3, xfade_bias: 0.85 };
  if (h >= 19 && h < 23) return { name: 'evening', target: 3, xfade_bias: 1.15 };
  return { name: 'night', target: 1, xfade_bias: 1.35 };
}

export class DjBrain {
  constructor(opts = {}) {
    this.histLen = 24;
    this.sameFamilyFatigue = 5;     // after N same-family tracks, the DJ breaks away
    this.hist = [];                 // [{family, energy, artist, title}]
    this.recentArtists = [];        // anti-repeat window
    this.recentTitles = [];
    this.energyBias = 0;            // user nudge: -1 cool .. +1 push
    this.now = opts.now || (() => new Date());
  }

  // ---- context ----
  daypart() { return daypart(this.now().getHours()); }

  rollingEnergy() {
    if (!this.hist.length) return 2.0;
    const last = this.hist.slice(-8);
    return last.reduce((s, r) => s + (r.energy || 2), 0) / last.length;
  }

  sameFamilyStreak() {
    if (!this.hist.length) return 0;
    const fam = this.hist[this.hist.length - 1].family;
    let n = 0;
    for (let i = this.hist.length - 1; i >= 0; i--) {
      if (fam && this.hist[i].family === fam) n++; else break;
    }
    return n;
  }

  note(track) {
    this.hist.push({
      family: track.family, energy: track.energy,
      artist: (track.artist || '').toLowerCase(),
      title: (track.title || '').toLowerCase(),
    });
    if (this.hist.length > this.histLen) this.hist.shift();
    if (track.artist) {
      this.recentArtists.push((track.artist || '').toLowerCase());
      if (this.recentArtists.length > 6) this.recentArtists.shift();
    }
    if (track.title) {
      this.recentTitles.push((track.title || '').toLowerCase());
      if (this.recentTitles.length > 12) this.recentTitles.shift();
    }
  }

  // Energy target for the NEXT track: daypart base, bent by the set's arc and
  // the user's nudge. If the set is already hot, let it breathe; if we've been
  // calm a while, allow a climb.
  targetEnergy() {
    const prof = this.daypart();
    let t = prof.target + this.energyBias;
    const re = this.rollingEnergy();
    if (re >= 2.6) t -= 1;          // already boiling -> cool
    else if (re <= 1.7) t += 1;     // been calm -> allow a lift
    return Math.max(1, Math.min(3, Math.round(t)));
  }

  // Context the planner needs.
  context() {
    const prof = this.daypart();
    return { name: prof.name, xfade_bias: prof.xfade_bias };
  }

  // ---- Auto-DJ track selection ----
  // lib: [{file_path, artist, title, genre, family, energy, duration_sec, npx}]
  // Picks the best next track after `current`. Deterministic given the same
  // history (no RNG): scores candidates and takes the max.
  pickNext(lib, current) {
    if (!lib || !lib.length) return null;
    const curFam = current ? current.family : null;
    const curCluster = curFam ? familyCluster(curFam) : null;
    const curKey = current && current.npx ? (current.npx.camelot) : null;
    const tgt = this.targetEnergy();
    const streak = this.sameFamilyStreak();

    let best = null, bestScore = -1e9;
    for (const t of lib) {
      if (current && t.file_path === current.file_path) continue;
      const fam = t.family || genreFamily(t.genre);
      const en = t.energy || genreEnergy(t.genre);
      const artist = (t.artist || '').toLowerCase();
      const title = (t.title || '').toLowerCase();

      // HARD constraints: no recent repeat of title; soft on artist.
      if (this.recentTitles.includes(title)) continue;

      let score = 0;
      // 1. stay in the same cluster (coherent set) unless monotony fatigue hit
      if (curCluster) {
        const sameCluster = familyCluster(fam) === curCluster;
        if (sameCluster) score += streak >= this.sameFamilyFatigue ? -2 : +5;
        else score += streak >= this.sameFamilyFatigue ? +3 : -3;
      }
      // 2. energy toward the daypart target (closer = better)
      score += 3 - Math.abs(en - tgt);
      // 3. harmonic match on the Camelot wheel
      const k = t.npx ? t.npx.camelot : null;
      const harm = camelotCompat(curKey, k);
      score += harm * 1.5;                  // +3 golden, +1.5 compatible, -1.5 clash
      // 4. mild penalty for repeating the same artist recently
      if (this.recentArtists.includes(artist)) score -= 2;
      // 5. tiny deterministic tie-break from the path (stable, not random)
      score += (hashStr(t.file_path) % 100) / 1000;

      if (score > bestScore) { bestScore = score; best = t; }
    }
    return best;
  }
}

function hashStr(s) {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 16777619); }
  return (h >>> 0);
}

export { daypart };

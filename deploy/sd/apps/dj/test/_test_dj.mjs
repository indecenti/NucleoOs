// Node test for the DJ brain: planner decisions + consciousness. No audio —
// feeds crafted track metadata and asserts the plan matches the Radio Index
// "tricks" we ported. Run: node apps/dj/www/_test_dj.mjs
import { planMusicMix, genreFamily, genreEnergy } from '../www/dj-planner.js';
import { DjBrain } from '../www/dj-brain.js';

let pass = 0, fail = 0;
function ok(name, cond, extra = '') {
  if (cond) { pass++; console.log('  ok  ', name); }
  else { fail++; console.log('  FAIL', name, extra); }
}

// Build a track with a beat-locked NPX (conf high so alignment engages).
function mk(path, genre, bpm, camelot, dur = 200, drop = 0, opts = {}) {
  const period = bpm > 0 ? 60 / bpm : 0;
  return {
    file_path: path, artist: opts.artist || path, title: opts.title || path,
    genre, family: genreFamily(genre), energy: genreEnergy(genre),
    duration_sec: dur,
    npx: {
      bpm, camelot, bands: { bass: opts.bass ?? 0.5, mid: 0.3, high: 0.2 },
      drop_sec: drop,
      shape: { intro_sec: opts.intro ?? 1, outro_sec: opts.outro ?? dur - 1,
               head_e: 0.6, tail_e: opts.tailE ?? 0.6,
               head_punch: opts.punch ?? 0.5, tail_fade: 0.4 },
      beat: { period_s: period, phase_s: 0, conf: 0.8 },
      bars: { bar4_period_s: period * 4, phrase16_period_s: period * 16,
              bar4_phase_s: 0, phrase16_phase_s: 0 },
      sections: { break_start_sec: opts.breakAt || 0, break_end_sec: 0 },
    },
  };
}

console.log('— two hardstyle, same 150 BPM, golden key —');
{
  const a = mk('/m/hs_a.mp3', 'hardstyle', 150, '5A', 220, 0, { bass: 0.6 });
  const b = mk('/m/hs_b.mp3', 'hardstyle', 150, '5A', 220, 0, { bass: 0.6 });
  const p = planMusicMix(a, b);
  console.log('   ', JSON.stringify(p));
  ok('beatmatch', p.type === 'beatmatch' || p.type === 'slam', p.type);
  ok('pitch locked ~1.0', Math.abs(p.pitch - 1) < 1e-6, '' + p.pitch);
  ok('harmonic = identical', p.harm === 2, '' + p.harm);
  ok('bass handled (sc_eq/eq_swap/kick)', /sc_eq|eq_swap|kick|impact/.test(p.fx || ''), p.fx);
  ok('phrase bar-locked or slam', p.xfade_bars >= 2 || p.type === 'slam', '' + p.xfade_bars);
}

console.log('— cross-cluster: hardstyle 150 vs house 128 (NEVER beatmatch) —');
{
  const a = mk('/m/hs.mp3', 'hardstyle', 150, '5A');
  const b = mk('/m/ho.mp3', 'house', 128, '8B');
  const p = planMusicMix(a, b);
  console.log('   ', JSON.stringify(p));
  ok('not beatmatch across clusters', p.type !== 'beatmatch' && p.type !== 'slam', p.type);
  ok('filtered/swept cut', /filter_in|riser|downsweep|echo/.test(p.type), p.type);
  ok('no harmonic across clusters', p.harm === 0, '' + p.harm);
}

console.log('— same cluster electronic, near BPM 128 vs 130 —');
{
  const a = mk('/m/e1.mp3', 'techno', 128, '8B');
  const b = mk('/m/e2.mp3', 'techno', 130, '9B');
  const p = planMusicMix(a, b);
  console.log('   ', JSON.stringify(p));
  ok('beatmatch or tempo_blend', ['beatmatch', 'tempo_blend', 'slam', 'bass_swap'].includes(p.type), p.type);
}

console.log('— incoming has a drop: enter on the drop —');
{
  const a = mk('/m/d1.mp3', 'hardstyle', 150, '5A', 220, 0, { bass: 0.6 });
  const b = mk('/m/d2.mp3', 'hardstyle', 150, '7A', 220, 32, { bass: 0.6 });
  const p = planMusicMix(a, b);
  console.log('   ', JSON.stringify(p));
  ok('mix_in_off set', typeof p.mix_in_off === 'number', '' + p.mix_in_off);
}

console.log('— never cut before half —');
{
  const a = mk('/m/s1.mp3', 'pop', 120, '1A', 30);
  const b = mk('/m/s2.mp3', 'pop', 120, '1A', 30);
  const p = planMusicMix(a, b);
  ok('xfade <= 45% of shortest', p.xfade <= 0.45 * 30 + 0.01, '' + p.xfade);
}

console.log('— consciousness: energy arc cools a hot set —');
{
  const brain = new DjBrain({ now: () => new Date(2026, 0, 1, 16) }); // drive, target 3
  for (let i = 0; i < 6; i++) brain.note({ family: 'hard', energy: 3, artist: 'X' + i, title: 'T' + i });
  ok('rolling energy hot', brain.rollingEnergy() >= 2.6, '' + brain.rollingEnergy());
  ok('target cooled below 3', brain.targetEnergy() < 3, '' + brain.targetEnergy());
  ok('family streak detected', brain.sameFamilyStreak() === 6, '' + brain.sameFamilyStreak());
}

console.log('— consciousness: pickNext stays coherent & harmonic —');
{
  const brain = new DjBrain({ now: () => new Date(2026, 0, 1, 16) });
  const cur = mk('/m/cur.mp3', 'hardstyle', 150, '5A', 200, 0, { artist: 'A' });
  brain.note({ ...cur, artist: 'a', title: '/m/cur.mp3' });
  const lib = [
    mk('/m/h1.mp3', 'hardstyle', 150, '7A', 200, 0, { artist: 'B' }),  // same cluster, compatible key
    mk('/m/h2.mp3', 'hardstyle', 150, '2B', 200, 0, { artist: 'C' }),  // same cluster, clash key
    mk('/m/p1.mp3', 'pop', 120, '8B', 200, 0, { artist: 'D' }),        // other cluster
  ];
  const pick = brain.pickNext(lib, cur);
  console.log('   picked', pick && pick.file_path);
  ok('picks same-cluster hardstyle', pick && pick.family === 'hard', pick && pick.family);
  ok('prefers harmonic match (7A over 2B)', pick && pick.file_path === '/m/h1.mp3', pick && pick.file_path);
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);

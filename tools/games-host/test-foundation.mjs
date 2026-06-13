// Host-side parse + pure-logic tests for the Game Center foundation (no device/browser).
// Shims the absolute browser import specifiers so the real source files load under Node.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const T = join(here, '_t');
mkdirSync(T, { recursive: true });

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL:', m); } };

// Minimal browser globals some modules touch lazily.
globalThis.localStorage = { _d: {}, getItem(k) { return this._d[k] ?? null; }, setItem(k, v) { this._d[k] = String(v); } };

function load(relSrc, repl) {
  let src = readFileSync(join(root, relSrc), 'utf8');
  for (const [from, to] of repl) src = src.split(from).join(to);
  const out = join(T, relSrc.split('/').pop().replace('.js', '.mjs'));
  writeFileSync(out, src);
  return pathToFileURL(out).href;
}

// ---- 1. nucleo-game.js parses + registry works ---------------------------------------------
const gameMod = await import(load('apps/games/www/nucleo-game.js', []));
ok(typeof gameMod.defineGame === 'function' && typeof gameMod.GameHarness === 'function', 'nucleo-game exports');
gameMod.defineGame({ id: 'x', name: 'X', minPlayers: 2, maxPlayers: 4, aiCapable: true, realtime: true });
ok(gameMod.getGame('x') && gameMod.getGame('x').name === 'X', 'registry stores game');
const listed = gameMod.listGames().find(g => g.id === 'x');
ok(listed && listed.maxPlayers === 4 && listed.realtime === true, 'listGames reflects contract');
ok(gameMod.getGame('nope') === null, 'unknown game -> null');

// ---- 2. nucleo-play.js parses + seat/spectator logic ---------------------------------------
const playUrl = load('apps/games/www/nucleo-play.js', []);
// Stub RTCPeerConnection so module-level + constructor stay happy if ever touched.
globalThis.RTCPeerConnection = class { constructor() {} createDataChannel() { return {}; } };
const { NucleoPlay } = await import(playUrl);
const play = new NucleoPlay({ profile: { name: 'Ada', avatar: '🦊', color: '#22d3ee' } });
ok(play.profile.name === 'Ada' && play.name === 'Ada', 'profile wired into play');
ok(play.peerId.startsWith('p_'), 'peer id generated');
play.maxSeats = 2;
play.roster = [{ seat: 0 }, { seat: -1, spectator: true }];   // 1 player + 1 spectator
ok(play._seatsTaken() === 1, 'spectators excluded from seat count');
ok(play._nextFreeSeat() === 1, 'next free seat skips taken');
play.roster = [{ seat: 0 }, { seat: 1 }];
ok(play._nextFreeSeat() === -1, 'no free seat when full');
play.roster = [{ seat: 0, name: 'Ada', avatar: '🦊', color: '#22d3ee', ai: false, spectator: false }];
const rw = play._rosterWire()[0];
ok(rw.avatar === '🦊' && rw.color === '#22d3ee' && rw.spectator === false, 'roster wire carries avatar/color/spectator');

// ---- 3. profile.js pure stat logic ---------------------------------------------------------
writeFileSync(join(T, 'play-shim.mjs'), 'export const _fs = { mkdir: async()=>{}, writeJSON: async()=>{}, readJSON: async()=>null }; export default {};');
const Pr = await import(load('apps/games/www/profile.js', [["'/apps/games/nucleo-play.js'", "'./play-shim.mjs'"]]));
let s = {};
s = Pr.applyResult(s, 'tris', 'win');
s = Pr.applyResult(s, 'tris', 'win');
ok(s.tris.w === 2 && s.tris.streak === 2 && s.tris.best === 2, 'two wins -> streak 2');
s = Pr.applyResult(s, 'tris', 'loss');
ok(s.tris.l === 1 && s.tris.streak === 0 && s.tris.best === 2, 'loss resets streak, keeps best');
s = Pr.applyResult(s, 'tris', 'draw');
ok(s.tris.d === 1 && s.tris.games === 4, 'draw counted, games total');
ok(s._total.w === 2 && s._total.games === 4, 'totals aggregate across games');
const sum = Pr.summarize(s, 'tris');
ok(sum.winRate === 50, 'win rate computed (2/4)');
ok(Pr.AVATARS.length >= 8 && Pr.COLORS.length >= 6, 'avatar/colour palettes present');
// purity: applyResult must not mutate input
const before = { tris: { w: 1, l: 0, d: 0, games: 1, streak: 1, best: 1 } };
const snap = JSON.stringify(before);
Pr.applyResult(before, 'tris', 'win');
ok(JSON.stringify(before) === snap, 'applyResult is pure (no mutation)');

// ---- 4. GameHarness runtime — regression guards (recursion, resilience, lifecycle) ---------
{
  const mkPlay = (isHost, seat) => {
    const hs = {};
    const play = { isHost, seat, roomId: 'r_test', roster: [{ seat: 0, name: 'H' }, { seat: 1, name: 'G' }], on: (e, f) => { (hs[e] = hs[e] || []).push(f); }, send: () => {} };
    return { play, emit: (e, p) => (hs[e] || []).forEach(f => f(p)) };
  };
  const base = { renderMode: 'custom', minPlayers: 1, maxPlayers: 2, aiCapable: false, realtime: false, render() {}, mount() {}, unmount() {}, onKey: () => null, onPointer: () => null, ai: () => null, tick: s => s };
  const _err = console.error; console.error = () => {};   // the throws below are caught + logged via _warn — keep output clean

  // (a) over fires exactly once even when the over-handler injects a host action (the stopCoach recursion)
  {
    const G = { ...base, setup: () => ({ done: false, n: 0 }), isOver: s => s && s.done ? { winner: 0 } : null,
      reduce: (s, a) => a.type === 'win' ? { ...s, done: true } : (a.type === 'brain' ? { ...s, brain: a.brain, t: (s.t || 0) + 1 } : (a.type === 'move' ? { ...s, n: s.n + 1 } : s)) };
    const { play } = mkPlay(true, 0); let h, overCalls = 0, threw = null;
    h = new gameMod.GameHarness(play, G, {}, { onOver: () => { overCalls++; try { h.hostAct({ type: 'brain', brain: null }); } catch (e) { threw = e; } } });
    h.begin({}); try { h._applyInput({ type: 'win' }, 0); } catch (e) { threw = e; }
    ok(overCalls === 1 && !threw && h._over === true, 'harness: over fires once, no recursion via injected host action');
    const n0 = h.state.n; h._applyInput({ type: 'move' }, 0);
    ok(h.state.n === n0, 'harness: input ignored after the match is decided');
  }

  // (b) a throwing reduce is dropped — the host survives and keeps accepting moves
  {
    const G = { ...base, setup: () => ({ n: 0, done: false }), isOver: s => s.done ? { winner: 0 } : null,
      reduce: (s, a) => { if (a.type === 'boom') throw new Error('reduce'); if (a.type === 'move') return { ...s, n: s.n + 1 }; return s; } };
    const { play } = mkPlay(true, 0); const h = new gameMod.GameHarness(play, G, {}, {});
    h.begin({}); let threw = null;
    try { h._applyInput({ type: 'move' }, 0); h._applyInput({ type: 'boom' }, 0); h._applyInput({ type: 'move' }, 0); } catch (e) { threw = e; }
    ok(!threw && h.state.n === 2, 'harness: throwing reduce dropped, host recovers');
  }

  // (c) a throwing isOver degrades to "not over" (no freeze), then recovers
  {
    let boom = true;
    const G = { ...base, setup: () => ({ n: 0 }), isOver: s => { if (boom) throw new Error('isOver'); return s.n >= 2 ? { winner: 0 } : null; },
      reduce: (s, a) => a.type === 'move' ? { ...s, n: s.n + 1 } : s };
    const { play } = mkPlay(true, 0); let overCalls = 0; const h = new gameMod.GameHarness(play, G, {}, { onOver: () => overCalls++ });
    h.begin({}); let threw = null;
    try { h._applyInput({ type: 'move' }, 0); h._applyInput({ type: 'move' }, 0); } catch (e) { threw = e; }
    ok(!threw && h._over === false, 'harness: throwing isOver treated as not-over (no freeze)');
    boom = false; h._applyInput({ type: 'move' }, 0);
    ok(overCalls === 1 && h._over === true, 'harness: isOver recovers and fires over once');
  }

  // (d) rematch re-arms over detection
  {
    const G = { ...base, setup: () => ({ done: false }), isOver: s => s.done ? { winner: 0 } : null, reduce: (s, a) => a.type === 'win' ? { ...s, done: true } : s };
    const { play } = mkPlay(true, 0); let overCalls = 0; const h = new gameMod.GameHarness(play, G, {}, { onOver: () => overCalls++ });
    h.begin({}); h._applyInput({ type: 'win' }, 0); const m1 = overCalls;
    h.rematch(); const reset = h._over; h._applyInput({ type: 'win' }, 0);
    ok(m1 === 1 && reset === false && overCalls === 2, 'harness: rematch re-arms over detection');
  }

  // (e) stop() leaves no dangling timers
  {
    const G = { ...base, realtime: true, tickHz: 20, setup: () => ({ n: 0 }), isOver: () => null, tick: s => ({ ...s, n: s.n + 1 }) };
    const { play } = mkPlay(true, 0); const h = new gameMod.GameHarness(play, G, {}, {});
    h.begin({}); const up = !!h._tickTimer; h.stop();
    ok(up && !h._tickTimer && !h._raf && h._aiTimers.length === 0, 'harness: stop() clears tick/raf/ai timers');
  }

  // (f) a throwing tick is skipped — no hard freeze, last good state kept, warnings bounded
  {
    const G = { ...base, realtime: true, tickHz: 20, setup: () => ({ n: 0 }), isOver: () => null, tick: () => { throw new Error('tick'); } };
    const { play } = mkPlay(true, 0); const h = new gameMod.GameHarness(play, G, {}, {});
    h.begin({});
    await new Promise(r => setTimeout(r, 130));
    ok(h.state && h.state.n === 0 && !!h._tickTimer && (h._warned.tick || 0) > 0, 'harness: throwing tick skipped (no freeze, bounded warns)');
    h.stop();
  }

  // (g) GUEST follows the host lifecycle from snapshots: onStart on a fresh match, onOver once, rematch re-arms
  {
    const G = { ...base, setup: () => ({}), isOver: s => s && s.done ? { winner: 1 } : null, reduce: s => s };
    const { play, emit } = mkPlay(false, 1); let overCalls = 0, startCalls = 0;
    new gameMod.GameHarness(play, G, {}, { onOver: () => overCalls++, onStart: () => startCalls++ });
    emit('message', { from: 0, data: { k: 'state', s: { done: false }, seq: 1 } });
    const startOnFresh = startCalls;
    emit('message', { from: 0, data: { k: 'state', s: { done: false }, seq: 2 } });
    emit('message', { from: 0, data: { k: 'state', s: { done: true }, seq: 3 } });
    emit('message', { from: 0, data: { k: 'state', s: { done: true }, seq: 4 } });
    const overOnce = overCalls;
    emit('message', { from: 0, data: { k: 'state', s: { done: false }, seq: 1 } });
    emit('message', { from: 0, data: { k: 'state', s: { done: true }, seq: 6 } });
    ok(startOnFresh === 1 && overOnce === 1, 'harness: guest fires onStart(fresh) + onOver exactly once');
    ok(startCalls === 2 && overCalls === 2, 'harness: guest rematch re-arms lifecycle');
  }

  console.error = _err;
}

console.log(`\nFoundation: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);

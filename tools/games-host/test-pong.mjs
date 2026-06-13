// Host-side logic test for pong.js (physics, scoring, coach mapping) — no device/browser.
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { pathToFileURL, fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
const here = dirname(fileURLToPath(import.meta.url)), root = join(here, '..', '..');
mkdirSync(join(here, '_t'), { recursive: true });
writeFileSync(join(here, '_t', 'shim.mjs'), 'export const defineGame = d => d;\nexport default defineGame;\n');
let src = readFileSync(join(root, 'apps/games/www/games/pong.js'), 'utf8').replace("'/apps/games/nucleo-game.js'", "'./shim.mjs'");
writeFileSync(join(here, '_t', 'pong.mjs'), src);
const g = (await import(pathToFileURL(join(here, '_t', 'pong.mjs')).href)).default;

let pass = 0, fail = 0; const ok = (c, m) => { if (c) pass++; else { fail++; console.log('  FAIL:', m); } };
const players = [{ seat: 0, name: 'A', ai: false }, { seat: 1, name: 'B', ai: true }];

let s = g.setup(players, 1, { punti: '5', livello: 'difficile', mente: 'llm' });
ok(s.scores.join() === '0,0' && s.target === 5, 'setup: score 0-0, target 5');
ok(s.ball.vx !== 0, 'ball is moving');
ok(s.aiSeats.length === 1 && s.aiSeats[0] === 1, 'seat 1 marked AI');
ok(s.policy[1] && s.policy[1].react > 0.9, 'difficile policy applied to AI seat');

// tick advances the ball
const t1 = g.tick(s, 33);
ok(t1.ball.x !== s.ball.x && t1.tk === 1, 'tick advances ball + tick counter');

// human input
let r = g.reduce(s, { type: 'paddle', y: 40 }, 0);
ok(r.paddles[0].target === 40 && r.paddles[0].vel === 0, 'paddle drag sets target');
r = g.reduce(s, { type: 'vel', dir: -1 }, 0);
ok(r.paddles[0].vel === -1, 'arrow key sets velocity');

// policy injection (the coach path)
r = g.reduce(s, { type: 'policy', seat: 1, params: { react: 0.6 } }, -1);
ok(r.policy[1].react === 0.6, 'policy action updates AI seat params');

// scoring: ball heading left past a far-away paddle -> seat 1 scores, ball recenters
let sc = g.setup(players, 0, { punti: '5' });
sc.ball = { x: 9, y: 10, vx: -130, vy: 0 };
sc.paddles[0].y = 110;                      // paddle nowhere near ball.y=10
let scored = sc; for (let i = 0; i < 4 && scored.scores[1] === 0; i++) scored = g.tick(scored, 33);
ok(scored.scores[1] === 1, 'missed ball on left -> seat 1 scores (got ' + scored.scores.join('-') + ')');
ok(Math.abs(scored.ball.x - 100) < 1, 'ball recentred after a point');

// a paddle in the way reflects the ball instead of conceding
let bnc = g.setup(players, 0, {}); bnc.ball = { x: 11, y: 60, vx: -130, vy: 0 }; bnc.paddles[0].y = 60;
const aft = g.tick(bnc, 33);
ok(aft.ball.vx > 0 && aft.scores.join() === '0,0', 'ball bounces off paddle (vx flips, no score)');

// win detection
let w = g.setup(players, 0, { punti: '5' }); w.scores = [5, 2];
const ov = g.isOver(w); ok(ov && ov.winner === 0, 'isOver: seat 0 wins at target');
ok(g.isOver(g.setup(players, 0, {})) === null, 'isOver null at start');

// continuous input mappers
ok(g.onPointerMove(0, 60, { height: 120 }).type === 'paddle', 'onPointerMove -> paddle');
ok(g.onKey('ArrowUp').dir === -1 && g.onKey('ArrowDown').dir === 1, 'arrows map to velocity');
ok(g.onKeyUp('ArrowUp').dir === 0, 'key release stops paddle');

// coach mapping (LLM style JSON -> safe policy params + taunt)
ok(typeof g.coach.brief === 'function', 'coach.brief present');
const brief = g.coach.brief(s, 1);
ok(brief.system.includes('JSON') && typeof brief.user === 'string', 'brief returns system+user prompts');
const hi = g.coach.toParams({ skill: 1, aggression: 1, aim: 'corners', taunt: 'Troppo facile!' });
ok(hi.params.react > 0.9 && hi.params.errAmp < 3 && hi.taunt === 'Troppo facile!', 'high-skill style -> sharp params + taunt');
const lo = g.coach.toParams({ skill: 0.1, aim: 'center' });
ok(lo.params.react < 0.6 && lo.params.errAmp > 12, 'low-skill style -> sloppy params');
const junk = g.coach.toParams(null);
ok(junk.params.react > 0 && junk.taunt === null, 'junk style -> safe defaults, no taunt');

// --- gamepad analog stick ---
ok(g.padMode === 'analog', 'pong padMode analog');
ok(g.onAxis(0, 1).dir === 1 && g.onAxis(0, -1).dir === -1 && g.onAxis(0, 0).dir === 0, 'onAxis maps stick Y to paddle velocity');

// --- 3D / fillViewport contract ---
ok(g.renderMode === '3d' && g.fillViewport === true, 'declares a 3D fill-viewport surface');

// --- power-ups, combos, events ---
let pu = g.setup(players, 0, { punti: '5', bonus: true });
ok(pu.bonus === true && Array.isArray(pu.orbs) && pu.orbs.length === 0, 'bonus on, orbs start empty');
ok(pu.fx[0].paddleScale === 1 && pu.fx[1].paddleScale === 1 && pu.combo === 0 && pu.lastHit === -1, 'fx/combo/lastHit initialised');

// a paddle hit sets lastHit + combo and emits a hit event
let hitState = g.setup(players, 0, {}); hitState.ball = { x: 11, y: 60, vx: -130, vy: 0 }; hitState.paddles[0].y = 60;
const h1 = g.tick(hitState, 33);
ok(h1.lastHit === 0 && h1.combo === 1, 'hit sets lastHit=0 and combo=1');
ok(h1.ev.some(e => e.t === 'hit'), 'hit emits a {t:hit} event');

// wall bounce emits a wall event
let wallState = g.setup(players, 0, {}); wallState.ball = { x: 100, y: 4, vx: 0, vy: -130 };
const w2 = g.tick(wallState, 33);
ok(w2.ev.some(e => e.t === 'wall') && w2.ball.vy > 0, 'top wall flips vy and emits wall event');

// shield saves one miss instead of conceding
let sh = g.setup(players, 0, { punti: '5' }); sh.fx[0] = { paddleScale: 1, shield: true };
sh.ball = { x: 9, y: 10, vx: -130, vy: 0 }; sh.paddles[0].y = 110;
let saved = sh; for (let i = 0; i < 4 && saved.scores[1] === 0 && (saved.fx[0] && saved.fx[0].shield); i++) saved = g.tick(saved, 33);
ok(saved.scores.join() === '0,0' && saved.fx[0].shield === false && saved.ball.vx > 0, 'shield consumed, ball reflected, no point conceded');

// orb pickup applies an effect to the last hitter (grow → bigger paddle)
let orbState = g.setup(players, 0, {});
orbState.lastHit = 0; orbState.orbs = [{ id: 7, x: 100, y: 60, kind: 'grow', born: 0 }];
orbState.ball = { x: 100, y: 60, vx: 60, vy: 0 };       // ball sitting on the orb
const og = g.tick(orbState, 33);
ok(og.orbs.length === 0 && og.fx[0].paddleScale > 1, 'grow orb claimed by hitter → paddle scaled up');
ok(og.ev.some(e => e.t === 'orb' && e.kind === 'grow'), 'orb pickup emits an orb event');

// fast orb sets the global ball-speed boost window
let fastState = g.setup(players, 0, {}); fastState.lastHit = 1; fastState.orbs = [{ id: 8, x: 100, y: 60, kind: 'fast', born: 0 }]; fastState.ball = { x: 100, y: 60, vx: 60, vy: 0 };
const of = g.tick(fastState, 33);
ok(of.ballFastUntil > of.clock, 'fast orb opens a ball-speed boost window');

// scoring clears combo + lastHit
let clr = g.setup(players, 0, { punti: '5' }); clr.combo = 5; clr.lastHit = 1; clr.ball = { x: 9, y: 10, vx: -130, vy: 0 }; clr.paddles[0].y = 110;
let after = clr; for (let i = 0; i < 4 && after.scores[1] === 0; i++) after = g.tick(after, 33);
ok(after.combo === 0 && after.lastHit === -1, 'a point resets combo and lastHit');

// coach persona flows through the policy action into state
const cp = g.reduce(g.setup(players, 0, {}), { type: 'policy', seat: 1, params: { react: 0.7 }, persona: 'Il Muro' }, -1);
ok(cp.persona[1] === 'Il Muro', 'policy action carries persona into state for the HUD');

console.log(`\nPong logic: ${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);

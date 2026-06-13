// pong.js — full real-time Pong in 3D. 1v1 humans over the shared P2P transport, or 1-vs-ANIMA.
//
// TWO LAYERS, cleanly split:
//  · LOGIC (this file): pure, host-authoritative physics on a 200×120 field — paddles, ball, scoring,
//    plus power-up orbs, timed paddle effects, rally combos and per-tick EVENTS (st.ev). Deterministic
//    and tiny, so the host broadcasts whole-state snapshots and every peer stays in sync.
//  · RENDER (games/pong3d.js): a Three.js WebGL scene driven from that state — neon arena, glowing ball
//    with trail, particle bursts, camera shake, power-up orbs — plus a procedural Web-Audio SFX engine.
//    Visual/audio reactions are derived locally from st.ev, so nothing extra crosses the wire.
//
// HOW THE AI PLAYS IN REAL TIME: the paddle is moved every tick by a LOCAL predictive controller (zero
// latency). An optional LLM "coach" (Claude / Grok / Groq — whatever teacher.json points at) is called
// BETWEEN POINTS, never per frame, to set the opponent's STYLE (skill/aggression/aim/persona) and talk
// smack. That knowledge lives in `coach` below; the generic hub stays game-agnostic.
import { defineGame } from './shim.mjs';

const FW = 200, FH = 120, PH = 26, PW = 4, R = 3;
const X0 = 8, X1 = FW - 8;                 // paddle planes (left/right)
const BALL = 130, PSPD = 165, MAXB = 270;  // units/sec
const ORB_R = 6, ORB_MIN = 7000, ORB_MAX = 11000, FAST_MUL = 1.32;
const KINDS = ['grow', 'shrink', 'fast', 'shield'];   // power-up flavours
const GEOM = { FW, FH, PH, PW, R, X0, X1 };

const clamp = (v, lo, hi) => v < lo ? lo : v > hi ? hi : v;
function fold(y, lo, hi) { const p = 2 * (hi - lo); let t = ((y - lo) % p + p) % p; return lo + (t <= hi - lo ? t : p - t); }
function rnd(st) { st.rng = (Math.imul(st.rng ^ (st.rng >>> 15), 0x2c1b3c6d) + 0x6d2b79f5) >>> 0; return st.rng / 4294967296; }
const phOf = (st, seat) => PH * ((st.fx[seat] && st.fx[seat].paddleScale) || 1);   // effective paddle half-height source

function policyFor(level) {
  if (level === 'facile') return { react: 0.50, errAmp: 15, anticipation: 0.60, aimBias: 0 };
  if (level === 'difficile') return { react: 0.95, errAmp: 2.5, anticipation: 1.0, aimBias: 1 };
  return { react: 0.76, errAmp: 7, anticipation: 0.85, aimBias: 0.5 };
}

// Where should the AI paddle be heading this tick?
function aiTarget(state, seat) {
  const b = state.ball, pol = state.policy[seat] || policyFor('medio');
  const px = seat === state.seats[0] ? X0 : X1;
  const approaching = (px === X0) ? b.vx < 0 : b.vx > 0;
  let desired = FH / 2;
  if (approaching && b.vx !== 0) {
    const t = (px - b.x) / b.vx;
    if (t > 0) { const raw = b.y + b.vy * t; const folded = fold(raw, R, FH - R); desired = b.y + (folded - b.y) * pol.anticipation; }
  }
  const wob = Math.sin(state.tk * 0.12 + seat * 1.7);   // deterministic "human" wobble
  const ph = phOf(state, seat);
  return clamp(desired + wob * pol.errAmp, ph / 2, FH - ph / 2);
}

export default defineGame({
  id: 'pong',
  name: 'Pong 3D',
  minPlayers: 2,
  maxPlayers: 2,
  aiCapable: true,
  aiCoach: true,         // the hub may attach an LLM coach (see `coach` below)
  realtime: true,
  tickHz: 30,
  renderMode: '3d',      // games/pong3d.js owns a Three.js WebGL surface (harness gives it the raw canvas)
  fillViewport: true,    // use the whole board area, responsive
  capturesPointer: true, // lock the mouse in fullscreen: relative motion drives the paddle (native feel)

  options: [
    { key: 'punti', label: 'Punti per vincere', type: 'select', default: '7', choices: [{ value: '5', label: '5' }, { value: '7', label: '7' }, { value: '11', label: '11' }] },
    { key: 'livello', label: 'Difficoltà', type: 'select', default: 'medio', choices: [{ value: 'facile', label: 'Facile' }, { value: 'medio', label: 'Medio' }, { value: 'difficile', label: 'Difficile' }] },
    { key: 'mente', label: 'Mente ANIMA', type: 'select', default: 'classica', choices: [{ value: 'classica', label: 'Classica (offline)' }, { value: 'llm', label: 'ANIMA-LLM (online)' }] },
    { key: 'bonus', label: 'Power-up', type: 'toggle', default: true },
  ],

  setup(players, seed, options) {
    const o = options || {};
    const aiSeats = players.filter(p => p.ai).map(p => p.seat);
    const level = o.livello || 'medio';
    const policy = {}, persona = {}, fx = {};
    for (const s of aiSeats) policy[s] = policyFor(level);
    for (const p of players) fx[p.seat] = { paddleScale: 1, shield: false };
    const dir = (seed % 2) ? 1 : -1;
    return {
      seats: players.map(p => p.seat), names: players.map(p => p.name), aiSeats,
      paddles: { [players[0].seat]: { y: FH / 2, target: FH / 2, vel: 0 }, [players[1].seat]: { y: FH / 2, target: FH / 2, vel: 0 } },
      ball: { x: FW / 2, y: FH / 2, vx: BALL * dir, vy: 28 }, scores: [0, 0], target: Number(o.punti) || 7,
      policy, persona, level, mente: o.mente || 'classica', tk: 0, humanAvgY: FH / 2,
      // brain: authoritative LLM-mind status set by the hub (null in classic mode). The HUD shows this
      // verbatim — green only when a real Grok/LLM call just succeeded, red when it's down (match blocked).
      brain: null,
      // power-ups & flavour
      bonus: o.bonus !== false, rng: (seed ^ 0x9e3779b9) >>> 0, clock: 0, nextOrbAt: 6000,
      lastHit: -1, combo: 0, orbs: [], fx, ballFastUntil: 0, ev: [],
    };
  },

  reduce(state, action, seat) {
    const p = state.paddles[seat];
    if (action.type === 'paddle' && p) { return patchPaddle(state, seat, { target: clamp(action.y, phOf(state, seat) / 2, FH - phOf(state, seat) / 2), vel: 0 }); }
    if (action.type === 'vel' && p) { return patchPaddle(state, seat, { vel: action.dir | 0 }); }
    if (action.type === 'policy' && action.seat != null) {
      const policy = { ...state.policy, [action.seat]: { ...(state.policy[action.seat] || {}), ...action.params } };
      const persona = action.persona != null ? { ...state.persona, [action.seat]: action.persona } : state.persona;
      return { ...state, policy, persona };
    }
    if (action.type === 'brain') { return { ...state, brain: action.brain || null }; }
    return state;
  },

  tick(state, dtMs) {
    const s = dtMs / 1000;
    const st = { ...state, paddles: { ...state.paddles }, ball: { ...state.ball }, scores: state.scores.slice(), fx: { ...state.fx }, orbs: state.orbs.slice(), tk: state.tk + 1, clock: state.clock + dtMs, ev: [] };
    // expire timed paddle effects
    for (const seatStr of Object.keys(st.fx)) {
      const f = st.fx[seatStr]; if (f && f.scaleUntil && st.clock >= f.scaleUntil) st.fx[seatStr] = { ...f, paddleScale: 1, scaleUntil: 0 };
    }
    // paddles
    for (const seatStr of Object.keys(st.paddles)) {
      const seat = Number(seatStr); const p = { ...st.paddles[seat] }; const ph = phOf(st, seat);
      if (st.aiSeats.includes(seat)) {
        const want = aiTarget(st, seat); const step = PSPD * (st.policy[seat] ? st.policy[seat].react : 0.76) * s;
        p.y += clamp(want - p.y, -step, step);
      } else if (p.vel) { p.y = clamp(p.y + p.vel * PSPD * s, ph / 2, FH - ph / 2); }
      else { p.y += clamp(p.target - p.y, -PSPD * 1.8 * s, PSPD * 1.8 * s); }
      p.y = clamp(p.y, ph / 2, FH - ph / 2);
      st.paddles[seat] = p;
    }
    // track the human's paddle position (feeds the coach's "read your style")
    const human = st.seats.find(x => !st.aiSeats.includes(x));
    if (human != null) st.humanAvgY = st.humanAvgY * 0.98 + st.paddles[human].y * 0.02;
    // ball (a fast power-up scales integration speed without touching stored velocity, so it reverts clean)
    const b = st.ball, mul = st.clock < st.ballFastUntil ? FAST_MUL : 1;
    b.x += b.vx * s * mul; b.y += b.vy * s * mul;
    if (b.y < R) { b.y = R; b.vy = -b.vy; st.ev.push({ t: 'wall', y: b.y }); }
    if (b.y > FH - R) { b.y = FH - R; b.vy = -b.vy; st.ev.push({ t: 'wall', y: b.y }); }
    // power-up orbs: spawn + pickup
    if (st.bonus) {
      if (st.clock >= st.nextOrbAt && st.orbs.length < 2 && st.scores[0] + st.scores[1] >= 0) {
        const kind = KINDS[(rnd(st) * KINDS.length) | 0];
        st.orbs.push({ id: (st.rng & 0xffff), x: FW * (0.32 + rnd(st) * 0.36), y: R * 4 + rnd(st) * (FH - R * 8), kind, born: st.clock });
        st.nextOrbAt = st.clock + ORB_MIN + rnd(st) * (ORB_MAX - ORB_MIN);
      }
      for (let i = st.orbs.length - 1; i >= 0; i--) {
        const o = st.orbs[i];
        if (Math.hypot(b.x - o.x, b.y - o.y) <= R + ORB_R && st.lastHit >= 0) { applyOrb(st, o, st.lastHit); st.orbs.splice(i, 1); }
        else if (st.clock - o.born > 14000) st.orbs.splice(i, 1);   // unclaimed orbs fade after a while
      }
    }
    // paddle collisions / scoring (shield can save one miss)
    const s0 = st.seats[0], s1 = st.seats[1];
    if (b.vx < 0 && b.x <= X0 + PW / 2 + R && b.x > X0 - 10) {
      if (Math.abs(b.y - st.paddles[s0].y) <= phOf(st, s0) / 2 + R) bounce(b, st.paddles[s0], +1, st, s0);
      else if (b.x < X0 - 4) concede(st, s0, 1, +1);
    } else if (b.vx > 0 && b.x >= X1 - PW / 2 - R && b.x < X1 + 10) {
      if (Math.abs(b.y - st.paddles[s1].y) <= phOf(st, s1) / 2 + R) bounce(b, st.paddles[s1], -1, st, s1);
      else if (b.x > X1 + 4) concede(st, s1, 0, -1);
    }
    return st;
  },

  isOver(state) {
    const [a, b] = state.scores;
    if (a >= state.target) return { winner: state.seats[0] };
    if (b >= state.target) return { winner: state.seats[1] };
    return null;
  },

  // Human controls: drag (pointer), arrow keys (hold), or analog stick / D-pad.
  onPointerMove(x, y, api) { return { type: 'paddle', y: (y / api.height) * FH }; },
  onKey(key) { if (key === 'ArrowUp' || key === 'w') return { type: 'vel', dir: -1 }; if (key === 'ArrowDown' || key === 's') return { type: 'vel', dir: 1 }; return null; },
  onKeyUp(key) { if (['ArrowUp', 'ArrowDown', 'w', 's'].includes(key)) return { type: 'vel', dir: 0 }; return null; },
  padMode: 'analog',
  onAxis(x, y) { return { type: 'vel', dir: y > 0 ? 1 : y < 0 ? -1 : 0 }; },   // stick/D-pad up-down drives the paddle

  ai() { return null; },   // movement is continuous in tick(); no per-turn action needed

  // ── 3D render surface (delegates to games/pong3d.js; lazy-loads Three.js) ─────────────────
  mount(canvas, api) { ensureRenderer(canvas, api); },
  render(api, state) { if (R3D) R3D.frame(state, api); },
  resize(w, h) { if (R3D) R3D.resize(w, h); },
  unmount() { if (R3D) { try { R3D.dispose(); } catch {} R3D = null; } },

  // ── LLM coach hooks (used by the hub only when "Mente: ANIMA-LLM" is chosen) ──────────────
  // The hub calls coach.brief() to get the prompt, sends it to the best LLM between points, then feeds
  // the model's JSON to coach.toParams() to update the AI's style + a chat taunt. The LLM NEVER moves
  // the paddle — it only tunes skill/aggression/aim and talks. That's what makes an LLM "play" Pong.
  coach: {
    brief(state, aiSeat) {
      const human = state.seats.find(x => x !== aiSeat);
      const hi = state.seats.indexOf(human), me = state.seats.indexOf(aiSeat);
      const tendency = state.humanAvgY < FH * 0.4 ? 'tende a stare in alto' : state.humanAvgY > FH * 0.6 ? 'tende a stare in basso' : 'sta al centro';
      return {
        system: "Sei l'allenatore di un avversario IA a Pong in tempo reale. NON muovi tu la racchetta: imposti solo lo STILE di gioco, che un controller locale eseguirà a 60fps. "
          + "Rispondi SOLO con JSON: {\"skill\":0..1,\"aggression\":0..1,\"aim\":\"corners\"|\"center\"|\"mirror\",\"persona\":\"breve\",\"taunt\":\"frase brevissima in italiano\"}. "
          + "skill alto = reazioni rapide e precise; aggression alto = colpi più forti e angolati; aim=corners mira agli angoli, center al centro, mirror specchia l'avversario. Adatta lo stile per essere competitivo ma divertente.",
        user: `Punteggio ${state.scores[me]}-${state.scores[hi]} su ${state.target}. Combo attuale ${state.combo}. L'avversario umano ${tendency}. Imposta lo stile per il prossimo scambio e lancia una battuta.`,
      };
    },
    // Map the model's style JSON to concrete policy params + a chat line. Always returns safe values.
    toParams(style) {
      const sk = clamp(Number(style && style.skill), 0, 1) || 0.7;
      const ag = clamp(Number(style && style.aggression), 0, 1) || 0.5;
      const aimMap = { corners: 1, center: 0, mirror: 0.5 };
      return {
        params: { react: 0.45 + sk * 0.5, errAmp: 16 - sk * 14, anticipation: 0.6 + sk * 0.4, aimBias: (aimMap[style && style.aim] != null ? aimMap[style.aim] : 0.5) * (0.5 + ag) },
        taunt: (style && style.taunt) ? String(style.taunt).slice(0, 120) : null,
        persona: (style && style.persona) ? String(style.persona).slice(0, 60) : null,
      };
    },
  },
});

// ── renderer bootstrap (module-scope: one active match at a time) ─────────────────────────────
let R3D = null, r3dLoading = false;
async function ensureRenderer(canvas, api) {
  if (R3D || r3dLoading) return;
  r3dLoading = true;
  try {
    const mod = await import('/apps/games/games/pong3d.js');
    R3D = await mod.createPongRenderer(canvas, api, GEOM);
    // If the renderer finished loading while the board is already on-screen, size it to the container
    // now (its initial fit may have seen a hidden 0×0 canvas during the waiting room).
    const wrap = canvas.parentElement; if (wrap) { const r = wrap.getBoundingClientRect(); if (r.width > 1 && r.height > 1) R3D.resize(r.width, r.height); }
  } catch (e) { console.error('pong3d load failed', e); }
  finally { r3dLoading = false; }
}

function patchPaddle(state, seat, patch) { return { ...state, paddles: { ...state.paddles, [seat]: { ...state.paddles[seat], ...patch } } }; }

function bounce(b, paddle, dir, st, seat) {
  b.x = (dir > 0 ? X0 + PW / 2 + R : X1 - PW / 2 - R);
  const off = (b.y - paddle.y) / (phOf(st, seat) / 2);    // -1..1 hit offset
  const aim = st.aiSeats.includes(seat) ? (st.policy[seat] ? st.policy[seat].aimBias || 0 : 0) : 0;
  const speed = Math.min(MAXB, Math.hypot(b.vx, b.vy) * 1.05);
  const angle = (off * 0.9 + (aim ? Math.sign(off || 1) * aim * 0.4 : 0));
  b.vx = dir * speed * Math.cos(angle * 0.8);
  b.vy = speed * Math.sin(angle * 0.9) + off * 18;
  if (Math.abs(b.vx) < 60) b.vx = dir * 60;               // never stall horizontally
  st.lastHit = seat; st.combo = (st.combo || 0) + 1;
  st.ev.push({ t: 'hit', seat, x: b.x, y: b.y, speed, combo: st.combo, off });
}

// A ball that got past `seat`'s plane: consume a shield (one save) or award the point to `who`.
function concede(st, seat, who, dir) {
  const f = st.fx[seat];
  if (f && f.shield) {
    st.fx[seat] = { ...f, shield: false };
    const b = st.ball, speed = Math.hypot(b.vx, b.vy);
    b.x = (dir > 0 ? X0 + PW / 2 + R : X1 - PW / 2 - R); b.vx = dir * Math.max(60, Math.abs(b.vx) || speed);
    st.ev.push({ t: 'shield', seat, x: b.x, y: b.y });
    return;
  }
  score(st, who);
}

function applyOrb(st, o, seat) {
  const other = st.seats.find(x => x !== seat);
  if (o.kind === 'grow') st.fx[seat] = { ...st.fx[seat], paddleScale: 1.6, scaleUntil: st.clock + 8000 };
  else if (o.kind === 'shrink') st.fx[other] = { ...st.fx[other], paddleScale: 0.6, scaleUntil: st.clock + 8000 };
  else if (o.kind === 'fast') st.ballFastUntil = st.clock + 6000;
  else if (o.kind === 'shield') st.fx[seat] = { ...st.fx[seat], shield: true };
  st.ev.push({ t: 'orb', kind: o.kind, seat, x: o.x, y: o.y });
}

function score(st, who) {
  st.scores[who]++;
  const dir = who === 0 ? -1 : 1;                          // serve toward the loser
  st.ev.push({ t: 'score', who, x: st.ball.x, y: st.ball.y });
  st.ball = { x: FW / 2, y: FH / 2, vx: BALL * dir, vy: ((st.scores[0] + st.scores[1]) % 2 ? 1 : -1) * 30 };
  st.combo = 0; st.lastHit = -1; st.ballFastUntil = 0;
}

// constellations.js — "Costellazioni 3D": the web Game Center continuation of the Cardputer space
// game. It CONTINUES THE SAME RUN: loads /sd/data/costellazioni/save.bin via the firmware endpoint
// (constellations-save.js), regenerates the current sector from (seed, sector) with the SHARED
// deterministic generator (constellations-gen.js, byte-identical to the firmware), runs a real-time
// first-person dogfight rendered in Three.js (constellations-3d.js), and writes credits/kills back to
// the save — so the campaign is shared both ways with the Cardputer.
//
// Single-player, real-time. The harness gives us the canvas + input (keyboard/mouse/gamepad). Campaign
// numbers live in the module-global RUN (the save); the harness `state` carries only the live phase +
// dogfight sim (pure tick/reduce). On debrief we mutate RUN and POST it (epoch/sector-merge guarded).
import { defineGame } from '/apps/games/nucleo-game.js';
import CONTENT from '/apps/games/games/constellations-content.js';
import { genSector, genMissions, beaconsPerSector, F_ECO } from '/apps/games/games/constellations-gen.js';
import { loadSave, storeSave, newSave } from '/apps/games/games/constellations-save.js';
import { unitBuy, unitSell, refuelPrice, jumpCost, sysDist, cargoUsed, beaconsLit, beaconsTotal } from '/apps/games/games/constellations-econ.js';
import { SHOP, shopMaxed, shopCost, shopBuy, repairCost } from '/apps/games/games/constellations-shop.js';

const G_RELIQ = 6, F_CUSTODI = 1;   // relic good index / Keepers faction (beacon relight bookkeeping)
let conflictPending = null;          // set by persist() when the device advanced the run (-> conflict screen)
let BOOTED = false;                  // true once the initial loadSave settles (continue vs new-run)

// ---- shared module state (one active run at a time) --------------------------------------------
let RUN = null;            // the campaign save (credits, hull, seed, sector, ...)
let SECTOR = null;         // genSector(RUN.seed, RUN.sector) — the 10 procedural systems
let MISSIONS = null;       // genMissions at the current system
let SAVING = false;        // a POST is in flight
let DIRTY = false;         // RUN mutated again while a POST was in flight -> flush once it lands
const MT = { PATROL: 0, BOUNTY: 1, ESCORT: 2, DEFEND: 3 };
const MT_NAME = [['Pattuglia', 'Patrol'], ['Taglia', 'Bounty'], ['Scorta', 'Escort'], ['Difesa', 'Defend']];

function rebuildSector() {
  SECTOR = genSector(RUN.seed, RUN.sector);
  if (RUN.sys < 0 || RUN.sys >= SECTOR.length) RUN.sys = 0;
  CONTENT.systems = SECTOR;              // feed the economy module the live sector (for trade screens)
  MISSIONS = genMissions(RUN.seed, RUN.sector, RUN.sys, SECTOR[RUN.sys].faction);
}
async function bootRun() {
  RUN = await loadSave();                 // continue the Cardputer run, or null -> offer a New Run
  BOOTED = true;
  if (RUN) rebuildSector();
}
function startNewRun() { RUN = newSave(); rebuildSector(); persist(); }
async function persist() {
  DIRTY = true;                          // mark the latest RUN as needing a write
  if (SAVING) return;                    // a POST is already running; it will flush the new delta below
  SAVING = true;
  try {
    while (DIRTY) {                       // re-POST until the in-RAM RUN matches what we last sent
      DIRTY = false;
      const r = await storeSave(RUN);
      if (r && r.conflict && r.disk) { RUN = r.disk; rebuildSector(); conflictPending = r.disk; break; }
    }
  } catch (e) { console.warn('[costellazioni] save failed', e); }
  finally { SAVING = false; }
}

// ---- combat tuning (mirrors the Cardputer rail shooter, in normalized space) -------------------
const ZFAR = 260, ZNEAR = 6, AIM_SPD = 1.7;        // reticle units/sec (screen is [-1..1])
export const FOCAL = 25;                           // shared projection: foes spread to the rim as they near
export const project = (ex, ey, ez) => { const inv = FOCAL / ez; return { sx: ex * inv, sy: ey * inv, scale: inv }; };
function combatFromMission(m) {
  return { type: m.type, foeFac: m.foe_fac, waves: m.waves, perWave: m.per_wave,
    foeHp: m.foe_hp, foeDmg: m.foe_dmg, foeSpeed: m.foe_speed_pml * 0.1, ace: m.ace,
    rewardCr: m.reward_cr, killCr: m.kill_cr, repFac: m.offer_fac, repGain: m.rep_gain,
    enemyRepFac: m.foe_fac, enemyRepLoss: m.enemy_rep_loss, mission: true };
}
function ambushCfg() {
  const tier = 1 + Math.min(12, RUN.sector);
  return { type: MT.PATROL, foeFac: 2, waves: 2 + (tier >= 6 ? 1 : 0), perWave: 2 + (tier & 1), foeHp: 30 + tier * 6,
    foeDmg: 7 + tier, foeSpeed: 7.8 + tier * 0.4, ace: 0, rewardCr: 0, killCr: 18 + tier * 4,
    repFac: -1, repGain: 0, enemyRepFac: 2, enemyRepLoss: 2, mission: false };
}
function spawnWave(s) {
  const cc = s.cc, last = s.wavesLeft === 1;
  for (let i = 0; i < cc.perWave; i++) s.foes.push(mkFoe(cc, 0));
  if (last && cc.ace) s.foes.push(mkFoe(cc, 3));
  s.wave++; s.wavesLeft--; s.spawnTimer = 1300;
  s.banner = 'WAVE ' + s.wave + '/' + cc.waves; s.bannerUntil = s.clock + 1400;
}
function mkFoe(cc, kind) {
  const hp = kind === 3 ? cc.foeHp * 2.2 + 30 : (kind === 1 ? cc.foeHp * 0.6 : (kind === 2 ? cc.foeHp * 1.8 + 20 : cc.foeHp));
  const engageZ = 36 + (kind === 2 ? 20 : kind === 1 ? -2 : 8) + Math.random() * 26;   // varied depth -> a real 3D battlefield
  const hx = Math.random() * 2.8 - 1.4, hy = Math.random() * 1.5 - 0.75;               // wide home "lanes" — foes fill the screen (Wing Commander), never bunch centre
  return { id: (FOE_ID = (FOE_ID + 1) >>> 0), ex: hx, ey: hy, hx, hy, ez: 150 + Math.random() * 80,   // id -> missiles can home a specific ship
    wphase: Math.random() * 6.28, hp, hpmax: hp, fireCd: 800 + Math.random() * 900,
    strafeCd: 2800 + Math.random() * 3500, engageZ, strafe: 0, kind };
}
function maybeDrop(s, f) {
  if (Math.random() < 0.16) {                                      // ~1 in 6 kills drops a power-up
    const r = Math.random(), kind = r < 0.45 ? 'missile' : r < 0.75 ? 'shield' : 'repair';   // missile favoured (the scarce resource)
    s.pickups.push({ ex: f.ex, ey: f.ey, ez: f.ez, kind, life: 6000 });
  }
}
function startCombat(state, cc) {
  return { ...state, phase: 'combat', cc, foes: [], bolts: [], parts: [], pmiss: [], pickups: [],
    wave: 0, wavesLeft: cc.waves, spawnTimer: 0, clock: 0,
    aimx: 0, aimy: 0, ah: 0, av: 0, fire: false, fireCd: 0, lock: -1,
    missiles: 3, missileCd: 0, launchMissile: false,                 // homing interceptors (limited; refill via pickups)
    shield: RUN.shield_max, shieldMax: RUN.shield_max, hull: RUN.hull, hullMax: RUN.hull_max, kills: 0, shake: 0,
    banner: '', bannerUntil: 0, result: 0, ev: [] };
}
let FOE_ID = 0;

// ---- bridge (pre-flight) actions ---------------------------------------------------------------
export default defineGame({
  id: 'constellations', name: 'Costellazioni 3D',
  minPlayers: 1, maxPlayers: 1, aiCapable: false, realtime: true, tickHz: 30,
  renderMode: '3d', fillViewport: true, capturesPointer: true, pointerAxes: 'xy', category: 'arcade',
  // The host (index.html) reads this to lock the pointer ONLY in the dogfight — the hub menu keeps the
  // system cursor visible/clickable. Falls back to the static capturesPointer for other games.
  wantsPointerLock(s) { return !!s && s.phase === 'combat' && !s.result; },

  setup() {
    return {
      phase: RUN ? 'hub' : (BOOTED ? 'new_run' : 'loading'),
      screen: 'bridge', focus: { bridge: 0, map: 0, market: 0, shipyard: 0, missions: 0 },
      marketCol: 0, marketQty: 1, target: -1, padArmed: true,
      toast: null, flash: null, sel: 0, clock: 0, ev: [],
    };
  },

  reduce(state, action) {
    const a = action || {};
    const p = state.phase;
    if (p === 'loading') return state;
    if (p === 'new_run') { if (a.type === 'confirm') { startNewRun(); return enterScreen({ ...state, phase: 'hub' }, 'bridge'); } return state; }
    if (p === 'conflict') { if (a.type === 'confirm' || a.type === 'back') return enterScreen({ ...state, phase: 'hub', toast: null }, 'bridge'); return state; }
    if (p === 'combat' && !state.result) return combatReduce(state, a);
    if (p === 'debrief') { if (['confirm', 'back', 'fire', 'click'].includes(a.type)) return enterScreen({ ...state, phase: 'hub' }, 'bridge'); return state; }
    if (p === 'hub') return hubReduce(state, a);
    return state;
  },

  tick(state, dtMs) {
    if (state.phase === 'loading') { if (RUN) return enterScreen({ ...state, phase: 'hub' }, 'bridge'); if (BOOTED) return { ...state, phase: 'new_run' }; return state; }
    if (conflictPending && state.phase === 'hub') { conflictPending = null; return { ...state, phase: 'conflict', toast: { text: 'Il Cardputer ha fatto avanzare la run', kind: 'warn' } }; }
    if (state.phase === 'combat' && !state.result) return stepCombat(state, dtMs);
    if (state.toast && state.toast.until && (state.clock || 0) + dtMs > state.toast.until) return { ...state, clock: (state.clock || 0) + dtMs, toast: null };
    return { ...state, clock: (state.clock || 0) + dtMs };
  },

  isOver() { return null; },   // endless campaign; never "over" from the harness' perspective

  onKey(key) { return mapKey(key); },
  onKeyUp(key) {
    if (['ArrowLeft', 'a', 'ArrowRight', 'd'].includes(key)) return { type: 'navrel', k: 'H' };
    if (['ArrowUp', 'w', 'ArrowDown', 's'].includes(key)) return { type: 'navrel', k: 'V' };
    return null;
  },
  onPointerMove(x, y, api) { return { type: 'aim', x: (x / api.width) * 2 - 1, y: (y / api.height) * 2 - 1 }; },   // mouse = free aim (both axes), pointer-locked in combat
  onPointer() { return { type: 'click' }; },                                            // canvas click = fire (combat only)
  padMode: 'analog',
  onAxis(x, y) { return { type: 'pad', x, y }; },
  onPadDir(dx, dy) { return { type: 'navkey', k: dx > 0 ? 'R' : dx < 0 ? 'L' : dy > 0 ? 'D' : 'U' }; },
  onPadButton(name) { return name === 'A' ? { type: 'confirm' } : name === 'B' ? { type: 'back' } : name === 'X' ? { type: 'col' } : name === 'Y' ? { type: 'refuel' } : null; },

  mount(canvas, api) { ensureRenderer(canvas, api); if (!RUN && !BOOTED) bootRun(); },
  render(api, state) {
    // leaving the dogfight -> hand the system cursor back for the hub menu (the host only re-locks in combat)
    if (state && state.phase !== 'combat' && typeof document !== 'undefined' && document.pointerLockElement) { try { document.exitPointerLock(); } catch {} }
    if (R3D) R3D.frame(state, MODEL(), api);
  },
  resize(w, h) { if (R3D) R3D.resize(w, h); },
  unmount() { if (R3D) { try { R3D.dispose(); } catch {} R3D = null; } },
});

// The live model handed to the renderer/UI (run + generated sector/missions + econ/shop helpers).
function MODEL() {
  return { run: RUN, sector: SECTOR, missions: MISSIONS, MT_NAME, CONTENT,
    econ: { unitBuy, unitSell, refuelPrice, jumpCost, sysDist, cargoUsed, beaconsLit, beaconsTotal, beaconsPerSector },
    shop: { SHOP, shopMaxed, shopCost, repairCost } };
}

// ---- input vocabulary (one keymap, phase-interpreted) ------------------------------------------
function mapKey(k) {
  if (typeof k === 'string') {
    if (k.startsWith('Hover:')) { const a = k.split(':'); return { type: 'hover', screen: a[1], i: +a[2] }; }
    if (k.startsWith('Tgt:')) return { type: 'mapTarget', i: +k.slice(4) };
    if (k.startsWith('Tab:')) return { type: 'screen', to: +k.slice(4) };
    if (k.startsWith('Buy:')) return { type: 'buy', g: +k.slice(4) };     // market: click the buy cell
    if (k.startsWith('Sell:')) return { type: 'sell', g: +k.slice(5) };   // market: click the sell cell
  }
  if (k === 'ArrowLeft' || k === 'a') return { type: 'navkey', k: 'L' };
  if (k === 'ArrowRight' || k === 'd') return { type: 'navkey', k: 'R' };
  if (k === 'ArrowUp' || k === 'w') return { type: 'navkey', k: 'U' };
  if (k === 'ArrowDown' || k === 's') return { type: 'navkey', k: 'D' };
  if (k === ' ' || k === 'Enter' || k === 'j') return { type: 'confirm' };
  if (k === 'Escape' || k === 'q' || k === 'Backspace') return { type: 'back' };
  if (k >= '1' && k <= '5') return { type: 'screen', to: +k - 1 };
  if (k === '[') return { type: 'tab', d: -1 };
  if (k === ']') return { type: 'tab', d: 1 };
  if (k === ',') return { type: 'qty', d: -1 };
  if (k === '.') return { type: 'qty', d: 1 };
  if (k === '<') return { type: 'qty', d: -5 };
  if (k === '>') return { type: 'qty', d: 5 };
  if (k === 'r') return { type: 'refuel' };
  if (k === 'x') return { type: 'col' };
  if (k === 'm' || k === 'Shift') return { type: 'missile' };   // secondary weapon: homing interceptor
  return null;
}
function combatReduce(state, a) {
  const t = a.type;
  if (t === 'navkey') { const k = a.k; if (k === 'L') return { ...state, ah: -1, ahU: state.clock + 150 }; if (k === 'R') return { ...state, ah: 1, ahU: state.clock + 150 }; if (k === 'U') return { ...state, av: -1, avU: state.clock + 150 }; if (k === 'D') return { ...state, av: 1, avU: state.clock + 150 }; }
  if (t === 'navrel') { if (a.k === 'H') return { ...state, ah: 0 }; if (a.k === 'V') return { ...state, av: 0 }; }
  if (t === 'pad') return { ...state, ah: a.x, av: a.y, ahU: state.clock + 150, avU: state.clock + 150 };
  if (t === 'aim') { const ns = { ...state, aimy: a.y, usedMouse: true }; if (a.x != null) ns.aimx = Math.max(-1, Math.min(1, a.x)); return ns; }   // usedMouse -> lighter aim assist (precise mouse wins)
  if (t === 'missile' || t === 'col') return { ...state, launchMissile: true };   // secondary fire (key m/x, pad X)
  if (t === 'confirm' || t === 'click') return { ...state, fire: true };
  if (t === 'back') return endCombat(state, -1);
  return state;
}

// ---- combat simulation (pure-ish on `state`) ---------------------------------------------------
function foeSpeedMul(k) { return k === 1 ? 1.55 : k === 2 ? 0.72 : k === 3 ? 1.3 : 1.0; }
function stepCombat(state, dtMs) {
  const dt = Math.min(0.05, dtMs / 1000), ms = dtMs;
  const s = { ...state, clock: state.clock + dtMs, foes: state.foes.map(f => ({ ...f })),
    bolts: state.bolts.filter(b => b.life > 0).map(b => ({ ...b })), parts: state.parts.filter(p => p.life > 0).map(p => ({ ...p })),
    pmiss: (state.pmiss || []).filter(m => m.life > 0).map(m => ({ ...m })),
    pickups: (state.pickups || []).filter(pk => pk.life > 0).map(pk => ({ ...pk })), ev: [] };
  const prevFoe = (state.lock >= 0 && state.lock < s.foes.length) ? s.foes[state.lock] : null;   // lock identity by REFERENCE (the foes array is compacted below, so indices shift)
  // reticle steering (intent windows) + mouse absolute already set in reduce
  if (s.clock < (s.ahU || 0)) s.aimx += s.ah * AIM_SPD * dt;
  if (s.clock < (s.avU || 0)) s.aimy += s.av * AIM_SPD * dt;
  s.aimx = Math.max(-1, Math.min(1, s.aimx)); s.aimy = Math.max(-0.85, Math.min(0.85, s.aimy));
  if (s.shake > 0) s.shake = Math.max(0, s.shake - dt * 2.4);
  // shield regen — slower recharge so damage matters (the player can't dodge; shield is the skill buffer)
  if (s.shield < s.shieldMax && s.clock - (s.shieldHit || -9999) > 1500) s.shield = Math.min(s.shieldMax, s.shield + 17 * dt);
  // enemy AI: a real dogfight. Foes approach to an engage distance and HOLD there, weaving and firing;
  // they die ONLY to player fire (no free fly-through win). Occasionally a foe makes a strafing run —
  // dives to point-blank, deals a hit, then retreats — for pressure. You must kill them all to advance.
  const closeF = 30 + s.cc.foeSpeed * 0.3;
  for (const f of s.foes) {
    const spd = closeF * foeSpeedMul(f.kind);
    f.wphase += dt * (f.kind === 1 ? 2.9 : f.kind === 3 ? 2.5 : 1.6);
    if (f.strafe) {                                                  // diving in for a close pass
      f.ez -= spd * 2.4 * dt;
      if (f.ez <= ZNEAR) { hurt(s, s.cc.foeDmg + 3); s.shake = 1.0; s.ev.push({ t: 'pass', ex: f.ex, ey: f.ey }); f.ez = 90 + Math.random() * 20; f.strafe = 0; f.strafeCd = 3200 + Math.random() * 3500; }
    } else {
      if (f.ez > f.engageZ) f.ez = Math.max(f.engageZ, f.ez - spd * dt);   // close to engage range…
      else f.ez = f.engageZ + Math.sin(f.wphase * 0.6) * 7;                // …then bob around it (alive, not a sitting duck)
      f.strafeCd -= ms;
      if (f.strafeCd <= 0 && f.ez < f.engageZ + 16) f.strafe = 1;          // launch a strafing run
    }
    const amp = (0.22 + (130 - Math.min(130, f.ez)) * 0.0016) * (f.kind === 1 ? 1.8 : f.kind === 2 ? 0.6 : 1);
    f.ex += ((f.hx || 0) + Math.sin(f.wphase) * amp - f.ex) * 1.4 * dt;            // weave around the home lane, not screen centre
    f.ey += ((f.hy || 0) + Math.sin(f.wphase * 0.7 + 1) * amp * 0.55 - f.ey) * 1.4 * dt;
    f.fireCd -= ms;
    if (f.fireCd <= 0 && f.ez < 130 && !f.strafe) { s.bolts.push({ ex: f.ex, ey: f.ey, ez: f.ez, life: 1000 }); f.fireCd = (f.kind === 3 ? 800 : 1300) + Math.random() * 600; }   // fiercer fire
  }
  s.foes = s.foes.filter(f => f.hp > 0);   // foes leave the field ONLY by dying to the player
  // enemy bolts converge to camera centre
  for (const b of s.bolts) { b.ez -= (b.ez / 0.9) * dt; b.life -= ms; if (b.ez < ZNEAR) { b.life = 0; hurt(s, s.cc.foeDmg); s.shake = 0.6; } }
  // lock-on: nearest foe to reticle, radius grows with the target's on-screen size (like the native),
  // with hysteresis (by foe IDENTITY, not index) so an acquired lock sticks rather than flickering.
  s.lock = -1; let best = Infinity;
  for (let i = 0; i < s.foes.length; i++) {
    const f = s.foes[i]; const sc = FOCAL / f.ez; const sx = f.ex * sc, sy = f.ey * sc;
    const dx = sx - s.aimx, dy = sy - s.aimy, dd = dx * dx + dy * dy;
    const base = 0.16 + 0.42 * sc;                       // floor + grows as the foe nears (mirror of native 10+26*sc px)
    const r = (f === prevFoe) ? base * 1.5 : base;       // hysteresis: keep the SAME ship locked longer
    if (dd < r * r && dd < best) { best = dd; s.lock = i; }
  }
  if (s.lock >= 0 && s.foes[s.lock] !== prevFoe) s.ev.push({ t: 'lock' });   // edge -> renderer pulses the box + sound
  // aim assist: glide toward the locked foe. Keyboard idle = full glide onto target; once the player has
  // touched the MOUSE, only a light sticky nudge so their precise absolute aim always wins.
  if (s.lock >= 0) {
    const f = s.foes[s.lock]; const sc = FOCAL / f.ez, tx = f.ex * sc, ty = f.ey * sc;
    const kbSteer = s.clock < (s.ahU || 0) || s.clock < (s.avU || 0);
    const k = s.usedMouse ? 0.05 : (kbSteer ? 0.04 : Math.min(0.3, 4 * dt));   // lighter assist -> more skill
    s.aimx += (tx - s.aimx) * k; s.aimy += (ty - s.aimy) * k;
  }
  // player fire (hitscan on lock)
  s.fireCd -= ms;
  if (s.fire && s.fireCd <= 0 && s.lock >= 0) {
    s.fireCd = Math.max(110, 240 - RUN.weapon * 28); s.muz = s.clock + 80;
    const f = s.foes[s.lock]; f.hp -= 18 + RUN.weapon * 8;
    s.ev.push({ t: 'laser', ex: f.ex, ey: f.ey, ez: f.ez });
    if (f.hp <= 0) { s.kills++; s.lock = -1; s.shake = f.kind === 3 ? 0.7 : 0.4; s.ev.push({ t: 'boom', ex: f.ex, ey: f.ey, ez: f.ez, big: f.kind === 3 }); maybeDrop(s, f); }
    else { f.hitAt = s.clock; s.ev.push({ t: 'hit', ex: f.ex, ey: f.ey, ez: f.ez }); }   // hitAt -> renderer white-flashes the struck ship
  }
  s.fire = false;
  // homing interceptors: launch at the locked foe (limited ammo), then fly out and track it
  s.missileCd -= ms;
  if (s.launchMissile && s.missiles > 0 && s.missileCd <= 0 && s.lock >= 0) {
    s.missiles--; s.missileCd = 450; const f = s.foes[s.lock];
    s.pmiss.push({ ex: s.aimx * 0.4, ey: s.aimy * 0.4, ez: 14, tid: f.id, life: 4000 });
    s.ev.push({ t: 'mfire' });
  }
  s.launchMissile = false;
  for (const m of s.pmiss) {
    m.life -= ms; m.ez += 230 * dt;
    const tf = s.foes.find(ff => ff.id === m.tid && ff.hp > 0);
    if (tf) {
      m.ex += (tf.ex - m.ex) * Math.min(1, 7 * dt); m.ey += (tf.ey - m.ey) * Math.min(1, 7 * dt);
      if (m.ez >= tf.ez - 5) { tf.hp -= 70 + RUN.weapon * 10; m.life = 0; s.ev.push({ t: 'boom', ex: tf.ex, ey: tf.ey, ez: tf.ez, big: true }); if (tf.hp <= 0) { s.kills++; s.shake = 0.7; maybeDrop(s, tf); } }
    } else if (m.ez > ZFAR) m.life = 0;     // target gone -> the missile flies off and expires
  }
  s.pmiss = s.pmiss.filter(m => m.life > 0);
  // power-up drops drift toward the player and AUTO-COLLECT at the near plane (no aiming needed)
  for (const pk of s.pickups) {
    pk.ez -= 64 * dt; pk.life -= ms;
    pk.ex += (0 - pk.ex) * Math.min(1, 1.2 * dt); pk.ey += (0 - pk.ey) * Math.min(1, 1.2 * dt);
    if (pk.ez <= ZNEAR) {
      if (pk.kind === 'missile') s.missiles = Math.min(5, s.missiles + 1);
      else if (pk.kind === 'shield') s.shield = s.shieldMax;
      else if (pk.kind === 'repair') s.hull = Math.min(s.hullMax, s.hull + 30);
      s.ev.push({ t: 'pickup', kind: pk.kind });
      s.banner = pk.kind === 'missile' ? 'MISSILE +1' : pk.kind === 'shield' ? 'SCUDO CARICO' : 'SCAFO RIPARATO'; s.bannerUntil = s.clock + 1100;
      pk.life = 0;
    }
  }
  s.pickups = s.pickups.filter(pk => pk.life > 0);
  s.foes = s.foes.filter(f => f.hp > 0);
  if (s.result) return s;
  // wave flow / win
  if (s.foes.length === 0) {
    if (s.wavesLeft > 0) { s.spawnTimer -= ms; if (s.spawnTimer <= 0) spawnWave(s); }
    else return endCombat(s, 1);
  }
  return s;
}
function hurt(s, dmg) {
  s.shieldHit = s.clock;
  if (s.ev) s.ev.push({ t: 'hurt' });                            // -> renderer plays the damage thud
  if (s.shield > 0) { const a = Math.min(s.shield, dmg); s.shield -= a; dmg -= a; }
  if (dmg > 0) { s.hull -= dmg; if (s.hull < 0) s.hull = 0; }
  if (s.hull <= 0 && !s.result) { Object.assign(s, endCombat(s, 2)); }
}
// Resolve combat -> debrief AND apply rewards to the shared run. The RUN mutation + async persist are
// a deliberate side-effect here (single-player: no netcode peers to keep in lockstep); the returned
// snapshot itself is fresh (we never mutate the passed `state`), and the debrief shows the real reward.
function endCombat(state, result) {
  const cc = state.cc;
  let earn = 0;
  if (result === 1) earn = cc.rewardCr + cc.killCr * state.kills;
  else if (!cc.mission) earn = cc.killCr * state.kills;          // ambush salvage even on retreat
  RUN.credits = Math.min(9999999, RUN.credits + earn);
  RUN.kills = (RUN.kills >>> 0) + state.kills;
  if (result === 1) {
    if (cc.repFac >= 0) RUN.rep[cc.repFac] = Math.max(-100, Math.min(100, RUN.rep[cc.repFac] + cc.repGain));
    if (cc.enemyRepFac >= 0) RUN.rep[cc.enemyRepFac] = Math.max(-100, Math.min(100, RUN.rep[cc.enemyRepFac] - cc.enemyRepLoss));
  }
  RUN.hull = Math.max(1, Math.round(state.hull));                // keep the run alive (never write hull 0)
  persist();                                                     // write the shared save (cross-play)
  return { ...state, phase: 'debrief', result, earnCr: earn, dbKills: state.kills };
}

// ---- hub navigation + economy (mutates the shared RUN + persists; single-player side-effect) ----
const SCREENS = ['bridge', 'map', 'market', 'shipyard', 'missions'];
const BRIDGE_ACT = ['map', 'market', 'shipyard', 'missions', 'missions'];
function enterScreen(state, sc) {
  const ns = { ...state, screen: sc };
  if (sc === 'map') { ns.target = firstOther(); ns.focus = { ...state.focus, map: 0 }; }
  if (sc === 'missions') { const n = MISSIONS ? MISSIONS.length : 0; const f = Math.min(state.focus.missions || 0, n); ns.focus = { ...state.focus, missions: f }; ns.sel = f; }   // clamp: the list shrinks at F_ECO systems (0 missions)
  return ns;
}
function firstOther() { for (let i = 0; i < SECTOR.length; i++) if (i !== RUN.sys) return i; return 0; }
function moveFocus(state, sc, n, d) { const i = ((state.focus[sc] + d) % n + n) % n; return { ...state, focus: { ...state.focus, [sc]: i } }; }
function deny(state, msg) { return { ...state, toast: { text: msg, kind: 'bad', until: state.clock + 1600 }, flash: { kind: 'shake', until: state.clock + 240 } }; }
const beaconLitIdx = (i) => ((RUN.beacon_lit >>> 0) & (1 << i)) !== 0;

function hubReduce(state, a) {
  const t = a.type, sc = state.screen;
  if (t === 'screen') return enterScreen(state, SCREENS[a.to] || 'bridge');
  if (t === 'tab') { const i = SCREENS.indexOf(sc); return enterScreen(state, SCREENS[(i + a.d + 5) % 5]); }
  if (t === 'hover') return (a.screen === sc && state.focus[sc] !== a.i) ? { ...state, focus: { ...state.focus, [sc]: a.i } } : state;
  if (t === 'mapTarget' && sc === 'map') return { ...state, target: a.i };
  if (t === 'pad') return padNav(state, a);
  if (t === 'back') return sc !== 'bridge' ? enterScreen(state, 'bridge') : state;
  if (sc === 'bridge') return bridgeReduce(state, a);
  if (sc === 'map') return mapReduce(state, a);
  if (sc === 'market') return marketReduce(state, a);
  if (sc === 'shipyard') return shipyardReduce(state, a);
  if (sc === 'missions') return missionsReduce(state, a);
  return state;
}
function padNav(state, a) {                          // edge-triggered analog stick -> discrete nav
  const dz = 0.35, th = 0.6;
  if (Math.abs(a.x) < dz && Math.abs(a.y) < dz) return state.padArmed ? state : { ...state, padArmed: true };
  if (!state.padArmed) return state;
  let act = null;
  if (Math.abs(a.y) > th && Math.abs(a.y) >= Math.abs(a.x)) act = { type: 'navkey', k: a.y > 0 ? 'D' : 'U' };
  else if (Math.abs(a.x) > th) act = { type: 'navkey', k: a.x > 0 ? 'R' : 'L' };
  return act ? hubReduce({ ...state, padArmed: false }, act) : state;
}
function bridgeReduce(state, a) {
  if (a.type === 'navkey' && (a.k === 'U' || a.k === 'D')) return moveFocus(state, 'bridge', 5, a.k === 'D' ? 1 : -1);
  if (a.type === 'confirm') return enterScreen(state, BRIDGE_ACT[state.focus.bridge] || 'map');
  return state;
}
function mapActions() { const acts = ['jump']; if (SECTOR[RUN.sys].beacon && !beaconLitIdx(RUN.sys)) acts.push('relight'); acts.push('back'); return acts; }
function cycleTarget(t, d) { let x = t; for (let k = 0; k < SECTOR.length; k++) { x = (x + d + SECTOR.length) % SECTOR.length; if (x !== RUN.sys) return x; } return t; }
function mapReduce(state, a) {
  if (a.type === 'navkey') {
    if (a.k === 'L' || a.k === 'R') return { ...state, target: cycleTarget(state.target, a.k === 'R' ? 1 : -1) };
    if (a.k === 'U' || a.k === 'D') return moveFocus(state, 'map', mapActions().length, a.k === 'D' ? 1 : -1);
  }
  if (a.type === 'confirm') { const acts = mapActions(); const act = acts[Math.min(state.focus.map, acts.length - 1)]; if (act === 'jump') return doJump(state); if (act === 'relight') return doRelight(state); return enterScreen(state, 'bridge'); }
  return state;
}
function doJump(state) {
  const t = state.target; if (t < 0 || t === RUN.sys) return deny(state, 'Seleziona una destinazione');
  const d = sysDist(CONTENT, RUN.sys, t), cost = jumpCost(d);
  if (d > RUN.jump_range) return deny(state, 'Fuori portata');
  if (RUN.fuel < cost) return deny(state, 'Celle insufficienti');
  RUN.sys = t; RUN.fuel -= cost; RUN.epoch = (RUN.epoch >>> 0) + 1;
  rebuildSector(); persist();
  return { ...state, target: cycleTarget(RUN.sys, 1), focus: { ...state.focus, map: 0, missions: 0 }, flash: { kind: 'warp', until: state.clock + 600 } };   // new system -> fresh mission list
}
function doRelight(state) {
  if (!(SECTOR[RUN.sys].beacon && !beaconLitIdx(RUN.sys))) return state;
  if (RUN.credits < 300) return deny(state, 'Servono 300 cr');
  if (RUN.cargo[G_RELIQ] < 1) return deny(state, 'Serve 1 Reliquia');
  RUN.credits -= 300; RUN.cargo[G_RELIQ] -= 1;
  RUN.rep[F_CUSTODI] = Math.max(-100, Math.min(100, RUN.rep[F_CUSTODI] + 12));
  RUN.beacon_lit = (RUN.beacon_lit >>> 0) | (1 << RUN.sys);
  if (beaconsLit(CONTENT, RUN) >= beaconsTotal(CONTENT)) {
    RUN.sector = (RUN.sector >>> 0) + 1; RUN.beacon_lit = 0; RUN.sys = 0; RUN.epoch = (RUN.epoch >>> 0) + 1;
    rebuildSector(); persist();
    return enterScreen({ ...state, flash: { kind: 'sector', until: state.clock + 1500 }, toast: { text: 'SETTORE ' + RUN.sector, kind: 'good', until: state.clock + 1900 } }, 'map');
  }
  persist();
  return { ...state, flash: { kind: 'ignite', until: state.clock + 800 }, toast: { text: 'Faro acceso', kind: 'good', until: state.clock + 1400 } };
}
function marketReduce(state, a) {
  const rows = CONTENT.goods.length + 1;            // 8 goods + refuel
  if (a.type === 'navkey') {
    if (a.k === 'U' || a.k === 'D') return moveFocus(state, 'market', rows, a.k === 'D' ? 1 : -1);
    if (a.k === 'L') return { ...state, marketCol: 0 };
    if (a.k === 'R') return { ...state, marketCol: 1 };
  }
  if (a.type === 'col') return { ...state, marketCol: state.marketCol ? 0 : 1 };
  if (a.type === 'qty') { const q = [1, 5, 10]; let i = q.indexOf(state.marketQty); i = Math.max(0, Math.min(2, i + (a.d > 0 ? 1 : -1))); return { ...state, marketQty: q[i] }; }
  if (a.type === 'refuel') return doRefuel(state);
  if (a.type === 'buy') return doBuy({ ...state, marketCol: 0, focus: { ...state.focus, market: a.g } }, a.g);     // mouse: direct buy of good a.g
  if (a.type === 'sell') return doSell({ ...state, marketCol: 1, focus: { ...state.focus, market: a.g } }, a.g);   // mouse: direct sell of good a.g
  if (a.type === 'confirm') { const r = state.focus.market; if (r >= CONTENT.goods.length) return doRefuel(state); return state.marketCol ? doSell(state, r) : doBuy(state, r); }
  return state;
}
function doBuy(state, g) {
  const price = unitBuy(CONTENT, RUN.sys, g, RUN.epoch, RUN.rep);
  const q = Math.min(state.marketQty, Math.floor(RUN.credits / price), RUN.cargo_max - cargoUsed(RUN));
  if (q <= 0) return deny(state, RUN.credits < price ? 'Crediti insufficienti' : 'Stiva piena');
  RUN.credits -= price * q; RUN.cargo[g] += q; persist();
  return { ...state, flash: { kind: 'pop', until: state.clock + 400 } };
}
function doSell(state, g) {
  if (RUN.cargo[g] <= 0) return deny(state, 'Niente da vendere');
  const price = unitSell(CONTENT, RUN.sys, g, RUN.epoch, RUN.rep), q = Math.min(state.marketQty, RUN.cargo[g]);
  RUN.credits = Math.min(9999999, RUN.credits + price * q); RUN.cargo[g] -= q; persist();
  return { ...state, flash: { kind: 'pop', until: state.clock + 400 } };
}
function doRefuel(state) {
  const price = refuelPrice(CONTENT, RUN.sys);
  const n = Math.min(state.marketQty, RUN.fuel_max - RUN.fuel, Math.floor(RUN.credits / price));
  if (n <= 0) return deny(state, RUN.fuel >= RUN.fuel_max ? 'Serbatoio pieno' : 'Crediti insufficienti');
  RUN.credits -= price * n; RUN.fuel += n; persist();
  return { ...state, flash: { kind: 'pop', until: state.clock + 400 } };
}
function shipyardReduce(state, a) {
  const rows = SHOP.length + 1;                     // upgrades + repair
  if (a.type === 'navkey' && (a.k === 'U' || a.k === 'D')) return moveFocus(state, 'shipyard', rows, a.k === 'D' ? 1 : -1);
  if (a.type === 'confirm') {
    const r = state.focus.shipyard, key = r < SHOP.length ? SHOP[r].key : 'repair';
    if (shopBuy(key, RUN)) { persist(); return { ...state, flash: { kind: 'pop', until: state.clock + 400 } }; }
    return deny(state, 'Non disponibile');
  }
  return state;
}
function missionsReduce(state, a) {
  const n = (MISSIONS ? MISSIONS.length : 0) + 1;
  if (a.type === 'navkey' && (a.k === 'U' || a.k === 'D')) { const ns = moveFocus(state, 'missions', n, a.k === 'D' ? 1 : -1); ns.sel = ns.focus.missions; return ns; }
  if (a.type === 'confirm') { const slot = state.focus.missions; const cc = (slot < (MISSIONS ? MISSIONS.length : 0)) ? combatFromMission(MISSIONS[slot]) : ambushCfg(); return startCombat({ ...state, sel: slot }, cc); }
  return state;
}

// ---- renderer bootstrap ------------------------------------------------------------------------
let R3D = null, loading = false;
async function ensureRenderer(canvas, api) {
  if (R3D || loading) return; loading = true;
  try { const mod = await import('/apps/games/games/constellations-3d.js'); R3D = await mod.createRenderer(canvas, api); const w = canvas.parentElement; if (w) { const r = w.getBoundingClientRect(); if (r.width > 1) R3D.resize(r.width, r.height); } }
  catch (e) { console.error('constellations-3d load failed', e); }
  finally { loading = false; }
}

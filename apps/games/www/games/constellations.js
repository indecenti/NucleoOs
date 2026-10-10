// constellations.js — "Costellazioni 3D": the web Game Center continuation of the Cardputer space
// game. It CONTINUES THE SAME RUN: loads /sd/data/costellazioni/save.bin via the firmware endpoint
// (constellations-save.js), regenerates the current sector from (seed, sector) with the SHARED
// deterministic generator (constellations-gen.js, byte-identical to the firmware), flies its missions
// as 6DOF space combat (stelle/sim.js, drawn by constellations-3d.js) in a system whose look comes from
// a web-only blueprint (stelle/world.js), and writes credits/kills/hull back to the save — so the
// campaign is shared both ways with the Cardputer.
//
// Single-player, real-time. Campaign numbers live in the module-global RUN (the save); the harness
// `state` carries the phase + hub navigation; the live flight is the module-global FLIGHT, advanced at
// a fixed 60 Hz by the renderer (input: stelle/hud.js). On debrief we mutate RUN and POST it
// (epoch/sector-merge guarded) with the same reward rules as the rail shooter it replaces.
import { defineGame } from '/apps/games/nucleo-game.js';
import CONTENT from '/apps/games/games/constellations-content.js';
import { genSector, genMissions, beaconsPerSector, F_ECO } from '/apps/games/games/constellations-gen.js';
import { loadSave, storeSave, newSave } from '/apps/games/games/constellations-save.js';
import { unitBuy, unitSell, refuelPrice, jumpCost, sysDist, cargoUsed, beaconsLit, beaconsTotal } from '/apps/games/games/constellations-econ.js';
import { SHOP, shopMaxed, shopCost, shopBuy, repairCost } from '/apps/games/games/constellations-shop.js';
import { createFlight, ACE_NAMES, cmd as simCmd, OUT, EV, CLS } from '/apps/games/games/stelle/sim.js';
import { placeOver, placeNearSite } from '/apps/games/games/stelle/surface.js';
import { systemBlueprint } from '/apps/games/games/stelle/world.js';
import { loadWeb, saveWeb, freshWeb, markVisited, markRelit, markScanned, unlock, markSeen, foundMask, markFound, markLooted, wasLooted } from '/apps/games/games/constellations-web.js';
import { ENTRIES, BY_ID, entriesOf, CATS } from '/apps/games/games/constellations-codex.js';

const G_RELIQ = 6, F_CUSTODI = 1;   // relic good index / Keepers faction (beacon relight bookkeeping)
let conflictPending = null;          // set by persist() when the device advanced the run (-> conflict screen)
let BOOTED = false;                  // true once the initial loadSave settles (continue vs new-run)

// ---- shared module state (one active run at a time) --------------------------------------------
let RUN = null;            // the campaign save (credits, hull, seed, sector, ...)
let SYNCED = null;         // the copy of RUN last read from / written to the card (storeSave's base)
let WEB = null;            // the web-only progress next to it (constellations-web.js): visited, codex, relit history
let PLOT = -1;             // the system the jump drive is plotted to (in flight)
const CODEX_NEW = [];      // codex entries unlocked since the last toast
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
  if (PLOT === RUN.sys || PLOT >= SECTOR.length) PLOT = -1;
  if (WEB) { if (markVisited(WEB, RUN.sector, RUN.sys)) saveWeb(WEB); codexOnSystem(); }
}
async function bootRun() {
  RUN = await loadSave();                 // continue the Cardputer run, or null -> offer a New Run
  SYNCED = RUN ? JSON.parse(JSON.stringify(RUN)) : null;
  if (RUN) { WEB = await loadWeb(RUN.seed); codexStart(); }
  BOOTED = true;
  if (RUN) rebuildSector();
}
function startNewRun() { RUN = newSave(); WEB = freshWeb(RUN.seed); codexStart(); rebuildSector(); persist(); saveWeb(WEB); }

// ---- codex: what play unlocks (the web save keeps it; a toast / HUD note says so) --------------------------------
function codexUnlock(id) { if (!WEB || !BY_ID[id] || !unlock(WEB, id)) return false; CODEX_NEW.push(id); saveWeb(WEB); return true; }
function codexStart() { for (const e of ENTRIES) if (e.start) unlock(WEB, e.id); }
const FAC_CODEX = ['gilda', 'custodi', 'relitti', 'eco'];
function codexOnSystem() {
  const s = SECTOR[RUN.sys];
  codexUnlock(FAC_CODEX[s.faction]);
  if (s.faction === F_ECO) codexUnlock('eco');
  if (s.beacon) codexUnlock(beaconLitIdx(RUN.sys) ? 'beacon_lit' : 'beacon_dead');
  if (RUN.sector >= 3) codexUnlock('deep');
  if (RUN.sector >= 6) codexUnlock('warden');
  if (RUN.sector >= 9) codexUnlock('beyond');
  if (RUN.rep[0] >= 25) codexUnlock('c_admiral');
}
function codexOnDock() {
  const f = SECTOR[RUN.sys].faction;
  if (f === 0) codexUnlock('ardali');
  if (f === 1) { codexUnlock('lamp'); codexUnlock('c_abbess'); }
  if (f === 2) { codexUnlock('gutterdeep'); codexUnlock('graveyard'); codexUnlock('c_matriarch'); }
}
const ACE_CODEX = ['c_ace_lancer', 'c_ace_vigil', 'c_ace_gutter', null, 'c_ace_bram'];
let evCursor = 0;
// flight events the codex cares about (kills, the Voice, surveys, the cast aces), read behind the renderer
function codexOnFlight(F) {
  if (F.evHead - evCursor > 500) evCursor = F.evHead - 500;
  while (evCursor < F.evHead) {
    const e = F.ev[evCursor % 512]; evCursor++;
    if ((e.n === EV.KILL || e.n === EV.CAPKILL) && e.s && !e.s.isPlayer && e.s.team === 1) codexUnlock('s_' + e.s.ck);
    else if (e.n === EV.WARPIN && e.s && e.s.fac === F_ECO) { if (e.s.ck === 'lattice' || e.s.ck === 'choir') { codexUnlock('s_' + e.s.ck); if (e.s.team === 1) codexUnlock('echo_fleet'); } }
    else if (e.n === EV.COMMS) {
      if (e.v && e.v.voice) { codexUnlock('voice'); codexUnlock('c_echo'); }
      const m = /^cz_c_acequip_(\d)$/.exec(e.k || ''); if (m && ACE_CODEX[+m[1]]) codexUnlock(ACE_CODEX[+m[1]]);
    } else if (e.n === EV.SCAN && e.v) {
      if (e.k === 'planet' || e.k === 'moon') { codexUnlock('w_' + e.v.type); if (markScanned(WEB, RUN.sector, RUN.sys, e.a)) saveWeb(WEB); }
    } else if (e.n === EV.RELIGHT) { codexUnlock('relight'); codexUnlock('c_novice'); codexUnlock('beacon_lit'); }
    else if (e.n === EV.DISCOVER && e.v) onDiscover(e.v);
    else if (e.n === EV.LOOT && e.v) onLoot(e.v);
    else if (e.n === EV.SURF && e.a === -1 && e.v && e.v.type) codexUnlock('w_' + e.v.type);
  }
}
// M3: a site found on a world pays once (the web save remembers it); ruins open their codex entry; a relic comes aboard
const RUIN_CODEX = { gate: 'gate', archive: 'archive', observatory: 'observatory' };
function onDiscover(v) {
  if (!WEB || !RUN) return;
  if (!markFound(WEB, RUN.sector, RUN.sys, v.w, v.i)) return;
  const L = v.loot || {};
  if (L.cr) RUN.credits = Math.min(9999999, RUN.credits + L.cr);
  if (L.rep >= 0 && L.rep < 4 && L.repN) RUN.rep[L.rep] = Math.max(-100, Math.min(100, RUN.rep[L.rep] + L.repN));
  if (RUIN_CODEX[v.kind]) { codexUnlock(RUIN_CODEX[v.kind]); codexUnlock('costellatori'); if (v.kind === 'archive') codexUnlock('c_novice'); }
  if (v.type) codexUnlock('w_' + v.type);
  if (FLIGHT) FLIGHT.earned += L.cr || 0;
  saveWeb(WEB); persist();
}
function onLoot(v) {
  if (!WEB || !RUN || wasLooted(WEB, RUN.sector, RUN.sys, v.w, v.i)) return;
  markLooted(WEB, RUN.sector, RUN.sys, v.w, v.i);
  if (cargoUsed(RUN) < RUN.cargo_max) RUN.cargo[G_RELIQ] = (RUN.cargo[G_RELIQ] | 0) + 1;
  else RUN.credits = Math.min(9999999, RUN.credits + 220);   // no room: the Keepers buy it off you on the spot
  saveWeb(WEB); persist();
}
async function persist() {
  DIRTY = true;                          // mark the latest RUN as needing a write
  if (SAVING) return;                    // a POST is already running; it will flush the new delta below
  SAVING = true;
  try {
    while (DIRTY) {                       // re-POST until the in-RAM RUN matches what we last sent
      DIRTY = false;
      const snap = JSON.parse(JSON.stringify(RUN));
      const r = await storeSave(snap, SYNCED);
      if (r && r.conflict && r.disk) { RUN = r.disk; SYNCED = JSON.parse(JSON.stringify(r.disk)); rebuildSector(); conflictPending = r.disk; break; }
      if (r && r.ok) SYNCED = snap;
    }
  } catch (e) { console.warn('[costellazioni] save failed', e); }
  finally { SAVING = false; }
}

// ---- flight setup: the shared mission numbers -> a 6DOF sortie (stelle/sim.js) --------------------
// The mission row from the shared generator is passed through unchanged (waves, per_wave, foe_hp,
// foe_dmg, rewards, reputation); the sim condenses it into 2-5 engagements and reports kills + hull.
let FLIGHT = null;          // the live flight (null outside combat)
let BP = null, BPKEY = '';  // web-only visual blueprint of the current system (stelle/world.js)
let lastTickAt = 0;         // harness tick heartbeat: the game bar pauses the tick -> pause the flight too
let MISSION_FLAVOR = null;  // flavor of the launched mission (for the mission card)
function combatFromMission(m) {
  const fl = m.flavor || {};
  return { type: m.type, foeFac: m.foe_fac, waves: m.waves, perWave: m.per_wave,
    foeHp: m.foe_hp, foeDmg: m.foe_dmg, foeSpeed: m.foe_speed_pml * 0.1, ace: m.ace,
    rewardCr: m.reward_cr, killCr: m.kill_cr, repFac: m.offer_fac, repGain: m.rep_gain,
    enemyRepFac: m.foe_fac, enemyRepLoss: m.enemy_rep_loss, mission: true,
    arch: fl.archK, gang: fl.gangI, targetName: fl.aceI >= 0 ? ACE_NAMES[fl.aceI] : (fl.nameBase || fl.name || ''), aceId: fl.aceI != null ? fl.aceI : -1, slot: m.slot };
}
// An interception: Wreck raiders, unless this is Echo space (its lattice drones) or the station's own faction
// has turned on you (reputation -25 or worse) — the same rule as the firmware's ambush_faction().
function ambushFaction() {
  const sf = SECTOR[RUN.sys].faction;
  if (sf === F_ECO) return F_ECO;
  return (sf !== 2 && RUN.rep[sf & 3] <= -25) ? sf : 2;
}
function ambushCfg() {
  const tier = 1 + Math.min(12, RUN.sector);
  const ff = ambushFaction();
  return { type: MT.PATROL, foeFac: ff, waves: 2 + (tier >= 6 ? 1 : 0), perWave: 2 + (tier & 1), foeHp: 30 + tier * 6,
    foeDmg: 7 + tier, foeSpeed: 7.8 + tier * 0.4, ace: 0, rewardCr: 0, killCr: 18 + tier * 4,
    repFac: -1, repGain: 0, enemyRepFac: ff, enemyRepLoss: 2, mission: false, slot: 15 };
}
function currentBlueprint() {
  if (!RUN || !SECTOR) return null;
  const lit = ((RUN.beacon_lit >>> 0) & (1 << RUN.sys)) !== 0;
  const key = RUN.seed + ':' + RUN.sector + ':' + RUN.sys + ':' + (lit ? 1 : 0);
  if (key !== BPKEY) { BP = systemBlueprint(RUN.seed >>> 0, RUN.sector >>> 0, RUN.sys, SECTOR[RUN.sys], lit); BPKEY = key; }
  return BP;
}
function startCombat(state, cc, flavor) {
  MISSION_FLAVOR = flavor || null;
  const w = typeof window !== 'undefined' ? window : {};
  FLIGHT = createFlight({ bp: currentBlueprint(), cc, run: RUN, seed: RUN.seed >>> 0, sector: RUN.sector >>> 0, sys: RUN.sys, slot: cc.slot | 0,
    autopilot: !!w.__czAutopilot, autoDock: !!w.__czAutoDock, god: !!w.__czGod, brief: w.__czNoBrief || cc.kind === 'explore' ? false : undefined, timeScale: w.__czTimeScale | 0,
    autoLand: !!w.__czAutoLand, autoSites: w.__czAutoSites | 0, clock: w.__czClock != null ? +w.__czClock : undefined,
    // M3: what the web save already found on this system's worlds (found sites do not pay twice)
    discovered: (wi) => (WEB ? foundMask(WEB, RUN.sector >>> 0, RUN.sys, wi) : 0),
    looted: (wi) => (WEB && WEB.looted ? (WEB.looted[(RUN.sector >>> 0) + ':' + RUN.sys + ':' + wi] >>> 0) || 0 : 0) });
  evCursor = FLIGHT.evHead;
  lastTickAt = (typeof performance !== 'undefined' ? performance.now() : 0);
  return { ...state, phase: 'combat', cc, result: 0, flightNo: (state.flightNo || 0) + 1 };
}
// Free flight in the current system: launched from the station (undock), arrived by jump, or flown to the beacon
// to relight it. A chance encounter waits in some systems (the Echo's drones in Echo space, raiders elsewhere).
function exploreCfg(o = {}) {
  const tier = 1 + Math.min(12, RUN.sector), ff = ambushFaction(), sf = SECTOR[RUN.sys].faction;
  const chance = o.relight ? 0 : sf === F_ECO ? 0.85 : (sf !== 2 && RUN.rep[sf & 3] <= -25) ? 0.75 : 0.4;
  const w = typeof window !== 'undefined' ? window : {};
  return { kind: 'explore', type: 0, foeFac: ff, waves: 2, perWave: 3, foeHp: 30 + tier * 6, foeDmg: 7 + tier, foeSpeed: 7.8 + tier * 0.4, ace: 0, rewardCr: 0, killCr: 18 + tier * 4,
    repFac: -1, repGain: 0, enemyRepFac: ff, enemyRepLoss: 2, mission: false, slot: 13,
    encounter: w.__czEncounter != null ? !!w.__czEncounter : Math.random() < chance, encounterAt: w.__czEncounterAt || 0,
    undock: !!o.undock, arrive: !!o.arrive, relight: !!o.relight,
    // the Echo answers the first beacon of a sector by measuring you; every further one, by fighting
    echoCalm: beaconsLit(CONTENT, RUN) === 0, litCount: beaconsLit(CONTENT, RUN), keepers: RUN.rep[F_CUSTODI] > -25 };
}
function startExplore(state, o) { return startCombat(state, exploreCfg(o), null); }
// the shared numbers of a relight (the hub used to do this on the spot; now it happens when the light bursts out)
function applyRelight() {
  RUN.credits = Math.max(0, RUN.credits - 300); RUN.cargo[G_RELIQ] = Math.max(0, RUN.cargo[G_RELIQ] - 1);
  RUN.rep[F_CUSTODI] = Math.max(-100, Math.min(100, RUN.rep[F_CUSTODI] + 12));
  RUN.beacon_lit = (RUN.beacon_lit >>> 0) | (1 << RUN.sys);
  if (WEB) { markRelit(WEB, RUN.sector, RUN.sys); saveWeb(WEB); }
  let sectorUp = false;
  if (beaconsLit(CONTENT, RUN) >= beaconsTotal(CONTENT)) { RUN.sector = (RUN.sector >>> 0) + 1; RUN.beacon_lit = 0; RUN.sys = 0; RUN.epoch = (RUN.epoch >>> 0) + 1; sectorUp = true; }
  persist();
  return sectorUp;
}
// after the flight: kills, salvage and hull go back to the shared run
function settleFlight(F, cc) {
  const kills = F ? F.kills : 0, earn = cc.killCr * kills;
  RUN.credits = Math.min(9999999, RUN.credits + earn); RUN.kills = (RUN.kills >>> 0) + kills;
  RUN.hull = Math.max(1, Math.min(RUN.hull_max, Math.round(F ? F.player.hull : RUN.hull)));
  return earn;
}
function dockedEnd(state) {
  const earn = settleFlight(FLIGHT, state.cc);
  if (FLIGHT.sectorUp) rebuildSector();
  persist(); codexOnDock(); FLIGHT = null;
  return enterScreen({ ...state, phase: 'hub', toast: { key: earn ? 'cz_t_docked_salvage' : 'cz_t_docked', v: { name: SECTOR[RUN.sys].it, cr: earn }, kind: 'good', until: (state.clock || 0) + 2400 } }, 'bridge');
}
// the drive fired in flight: the shared jump (fuel, epoch) and a new free flight that drops out in the target system
function jumpedEnd(state) {
  const to = FLIGHT.jumpedTo;
  settleFlight(FLIGHT, state.cc);
  const d = sysDist(CONTENT, RUN.sys, to), cost = jumpCost(d);
  if (to >= 0 && to < SECTOR.length && to !== RUN.sys && RUN.fuel >= cost) { RUN.sys = to; RUN.fuel -= cost; RUN.epoch = (RUN.epoch >>> 0) + 1; }
  PLOT = -1; rebuildSector(); persist();
  return startExplore({ ...state, focus: { ...state.focus, missions: 0 } }, { arrive: true });
}
function jumpInfo(t) {
  if (!RUN || !SECTOR || t < 0 || t >= SECTOR.length || t === RUN.sys) return { ok: false, why: 'cz_t_pick_dest' };
  const d = sysDist(CONTENT, RUN.sys, t), cost = jumpCost(d);
  const why = d > RUN.jump_range ? 'cz_t_out_of_range' : RUN.fuel < cost ? 'cz_t_no_cells' : '';
  return { ok: !why, why, cost, d: Math.round(d), name: SECTOR[t].it, fac: SECTOR[t].faction };
}

// ---- bridge (pre-flight) actions ---------------------------------------------------------------
export default defineGame({
  id: 'constellations', name: 'Costellazioni 3D',
  minPlayers: 1, maxPlayers: 1, aiCapable: false, realtime: true, tickHz: 20,
  renderMode: '3d', fillViewport: true, capturesPointer: true, pointerAxes: 'xy', category: 'arcade',
  // The host (index.html) reads this to lock the pointer ONLY in the dogfight — the hub menu keeps the
  // system cursor visible/clickable. Falls back to the static capturesPointer for other games.
  wantsPointerLock(s) { return !!s && s.phase === 'combat' && !!FLIGHT && !FLIGHT.outcome; },

  setup() {
    return {
      phase: RUN ? 'hub' : (BOOTED ? 'new_run' : 'loading'),
      screen: 'bridge', focus: { bridge: 0, map: 0, market: 0, shipyard: 0, missions: 0, codex: 0 }, codexCat: 0,
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
    if (p === 'combat') return state;   // flight input is read directly by stelle/hud.js
    if (p === 'debrief') { if (['confirm', 'back', 'fire', 'click'].includes(a.type)) { FLIGHT = null; return enterScreen({ ...state, phase: 'hub' }, 'bridge'); } return state; }
    if (p === 'hub') return hubReduce(state, a);
    return state;
  },

  tick(state, dtMs) {
    if (state.phase === 'loading') { if (RUN) return enterScreen({ ...state, phase: 'hub' }, 'bridge'); if (BOOTED) return { ...state, phase: 'new_run' }; return state; }
    if (conflictPending && state.phase === 'hub') { conflictPending = null; return { ...state, phase: 'conflict', toast: { key: 'cz_t_conflict', kind: 'warn' } }; }
    if (state.phase === 'combat') {
      lastTickAt = (typeof performance !== 'undefined' ? performance.now() : 0);
      if (FLIGHT) {
        codexOnFlight(FLIGHT);
        if (FLIGHT.relit && !FLIGHT.relitDone) { FLIGHT.relitDone = true; FLIGHT.sectorUp = applyRelight(); }
        if (FLIGHT.done) {
          if (FLIGHT.outcome === OUT.DOCKED) return dockedEnd(state);
          if (FLIGHT.outcome === OUT.JUMPED) return jumpedEnd(state);
          return endCombat(state, FLIGHT.outcome === 1 ? 1 : FLIGHT.outcome === 2 ? 2 : -1);
        }
      }
      return state;
    }
    if (CODEX_NEW.length && state.phase === 'hub' && !(state.toast && state.toast.until > (state.clock || 0))) {
      const id = CODEX_NEW.shift();
      return { ...state, clock: (state.clock || 0) + dtMs, toast: { key: 'cz_t_codex', v: { cx: id }, kind: 'good', until: (state.clock || 0) + 2600 } };
    }
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
    if (state && (state.phase !== 'combat' || (FLIGHT && FLIGHT.outcome)) && typeof document !== 'undefined' && document.pointerLockElement) { try { document.exitPointerLock(); } catch {} }
    if (R3D) R3D.frame(state, MODEL(), api);
  },
  resize(w, h) { if (R3D) R3D.resize(w, h); },
  unmount() { if (R3D) { try { R3D.dispose(); } catch {} R3D = null; } },
});

// The live model handed to the renderer/UI (run + generated sector/missions + econ/shop helpers).
const _model = { run: null, sector: null, missions: null, MT_NAME, CONTENT,
  econ: { unitBuy, unitSell, refuelPrice, jumpCost, sysDist, cargoUsed, beaconsLit, beaconsTotal, beaconsPerSector },
  shop: { SHOP, shopMaxed, shopCost, repairCost }, flight: null, bp: null, paused: false, flavor: null, sysName: '', web: null, plot: -1, notes: CODEX_NEW,
  // what the in-flight HUD may ask of the game (the jump drive needs the shared run's fuel and range)
  actions: {
    plot(i) { PLOT = i; },
    jumpInfo,
    jump() { const J = jumpInfo(PLOT); if (!J.ok) return J.why; return FLIGHT && simCmd(FLIGHT, 'jump', PLOT) ? '' : 'cz_cr_blocked'; },
    seen(id) { if (markSeen(WEB, id)) saveWeb(WEB); },
  } };
function MODEL() {
  const m = _model;
  m.run = RUN; m.sector = SECTOR; m.missions = MISSIONS; m.flight = FLIGHT; m.bp = FLIGHT ? FLIGHT.bp : currentBlueprint(); m.flavor = MISSION_FLAVOR;
  m.web = WEB; m.plot = PLOT;
  m.sysName = SECTOR && RUN ? SECTOR[RUN.sys].it : '';
  // the harness stops ticking while the Game Center menu is open: freeze the flight with it
  m.paused = !!FLIGHT && typeof performance !== 'undefined' && performance.now() - lastTickAt > 400;
  return m;
}
// test hook (games-host / e2e): read-only view of the live run and flight
export const __cz = { get run() { return RUN; }, get flight() { return FLIGHT; }, get bp() { return currentBlueprint(); }, get web() { return WEB; }, get model() { return MODEL(); } };
// review / test hook: look at another system of the sector from the hub (in memory only — never persisted)
const __dev = { goto(i) { if (!RUN || !SECTOR || i < 0 || i >= SECTOR.length || FLIGHT) return false; RUN.sys = i; rebuildSector(); return true; },
  // M3 review: over world wi of this system, in daylight (sun sunEl degrees up), at height alt
  overWorld(wi, alt, sunEl, az, v) { return !!FLIGHT && placeOver(FLIGHT, wi, alt, sunEl, az, v); },
  nearSite(wi, kinds, dist, alt, v, sunEl) { return FLIGHT ? placeNearSite(FLIGHT, wi, kinds, dist, alt, v, sunEl) : -1; } };
if (typeof window !== 'undefined') { window.__cz = window.__cz || {}; window.__cz.game = __cz; window.__cz.dev = __dev; }

// ---- input vocabulary (one keymap, phase-interpreted) ------------------------------------------
function mapKey(k) {
  if (typeof k === 'string') {
    if (k.startsWith('Hover:')) { const a = k.split(':'); return { type: 'hover', screen: a[1], i: +a[2] }; }
    if (k.startsWith('Tgt:')) return { type: 'mapTarget', i: +k.slice(4) };
    if (k.startsWith('Tab:')) return { type: 'screen', to: +k.slice(4) };
    if (k.startsWith('Buy:')) return { type: 'buy', g: +k.slice(4) };     // market: click the buy cell
    if (k.startsWith('Sell:')) return { type: 'sell', g: +k.slice(5) };   // market: click the sell cell
    if (k.startsWith('Cat:')) return { type: 'codexCat', i: +k.slice(4) };   // codex: click a category
  }
  if (k === 'ArrowLeft' || k === 'a') return { type: 'navkey', k: 'L' };
  if (k === 'ArrowRight' || k === 'd') return { type: 'navkey', k: 'R' };
  if (k === 'ArrowUp' || k === 'w') return { type: 'navkey', k: 'U' };
  if (k === 'ArrowDown' || k === 's') return { type: 'navkey', k: 'D' };
  if (k === ' ' || k === 'Enter' || k === 'j') return { type: 'confirm' };
  if (k === 'Escape' || k === 'q' || k === 'Backspace') return { type: 'back' };
  if (k >= '1' && k <= '6') return { type: 'screen', to: +k - 1 };
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
// Resolve combat -> debrief AND apply rewards to the shared run. The RUN mutation + async persist are
// a deliberate side-effect here (single-player: no netcode peers to keep in lockstep); the returned
// snapshot itself is fresh (we never mutate the passed `state`), and the debrief shows the real reward.
function endCombat(state, result) {
  const cc = state.cc, F = FLIGHT;
  const kills = F ? F.kills : 0;
  let earn = 0;
  if (result === 1) earn = cc.rewardCr + cc.killCr * kills;
  else if (!cc.mission) earn = cc.killCr * kills;               // ambush salvage even on retreat
  const relit = !!(F && F.relit);
  if (F && F.sectorUp) rebuildSector();
  RUN.credits = Math.min(9999999, RUN.credits + earn);
  RUN.kills = (RUN.kills >>> 0) + kills;
  if (result === 1) {
    if (cc.repFac >= 0) RUN.rep[cc.repFac] = Math.max(-100, Math.min(100, RUN.rep[cc.repFac] + cc.repGain));
    if (cc.enemyRepFac >= 0) RUN.rep[cc.enemyRepFac] = Math.max(-100, Math.min(100, RUN.rep[cc.enemyRepFac] - cc.enemyRepLoss));
  }
  RUN.hull = Math.max(1, Math.min(RUN.hull_max, Math.round(F ? F.player.hull : RUN.hull)));   // keep the run alive (never write hull 0)
  persist();                                                     // write the shared save (cross-play)
  const st = F ? F.stats : null;
  const dbStats = st ? { time: Math.round(F.t), shots: st.shots, hits: st.hits, acc: st.shots ? Math.round(st.hits / st.shots * 100) : 0,
    dmgIn: Math.round(st.dmgIn), aceKill: st.aceKill, capKill: st.capKill, kind: F.mission.kind, relit, sectorUp: !!F.sectorUp, sector: RUN.sector } : null;
  return { ...state, phase: 'debrief', result, earnCr: earn, dbKills: kills, dbStats };
}

// ---- hub navigation + economy (mutates the shared RUN + persists; single-player side-effect) ----
const SCREENS = ['bridge', 'map', 'market', 'shipyard', 'missions', 'codex'];
const BRIDGE_ACT = ['launch', 'map', 'market', 'shipyard', 'missions', 'codex'];
function enterScreen(state, sc) {
  const ns = { ...state, screen: sc };
  if (sc === 'map') { ns.target = firstOther(); ns.focus = { ...state.focus, map: 0 }; }
  if (sc === 'missions') { const n = MISSIONS ? MISSIONS.length : 0; const f = Math.min(state.focus.missions || 0, n); ns.focus = { ...state.focus, missions: f }; ns.sel = f; }   // clamp: the list shrinks at F_ECO systems (0 missions)
  if (sc === 'codex') codexSeen(ns);
  return ns;
}
function firstOther() { for (let i = 0; i < SECTOR.length; i++) if (i !== RUN.sys) return i; return 0; }
function moveFocus(state, sc, n, d) { const i = ((state.focus[sc] + d) % n + n) % n; return { ...state, focus: { ...state.focus, [sc]: i } }; }
function deny(state, key, v) { return { ...state, toast: { key, v, kind: 'bad', until: state.clock + 1600 }, flash: { kind: 'shake', until: state.clock + 240 } }; }
const beaconLitIdx = (i) => ((RUN.beacon_lit >>> 0) & (1 << i)) !== 0;

function hubReduce(state, a) {
  const t = a.type, sc = state.screen;
  if (t === 'screen') return enterScreen(state, SCREENS[a.to] || 'bridge');
  if (t === 'tab') { const i = SCREENS.indexOf(sc); return enterScreen(state, SCREENS[(i + a.d + SCREENS.length) % SCREENS.length]); }
  if (t === 'hover') { if (a.screen !== sc || state.focus[sc] === a.i) return state; const ns = { ...state, focus: { ...state.focus, [sc]: a.i } }; if (sc === 'codex') codexSeen(ns); return ns; }
  if (t === 'mapTarget' && sc === 'map') return { ...state, target: a.i };
  if (t === 'codexCat') return codexReduce(enterScreen(state, 'codex'), a);
  if (t === 'pad') return padNav(state, a);
  if (t === 'back') return sc !== 'bridge' ? enterScreen(state, 'bridge') : state;
  if (sc === 'bridge') return bridgeReduce(state, a);
  if (sc === 'map') return mapReduce(state, a);
  if (sc === 'market') return marketReduce(state, a);
  if (sc === 'shipyard') return shipyardReduce(state, a);
  if (sc === 'missions') return missionsReduce(state, a);
  if (sc === 'codex') return codexReduce(state, a);
  return state;
}
function codexSeen(st) { const e = entriesOf(st.codexCat | 0)[st.focus.codex | 0]; if (e && WEB && WEB.codex[e.id] != null && markSeen(WEB, e.id)) saveWeb(WEB); }
function codexReduce(state, a) {
  const cat = state.codexCat | 0, n = entriesOf(cat).length;
  let ns = state;
  if (a.type === 'codexCat') ns = { ...state, codexCat: a.i % CATS.length, focus: { ...state.focus, codex: 0 } };
  else if (a.type === 'navkey' && (a.k === 'L' || a.k === 'R')) ns = { ...state, codexCat: (cat + (a.k === 'R' ? 1 : -1) + CATS.length) % CATS.length, focus: { ...state.focus, codex: 0 } };
  else if (a.type === 'navkey' && (a.k === 'U' || a.k === 'D')) ns = moveFocus(state, 'codex', n, a.k === 'D' ? 1 : -1);
  if (ns !== state) codexSeen(ns);
  return ns;
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
  if (a.type === 'navkey' && (a.k === 'U' || a.k === 'D')) return moveFocus(state, 'bridge', BRIDGE_ACT.length, a.k === 'D' ? 1 : -1);
  if (a.type === 'confirm') { const act = BRIDGE_ACT[state.focus.bridge] || 'map'; return act === 'launch' ? startExplore(state, { undock: true }) : enterScreen(state, act); }
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
  const t = state.target; if (t < 0 || t === RUN.sys) return deny(state, 'cz_t_pick_dest');
  const d = sysDist(CONTENT, RUN.sys, t), cost = jumpCost(d);
  if (d > RUN.jump_range) return deny(state, 'cz_t_out_of_range');
  if (RUN.fuel < cost) return deny(state, 'cz_t_no_cells');
  RUN.sys = t; RUN.fuel -= cost; RUN.epoch = (RUN.epoch >>> 0) + 1;
  rebuildSector(); persist();
  // the renderer flies the jump (drive, tunnel, arrival); you land on the bridge of the new system
  return enterScreen({ ...state, target: cycleTarget(RUN.sys, 1), focus: { ...state.focus, map: 0, missions: 0, bridge: 0 } }, 'bridge');
}
// The relight is flown: the relic is carried to the spire, the beacon charges while you hold it, then the light
// bursts out and the Echo answers (sim.js relight*). The shared numbers change when the light bursts (applyRelight).
function doRelight(state) {
  if (!(SECTOR[RUN.sys].beacon && !beaconLitIdx(RUN.sys))) return state;
  if (RUN.credits < 300) return deny(state, 'cz_t_need_cr', { n: 300 });
  if (RUN.cargo[G_RELIQ] < 1) return deny(state, 'cz_t_need_relic');
  return startExplore(state, { relight: true });
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
  if (q <= 0) return deny(state, RUN.credits < price ? 'cz_t_no_credits' : 'cz_t_hold_full');
  RUN.credits -= price * q; RUN.cargo[g] += q; persist();
  return { ...state, flash: { kind: 'pop', until: state.clock + 400 } };
}
function doSell(state, g) {
  if (RUN.cargo[g] <= 0) return deny(state, 'cz_t_nothing_to_sell');
  const price = unitSell(CONTENT, RUN.sys, g, RUN.epoch, RUN.rep), q = Math.min(state.marketQty, RUN.cargo[g]);
  RUN.credits = Math.min(9999999, RUN.credits + price * q); RUN.cargo[g] -= q; persist();
  return { ...state, flash: { kind: 'pop', until: state.clock + 400 } };
}
function doRefuel(state) {
  const price = refuelPrice(CONTENT, RUN.sys);
  const n = Math.min(state.marketQty, RUN.fuel_max - RUN.fuel, Math.floor(RUN.credits / price));
  if (n <= 0) return deny(state, RUN.fuel >= RUN.fuel_max ? 'cz_t_tank_full' : 'cz_t_no_credits');
  RUN.credits -= price * n; RUN.fuel += n; persist();
  return { ...state, flash: { kind: 'pop', until: state.clock + 400 } };
}
function shipyardReduce(state, a) {
  const rows = SHOP.length + 1;                     // upgrades + repair
  if (a.type === 'navkey' && (a.k === 'U' || a.k === 'D')) return moveFocus(state, 'shipyard', rows, a.k === 'D' ? 1 : -1);
  if (a.type === 'confirm') {
    const r = state.focus.shipyard, key = r < SHOP.length ? SHOP[r].key : 'repair';
    if (shopBuy(key, RUN)) { persist(); if (key !== 'repair' && SECTOR[RUN.sys].faction === 2) codexUnlock('c_mechanic'); return { ...state, flash: { kind: 'pop', until: state.clock + 400 } }; }
    return deny(state, 'cz_t_unavailable');
  }
  return state;
}
function missionsReduce(state, a) {
  const n = (MISSIONS ? MISSIONS.length : 0) + 1;
  if (a.type === 'navkey' && (a.k === 'U' || a.k === 'D')) { const ns = moveFocus(state, 'missions', n, a.k === 'D' ? 1 : -1); ns.sel = ns.focus.missions; return ns; }
  if (a.type === 'confirm') { const slot = state.focus.missions; const has = slot < (MISSIONS ? MISSIONS.length : 0); const cc = has ? combatFromMission(MISSIONS[slot]) : ambushCfg(); return startCombat({ ...state, sel: slot }, cc, has ? MISSIONS[slot].flavor : null); }
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

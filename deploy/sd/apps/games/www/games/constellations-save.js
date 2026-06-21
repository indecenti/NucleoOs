// constellations-save.js — cross-play campaign bridge for "Costellazioni 3D".
//
// The Cardputer native game and this web game share ONE run: the canonical /sd/data/costellazioni/
// save.bin on the device SD. The firmware exposes it as JSON at /api/game/costellazioni/save
// (registered in nucleo_app/app_constellations.cpp), so the web never parses raw struct bytes —
// it GETs/POSTs the same fields. save.bin stays the single source of truth; the Cardputer keeps
// writing it natively. Auth is the OS session cookie (same-origin) — no token plumbing.
//
// Conflict model: the struct has no timestamp, so `epoch` (the jump counter, monotonic per run) is
// the progress clock, tie-broken by missions completed, then kills, then credits. On save we first
// re-read the device copy; if it advanced past ours, the Cardputer moved the run forward while the
// tab was idle → the device wins and we reload (storeSave returns {conflict, disk}).

const URL = '/api/game/costellazioni/save';
export const SAVE_VER = 3;        // procedural universe: + seed + sector

// Fresh run — mirrors the native new_game() defaults (app_constellations.cpp new_game()).
// seed: a random u32 (only used if the WEB starts a brand-new run; normally inherited from the device).
export function newSave(seed) {
  const sd = (seed >>> 0) || ((Math.floor(Math.random() * 0xffffffff)) >>> 0) || 0xC057E11A;
  return {
    ver: SAVE_VER, credits: 600, fuel: 8, fuel_max: 8, hull: 100, hull_max: 100,
    cargo_max: 20, jump_range: 64, sensors: 0, weapon: 0, shield_max: 40, sys: 0,
    cargo: [0, 0, 0, 0, 0, 0, 0, 0], rep: [0, 0, 0, 0],
    flags: 0, beacon_lit: 0, epoch: 1, missions_done: 0, kills: 0, seed: sd, sector: 0,
  };
}

// Coerce whatever the endpoint returns into the canonical shape (defensive against partial JSON).
function normalize(s) {
  const d = newSave();
  const num = (v, def) => (typeof v === 'number' && isFinite(v) ? v : def);
  const out = { ...d };
  for (const k of ['credits', 'fuel', 'fuel_max', 'hull', 'hull_max', 'cargo_max', 'jump_range',
    'sensors', 'weapon', 'shield_max', 'sys', 'flags', 'beacon_lit', 'epoch', 'missions_done', 'kills',
    'seed', 'sector'])
    out[k] = num(s[k], d[k]);
  out.cargo = (Array.isArray(s.cargo) ? s.cargo : d.cargo).slice(0, 8).map((v) => num(v, 0));
  while (out.cargo.length < 8) out.cargo.push(0);
  out.rep = (Array.isArray(s.rep) ? s.rep : d.rep).slice(0, 4).map((v) => num(v, 0));
  while (out.rep.length < 4) out.rep.push(0);
  out.ver = num(s.ver, SAVE_VER);
  return out;
}

const popcount = (n) => { n >>>= 0; let c = 0; while (n) { c += n & 1; n >>>= 1; } return c; };
// Strict progress ordering: SECTOR (deepest wins), then epoch, then kills, then credits.
export function progressKey(s) {
  return [(s.sector || 0) >>> 0, s.epoch >>> 0, s.kills >>> 0, s.credits >>> 0];
}
function progressGreater(a, b) {
  const ka = progressKey(a), kb = progressKey(b);
  for (let i = 0; i < ka.length; i++) { if (ka[i] !== kb[i]) return ka[i] > kb[i]; }
  return false;
}

// Read the shared run. Returns the save object, or null if there is no continuable run
// (no save.bin / incompatible version / network error).
export async function loadSave() {
  try {
    const r = await fetch(URL, { credentials: 'same-origin', cache: 'no-store' });
    if (r.status === 404) return null;
    if (!r.ok) throw new Error('GET ' + r.status);
    const s = await r.json();
    if (!s || s.ver !== SAVE_VER) return null;     // future/incompatible struct → no continue, never corrupt
    return normalize(s);
  } catch (e) { console.warn('[costellazioni] loadSave failed', e); return null; }
}

// Persist the run. Epoch-merge guard: if the device copy has progressed beyond `save`, abort and
// return { ok:false, conflict:true, disk } so the UI can reload from disk and warn the player.
export async function storeSave(save) {
  const disk = await loadSave();
  if (disk && progressGreater(disk, save)) return { ok: false, conflict: true, disk };
  const body = normalize(save); body.ver = SAVE_VER;
  const r = await fetch(URL, {
    method: 'POST', credentials: 'same-origin',
    headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
  });
  if (!r.ok) throw new Error('POST ' + r.status);
  return { ok: true, ...(await r.json()) };
}

// ---- tiny bit helpers (flags / beacons / missions are bitmasks, matching the firmware) ----------
export const flagSet = (s, bit) => ((s.flags >>> 0) & (1 << bit)) !== 0;
export const beaconLit = (s, sysIdx) => ((s.beacon_lit >>> 0) & (1 << sysIdx)) !== 0;
export const missionDone = (s, missionIdx) => ((s.missions_done >>> 0) & (1 << missionIdx)) !== 0;
export function setBit(value, bit) { return (value >>> 0) | (1 << bit); }

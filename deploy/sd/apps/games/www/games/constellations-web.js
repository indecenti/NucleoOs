// constellations-web.js — the WEB-ONLY progress of Costellazioni, next to the shared run.
//
// The shared save (constellations-save.js: credits, cargo, beacon_lit, sector, ...) is the contract with the
// Cardputer and never changes shape. What only the 3D game knows lives here, in /sd/data/costellazioni/web.json
// (fs path /data/costellazioni/web.json) with a localStorage mirror for when the card cannot be reached:
//   · visited[sector]  — bitmask of the systems you have been to (the galaxy map's charted / uncharted);
//   · relit[sector]    — beacons you relit, kept after the sector advances (the shared beacon_lit resets);
//   · scanned["sec:sys"] — surveyed worlds (bit per point of interest);
//   · codex{id:order}  — unlocked codex entries, and seen{id:1} for the ones already read;
//   · relights         — how many beacons you lit (the Echo weighs it);
//   · found["sec:sys:w"] / looted[...] — sites discovered on world w of a system, relics recovered there (bit per site).
// Keyed by the run's seed: a new run (new seed) starts a fresh web save. Writes are debounced and best-effort.
const PATH = '/data/costellazioni/web.json', DIR = '/data/costellazioni';
const LS = 'cz.web';
export const WEB_VER = 1;

export function freshWeb(seed) {
  return { ver: WEB_VER, seed: seed >>> 0, visited: {}, relit: {}, scanned: {}, codex: {}, seen: {}, found: {}, looted: {}, relights: 0, n: 0 };
}
function sane(w, seed) {
  if (!w || typeof w !== 'object' || w.ver !== WEB_VER || (w.seed >>> 0) !== (seed >>> 0)) return null;
  const o = freshWeb(seed);
  for (const k of ['visited', 'relit', 'scanned', 'codex', 'seen', 'found', 'looted']) if (w[k] && typeof w[k] === 'object' && !Array.isArray(w[k])) o[k] = w[k];
  o.relights = Number.isFinite(w.relights) ? w.relights : 0; o.n = Number.isFinite(w.n) ? w.n : 0;
  return o;
}
const lsGet = () => { try { return JSON.parse(localStorage.getItem(LS) || 'null'); } catch { return null; } };
const lsSet = (w) => { try { localStorage.setItem(LS, JSON.stringify(w)); } catch {} };

// Load the web save for this run: the card's copy, else the browser's, else a fresh one (never throws).
export async function loadWeb(seed) {
  let card = null;
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(PATH), { credentials: 'same-origin', cache: 'no-store' });
    if (r.ok) card = sane(JSON.parse((await r.text()) || 'null'), seed);
  } catch {}
  const local = sane(lsGet(), seed);
  // two copies of the same run: the one that has seen more wins
  const w = card && local ? (local.n > card.n ? local : card) : card || local || freshWeb(seed);
  lsSet(w);
  return w;
}

let timer = null, writing = false, again = null, dirMade = false;
async function write(w) {
  const body = JSON.stringify(w);
  lsSet(w);
  try {
    if (!dirMade) {   // the device mkdir is not recursive: parents first
      for (const d of ['/data', DIR]) await fetch('/api/fs/mkdir?path=' + encodeURIComponent(d), { method: 'POST', credentials: 'same-origin' }).catch(() => {});
      dirMade = true;
    }
    const r = await fetch('/api/fs/write?path=' + encodeURIComponent(PATH), { method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/octet-stream' }, body });
    if (!r.ok) dirMade = false;
    return r.ok;
  } catch { dirMade = false; return false; }
}
// Debounced save (one write per burst of changes; a write never overlaps another).
export function saveWeb(w, now = false) {
  if (!w) return;
  w.n = (w.n | 0) + 1;
  lsSet(w);
  clearTimeout(timer);
  const go = async () => {
    if (writing) { again = w; return; }
    writing = true;
    try { await write(w); } finally { writing = false; }
    if (again) { again = null; go(); }
  };
  if (now) go(); else timer = setTimeout(go, 700);
}

// ---- small helpers (pure; the caller saves) -------------------------------------------------------------------
const bit = (m, i) => ((m >>> 0) & (1 << i)) !== 0;
export const visited = (w, sector, sys) => !!w && bit(w.visited[sector] || 0, sys);
export function markVisited(w, sector, sys) { if (!w || visited(w, sector, sys)) return false; w.visited[sector] = ((w.visited[sector] || 0) | (1 << sys)) >>> 0; return true; }
export const wasRelit = (w, sector, sys) => !!w && bit(w.relit[sector] || 0, sys);
export function markRelit(w, sector, sys) { if (!w) return; w.relit[sector] = ((w.relit[sector] || 0) | (1 << sys)) >>> 0; w.relights = (w.relights | 0) + 1; }
export function markScanned(w, sector, sys, poi) { if (!w) return false; const k = sector + ':' + sys, m = w.scanned[k] || 0; if (bit(m, poi)) return false; w.scanned[k] = (m | (1 << poi)) >>> 0; return true; }
export const scanned = (w, sector, sys, poi) => !!w && bit(w.scanned[sector + ':' + sys] || 0, poi);
export const hasCodex = (w, id) => !!w && w.codex[id] != null;
export function unlock(w, id) { if (!w || w.codex[id] != null) return false; w.codex[id] = Object.keys(w.codex).length; return true; }
export function markSeen(w, id) { if (!w || w.seen[id]) return false; w.seen[id] = 1; return true; }
// M3: surface sites (per world of a system: "sector:sys:world" -> bitmask of site indices)
const wkey = (sector, sys, wi) => sector + ':' + sys + ':' + wi;
export const foundMask = (w, sector, sys, wi) => (w && w.found ? (w.found[wkey(sector, sys, wi)] >>> 0) || 0 : 0);
export function markFound(w, sector, sys, wi, i) { if (!w) return false; w.found = w.found || {}; const k = wkey(sector, sys, wi), m = w.found[k] >>> 0; if (bit(m, i)) return false; w.found[k] = (m | (1 << i)) >>> 0; return true; }
export const wasLooted = (w, sector, sys, wi, i) => !!w && !!w.looted && bit(w.looted[wkey(sector, sys, wi)] || 0, i);
export function markLooted(w, sector, sys, wi, i) { if (!w) return false; w.looted = w.looted || {}; const k = wkey(sector, sys, wi), m = w.looted[k] >>> 0; if (bit(m, i)) return false; w.looted[k] = (m | (1 << i)) >>> 0; return true; }
export const foundCount = (w) => { let n = 0; if (w && w.found) for (const k in w.found) { let m = w.found[k] >>> 0; while (m) { n += m & 1; m >>>= 1; } } return n; };

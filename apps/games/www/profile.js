// profile.js — NucleoOS player identity + stats ("gamercard"). The Xbox-gamertag analogue, but
// local-first and device-owned: profile and per-game stats live in localStorage AND on the SD
// (/data/play/*.json) so every surface (Cardputer, web, another browser) shares one identity.
//
// The stat mutation is a PURE function (applyResult) so it can be unit-tested off-device.
import { _fs as FS } from '/apps/games/nucleo-play.js';

const PROFILE_KEY = 'play.profile';
const PROFILE_PATH = '/data/play/profile.json';
const STATS_PATH = '/data/play/stats.json';

export const AVATARS = ['🦊', '🐼', '🐲', '🦉', '🐙', '🦁', '🐝', '🦄', '👾', '🤖', '🐺', '🦅', '🐬', '🦂', '🐧', '🦖'];
export const COLORS = ['#22d3ee', '#e879f9', '#fbbf24', '#34d399', '#f87171', '#60a5fa', '#a78bfa', '#fb923c'];

// ---- profile ---------------------------------------------------------------------------------
export function getProfile() {
  let p = null;
  try { p = JSON.parse(localStorage.getItem(PROFILE_KEY) || 'null'); } catch {}
  if (!p || !p.name) {
    p = { name: 'Player' + ((Math.random() * 900 + 100) | 0), avatar: AVATARS[(Math.random() * AVATARS.length) | 0], color: COLORS[(Math.random() * COLORS.length) | 0], created: Date.now() };
  }
  return p;
}

export async function setProfile(p) {
  const clean = { name: (p.name || 'Player').slice(0, 16).trim() || 'Player', avatar: p.avatar || AVATARS[0], color: p.color || COLORS[0], created: p.created || Date.now() };
  localStorage.setItem(PROFILE_KEY, JSON.stringify(clean));
  FS.mkdir('/data/play').then(() => FS.writeJSON(PROFILE_PATH, clean)).catch(() => {});   // best-effort SD mirror
  return clean;
}

// ---- stats -----------------------------------------------------------------------------------
// Shape: { [gameId]: { w, l, d, games, streak, best }, _total: {…} }
const blank = () => ({ w: 0, l: 0, d: 0, games: 0, streak: 0, best: 0 });

// PURE: apply one outcome ('win'|'loss'|'draw') for a game, returning a NEW stats object.
export function applyResult(stats, gameId, outcome) {
  const s = JSON.parse(JSON.stringify(stats || {}));
  for (const key of [gameId, '_total']) {
    const g = s[key] || blank();
    g.games++;
    if (outcome === 'win') { g.w++; g.streak++; if (g.streak > g.best) g.best = g.streak; }
    else if (outcome === 'loss') { g.l++; g.streak = 0; }
    else { g.d++; g.streak = 0; }
    s[key] = g;
  }
  return s;
}

export async function loadStats() {
  const s = await FS.readJSON(STATS_PATH);
  return s || {};
}

// The read half of a read-modify-write. ONLY a 404 (or an empty file) means "no stats yet": a busy device
// (503), a lost session (401), a network drop or a garbled body THROWS. loadStats() above reads any of those
// as {} — fine for painting the card, fatal here, where it rewrote stats.json with this one match and wiped
// the player's whole record.
async function readStatsForUpdate() {
  const r = await fetch('/api/fs/read?path=' + encodeURIComponent(STATS_PATH), { credentials: 'same-origin', cache: 'no-store' });
  if (r.status === 404) return {};
  if (!r.ok) throw new Error('stats read failed: HTTP ' + r.status);
  const txt = (await r.text()).trim();
  if (!txt) return {};
  const s = JSON.parse(txt);                          // a garbled body throws: never rebuild over it
  if (!s || typeof s !== 'object' || Array.isArray(s)) throw new Error('stats file is not an object');
  return s;
}

// Rejects (nothing written) when the current record cannot be read or the write fails.
export async function recordResult(gameId, outcome) {
  const cur = await readStatsForUpdate();
  const next = applyResult(cur, gameId, outcome);
  await FS.mkdir('/data/play');
  if (!(await FS.writeJSON(STATS_PATH, next))) throw new Error('stats write failed');
  return next;
}

// A one-line summary for a game (or overall with '_total').
export function summarize(stats, key = '_total') {
  const g = (stats && stats[key]) || blank();
  const rate = g.games ? Math.round((g.w / g.games) * 100) : 0;
  return { ...g, winRate: rate };
}

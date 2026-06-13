// NucleoOS file index — the brains behind taskbar/Start search.
//
// The device exposes only /api/fs/list (no native search). Hitting it per keystroke is slow
// and hammers the ESP, so instead we crawl the SD ONCE (breadth-first, capped), keep a flat
// in-memory index, and answer every search from RAM — instant, zero network. The index is:
//   • persisted to localStorage so a returning browser searches immediately (cold start),
//     then silently revalidated against the live device in the background;
//   • refreshed (debounced) whenever the device reports an fs.changed event, so results stay
//     fresh without polling;
//   • bounded (depth + total directory reads) to stay kind to the ESP32 and its SD card.
//
// Everything here is framework-free and self-contained; the shell just calls search()/ensure().

const ROOTS = ['/data'];
const MAX_DEPTH = 6;            // how deep into /data we descend
const MAX_DIRS = 600;          // hard cap on directory reads per full crawl (ESP-friendly)
const PERSIST_KEY = 'nucleo.fsindex.v1';
const PERSIST_TTL = 10 * 60 * 1000;   // a stored index is shown instantly if younger than this
const REFRESH_DEBOUNCE = 1500;        // coalesce bursts of fs.changed into one re-crawl

// Map an extension to a coarse category used for grouping + glyphs in the UI.
const CAT = {
  image: new Set(['jpg', 'jpeg', 'png', 'bmp', 'gif', 'webp', 'svg']),
  audio: new Set(['mp3', 'wav', 'ogg', 'flac', 'm4a', 'aac']),
  video: new Set(['nfv', 'mp4', 'webm', 'mov', 'mkv', 'avi']),
  doc: new Set(['txt', 'md', 'pdf', 'doc', 'docx', 'rtf', 'log']),
  code: new Set(['js', 'mjs', 'ts', 'json', 'c', 'h', 'cpp', 'py', 'sh', 'html', 'css', 'xml', 'yaml', 'yml', 'toml', 'ini', 'cfg', 'csv']),
  game: new Set(['jsdos', 'com', 'exe']),
};
export function categoryOf(name, isDir) {
  if (isDir) return 'folder';
  const ext = (name.split('.').pop() || '').toLowerCase();
  for (const c in CAT) if (CAT[c].has(ext)) return c;
  return 'other';
}

let index = [];          // [{ path, name, lower, dir, ext, isDir, cat }]
let builtAt = 0;         // timestamp of the last successful crawl (0 = never)
let building = null;     // in-flight crawl promise (dedupes concurrent callers)
let refreshTimer = null;
const updateCbs = new Set();   // notified after every (re)build so the UI can repaint

const host = (() => { try { return location.host; } catch { return ''; } })();
const now = () => Date.now();

// ---- persistence (instant cold-start, revalidated in the background) ----
function loadPersisted() {
  try {
    const raw = localStorage.getItem(PERSIST_KEY);
    if (!raw) return false;
    const o = JSON.parse(raw);
    if (!o || o.host !== host || !Array.isArray(o.items)) return false;
    index = o.items;
    builtAt = o.builtAt || 0;
    return (now() - builtAt) < PERSIST_TTL;     // fresh enough to trust until revalidation
  } catch { return false; }
}
function persist() {
  try { localStorage.setItem(PERSIST_KEY, JSON.stringify({ host, builtAt, items: index })); } catch {}
}

// ---- crawl ----
async function listDir(path) {
  try {
    const r = await fetch('/api/fs/list?path=' + encodeURIComponent(path), { cache: 'no-store' });
    if (!r.ok) return null;
    const d = await r.json();
    return Array.isArray(d.entries) ? d.entries : [];
  } catch { return null; }
}

async function crawl() {
  const out = [];
  const queue = ROOTS.map((p) => ({ path: p, depth: 0 }));
  let reads = 0;
  while (queue.length && reads < MAX_DIRS) {
    const { path, depth } = queue.shift();
    reads++;
    const entries = await listDir(path);
    if (!entries) continue;
    for (const e of entries) {
      const isDir = e.type === 'dir';
      const full = path.endsWith('/') ? path + e.name : path + '/' + e.name;
      out.push({
        path: full, name: e.name, lower: e.name.toLowerCase(),
        dir: path, isDir, size: e.size || 0,
        ext: isDir ? '' : (e.name.split('.').pop() || '').toLowerCase(),
        cat: categoryOf(e.name, isDir),
      });
      if (isDir && depth + 1 < MAX_DEPTH) queue.push({ path: full, depth: depth + 1 });
    }
  }
  return out;
}

// Build (or rebuild) the index. Concurrent callers share one crawl. `force` ignores freshness.
export function ensure(force = false) {
  if (building) return building;
  if (!force && builtAt && (now() - builtAt) < PERSIST_TTL) return Promise.resolve(index);
  building = (async () => {
    const items = await crawl();
    // A crawl that returns nothing (device unreachable mid-session) shouldn't wipe a good index.
    if (items.length || !index.length) { index = items; builtAt = now(); persist(); }
    building = null;
    for (const cb of updateCbs) try { cb(); } catch {}
    return index;
  })();
  return building;
}

// The device changed something on disk — schedule a debounced rebuild so search stays fresh.
export function invalidate() {
  clearTimeout(refreshTimer);
  refreshTimer = setTimeout(() => ensure(true), REFRESH_DEBOUNCE);
}

export function onUpdate(cb) { updateCbs.add(cb); return () => updateCbs.delete(cb); }
export const isReady = () => builtAt > 0;
export const size = () => index.length;

// ---- ranking ----
// Higher is better. Rewards prefix and word-boundary matches over buried substrings, penalises
// depth and length a touch so the shortest, shallowest, closest name wins — like a real OS.
function scoreEntry(it, terms) {
  let score = 0;
  for (const t of terms) {
    const name = it.lower, path = it.path.toLowerCase();
    let s;
    if (name === t) s = 1000;
    else if (name.startsWith(t)) s = 760;
    else if (wordStarts(name, t)) s = 620;
    else if (name.includes(t)) s = 440;
    else if (path.includes(t)) s = 220;
    else return -1;                    // every term must match somewhere (AND semantics)
    score += s;
  }
  score += it.isDir ? 30 : 0;          // folders are useful navigation targets
  score -= (it.path.split('/').length - 2) * 8;   // shallower wins
  score -= Math.min(it.name.length, 40) * 0.4;    // shorter wins
  return score;
}
// True if `t` starts a word in `s` (after a separator), e.g. "los" matches "My Bones — Losing".
function wordStarts(s, t) {
  let i = s.indexOf(t);
  while (i > 0) {
    if (/[\s_\-.\/()]/.test(s[i - 1])) return true;
    i = s.indexOf(t, i + 1);
  }
  return i === 0;
}

// Search the in-memory index. Returns ranked [{ ...entry, cat }]. Pure + synchronous — call
// ensure() once up front (or rely on the persisted index) so this stays instant per keystroke.
export function search(query, limit = 40) {
  const q = (query || '').trim().toLowerCase();
  if (!q) return [];
  const terms = q.split(/\s+/).filter(Boolean);
  const scored = [];
  for (const it of index) {
    const sc = scoreEntry(it, terms);
    if (sc >= 0) scored.push({ it, sc });
  }
  scored.sort((a, b) => b.sc - a.sc);
  return scored.slice(0, limit).map((x) => x.it);
}

// Initialise from the persisted index (instant), then revalidate against the device.
export function init() {
  const warm = loadPersisted();
  if (index.length) for (const cb of updateCbs) try { cb(); } catch {}
  // Revalidate now if cold, or shortly after if warm (don't compete with first paint).
  if (warm) setTimeout(() => ensure(true), 4000);
  else ensure(true);
}

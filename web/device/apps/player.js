// Music — device-simulator app. Mirrors firmware app_player.cpp.
// Evolved: reads shared music_db.jsonl for metadata (title, artist, genre, plays, fav).
// Supports: full-text search (TAB), filter by Favorites (f), Most Played (t), Artist/Genre.
import { makeListState, drawFocusList } from './_list.js';
const lst = makeListState();

const MUSIC_DIR  = '/data/Music';
const DB_PATH    = '/system/config/music_db.jsonl';
const AUDIO_RE   = /\.(mp3|wav)$/i;
const ACCENT     = '#ff7eb6';
const GOOD       = '#7CFC9A';
const clampi     = (v, a, b) => Math.max(a, Math.min(b, v));
const fmt        = s => `${(s / 60) | 0}:${String((s | 0) % 60).padStart(2, '0')}`;

// ---- State ------------------------------------------------------------------
let db        = [];          // parsed from music_db.jsonl [{path,title,artist,genre,plays,fav}]
let names     = [];          // current displayed list (path strings)
let meta      = [];          // parallel metadata for current list
let sel       = 0;
let playingIdx = -1;
let mode      = 'list';       // 'list' | 'play' | 'search' | 'filter'
let searchQ   = '';
let filterMode = 'all';      // 'all' | 'fav' | 'top' | 'artist:X' | 'genre:X'
let filterSel  = 0;          // selection within filter sub-list
let filterItems = [];        // items for filter sub-list
let filterType  = '';        // 'artist' | 'genre'

const audio = new Audio();
audio.preload = 'none';

// ---- DB loading -------------------------------------------------------------
async function loadDB() {
  try {
    const text = await (await fetch('/api/fs/read?path=' + encodeURIComponent(DB_PATH))).text();
    db = text.trim().split('\n').filter(Boolean).map(l => {
      try { return JSON.parse(l); } catch { return null; }
    }).filter(Boolean);
  } catch { db = []; }
}

function applyFilter() {
  let src = db.length ? db : [];
  if (filterMode === 'fav') {
    src = src.filter(t => t.fav);
  } else if (filterMode === 'top') {
    src = [...src].sort((a, b) => (b.plays || 0) - (a.plays || 0));
  } else if (filterMode.startsWith('artist:')) {
    const art = filterMode.slice(7);
    src = src.filter(t => (t.artist || '') === art);
  } else if (filterMode.startsWith('genre:')) {
    const gen = filterMode.slice(6);
    src = src.filter(t => (t.genre || '') === gen);
  }
  if (searchQ) {
    const q = searchQ.toLowerCase();
    src = src.filter(t =>
      (t.title || '').toLowerCase().includes(q) ||
      (t.artist || '').toLowerCase().includes(q) ||
      (t.path || '').toLowerCase().includes(q)
    );
  }
  names = src.map(t => t.path);
  meta  = src;
}

async function scan() {
  sel = 0;
  if (db.length === 0) await loadDB();
  if (db.length > 0) {
    applyFilter();
  } else {
    // Fallback: raw directory listing (no DB yet)
    try {
      const r = await (await fetch('/api/fs/list?path=' + encodeURIComponent(MUSIC_DIR))).json();
      names = (r.entries || []).filter(e => e.type === 'file' && AUDIO_RE.test(e.name))
        .map(e => e.name).sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }));
      meta = names.map(n => ({ path: n, title: n, artist: '', genre: '', plays: 0, fav: false }));
    } catch { names = []; meta = []; }
  }
}

// ---- Playback ---------------------------------------------------------------
function play(i) {
  if (i < 0 || i >= names.length) return;
  audio.src = '/api/fs/read?path=' + encodeURIComponent(MUSIC_DIR + '/' + names[i]);
  audio.currentTime = 0;
  audio.play().catch(() => {});
  playingIdx = i;
}
function stop() { audio.pause(); audio.currentTime = 0; playingIdx = -1; }
function togglePause() {
  if (playingIdx < 0) return;
  if (audio.paused) audio.play().catch(() => {}); else audio.pause();
}
function seek(ds) {
  if (playingIdx < 0) return;
  const dur = isFinite(audio.duration) ? audio.duration : 0;
  audio.currentTime = clampi((audio.currentTime || 0) + ds, 0, dur || 1e9);
}

audio.addEventListener('ended', () => {
  if (playingIdx >= 0 && playingIdx < names.length - 1) play(playingIdx + 1);
  else { playingIdx = -1; mode = 'list'; }
});

// ---- Unique helpers ---------------------------------------------------------
function uniqueField(field) {
  const seen = new Set();
  return db.map(t => t[field] || '').filter(v => v && v !== 'Unknown' && !seen.has(v) && seen.add(v));
}

// ---- App export -------------------------------------------------------------
export const playerApp = {
  id: 'music',

  enter() { scan(); mode = 'list'; filterMode = 'all'; searchQ = ''; return { hint: 'enter play  space pause  tab search  f fav  t top' }; },
  exit()  { stop(); mode = 'list'; },

  key(key, ch) {
    // ---- search typing mode -------------------------------------------------
    if (mode === 'search') {
      if (key === 'backspace') { searchQ = searchQ.slice(0, -1); applyFilter(); }
      else if (key === 'escape' || key === 'tab') { mode = 'list'; }
      else if (key === 'enter') { mode = 'list'; sel = 0; }
      else if (ch && ch >= ' ') { searchQ += ch; applyFilter(); }
      return;
    }

    // ---- filter sub-list mode (artist/genre picker) -------------------------
    if (mode === 'filter') {
      if (key === 'escape') { mode = 'list'; }
      else if (key === 'up')    { filterSel = (filterSel + filterItems.length - 1) % filterItems.length; }
      else if (key === 'down')  { filterSel = (filterSel + 1) % filterItems.length; }
      else if (key === 'enter') {
        filterMode = filterType + ':' + filterItems[filterSel];
        applyFilter(); sel = 0; mode = 'list';
      }
      return;
    }

    // ---- Now Playing controller ---------------------------------------------
    if (mode === 'play') {
      const isDigit = typeof ch === 'string' && ch >= '0' && ch <= '9';
      if (key === 'backspace' || key === 'tab') mode = 'list';
      else if (ch === 's')                      { stop(); mode = 'list'; }
      else if (key === 'enter' || ch === ' ' || ch === 'p') togglePause();
      else if (key === 'right' || ch === '/')   seek(10);
      else if (ch === ',')                      seek(-10);
      else if (key === 'up' || ch === ';')      audio.volume = clampi(Math.round((audio.volume + 0.1) * 10) / 10, 0, 1);
      else if (key === 'down' || ch === '.')    audio.volume = clampi(Math.round((audio.volume - 0.1) * 10) / 10, 0, 1);
      else if (ch === '[' && playingIdx > 0)                       play(playingIdx - 1);
      else if (ch === ']' && playingIdx >= 0 && playingIdx < names.length - 1) play(playingIdx + 1);
      else if (isDigit) { const dur = isFinite(audio.duration) ? audio.duration : 0; if (dur) audio.currentTime = clampi((+ch) * dur / 10, 0, dur); }
      return;
    }

    // ---- List mode ----------------------------------------------------------
    if (key === 'up')    { if (names.length) sel = (sel + names.length - 1) % names.length; }
    else if (key === 'down')  { if (names.length) sel = (sel + 1) % names.length; }
    else if (key === 'enter' && names.length) {
      if (sel !== playingIdx || audio.paused || audio.ended) play(sel);
      mode = 'play';
    }
    else if (key === 'tab')  { mode = 'search'; }
    else if (ch === ' ' || ch === 'p') togglePause();
    else if (ch === 's')  stop();
    else if (ch === 'f')  { filterMode = filterMode === 'fav' ? 'all' : 'fav'; applyFilter(); sel = 0; }
    else if (ch === 't')  { filterMode = filterMode === 'top' ? 'all' : 'top'; applyFilter(); sel = 0; }
    else if (ch === 'a')  { filterItems = uniqueField('artist'); if (filterItems.length) { filterType='artist'; filterSel=0; mode='filter'; } }
    else if (ch === 'g')  { filterItems = uniqueField('genre');  if (filterItems.length) { filterType='genre';  filterSel=0; mode='filter'; } }
    else if (ch === 'c')  { filterMode = 'all'; searchQ = ''; applyFilter(); sel = 0; } // clear filters
  },

  draw(g) {
    if (mode === 'play')   return drawController(g);
    if (mode === 'search') return drawSearch(g);
    if (mode === 'filter') return drawFilterList(g);
    return drawList(g);
  },
};

// ---- Draw: list -------------------------------------------------------------
function drawList(g) {
  const top = g.contentTop, h = g.contentH;

  // Header with filter badge
  g.text('Music', 10, top + 8, ACCENT, 11, 'bold');
  if (filterMode !== 'all' || searchQ) {
    const badge = searchQ ? `"${searchQ.slice(0,8)}"` : filterMode.replace(':', ': ').toUpperCase();
    g.roundRect(g.W - badge.length * 5.5 - 14, top + 2, badge.length * 5.5 + 10, 12, 6, '#3a0a22');
    g.text(badge, g.W - badge.length * 5.5 - 9, top + 8, ACCENT, 8, 'bold');
  }

  // Mini strip: now playing
  const playing = playingIdx >= 0 && !audio.ended;
  const icon = playingIdx < 0 ? '[]' : audio.paused ? '||' : '▶';
  g.text(icon, 10, top + 24, playing && !audio.paused ? GOOD : g.COL.muted, 9, 'bold');
  const nowMeta = playingIdx >= 0 ? meta[playingIdx] : null;
  const nowLabel = nowMeta
    ? g.clamp((nowMeta.title && nowMeta.title !== nowMeta.path ? nowMeta.title : nowMeta.path), 26)
    : 'nothing playing';
  g.text(nowLabel, 28, top + 24, g.COL.fg, 9);
  if (nowMeta?.fav) g.text('♥', g.W - 16, top + 24, ACCENT, 9);

  const el = audio.currentTime || 0, du = isFinite(audio.duration) ? audio.duration : 0;
  g.text(du ? `${fmt(el)}/${fmt(du)}` : fmt(el), g.W - (nowMeta?.fav ? 20 : 8), top + 24, g.COL.muted, 8, 'normal', 'right');
  const pct = du ? Math.min(100, el / du * 100) : 0;
  g.roundRect(10, top + 34, 220, 4, 2, g.COL.line);
  if (pct > 0) g.roundRect(10, top + 34, 220 * pct / 100, 4, 2, GOOD);

  if (!names.length) {
    g.text(db.length ? 'No results' : 'No tracks in /data/Music', 12, top + 56, g.COL.dim, 9);
    return { hint: 'tab search  c clear  esc back' };
  }
  drawFocusList(g, lst, {
    top: top + 42, h: h - 42, count: names.length, sel, now: g.now,
    label: i => {
      const m = meta[i];
      if (!m) return names[i];
      const title = (m.title && m.title !== m.path) ? m.title : m.path.replace(/\.[^.]+$/, '');
      return m.fav ? `♥ ${title}` : title;
    },
    sublabel: i => {
      const m = meta[i];
      if (!m || !m.artist || m.artist === 'Unknown') return '';
      return m.plays > 0 ? `${m.artist} · ${m.plays}▶` : m.artist;
    },
    color:  () => ACCENT,
    marked: i => i === playingIdx,
  });
  return { instruction: 'Play MP3/WAV · TAB search · f fav · t top · a artist · g genre · c clear', hint: 'enter play  space pause  tab search' };
}

// ---- Draw: search overlay ---------------------------------------------------
function drawSearch(g) {
  const W = g.W, ct = g.contentTop;
  g.ctx.fillStyle = g.COL.bg; g.ctx.fillRect(0, 0, W, g.H);
  g.roundRect(10, ct + 8, W - 20, 22, 4, '#18203e');
  g.roundRect(10, ct + 8, W - 20, 22, 4, ACCENT, false, true); // stroke
  g.text('🔍 ' + (searchQ || ' '), 18, ct + 19, searchQ ? g.COL.fg : g.COL.muted, 9);
  // blinking cursor effect
  g.text('_', 18 + (searchQ.length + 2) * 5.5, ct + 19, ACCENT, 9);

  g.text(`${names.length} result${names.length !== 1 ? 's' : ''}`, W - 8, ct + 19, g.COL.muted, 8, 'normal', 'right');
  g.drawFastHLine && g.drawFastHLine(10, ct + 32, W - 20, g.COL.line);

  drawFocusList(g, lst, {
    top: ct + 36, h: g.contentH - 36, count: names.length, sel, now: g.now,
    label: i => { const m = meta[i]; return (m?.title && m.title !== m.path) ? m.title : names[i]; },
    sublabel: i => meta[i]?.artist && meta[i].artist !== 'Unknown' ? meta[i].artist : '',
    color: () => ACCENT,
    marked: i => i === playingIdx,
  });
  return { hint: 'type to search  enter confirm  esc back' };
}

// ---- Draw: filter sub-list (artist / genre picker) --------------------------
function drawFilterList(g) {
  const W = g.W, ct = g.contentTop;
  g.ctx.fillStyle = g.COL.bg; g.ctx.fillRect(0, 0, W, g.H);
  const title = filterType === 'artist' ? 'Filter by Artist' : 'Filter by Genre';
  g.text(title, 10, ct + 8, ACCENT, 10, 'bold');
  g.roundRect(10, ct + 18, W - 20, 1, 0, g.COL.line);

  if (!filterItems.length) { g.text('No entries', 12, ct + 32, g.COL.dim, 9); return { hint: 'esc back' }; }

  const vis = 6, rowH = 16;
  let scroll = filterSel - Math.floor(vis / 2);
  scroll = Math.max(0, Math.min(filterItems.length - vis, scroll));

  for (let i = 0; i < vis && scroll + i < filterItems.length; i++) {
    const idx = scroll + i, y = ct + 22 + i * rowH;
    const focus = idx === filterSel;
    if (focus) g.roundRect(10, y, W - 20, rowH, 2, ACCENT);
    g.text(filterItems[idx], 18, y + rowH / 2 + 1, focus ? '#04122b' : g.COL.fg, 9, focus ? 'bold' : 'normal');
  }

  // Scrollbar
  if (filterItems.length > vis) {
    const kh = Math.max(8, vis * rowH * vis / filterItems.length);
    const ky = ct + 22 + (vis * rowH - kh) * filterSel / (filterItems.length - 1);
    g.roundRect(W - 4, ky, 2, kh, 1, ACCENT);
  }
  return { hint: 'up/down select  enter filter  esc back' };
}

// ---- Draw: Now Playing full-screen controller --------------------------------
function drawController(g) {
  const ctx = g.ctx, W = g.W, H = g.H;
  ctx.fillStyle = g.COL.bg; ctx.fillRect(0, 0, W, H);
  const paused = audio.paused, playing = playingIdx >= 0 && !audio.ended;
  const m = meta[playingIdx] || {};

  g.text('NOW PLAYING', 10, g.contentTop + 4, g.COL.muted, 8);
  g.text(`${playingIdx + 1}/${names.length}`, W - 8, g.contentTop + 4, g.COL.muted, 8, 'normal', 'right');

  // status chip + title
  g.roundRect(10, 28, 18, 18, 4, paused ? ACCENT : (playing ? GOOD : g.COL.muted));
  g.text(paused ? '❚❚' : '▶', 19, 37, '#04122b', 8, 'bold', 'center');
  const title = (m.title && m.title !== m.path) ? m.title : (m.path || '').replace(/\.[^.]+$/, '');
  g.text(g.clamp(title, 28), 36, 33, playing ? g.COL.fg : g.COL.muted, 10, 'bold');
  if (m.artist && m.artist !== 'Unknown') g.text(g.clamp(m.artist, 32), 36, 43, g.COL.muted, 8);

  // Fav heart + play count
  if (m.fav)       g.text('♥', W - 18, 32, ACCENT, 10, 'bold');
  if (m.plays > 0) g.text(`${m.plays}▶`, W - (m.fav ? 32 : 10), 43, g.COL.muted, 8, 'normal', 'right');

  // progress bar
  const el = audio.currentTime || 0, du = isFinite(audio.duration) ? audio.duration : 0;
  const px = 10, pw = W - 20, py = 60;
  g.roundRect(px, py, pw, 4, 2, g.COL.line);
  const pct = du ? Math.min(1, el / du) : 0;
  if (pct > 0) g.roundRect(px, py, pw * pct, 4, 2, playing ? GOOD : g.COL.muted);
  g.circle(px + pw * pct, py + 2, 3, '#fff');
  g.text(du ? `${fmt(el)} / ${fmt(du)}` : fmt(el), 10, py + 14, g.COL.muted, 8);

  // Genre badge
  if (m.genre && m.genre !== 'Unknown') {
    const gb = m.genre.slice(0, 12);
    g.roundRect(W - gb.length * 5.5 - 14, py + 10, gb.length * 5.5 + 10, 10, 5, '#18203e');
    g.text(gb, W - gb.length * 5.5 - 9, py + 16, g.COL.muted, 7);
  }

  // volume
  g.text('VOL', 10, 90, g.COL.muted, 8);
  const bx = 40, segs = 10, sw = 14, gap = 2, on = Math.round(audio.volume * segs);
  for (let i = 0; i < segs; i++) g.roundRect(bx + i * (sw + gap), 87, sw, 7, 1, i < on ? GOOD : '#10223e');
  g.text(Math.round(audio.volume * 100) + '%', bx + segs * (sw + gap) + 4, 90, g.COL.muted, 8);

  g.text(', / seek   ; . vol   space pause', 10, 108, g.COL.dim, 8);
  g.text('[ ] track   s stop   esc list', 10, 120, g.COL.dim, 8);
  return { fullscreen: true };
}

// Test/inspection hook.
export function _debugState() {
  return { mode, n: names.length, sel, playingIdx, paused: audio.paused,
    src: audio.src.split('/').pop(), filterMode, searchQ, dbLoaded: db.length,
    t: Math.round((audio.currentTime || 0) * 10) / 10, vol: Math.round(audio.volume * 100), dur: Math.round(audio.duration || 0) };
}

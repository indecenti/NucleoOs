/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-state.js — the file manager's pure logic: sorting, hidden files and the filter, navigation history,
// remembered preferences, sizes and dates, error words, keyboard movement in the list, type-ahead.
// No DOM, no fs: tests/suite/file-commander-state.test.mjs drives all of it in node.
import { typeSortKey } from './fm-types.js';

// ── sorting ──
export const SORT_KEYS = ['name', 'mtime', 'type', 'size'];
const collator = (() => { try { return new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' }); } catch { return null; } })();
export const compareNames = (a, b) => (collator ? collator.compare(a, b) : (a < b ? -1 : a > b ? 1 : 0)) || (a < b ? -1 : a > b ? 1 : 0);

// The device may give mtime in seconds or milliseconds, as a number or a date string: → ms, or null.
export function normMtime(v) {
  if (v == null || v === '') return null;
  if (typeof v === 'number' || /^\d+(\.\d+)?$/.test(String(v))) {
    const n = Number(v);
    if (!Number.isFinite(n) || n <= 0) return null;
    return n < 1e11 ? Math.round(n * 1000) : Math.round(n);
  }
  const d = Date.parse(String(v));
  return Number.isFinite(d) ? d : null;
}
export const hasMtime = (entries) => entries.some((e) => normMtime(e.mtime) != null);

// Folders always first (both directions, like every desktop file manager), then the key, then the name.
export function compareEntries(a, b, { key = 'name', dir = 1 } = {}) {
  const da = a.type === 'dir', db = b.type === 'dir';
  if (da !== db) return da ? -1 : 1;
  let c = 0;
  if (key === 'size') c = (Number(a.size) || 0) - (Number(b.size) || 0);
  else if (key === 'mtime') c = (normMtime(a.mtime) || 0) - (normMtime(b.mtime) || 0);
  else if (key === 'type') c = compareNames(typeSortKey(a.name, da), typeSortKey(b.name, db));
  if (!c) c = compareNames(String(a.name), String(b.name));
  return c * (dir < 0 ? -1 : 1);
}
export const sortEntries = (list, opts) => list.slice().sort((a, b) => compareEntries(a, b, opts));

// ── what is shown ──
export const isHidden = (name) => String(name).startsWith('.');
export function visibleEntries(entries, { showHidden = false, filter = '' } = {}) {
  const f = String(filter || '').trim().toLowerCase();
  return entries.filter((e) => (showHidden || !isHidden(e.name)) && (!f || String(e.name).toLowerCase().includes(f)));
}

// ── back / forward ──
// Places are plain objects compared by `same` (default: JSON equality). Going somewhere new drops the forward
// branch, like a browser; revisiting the place already shown adds nothing.
export function createHistory({ cap = 60, same = (a, b) => JSON.stringify(a) === JSON.stringify(b) } = {}) {
  let list = [], at = -1;
  return {
    get current() { return at >= 0 ? list[at] : null; },
    get canBack() { return at > 0; },
    get canForward() { return at >= 0 && at < list.length - 1; },
    push(place) {
      if (at >= 0 && same(list[at], place)) return false;
      list = list.slice(0, at + 1);
      list.push(place);
      if (list.length > cap) list = list.slice(list.length - cap);
      at = list.length - 1;
      return true;
    },
    replace(place) { if (at < 0) { list = [place]; at = 0; } else list[at] = place; },
    back() { if (at > 0) at--; return this.current; },
    forward() { if (at < list.length - 1) at++; return this.current; },
    // A place that is gone (a removed location) is dropped everywhere.
    drop(pred) {
      const cur = list[at];
      list = list.filter((p) => !pred(p));
      at = Math.min(Math.max(0, list.indexOf(cur)), list.length - 1);
    },
    get size() { return list.length; },
  };
}

// ── preferences (per user, in this browser) ──
export const VIEWS = ['details', 'list', 'icons'];
export const DEFAULT_PREFS = { view: 'details', sortKey: 'name', sortDir: 1, showHidden: false, preview: false, favorites: [], searchDeep: false };
export function sanitizePrefs(raw) {
  const p = { ...DEFAULT_PREFS };
  if (!raw || typeof raw !== 'object') return p;
  if (VIEWS.includes(raw.view)) p.view = raw.view;
  if (SORT_KEYS.includes(raw.sortKey)) p.sortKey = raw.sortKey;
  if (raw.sortDir === -1 || raw.sortDir === 1) p.sortDir = raw.sortDir;
  p.showHidden = raw.showHidden === true;
  p.preview = raw.preview === true;
  p.searchDeep = raw.searchDeep === true;
  if (Array.isArray(raw.favorites)) {
    p.favorites = [...new Set(raw.favorites.filter((x) => typeof x === 'string' && x.startsWith('/')))].slice(0, 40);
  }
  return p;
}
export const PREFS_KEY = 'fc.prefs';
export function loadPrefs(storage) {
  try { return sanitizePrefs(JSON.parse(storage.getItem(PREFS_KEY) || 'null')); } catch { return { ...DEFAULT_PREFS }; }
}
export function savePrefs(storage, prefs) {
  try { storage.setItem(PREFS_KEY, JSON.stringify(sanitizePrefs(prefs))); return true; } catch { return false; }
}

// ── numbers the user reads ──
export function formatSize(bytes, locale) {
  const u = ['B', 'KB', 'MB', 'GB', 'TB'];
  let n = Number(bytes) || 0, i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  let s;
  try { s = new Intl.NumberFormat(locale, { maximumFractionDigits: i === 0 ? 0 : n < 10 ? 1 : 0 }).format(n); }
  catch { s = i === 0 ? String(n) : n.toFixed(n < 10 ? 1 : 0); }
  return s + ' ' + u[i];
}
export function formatDate(ms, locale) {
  if (ms == null) return '';
  try { return new Intl.DateTimeFormat(locale, { dateStyle: 'short', timeStyle: 'short' }).format(new Date(ms)); }
  catch { return new Date(ms).toISOString().slice(0, 16).replace('T', ' '); }
}
export function summarize(entries) {
  let files = 0, dirs = 0, bytes = 0;
  for (const e of entries) { if (e.type === 'dir') dirs++; else { files++; bytes += Number(e.size) || 0; } }
  return { count: entries.length, files, dirs, bytes };
}

// ── errors in plain words ──
// FsError.code → the app's message key; anything else (a bridge or browser error) is 'err_failed'.
const ERR_CODES = ['not-found', 'exists', 'no-space', 'denied', 'offline', 'too-large', 'bad-path', 'unsupported', 'failed'];
export function errorKey(err) {
  const c = err && err.code;
  return 'err_' + (ERR_CODES.includes(c) ? c : 'failed').replace(/-/g, '_');
}
// t(key, vars) → "Not enough space on the device." / "… (r2_locked)" — the device's own short reason when it
// gave one, so a support question has something to go on.
export function errorText(err, t, vars = {}) {
  if (err && err.user && err.message) return err.message;
  const base = t(errorKey(err), vars);
  const why = err && typeof err.reason === 'string' && /^[\w .:-]{1,60}$/.test(err.reason) ? err.reason : '';
  // a reason the OS has words for (core catalog fs_reason_*: the cloud archive's) is said in them
  const said = why && /^\w+$/.test(why) ? t('fs_reason_' + why) : '';
  if (said && said !== 'fs_reason_' + why) return said;
  return why ? `${base} (${why})` : base;
}

// ── keyboard movement in the list ──
// index of the item the key moves to; cols = items per row (1 outside the icon view), page = rows per screen.
export function moveIndex(index, key, { count, cols = 1, page = 10 } = {}) {
  if (!count) return -1;
  const last = count - 1;
  const i = index < 0 ? -1 : Math.min(index, last);
  const clamp = (n) => Math.max(0, Math.min(last, n));
  switch (key) {
    case 'ArrowDown': return i < 0 ? 0 : clamp(i + cols);
    case 'ArrowUp': return i < 0 ? 0 : clamp(i - cols);
    case 'ArrowRight': return cols > 1 ? (i < 0 ? 0 : clamp(i + 1)) : i;
    case 'ArrowLeft': return cols > 1 ? (i < 0 ? 0 : clamp(i - 1)) : i;
    case 'Home': return 0;
    case 'End': return last;
    case 'PageDown': return i < 0 ? 0 : clamp(i + Math.max(1, page) * cols);
    case 'PageUp': return i < 0 ? 0 : clamp(i - Math.max(1, page) * cols);
    default: return i;
  }
}

// Type-ahead: letters typed within `ms` of each other form one prefix; the next name starting with it wins
// (from the current item on, wrapping; the same letter again cycles through the names starting with it).
export function createTypeAhead(ms = 900) {
  let buf = '', last = 0;
  return {
    feed(ch, now = Date.now()) { buf = now - last > ms ? ch : buf + ch; last = now; return buf; },
    reset() { buf = ''; last = 0; },
    get buffer() { return buf; },
  };
}
export function findByPrefix(names, prefix, from = -1) {
  const p = String(prefix).toLowerCase();
  if (!p || !names.length) return -1;
  const cycle = p.length > 1 && [...p].every((c) => c === p[0]);
  const want = cycle ? p[0] : p;
  const start = cycle || p.length === 1 ? from + 1 : Math.max(0, from);
  for (let k = 0; k < names.length; k++) {
    const i = (start + k) % names.length;
    if (String(names[i]).toLowerCase().startsWith(want)) return i;
  }
  return -1;
}

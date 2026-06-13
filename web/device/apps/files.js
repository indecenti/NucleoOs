// Files — device-simulator app. Mirrors firmware app_files.cpp: a folder-first SD
// browser with per-type glyphs, sizes, a breadcrumb and smooth keyboard navigation.
// Reads the card through the same /api/fs/list endpoint the real device serves.

let path = '/';        // always ends in '/'
let entries = [];      // { name, dir, kb }
let sel = 0;

import { makeListState, drawFocusList } from './_list.js';
const lst = makeListState();

// Pure path helpers (unit-tested in tools/files-path.test.mjs).
export function joinPath(dir, name) { return (dir.endsWith('/') ? dir : dir + '/') + name + '/'; }
export function parentPath(dir) {
  if (dir === '/' || dir === '') return '/';
  const trimmed = dir.replace(/\/+$/, '');
  const i = trimmed.lastIndexOf('/');
  return i <= 0 ? '/' : trimmed.slice(0, i + 1);
}

function glyph(e) {
  if (e.dir) return '📁';
  const dot = (e.name.match(/\.[^.]+$/) || [''])[0].toLowerCase();
  if (['.mp3', '.wav'].includes(dot)) return '🎵';
  if (['.png', '.jpg', '.jpeg', '.gif'].includes(dot)) return '🖼';
  if (['.txt', '.md', '.json'].includes(dot)) return '📄';
  return '•';
}

async function scan() {
  sel = 0; entries = [];
  try {
    const r = await (await fetch('/api/fs/list?path=' + encodeURIComponent(path))).json();
    entries = (r.entries || []).map(e => ({ name: e.name, dir: e.type === 'dir', kb: Math.ceil((e.size || 0) / 1024) }))
      .filter(e => !e.name.startsWith('.'))
      .sort((a, b) => (a.dir !== b.dir) ? (a.dir ? -1 : 1) : a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }));
  } catch { entries = []; }
}

export const filesApp = {
  id: 'files',

  enter() { path = '/'; scan(); return { hint: ';/. move  enter open  del up  esc back' }; },

  key(key, ch) {
    if (key === 'up') { if (entries.length) sel = (sel + entries.length - 1) % entries.length; }
    else if (key === 'down') { if (entries.length) sel = (sel + 1) % entries.length; }
    else if (key === 'backspace') { if (path !== '/') { path = parentPath(path); scan(); } }
    else if (key === 'enter') { const e = entries[sel]; if (e?.dir) { path = joinPath(path, e.name); scan(); } }
    else if (ch >= '1' && ch <= '9') { const i = +ch - 1; if (i < entries.length) sel = i; }
  },

  draw(g) {
    const top = g.contentTop, h = g.contentH;
    let p = path; if (p.length > 34) p = '…' + p.slice(p.length - 33);
    g.text('SD:' + p, 8, top + 7, '#9fd0ff', 8, 'bold');
    g.roundRect(0, top + 14, g.W, 1, 0, g.COL.line);   // separator

    if (!entries.length) { g.text('(empty folder)', 12, top + 40, g.COL.dim, 9); return { instruction: 'Browse and open SD card files', hint: ';/. move  del up  esc back' }; }

    drawFocusList(g, lst, {
      top: top + 18, h: h - 18, count: entries.length, sel, now: g.now,
      label: i => (entries[i].dir ? '📁 ' : '') + entries[i].name,
      right: i => entries[i].dir ? '›' : entries[i].kb + 'K',
      color: i => entries[i].dir ? '#ffd166' : '#4ea1ff',
    });
    return { instruction: 'Browse and open SD card files', hint: ';/. move  enter open  del up  esc back' };
  },
};

// Test/inspection hook (preview harness; not part of the app contract).
export function _debugState() { return { path, n: entries.length, sel, focused: entries[sel]?.name || null }; }

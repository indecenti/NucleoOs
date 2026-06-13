// Notes — device-simulator app. Mirrors firmware app_notepad.cpp.
// Smartwatch feel: the note list uses the shared enlarged-focus list; opening a note
// shows it in a big, readable VIEW (size-2 text, scroll with the arrows), and Enter
// switches to EDIT (append at the end, view auto-follows the cursor). Esc saves & exits.
// Two modes are needed because the matrix keyboard reuses ; . / for both navigation and
// typing — VIEW gives arrow-scroll, EDIT gives typing.
import { makeListState, drawFocusList, title } from './_list.js';
const lst = makeListState();

const NOTES_DIR = '/data/Notes';
const TEXT_RE = /\.(txt|md|json)$/i;
const WRAP = 19, MAXBUF = 2048;          // ~19 chars fit at text size 2 on 240px

let names = [];
let sel = 0;
let mode = 'list';                        // 'list' | 'view' | 'edit'
let dirty = false;
let buf = '';
let file = '';
let vscroll = 0;                          // top wrapped-line shown in VIEW

async function scan() {
  names = []; sel = 0;
  try {
    const r = await (await fetch('/api/fs/list?path=' + encodeURIComponent(NOTES_DIR))).json();
    names = (r.entries || []).filter(e => e.type === 'file' && TEXT_RE.test(e.name))
      .map(e => e.name).sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }));
  } catch { names = []; }
}

async function load(name) {
  file = name; buf = ''; dirty = false; mode = 'view'; vscroll = 0;
  try { buf = await (await fetch('/api/fs/read?path=' + encodeURIComponent(NOTES_DIR + '/' + name))).text(); }
  catch { buf = ''; }
  if (buf.length > MAXBUF) buf = buf.slice(0, MAXBUF);
}

function newNote() {
  let max = 0;
  for (const n of names) { const m = /^note-(\d+)/.exec(n); if (m && +m[1] > max) max = +m[1]; }
  file = `note-${max + 1}.txt`; buf = ''; dirty = true; mode = 'edit'; vscroll = 0;
}

async function save() {
  if (!dirty || !file) return;
  try {
    await fetch('/api/fs/mkdir?path=' + encodeURIComponent(NOTES_DIR), { method: 'POST' });
    await fetch('/api/fs/write?path=' + encodeURIComponent(NOTES_DIR + '/' + file), { method: 'POST', body: buf });
    dirty = false;
  } catch { /* ignore */ }
}

function wrap(text) {
  const out = []; let col = 0, line = '';
  for (const ch of text) {
    if (ch === '\n') { out.push(line); line = ''; col = 0; continue; }
    line += ch; if (++col >= WRAP) { out.push(line); line = ''; col = 0; }
  }
  out.push(line);
  return out;
}

export const notesApp = {
  id: 'notepad',

  enter() { mode = 'list'; dirty = false; file = ''; scan(); return { hint: ';/. move  enter open  esc back' }; },
  exit() { save(); },

  key(key, ch) {
    if (mode === 'list') {
      const items = names.length + 1;
      if (key === 'up') sel = (sel + items - 1) % items;
      else if (key === 'down') sel = (sel + 1) % items;
      else if (key === 'enter') { if (sel === 0) newNote(); else load(names[sel - 1]); }
    } else if (mode === 'view') {
      const lines = wrap(buf);
      if (key === 'up') vscroll = Math.max(0, vscroll - 1);
      else if (key === 'down') vscroll = Math.min(Math.max(0, lines.length - 1), vscroll + 1);
      else if (key === 'enter') mode = 'edit';     // start editing (append at end)
    } else {                                        // edit: every printable key types
      if (key === 'enter') { if (buf.length < MAXBUF - 1) { buf += '\n'; dirty = true; } }
      else if (key === 'backspace') { if (buf.length) { buf = buf.slice(0, -1); dirty = true; } }
      else if (ch && ch.charCodeAt(0) >= 32 && ch.charCodeAt(0) < 127) { if (buf.length < MAXBUF - 1) { buf += ch; dirty = true; } }
    }
  },

  draw(g) {
    if (mode === 'list') return drawList(g);
    return drawText(g);
  },
};

function drawList(g) {
  const top = g.contentTop, h = g.contentH;
  const y0 = title(g, 'Notes', '#4ea1ff', `${names.length} note${names.length === 1 ? '' : 's'}`);
  drawFocusList(g, lst, {
    top: y0, h: top + h - y0, count: names.length + 1, sel, now: g.now,
    label: i => i === 0 ? '+ New note' : names[i - 1],
    color: i => i === 0 ? '#7CFC9A' : '#4ea1ff',
  });
  return { instruction: 'Read and edit text files', hint: ';/. move  enter open  esc back' };
}

function drawText(g) {
  const top = g.contentTop, h = g.contentH, editing = mode === 'edit';
  g.text(file + (dirty ? ' *' : ''), 8, top + 7, '#4ea1ff', 8, 'bold');
  g.text(editing ? 'EDIT' : 'VIEW', g.W - 8, top + 7, editing ? '#7CFC9A' : g.COL.muted, 8, 'bold', 'right');
  g.roundRect(0, top + 14, g.W, 1, 0, g.COL.line);

  const LINEH = 16, textTop = top + 22, vis = Math.max(1, Math.floor((h - 18) / LINEH));
  const lines = wrap(buf);
  // EDIT auto-follows the cursor (end); VIEW uses the manual scroll offset.
  const first = editing ? Math.max(0, lines.length - vis) : Math.min(vscroll, Math.max(0, lines.length - vis));
  let y = textTop;
  for (let l = first; l < lines.length && l < first + vis; l++) {
    g.text(lines[l], 8, y, g.COL.fg, 13);                      // BIG, readable text
    if (editing && l === lines.length - 1) g.roundRect(8 + lines[l].length * 7.5, y - 6, 4, 13, 0, '#7CFC9A');
    y += LINEH;
  }
  if (!buf.length) g.text(editing ? 'Type to write…' : '(empty)', 8, textTop, g.COL.dim, 9);

  // scroll indicator (VIEW)
  if (!editing && lines.length > vis) {
    const track = h - 22, kh = Math.max(8, track * vis / lines.length);
    const ky = textTop + (track - kh) * (first / Math.max(1, lines.length - vis));
    g.roundRect(g.W - 3, textTop, 2, track, 1, g.COL.line);
    g.roundRect(g.W - 3, ky, 2, kh, 1, '#4ea1ff');
  }
  return editing
    ? { instruction: 'Editing ' + file, hint: 'type  enter newline  del bksp  esc save+exit' }
    : { instruction: 'Viewing ' + file, hint: ';/. scroll  enter edit  esc back' };
}

// Test/inspection hook (preview harness; not part of the app contract).
export function _debugState() { return { mode, dirty, file, len: buf.length, n: names.length, sel, vscroll, tail: buf.slice(-20) }; }

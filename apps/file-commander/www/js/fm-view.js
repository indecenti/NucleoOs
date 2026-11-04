/* SPDX-License-Identifier: AGPL-3.0-or-later */
// fm-view.js — the list itself: Details (columns), List and Large icons, drawn from the entries the app hands
// over. Rows are keyed and reconciled, never torn down: selection, filtering and a background re-list touch only
// what changed (no blink, no scroll jump). Thumbnails load only once a tile is on screen.
import { iconFor, typeLabel, isImage } from './fm-types.js';
import { normMtime, formatSize, formatDate } from './fm-state.js';

const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

// opts: { list, head, t, locale(), thumbUrl(entry) → string | Promise<string> | null, onHeaderSort(key) }
export function createView({ list, head, t, locale, thumbUrl, onHeaderSort }) {
  const rows = new Map();                      // key → row element
  let mode = 'details';
  let cols = { folder: false, mtime: false };
  let shown = [];
  let state = { selected: new Set(), cursor: -1, cutKeys: new Set() };
  let placeholder = null;                      // the empty / loading / error block, when shown
  const thumbs = new Map();                    // key → object URL to revoke (local files)

  // ── thumbnails: set src only when the tile scrolls into view ──
  const io = typeof IntersectionObserver === 'function' ? new IntersectionObserver((ents) => {
    for (const en of ents) {
      if (!en.isIntersecting) continue;
      io.unobserve(en.target);
      loadThumb(en.target);
    }
  }, { root: list, rootMargin: '120px' }) : null;
  async function loadThumb(row) {
    const e = row._e;
    if (!e || row._thumbFor === e.key) return;
    row._thumbFor = e.key;
    let url = null;
    try { url = await thumbUrl(e); } catch { url = null; }
    if (!url || !row._e || row._e.key !== e.key || mode !== 'icons') { if (row._thumbFor === e.key) row._thumbFor = null; return; }
    if (url.startsWith('blob:')) { const old = thumbs.get(e.key); if (old && old !== url) URL.revokeObjectURL(old); thumbs.set(e.key, url); }
    const img = document.createElement('img');
    img.alt = '';
    img.decoding = 'async';
    img.draggable = false;
    img.addEventListener('load', () => { const ic = row.querySelector('.ic'); if (ic && row._e && row._e.key === e.key && mode === 'icons') { ic.textContent = ''; ic.appendChild(img); ic.classList.add('thumb'); } }, { once: true });
    img.addEventListener('error', () => { row._thumbFor = 'failed:' + e.key; }, { once: true });
    img.src = url;
  }
  const wantsThumb = (e) => mode === 'icons' && e.type !== 'dir' && isImage(e.name) && (Number(e.size) || 0) <= 16 * 1024 * 1024;

  function header() {
    if (!head) return;
    const key = state.sortKey, dir = state.sortDir;
    const cell = (k, label, cls) => {
      const on = key === k;
      return `<div class="hc ${cls}" role="columnheader" tabindex="-1" data-sort="${k}" aria-sort="${on ? (dir > 0 ? 'ascending' : 'descending') : 'none'}">` +
        `<span>${esc(label)}</span><span class="arr" aria-hidden="true">${on ? (dir > 0 ? '<svg viewBox="0 0 10 10" width="10" height="10"><path d="M2 6.5L5 3.5l3 3" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/></svg>' : '<svg viewBox="0 0 10 10" width="10" height="10"><path d="M2 3.5L5 6.5l3-3" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"/></svg>') : ''}</span></div>`;
    };
    head.innerHTML = '<div class="hc-ic" aria-hidden="true"></div>' + cell('name', t('col_name'), 'c-name') +
      (cols.folder ? `<div class="hc c-folder" role="columnheader"><span>${esc(t('col_folder'))}</span></div>` : '') +
      (cols.mtime ? cell('mtime', t('col_mtime'), 'c-date') : '') +
      cell('type', t('col_type'), 'c-type') + cell('size', t('col_size'), 'c-size');
  }
  if (head) head.addEventListener('click', (ev) => { const h = ev.target.closest('[data-sort]'); if (h) onHeaderSort(h.dataset.sort); });

  function build(e) {
    const row = document.createElement('div');
    row.className = 'row';
    row.setAttribute('role', 'option');
    row.innerHTML = '<span class="ic"></span><span class="nm"></span><span class="c-folder"></span><span class="c-date"></span><span class="c-type"></span><span class="c-size"></span>';
    return row;
  }
  function update(row, e, i) {
    const prev = row._e;
    row._e = e;
    row.id = 'r' + i;
    row.dataset.idx = i;
    const isDir = e.type === 'dir';
    const [ic, nm, fd, dt, ty, sz] = row.children;
    if (!prev || prev.name !== e.name || prev.type !== e.type || row._mode !== mode) {
      ic.className = 'ic';
      ic.innerHTML = iconFor(e.name, isDir);
      row._thumbFor = null;
    }
    row._mode = mode;
    if (nm.textContent !== e.name) { nm.textContent = e.name; }
    nm.title = e.name;
    const folder = cols.folder ? (e.dir || '') : '';
    if (fd.textContent !== folder) fd.textContent = folder;
    const ms = cols.mtime ? normMtime(e.mtime) : null;
    const date = ms != null ? formatDate(ms, locale()) : '';
    if (dt.textContent !== date) dt.textContent = date;
    const type = typeLabel(e.name, isDir, t);
    if (ty.textContent !== type) ty.textContent = type;
    const size = isDir ? '' : formatSize(e.size, locale());
    if (sz.textContent !== size) sz.textContent = size;
    const sel = state.selected.has(e.key);
    row.classList.toggle('sel', sel);
    row.setAttribute('aria-selected', String(sel));
    row.classList.toggle('cur', i === state.cursor);
    row.classList.toggle('cut', state.cutKeys.has(e.key));
    row.classList.toggle('prot', !!e.protected);
    row.classList.toggle('hid', e.name.startsWith('.'));
    row.title = e.protected ? t('prot_tooltip') : '';
    row.draggable = !e.protected && (e.type !== 'dir' || String(e.path || '').startsWith('/'));
    if (wantsThumb(e) && row._thumbFor !== e.key && !String(row._thumbFor || '').startsWith('failed:')) { if (io) io.observe(row); else loadThumb(row); }
  }

  function clearPlaceholder() { if (placeholder) { placeholder.remove(); placeholder = null; } }

  // render(entries, { selected, cursor, cutKeys, sortKey, sortDir, cols }) — entries already sorted and filtered.
  function render(entries, st) {
    state = { ...state, ...st };
    shown = entries;
    const nextCols = { folder: !!(st.cols && st.cols.folder), mtime: !!(st.cols && st.cols.mtime) };
    cols = nextCols;
    list.classList.toggle('has-folder', cols.folder);
    list.classList.toggle('has-date', cols.mtime);
    if (head) head.classList.toggle('has-folder', cols.folder), head.classList.toggle('has-date', cols.mtime);
    header();
    clearPlaceholder();
    const want = new Set(entries.map((e) => e.key));
    for (const [k, r] of rows) if (!want.has(k)) { if (io) io.unobserve(r); r.remove(); rows.delete(k); const u = thumbs.get(k); if (u) { URL.revokeObjectURL(u); thumbs.delete(k); } }
    let prev = null;
    entries.forEach((e, i) => {
      let r = rows.get(e.key);
      if (!r) { r = build(e); rows.set(e.key, r); }
      update(r, e, i);
      const ref = prev ? prev.nextSibling : list.firstChild;
      if (r !== ref) list.insertBefore(r, ref);
      prev = r;
    });
    list.setAttribute('aria-activedescendant', state.cursor >= 0 && entries[state.cursor] ? 'r' + state.cursor : '');
  }
  // Selection, cursor and cut marks only: classes in place, nothing rebuilt.
  function paint(st) {
    state = { ...state, ...st };
    shown.forEach((e, i) => {
      const r = rows.get(e.key);
      if (!r) return;
      const sel = state.selected.has(e.key);
      r.classList.toggle('sel', sel);
      r.setAttribute('aria-selected', String(sel));
      r.classList.toggle('cur', i === state.cursor);
      r.classList.toggle('cut', state.cutKeys.has(e.key));
    });
    list.setAttribute('aria-activedescendant', state.cursor >= 0 && shown[state.cursor] ? 'r' + state.cursor : '');
  }
  // An empty, loading or error state in place of the rows. html is built by the caller from escaped text.
  function showBlock(kind, html) {
    for (const r of rows.values()) { if (io) io.unobserve(r); r.remove(); }
    rows.clear();
    shown = [];
    clearPlaceholder();
    placeholder = document.createElement('div');
    placeholder.className = 'block block-' + kind;
    placeholder.innerHTML = html;
    list.appendChild(placeholder);
    list.setAttribute('aria-activedescendant', '');
    return placeholder;
  }
  function skeleton(n = 8) {
    let html = '';
    for (let i = 0; i < n; i++) html += `<div class="sk-row"><span class="sk sk-ic"></span><span class="sk sk-nm" style="width:${30 + ((i * 37) % 45)}%"></span><span class="sk sk-sz"></span></div>`;
    return showBlock('loading', html);
  }
  function setMode(m) {
    if (head) head.hidden = m !== 'details';
    if (m === mode && list.dataset.view === m) return;
    mode = m;
    list.dataset.view = m;
    for (const r of rows.values()) { r._mode = null; r._thumbFor = null; }
    if (head) head.hidden = m !== 'details';
  }
  // Items per visual row (the icon and list views wrap), for the arrow keys.
  function perRow() {
    if (mode === 'details') return 1;
    const kids = [...rows.values()].filter((r) => r.parentNode === list);
    if (kids.length < 2) return 1;
    const sorted = kids.sort((a, b) => Number(a.dataset.idx) - Number(b.dataset.idx));
    const top = sorted[0].offsetTop;
    let n = 0;
    for (const r of sorted) { if (r.offsetTop !== top) break; n++; }
    return Math.max(1, n);
  }
  function pageRows() {
    const first = list.querySelector('.row');
    const h = first ? first.offsetHeight || 32 : 32;
    return Math.max(1, Math.floor(list.clientHeight / h) - 1);
  }
  function reveal(i) { const e = shown[i]; const r = e && rows.get(e.key); if (r) r.scrollIntoView({ block: 'nearest' }); }
  const rowOf = (key) => rows.get(key) || null;
  const indexAt = (el) => { const r = el && el.closest ? el.closest('.row') : null; return r && r.parentNode === list ? Number(r.dataset.idx) : -1; };

  return { render, paint, showBlock, skeleton, setMode, perRow, pageRows, reveal, rowOf, indexAt, get mode() { return mode; } };
}

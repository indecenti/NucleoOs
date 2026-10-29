/* SPDX-License-Identifier: AGPL-3.0-or-later */
// nucleo-menu.js — the OS context menu, for every app: one look, one behaviour, every input.
//
//   import menu, { ICONS } from '/nucleo-menu.js';
//   menu.attach(listEl, (ev) => items);                  // right click, long press, the Menu key, Shift+F10
//   menu.open(x, y, items);                               // anywhere (a "⋯" button)
//   items: [{ label, icon: 'copy', shortcut: 'Ctrl+C', disabled, danger, checked, fn },
//           { sep: true }, { header: 'Open with' }, { label, icon, sub: [ …items ] }]
//
// Keys: arrows move (and open/close submenus), Home/End, Enter/Space activate, Esc closes one level, a letter
// jumps to the next item starting with it. Touch: a long press opens it where the finger is. The menu flips to
// stay on screen, takes the app's own theme variables (--panel, --text, --muted, --line, --accent, --danger),
// returns focus where it came from, and carries the ARIA roles a screen reader expects. Styles come from a
// constructed stylesheet and no inline handler is ever written, so the strictest page CSP still allows it.

// 16 px line icons, the same set in every app (toolbars may use them too): currentColor, no fill.
const P = (d) => `<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">${d}</svg>`;
export const ICONS = {
  open: P('<path d="M2 4.5h4l1.2 1.5H14v6.5H2z"/>'),
  'open-with': P('<rect x="2" y="2.5" width="5" height="5" rx="1"/><rect x="9" y="2.5" width="5" height="5" rx="1"/><rect x="2" y="9" width="5" height="5" rx="1"/><path d="M11.5 9.5v4M9.5 11.5h4"/>'),
  run: P('<path d="M5 3l8 5-8 5z"/>'),
  cut: P('<circle cx="4.5" cy="11.5" r="2"/><circle cx="11.5" cy="11.5" r="2"/><path d="M5.8 10L12 2.5M10.2 10L4 2.5"/>'),
  copy: P('<rect x="5" y="5" width="8.5" height="8.5" rx="1.2"/><path d="M3 10.5V3.8C3 3.3 3.3 3 3.8 3h6.7"/>'),
  paste: P('<rect x="3" y="3.5" width="10" height="10.5" rx="1.2"/><path d="M6 3.5V2.5h4v1"/><path d="M5.5 8h5M5.5 10.5h3.5"/>'),
  duplicate: P('<rect x="5" y="5" width="8.5" height="8.5" rx="1.2"/><path d="M3 10.5V3.8C3 3.3 3.3 3 3.8 3h6.7M9.25 7.5v4M7.25 9.5h4"/>'),
  rename: P('<path d="M3 13h3l7-7-3-3-7 7z"/><path d="M9 4l3 3"/>'),
  delete: P('<path d="M3 4.5h10M6.5 4.5V3h3v1.5M4.5 4.5l.6 9h5.8l.6-9"/>'),
  info: P('<circle cx="8" cy="8" r="6"/><path d="M8 7.2V11M8 5v.2"/>'),
  'folder-new': P('<path d="M2 4.5h4l1.2 1.5H14v6.5H2z"/><path d="M10.5 7.8v3M9 9.3h3"/>'),
  'file-new': P('<path d="M4 2h5l3 3v9H4z"/><path d="M9 2v3h3M8 8v4M6 10h4"/>'),
  file: P('<path d="M4 2h5l3 3v9H4z"/><path d="M9 2v3h3"/>'),
  folder: P('<path d="M2 4.5h4l1.2 1.5H14v6.5H2z"/>'),
  refresh: P('<path d="M13 8a5 5 0 1 1-1.5-3.5"/><path d="M13 2.5V5h-2.5"/>'),
  download: P('<path d="M8 2.5v8M5 7.5l3 3 3-3M3 13h10"/>'),
  upload: P('<path d="M8 10.5v-8M5 5.5l3-3 3 3M3 13h10"/>'),
  link: P('<path d="M6.5 9.5l3-3M5.5 7.5l-1.8 1.8a2.1 2.1 0 0 0 3 3L8.5 10.5M10.5 8.5l1.8-1.8a2.1 2.1 0 0 0-3-3L7.5 5.5"/>'),
  desktop: P('<rect x="2" y="3" width="12" height="8" rx="1"/><path d="M6 14h4M8 11v3"/>'),
  archive: P('<rect x="2" y="3" width="12" height="3" rx=".8"/><path d="M3 6v7h10V6M6.5 8.5h3"/>'),
  extract: P('<rect x="2" y="3" width="12" height="3" rx=".8"/><path d="M3 6v7h10V6M8 7.5v4M6.3 9.8L8 11.5l1.7-1.7"/>'),
  terminal: P('<rect x="2" y="3" width="12" height="10" rx="1.2"/><path d="M4.5 6.5L6.5 8l-2 1.5M8 10h3"/>'),
  view: P('<rect x="2" y="2.5" width="5" height="5" rx="1"/><rect x="9" y="2.5" width="5" height="5" rx="1"/><rect x="2" y="9" width="5" height="5" rx="1"/><rect x="9" y="9" width="5" height="5" rx="1"/>'),
  list: P('<path d="M5.5 4h8M5.5 8h8M5.5 12h8M2.5 4h.1M2.5 8h.1M2.5 12h.1"/>'),
  details: P('<path d="M2 4h12M2 8h12M2 12h12M7 2.5v11"/>'),
  sort: P('<path d="M4.5 3v10M2.5 11l2 2 2-2M11.5 13V3M9.5 5l2-2 2 2"/>'),
  check: P('<path d="M3 8.5l3 3 7-7"/>'),
  path: P('<path d="M2 12.5l4-9M7 12.5l4-9M11.5 12.5h2.5"/>'),
  'select-all': P('<rect x="2.5" y="2.5" width="11" height="11" rx="1.5" stroke-dasharray="2 1.6"/><path d="M5.5 8l2 2 3.5-3.5"/>'),
  back: P('<path d="M10 3L5 8l5 5"/>'),
  forward: P('<path d="M6 3l5 5-5 5"/>'),
  up: P('<path d="M8 13V4M4 7.5L8 3.5l4 4"/>'),
  search: P('<circle cx="7" cy="7" r="4.5"/><path d="M10.6 10.6L14 14"/>'),
  star: P('<path d="M8 2.2l1.8 3.7 4 .6-2.9 2.8.7 4L8 11.4l-3.6 1.9.7-4L2.2 6.5l4-.6z"/>'),
  eye: P('<path d="M1.5 8S4 3.5 8 3.5 14.5 8 14.5 8 12 12.5 8 12.5 1.5 8 1.5 8z"/><circle cx="8" cy="8" r="2"/>'),
  lock: P('<rect x="3.5" y="7" width="9" height="7" rx="1.2"/><path d="M5.5 7V5a2.5 2.5 0 0 1 5 0v2"/>'),
  share: P('<circle cx="4" cy="8" r="1.8"/><circle cx="12" cy="4" r="1.8"/><circle cx="12" cy="12" r="1.8"/><path d="M5.6 7.2l4.8-2.4M5.6 8.8l4.8 2.4"/>'),
  restore: P('<path d="M3 8a5 5 0 1 0 1.5-3.5"/><path d="M3 2.5V5h2.5"/>'),
  more: P('<path d="M3.5 8h.1M8 8h.1M12.5 8h.1" stroke-width="2.4"/>'),
  close: P('<path d="M4 4l8 8M12 4l-8 8"/>'),
};

const CSS = `
.nm-menu{position:fixed;z-index:2147483000;min-width:200px;max-width:min(360px,calc(100vw - 16px));max-height:calc(100vh - 16px);overflow-y:auto;
  padding:5px;border-radius:10px;background:var(--panel,#18233a);color:var(--text,#e8eefc);border:1px solid var(--line,#2a3a5e);
  box-shadow:0 14px 40px rgba(0,0,0,.45);font:inherit;font-size:13px;outline:none;user-select:none;-webkit-user-select:none}
.nm-item{display:flex;align-items:center;gap:10px;min-height:30px;padding:0 10px 0 8px;border-radius:6px;cursor:default;white-space:nowrap}
.nm-item .nm-ic{width:16px;height:16px;flex:none;display:inline-flex;opacity:.85}
.nm-item .nm-lb{flex:1;overflow:hidden;text-overflow:ellipsis}
.nm-item .nm-sc{color:var(--muted,#8aa0c8);font-size:12px;margin-left:18px}
.nm-item .nm-ar{width:12px;opacity:.7}
.nm-item.nm-on{background:color-mix(in srgb,var(--accent,#4ea1ff) 22%,transparent)}
.nm-item.nm-dis{opacity:.42}
.nm-item.nm-danger{color:var(--danger,#ff7b7b)}
.nm-sep{height:1px;margin:5px 6px;background:var(--line,#2a3a5e)}
.nm-head{padding:6px 10px 3px;font-size:11px;color:var(--muted,#8aa0c8);letter-spacing:.02em;text-transform:uppercase}
@media (pointer:coarse){.nm-item{min-height:40px}}
`;
let sheetDone = false;
function ensureStyle(doc) {
  if (sheetDone) return;
  sheetDone = true;
  try {
    const sh = new doc.defaultView.CSSStyleSheet();
    sh.replaceSync(CSS);
    doc.adoptedStyleSheets = [...doc.adoptedStyleSheets, sh];
  } catch {
    const st = doc.createElement('style');
    st.textContent = CSS;
    doc.head.appendChild(st);
  }
}
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const iconHtml = (ic) => (ic && ICONS[ic]) || (ic && ic.startsWith('<svg') ? ic : '');

export function createMenu(doc = document) {
  const win = doc.defaultView;
  let stack = [];                 // open menu levels: { el, items, idx, parentRow }
  let returnFocus = null;

  function closeFrom(level) {
    while (stack.length > level) { const m = stack.pop(); m.el.remove(); }
    if (!stack.length) {
      doc.removeEventListener('pointerdown', onOutside, true);
      doc.removeEventListener('keydown', onKey, true);
      win.removeEventListener('blur', closeAll);
      win.removeEventListener('resize', closeAll);
      if (returnFocus && returnFocus.focus) { try { returnFocus.focus({ preventScroll: true }); } catch {} }
      returnFocus = null;
    }
  }
  const closeAll = () => closeFrom(0);

  function place(el, x, y, anchor) {
    const r = el.getBoundingClientRect();
    const vw = win.innerWidth, vh = win.innerHeight;
    let left = x, top = y;
    if (anchor) {                                          // a submenu: beside its row, flipped if no room
      left = anchor.right - 2;
      if (left + r.width > vw - 6) left = anchor.left - r.width + 2;
      top = anchor.top - 5;
    }
    left = Math.max(6, Math.min(left, vw - r.width - 6));
    top = Math.max(6, Math.min(top, vh - r.height - 6));
    el.style.left = left + 'px';
    el.style.top = top + 'px';
  }

  function build(items, level, x, y, anchor) {
    ensureStyle(doc);
    closeFrom(level);
    const el = doc.createElement('div');
    el.className = 'nm-menu';
    el.setAttribute('role', 'menu');
    el.tabIndex = -1;
    const rows = [];
    for (const it of items) {
      if (!it) continue;
      if (it.sep) { if (rows.length && !rows[rows.length - 1].sep) { const s = doc.createElement('div'); s.className = 'nm-sep'; s.setAttribute('role', 'separator'); el.appendChild(s); rows.push({ sep: true }); } continue; }
      if (it.header) { const h = doc.createElement('div'); h.className = 'nm-head'; h.textContent = it.header; el.appendChild(h); continue; }
      const row = doc.createElement('div');
      const hasSub = Array.isArray(it.sub);
      row.className = 'nm-item' + (it.disabled ? ' nm-dis' : '') + (it.danger ? ' nm-danger' : '');
      row.setAttribute('role', it.checked != null ? 'menuitemcheckbox' : 'menuitem');
      if (it.checked != null) row.setAttribute('aria-checked', String(!!it.checked));
      if (it.disabled) row.setAttribute('aria-disabled', 'true');
      if (hasSub) row.setAttribute('aria-haspopup', 'menu');
      const icon = it.checked ? ICONS.check : iconHtml(it.icon);
      row.innerHTML = `<span class="nm-ic">${icon}</span><span class="nm-lb">${esc(it.label)}</span>` +
        (it.shortcut ? `<span class="nm-sc">${esc(it.shortcut)}</span>` : '') +
        (hasSub ? `<span class="nm-ar">${ICONS.forward}</span>` : '');
      const entry = { row, it, sub: hasSub };
      const index = rows.length;
      row.addEventListener('pointerenter', () => { focusRow(level, index); if (hasSub && !it.disabled) openSub(level, index); else closeFrom(level + 1); });
      row.addEventListener('click', (e) => { e.stopPropagation(); activate(level, index); });
      el.appendChild(row);
      rows.push(entry);
    }
    while (rows.length && rows[rows.length - 1].sep) { rows.pop(); el.lastChild.remove(); }
    doc.body.appendChild(el);
    place(el, x, y, anchor);
    stack[level] = { el, rows, idx: -1 };
    return stack[level];
  }

  const live = (level) => stack[level].rows.map((r, i) => (r.sep || r.it.disabled ? -1 : i)).filter((i) => i >= 0);
  function focusRow(level, i) {
    const m = stack[level];
    if (!m) return;
    m.rows.forEach((r, k) => r.row && r.row.classList.toggle('nm-on', k === i));
    m.idx = i;
    const r = m.rows[i];
    if (r && r.row) { r.row.scrollIntoView({ block: 'nearest' }); m.el.setAttribute('aria-activedescendant', ''); }
  }
  function openSub(level, i) {
    const r = stack[level].rows[i];
    if (!r || !r.sub || r.it.disabled) return;
    if (stack[level + 1] && stack[level + 1].from === i) return;
    const m = build(r.it.sub, level + 1, 0, 0, r.row.getBoundingClientRect());
    m.from = i;
  }
  function activate(level, i) {
    const r = stack[level] && stack[level].rows[i];
    if (!r || r.sep || r.it.disabled) return;
    if (r.sub) { openSub(level, i); const first = live(level + 1)[0]; if (first != null) focusRow(level + 1, first); return; }
    const fn = r.it.fn;
    closeAll();
    if (typeof fn === 'function') { try { fn(); } catch (e) { console.error(e); } }
  }
  function onOutside(e) { if (!stack.some((m) => m.el.contains(e.target))) closeAll(); }
  function onKey(e) {
    if (!stack.length) return;
    const level = stack.length - 1;
    const m = stack[level];
    const ids = live(level);
    const pos = ids.indexOf(m.idx);
    const k = e.key;
    const take = () => { e.preventDefault(); e.stopPropagation(); };
    if (k === 'Escape') { take(); closeFrom(level); return; }
    if (k === 'ArrowDown') { take(); focusRow(level, ids[(pos + 1) % ids.length]); return; }
    if (k === 'ArrowUp') { take(); focusRow(level, ids[(pos - 1 + ids.length) % ids.length]); return; }
    if (k === 'Home') { take(); focusRow(level, ids[0]); return; }
    if (k === 'End') { take(); focusRow(level, ids[ids.length - 1]); return; }
    if (k === 'ArrowRight') { take(); if (m.rows[m.idx] && m.rows[m.idx].sub) activate(level, m.idx); return; }
    if (k === 'ArrowLeft') { take(); if (level > 0) closeFrom(level); return; }
    if (k === 'Enter' || k === ' ') { take(); if (m.idx >= 0) activate(level, m.idx); return; }
    if (k === 'Tab') { take(); closeAll(); return; }
    if (k.length === 1 && /\S/.test(k)) {                // type-ahead: the next item starting with that letter
      take();
      const order = ids.slice(pos + 1).concat(ids.slice(0, pos + 1));
      const hit = order.find((i) => String(m.rows[i].it.label || '').toLowerCase().startsWith(k.toLowerCase()));
      if (hit != null) focusRow(level, hit);
    }
  }

  // open(x, y, items, { focusFirst }): focusFirst when opened from the keyboard.
  function open(x, y, items, opts = {}) {
    returnFocus = doc.activeElement;
    closeAll();
    returnFocus = doc.activeElement;
    const m = build(items, 0, x, y, null);
    if (!m.rows.length) { closeAll(); return; }
    doc.addEventListener('pointerdown', onOutside, true);
    doc.addEventListener('keydown', onKey, true);
    win.addEventListener('blur', closeAll);
    win.addEventListener('resize', closeAll);
    m.el.focus({ preventScroll: true });
    if (opts.focusFirst) { const first = live(0)[0]; if (first != null) focusRow(0, first); }
  }

  // attach(el, build): build(event, target) → items. Right click, a long press on touch, the Menu key and
  // Shift+F10 (on the focused element) all open it; the browser's own menu is suppressed only where we show ours.
  function attach(el, buildItems) {
    const show = (ev, x, y, keyboard) => {
      const items = buildItems(ev);
      if (!items || !items.length) return false;
      open(x, y, items, { focusFirst: keyboard });
      return true;
    };
    el.addEventListener('contextmenu', (ev) => { if (show(ev, ev.clientX, ev.clientY, false)) ev.preventDefault(); });
    el.addEventListener('keydown', (ev) => {
      if (ev.key === 'ContextMenu' || (ev.key === 'F10' && ev.shiftKey)) {
        const t = ev.target.getBoundingClientRect ? ev.target.getBoundingClientRect() : { left: 20, bottom: 20 };
        if (show(ev, t.left + 12, t.bottom - 4, true)) ev.preventDefault();
      }
    });
    let timer = 0, start = null;
    el.addEventListener('pointerdown', (ev) => {
      if (ev.pointerType !== 'touch') return;
      start = { x: ev.clientX, y: ev.clientY, ev };
      clearTimeout(timer);
      timer = setTimeout(() => { if (start) { show(start.ev, start.x, start.y, false); start = null; } }, 520);
    });
    const cancel = (ev) => { if (start && (!ev || Math.hypot(ev.clientX - start.x, ev.clientY - start.y) > 10)) { clearTimeout(timer); start = null; } };
    el.addEventListener('pointermove', cancel);
    el.addEventListener('pointerup', () => { clearTimeout(timer); start = null; });
    el.addEventListener('pointercancel', () => { clearTimeout(timer); start = null; });
  }

  return { open, attach, close: closeAll, get isOpen() { return stack.length > 0; } };
}

const menu = typeof document !== 'undefined' ? createMenu(document) : null;
export default menu;

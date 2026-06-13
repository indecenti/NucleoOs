// Minimal window manager: open/focus/drag/minimize/close app windows.
// No dependencies. Apps are hosted in an <iframe> pointed at their web route.
const layer = document.getElementById('windows');
let z = 10;
const windows = new Map(); // id -> { el, app, min }
let onChange = () => {};
let onFrameLoad = () => {};

export function setOnChange(fn) { onChange = fn; }
// Called with (iframe, app) each time an app's iframe finishes loading, so the shell can
// inject OS-wide keyboard shortcuts into the app document (same-origin apps only).
export function setOnFrameLoad(fn) { onFrameLoad = fn; }
export function list() { return [...windows.values()]; }
export function active() { return [...windows.values()].find((w) => w.el.classList.contains('active') && !w.min) || null; }

function focus(id) {
  for (const w of windows.values()) w.el.classList.remove('active');
  const w = windows.get(id);
  if (!w) return;
  w.min = false;
  w.el.classList.remove('hidden');
  w.el.classList.add('active');
  w.el.style.zIndex = ++z;
  onChange();
}

export function toggle(id) {
  const w = windows.get(id);
  if (!w) return;
  if (w.el.classList.contains('active') && !w.min) { w.min = true; w.el.classList.add('hidden'); onChange(); }
  else focus(id);
}

export function open(app, query) {
  const src = app.route ? app.route + (query ? '?' + query : '') : '';
  if (windows.has(app.id)) {                       // already open: optionally navigate, then focus
    const w = windows.get(app.id);
    const iframe = w.el.querySelector('iframe');
    if (iframe && src) iframe.src = src;
    focus(app.id);
    return;
  }
  const el = document.createElement('div');
  el.className = 'win';
  const n = windows.size;
  el.style.left = (60 + n * 28) + 'px';
  el.style.top = (40 + n * 28) + 'px';

  const body = app.route
    ? `<iframe src="${src}" title="${app.name}"></iframe>`
    : `<div class="placeholder">${glyph(app)}<br><br>${app.name}<br><small>No web route declared.</small></div>`;
  el.innerHTML =
    `<div class="bar"><span class="glyph">${glyph(app)}</span>` +
    `<span class="t">${app.name}</span>` +
    `<button class="min" title="Minimize"><svg viewBox="0 0 11 11" width="11" height="11" fill="none" stroke="currentColor" stroke-width="1.3"><line x1="2" y1="6" x2="9" y2="6" stroke-linecap="round"/></svg></button>` +
    `<button class="max" title="Maximize"><svg viewBox="0 0 11 11" width="11" height="11" fill="none" stroke="currentColor" stroke-width="1.2"><rect x="2" y="2" width="7" height="7" rx="1.2"/></svg></button>` +
    `<button class="close" title="Close"><svg viewBox="0 0 11 11" width="11" height="11" fill="none" stroke="currentColor" stroke-width="1.3"><line x1="2.6" y1="2.6" x2="8.4" y2="8.4" stroke-linecap="round"/><line x1="8.4" y1="2.6" x2="2.6" y2="8.4" stroke-linecap="round"/></svg></button></div>` +
    `<div class="body">${body}</div>`;

  layer.appendChild(el);
  windows.set(app.id, { el, app, min: false, max: false });

  const frame = el.querySelector('iframe');
  if (frame) frame.addEventListener('load', () => { try { onFrameLoad(frame, app); } catch {} });

  const bar = el.querySelector('.bar');
  el.addEventListener('mousedown', () => focus(app.id));
  el.querySelector('.min').addEventListener('click', (e) => { e.stopPropagation(); toggle(app.id); });
  el.querySelector('.max').addEventListener('click', (e) => { e.stopPropagation(); maximize(app.id); });
  el.querySelector('.close').addEventListener('click', (e) => { e.stopPropagation(); close(app.id); });
  bar.addEventListener('dblclick', (e) => { if (!e.target.closest('button')) maximize(app.id); });
  // Persist size changes (the native resize grip) — debounced by the shell's saver.
  let ro;
  try { ro = new ResizeObserver(() => onChange()); ro.observe(el); } catch {}
  windows.get(app.id).ro = ro;
  drag(el, bar);
  focus(app.id);
}

// Serialize open windows so the device can restore the session (real-OS behaviour).
export function serialize() {
  const out = [];
  for (const w of windows.values()) {
    const g = (w.max && w.prev) ? w.prev                       // pre-maximize size, so restore-down works
      : { left: w.el.style.left, top: w.el.style.top, width: w.el.style.width || w.el.offsetWidth + 'px', height: w.el.style.height || w.el.offsetHeight + 'px' };
    out.push({ id: w.app.id, x: parseInt(g.left) || 0, y: parseInt(g.top) || 0,
      w: parseInt(g.width) || 0, h: parseInt(g.height) || 0, min: !!w.min, max: !!w.max, z: parseInt(w.el.style.zIndex) || 0 });
  }
  return out;
}

// Apply a saved geometry to an already-open window (used during session restore).
export function applyGeom(id, g) {
  const w = windows.get(id);
  if (!w) return;
  if (g.x != null) w.el.style.left = g.x + 'px';
  if (g.y != null) w.el.style.top = g.y + 'px';
  if (g.w) w.el.style.width = g.w + 'px';
  if (g.h) w.el.style.height = g.h + 'px';
  if (g.z) { w.el.style.zIndex = g.z; if (g.z > z) z = g.z; }
  if (g.max) maximize(id);                  // re-maximize (stores current geom as prev)
  if (g.min) { w.min = true; w.el.classList.add('hidden'); }
}

// Toggle a window between maximized (fills the desktop) and its prior size/position.
export function maximize(id) {
  const w = windows.get(id);
  if (!w) return;
  if (w.max) {
    w.el.classList.remove('max');
    if (w.prev) { Object.assign(w.el.style, w.prev); }
    w.el.querySelector('.max').title = 'Maximize';
  } else {
    w.prev = { left: w.el.style.left, top: w.el.style.top, width: w.el.style.width, height: w.el.style.height };
    w.el.classList.add('max');
    w.el.querySelector('.max').title = 'Restore';
  }
  w.max = !w.max;
  focus(id);
}

export function close(id) {
  const w = windows.get(id);
  if (!w) return;
  if (w.ro) try { w.ro.disconnect(); } catch {}
  w.el.remove();
  windows.delete(id);
  onChange();
}

function glyph(app) { return app.glyph || '▦'; }

// Pointer dragging that stays within the desktop area.
function drag(win, handle) {
  let sx, sy, ox, oy, moving = false;
  handle.addEventListener('mousedown', (e) => {
    if (e.target.closest('button')) return;
    moving = true; sx = e.clientX; sy = e.clientY;
    ox = win.offsetLeft; oy = win.offsetTop;
    e.preventDefault();
  });
  window.addEventListener('mousemove', (e) => {
    if (!moving) return;
    const maxY = window.innerHeight - 44 - 34;
    win.style.left = Math.max(0, ox + e.clientX - sx) + 'px';
    win.style.top = Math.min(maxY, Math.max(0, oy + e.clientY - sy)) + 'px';
  });
  window.addEventListener('mouseup', () => { if (moving) { moving = false; onChange(); } });
}

// Minimal window manager: open/focus/drag/minimize/close app windows.
// No dependencies. Apps are hosted in an <iframe> pointed at their web route.
// Real-OS extras: per-app geometry memory (via a provider the shell wires up) and
// Windows-11-style snapping — drag to edges/corners, or use the Snap Layouts flyout.
const layer = document.getElementById('windows');
let z = 10;
const windows = new Map(); // id -> { el, app, min, max, snap, prev, ro }
let onChange = () => {};
let onFrameLoad = () => {};
let geomFor = () => null;  // shell-supplied: last-known geometry for an app id (survives close)

export function setOnChange(fn) { onChange = fn; }
// Called with (iframe, app) each time an app's iframe finishes loading, so the shell can
// inject OS-wide keyboard shortcuts into the app document (same-origin apps only).
export function setOnFrameLoad(fn) { onFrameLoad = fn; }
// The shell remembers each app's window geometry across closes/reboots and hands it back here.
export function setGeomProvider(fn) { geomFor = fn; }
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
    `<div class="body">${body}</div>` +
    `<div class="win-resizer n" data-dir="n"></div><div class="win-resizer s" data-dir="s"></div>` +
    `<div class="win-resizer e" data-dir="e"></div><div class="win-resizer w" data-dir="w"></div>` +
    `<div class="win-resizer nw" data-dir="nw"></div><div class="win-resizer ne" data-dir="ne"></div>` +
    `<div class="win-resizer sw" data-dir="sw"></div><div class="win-resizer se" data-dir="se"></div>`;

  layer.appendChild(el);
  windows.set(app.id, { el, app, min: false, max: false, snap: null, prev: null });

  const frame = el.querySelector('iframe');
  if (frame) frame.addEventListener('load', () => { try { onFrameLoad(frame, app); } catch {} });

  const bar = el.querySelector('.bar');
  const maxBtn = el.querySelector('.max');
  el.addEventListener('mousedown', () => focus(app.id));
  el.querySelector('.min').addEventListener('click', (e) => { e.stopPropagation(); toggle(app.id); });
  maxBtn.addEventListener('click', (e) => { e.stopPropagation(); maximize(app.id); });
  el.querySelector('.close').addEventListener('click', (e) => { e.stopPropagation(); close(app.id); });
  bar.addEventListener('dblclick', (e) => { if (!e.target.closest('button')) maximize(app.id); });
  attachSnapFlyout(maxBtn, app.id);                // Windows-11 Snap Layouts on hover
  // NOTE: no ResizeObserver here. The explicit resize handles (attachResize) call onChange()
  // once on pointerup; observing every frame floods renderTaskbar()+serialize() and freezes
  // the device mid-drag (esp. the heavy browser iframe).
  drag(el, bar, app.id);
  attachResize(el, app.id);

  const saved = geomFor(app.id);                   // restore this app's last geometry, if any
  if (saved) applyGeom(app.id, saved);
  focus(app.id);
}

// ---- geometry helpers --------------------------------------------------------------------
function curGeom(w) {
  return { left: w.el.style.left, top: w.el.style.top,
    width: w.el.style.width || w.el.offsetWidth + 'px', height: w.el.style.height || w.el.offsetHeight + 'px' };
}
// The desktop work area: the viewport minus the taskbar. (The #windows layer has no intrinsic
// height — its children are absolutely positioned — so we measure the viewport directly, which
// is the same origin window left/top use and matches the CSS `.win.max` rectangle.)
function workArea() {
  const tb = document.getElementById('taskbar');
  const th = tb ? tb.offsetHeight : 44;
  return { w: window.innerWidth, h: window.innerHeight - th };
}
// Pixel rectangle for a snap zone within the work area.
function zoneRect(zone, A) {
  const hw = Math.round(A.w / 2), hh = Math.round(A.h / 2);
  switch (zone) {
    case 'full':  return { left: 0,  top: 0,  width: A.w, height: A.h };
    case 'left':  return { left: 0,  top: 0,  width: hw,  height: A.h };
    case 'right': return { left: hw, top: 0,  width: A.w - hw, height: A.h };
    case 'tl':    return { left: 0,  top: 0,  width: hw,  height: hh };
    case 'tr':    return { left: hw, top: 0,  width: A.w - hw, height: hh };
    case 'bl':    return { left: 0,  top: hh, width: hw,  height: A.h - hh };
    case 'br':    return { left: hw, top: hh, width: A.w - hw, height: A.h - hh };
  }
  return null;
}
// Which snap zone (if any) does a pointer at (px,py) in work-area coords land in?
function detectZone(px, py, A) {
  const EDGE = 18;
  const nearTop = py <= EDGE, nearLeft = px <= EDGE, nearRight = px >= A.w - EDGE, nearBottom = py >= A.h - EDGE;
  if (nearTop) return (px < A.w * 0.18) ? 'tl' : (px > A.w * 0.82) ? 'tr' : 'full';
  if (nearLeft) return py < A.h * 0.34 ? 'tl' : py > A.h * 0.66 ? 'bl' : 'left';
  if (nearRight) return py < A.h * 0.34 ? 'tr' : py > A.h * 0.66 ? 'br' : 'right';
  if (nearBottom) return px < A.w * 0.5 ? 'bl' : 'br';
  return null;
}

// Resize callback: if a SNAPPED window is dragged off its zone size via the grip, it stops
// being snapped (it becomes a floating window at the new size, which then persists correctly).
function onWinResized(w) {
  if (!w) return;
  if (w.snap && !w.max) {
    const r = zoneRect(w.snap, workArea());
    if (Math.abs(w.el.offsetWidth - r.width) > 3 || Math.abs(w.el.offsetHeight - r.height) > 3) {
      w.snap = null; w.el.classList.remove('snapped');
    }
  }
  onChange();
}

// Snap a window to a zone, remembering its floating geometry first so it can be restored.
export function applySnap(id, zone) {
  const w = windows.get(id); if (!w || !zone) return;
  if (!w.snap && !w.max) w.prev = curGeom(w);
  w.el.classList.remove('max', 'snapped');
  if (zone === 'full') {
    w.max = true; w.snap = null; w.el.classList.add('max'); w.el.querySelector('.max').title = 'Restore';
  } else {
    const r = zoneRect(zone, workArea());
    w.max = false; w.snap = zone; w.el.classList.add('snapped');
    Object.assign(w.el.style, { left: r.left + 'px', top: r.top + 'px', width: r.width + 'px', height: r.height + 'px' });
    w.el.querySelector('.max').title = 'Maximize';
  }
  focus(id);
}

// Keyboard snapping (Win/⌘ + arrows), Windows-11 style. The current state is one of
// F(floating) M(maximized) left/right/tl/tr/bl/br; each arrow steps it through this grid,
// so e.g. Win+Left then Win+Up lands a window in the top-left quarter.
const SNAP_NEXT = {
  left:  { F: 'left',  M: 'left',  left: 'F',  right: 'F',  tl: 'F',    bl: 'F',    tr: 'tl',   br: 'bl' },
  right: { F: 'right', M: 'right', right: 'F', left: 'F',   tr: 'F',    br: 'F',    tl: 'tr',   bl: 'br' },
  up:    { F: 'M',     M: 'M',     left: 'tl', right: 'tr', bl: 'left', br: 'right', tl: 'M',   tr: 'M' },
  down:  { F: 'MIN',   M: 'F',     tl: 'left', tr: 'right', left: 'bl', right: 'br', bl: 'F',   br: 'F' },
};
export function snapByKey(id, dir) {
  const w = windows.get(id); if (!w || !SNAP_NEXT[dir]) return;
  const state = w.max ? 'M' : (w.snap || 'F');
  const next = SNAP_NEXT[dir][state];
  if (next === 'MIN') { w.min = true; w.el.classList.add('hidden'); onChange(); return; }
  if (next === 'M') { applySnap(id, 'full'); return; }       // force-maximize regardless of current snap
  if (next === 'F') { if (w.max || w.snap) maximize(id); return; }  // restore to floating geometry
  applySnap(id, next);                                       // a half/quarter zone
}

// Re-tile snapped windows when the desktop work area changes (resolution/DeX/rotation).
let retileT = null;
window.addEventListener('resize', () => {
  clearTimeout(retileT);
  retileT = setTimeout(() => {
    const A = workArea(); let any = false;
    for (const w of windows.values()) {
      if (!w.snap || w.max) continue;
      const r = zoneRect(w.snap, A);
      Object.assign(w.el.style, { left: r.left + 'px', top: r.top + 'px', width: r.width + 'px', height: r.height + 'px' });
      any = true;
    }
    if (any) onChange();
  }, 120);
});

// Serialize open windows so the device can restore the session (real-OS behaviour).
export function serialize() {
  const out = [];
  for (const w of windows.values()) {
    const g = ((w.max || w.snap) && w.prev) ? w.prev : curGeom(w);   // floating geom, so restore works
    out.push({ id: w.app.id, x: parseInt(g.left) || 0, y: parseInt(g.top) || 0,
      w: parseInt(g.width) || 0, h: parseInt(g.height) || 0,
      min: !!w.min, max: !!w.max, snap: w.snap || null, z: parseInt(w.el.style.zIndex) || 0 });
  }
  return out;
}

// Apply a saved geometry to an already-open window (session restore / per-app memory).
export function applyGeom(id, g) {
  const w = windows.get(id);
  if (!w) return;
  if (g.x != null) w.el.style.left = g.x + 'px';
  if (g.y != null) w.el.style.top = g.y + 'px';
  if (g.w) w.el.style.width = g.w + 'px';
  if (g.h) w.el.style.height = g.h + 'px';
  if (g.z) { w.el.style.zIndex = g.z; if (g.z > z) z = g.z; }
  if (g.max) maximize(id);                  // re-maximize (stores current floating geom as prev)
  else if (g.snap) applySnap(id, g.snap);   // re-snap to its half/quarter
  if (g.min) { w.min = true; w.el.classList.add('hidden'); }
}

// Toggle a window between maximized (fills the desktop) and its prior size/position.
export function maximize(id) {
  const w = windows.get(id);
  if (!w) return;
  if (w.max || w.snap) {                     // restore from maximized OR snapped
    w.el.classList.remove('max', 'snapped');
    if (w.prev) Object.assign(w.el.style, w.prev);
    w.max = false; w.snap = null;
    w.el.querySelector('.max').title = 'Maximize';
  } else {
    w.prev = curGeom(w);
    w.el.classList.add('max');
    w.max = true;
    w.el.querySelector('.max').title = 'Restore';
  }
  focus(id);
}

// Minimize every window; if they're already all hidden, restore them (Win "show desktop").
export function showDesktop() {
  const vis = [...windows.values()].filter((w) => !w.min);
  if (vis.length) for (const w of vis) { w.min = true; w.el.classList.add('hidden'); }
  else for (const w of windows.values()) { w.min = false; w.el.classList.remove('hidden'); }
  onChange();
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

// ---- snap preview overlay (shown while dragging near an edge) ----------------------------
let preview = null;
function showPreview(zone) {
  if (!preview) { preview = document.createElement('div'); preview.className = 'snap-preview hidden'; layer.appendChild(preview); }
  const r = zoneRect(zone, workArea());
  Object.assign(preview.style, { left: r.left + 'px', top: r.top + 'px', width: r.width + 'px', height: r.height + 'px' });
  preview.classList.remove('hidden');
}
function hidePreview() { if (preview) preview.classList.add('hidden'); }

// Pointer dragging within the desktop, with Windows-11 edge/corner snapping.
function drag(win, handle, id) {
  let sx, sy, ox, oy, moving = false, zone = null;
  handle.addEventListener('mousedown', (e) => {
    if (e.target.closest('button')) return;
    const w = windows.get(id);
    // Dragging a maximized/snapped window "tears it off": restore floating size under the cursor.
    if (w && (w.max || w.snap)) {
      const rect = win.getBoundingClientRect();
      const ratioX = rect.width ? (e.clientX - rect.left) / rect.width : 0.5;
      w.el.classList.remove('max', 'snapped'); w.max = false; w.snap = null;
      w.el.querySelector('.max').title = 'Maximize';
      const prev = w.prev || { width: '520px', height: '360px' };
      const fw = parseInt(prev.width) || 520, fh = parseInt(prev.height) || 360;
      win.style.width = fw + 'px'; win.style.height = fh + 'px';
      win.style.left = Math.max(0, e.clientX - fw * ratioX) + 'px';
      win.style.top = Math.max(0, e.clientY - 17) + 'px';
    }
    moving = true; sx = e.clientX; sy = e.clientY;
    ox = win.offsetLeft; oy = win.offsetTop;
    e.preventDefault();
    const iframe = win.querySelector('iframe');
    if (iframe) iframe.style.pointerEvents = 'none';
  });
  window.addEventListener('mousemove', (e) => {
    if (!moving) return;
    const A = workArea();
    win.style.left = Math.max(0, ox + e.clientX - sx) + 'px';
    win.style.top = Math.min(A.h - 34, Math.max(0, oy + e.clientY - sy)) + 'px';
    zone = detectZone(e.clientX, e.clientY, A);     // pointer is already in work-area coords
    if (zone) showPreview(zone); else hidePreview();
  });
  window.addEventListener('mouseup', () => {
    if (!moving) return;
    moving = false; hidePreview();
    const iframe = win.querySelector('iframe');
    if (iframe) iframe.style.pointerEvents = '';
    if (zone) { applySnap(id, zone); zone = null; }
    else onChange();
  });
}

// ---- Snap Layouts flyout (Windows-11: hover the maximize button) -------------------------
let fly = null, flyId = null, flyHideT = null;
function buildFly() {
  fly = document.createElement('div');
  fly.className = 'snap-fly';
  fly.innerHTML =
    `<div class="lay full"><div class="cell" data-z="full"></div></div>` +
    `<div class="lay two"><div class="cell" data-z="left"></div><div class="cell" data-z="right"></div></div>` +
    `<div class="lay quad"><div class="cell" data-z="tl"></div><div class="cell" data-z="tr"></div>` +
      `<div class="cell" data-z="bl"></div><div class="cell" data-z="br"></div></div>`;
  fly.addEventListener('mouseenter', () => clearTimeout(flyHideT));
  fly.addEventListener('mouseleave', hideFly);
  fly.addEventListener('click', (e) => { const c = e.target.closest('.cell'); if (c && flyId) { applySnap(flyId, c.dataset.z); hideFly(); } });
  document.body.appendChild(fly);
}
function showFly(id, btn) {
  if (!fly) buildFly();
  clearTimeout(flyHideT);
  flyId = id;
  fly.style.visibility = 'hidden'; fly.style.display = 'grid';
  const r = btn.getBoundingClientRect();
  const fw = fly.offsetWidth, fh = fly.offsetHeight;
  let left = Math.round(r.left + r.width / 2 - fw / 2);
  left = Math.max(6, Math.min(left, window.innerWidth - fw - 6));
  let top = r.bottom + 6;
  if (top + fh > window.innerHeight - 6) top = r.top - fh - 6;
  fly.style.left = left + 'px'; fly.style.top = top + 'px';
  fly.style.visibility = 'visible';
}
function hideFly() { if (fly) fly.style.display = 'none'; flyId = null; }
function attachSnapFlyout(btn, id) {
  let openT = null;
  btn.addEventListener('mouseenter', () => { openT = setTimeout(() => showFly(id, btn), 350); });
  btn.addEventListener('mouseleave', () => { clearTimeout(openT); flyHideT = setTimeout(hideFly, 250); });
}

// Window resizing from all edges and corners
function attachResize(win, id) {
  const resizers = win.querySelectorAll('.win-resizer');
  resizers.forEach(r => {
    r.addEventListener('pointerdown', e => {
      if (e.button !== 0) return;
      e.preventDefault(); e.stopPropagation();
      focus(id);
      const w = windows.get(id);
      if (w.max || w.snap) {
        w.el.classList.remove('max', 'snapped'); w.max = false; w.snap = null;
        w.el.querySelector('.max').title = 'Maximize';
      }
      const dir = r.dataset.dir;
      const startX = e.clientX, startY = e.clientY;
      const startW = win.offsetWidth, startH = win.offsetHeight;
      const startL = win.offsetLeft, startT = win.offsetTop;
      const minW = 240, minH = 160;
      
      const iframe = win.querySelector('iframe');
      if (iframe) iframe.style.pointerEvents = 'none';

      const move = (ev) => {
        let dx = ev.clientX - startX, dy = ev.clientY - startY;
        let newW = startW, newH = startH, newL = startL, newT = startT;
        if (dir.includes('e')) newW = startW + dx;
        if (dir.includes('w')) { newW = startW - dx; newL = startL + dx; }
        if (dir.includes('s')) newH = startH + dy;
        if (dir.includes('n')) { newH = startH - dy; newT = startT + dy; }

        if (newW < minW) { newL -= (minW - newW); newW = minW; }
        if (newH < minH) { newT -= (minH - newH); newH = minH; }
        if (newT < 0) { newH += newT; newT = 0; }

        win.style.width = newW + 'px'; win.style.height = newH + 'px';
        if (dir.includes('w') || dir.includes('n')) {
          win.style.left = newL + 'px'; win.style.top = newT + 'px';
        }
      };
      const up = () => {
        window.removeEventListener('pointermove', move);
        window.removeEventListener('pointerup', up);
        if (iframe) iframe.style.pointerEvents = '';
        onChange();
      };
      window.addEventListener('pointermove', move);
      window.addEventListener('pointerup', up);
    });
  });
}

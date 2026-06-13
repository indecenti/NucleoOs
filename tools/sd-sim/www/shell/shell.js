// NucleoOS desktop shell controller. Loads apps + status from the device and
// renders the desktop, taskbar and Start menu. Heavy lifting stays in the browser.
import * as WM from './wm.js';

// Emoji glyphs keep the device free of icon assets (a deliberate weight trick).
const GLYPHS = {
  'file-commander': '🗂️', notepad: '📝', 'photo-viewer': '🖼️', 'media-player': '🎵',
  'video-player': '🎬', 'automation-studio': '⚙️', 'ir-remote': '📡', swarm: '🛰️',
  settings: '🔧', 'log-viewer': '📜', 'system-monitor': '🩺', calculator: '🧮', clock: '🕐',
  calendar: '📅', dosbox: '🕹️', recorder: '🎙️', 'recycle-bin': '♻️', updates: '🔄',
};
const glyph = (a) => GLYPHS[a.id] || (a.name ? a.name[0].toUpperCase() : '▦');

// Inline SVG icons for OS chrome (theme via currentColor). Keeping them here avoids
// shipping icon files to the device — the same weight trick as the emoji app glyphs.
const ICONS = {
  wifi: '<svg viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.4"><path d="M2 5.6C5.5 2.6 10.5 2.6 14 5.6" stroke-linecap="round"/><path d="M4 8.1C6.4 6.1 9.6 6.1 12 8.1" stroke-linecap="round"/><path d="M6 10.6c1.2-1 2.8-1 4 0" stroke-linecap="round"/><circle cx="8" cy="13" r="0.9" fill="currentColor" stroke="none"/></svg>',
  ap: '<svg viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.4"><circle cx="8" cy="6" r="2"/><path d="M4.5 9.5a5 5 0 0 1 0-7M11.5 2.5a5 5 0 0 1 0 7" stroke-linecap="round"/><line x1="8" y1="8" x2="8" y2="14" stroke-linecap="round"/></svg>',
  offline: '<svg viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.4"><path d="M2 5.6C5.5 2.6 10.5 2.6 14 5.6" stroke-linecap="round" opacity=".45"/><line x1="3" y1="13" x2="13" y2="3" stroke-linecap="round"/></svg>',
};

// Fallback list so the desktop renders during development / offline.
const MOCK = Object.keys(GLYPHS).map((id) => ({
  id, name: id.replace(/(^|-)([a-z])/g, (_, s, c) => (s ? ' ' : '') + c.toUpperCase()),
  route: '', enabled: true,
}));

// UI state (pinned apps, wallpaper) is authoritative on the device, not in the
// browser: it lives in one canonical JSON on the SD, read/written via the file
// API. So every client — PC, Android desktop mode, another browser — shares one
// desktop. Writes are debounced to spare the SD; cross-client sync rides the
// existing fs.changed event the device already emits on every write.
const UI_STATE_PATH = '/system/config/ui-state.json';
// desktop: ordered list of desktop shortcuts the user arranges. Each item is
//   { uid, type:'app'|'file'|'url', target, label }. type 'app' -> target is an
// app id; 'file' -> an SD path opened via its association; 'url' -> a link.
// null means "not seeded yet" -> seed from the installed apps on first run.
const UI_DEFAULTS = { pins: [], wallpaper: '/data/Pictures/wallpaper.png', desktop: null };

const state = { apps: [], pins: [...UI_DEFAULTS.pins], wallpaper: UI_DEFAULTS.wallpaper,
  desktop: [], assoc: { default_open: {}, fallback: 'file-commander' } };

// Open-window session (which apps are open + their geometry) is remembered by the device
// so the desktop comes back as you left it. Kept in its own file — unlike the shared
// desktop arrangement it is NOT live-synced across clients (windows shouldn't jump around
// when another client moves one); it is only restored on load.
const SESSION_PATH = '/system/config/session.json';
let restoring = false, sessTimer = null;
function saveSession() {
  if (restoring) return;
  clearTimeout(sessTimer);
  sessTimer = setTimeout(() => {
    const body = JSON.stringify({ windows: WM.serialize() });
    fetch('/api/fs/write?path=' + encodeURIComponent(SESSION_PATH), { method: 'POST', body }).catch(() => {});
  }, 500);
}
async function restoreSession() {
  let saved;
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(SESSION_PATH), { cache: 'no-store' });
    if (!r.ok) return;
    saved = JSON.parse(await r.text());
  } catch { return; }
  if (!saved || !Array.isArray(saved.windows)) return;
  restoring = true;
  // Open in saved z-order so stacking is preserved.
  for (const g of [...saved.windows].sort((a, b) => (a.z || 0) - (b.z || 0))) {
    const app = byId(g.id);
    if (!app) continue;                        // app was uninstalled since
    WM.open(app);
    WM.applyGeom(g.id, g);
  }
  restoring = false;
  renderTaskbar();
}

// ===== OS-wide layer: clipboard (persistent, bounded) + keyboard shortcuts =====
// The clipboard is an OS service shared by every app and remembered by the device, with a
// BOUNDED history (not infinite — kind to the ESP/SD). Shortcuts work across the whole OS:
// the shell handles window management itself and injects an editing-shortcut bridge into
// each app window, so Ctrl+C/X/V/A/S/Z, F2, Delete behave the same everywhere.
const CLIP_PATH = '/system/config/clipboard.json';
const CLIP_CAP = 20;                              // history depth — the "not infinite" cap
let clip = { items: [] };                         // newest first
let clipTimer = null;
async function loadClipboard() {
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(CLIP_PATH), { cache: 'no-store' });
    if (r.ok) { const c = JSON.parse(await r.text()); if (Array.isArray(c.items)) clip.items = c.items.slice(0, CLIP_CAP); }
  } catch {}
}
function saveClipboard() {
  clearTimeout(clipTimer);
  clipTimer = setTimeout(() => {
    fetch('/api/fs/write?path=' + encodeURIComponent(CLIP_PATH), { method: 'POST', body: JSON.stringify({ items: clip.items }) }).catch(() => {});
  }, 400);
}
function clipboardWrite(kind, data) {
  const key = JSON.stringify({ kind, data });
  clip.items = clip.items.filter((it) => JSON.stringify({ kind: it.kind, data: it.data }) !== key);  // dedup
  clip.items.unshift({ kind, data, ts: Date.now() });
  if (clip.items.length > CLIP_CAP) clip.items.length = CLIP_CAP;                                     // bound it
  saveClipboard();
}
const clipboardLatest = () => clip.items[0] || null;

// Forward an editing intent to the focused app window (it decides what to do).
function sendToActive(action, extra) {
  const w = WM.active();
  const f = w && w.el.querySelector('iframe');
  if (f) try { f.contentWindow.postMessage({ type: 'os-shortcut', action, ...extra }, '*'); } catch {}
}
function cycleWindows(dir) {
  const ws = WM.list().filter((w) => !w.min).sort((a, b) => (parseInt(a.el.style.zIndex) || 0) - (parseInt(b.el.style.zIndex) || 0));
  if (ws.length < 2) { if (ws[0]) WM.toggle(ws[0].app.id); return; }
  const cur = WM.active();
  let i = cur ? ws.indexOf(cur) : -1;
  i = (i + dir + ws.length) % ws.length;
  WM.toggle(ws[i].app.id);                          // focus the next window
}
const isEditable = (t) => !!t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName || ''));

// One handler used by the shell document AND injected into every app iframe.
function osKeydown(e) {
  const ctrl = e.ctrlKey || e.metaKey;
  if (e.altKey && e.key === 'Tab') { e.preventDefault(); cycleWindows(e.shiftKey ? -1 : 1); return; }
  if (ctrl && (e.key === 'w' || e.key === 'W')) { const w = WM.active(); if (w) { e.preventDefault(); WM.close(w.app.id); } return; }
  if (ctrl && e.shiftKey && (e.key === 'v' || e.key === 'V')) { e.preventDefault(); toggleClipHistory(); return; }
  if (isEditable(e.target)) return;                 // text fields keep native Ctrl+C/V/A/Z, etc.
  let action = null;
  if (ctrl && !e.shiftKey) action = { s: 'save', c: 'copy', x: 'cut', v: 'paste', a: 'selectAll', z: 'undo', y: 'redo' }[e.key.toLowerCase()];
  else if (e.key === 'F2') action = 'rename';
  else if (e.key === 'Delete') action = 'delete';
  else if (e.key === 'F5') action = 'refresh';
  if (!action) return;
  e.preventDefault();
  // No window focused → the desktop itself is the target (select-all / delete its icons).
  if (!WM.active()) {
    if (action === 'selectAll') selectAllIcons();
    else if (action === 'delete') removeSelectedIcons();
    return;
  }
  sendToActive(action, action === 'paste' ? { clip: clipboardLatest() } : {});
}

// Clipboard history (Ctrl+Shift+V) — visualises the bounded, persisted history (Win+V style).
function toggleClipHistory() {
  if (!document.getElementById('ctxmenu').classList.contains('hidden')) { hideCtx(); return; }
  if (!clip.items.length) { showCtx(window.innerWidth / 2, window.innerHeight / 2, [{ label: 'Clipboard is empty', disabled: true }]); return; }
  const preview = (it) => it.kind === 'files' ? `${it.data.op || 'copy'}: ${(it.data.paths || []).length} item(s)` : String(it.data).slice(0, 40);
  showCtx(window.innerWidth / 2 - 90, 80, clip.items.map((it) => ({
    label: preview(it), glyph: it.kind === 'files' ? '🗂️' : '📋',
    fn: () => { clipboardWrite(it.kind, it.data); sendToActive('paste', { clip: clipboardLatest() }); },
  })));
}

function initOS() {
  loadClipboard();
  document.addEventListener('keydown', osKeydown);
  // Inject the same handler into each app window so shortcuts work over apps too.
  WM.setOnFrameLoad((frame) => {
    try { frame.contentWindow.addEventListener('keydown', osKeydown); } catch {}   // cross-origin (external links) → skip
  });
}

let needSeed = false;            // true when the device had no state yet → seed it
async function loadUiState() {
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(UI_STATE_PATH), { cache: 'no-store' });
    if (r.ok) return { ...UI_DEFAULTS, ...JSON.parse(await r.text()) };
  } catch {}
  needSeed = true;
  return migrateLegacy();        // first run: lift any pre-existing localStorage value onto the device
}
function migrateLegacy() {
  const legacy = (k, d) => { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } };
  const s = { pins: legacy('nucleo.pins', UI_DEFAULTS.pins), wallpaper: legacy('nucleo.wallpaper', UI_DEFAULTS.wallpaper) };
  localStorage.removeItem('nucleo.pins'); localStorage.removeItem('nucleo.wallpaper');
  return s;
}

let saveTimer = null, lastSaved = '';
function saveUiState() {
  const body = JSON.stringify({ pins: state.pins, wallpaper: state.wallpaper, desktop: state.desktop });
  lastSaved = body;              // remember our own write so we can ignore its echo
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    fetch('/api/fs/write?path=' + encodeURIComponent(UI_STATE_PATH), { method: 'POST', body }).catch(() => {});
  }, 400);
}

// Build the default desktop (one shortcut per installed app) used to seed a fresh device.
function seedDesktop() {
  return state.apps.map((a, i) => ({ uid: 'app-' + a.id, type: 'app', target: a.id, label: a.name }));
}
// Another client changed the shared UI state: reload and re-render (skip our own echo).
async function syncUiState() {
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(UI_STATE_PATH), { cache: 'no-store' });
    if (!r.ok) return;
    const body = await r.text();
    if (body === lastSaved) return;
    const s = { ...UI_DEFAULTS, ...JSON.parse(body) };
    state.pins = s.pins;
    state.desktop = Array.isArray(s.desktop) ? s.desktop : seedDesktop();
    if (s.wallpaper !== state.wallpaper) { state.wallpaper = s.wallpaper; applyWallpaper(s.wallpaper); }
    renderDesktop();
    renderTaskbar();
  } catch {}
}
const byId = (id) => state.apps.find((a) => a.id === id);
const fmtSize = (b) => (!b ? '—' : b >= 1e9 ? (b / 1e9).toFixed(1) + ' GB' : (b / 1e6).toFixed(0) + ' MB');

async function fetchJSON(path) {
  const r = await fetch(path, { cache: 'no-store' });
  if (!r.ok) throw new Error(r.status);
  return r.json();
}

// ===== pairing gate =====
// The device requires pairing before it will serve user data (files, OTA, live events).
// We ask /api/auth/status first; if this browser isn't paired yet, block the OS behind a
// full-screen overlay until the user types the 6-digit code shown on the Cardputer screen.
// On success the device sets an HttpOnly session cookie, so every later request — including
// from app iframes and the WebSocket — is authenticated automatically.
async function ensurePaired() {
  let st;
  try { st = await (await fetch('/api/auth/status', { cache: 'no-store' })).json(); }
  catch { return; }                              // device unreachable → let boot fall back to mock
  if (!st.required || st.paired) return;
  await showPairing();
}
function showPairing() {
  return new Promise((resolve) => {
    const ov = document.getElementById('pair-overlay');
    const form = document.getElementById('pair-form');
    const pin = document.getElementById('pair-pin');
    const msg = document.getElementById('pair-msg');
    const btn = document.getElementById('pair-submit');
    ov.classList.remove('hidden');
    pin.value = ''; pin.focus();
    pin.addEventListener('input', () => { pin.value = pin.value.replace(/\D/g, '').slice(0, 6); msg.textContent = ''; });
    form.addEventListener('submit', async (e) => {
      e.preventDefault();
      if (pin.value.length !== 6) { msg.textContent = 'Enter all 6 digits.'; return; }
      btn.disabled = true; msg.textContent = '';
      let r;
      try { r = await fetch('/api/pair', { method: 'POST', headers: { 'content-type': 'application/json' }, body: JSON.stringify({ pin: pin.value }) }); }
      catch { msg.textContent = 'Network error — is the device on?'; btn.disabled = false; return; }
      if (r.ok) { ov.classList.add('hidden'); resolve(); return; }   // cookie is set; proceed
      let body = {}; try { body = await r.json(); } catch {}
      msg.textContent = r.status === 429 || body.locked
        ? 'Too many attempts. Wait a moment and try again.'
        : 'Wrong code. Check the screen and retry.';
      pin.value = ''; pin.focus(); btn.disabled = false;
    });
  });
}

async function boot() {
  await ensurePaired();                          // block until this browser is paired with the device
  try {
    const d = await fetchJSON('/api/apps');
    state.apps = d.apps.filter((a) => a.enabled).map((a) => ({ ...a, glyph: glyph(a) }));
  } catch {
    state.apps = MOCK.map((a) => ({ ...a, glyph: glyph(a) }));
  }
  try { state.assoc = await fetchJSON('/api/associations'); } catch {}
  const ui = await loadUiState();          // pins + wallpaper come from the device, not the browser
  state.pins = ui.pins; state.wallpaper = ui.wallpaper;
  // Desktop shortcuts: use the saved arrangement, or seed one-per-app on a fresh device.
  state.desktop = Array.isArray(ui.desktop) ? ui.desktop : seedDesktop();
  if (!Array.isArray(ui.desktop)) needSeed = true;
  // Window changes (open/close/move/resize/min/max/focus) update the taskbar AND get
  // remembered by the device, so the session is restored on the next load.
  WM.setOnChange(() => { renderTaskbar(); saveSession(); });
  renderDesktop();
  renderStartMenu();
  renderTaskbar();
  wireChrome();
  wireMessages();
  initOS();                                // OS-wide clipboard + keyboard shortcuts
  applyWallpaper(state.wallpaper);
  if (needSeed) saveUiState();             // device had nothing yet → persist current state onto it
  applySettingsFromDevice();               // theme + device name are authoritative on the device too
  await restoreSession();                  // bring back the windows that were open last time
  connectWS();
  tickClock(); setInterval(tickClock, 10000);
  refreshStatus(); setInterval(refreshStatus, 15000);
}

// Live event stream: update the tray and forward every event to open app windows.
function connectWS() {
  let ws;
  try { ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws'); }
  catch { return; }
  ws.onopen = () => ws.send(JSON.stringify({ op: 'subscribe', since: 0 }));
  ws.onmessage = (m) => {
    let msg; try { msg = JSON.parse(m.data); } catch { return; }
    const events = msg.op === 'sync' ? (msg.events || []) : [msg];
    for (const ev of events) {
      if (!ev || !ev.t) continue;
      if (ev.t === 'fs.changed' || ev.t.startsWith('storage')) refreshStatus();
      // Shared UI state changed on the device (this or another client) → re-sync.
      if (ev.t === 'fs.changed' && ev.d && typeof ev.d.path === 'string' && ev.d.path.endsWith('ui-state.json')) syncUiState();
      if (ev.t === 'fs.changed' && ev.d && typeof ev.d.path === 'string' && ev.d.path.endsWith('settings.json')) applySettingsFromDevice();
      // The device handed its screen over to web clients (or reclaimed it) — reflect it.
      if (ev.t === 'system.remote' && ev.d) renderRemote(!!ev.d.active, ev.d.clients);
      for (const w of WM.list()) { const f = w.el.querySelector('iframe'); if (f) try { f.contentWindow.postMessage(ev, '*'); } catch {} }
    }
  };
  ws.onclose = () => { renderRemote(false); setTimeout(connectWS, 3000); };   // auto-reconnect
}

// Apps (e.g. File Commander) ask the shell to open a file with its associated app.
function wireMessages() {
  window.addEventListener('message', (e) => {
    const d = e.data;
    if (!d) return;
    if (d.type === 'set-theme' && d.theme) { applyTheme(d.theme); return; }   // live preview from Settings
    if (d.type === 'set-wallpaper' && d.path) {
      state.wallpaper = d.path; applyWallpaper(d.path); saveUiState(); return;
    }
    // OS clipboard service: apps copy/cut into it, or request the latest entry.
    if (d.type === 'clipboard-write' && d.kind) { clipboardWrite(d.kind, d.data); return; }
    if (d.type === 'clipboard-read') {
      if (e.source) try { e.source.postMessage({ type: 'clipboard-data', item: clipboardLatest() }, '*'); } catch {}
      return;
    }
    if (d.type !== 'open-file' || !d.path) return;
    openFile(d.path);
  });
}

function applyTheme(theme) { document.documentElement.dataset.theme = theme === 'light' ? 'light' : 'dark'; }
function applyDeviceName(name) {
  const el = document.getElementById('sm-device');
  if (el) el.textContent = name ? '· ' + name : '';
}
// Device-owned settings (theme, name) read straight from the canonical SD file.
async function applySettingsFromDevice() {
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent('/system/config/settings.json'), { cache: 'no-store' });
    if (!r.ok) return;
    const s = JSON.parse(await r.text());
    if (s.ui && s.ui.theme) applyTheme(s.ui.theme);
    if (s.device && s.device.name) applyDeviceName(s.device.name);
  } catch {}
}

function applyWallpaper(path) {
  const d = document.getElementById('desktop');
  d.style.backgroundImage = `url("/api/fs/read?path=${encodeURIComponent(path)}")`;
  d.style.backgroundSize = 'cover';
  d.style.backgroundPosition = 'center';
}

const escapeHtml = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// Resolve a desktop item to its display glyph + label (apps may be renamed/removed in the registry).
function itemGlyph(item) {
  if (item.type === 'app') { const a = byId(item.target); return a ? a.glyph : '❓'; }
  if (item.type === 'url') return '🔗';
  // file: borrow the associated app's glyph, else a generic document.
  const ext = (item.target.split('.').pop() || '').toLowerCase();
  const a = byId(state.assoc.default_open[ext]);
  return a ? a.glyph : '📄';
}
function itemLabel(item) {
  if (item.label) return item.label;
  if (item.type === 'app') { const a = byId(item.target); return a ? a.name : item.target; }
  if (item.type === 'file') return item.target.split('/').pop();
  return item.target;
}
// Open a desktop item according to its type.
function openItem(item) {
  if (item.type === 'app') { const a = byId(item.target); if (a) WM.open(a); return; }
  if (item.type === 'file') { openFile(item.target); return; }
  if (item.type === 'url') {
    if (/^https?:\/\//i.test(item.target)) window.open(item.target, '_blank', 'noopener');
    else WM.open({ id: 'link:' + item.target, name: itemLabel(item), route: item.target, glyph: '🔗' });
  }
}
// Open an SD file with its associated app (shared by File Commander and file shortcuts).
function openFile(path) {
  const ext = (path.split('.').pop() || '').toLowerCase();
  const app = byId(state.assoc.default_open[ext] || state.assoc.fallback);
  if (app) WM.open(app, 'path=' + encodeURIComponent(path));
}

// Desktop icons are freely positioned (Windows-style) and snap to a grid. Every icon
// gets a STABLE {x,y} the first time it is placed and that position is persisted to the
// device — so icons never silently reflow again (the old auto-flow recomputed layout from
// the live desktop height on every render, which made icons jump when the window resized,
// when another client with a different screen synced, or when a neighbour was dragged).
const CELL = 98, PAD = 14, ICON_W = 84, ICON_H = 92;   // icon box + grid gap; matches the old grid
const snap = (v) => Math.max(0, Math.round((v - PAD) / CELL)) * CELL + PAD;

// Desktop dimensions, with a sane fallback before first layout (clientHeight may be 0).
function desktopBox() {
  const d = document.getElementById('desktop');
  return { w: d.clientWidth || window.innerWidth, h: d.clientHeight || (window.innerHeight - 44) };
}
// Snapped cells already occupied by positioned items (optionally excluding some uids).
function occupiedCells(exclude) {
  const skip = exclude instanceof Set ? exclude : new Set(exclude ? [exclude] : []);
  const s = new Set();
  for (const it of state.desktop) {
    if (skip.has(it.uid) || it.x == null || it.y == null) continue;
    s.add(snap(it.x) + ',' + snap(it.y));
  }
  return s;
}
// First free grid cell, filling top-to-bottom then left-to-right (column-first), like Windows.
function firstFreeCell(taken) {
  const rows = Math.max(1, Math.floor((desktopBox().h - PAD) / CELL));
  for (let slot = 0; slot < 5000; slot++) {
    const x = PAD + Math.floor(slot / rows) * CELL, y = PAD + (slot % rows) * CELL;
    if (!taken.has(x + ',' + y)) return { x, y };
  }
  return { x: PAD, y: PAD };
}
// Nearest free cell to a desired position (ring search) — used on drop to avoid stacking.
function nearestFreeCell(px, py, taken) {
  const x0 = snap(px), y0 = snap(py);
  if (!taken.has(x0 + ',' + y0)) return { x: x0, y: y0 };
  for (let r = 1; r < 40; r++) {
    for (let dx = -r; dx <= r; dx++) for (let dy = -r; dy <= r; dy++) {
      if (Math.max(Math.abs(dx), Math.abs(dy)) !== r) continue;     // ring perimeter only
      const x = Math.max(PAD, x0 + dx * CELL), y = Math.max(PAD, y0 + dy * CELL);
      if (!taken.has(x + ',' + y)) return { x, y };
    }
  }
  return { x: x0, y: y0 };
}
// Clamp a position into the visible desktop for DISPLAY only (stored x/y stays authoritative,
// so a small screen never rewrites the shared layout used by a larger one).
function clampPos(x, y) {
  const { w, h } = desktopBox();
  if (w < ICON_W || h < ICON_H) return { x, y };          // not laid out yet
  return { x: Math.min(Math.max(0, x), w - ICON_W), y: Math.min(Math.max(0, y), h - ICON_H) };
}
// Assign a stable cell to every icon that lacks one; returns true if anything changed.
function assignMissingPositions() {
  const taken = occupiedCells();
  let changed = false;
  for (const it of state.desktop) {
    if (it.x != null && it.y != null) continue;
    const cell = firstFreeCell(taken);
    it.x = cell.x; it.y = cell.y; taken.add(cell.x + ',' + cell.y);
    changed = true;
  }
  return changed;
}

const selIcons = new Set();   // selected desktop item uids (Windows-style multi-select)
let iconAnchor = -1;          // anchor index for Shift-range

function renderDesktop() {
  const d = document.getElementById('desktop');
  d.innerHTML = '';
  // Pin down any icon that has no position yet, then persist once. After this an icon's
  // position is fixed until the user drags it — renders, resizes and cross-client syncs
  // no longer move it.
  if (assignMissingPositions()) saveUiState();
  for (const uid of [...selIcons]) if (!state.desktop.some((i) => i.uid === uid)) selIcons.delete(uid);  // drop stale
  state.desktop.forEach((item, idx) => {
    const pos = clampPos(snap(item.x), snap(item.y));
    const el = document.createElement('div');
    el.className = 'icon' + (selIcons.has(item.uid) ? ' sel' : ''); el.tabIndex = 0;
    el.dataset.uid = item.uid;
    el.style.left = pos.x + 'px'; el.style.top = pos.y + 'px';
    el.innerHTML = `<div class="glyph">${itemGlyph(item)}</div><div class="label">${escapeHtml(itemLabel(item))}</div>`;
    el.addEventListener('click', (e) => iconClick(e, item, idx));
    el.addEventListener('dblclick', () => openItem(item));
    el.addEventListener('keydown', (e) => { if (e.key === 'Enter') openItem(item); else if (e.key === 'Delete') removeSelectedIcons(); });
    el.addEventListener('contextmenu', (e) => {
      e.preventDefault(); e.stopPropagation();
      if (!selIcons.has(item.uid)) { selIcons.clear(); selIcons.add(item.uid); iconAnchor = idx; renderDesktop(); }
      iconMenu(e, item);
    });
    dragIcon(el, item);
    d.appendChild(el);
  });
}

// Click an icon: plain = select only it; Ctrl = toggle; Shift = range from the anchor.
function iconClick(e, item, idx) {
  if (draggedRecently) return;                // a drag just moved the group; don't reset selection
  if (e.ctrlKey || e.metaKey) {
    selIcons.has(item.uid) ? selIcons.delete(item.uid) : selIcons.add(item.uid); iconAnchor = idx;
  } else if (e.shiftKey && iconAnchor >= 0) {
    const [a, b] = [Math.min(iconAnchor, idx), Math.max(iconAnchor, idx)];
    selIcons.clear(); for (let k = a; k <= b; k++) selIcons.add(state.desktop[k].uid);
  } else { selIcons.clear(); selIcons.add(item.uid); iconAnchor = idx; }
  renderDesktop();
}
function selectAllIcons() { state.desktop.forEach((i) => selIcons.add(i.uid)); renderDesktop(); }
function removeSelectedIcons() {
  if (!selIcons.size) return;
  state.desktop = state.desktop.filter((x) => !selIcons.has(x.uid));
  selIcons.clear(); saveUiState(); renderDesktop();
}

// Drag an icon to reposition it; on drop, snap to the grid and persist {x,y} to the device.
// If the icon is part of a multi-selection, the whole group moves together (Windows-style).
let draggedRecently = false;       // suppress the click that fires right after a drag
function dragIcon(el, item) {
  let sx, sy, down = false, moving = false, group = [];
  el.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    down = true; moving = false; sx = e.clientX; sy = e.clientY;
    const uids = selIcons.has(item.uid) && selIcons.size > 1 ? [...selIcons] : [item.uid];
    group = uids.map((uid) => {
      const g = document.querySelector(`#desktop .icon[data-uid="${CSS.escape(uid)}"]`);
      return g && { uid, el: g, ox: g.offsetLeft, oy: g.offsetTop };
    }).filter(Boolean);
    try { el.setPointerCapture(e.pointerId); } catch {}   // best-effort; not required for the drag to work
  });
  el.addEventListener('pointermove', (e) => {
    if (!down) return;
    if (!moving && Math.abs(e.clientX - sx) + Math.abs(e.clientY - sy) < 5) return;  // ignore micro-moves (it's a click)
    moving = true;
    const dx = e.clientX - sx, dy = e.clientY - sy;
    for (const g of group) {
      g.el.classList.add('dragging');
      g.el.style.left = Math.max(0, g.ox + dx) + 'px';
      g.el.style.top = Math.max(0, g.oy + dy) + 'px';
    }
  });
  const end = () => {
    if (!down) return;
    down = false;
    if (!moving) return;
    draggedRecently = true; setTimeout(() => { draggedRecently = false; }, 0);   // don't let the click reset selection
    // Drop each dragged icon onto the nearest free cell so they never stack on a neighbour.
    const taken = occupiedCells(new Set(group.map((g) => g.uid)));
    for (const g of group) {
      const it = state.desktop.find((x) => x.uid === g.uid);
      if (!it) continue;
      const cell = nearestFreeCell(g.el.offsetLeft, g.el.offsetTop, taken);
      it.x = cell.x; it.y = cell.y; taken.add(cell.x + ',' + cell.y);
    }
    saveUiState(); renderDesktop();              // persist final positions + repaint
  };
  el.addEventListener('pointerup', end);
  el.addEventListener('pointercancel', end);
}

// ---- desktop shortcut management (deletable icons + Windows-style shortcuts) ----
const uniqueUid = (base) => { let u = base, i = 2; while (state.desktop.some((x) => x.uid === u)) u = base + '-' + i++; return u; };

function addItem(item) {
  item.uid = uniqueUid(item.uid || (item.type + '-' + item.target));
  state.desktop = [...state.desktop, item];
  saveUiState(); renderDesktop();
}
function removeItem(uid) {
  state.desktop = state.desktop.filter((x) => x.uid !== uid);
  saveUiState(); renderDesktop();
}
function renameItem(uid) {
  const item = state.desktop.find((x) => x.uid === uid); if (!item) return;
  const name = prompt('Rename shortcut', itemLabel(item));
  if (name == null) return;
  item.label = name.trim() || undefined;
  saveUiState(); renderDesktop();
}

// Context menu on an icon: Open / Rename / Remove from desktop (the app stays installed).
function iconMenu(e, item) {
  const multi = selIcons.size > 1;
  showCtx(e.clientX, e.clientY, (multi ? [
    { label: `Remove ${selIcons.size} from desktop`, danger: true, fn: () => removeSelectedIcons() },
  ] : [
    { label: 'Open', fn: () => openItem(item) },
    item.type === 'app' ? { label: state.pins.includes(item.target) ? 'Unpin from taskbar' : 'Pin to taskbar', fn: () => togglePin(item.target) } : null,
    { label: 'Rename…', fn: () => renameItem(item.uid) },
    { sep: true },
    { label: 'Remove from desktop', danger: true, fn: () => removeItem(item.uid) },
  ]).filter(Boolean));
}

// Context menu on the empty desktop: add a new shortcut (app / file / link) or reset.
function desktopMenu(e) {
  const onDesktop = new Set(state.desktop.filter((x) => x.type === 'app').map((x) => x.target));
  const addable = state.apps.filter((a) => !onDesktop.has(a.id));
  showCtx(e.clientX, e.clientY, [
    { label: 'New app shortcut…', disabled: !addable.length, fn: () => pickAppMenu(e, addable) },
    { label: 'New file shortcut…', fn: () => {
        const p = prompt('SD file path (e.g. /data/Music/song.mp3)'); if (p && p.trim()) addItem({ type: 'file', target: p.trim() });
      } },
    { label: 'New link…', fn: () => {
        const u = prompt('Link URL (https://… or /apps/…)'); if (u && u.trim()) addItem({ type: 'url', target: u.trim() });
      } },
    { label: 'Arrange icons', fn: () => arrangeIcons() },
    { sep: true },
    { label: 'Reset to all apps', danger: true, fn: () => { state.desktop = seedDesktop(); saveUiState(); renderDesktop(); } },
  ]);
}
// Re-flow every icon into a clean column-first grid (keeps the desktop order) and persist.
function arrangeIcons() {
  const taken = new Set();
  for (const it of state.desktop) {
    const cell = firstFreeCell(taken);
    it.x = cell.x; it.y = cell.y; taken.add(cell.x + ',' + cell.y);
  }
  saveUiState(); renderDesktop();
}
function pickAppMenu(e, apps) {
  showCtx(e.clientX, e.clientY, apps.map((a) => ({
    label: a.name, glyph: a.glyph, fn: () => addItem({ type: 'app', target: a.id, label: a.name }),
  })));
}

// ---- lightweight context-menu widget ----
function showCtx(x, y, entries) {
  const m = document.getElementById('ctxmenu');
  m.innerHTML = '';
  for (const en of entries) {
    if (en.sep) { const s = document.createElement('div'); s.className = 'ctx-sep'; m.appendChild(s); continue; }
    const it = document.createElement('div');
    it.className = 'ctx-item' + (en.danger ? ' danger' : '') + (en.disabled ? ' disabled' : '');
    it.innerHTML = (en.glyph ? `<span class="g">${en.glyph}</span>` : '') + `<span>${escapeHtml(en.label)}</span>`;
    if (!en.disabled) it.addEventListener('click', () => { hideCtx(); en.fn(); });
    m.appendChild(it);
  }
  m.classList.remove('hidden');
  // Clamp to viewport.
  const r = m.getBoundingClientRect();
  m.style.left = Math.min(x, window.innerWidth - r.width - 6) + 'px';
  m.style.top = Math.min(y, window.innerHeight - r.height - 6) + 'px';
}
function hideCtx() { document.getElementById('ctxmenu').classList.add('hidden'); }

function renderStartMenu() {
  const g = document.getElementById('sm-apps');
  g.innerHTML = '';
  for (const a of state.apps) {
    const el = document.createElement('div');
    el.className = 'sm-item';
    el.innerHTML = `<div class="glyph">${a.glyph}</div><div class="label">${a.name}</div>`;
    el.title = 'Click to open · right-click to pin';
    el.addEventListener('click', () => { WM.open(a); closeStart(); });
    el.addEventListener('contextmenu', (e) => { e.preventDefault(); togglePin(a.id); });
    g.appendChild(el);
  }
}

function renderTaskbar() {
  const pinned = document.getElementById('task-pinned');
  const running = document.getElementById('task-running');
  const openIds = new Set(WM.list().map((w) => w.app.id));
  pinned.innerHTML = '';
  for (const id of state.pins) {
    const a = byId(id); if (!a) continue;
    pinned.appendChild(taskBtn(a, openIds.has(id)));
  }
  running.innerHTML = '';
  for (const w of WM.list()) {
    if (state.pins.includes(w.app.id)) continue; // already shown as pinned
    running.appendChild(taskBtn(w.app, true));
  }
}

function taskBtn(a, isOpen) {
  const b = document.createElement('button');
  b.className = 'task-btn' + (isOpen ? ' running' : '');
  if (WM.list().some((w) => w.app.id === a.id && w.el.classList.contains('active') && !w.min)) b.classList.add('focus');
  b.innerHTML = `<span class="glyph">${a.glyph}</span><span class="label">${a.name}</span>`;
  b.addEventListener('click', () => (isOpen ? WM.toggle(a.id) : WM.open(a)));
  b.addEventListener('contextmenu', (e) => { e.preventDefault(); togglePin(a.id); });
  return b;
}

function togglePin(id) {
  state.pins = state.pins.includes(id) ? state.pins.filter((x) => x !== id) : [...state.pins, id];
  saveUiState();
  renderTaskbar();
}

// ---- taskbar search (apps + SD files), real-OS style ----
const SEARCH = { results: [], sel: 0, token: 0 };
let searchTimer = null;

// Bounded breadth-first file search over the SD via /api/fs/list — light enough for
// the ESP (caps total directory reads) and works the same in the simulator.
async function searchFiles(query, token) {
  const out = [], queue = ['/data'];
  let visits = 0;
  while (queue.length && out.length < 30 && visits < 60) {
    if (token !== SEARCH.token) return [];           // a newer query superseded us
    const dir = queue.shift(); visits++;
    let data; try { data = await (await fetch('/api/fs/list?path=' + encodeURIComponent(dir), { cache: 'no-store' })).json(); } catch { continue; }
    for (const e of data.entries || []) {
      const path = dir.endsWith('/') ? dir + e.name : dir + '/' + e.name;
      if (e.type === 'dir') { if (dir.split('/').length < 5) queue.push(path); }
      if (e.name.toLowerCase().includes(query)) out.push({ kind: 'file', path, name: e.name, isDir: e.type === 'dir' });
    }
  }
  return out;
}

async function runSearch() {
  const q = document.getElementById('search-input').value.trim().toLowerCase();
  if (!q) return closeSearch();
  const token = ++SEARCH.token;
  const apps = state.apps.filter((a) => a.name.toLowerCase().includes(q)).map((a) => ({ kind: 'app', app: a }));
  renderSearch(apps, null, q);                        // show apps immediately
  const files = await searchFiles(q, token);
  if (token === SEARCH.token) renderSearch(apps, files, q);
}

function renderSearch(apps, files, q) {
  const panel = document.getElementById('search-panel');
  SEARCH.results = [...apps, ...(files || [])];
  SEARCH.sel = 0;
  let html = '';
  if (apps.length) {
    html += '<div class="sp-cat">Apps</div>';
    apps.forEach((r, i) => { html += spRow(i, r.app.glyph, r.app.name, ''); });
  }
  if (files === null) html += '<div class="sp-cat">Files</div><div class="sp-empty">Searching…</div>';
  else if (files.length) {
    const base = apps.length;
    html += '<div class="sp-cat">Files</div>';
    files.forEach((r, i) => { html += spRow(base + i, r.isDir ? '📁' : itemGlyph({ type: 'file', target: r.path }), r.name, r.path); });
  }
  if (!SEARCH.results.length && files !== null) html = `<div class="sp-empty">No apps or files match “${escapeHtml(q)}”.</div>`;
  panel.innerHTML = html;
  panel.classList.remove('hidden');
  [...panel.querySelectorAll('.sp-item')].forEach((el) => {
    el.addEventListener('click', () => openResult(SEARCH.results[+el.dataset.i]));
    el.addEventListener('mousemove', () => setSel(+el.dataset.i));
  });
  highlightSel();
}
const spRow = (i, g, name, sub) =>
  `<div class="sp-item" data-i="${i}"><span class="g">${g}</span><span class="t"><div class="n">${escapeHtml(name)}</div>${sub ? `<div class="p">${escapeHtml(sub)}</div>` : ''}</span></div>`;

function setSel(i) { SEARCH.sel = i; highlightSel(); }
function moveSel(d) {
  if (!SEARCH.results.length) return;
  SEARCH.sel = (SEARCH.sel + d + SEARCH.results.length) % SEARCH.results.length;
  highlightSel(true);
}
function highlightSel(scroll) {
  const items = document.querySelectorAll('#search-panel .sp-item');
  items.forEach((el, i) => el.classList.toggle('sel', i === SEARCH.sel));
  if (scroll && items[SEARCH.sel]) items[SEARCH.sel].scrollIntoView({ block: 'nearest' });
}
function openResult(r) {
  if (!r) return;
  if (r.kind === 'app') WM.open(r.app);
  else if (r.isDir) WM.open(byId('file-commander'), 'path=' + encodeURIComponent(r.path));
  else openFile(r.path);
  closeSearch(); const i = document.getElementById('search-input'); i.value = ''; i.blur();
}
function closeSearch() { document.getElementById('search-panel').classList.add('hidden'); SEARCH.results = []; SEARCH.sel = 0; }

function wireSearch() {
  const input = document.getElementById('search-input');
  input.addEventListener('input', () => { clearTimeout(searchTimer); searchTimer = setTimeout(runSearch, 180); });
  input.addEventListener('focus', () => { if (input.value.trim()) runSearch(); });
  input.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown') { e.preventDefault(); moveSel(1); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); moveSel(-1); }
    else if (e.key === 'Enter') { e.preventDefault(); openResult(SEARCH.results[SEARCH.sel]); }
    else if (e.key === 'Escape') { closeSearch(); input.value = ''; input.blur(); }
  });
}

function wireChrome() {
  document.getElementById('start-btn').addEventListener('click', toggleStart);
  wireSearch();
  document.addEventListener('click', (e) => {
    const sm = document.getElementById('start-menu');
    if (!sm.contains(e.target) && !document.getElementById('start-btn').contains(e.target)) closeStart();
    if (!document.getElementById('ctxmenu').contains(e.target)) hideCtx();
    if (!document.getElementById('search').contains(e.target) && !document.getElementById('search-panel').contains(e.target)) closeSearch();
  });
  // Right-click on the empty desktop opens the "new shortcut" menu.
  const desktop = document.getElementById('desktop');
  desktop.addEventListener('contextmenu', (e) => {
    if (e.target !== desktop) return;            // icon menus handle their own right-click
    e.preventDefault(); desktopMenu(e);
  });
  wireMarquee(desktop);                          // rubber-band multi-select on the empty desktop
  window.addEventListener('blur', hideCtx);
  let resizeTimer = null;
  window.addEventListener('resize', () => {
    hideCtx();
    // Re-clamp icons into the new bounds for display (stored positions are untouched, so
    // resizing never permanently moves an icon — it just stays on-screen).
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(renderDesktop, 150);
  });
}

// Rubber-band selection: drag a rectangle over the empty desktop to select icons within it
// (hold Ctrl to add to the current selection), exactly like the Windows desktop.
function wireMarquee(desktop) {
  let box = null, x0 = 0, y0 = 0, addMode = false;
  desktop.addEventListener('pointerdown', (e) => {
    if (e.target !== desktop || e.button !== 0) return;
    addMode = e.ctrlKey || e.metaKey;
    if (!addMode) { selIcons.clear(); renderDesktop(); }
    x0 = e.clientX; y0 = e.clientY;
    box = document.createElement('div'); box.className = 'marquee';
    desktop.appendChild(box);
    try { desktop.setPointerCapture(e.pointerId); } catch {}
  });
  desktop.addEventListener('pointermove', (e) => {
    if (!box) return;
    const x = Math.min(x0, e.clientX), y = Math.min(y0, e.clientY), w = Math.abs(e.clientX - x0), h = Math.abs(e.clientY - y0);
    Object.assign(box.style, { left: x + 'px', top: y + 'px', width: w + 'px', height: h + 'px' });
    const r = { left: x, top: y, right: x + w, bottom: y + h };
    desktop.querySelectorAll('.icon').forEach((el) => {
      const b = el.getBoundingClientRect();
      const hit = b.left < r.right && b.right > r.left && b.top < r.bottom && b.bottom > r.top;
      el.classList.toggle('sel', hit || (addMode && selIcons.has(el.dataset.uid)));
    });
  });
  const finish = (e) => {
    if (!box) return;
    desktop.querySelectorAll('.icon.sel').forEach((el) => selIcons.add(el.dataset.uid));
    box.remove(); box = null;
    try { desktop.releasePointerCapture(e.pointerId); } catch {}
    renderDesktop();
  };
  desktop.addEventListener('pointerup', finish);
  desktop.addEventListener('pointercancel', finish);
}
const toggleStart = () => document.getElementById('start-menu').classList.toggle('hidden');
const closeStart = () => document.getElementById('start-menu').classList.add('hidden');

function tickClock() {
  const d = new Date();
  document.getElementById('clock').textContent =
    String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
}

async function refreshStatus() {
  try {
    const s = await fetchJSON('/api/status');
    const txt = `SD ${fmtSize(s.storage.free_bytes)} free`;
    document.querySelector('#tray-storage .v').textContent = txt;
    document.getElementById('sm-storage').textContent = `${s.storage.fs} · ${txt}`;
    document.getElementById('sm-version').textContent = 'v' + s.version;
    renderNetwork(s.network);
  } catch {
    document.querySelector('#tray-storage .v').textContent = 'offline';
    renderNetwork(null);
  }
}

// Network indicator in the tray: connected SSID in STA mode, or the setup AP name.
function renderNetwork(net) {
  const el = document.getElementById('tray-net');
  if (!el) return;
  let icon, label, title;
  if (!net) { icon = ICONS.offline; label = 'offline'; title = 'Device unreachable'; }
  else if (net.mode === 'sta' && net.ssid) {
    icon = ICONS.wifi; label = net.ssid; title = net.ip ? `Wi-Fi ${net.ssid} · ${net.ip}` : `Wi-Fi ${net.ssid}`;
  } else {
    icon = ICONS.ap; label = net.ssid || 'AP'; title = `Access Point ${net.ssid || ''} · http://192.168.4.1`;
  }
  el.innerHTML = icon + `<span class="v">${escapeHtml(label)}</span>`;
  el.title = title;
}

// Remote-control indicator: shown when the device has handed its screen over to web
// clients (the firmware publishes system.remote). Mirrors the on-device "Remote session"
// screen so both ends agree on who is in control.
function renderRemote(active, clients) {
  const el = document.getElementById('tray-remote');
  if (!el) return;
  el.hidden = !active;
  if (!active) return;
  const n = Number.isFinite(clients) ? clients : null;
  el.innerHTML = `🎮<span class="v">Controlling${n && n > 1 ? ` (${n})` : ''}</span>`;
  el.title = n && n > 1
    ? `This browser is controlling the device (${n} sessions). The Cardputer screen is paused.`
    : 'This browser is controlling the device. The Cardputer screen is paused.';
}

if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(() => {});
boot();

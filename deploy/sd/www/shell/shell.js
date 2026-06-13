// NucleoOS desktop shell controller. Loads apps + status from the device and
// renders the desktop, taskbar and Start menu. Heavy lifting stays in the browser.
import * as WM from './wm.js';
import * as FsIndex from './fsindex.js';
import { resolveShortcut, resolveEscape } from './shortcuts.js';
import { createBusyController } from './busy.js';
import { ensureOnboarding } from './onboarding.js';   // first-boot AI setup + install tutorial
import I18N from './nucleo-i18n.js';                  // centralized OS-wide internationalization

// Shell-namespaced translator: t(key, vars) → active-language string, falling back to core then key.
// The catalog is loaded by I18N.init('shell') at boot; before that, calls return the key (harmless,
// since the only t() callers below run after boot).
const t = I18N.scope('shell');

// The OS-wide AI copilot is loaded lazily in initOS(); held here so OS-level handlers
// (Escape, the unified search row) can talk to it. null until copilot.js initialises.
let Copilot = null;

// The system Notification Center (the single web surface for all notifications). Loaded lazily
// in initOS(); the WebSocket handler routes notify.post / calendar.reminder into it. See notify.js.
let Notify = null;

// Emoji glyphs keep the device free of icon assets (a deliberate weight trick).
const GLYPHS = {
  'file-commander': '🗂️', notepad: '📝', 'photo-viewer': '🖼️', 'media-player': '🎵',
  'video-player': '🎬', 'automation-studio': '⚙️', 'ir-remote': '📡', swarm: '🛰️',
  settings: '🔧', 'log-viewer': '📜', 'system-monitor': '🩺', calculator: '🧮', clock: '🕐',
  calendar: '📅', dosbox: '🕹️', recorder: '🎙️', 'recycle-bin': '♻️', updates: '🔄',
  anima: '🧠', help: '❓', browser: '🌐', spreadsheet: '📊', games: '🎮',
};
const glyph = (a) => {
  const fb = GLYPHS[a.id] || (a.name ? a.name[0].toUpperCase() : '▦');
  if (a.icon) {
    let src = a.icon;
    if (!src.startsWith('data:') && !src.startsWith('http:') && !src.startsWith('https:')) {
      if (!src.startsWith('/')) {
        let appPath = a.path || a.web_route || `/apps/${a.id}`;
        if (appPath.endsWith('/')) appPath = appPath.slice(0, -1);
        src = appPath + '/' + src;
      }
      if (src.startsWith('/data/') || src.startsWith('/system/')) src = '/api/fs/read?path=' + encodeURIComponent(src);
    }
    // A missing/404 icon file (e.g. an app whose icon.svg wasn't deployed) must NOT leave a broken
    // image — fall back to the emoji glyph in place. Resilient to any not-yet-deployed app icon.
    const safeFb = fb.replace(/['"\\<>&]/g, '');
    return `<img src="${src}" onerror="this.onerror=null;this.replaceWith(document.createTextNode('${safeFb}'))" style="width:1em; height:1em; vertical-align:middle; pointer-events:none; filter: drop-shadow(0 2px 4px rgba(0,0,0,0.15));">`;
  }
  return fb;
};

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
// startPins: apps pinned to the Start menu grid (separate from taskbar `pins`, exactly like
//   Windows 11). null -> seed from the installed apps on first run.
// recent: recently opened files (newest first), shown under "Consigliati" in Start. Each is
//   { path, ts }. Bounded so it never bloats the SD.
// iconSize: 'sm'|'md'|'lg' — desktop icon scale (Windows "View" submenu). autoArrange: when on,
// every render re-flows icons into a clean grid (drag is disabled, like Windows auto-arrange).
const UI_DEFAULTS = { pins: [], wallpaper: '/data/Pictures/wallpaper.png', desktop: null, startPins: null, recent: [], recoCollapsed: false, iconSize: 'md', autoArrange: false };
const RECENT_CAP = 12;

const state = { apps: [], pins: [...UI_DEFAULTS.pins], wallpaper: UI_DEFAULTS.wallpaper,
  startPins: [], recent: [], recoCollapsed: false, iconSize: UI_DEFAULTS.iconSize, autoArrange: UI_DEFAULTS.autoArrange,
  desktop: [], assoc: {
    default_open: {
      txt: 'notepad', md: 'notepad', log: 'notepad', json: 'notepad', csv: 'notepad',
      ini: 'notepad', cfg: 'notepad', yaml: 'notepad', yml: 'notepad', toml: 'notepad',
      xml: 'notepad', html: 'notepad', c: 'notepad', h: 'notepad', cpp: 'notepad',
      py: 'notepad', sh: 'notepad',
      jpg: 'photo-viewer', jpeg: 'photo-viewer', png: 'photo-viewer', bmp: 'photo-viewer', gif: 'photo-viewer',
      wav: 'media-player', mp3: 'media-player',
      mp4: 'video-player', webm: 'video-player', mov: 'video-player', mkv: 'video-player',
      todo: 'tasks', info: 'help'
    },
    fallback: 'file-commander'
  } };

// Applica immediatamente lo sfondo in cache (se esiste) per eliminare il flash
// visivo prima che l'OS finisca di caricare lo stato dal dispositivo via rete.
try {
  const cached = localStorage.getItem('nucleo.wallpaper.cache');
  if (cached) applyWallpaper(cached);
} catch {}

// Open-window session (which apps are open + their geometry) is remembered by the device
// so the desktop comes back as you left it. Kept in its own file — unlike the shared
// desktop arrangement it is NOT live-synced across clients (windows shouldn't jump around
// when another client moves one); it is only restored on load.
const SESSION_PATH = '/system/config/session.json';

async function fetchWithRetry(url, options = {}, maxRetries = 3) {
  for (let i = 0; i < maxRetries; i++) {
    const res = await fetch(url, options);
    if (res.status === 503 || res.status === 504) {
      if (i < maxRetries - 1) {
        let delay = 500;
        try {
          const j = await res.clone().json();
          if (j.retry_after_ms) delay = j.retry_after_ms;
        } catch {}
        await new Promise(r => setTimeout(r, delay));
        continue;
      }
    }
    return res;
  }
}

let restoring = false, sessTimer = null;
// Per-app window geometry that OUTLIVES a close: { appId -> {x,y,w,h,max,snap} }. The session's
// `windows` array says what was open; `geom` lets a freshly-reopened app reclaim its last size.
let winGeom = {};
function saveSession() {
  if (restoring) return;
  clearTimeout(sessTimer);
  sessTimer = setTimeout(() => {
    const body = JSON.stringify({ windows: WM.serialize(), geom: winGeom });
    fetchWithRetry('/api/fs/write?path=' + encodeURIComponent(SESSION_PATH), { method: 'POST', body }).catch(() => {});
  }, 500);
}
async function restoreSession() {
  let saved;
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(SESSION_PATH), { cache: 'no-store' });
    if (!r.ok) return;
    saved = JSON.parse(await r.text());
  } catch { return; }
  if (!saved) return;
  if (saved.geom && typeof saved.geom === 'object') winGeom = saved.geom;   // restore per-app memory first
  if (!Array.isArray(saved.windows)) return;
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
const CLIP_ITEM_MAX = 8 * 1024;                   // per-entry byte cap: a huge paste (whole Notepad doc) must
const CLIP_TOTAL_MAX = 64 * 1024;                 // not bloat the file rewritten to SD on EVERY copy
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
    fetchWithRetry('/api/fs/write?path=' + encodeURIComponent(CLIP_PATH), { method: 'POST', body: JSON.stringify({ items: clip.items }) }).catch(() => {});
  }, 400);
}
function clipboardWrite(kind, data) {
  // Cap a single entry by size: text apps (Notepad/ANIMA) can copy an entire document, and the whole
  // history is rewritten to SD on every Ctrl+C — an unbounded entry made each copy a multi-KB write
  // (exclusive SW lock + a cascade of fs.changed). Truncate with a marker; clipping is display-only.
  if (typeof data === 'string' && data.length > CLIP_ITEM_MAX) data = data.slice(0, CLIP_ITEM_MAX) + '…';
  const key = JSON.stringify({ kind, data });
  clip.items = clip.items.filter((it) => JSON.stringify({ kind: it.kind, data: it.data }) !== key);  // dedup
  clip.items.unshift({ kind, data, ts: Date.now() });
  if (clip.items.length > CLIP_CAP) clip.items.length = CLIP_CAP;                                     // bound count
  // Bound TOTAL bytes too (many medium entries): drop oldest until under budget, always keep the newest.
  let total = 0;
  for (let i = 0; i < clip.items.length; i++) {
    total += (clip.items[i].data && clip.items[i].data.length) || 0;
    if (total > CLIP_TOTAL_MAX && i > 0) { clip.items.length = i; break; }
  }
  saveClipboard();
}
const clipboardLatest = () => clip.items[0] || null;

// Forward an editing intent to the focused app window (it decides what to do).
function sendToActive(action, extra) {
  const w = WM.active();
  const f = w && w.el.querySelector('iframe');
  if (f) try { f.contentWindow.postMessage({ type: 'os-shortcut', action, ...extra }, '*'); } catch {}
}
let tsWindows = [];
let tsIndex = 0;
let tsActive = false;
let tsKeyUp = null;                                // the live Alt/Win keyup handler, so we can tear it down
// Cancel the Alt+Tab switcher without committing (used on blur/tab-hide/Escape). Without this the
// overlay could get stuck visible if the modifier keyup never arrives (window lost focus mid-cycle).
function cancelTaskSwitcher() {
  if (!tsActive && !tsKeyUp) return;
  tsActive = false;
  if (tsKeyUp) { window.removeEventListener('keyup', tsKeyUp); tsKeyUp = null; }
  const ov = document.getElementById('task-switcher');
  if (ov) ov.classList.add('hidden');
}
function cycleWindows(dir) {
  tsWindows = WM.list().sort((a, b) => (parseInt(b.el.style.zIndex) || 0) - (parseInt(a.el.style.zIndex) || 0));
  if (tsWindows.length < 2) { if (tsWindows[0]) WM.open(tsWindows[0].app); return; }
  let tsOverlay = document.getElementById('task-switcher');
  if (!tsOverlay) { tsOverlay = document.createElement('div'); tsOverlay.id = 'task-switcher'; document.body.appendChild(tsOverlay); }
  if (!tsActive) {
    tsActive = true; tsIndex = dir > 0 ? 1 : tsWindows.length - 1;
    tsKeyUp = (e) => {
      if (e.key === 'Alt' || e.key === 'Meta') {
        window.removeEventListener('keyup', tsKeyUp); tsKeyUp = null;
        tsOverlay.classList.add('hidden'); tsActive = false;
        const w = tsWindows[tsIndex];
        if (w) WM.open(w.app);
      }
    };
    window.addEventListener('keyup', tsKeyUp);
  } else {
    tsIndex = (tsIndex + dir + tsWindows.length) % tsWindows.length;
  }
  tsOverlay.innerHTML = '';
  tsWindows.forEach((w, i) => {
    const el = document.createElement('div');
    el.className = 'ts-item' + (i === tsIndex ? ' sel' : '');
    el.innerHTML = `<div class="glyph">${w.app.glyph || '▦'}</div><div class="t">${escapeHtml(w.app.name)}</div>`;
    tsOverlay.appendChild(el);
  });
  tsOverlay.classList.remove('hidden');
}
const isEditable = (t) => !!t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName || ''));

let metaTap = false;                               // armed by a lone Win/⌘ keydown, fired on keyup
function osKeyup(e) {
  if (e.key === 'Meta') { if (metaTap) toggleStart(); metaTap = false; }
}

// One handler used by the shell document AND injected into every app iframe.
function osKeydown(e) {
  const ctrl = e.ctrlKey || e.metaKey;

  // While a blocking model install is up, the OS is strictly "wait or cancel": swallow every navigation /
  // window / editing shortcut (Alt+Tab, Win, Ctrl+W, Esc, …) so the user can't slip away from the installer.
  // The installer's own Cancel is a button click, so it still works. (Browser reload we can't truly block;
  // it just aborts the download, which is resumable.)
  if (installScrim) { e.preventDefault(); e.stopPropagation(); return; }

  // Esc belongs to the focused app — like every real desktop OS, the system never closes an app on
  // Esc (apps own it to cancel a dialog, clear a selection, or dismiss a popup of their own). We only
  // consume it to dismiss the topmost open SYSTEM surface; the moment none is up, Esc falls through to
  // the app frame untouched (this handler is injected into every iframe, so the app's own listeners
  // still fire). The priority is decided by the pure, unit-tested resolveEscape(), which can NEVER
  // return a "close window" verdict. Closing a window stays an explicit gesture: title-bar ✕ or Ctrl+W.
  if (e.key === 'Escape') {
    const dlg = document.getElementById('os-dialog-scrim');
    const ac = document.getElementById('action-center');
    const sm = document.getElementById('start-menu');
    const ctx = document.getElementById('ctxmenu');
    const shown = (el) => !!el && !el.classList.contains('hidden');
    switch (resolveEscape({
      taskSwitcher: tsActive,
      dialog: !!(dlg && dlg.classList.contains('visible')),
      copilot: !!(Copilot && Copilot.isOpen && Copilot.isOpen()),
      actionCenter: shown(ac),
      start: shown(sm),
      contextMenu: shown(ctx),
    })) {
      case 'taskSwitcher': cancelTaskSwitcher(); break;
      case 'dialog':       closeOsFileDialog(null); break;
      case 'copilot':      Copilot.close(); break;
      case 'actionCenter': ac.classList.add('hidden'); break;
      case 'start':        closeStart(); break;
      case 'contextMenu':  hideCtx(); break;
      default:             return;   // 'app' → leave Esc to the focused app; the OS never closes a window on Esc
    }
    e.preventDefault();   // a system surface consumed Esc — don't let it also reach the app/browser
    return;
  }

  // Ctrl/⌘+Space → summon the OS-wide AI copilot from anywhere (works inside app iframes too,
  // since this handler is injected into every app frame).
  if (ctrl && e.key === ' ') { e.preventDefault(); if (Copilot) Copilot.toggle(); return; }
  if (e.altKey && e.key === 'Tab') { e.preventDefault(); cycleWindows(e.shiftKey ? -1 : 1); return; }
  // Win/⌘ TAPPED alone → toggle Start (decided on keyup). On keydown we only arm it, so that
  // Win+<arrow>/Win+<key> combos don't ALSO pop the Start menu (the old bug).
  if (e.key === 'Meta') { e.preventDefault(); metaTap = true; return; }
  if (e.metaKey) metaTap = false;                  // another key while Win is held = a combo, not a tap
  // Win/⌘ + arrows = Windows-11 keyboard snapping on the focused window (OS-level, even while typing).
  if (e.metaKey && !e.ctrlKey && !e.altKey && /^Arrow(Left|Right|Up|Down)$/.test(e.key)) {
    const w = WM.active();
    if (w) { e.preventDefault(); WM.snapByKey(w.app.id, e.key.slice(5).toLowerCase()); }
    return;
  }
  // Desktop keyboard navigation (arrows / Home / End / F2 / Enter) when no window/chrome has focus.
  if (desktopNavKey(e)) return;
  if (ctrl && (e.key === 'w' || e.key === 'W')) { const w = WM.active(); if (w) { e.preventDefault(); WM.close(w.app.id); } return; }
  if (ctrl && e.shiftKey && (e.key === 'v' || e.key === 'V')) { e.preventDefault(); toggleClipHistory(); return; }
  // Resolve the remaining OS shortcuts through the shared pure resolver (unit-tested in
  // tools/shortcuts.test.mjs). Document actions (Ctrl+S/Shift+S/N/O) fire even while typing and must
  // NOT fall through to the browser; editing actions route to the app, or the desktop when idle.
  const sc = resolveShortcut(e, { hasActiveWindow: !!WM.active(), editable: isEditable(e.target) });
  if (sc.type === 'doc') { e.preventDefault(); sendToActive(sc.action); return; }
  if (sc.type !== 'edit') return;                   // 'native' (text field) or 'none' → leave it be
  e.preventDefault();
  // No window focused → the desktop itself is the target (select-all / delete its icons).
  if (!WM.active()) {
    if (sc.action === 'selectAll') selectAllIcons();
    else if (sc.action === 'delete') removeSelectedIcons();
    return;
  }
  sendToActive(sc.action, sc.action === 'paste' ? { clip: clipboardLatest() } : {});
}

// Clipboard history (Ctrl+Shift+V) — visualises the bounded, persisted history (Win+V style).
function toggleClipHistory() {
  if (!document.getElementById('ctxmenu').classList.contains('hidden')) { hideCtx(); return; }
  if (!clip.items.length) { showCtx(window.innerWidth / 2, window.innerHeight / 2, [{ label: t('clipboard_empty'), disabled: true }]); return; }
  const preview = (it) => it.kind === 'files' ? `${it.data.op || 'copy'}: ${(it.data.paths || []).length} item(s)` : String(it.data).slice(0, 40);
  showCtx(window.innerWidth / 2 - 90, 80, clip.items.map((it) => ({
    label: preview(it), glyph: it.kind === 'files' ? '🗂️' : '📋',
    fn: () => { clipboardWrite(it.kind, it.data); sendToActive('paste', { clip: clipboardLatest() }); },
  })));
}

function initOS() {
  loadClipboard();
  document.addEventListener('keydown', osKeydown);
  document.addEventListener('keyup', osKeyup);
  // Never leave the Alt+Tab overlay stuck if the window loses focus mid-cycle.
  window.addEventListener('blur', cancelTaskSwitcher);
  document.addEventListener('visibilitychange', () => { if (document.hidden) cancelTaskSwitcher(); });
  // Load the OS-wide AI copilot (ANIMA as a system service). Additive: it reuses the /api/anima
  // engine and acts on the OS through this small surface — the same handlers the shell already uses.
  const OS_API = { byId, WM, openFile, showToast, refreshStatus, FsIndex };
  import('./copilot.js').then((m) => { Copilot = m.initCopilot(OS_API); }).catch((e) => console.warn('[copilot] load failed', e));
  // System Notification Center: one surface for every source (calendar, ANIMA, system, …).
  import('./notify.js').then((m) => { Notify = m.initNotify(OS_API); window.Notify = Notify; }).catch((e) => console.warn('[notify] load failed', e));
  // A notification whose action is "anima:<query>" asks the copilot when clicked.
  document.addEventListener('nucleo:anima-ask', (e) => { if (Copilot && e.detail && e.detail.q) Copilot.ask(e.detail.q); });
  // Inject the same handlers into each app window so shortcuts work over apps too.
  WM.setOnFrameLoad((frame) => {
    try { frame.contentWindow.addEventListener('keydown', osKeydown); frame.contentWindow.addEventListener('keyup', osKeyup); } catch {}   // cross-origin (external links) → skip
    injectGlobalTheme(frame.contentDocument);
  });
  // NOTE: desktop rubber-band selection is wired once by wireMarquee() (called from wireChrome).
  // A second marquee handler here would double-bind #desktop and fight the first one.
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
  const body = JSON.stringify({ pins: state.pins, wallpaper: state.wallpaper, desktop: state.desktop, startPins: state.startPins, recent: state.recent, recoCollapsed: state.recoCollapsed, iconSize: state.iconSize, autoArrange: state.autoArrange });
  lastSaved = body;              // remember our own write so we can ignore its echo
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    fetchWithRetry('/api/fs/write?path=' + encodeURIComponent(UI_STATE_PATH), { method: 'POST', body }).catch(() => {});
  }, 400);
}

// Build the default desktop (one shortcut per installed app) used to seed a fresh device.
function seedDesktop() {
  return state.apps.map((a, i) => ({ uid: 'app-' + a.id, type: 'app', target: a.id, label: a.name }));
}
// Add a desktop shortcut for any installed app that doesn't have one yet (so newly
// installed apps — e.g. ANIMA — appear on an existing desktop, not only in Start).
// Returns the number added.
function addMissingAppIcons() {
  const have = new Set(state.desktop.filter((it) => it.type === 'app').map((it) => it.target));
  let added = 0;
  for (const a of state.apps) if (!have.has(a.id)) {
    state.desktop.push({ uid: 'app-' + a.id, type: 'app', target: a.id, label: a.name }); added++;
  }
  return added;
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
    state.startPins = Array.isArray(s.startPins) ? s.startPins : state.apps.map((a) => a.id);
    state.recent = Array.isArray(s.recent) ? s.recent : [];
    state.recoCollapsed = !!s.recoCollapsed;
    if (s.iconSize === 'sm' || s.iconSize === 'md' || s.iconSize === 'lg') state.iconSize = s.iconSize;
    state.autoArrange = !!s.autoArrange;
    state.desktop = Array.isArray(s.desktop) ? s.desktop : seedDesktop();
    if (s.wallpaper !== state.wallpaper) { state.wallpaper = s.wallpaper; applyWallpaper(s.wallpaper); }
    applyIconSize();
    renderDesktop();
    renderStartMenu();
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
  // Timeout the probe: a hung connection must never trap boot on the splash screen.
  try { st = await (await fetch('/api/auth/status', { cache: 'no-store', signal: AbortSignal.timeout(4000) })).json(); }
  catch { return; }                              // device unreachable/slow → let boot fall back to mock
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
    // Defensive: if the pairing markup is missing (e.g. a stale cached index.html during a SW
    // update), don't throw — resolve so boot proceeds instead of hanging on the splash forever.
    if (!ov || !form || !pin || !btn) { resolve(); return; }
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

function hideBootScreen() { const bs = document.getElementById('boot-screen'); if (bs) bs.classList.add('hidden'); }
async function boot() {
  // The splash must never be a dead end: even if a fetch hangs forever (no reject, just no
  // response) this failsafe reveals the UI. The happy path clears it in the finally below.
  const bootFailsafe = setTimeout(hideBootScreen, 8000);
  try {
  // Load the active language and paint the static chrome ([data-i18n] in index.html) before anything
  // is shown — including the pairing overlay. Re-render the imperatively-built surfaces (Start menu,
  // taskbar, tray) whenever the OS language changes live (from Settings, any window).
  await I18N.init('shell');
  I18N.onChange(() => { try { renderStartMenu(); renderTaskbar(); setWsBadge(wsState); if (searchActive()) refreshSearchView(); } catch {} });
  await ensurePaired();                          // block until this browser is paired with the device
  try {
    const d = await fetchJSON('/api/apps');
    state.apps = d.apps.filter((a) => a.enabled).map((a) => ({ ...a, glyph: glyph(a) }));
  } catch {
    state.apps = MOCK.map((a) => ({ ...a, glyph: glyph(a) }));
  }
  try {
    const loadedAssoc = await fetchJSON('/api/associations');
    if (loadedAssoc && loadedAssoc.default_open) {
      state.assoc.default_open = { ...state.assoc.default_open, ...loadedAssoc.default_open };
      if (loadedAssoc.fallback) state.assoc.fallback = loadedAssoc.fallback;
    }
  } catch {}
  const ui = await loadUiState();          // pins + wallpaper come from the device, not the browser
  state.pins = ui.pins; state.wallpaper = ui.wallpaper;
  // Start-menu pins: saved arrangement, or seed one-per-app on a fresh device (like Windows 11).
  state.startPins = Array.isArray(ui.startPins) ? ui.startPins.filter((id) => byId(id)) : state.apps.map((a) => a.id);
  if (!Array.isArray(ui.startPins)) needSeed = true;
  state.recent = Array.isArray(ui.recent) ? ui.recent : [];
  state.recoCollapsed = !!ui.recoCollapsed;
  if (ui.iconSize === 'sm' || ui.iconSize === 'md' || ui.iconSize === 'lg') state.iconSize = ui.iconSize;
  state.autoArrange = !!ui.autoArrange;
  applyIconSize();                               // size the grid before the first desktop render
  // Desktop shortcuts: use the saved arrangement, or seed one-per-app on a fresh device.
  state.desktop = Array.isArray(ui.desktop) ? ui.desktop : seedDesktop();
  if (!Array.isArray(ui.desktop)) needSeed = true;
  if (addMissingAppIcons()) needSeed = true;     // surface newly installed apps on the desktop
  if (await reconcileDesktopFolder()) needSeed = true;   // mirror the real /data/Desktop folder onto the desktop
  // Window changes (open/close/move/resize/min/max/snap/focus) update the taskbar AND get
  // remembered by the device, so the session is restored on the next load. Each change also
  // folds the current geometry into winGeom (synchronously, so it's captured even right
  // before a close) — that map is what gives apps a persistent window size across closes.
  WM.setGeomProvider((id) => { const g = winGeom[id]; return g ? { ...g, min: false } : null; });
  WM.setOnChange(() => {
    renderTaskbar();
    for (const w of WM.serialize()) winGeom[w.id] = { x: w.x, y: w.y, w: w.w, h: w.h, max: w.max, snap: w.snap };
    if (installScrim) checkInstallGone();        // installer window closed/crashed → never leave the OS blocked
    saveSession();
  });
  renderDesktop();
  renderStartMenu();
  renderTaskbar();
  wireChrome();
  wireMessages();
  wireFileDrop();                          // Drag & Drop file dal PC → SD Cardputer
  initOS();                                // OS-wide clipboard + keyboard shortcuts
  applyWallpaper(state.wallpaper);
  try { applyBrightness(localStorage.getItem('nucleo.brightness') || 100); } catch {}
  try { applyEco(localStorage.getItem('nucleo.eco') === '1'); } catch {}
  if (needSeed) saveUiState();             // device had nothing yet → persist current state onto it
  applySettingsFromDevice();               // theme + device name are authoritative on the device too
  ensureOnboarding().catch(() => {});      // first paired boot with no AI key → curated welcome/setup (overlays the desktop)
  await restoreSession();                  // bring back the windows that were open last time
  connectWS();
  // Warm the search index: instant from localStorage (if any), then revalidated against the
  // device. Repaint search results live whenever the index finishes (re)building.
  FsIndex.onUpdate(() => { if (searchActive()) refreshSearchView(); });
  FsIndex.init();
  tickClock(); setInterval(tickClock, 10000);
  refreshStatus(); setInterval(refreshStatus, 15000);
  } catch (e) {
    console.error('[boot] init error — revealing UI anyway:', e);
  } finally {
    clearTimeout(bootFailsafe);
    setTimeout(hideBootScreen, 600);
  }
}

// Re-pull the installed-app list (after an over-the-air install/uninstall) and repaint the
// launcher surfaces that list apps. Desktop shortcuts are user-arranged, so we leave them be;
// the new app shows up in the Start menu and search right away.
async function refreshApps() {
  try {
    const d = await fetchJSON('/api/apps');
    if (!d || !Array.isArray(d.apps)) return;
    state.apps = d.apps.filter((a) => a.enabled).map((a) => ({ ...a, glyph: glyph(a) }));
    // Surface any newly installed app in the Start grid (Windows-11 pins new apps automatically).
    let added = false;
    for (const a of state.apps) if (!state.startPins.includes(a.id)) { state.startPins.push(a.id); added = true; }
    state.startPins = state.startPins.filter((id) => byId(id));   // drop uninstalled
    if (added) saveUiState();
    renderStartMenu();
    renderDesktop();
  } catch {}
}

// Live event stream: update the tray and forward every event to open app windows.
let wsState = 'offline';                       // remembered so the tooltip can be re-localized on language change
// Reconnect discipline for the PSRAM-less device: never open a 2nd socket (it serves ONE client),
// never stack reconnect timers, and back off exponentially instead of hammering /ws every 3s — a
// rejected handshake (unpaired = 401) or a flaky link otherwise spins endless wasted requests.
let wsSock = null, wsTimer = null, wsBackoff = 3000;
const WS_BACKOFF_MAX = 30000;
function scheduleWS() {
  if (wsTimer || document.hidden) return;      // one pending retry only; stay silent while backgrounded
  wsTimer = setTimeout(() => { wsTimer = null; connectWS(); }, wsBackoff);
  wsBackoff = Math.min(wsBackoff * 2, WS_BACKOFF_MAX);
}
function setWsBadge(state) {
  wsState = state;
  const el = document.getElementById('tray-ws');
  if (!el) return;
  const dot = el.querySelector('.ws-dot');
  if (!dot) return;
  dot.className = `ws-dot ${state}`;
  el.title = t('ws_' + state) || state;
}

function connectWS() {
  // Reuse a socket that's already open/connecting — a stray parallel handshake just churns the
  // device's tiny heap. Defer entirely while the tab is hidden; the visibility hook revives us.
  if (wsSock && (wsSock.readyState === WebSocket.OPEN || wsSock.readyState === WebSocket.CONNECTING)) return;
  if (document.hidden) return;
  let ws;
  setWsBadge('reconnecting');
  try { ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws'); }
  catch { setWsBadge('offline'); scheduleWS(); return; }
  wsSock = ws;
  ws.onopen = () => {
    wsBackoff = 3000;                          // healthy link → reset backoff for the next drop
    setWsBadge('connected');
    ws.send(JSON.stringify({ op: 'subscribe', since: 0 }));
  };
  ws.onmessage = (m) => {
    let msg; try { msg = JSON.parse(m.data); } catch { return; }
    const events = msg.op === 'sync' ? (msg.events || []) : [msg];
    for (const ev of events) {
      if (!ev || !ev.t) continue;
      if (ev.t === 'fs.changed' || ev.t.startsWith('storage')) refreshStatus();
      // Rebuild the search index only for changes that actually affect it. The index crawls ONLY
      // /data, yet the shell's own frequent writes to /system/config (session, clipboard, ui-state,
      // settings) and the Game Center lobby churn under /data/play each fired a FULL /data re-crawl
      // (up to ~32 /api/fs/list) for nothing. Skip those; an unknown change (no path) still rebuilds.
      if (ev.t === 'fs.changed') {
        const p = ev.d && typeof ev.d.path === 'string' ? ev.d.path : null;
        if (!p || (p.startsWith('/data') && !p.startsWith('/data/play'))) FsIndex.invalidate();
      }
      // Shared UI state changed on the device (this or another client) → re-sync.
      if (ev.t === 'fs.changed' && ev.d && typeof ev.d.path === 'string' && ev.d.path.endsWith('ui-state.json')) syncUiState();
      // The real Desktop folder changed (a shortcut/file added or removed here or by another client)
      // → re-mirror it onto the desktop. Debounced inside via the fs.changed coalescing upstream.
      if (ev.t === 'fs.changed' && ev.d && typeof ev.d.path === 'string' && (ev.d.path === DESKTOP_DIR || ev.d.path.startsWith(DESKTOP_DIR + '/'))) {
        reconcileDesktopFolder().then((ch) => { if (ch) { saveUiState(); renderDesktop(); } });
      }
      if (ev.t === 'fs.changed' && ev.d && typeof ev.d.path === 'string' && ev.d.path.endsWith('settings.json')) applySettingsFromDevice();
      // Notifications: the unified backbone topic (notify.post) plus the legacy calendar.reminder
      // both flow into the system Notification Center (toast + history + chime). See notify.js.
      if ((ev.t === 'notify.post' || ev.t === 'calendar.reminder') && ev.d) {
        if (Notify) Notify.fromBus(ev.t, ev.d);
        else if (ev.t === 'calendar.reminder') {     // module not loaded yet → graceful fallback
          showToast((ev.d.text || 'Promemoria') + (ev.d.time ? ` (${ev.d.time})` : ''), '🔔', 'info', 9000);
        }
      }
      // An app was installed/removed over the air (registry reloaded on the device) → refresh
      // the launcher live so the new app appears without a reboot or page reload. Also flush the
      // service worker's version-keyed app-asset cache so an UPDATED app serves its new files
      // immediately (the cache is otherwise authoritative until the next sw.js version bump).
      if (ev.t === 'apps.changed') {
        refreshApps();
        try { navigator.serviceWorker?.controller?.postMessage({ type: 'flush-app-cache' }); } catch {}
      }
      // The device handed its screen over to web clients (or reclaimed it) — reflect it.
      if (ev.t === 'system.remote' && ev.d) renderRemote(!!ev.d.active, ev.d.clients);
      // The heavy-work arbiter took/released its single token (one TLS/SD/heavy job at a time on the
      // PSRAM-less device) — show a subtle, debounced "busy" tray indicator. NOT a blocking overlay:
      // the arbiter already degrades gracefully (503/offline), this only tells the user the device is
      // prioritising a heavy task so a brief delay reads as intentional, not a bug.
      if (ev.t === 'system.busy' && ev.d) busyCtl.onEvent(!!ev.d.busy, ev.d.job);
      for (const w of WM.list()) { const f = w.el.querySelector('iframe'); if (f) try { f.contentWindow.postMessage(ev, '*'); } catch {} }
    }
  };
  ws.onclose = () => {
    if (wsSock === ws) wsSock = null;
    setWsBadge('offline');
    renderRemote(false);
    scheduleWS();                  // backed-off auto-reconnect (no fixed 3s storm)
  };
}
// Returning to the foreground revives the link at once (and resets backoff so it feels instant),
// rather than waiting out a long backed-off timer that grew while the tab was hidden.
document.addEventListener('visibilitychange', () => {
  if (document.hidden) return;
  if (wsSock && (wsSock.readyState === WebSocket.OPEN || wsSock.readyState === WebSocket.CONNECTING)) return;
  if (wsTimer) { clearTimeout(wsTimer); wsTimer = null; }
  wsBackoff = 3000;
  connectWS();
});

// ===== OS-wide install block =====================================================================
// When the ANIMA model installer runs a blocking download it asks the shell (via postMessage) to make the
// whole OS "wait or cancel": a full-desktop scrim covers the taskbar, Start and every other window, and the
// installer's OWN window is floated ABOVE the scrim so the user can only watch its progress or hit Cancel.
// Genuinely OS-wide. Torn down when the app posts state:'close' (always sent, even on cancel/pagehide), with
// a safety net (checkInstallGone) so a vanished installer window can never leave the desktop frozen.
let installScrim = null, installWinId = null, installPrevZ = '';
function findWinBySource(src) {
  for (const w of WM.list()) { const f = w.el.querySelector('iframe'); if (f && f.contentWindow === src) return w; }
  return null;
}
function openInstallBlock(srcWin, label) {
  if (installScrim) return;                                   // already blocking
  const win = findWinBySource(srcWin);
  const scrim = document.createElement('div');
  scrim.id = 'os-install-scrim';
  scrim.style.cssText = 'position:fixed;inset:0;z-index:100050;background:rgba(4,8,18,.55);backdrop-filter:blur(3px);'
    + 'display:flex;align-items:flex-end;justify-content:center;padding-bottom:92px;';
  const hint = document.createElement('div');
  hint.style.cssText = 'font:600 13px system-ui;color:#cfe0ff;background:#0e1830cc;border:1px solid #ffffff22;'
    + 'padding:9px 16px;border-radius:999px;box-shadow:0 12px 40px #0008;';
  hint.textContent = t('toast_model_install', { win: label || t('app') });
  scrim.appendChild(hint);
  // A click on the scrim does nothing but pulse the installer window to draw the eye to it.
  scrim.addEventListener('mousedown', (e) => { e.preventDefault(); if (win && win.el.animate) win.el.animate([{ transform: 'scale(1)' }, { transform: 'scale(1.012)' }, { transform: 'scale(1)' }], { duration: 190 }); });
  document.body.appendChild(scrim);
  installScrim = scrim;
  // Float the installer window above the scrim AND pin it there, so a click to reach its Cancel button
  // can't trigger focus() and demote the window under the scrim mid-gesture (which would swallow the click).
  if (win) { installWinId = win.app.id; installPrevZ = win.el.style.zIndex; win.el.style.zIndex = '100051'; if (WM.setPinnedTop) WM.setPinnedTop(win.app.id, '100051'); }
  cancelTaskSwitcher();
}
function closeInstallBlock() {
  if (!installScrim) return;
  try { installScrim.remove(); } catch {}
  installScrim = null;
  if (WM.setPinnedTop) WM.setPinnedTop(null, '');
  if (installWinId) { const w = WM.list().find((x) => x.app.id === installWinId); if (w) w.el.style.zIndex = installPrevZ || ''; }
  installWinId = null; installPrevZ = '';
}
// Safety net: if the installer window is gone (closed/crashed) and never sent 'close', unblock the OS.
function checkInstallGone() { if (installWinId && !WM.list().some((x) => x.app.id === installWinId)) closeInstallBlock(); }

// Apps (e.g. File Commander) ask the shell to open a file with its associated app.
function wireMessages() {
  window.addEventListener('message', (e) => {
    const d = e.data;
    if (!d) return;
    if (d.type === 'set-theme') {
      if (d.theme) currentThemeState.theme = d.theme;
      if (d.accent) currentThemeState.accent = d.accent;
      if (d.fontSize) currentThemeState.fontSize = d.fontSize;
      applyGlobalUI();
      return; 
    }   // live preview from Settings
    if (d.type === 'set-wallpaper' && d.path) {
      state.wallpaper = d.path; applyWallpaper(d.path); saveUiState(); return;
    }
    // Live system-language change from Settings: mirror into the runtime key the copilot/ANIMA read.
    if (d.type === 'set-language' && (d.lang === 'it' || d.lang === 'en')) {
      localStorage.setItem('anima.lang', d.lang); document.documentElement.lang = d.lang; return;
    }
    // OS clipboard service: apps copy/cut into it, or request the latest entry.
    if (d.type === 'clipboard-write' && d.kind) { clipboardWrite(d.kind, d.data); return; }
    if (d.type === 'clipboard-read') {
      if (e.source) try { e.source.postMessage({ type: 'clipboard-data', item: clipboardLatest() }, '*'); } catch {}
      return;
    }
    // ANIMA (and other apps) can ask the shell to launch an app by id.
    if (d.type === 'open-app' && d.id) { const a = byId(d.id); if (a) WM.open(a, d.query || ''); return; }

    // The ANIMA model installer is running a blocking download → make the OS "wait or cancel".
    if (d.type === 'os-install-modal') {
      if (d.state === 'open') openInstallBlock(e.source, d.label);
      else if (d.state === 'close') closeInstallBlock();
      return;
    }
    
    // OS File Dialog: used by apps like Paint to pick/save files centrally
    if (d.type === 'os-file-dialog') {
      openOsFileDialog(e.source, d);
      return;
    }

    // An app asking to close its own window (e.g. Paint "Esci") or update its title bar. Map the
    // posting iframe back to its window via contentWindow identity.
    if (d.type === 'close-window' || d.type === 'set-window-title') {
      for (const w of WM.list()) {
        const f = w.el.querySelector('iframe');
        if (f && f.contentWindow === e.source) {
          if (d.type === 'close-window') WM.close(w.app.id);
          else { const t = w.el.querySelector('.bar .t'); if (t && d.title) t.textContent = d.title; }
          break;
        }
      }
      return;
    }

    // File Commander announces a drag in flight so the desktop can be a drop target for it. The
    // timestamp lets the desktop ignore a STALE session if fc-drag-end is ever lost (e.g. the File
    // Commander window closes mid-drag), so a later real file-from-PC drop is never misread.
    if (d.type === 'fc-drag-start') { fcDrag = Array.isArray(d.items) ? { items: d.items, ts: Date.now() } : null; return; }
    if (d.type === 'fc-drag-end') { fcDrag = null; return; }

    if (d.type !== 'open-file' || !d.path) return;
    openFile(d.path);
  });
}

// --- Global Theme Engine ---
let currentThemeState = { theme: 'dark', accent: '#4ea1ff', fontSize: '14px' };

function applyGlobalUI() {
  document.documentElement.dataset.theme = currentThemeState.theme === 'light' ? 'light' : 'dark';
  document.documentElement.style.setProperty('--accent', currentThemeState.accent);
  document.documentElement.style.setProperty('--font-base', currentThemeState.fontSize);
  document.documentElement.style.fontSize = currentThemeState.fontSize;
  document.body.style.fontSize = currentThemeState.fontSize;
  
  // Applica anche al documento principale (Shell)
  injectGlobalTheme(document);

  // Inject into all currently open iframes
  for (const w of WM.list()) {
    const f = w.el.querySelector('iframe');
    if (f) injectGlobalTheme(f.contentDocument);
  }
}

function injectGlobalTheme(doc) {
  if (!doc) return;
  try {
    let style = doc.getElementById('os-global-theme');
    if (!style) {
      style = doc.createElement('style');
      style.id = 'os-global-theme';
      doc.head.appendChild(style);
    }
    const isLight = currentThemeState.theme === 'light';
    const acc = currentThemeState.accent;
    const fs = currentThemeState.fontSize;
    // Forziamo i valori di base in modo che sovrascrivano i :root delle app usando !important
    style.textContent = `
      :root, html {
        --accent: ${acc} !important;
        font-size: ${fs} !important;
      }
      ${isLight ? `
      :root, html {
        --bg:#eef2fb !important; --panel:#ffffff !important; --panel2:#e6ecf8 !important;
        --text:#10203a !important; --muted:#5a6a86 !important;
        --line:#cdd8ee !important; --field:#f4f7fe !important;
      }
      ` : `
      :root, html {
        --bg:#0e1830 !important; --panel:#18233a !important; --panel2:#1f2d4a !important;
        --text:#e8eefc !important; --muted:#8aa0c8 !important;
        --line:#2a3a5e !important; --field:#0b1220 !important;
      }
      `}
    `;
    // Molte app usano 'font: 14px system-ui' sul body. Sovrascriviamolo se necessario
    if (!doc.getElementById('os-global-body')) {
      const bstyle = doc.createElement('style');
      bstyle.id = 'os-global-body';
      bstyle.textContent = `body { font-size: ${fs} !important; }`;
      doc.head.appendChild(bstyle);
    } else {
      doc.getElementById('os-global-body').textContent = `body { font-size: ${fs} !important; }`;
    }
  } catch (err) {} // cross-origin safe
}

// Mantieni retrocompatibilità
function applyTheme(theme) {
  currentThemeState.theme = theme === 'light' ? 'light' : 'dark';
  applyGlobalUI();
}

function applyDeviceName(name) {
  const el = document.getElementById('sm-device');
  if (el) el.textContent = name || 'NucleoOS';
}
// Device-owned settings (theme, name) read straight from the canonical SD file.
async function applySettingsFromDevice() {
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent('/system/config/settings.json'), { cache: 'no-store' });
    if (!r.ok) return;
    const s = JSON.parse(await r.text());
    if (s.ui) {
      if (s.ui.theme) currentThemeState.theme = s.ui.theme;
      if (s.ui.accent) currentThemeState.accent = s.ui.accent;
      if (s.ui.fontSize) currentThemeState.fontSize = s.ui.fontSize;
      applyGlobalUI();
      const btn = document.getElementById('ac-theme');
      if (btn) btn.classList.toggle('active', currentThemeState.theme === 'dark');
      // System language + regional format are canonical in settings.json. Mirror them into the
      // localStorage keys the i18n engine reads at runtime (anima.lang = display language, driving
      // every surface's translation; nucleo.locale = optional Intl format override) so the stored
      // OS settings drive the whole OS on every client.
      if (s.ui.language === 'it' || s.ui.language === 'en') {
        if (window.NucleoI18N) window.NucleoI18N.setLang(s.ui.language);
        else { localStorage.setItem('anima.lang', s.ui.language); document.documentElement.lang = s.ui.language; }
      }
      if (typeof s.ui.regionLocale === 'string' && window.NucleoI18N) window.NucleoI18N.setLocale(s.ui.regionLocale);
    }
    if (s.device && s.device.name) applyDeviceName(s.device.name);
  } catch {}
}

async function persistTheme(newTheme) {
  try {
    const path = '/system/config/settings.json';
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(path), { cache: 'no-store' });
    let s = {};
    if (r.ok) { const txt = await r.text(); if (txt) s = JSON.parse(txt); }
    if (!s.ui) s.ui = {};
    s.ui.theme = newTheme;
    await fetch('/api/fs/write?path=' + encodeURIComponent(path), { method: 'POST', body: JSON.stringify(s, null, 2) });
    showToast(t('toast_theme_saved'), newTheme === 'dark' ? '🌙' : '☀️', 'success');
  } catch (err) {
    console.error('Failed to persist theme', err);
    showToast(t('toast_theme_error'), '⚠️', 'error');
  }
}

// ===== Utility: HTML escaping (must be before showToast which uses it) =====
const escapeHtml = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ===== Toast Notifications =====
function showToast(msg, icon = 'ℹ️', type = 'info', duration = 3500) {
  const container = document.getElementById('toast-container');
  if (!container) return;
  const t = document.createElement('div');
  t.className = `toast ${type}`;
  t.innerHTML = `<div class="toast-icon">${icon}</div><div class="toast-body"><div class="toast-title">${escapeHtml(msg)}</div></div>`;
  t.addEventListener('click', () => removeToast(t));
  container.appendChild(t);
  const timer = setTimeout(() => removeToast(t), duration);
  t._timer = timer;
}
function removeToast(t) {
  clearTimeout(t._timer);
  t.classList.add('removing');
  setTimeout(() => t.remove(), 220);
}

// ===== Drag & Drop file dal PC → SD del Cardputer =====
// I container video (mp4/mkv/…) NON vanno caricati grezzi sul device: il Cardputer non ha
// ffmpeg e non riproduce un .mkv — vanno convertiti in .nfv dal companion Video Studio (sul
// PC). Per giunta caricare un film intero via Wi-Fi con un solo POST satura l'ESP32 e fa
// ERR_CONNECTION_RESET. Quindi i video aprono il Video Studio; tutto il resto (compresi i
// .nfv/.mp3 già convertiti) si carica sulla SD, in streaming (niente file intero in RAM).
const VIDEO_SRC = new Set(['mp4', 'mkv', 'mov', 'avi', 'webm', 'm4v', 'wmv', 'flv', 'mpg', 'mpeg', 'ts', 'm2ts', 'mts', '3gp', 'ogv']);
const fileExt = (n) => (n.split('.').pop() || '').toLowerCase();

function wireFileDrop() {
  const overlay = document.getElementById('drop-overlay');
  const destLabel = overlay ? overlay.querySelector('.drop-dest') : null;
  let dragDepth = 0;

  // Solo i drop sullo SFONDO del desktop diventano upload. Sopra una finestra app (es. il
  // Video Studio) NON intercettiamo: l'iframe gestisce il proprio drop. L'overlay è anche
  // pointer-events:none (CSS), così non copre mai le finestre.
  const overWindow = (e) => !!(e.target && e.target.closest && e.target.closest('.win'));

  // Previeni il comportamento default del browser (aprirebbe il file) — ma non sulle finestre.
  // Quando un drag di File Commander è in volo, il desktop è una zona di rilascio: mostriamo il
  // cursore giusto (copia/sposta/collega) in base al modificatore tenuto premuto.
  const fcLive = () => fcDrag && (Date.now() - fcDrag.ts < 30000);   // honour a File Commander drag only while fresh
  document.addEventListener('dragover', (e) => {
    if (overWindow(e)) return;
    e.preventDefault();
    if (fcLive()) { try { e.dataTransfer.dropEffect = e.ctrlKey ? 'copy' : e.shiftKey ? 'move' : 'link'; } catch {} }
  });

  document.addEventListener('dragenter', (e) => {
    if (!e.dataTransfer || !e.dataTransfer.types.includes('Files')) return;
    fcDrag = null;                          // a real file-from-PC drag supersedes any stale FC session
    if (overWindow(e)) { if (overlay) overlay.classList.add('hidden'); return; }
    dragDepth++;
    if (dragDepth === 1 && overlay) overlay.classList.remove('hidden');
  });

  document.addEventListener('dragleave', (e) => {
    dragDepth = Math.max(0, dragDepth - 1);
    if (dragDepth === 0 && overlay) overlay.classList.add('hidden');
  });

  document.addEventListener('drop', async (e) => {
    if (overWindow(e)) return;              // drop sopra una finestra app → lo gestisce lei, non noi
    e.preventDefault();
    dragDepth = 0;
    if (overlay) overlay.classList.add('hidden');
    // A File Commander file dragged onto the desktop → copy/move/shortcut into /data/Desktop,
    // at the drop point. The payload came over postMessage (fcDrag); the modifier + position here.
    if (fcLive()) { const items = fcDrag.items; fcDrag = null; performFcDrop(dropOp(e), items, { x: e.clientX, y: e.clientY }); return; }
    fcDrag = null;                          // drop wasn't an FC drag → drop any stale session
    const files = e.dataTransfer ? Array.from(e.dataTransfer.files) : [];
    if (!files.length) return;

    // Container video → vanno convertiti: apri il Video Studio, NON caricarli grezzi.
    const videos = files.filter((f) => VIDEO_SRC.has(fileExt(f.name)));
    const others = files.filter((f) => !VIDEO_SRC.has(fileExt(f.name)));
    if (videos.length) {
      const vs = byId('video-studio');
      if (vs) WM.open(vs);
      const names = videos.map((f) => f.name).join(', ');
      showToast(t('toast_video_studio', { count: videos.length, names }), '🎬', 'info', 9000);
    }

    // Tutto il resto va sulla SD. I .nfv/.mp3 già convertiti vanno in /data/Videos (pronti da
    // riprodurre); il resto in /data/uploads. Passiamo il File direttamente a fetch così il
    // browser fa streaming e non bufferizza l'intero file in RAM.
    for (const file of others) {
      const ext = fileExt(file.name);
      const dir = (ext === 'nfv' || ext === 'mp3') ? '/data/Videos' : '/data/uploads';
      const targetPath = `${dir}/${file.name}`;
      if (destLabel) destLabel.textContent = targetPath;
      showToast(t('toast_uploading', { name: file.name }), '📤', 'info', 60000);
      try {
        const resp = await fetchWithRetry('/api/fs/write?path=' + encodeURIComponent(targetPath), {
          method: 'POST',
          headers: { 'Content-Type': 'application/octet-stream' },
          body: file,                          // streamed dal browser, niente arrayBuffer() in RAM
        });
        // Rimuovi il toast di caricamento (era a durata lunga)
        const tc = document.getElementById('toast-container');
        if (tc) { const last = tc.lastElementChild; if (last) removeToast(last); }
        if (resp.ok) {
          showToast(t('toast_upload_ok', { name: file.name }), '✅', 'success');
        } else {
          showToast(t('toast_upload_err', { name: file.name }), '❌', 'error');
        }
      } catch (err) {
        const tc = document.getElementById('toast-container');
        if (tc) { const last = tc.lastElementChild; if (last) removeToast(last); }
        showToast(t('toast_net_err', { name: file.name }), '❌', 'error');
      }
    }
  });
}

function applyWallpaper(path) {
  try { localStorage.setItem('nucleo.wallpaper.cache', path); } catch {}
  const d = document.getElementById('desktop');
  d.style.backgroundImage = `url("/api/fs/read?path=${encodeURIComponent(path)}")`;
  d.style.backgroundSize = 'cover';
  d.style.backgroundPosition = 'center';
}

// ===== Display controls — real, persisted Action Center (were dead placeholders) =====
// The Cardputer's TFT backlight is firmware-owned, so the brightness slider here dims the
// OPERATOR CONSOLE view itself — a genuine per-browser display setting (easy on the eyes at
// night, zero firmware round-trip). Eco trims the cosmetic animations to save the client GPU.
let dimEl = null;
function applyBrightness(v) {
  v = Math.max(20, Math.min(100, Number(v) || 100));         // never fully black the console out
  if (!dimEl) { dimEl = document.createElement('div'); dimEl.id = 'screen-dim'; document.body.appendChild(dimEl); }
  dimEl.style.opacity = String((100 - v) / 100 * 0.7);       // 100% → no dim, 20% → ~0.56 dim
  try { localStorage.setItem('nucleo.brightness', String(v)); } catch {}
}
function applyEco(on) {
  document.documentElement.classList.toggle('eco', !!on);
  try { localStorage.setItem('nucleo.eco', on ? '1' : '0'); } catch {}
}

// Resolve a desktop item to its display glyph + label (apps may be renamed/removed in the registry).
function itemGlyph(item) {
  if (item.type === 'app') { const a = byId(item.target); return a ? a.glyph : '❓'; }
  if (item.type === 'url') return '🔗';
  if (/\.lnk$/i.test(item.target)) return '🔗';     // a real .lnk shortcut file
  // file: borrow the associated app's glyph, else a generic document.
  const ext = (item.target.split('.').pop() || '').toLowerCase();
  const a = byId(state.assoc.default_open[ext]);
  return a ? a.glyph : '📄';
}
function itemLabel(item) {
  if (item.label) return item.label;
  if (item.type === 'app') { const a = byId(item.target); return a ? a.name : item.target; }
  // a .lnk shows its name WITHOUT the extension (Windows shows the shortcut's display name).
  if (item.type === 'file') return item.target.split('/').pop().replace(/\.lnk$/i, '');
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
// Remember a freshly-opened file so it surfaces under Start → "Consigliati" (newest first,
// deduped, bounded). Persisted with the rest of the UI state so it follows the user across clients.
function pushRecent(path) {
  if (!path || typeof path !== 'string') return;
  state.recent = [{ path, ts: Date.now() }, ...state.recent.filter((r) => r.path !== path)].slice(0, RECENT_CAP);
  saveUiState();
  renderRecommended();
}

// Open an SD file with its associated app (shared by File Commander and file shortcuts).
function openFile(path) {
  if (/\.lnk$/i.test(path)) { openLnk(path); return; }   // a shortcut → resolve + open its target
  pushRecent(path);
  const ext = (path.split('.').pop() || '').toLowerCase();
  const appId = state.assoc.default_open[ext];
  if (appId) {
    const app = byId(appId);
    if (app) {
      WM.open(app, 'path=' + encodeURIComponent(path));
      return;
    } else {
      console.warn(`App associata "${appId}" per estensione .${ext} non trovata. Uso il fallback.`);
    }
  }
  const fallbackApp = byId(state.assoc.fallback);
  if (fallbackApp) {
    WM.open(fallbackApp, 'path=' + encodeURIComponent(path));
  } else {
    showToast(t('toast_no_app'), '⚠️', 'error');
  }
}

// ===== Shortcut (.lnk) files — real Windows-style links that live on the SD =======================
// A NucleoOS shortcut is a small JSON file with a .lnk extension:
//   { schema:1, type:'app'|'file'|'url', target:<app-id | /sd/path | url>, label?, createdAt }
// The shell intercepts opening a .lnk and resolves it. The desktop MIRRORS the real /data/Desktop
// folder, so every file or .lnk placed there shows up as an icon — the Windows model where the
// Desktop is a real folder. Shortcuts created via drag/menus are written there as real files.
const DESKTOP_DIR = '/data/Desktop';
const PROTECTED_TARGET = /^\/(system|www|apps)(\/|$)|^\/data\/anima(\/|$)|(^|\/)\.\.(\/|$)/i;   // never link/copy/move these
const safeLnkBase = (s) => (String(s || 'Collegamento').replace(/\.lnk$/i, '').replace(/[\\/:*?"<>|]+/g, ' ').replace(/\s+/g, ' ').trim() || 'Collegamento');
function defaultLnkLabel(p) {
  if (p.type === 'app') { const a = byId(p.target); return a ? a.name : p.target; }
  if (p.type === 'url') { try { return new URL(p.target).hostname || p.target; } catch { return p.target; } }
  return (p.target.split('/').pop() || p.target);
}
const serializeLnk = (p) => JSON.stringify({ schema: 1, type: p.type, target: p.target, label: p.label || undefined, createdAt: Date.now() });
async function readLnk(path) {
  try {
    const r = await fetch('/api/fs/read?path=' + encodeURIComponent(path), { cache: 'no-store' });
    if (!r.ok) return null;
    const j = JSON.parse(await r.text());
    if (j && (j.type === 'app' || j.type === 'file' || j.type === 'url') && typeof j.target === 'string') return j;
  } catch {}
  return null;
}
// Resolve + open a .lnk. `depth` guards against a shortcut-to-shortcut chain or self-reference loop.
async function openLnk(path, depth = 0) {
  if (depth > 4) { showToast(t('lnk_broken'), '⚠️', 'error'); return; }
  const l = await readLnk(path);
  if (!l) { showToast(t('lnk_broken'), '⚠️', 'error'); return; }
  if (l.type === 'app') { const a = byId(l.target); a ? WM.open(a) : showToast(t('lnk_broken'), '⚠️', 'error'); return; }
  if (l.type === 'url') {
    if (/^https?:\/\//i.test(l.target)) window.open(l.target, '_blank', 'noopener');
    else WM.open({ id: 'link:' + l.target, name: l.label || l.target, route: l.target, glyph: '🔗' });
    return;
  }
  if (/\.lnk$/i.test(l.target)) return openLnk(l.target, depth + 1);   // shortcut chain
  openFile(l.target);
}
// Mirror the real /data/Desktop folder onto the desktop: every file there becomes an icon, and an
// icon whose backing file vanished is pruned. Returns true if state.desktop changed. Safe when the
// folder doesn't exist yet (list returns empty).
async function reconcileDesktopFolder() {
  let entries = [];
  try {
    // Ensure the folder exists first, so the list is an empty 200 instead of a 404 the browser logs
    // as a red console error on every fresh-SD boot. mkdir is idempotent and the firmware only emits
    // fs.changed when it actually creates the dir (so this can't loop); on an unpaired session it just
    // 401s harmlessly and we fall through. Mirrors createDesktopShortcut's mkdir-then-use.
    await fetch('/api/fs/mkdir?path=' + encodeURIComponent(DESKTOP_DIR), { method: 'POST' }).catch(() => {});
    const r = await fetch('/api/fs/list?path=' + encodeURIComponent(DESKTOP_DIR), { cache: 'no-store' });
    if (!r.ok) return false;
    entries = (await r.json()).entries || [];
  } catch { return false; }
  const want = new Set(entries.filter((e) => e.type === 'file').map((e) => DESKTOP_DIR + '/' + e.name));
  const isFolderIcon = (it) => it.type === 'file' && typeof it.target === 'string' && it.target.startsWith(DESKTOP_DIR + '/');
  const before = state.desktop.length;
  state.desktop = state.desktop.filter((it) => !(isFolderIcon(it) && !want.has(it.target)));   // prune vanished
  let changed = state.desktop.length !== before;
  const have = new Set(state.desktop.filter((it) => it.type === 'file').map((it) => it.target));
  const taken = occupiedCells();
  for (const p of want) {
    if (have.has(p)) continue;
    const cell = firstFreeCell(taken); taken.add(cell.x + ',' + cell.y);
    state.desktop.push({ uid: uniqueUid('desk-' + p), type: 'file', target: p, x: cell.x, y: cell.y });
    changed = true;
  }
  return changed;
}
// Write a shortcut into /data/Desktop and surface it as an icon at `pos` (a {x,y} or null). The
// device's fs.changed echo also reconciles, but we add it immediately so the icon appears at once.
async function createDesktopShortcut(payload, pos) {
  if (payload.type === 'file' && PROTECTED_TARGET.test(payload.target)) { showToast(t('lnk_protected'), '⚠️', 'error'); return null; }
  try { await fetch('/api/fs/mkdir?path=' + encodeURIComponent(DESKTOP_DIR), { method: 'POST' }); } catch {}
  const base = safeLnkBase(payload.label || defaultLnkLabel(payload));
  const existing = new Set();
  try { const r = await fetch('/api/fs/list?path=' + encodeURIComponent(DESKTOP_DIR), { cache: 'no-store' }); if (r.ok) (await r.json()).entries.forEach((e) => existing.add(e.name)); } catch {}
  let name = base + '.lnk';
  for (let i = 2; existing.has(name); i++) name = base + ' (' + i + ').lnk';
  const path = DESKTOP_DIR + '/' + name;
  try { await fetch('/api/fs/write?path=' + encodeURIComponent(path), { method: 'POST', body: serializeLnk(payload) }); }
  catch { showToast(t('lnk_create_fail'), '⚠️', 'error'); return null; }
  const cell = pos ? nearestFreeCell(pos.x, pos.y, occupiedCells()) : firstFreeCell(occupiedCells());
  state.desktop.push({ uid: uniqueUid('desk-' + path), type: 'file', target: path, x: cell.x, y: cell.y });
  saveUiState(); renderDesktop();
  return path;
}
// A free destination path in `dir` for `name` (dedupes "file.txt" → "file (1).txt").
async function uniqueDestPath(dir, name) {
  const existing = new Set();
  try { const r = await fetch('/api/fs/list?path=' + encodeURIComponent(dir), { cache: 'no-store' }); if (r.ok) (await r.json()).entries.forEach((e) => existing.add(e.name)); } catch {}
  if (!existing.has(name)) return dir + '/' + name;
  const dot = name.lastIndexOf('.'), bse = dot > 0 ? name.slice(0, dot) : name, ext = dot > 0 ? name.slice(dot) : '';
  for (let i = 1; ; i++) { const c = `${bse} (${i})${ext}`; if (!existing.has(c)) return dir + '/' + c; }
}

// ---- cross-window drag: a drag from the File Commander iframe dropped on the desktop ----
// The browser sandbox stops the iframe's dataTransfer from crossing the frame boundary, so the
// File Commander announces its drag payload over postMessage (fcDrag); the shell's own drop handler
// supplies the modifier (op) and the drop position. op: 'copy' (Ctrl) / 'move' (Shift) / 'shortcut'
// (Alt or no modifier — the desktop is a launcher surface, so the safe default is a link).
let fcDrag = null;        // { items:[{path,name,isDir}] } while a File Commander drag is in flight
const dropOp = (e) => e.ctrlKey ? 'copy' : e.shiftKey ? 'move' : 'shortcut';   // Alt or none → shortcut
async function performFcDrop(op, items, pos) {
  await fetch('/api/fs/mkdir?path=' + encodeURIComponent(DESKTOP_DIR), { method: 'POST' }).catch(() => {});
  let cellSeed = pos, ok = 0;
  for (const it of (items || [])) {
    if (!it || !it.path) continue;
    if (PROTECTED_TARGET.test(it.path)) { showToast(t('lnk_protected'), '⚠️', 'error'); continue; }
    const cell = cellSeed ? nearestFreeCell(cellSeed.x, cellSeed.y, occupiedCells()) : firstFreeCell(occupiedCells());
    cellSeed = { x: cell.x + CELL, y: cell.y };                       // stagger multiple drops
    if (op === 'shortcut' || it.isDir) {                             // folders can only be linked, not byte-copied
      if (await createDesktopShortcut({ type: 'file', target: it.path, label: (it.name || '').replace(/\.lnk$/i, '') }, cell)) ok++;
      continue;
    }
    const dst = await uniqueDestPath(DESKTOP_DIR, it.name);
    try {
      if (op === 'move') {
        const r = await fetch('/api/fs/move?from=' + encodeURIComponent(it.path) + '&to=' + encodeURIComponent(dst), { method: 'POST' });
        if (!r.ok) throw 0;
      } else {                                                        // copy: read + write (no copy endpoint on the device)
        const buf = await (await fetch('/api/fs/read?path=' + encodeURIComponent(it.path), { cache: 'no-store' })).arrayBuffer();
        const w = await fetch('/api/fs/write?path=' + encodeURIComponent(dst), { method: 'POST', body: buf });
        if (!w.ok) throw 0;
      }
      state.desktop.push({ uid: uniqueUid('desk-' + dst), type: 'file', target: dst, x: cell.x, y: cell.y });
      ok++;
    } catch { showToast(t('drop_failed'), '⚠️', 'error'); }
  }
  if (ok) { saveUiState(); renderDesktop(); showToast(t(op === 'copy' ? 'drop_copied' : op === 'move' ? 'drop_moved' : 'drop_linked'), '✅', 'success'); }
}

// Desktop icons are freely positioned (Windows-style) and snap to a grid. Every icon
// gets a STABLE {x,y} the first time it is placed and that position is persisted to the
// device — so icons never silently reflow again (the old auto-flow recomputed layout from
// the live desktop height on every render, which made icons jump when the window resized,
// when another client with a different screen synced, or when a neighbour was dragged).
// Grid metrics scale with the user's icon size (View ▸ submenu). PAD (margin from the edges) is
// fixed; CELL/ICON_W/ICON_H are `let` so applyIconSize() can retune them — snap() and every helper
// read the live values. CSS vars on #desktop scale the actual icon boxes + glyph to match.
const ICON_SIZES = { sm: { cell: 84, w: 72, h: 76, glyph: 24 }, md: { cell: 98, w: 84, h: 92, glyph: 30 }, lg: { cell: 120, w: 104, h: 112, glyph: 42 } };
const PAD = 14;
let CELL = 98, ICON_W = 84, ICON_H = 92;
const snap = (v) => Math.max(0, Math.round((v - PAD) / CELL)) * CELL + PAD;
function applyIconSize() {
  const s = ICON_SIZES[state.iconSize] || ICON_SIZES.md;
  CELL = s.cell; ICON_W = s.w; ICON_H = s.h;
  const d = document.getElementById('desktop');
  if (d) { d.style.setProperty('--icon-w', s.w + 'px'); d.style.setProperty('--icon-h', s.h + 'px'); d.style.setProperty('--glyph-size', s.glyph + 'px'); }
}

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
  if (state.autoArrange && layoutGrid()) saveUiState();   // auto-arrange: re-flow into a clean grid
  for (const uid of [...selIcons]) if (!state.desktop.some((i) => i.uid === uid)) selIcons.delete(uid);  // drop stale
  const visualTaken = new Set();
  
  state.desktop.forEach((item, idx) => {
    let px = snap(item.x), py = snap(item.y);
    const { w, h } = desktopBox();
    if (w >= ICON_W && h >= ICON_H) {
      // Clamp visivo: maxPx/maxPy calcolati con Math.floor (non snap/round) per garantire
      // che l'icona entri SEMPRE nel viewport. snap() con round può eccedere w-ICON_W di
      // qualche pixel, il che faceva scattare subito il break del loop anti-overlap senza
      // che il de-overlap girasse, lasciando più icone nella stessa cella (overlap visivo).
      const maxPx = PAD + Math.max(0, Math.floor((w - PAD - ICON_W) / CELL)) * CELL;
      const maxPy = PAD + Math.max(0, Math.floor((h - PAD - ICON_H) / CELL)) * CELL;
      px = Math.min(snap(item.x), maxPx);
      py = Math.min(snap(item.y), maxPy);
      // Anti-overlap: avanza riga→colonna senza mai fermarsi su px > viewport.
      // Icone in eccesso (viewport troppo piccolo) vanno off-screen e spariscono per
      // overflow:hidden — mai sovrapposte. Alle prossime renderDesktop (window cresce)
      // le coordinate originali da state.desktop.{x,y} vengono ripristinate.
      let guard = 0;
      while (visualTaken.has(px + ',' + py) && ++guard < 2000) {
        py += CELL;
        if (py > maxPy) { py = PAD; px += CELL; }
      }
    }
    visualTaken.add(px + ',' + py);
    const pos = { x: px, y: py };
    
    const el = document.createElement('div');
    el.className = 'icon' + (selIcons.has(item.uid) ? ' sel' : ''); el.tabIndex = 0;
    el.dataset.uid = item.uid;
    el.style.left = pos.x + 'px'; el.style.top = pos.y + 'px';
    const label = itemLabel(item);
    el.title = label;                                  // full name on hover (long labels are clamped to 2 lines)
    el.setAttribute('role', 'button'); el.setAttribute('aria-label', label);
    el.innerHTML = `<div class="glyph">${itemGlyph(item)}</div><div class="label">${escapeHtml(label)}</div>`;
    el.addEventListener('click', (e) => iconClick(e, item, idx));
    el.addEventListener('dblclick', () => openItem(item));
    el.addEventListener('keydown', (e) => {
      // Handle these on the focused icon and stop them here, so the global osKeydown /
      // desktopNavKey don't ALSO act on the same key (e.g. opening the item twice on Enter).
      if (e.key === 'Enter') { e.stopPropagation(); openItem(item); }
      else if (e.key === 'Delete') { e.stopPropagation(); removeSelectedIcons(); }
      else if (e.key === 'F2') { e.preventDefault(); e.stopPropagation(); beginRename(item.uid); }
    });
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
// Remove desktop icons. A folder-backed icon (a file or .lnk living in /data/Desktop) IS the real
// thing on the SD, so removing it deletes that file: a .lnk (just a pointer) goes silently; a real
// FILE asks first, since deleting it loses data. App icons / legacy entries only drop their ui-state.
async function removeIcons(uids) {
  const items = uids.map((u) => state.desktop.find((x) => x.uid === u)).filter(Boolean);
  if (!items.length) return;
  const backed = (it) => it.type === 'file' && typeof it.target === 'string' && it.target.startsWith(DESKTOP_DIR + '/');
  const realFiles = items.filter((it) => backed(it) && !/\.lnk$/i.test(it.target));
  if (realFiles.length) {
    const name = realFiles.length === 1 ? realFiles[0].target.split('/').pop() : realFiles.length + '';
    if (!await osConfirm({ title: t('del_file_title'), body: t('del_file_body', { name }), okText: t('delete'), danger: true })) return;
  }
  const removable = new Set();
  for (const it of items) {
    if (backed(it)) {
      try { const r = await fetch('/api/fs/delete?path=' + encodeURIComponent(it.target), { method: 'POST' }); if (!r.ok) { if (r.status === 403) showToast(t('lnk_protected'), '⚠️', 'error'); continue; } } catch { continue; }
    }
    removable.add(it.uid);
  }
  state.desktop = state.desktop.filter((x) => !removable.has(x.uid));
  selIcons.clear(); saveUiState(); renderDesktop();
}
function removeSelectedIcons() { if (selIcons.size) removeIcons([...selIcons]); }

// Drag an icon to reposition it; on drop, snap to the grid and persist {x,y} to the device.
// If the icon is part of a multi-selection, the whole group moves together (Windows-style). A
// dashed "drop ghost" tracks the primary icon's target cell so the landing spot is always clear.
let draggedRecently = false;       // suppress the click that fires right after a drag
function dropGhost() {
  let g = document.getElementById('drop-ghost');
  if (!g) { g = document.createElement('div'); g.id = 'drop-ghost'; g.className = 'drop-ghost'; }
  return g;
}
function dragIcon(el, item) {
  let sx, sy, down = false, moving = false, group = [], ghost = null;
  el.addEventListener('pointerdown', (e) => {
    if (e.button !== 0 || state.autoArrange) return;   // auto-arrange locks icons to the grid
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
    if (!moving) { moving = true; ghost = dropGhost(); document.getElementById('desktop').appendChild(ghost); }
    const dx = e.clientX - sx, dy = e.clientY - sy;
    const { w, h } = desktopBox();
    for (const g of group) {
      g.el.classList.add('dragging');
      // Clamp the box fully inside the desktop on BOTH axes so it can't be dropped off-screen.
      g.el.style.left = Math.max(0, Math.min(g.ox + dx, w - ICON_W)) + 'px';
      g.el.style.top = Math.max(0, Math.min(g.oy + dy, h - ICON_H)) + 'px';
    }
    if (ghost && group[0]) {                              // preview the primary icon's landing cell
      const taken = occupiedCells(new Set(group.map((g) => g.uid)));
      const cell = nearestFreeCell(group[0].el.offsetLeft, group[0].el.offsetTop, taken);
      ghost.style.left = cell.x + 'px'; ghost.style.top = cell.y + 'px';
    }
  });
  const end = () => {
    if (!down) return;
    down = false;
    if (ghost) { ghost.remove(); ghost = null; }
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
function removeItem(uid) { removeIcons([uid]); }
// Inline rename (F2 / context menu): the label turns into an input in place — no jarring native
// prompt(). Enter or blur commits, Esc cancels; an empty value falls back to the item's default name.
function beginRename(uid) {
  const item = state.desktop.find((x) => x.uid === uid); if (!item) return;
  const el = document.querySelector(`#desktop .icon[data-uid="${CSS.escape(uid)}"]`); if (!el) return;
  const labelEl = el.querySelector('.label'); if (!labelEl || el.querySelector('.rename')) return;
  const input = document.createElement('input');
  input.className = 'rename'; input.value = itemLabel(item); input.spellcheck = false;
  labelEl.replaceWith(input);
  input.focus(); input.select();
  const backed = item.type === 'file' && typeof item.target === 'string' && item.target.startsWith(DESKTOP_DIR + '/');
  let done = false;
  const commit = async (save) => {
    if (done) return; done = true;
    const v = save ? input.value.trim() : '';
    // A folder-backed icon IS a real file: renaming it renames the file on the SD (keeping its
    // extension), so File Commander and the desktop stay in agreement. Others just relabel.
    if (save && v && backed) {
      const cur = item.target.split('/').pop();
      const ext = (cur.match(/\.[^.]+$/) || [''])[0];                 // preserve .lnk / .txt / …
      const base = safeLnkBase(v);
      if (base && base + ext !== cur) {
        const dst = await uniqueDestPath(DESKTOP_DIR, base + ext);
        try {
          const r = await fetch('/api/fs/move?from=' + encodeURIComponent(item.target) + '&to=' + encodeURIComponent(dst), { method: 'POST' });
          if (r.ok) { item.target = dst; item.uid = uniqueUid('desk-' + dst); item.label = undefined; saveUiState(); }
          else if (r.status === 403) showToast(t('lnk_protected'), '⚠️', 'error');
        } catch {}
      }
    } else if (save) { item.label = v || undefined; saveUiState(); }
    renderDesktop();
  };
  input.addEventListener('keydown', (e) => {                // swallow OS shortcuts while typing a name
    e.stopPropagation();
    if (e.key === 'Enter') { e.preventDefault(); commit(true); }
    else if (e.key === 'Escape') { e.preventDefault(); commit(false); }
  });
  input.addEventListener('blur', () => commit(true));
  input.addEventListener('pointerdown', (e) => e.stopPropagation());   // don't start a drag from the field
}

// Re-flow icons into a clean column-first grid in their current order (mutates x/y only; no save/
// render). Returns true if anything moved — callers decide when to persist + repaint.
function layoutGrid() {
  const taken = new Set(); let changed = false;
  for (const it of state.desktop) {
    const cell = firstFreeCell(taken);
    if (it.x !== cell.x || it.y !== cell.y) { it.x = cell.x; it.y = cell.y; changed = true; }
    taken.add(cell.x + ',' + cell.y);
  }
  return changed;
}
function arrangeIcons() { if (layoutGrid()) saveUiState(); renderDesktop(); }
// Sort the desktop order then re-flow into the grid (Windows "Sort by ▸"). 'date' keeps the array's
// natural insertion order (≈ when each shortcut was added) and just re-aligns it.
function sortDesktop(by) {
  const coll = new Intl.Collator(I18N.locale());
  const typeRank = { app: 0, file: 1, url: 2 };
  const arr = [...state.desktop];
  if (by === 'name') arr.sort((a, b) => coll.compare(itemLabel(a), itemLabel(b)));
  else if (by === 'type') arr.sort((a, b) => ((typeRank[a.type] ?? 9) - (typeRank[b.type] ?? 9)) || coll.compare(itemLabel(a), itemLabel(b)));
  state.desktop = arr;
  layoutGrid(); saveUiState(); renderDesktop();
}
// Change icon scale (View ▸). A different cell size needs a re-flow so nothing overlaps on the new grid.
function setIconSize(sz) {
  if (state.iconSize === sz) return;
  state.iconSize = sz; applyIconSize(); layoutGrid(); saveUiState(); renderDesktop();
}
function toggleAutoArrange() {
  state.autoArrange = !state.autoArrange;
  if (state.autoArrange) layoutGrid();
  saveUiState(); renderDesktop();
}
function openSelected() {
  for (const uid of selIcons) { const it = state.desktop.find((x) => x.uid === uid); if (it) openItem(it); }
}
// Read-only properties sheet for a desktop item.
function itemProperties(item) {
  const typeLbl = item.type === 'app' ? t('type_app') : item.type === 'file' ? t('type_file') : t('type_url');
  const rows = [[t('name'), itemLabel(item)], [t('type'), typeLbl], [t('prop_target'), item.target]];
  if (item.x != null) rows.push([t('prop_position'), `${snap(item.x)}, ${snap(item.y)}`]);
  if (item.type === 'file') {
    const ext = (item.target.split('.').pop() || '').toLowerCase();
    const a = byId(state.assoc.default_open[ext]); rows.push([t('prop_app'), a ? a.name : t('none')]);
  }
  osInfo({ title: itemLabel(item), glyph: itemGlyph(item), rows });
}
async function newFileShortcut() {
  const p = await osPrompt({ title: t('dlg_newfile_title'), glyph: '📄', label: t('prompt_new_file_path'), okText: t('dlg_create') });
  if (p && p.trim()) createDesktopShortcut({ type: 'file', target: p.trim() }, null);
}
async function newLinkShortcut() {
  const u = await osPrompt({ title: t('dlg_newlink_title'), glyph: '🔗', label: t('prompt_link_url'), okText: t('dlg_create') });
  if (u && u.trim()) createDesktopShortcut({ type: 'url', target: u.trim() }, null);
}
// "New ▸ Shortcut…" — one field that accepts an app id, an SD path, or a URL; the type is detected.
async function newShortcut() {
  const v = (await osPrompt({ title: t('dlg_shortcut_title'), glyph: '🔗', label: t('dlg_shortcut_label'), okText: t('dlg_create') }) || '').trim();
  if (!v) return;
  const type = /^https?:\/\//i.test(v) ? 'url' : v.startsWith('/') ? 'file' : byId(v) ? 'app' : 'url';
  createDesktopShortcut({ type, target: v }, null);
}
async function confirmReset() {
  if (await osConfirm({ title: t('reset_title'), body: t('reset_body'), okText: t('reset_ok'), danger: true })) {
    state.desktop = seedDesktop(); selIcons.clear(); saveUiState(); renderDesktop();
  }
}

// Context menu on an icon: Open / Rename / Properties / Remove (the app stays installed). On a
// multi-selection it collapses to bulk actions (open all / remove all).
function iconMenu(e, item) {
  if (selIcons.size > 1) {
    return showCtx(e.clientX, e.clientY, [
      { label: t('ctx_open_all', { n: selIcons.size }), glyph: '📂', fn: () => openSelected() },
      { sep: true },
      { label: t('ctx_remove_multi', { n: selIcons.size }), glyph: '🗑️', danger: true, fn: () => removeSelectedIcons() },
    ]);
  }
  const isApp = item.type === 'app';
  const isWebUrl = item.type === 'url' && /^https?:\/\//i.test(item.target);
  showCtx(e.clientX, e.clientY, [
    { label: t('open'), glyph: '↗', fn: () => openItem(item) },
    isWebUrl ? { label: t('ctx_open_new'), glyph: '🔗', fn: () => window.open(item.target, '_blank', 'noopener') } : null,
    isApp ? { label: state.pins.includes(item.target) ? t('ctx_unpin_taskbar') : t('ctx_pin_taskbar'), glyph: '📌', fn: () => togglePin(item.target) } : null,
    { label: t('ctx_rename'), glyph: '✏️', fn: () => beginRename(item.uid) },
    { label: t('ctx_properties'), glyph: 'ℹ️', fn: () => itemProperties(item) },
    { sep: true },
    { label: t('ctx_remove_desktop'), glyph: '🗑️', danger: true, fn: () => removeItem(item.uid) },
  ].filter(Boolean));
}

// Context menu on the empty desktop: View / Sort / Refresh / New / Reset, with nested submenus.
function desktopMenu(e) {
  const onDesktop = new Set(state.desktop.filter((x) => x.type === 'app').map((x) => x.target));
  const addable = state.apps.filter((a) => !onDesktop.has(a.id));
  showCtx(e.clientX, e.clientY, [
    { label: t('ctx_view'), glyph: '🖼', sub: [
      { label: t('ctx_view_sm'), check: state.iconSize === 'sm', fn: () => setIconSize('sm') },
      { label: t('ctx_view_md'), check: state.iconSize === 'md', fn: () => setIconSize('md') },
      { label: t('ctx_view_lg'), check: state.iconSize === 'lg', fn: () => setIconSize('lg') },
      { sep: true },
      { label: t('ctx_autoarrange'), check: state.autoArrange, fn: () => toggleAutoArrange() },
      { label: t('ctx_align_grid'), fn: () => arrangeIcons() },
    ] },
    { label: t('ctx_sort'), glyph: '↕', sub: [
      { label: t('ctx_sort_name'), fn: () => sortDesktop('name') },
      { label: t('ctx_sort_type'), fn: () => sortDesktop('type') },
      { label: t('ctx_sort_date'), fn: () => sortDesktop('date') },
    ] },
    { label: t('ctx_refresh'), glyph: '🔄', fn: () => renderDesktop() },
    { sep: true },
    { label: t('ctx_new'), glyph: '✚', sub: [
      { label: t('ctx_new_shortcut'), glyph: '🔗', fn: () => newShortcut() },
      { label: t('ctx_new_app_shortcut'), glyph: '▦', disabled: !addable.length, fn: () => pickAppMenu(e, addable) },
      { label: t('ctx_new_file_shortcut'), glyph: '📄', fn: () => newFileShortcut() },
      { label: t('ctx_new_link'), glyph: '🔗', fn: () => newLinkShortcut() },
    ] },
    { sep: true },
    { label: t('ctx_reset_apps'), glyph: '♻', danger: true, fn: () => confirmReset() },
  ]);
}
function pickAppMenu(e, apps) {
  showCtx(e.clientX, e.clientY, apps.map((a) => ({
    label: a.name, glyph: a.glyph, fn: () => addItem({ type: 'app', target: a.id, label: a.name }),
  })));
}

// ---- context-menu widget: nested submenus + checkmarks + full keyboard navigation ----
// Entry shape (all back-compatible — old callers pass a flat list and still work):
//   { label, glyph?, fn, sep?, danger?, disabled?, sub?:[entries], check? }
let ctxLayers = [];                 // open menus, root first; submenus are <body>-appended siblings
let ctxKeyBound = false;

function ctxItemHTML(en) {
  const g = `<span class="g">${en.glyph || ''}</span>`;
  const tail = en.sub ? '<span class="chev">›</span>' : (en.check ? '<span class="ck">✓</span>' : '');
  return g + `<span class="lbl">${escapeHtml(en.label)}</span>` + tail;
}
function buildLayer(el, entries, depth) {
  el.innerHTML = ''; el.setAttribute('role', 'menu');
  const layer = { el, depth, items: [], focus: -1 };
  for (const en of entries) {
    if (en.sep) { const s = document.createElement('div'); s.className = 'ctx-sep'; el.appendChild(s); continue; }
    const it = document.createElement('div');
    it.className = 'ctx-item' + (en.danger ? ' danger' : '') + (en.disabled ? ' disabled' : '') + (en.sub ? ' has-sub' : '');
    it.setAttribute('role', 'menuitem'); if (en.sub) it.setAttribute('aria-haspopup', 'true');
    it.innerHTML = ctxItemHTML(en);
    const idx = layer.items.length;
    it.addEventListener('mouseenter', () => { setFocus(layer, idx); if (en.sub) openSub(layer, layer.items[idx]); else closeDeeperThan(depth); });
    it.addEventListener('click', (ev) => { ev.stopPropagation(); activate(layer, layer.items[idx]); });
    el.appendChild(it);
    layer.items.push({ el: it, en, idx });
  }
  return layer;
}
function placeMenu(el, x, y) {
  el.classList.remove('hidden');
  const r = el.getBoundingClientRect();
  el.style.left = Math.max(6, Math.min(x, window.innerWidth - r.width - 6)) + 'px';
  el.style.top = Math.max(6, Math.min(y, window.innerHeight - r.height - 6)) + 'px';
}
function showCtx(x, y, entries) {
  hideCtx();                                  // reset any previous menu + submenus
  const root = document.getElementById('ctxmenu');
  ctxLayers = [buildLayer(root, entries, 0)];
  placeMenu(root, x, y);
  if (!ctxKeyBound) { document.addEventListener('keydown', ctxKey, true); ctxKeyBound = true; }
}
function hideCtx() {
  ctxLayers = [];
  // Remove EVERY body-level submenu, including any that a hover/event race left untracked, so
  // layers can never accumulate. The root #ctxmenu is excluded — it's a permanent node we just hide.
  document.querySelectorAll('body > .ctx:not(#ctxmenu)').forEach((el) => el.remove());
  const root = document.getElementById('ctxmenu');
  if (root) { root.classList.add('hidden'); root.innerHTML = ''; }
  if (ctxKeyBound) { document.removeEventListener('keydown', ctxKey, true); ctxKeyBound = false; }
}
function closeDeeperThan(depth) {
  while (ctxLayers.length && ctxLayers[ctxLayers.length - 1].depth > depth) ctxLayers.pop().el.remove();
}
function openSub(layer, item) {
  if (!item.en.sub) return null;
  closeDeeperThan(layer.depth);               // collapse any sibling submenu first
  const el = document.createElement('div'); el.className = 'ctx'; document.body.appendChild(el);
  const child = buildLayer(el, item.en.sub, layer.depth + 1);
  const r = item.el.getBoundingClientRect();
  el.classList.remove('hidden');
  const w = el.getBoundingClientRect().width;
  let x = r.right - 4; if (x + w > window.innerWidth - 6) x = r.left - w + 4;   // flip to the left if no room
  placeMenu(el, x, r.top - 6);
  ctxLayers.push(child);
  return child;
}
function setFocus(layer, idx) { layer.items.forEach((it, i) => it.el.classList.toggle('focus', i === idx)); layer.focus = idx; }
function moveFocus(layer, dir) {
  const n = layer.items.length; if (!n) return;
  let i = layer.focus;
  for (let k = 0; k < n; k++) { i = (i + dir + n) % n; if (!layer.items[i].en.disabled) return setFocus(layer, i); }
}
function activate(layer, item) {
  const en = item && item.en; if (!en || en.disabled) return;
  if (en.sub) { const child = openSub(layer, item); if (child) moveFocus(child, 1); return; }
  hideCtx(); en.fn();
}
// Keyboard model: ↑/↓ (+ Tab) move, Home/End jump, →/Enter open submenu or activate, ←/Esc step back.
// Bound in capture phase so it pre-empts the global osKeydown (e.g. its own Escape handler).
function ctxKey(e) {
  if (!ctxLayers.length) return;
  const layer = ctxLayers[ctxLayers.length - 1], k = e.key;
  const stop = () => { e.preventDefault(); e.stopPropagation(); };
  if (k === 'ArrowDown' || (k === 'Tab' && !e.shiftKey)) { stop(); moveFocus(layer, 1); }
  else if (k === 'ArrowUp' || (k === 'Tab' && e.shiftKey)) { stop(); moveFocus(layer, -1); }
  else if (k === 'Home') { stop(); layer.focus = -1; moveFocus(layer, 1); }
  else if (k === 'End') { stop(); layer.focus = 0; moveFocus(layer, -1); }
  else if (k === 'ArrowRight') { const it = layer.items[layer.focus]; if (it && it.en.sub) { stop(); const c = openSub(layer, it); if (c) moveFocus(c, 1); } }
  else if (k === 'ArrowLeft') { if (ctxLayers.length > 1) { stop(); closeDeeperThan(layer.depth - 1); } }
  else if (k === 'Enter' || k === ' ') { const it = layer.items[layer.focus]; if (it) { stop(); activate(layer, it); } }
  else if (k === 'Escape') { stop(); if (ctxLayers.length > 1) closeDeeperThan(layer.depth - 1); else hideCtx(); }
}

// ---- generic OS modal: styled prompt / confirm / properties (replaces native prompt()/confirm()) ----
// Resolves to the input string (prompt), true/false (confirm), or true (info). Enter = OK, Esc /
// backdrop = cancel. One modal at a time; the scrim element is reused.
function osModal({ title, glyph, bodyHTML = '', withInput, value, placeholder, okText, cancelText, danger, okOnly }) {
  return new Promise((resolve) => {
    const scrim = document.getElementById('os-modal-scrim');
    const box = document.createElement('div');
    box.className = 'os-modal'; box.setAttribute('role', 'dialog'); box.setAttribute('aria-modal', 'true');
    box.innerHTML =
      (title ? `<h3>${glyph ? `<span class="g">${glyph}</span>` : ''}<span>${escapeHtml(title)}</span></h3>` : '') +
      bodyHTML +
      (withInput ? `<input type="text" id="os-modal-input" autocomplete="off" spellcheck="false">` : '') +
      `<div class="row">` +
        (okOnly ? '' : `<button class="os-dialog-btn" data-act="cancel">${escapeHtml(cancelText || t('cancel'))}</button>`) +
        `<button class="os-dialog-btn ${danger ? 'danger' : 'primary'}" data-act="ok">${escapeHtml(okText || t('ok'))}</button>` +
      `</div>`;
    scrim.innerHTML = ''; scrim.appendChild(box);
    scrim.classList.add('show'); scrim.setAttribute('aria-hidden', 'false');
    const input = box.querySelector('#os-modal-input');
    if (input) { input.value = value || ''; if (placeholder) input.placeholder = placeholder; setTimeout(() => { input.focus(); input.select(); }, 30); }
    let closed = false;
    const close = (result) => {
      if (closed) return; closed = true;
      scrim.classList.remove('show'); scrim.setAttribute('aria-hidden', 'true');
      setTimeout(() => { if (!scrim.classList.contains('show')) scrim.innerHTML = ''; }, 160);
      document.removeEventListener('keydown', onKey, true);
      resolve(result);
    };
    const onKey = (e) => {
      if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(okOnly ? true : null); }
      else if (e.key === 'Enter') { e.preventDefault(); e.stopPropagation(); close(input ? input.value : true); }
    };
    document.addEventListener('keydown', onKey, true);
    box.querySelector('[data-act="ok"]').addEventListener('click', () => close(input ? input.value : true));
    const cancelBtn = box.querySelector('[data-act="cancel"]');
    if (cancelBtn) cancelBtn.addEventListener('click', () => close(okOnly ? true : null));
    scrim.onclick = (e) => { if (e.target === scrim) close(okOnly ? true : null); };
  });
}
function osPrompt({ title, glyph, label, value, okText }) {
  return osModal({ title, glyph, bodyHTML: label ? `<p>${escapeHtml(label)}</p>` : '', withInput: true, value, placeholder: label, okText })
    .then((v) => (v == null ? null : String(v)));
}
function osConfirm({ title, body, okText, danger }) {
  return osModal({ title, glyph: danger ? '⚠️' : 'ℹ️', bodyHTML: body ? `<p>${escapeHtml(body)}</p>` : '', okText, danger })
    .then((v) => v != null && v !== false);
}
function osInfo({ title, glyph, rows }) {
  const dl = `<dl class="props">${rows.map(([k, v]) => `<dt>${escapeHtml(k)}</dt><dd>${escapeHtml(String(v))}</dd>`).join('')}</dl>`;
  return osModal({ title, glyph, bodyHTML: dl, okOnly: true, okText: t('close') });
}

// Spatial keyboard navigation on the desktop (arrows move the selection to the nearest icon in that
// direction; Home/End jump to first/last; F2 renames; Enter opens). Active only when the desktop
// itself "has focus": no window, no open chrome/modal, and the event isn't in a text field. Returns
// true when it consumed the key.
function desktopNavKey(e) {
  if (WM.active() || isEditable(e.target) || e.ctrlKey || e.altKey || e.metaKey) return false;
  const hidden = (id) => { const el = document.getElementById(id); return !el || el.classList.contains('hidden'); };
  const modalUp = document.getElementById('os-modal-scrim').classList.contains('show');
  const dlgUp = (document.getElementById('os-dialog-scrim') || { classList: { contains: () => false } }).classList.contains('visible');
  const copilotUp = Copilot && Copilot.isOpen && Copilot.isOpen();
  if (!hidden('ctxmenu') || !hidden('start-menu') || !hidden('action-center') || modalUp || dlgUp || copilotUp) return false;
  const k = e.key;
  if (k === 'F2') { const uid = [...selIcons][0]; if (uid) { e.preventDefault(); beginRename(uid); return true; } return false; }
  if (k === 'Enter') {
    if (selIcons.size === 1) { const it = state.desktop.find((x) => x.uid === [...selIcons][0]); if (it) { e.preventDefault(); openItem(it); return true; } }
    return false;
  }
  if (!/^Arrow(Left|Right|Up|Down)$|^Home$|^End$/.test(k)) return false;
  const els = [...document.querySelectorAll('#desktop .icon')];
  if (!els.length) return false;
  const mid = (el) => { const r = el.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; };
  const curUid = [...selIcons][selIcons.size - 1] || null;
  const curEl = curUid ? els.find((el) => el.dataset.uid === curUid) : null;
  let next = null;
  if (k === 'Home') next = els[0];
  else if (k === 'End') next = els[els.length - 1];
  else if (!curEl) next = els[0];
  else {
    const c = mid(curEl), dir = k.slice(5).toLowerCase();
    let bestScore = Infinity;
    for (const el of els) {
      if (el === curEl) continue;
      const m = mid(el), dx = m.x - c.x, dy = m.y - c.y;
      const ok = dir === 'left' ? dx < -2 : dir === 'right' ? dx > 2 : dir === 'up' ? dy < -2 : dy > 2;
      if (!ok) continue;
      const horiz = dir === 'left' || dir === 'right';
      const score = (horiz ? Math.abs(dx) : Math.abs(dy)) + (horiz ? Math.abs(dy) : Math.abs(dx)) * 2;   // prefer same row/column
      if (score < bestScore) { bestScore = score; next = el; }
    }
  }
  e.preventDefault();
  if (!next) return true;                       // nothing that way — swallow so the page doesn't scroll
  selIcons.clear(); selIcons.add(next.dataset.uid);
  iconAnchor = state.desktop.findIndex((x) => x.uid === next.dataset.uid);
  renderDesktop();
  const f = document.querySelector(`#desktop .icon[data-uid="${CSS.escape(next.dataset.uid)}"]`); if (f) f.focus();
  return true;
}

// ===== Start menu (Windows-11 style: pinned grid, recommended, all-apps, in-menu search) =====
function renderStartMenu() {
  renderStartPinned();
  renderRecommended();
  renderAllApps();
}

// Pinned apps grid (the "Aggiunte" section). Drag-free, but right-click toggles pin/taskbar.
function renderStartPinned() {
  const g = document.getElementById('sm-pinned');
  if (!g) return;
  g.innerHTML = '';
  const pinned = state.startPins.map(byId).filter(Boolean);
  if (!pinned.length) { g.innerHTML = `<div class="sm-empty">${escapeHtml(t('start_empty_pinned'))}</div>`; return; }
  for (const a of pinned) g.appendChild(startAppTile(a));
}

function startAppTile(a) {
  const el = document.createElement('button');
  el.className = 'sm-item'; el.type = 'button';
  el.innerHTML = `<span class="glyph">${a.glyph}</span><span class="label">${escapeHtml(a.name)}</span>`;
  el.title = a.name;
  el.addEventListener('click', () => { WM.open(a); closeStart(); });
  el.addEventListener('contextmenu', (e) => { e.preventDefault(); startAppMenu(e, a); });
  return el;
}

// All-apps list (alphabetical), shown when the user taps "Tutte le app".
function renderAllApps() {
  const list = document.getElementById('sm-all');
  if (!list) return;
  list.innerHTML = '';
  const apps = [...state.apps].sort((a, b) => a.name.localeCompare(b.name));
  for (const a of apps) {
    const pinned = state.startPins.includes(a.id);
    const row = document.createElement('button');
    row.className = 'sm-row'; row.type = 'button';
    row.innerHTML = `<span class="g">${a.glyph}</span><span class="n">${escapeHtml(a.name)}</span>` +
      (pinned ? `<span class="sm-rowtag">${escapeHtml(t('start_pinned_tag'))}</span>` : '');
    row.title = a.name;
    row.addEventListener('click', () => { WM.open(a); closeStart(); });
    row.addEventListener('contextmenu', (e) => { e.preventDefault(); startAppMenu(e, a); });
    list.appendChild(row);
  }
}

// Recommended: the most recently opened files (Windows-11 "Consigliati"). Falls back to a
// friendly hint when nothing has been opened yet.
function renderRecommended() {
  const r = document.getElementById('sm-reco');
  if (!r) return;
  r.innerHTML = '';
  applyRecoCollapsed();
  const recents = state.recent.filter((x) => x && x.path).slice(0, 6);
  if (!recents.length) {
    r.innerHTML = `<div class="sm-empty">${escapeHtml(t('start_empty_reco'))}</div>`;
    return;
  }
  for (const it of recents) {
    const name = it.path.split('/').pop();
    const dir = it.path.slice(0, it.path.length - name.length - 1) || '/';
    const card = document.createElement('button');
    card.className = 'sm-reco-item'; card.type = 'button';
    card.innerHTML = `<span class="g">${itemGlyph({ type: 'file', target: it.path })}</span>` +
      `<span class="t"><span class="n">${escapeHtml(name)}</span><span class="p">${escapeHtml(dir)} · ${relTime(it.ts)}</span></span>`;
    card.title = it.path;
    card.addEventListener('click', () => { openFile(it.path); closeStart(); });
    card.addEventListener('contextmenu', (e) => {
      e.preventDefault();
      showCtx(e.clientX, e.clientY, [
        { label: t('open'), glyph: '📂', fn: () => { openFile(it.path); closeStart(); } },
        { label: t('ctx_show_in_folder'), glyph: '🗂️', fn: () => { WM.open(byId('file-commander'), 'path=' + encodeURIComponent(dir)); closeStart(); } },
        { sep: true },
        { label: t('ctx_remove_reco'), danger: true, fn: () => { state.recent = state.recent.filter((x) => x.path !== it.path); saveUiState(); renderRecommended(); } },
      ]);
    });
    r.appendChild(card);
  }
}

// Compact relative time ("ora", "5 min fa", "3 h fa", "2 g fa") for recommended subtitles.
function relTime(ts) {
  if (!ts) return '';
  const s = Math.max(0, Math.round((Date.now() - ts) / 1000));
  if (s < 60) return t('rel_now');
  const m = Math.round(s / 60); if (m < 60) return t('rel_min', { n: m });
  const h = Math.round(m / 60); if (h < 24) return t('rel_hour', { n: h });
  return t('rel_day', { n: Math.round(h / 24) });
}

// Right-click on a Start app: pin/unpin to Start, pin/unpin to taskbar, open.
function startAppMenu(e, a) {
  const inStart = state.startPins.includes(a.id);
  showCtx(e.clientX, e.clientY, [
    { label: t('open'), glyph: '📂', fn: () => { WM.open(a); closeStart(); } },
    { label: inStart ? t('ctx_unpin') : t('ctx_pin'), glyph: '📌', fn: () => toggleStartPin(a.id) },
    { label: state.pins.includes(a.id) ? t('ctx_unpin_taskbar') : t('ctx_pin_taskbar'), glyph: '📎', fn: () => togglePin(a.id) },
  ]);
}
function toggleStartPin(id) {
  state.startPins = state.startPins.includes(id) ? state.startPins.filter((x) => x !== id) : [...state.startPins, id];
  saveUiState();
  renderStartPinned();
  renderAllApps();
}

// ---- Start view switching (home ↔ all-apps ↔ search results) ----
function showStartView(name) {
  for (const v of ['sm-home', 'sm-allapps', 'sm-results']) {
    const el = document.getElementById(v);
    if (el) el.classList.toggle('hidden', v !== name);
  }
  startNavReset();                                 // each view starts with a clean keyboard selection
}
function resetStartView() {
  const inp = document.getElementById('sm-search-input');
  if (inp) inp.value = '';
  showStartView('sm-home');
}

// ---- Collapsible "Consigliati" (declutter, persisted on the device, synced across clients) ----
function applyRecoCollapsed() {
  const on = !!state.recoCollapsed;
  const reco = document.getElementById('sm-reco');
  const head = document.querySelector('.sm-reco-head');
  const btn = document.getElementById('sm-reco-toggle');
  if (reco) reco.classList.toggle('collapsed', on);
  if (head) head.classList.toggle('collapsed', on);
  if (btn) { btn.setAttribute('aria-expanded', String(!on)); btn.title = on ? t('reco_show') : t('reco_hide'); }
}

// ---- Start menu keyboard navigation (the Cardputer is keyboard-first) ----
// Open Start and the arrows move a highlight across the visible region — the pinned grid
// (6-wide, grid-aware) on Home, or the all-apps list — Enter launches, Home/End jump, and
// ArrowUp off the top row hands focus back to the search box. Typing letters still goes to the
// search field; once a query is present the existing search-results keys take over.
const StartNav = { sel: -1 };
function startNavItems() {
  const inp = document.getElementById('sm-search-input');
  if (inp && inp.value.trim()) return { els: [], cols: 1 };     // searching → results keys own it
  const all = document.getElementById('sm-allapps');
  if (all && !all.classList.contains('hidden')) return { els: [...document.querySelectorAll('#sm-all .sm-row')], cols: 1 };
  const home = document.getElementById('sm-home');
  if (home && !home.classList.contains('hidden')) return { els: [...document.querySelectorAll('#sm-pinned .sm-item')], cols: 6 };
  return { els: [], cols: 1 };
}
function startNavPaint(els) {
  for (let i = 0; i < els.length; i++) els[i].classList.toggle('kbsel', i === StartNav.sel);
  if (StartNav.sel >= 0 && els[StartNav.sel]) els[StartNav.sel].scrollIntoView({ block: 'nearest' });
}
function startNavReset() {
  StartNav.sel = -1;
  document.querySelectorAll('#start-menu .kbsel').forEach((el) => el.classList.remove('kbsel'));
}
function onStartKey(e) {
  const inp = document.getElementById('sm-search-input');
  const searching = !!(inp && inp.value.trim());
  if (e.key === 'Escape') { if (!searching) { e.preventDefault(); closeStart(); } return; }
  if (searching) return;                            // onSearchKey drives the results list
  if (e.key === 'Enter') {
    const { els } = startNavItems();
    if (StartNav.sel >= 0 && els[StartNav.sel]) { e.preventDefault(); e.stopPropagation(); els[StartNav.sel].click(); }
    return;
  }
  if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(e.key)) return;
  const { els, cols } = startNavItems();
  const n = els.length;
  if (!n) return;
  e.preventDefault(); e.stopPropagation();
  if (StartNav.sel < 0) { StartNav.sel = 0; startNavPaint(els); return; }
  let i = StartNav.sel;
  if (e.key === 'Home') i = 0;
  else if (e.key === 'End') i = n - 1;
  else if (e.key === 'ArrowRight') i = Math.min(n - 1, i + 1);
  else if (e.key === 'ArrowLeft') i = Math.max(0, i - 1);
  else if (e.key === 'ArrowDown') { const ni = i + cols; i = ni < n ? ni : n - 1; }
  else if (e.key === 'ArrowUp') {
    const ni = i - cols;
    if (ni >= 0) i = ni;
    else { startNavReset(); if (inp) inp.focus(); return; }   // off the top → back to search
  }
  StartNav.sel = i; startNavPaint(els);
}

let prevOpen = new Set();                          // ids open at the last render → animate only new launches
function renderTaskbar() {
  const pinned = document.getElementById('task-pinned');
  const running = document.getElementById('task-running');
  const openIds = new Set(WM.list().map((w) => w.app.id));
  pinned.innerHTML = '';
  for (const id of state.pins) {
    const a = byId(id); if (!a) continue;
    pinned.appendChild(taskBtn(a, openIds.has(id), openIds.has(id) && !prevOpen.has(id)));
  }
  running.innerHTML = '';
  for (const w of WM.list()) {
    if (state.pins.includes(w.app.id)) continue; // already shown as pinned
    running.appendChild(taskBtn(w.app, true, !prevOpen.has(w.app.id)));
  }
  // The divider only makes sense when there's at least one app button after the search box.
  const div = document.querySelector('.tb-divider');
  if (div) div.classList.toggle('hidden', pinned.childElementCount + running.childElementCount === 0);
  prevOpen = openIds;
}

function taskBtn(a, isOpen, isNew) {
  const b = document.createElement('button');
  b.className = 'task-btn' + (isOpen ? ' running' : '') + (isNew ? ' launching' : '');
  if (WM.list().some((w) => w.app.id === a.id && w.el.classList.contains('active') && !w.min)) b.classList.add('focus');
  b.innerHTML = `<span class="glyph">${a.glyph}</span><span class="label">${a.name}</span>`;
  b.title = a.name;                                // native fallback tooltip too
  b.addEventListener('click', () => (isOpen ? WM.toggle(a.id) : WM.open(a)));
  b.addEventListener('contextmenu', (e) => { e.preventDefault(); togglePin(a.id); });
  return b;
}

function togglePin(id) {
  state.pins = state.pins.includes(id) ? state.pins.filter((x) => x !== id) : [...state.pins, id];
  saveUiState();
  renderTaskbar();
}

// ---- unified search (apps + SD files), shared by the taskbar box and the Start menu ----
// Both inputs drive ONE engine. File results come from the in-memory FsIndex (crawled once,
// cached, refreshed on fs.changed) — so every keystroke filters RAM, never the network. The
// engine renders into whichever surface is active: the taskbar popover or the Start results view.
const SEARCH = { results: [], sel: 0, q: '', target: null, userMoved: false };   // target: 'taskbar' | 'start'
let searchTimer = null;

// Friendly headings + order for grouping file results (folders first, then by kind). Labels are
// resolved live via the catalog so they follow the OS language.
const CAT_KEY = {
  folder: 'folders', doc: 'cat_doc', image: 'cat_image', audio: 'cat_audio',
  video: 'cat_video', code: 'cat_code', game: 'cat_game', other: 'cat_other',
};
const catLabel = (cat) => t(CAT_KEY[cat] || 'file');
const CAT_ORDER = ['folder', 'doc', 'image', 'audio', 'video', 'code', 'game', 'other'];

const searchActive = () => SEARCH.target != null;
const resultsContainer = () => document.getElementById(SEARCH.target === 'start' ? 'sm-results' : 'search-panel');

// Build the flat, ranked result list for a query: matching apps first, then indexed files.
function buildResults(q) {
  const ql = q.toLowerCase();
  const apps = state.apps.filter((a) => a.name.toLowerCase().includes(ql)).map((a) => ({ kind: 'app', app: a, name: a.name }));
  const files = FsIndex.search(q, 40).map((f) => ({ kind: 'file', path: f.path, name: f.name, isDir: f.isDir, cat: f.cat }));
  return { apps, files };
}

function runSearch(q, target) {
  SEARCH.q = q; SEARCH.target = target; SEARCH.sel = 0; SEARCH.userMoved = false;
  // Make sure the index is warm; results repaint via FsIndex.onUpdate when the crawl lands.
  if (!FsIndex.isReady()) FsIndex.ensure();
  refreshSearchView();
}

// Re-render the current query into the active surface (called on input and when the index updates).
function refreshSearchView() {
  if (!searchActive()) return;
  const { apps, files } = buildResults(SEARCH.q);
  // Result 0 is always the "Ask ANIMA" action — the unified search→AI bridge. It fires NO network
  // per keystroke (the /api/anima cascade runs only on commit), so file search stays instant.
  SEARCH.results = [{ kind: 'anima', q: SEARCH.q }, ...apps, ...files];
  const OFF = 1;                                   // index offset the anima row introduces

  let html = animaRow(SEARCH.q);
  if (apps.length) {
    html += `<div class="sp-cat">${escapeHtml(t('apps'))}</div>`;
    apps.forEach((r, i) => { html += spRow(OFF + i, r.app.glyph, r.app.name, '', SEARCH.q); });
  }
  if (files.length) {
    const grouped = {};
    files.forEach((r, i) => { (grouped[r.cat] = grouped[r.cat] || []).push({ r, i: OFF + apps.length + i }); });
    for (const cat of CAT_ORDER) {
      const items = grouped[cat]; if (!items) continue;
      html += `<div class="sp-cat">${escapeHtml(catLabel(cat))}</div>`;
      for (const { r, i } of items) {
        const g = r.isDir ? '📁' : itemGlyph({ type: 'file', target: r.path });
        const dir = r.path.slice(0, Math.max(0, r.path.length - r.name.length - 1)) || '/';
        html += spRow(i, g, r.name, dir, SEARCH.q);
      }
    }
  }
  if (!apps.length && !files.length && !FsIndex.isReady()) {
    html += `<div class="sp-empty"><span class="sp-spin"></span> ${escapeHtml(t('search_indexing'))}</div>`;
  }
  // Default selection: the top app/file hit for short keyword lookups, the ANIMA row for questions.
  if (!SEARCH.userMoved) SEARCH.sel = (apps.length + files.length && !looksLikeNL(SEARCH.q)) ? OFF : 0;
  if (SEARCH.sel >= SEARCH.results.length) SEARCH.sel = 0;

  const panel = resultsContainer();
  panel.innerHTML = html;
  if (SEARCH.target === 'taskbar') panel.classList.remove('hidden');
  else showStartView('sm-results');
  [...panel.querySelectorAll('.sp-item')].forEach((el) => {
    el.addEventListener('click', () => openResult(SEARCH.results[+el.dataset.i]));
    el.addEventListener('mousemove', () => setSel(+el.dataset.i));
  });
  highlightSel();
}

// Bold the matched part of a name (Windows-11-style match highlighting).
function highlightMatch(name, q) {
  const esc = escapeHtml(name);
  const term = (q || '').trim().split(/\s+/)[0];
  if (!term) return esc;
  const i = name.toLowerCase().indexOf(term.toLowerCase());
  if (i < 0) return esc;
  return escapeHtml(name.slice(0, i)) + '<b>' + escapeHtml(name.slice(i, i + term.length)) + '</b>' + escapeHtml(name.slice(i + term.length));
}
const spRow = (i, g, name, sub, q) =>
  `<div class="sp-item" data-i="${i}"><span class="g">${g}</span><span class="t"><div class="n">${highlightMatch(name, q)}</div>${sub ? `<div class="p">${escapeHtml(sub)}</div>` : ''}</span></div>`;
// The "Ask ANIMA" row that bridges search → the AI copilot (always result index 0).
const animaRow = (q) =>
  `<div class="sp-item sp-anima" data-i="0"><span class="g">✻</span><span class="t"><div class="n">${escapeHtml(t('search_anima'))}</div><div class="p">${escapeHtml(q)}</div></span></div>`;
// Heuristic: does the query read like a natural-language question/command (→ default to ANIMA)
// rather than a short app/file name lookup (→ default to the first hit)?
function looksLikeNL(q) {
  const s = (q || '').trim();
  if (/\?$/.test(s)) return true;
  if (s.split(/\s+/).length >= 3) return true;
  return /^(apri|crea|ricord|cerca |che |come |chi |cosa |quando |dove |perch|quanto|meteo|calcola|open |create |remind|what |who |how |when |where |why |weather )/i.test(s);
}

function setSel(i) { SEARCH.sel = i; highlightSel(); }
function moveSel(d) {
  if (!SEARCH.results.length) return;
  SEARCH.userMoved = true;
  SEARCH.sel = (SEARCH.sel + d + SEARCH.results.length) % SEARCH.results.length;
  highlightSel(true);
}
function highlightSel(scroll) {
  const panel = resultsContainer();
  if (!panel) return;
  const items = panel.querySelectorAll('.sp-item');
  items.forEach((el, i) => el.classList.toggle('sel', i === SEARCH.sel));
  if (scroll && items[SEARCH.sel]) items[SEARCH.sel].scrollIntoView({ block: 'nearest' });
}
function openResult(r) {
  if (!r) return;
  // The "Ask ANIMA" row hands the query to the OS-wide copilot (closing search first).
  if (r.kind === 'anima') {
    const q = r.q;
    if (SEARCH.target === 'start') closeStart(); else closeSearch();
    if (Copilot) Copilot.ask(q); else WM.open(byId('anima'), 'q=' + encodeURIComponent(q));
    return;
  }
  if (r.kind === 'app') WM.open(r.app);
  else if (r.isDir) WM.open(byId('file-commander'), 'path=' + encodeURIComponent(r.path));
  else openFile(r.path);
  if (SEARCH.target === 'start') closeStart(); else closeSearch();
}
// Close the taskbar search popover and clear its box.
function closeSearch() {
  document.getElementById('search-panel').classList.add('hidden');
  const i = document.getElementById('search-input'); if (i) { i.value = ''; i.blur(); }
  SEARCH.results = []; SEARCH.sel = 0; SEARCH.target = null;
}

function wireSearch() {
  // Taskbar search box → taskbar popover.
  const input = document.getElementById('search-input');
  input.addEventListener('input', () => {
    clearTimeout(searchTimer);
    const q = input.value.trim();
    if (!q) { document.getElementById('search-panel').classList.add('hidden'); SEARCH.target = null; return; }
    searchTimer = setTimeout(() => runSearch(q, 'taskbar'), 120);
  });
  input.addEventListener('focus', () => { const q = input.value.trim(); if (q) runSearch(q, 'taskbar'); });
  input.addEventListener('keydown', (e) => onSearchKey(e, () => { closeSearch(); }));

  // Start menu search box → in-menu results view.
  const sInput = document.getElementById('sm-search-input');
  if (sInput) {
    sInput.addEventListener('input', () => {
      clearTimeout(searchTimer);
      startNavReset();                              // clear any grid highlight when the query changes
      const q = sInput.value.trim();
      if (!q) { SEARCH.target = null; showStartView('sm-home'); return; }
      searchTimer = setTimeout(() => runSearch(q, 'start'), 120);
    });
    sInput.addEventListener('keydown', (e) => onSearchKey(e, () => { sInput.value = ''; SEARCH.target = null; showStartView('sm-home'); }));
  }
}

// Shared arrow/enter/escape handling for both search inputs.
function onSearchKey(e, onEscape) {
  if (e.key === 'ArrowDown') { e.preventDefault(); moveSel(1); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); moveSel(-1); }
  else if (e.key === 'Enter') { e.preventDefault(); openResult(SEARCH.results[SEARCH.sel]); }
  else if (e.key === 'Escape') { e.preventDefault(); onEscape(); }
}

// Reboot the Cardputer from the Start power button, with a confirm + toast (auth-gated API).
async function rebootDevice() {
  showCtx(document.getElementById('sm-power').getBoundingClientRect().left - 40,
    window.innerHeight - 120, [
    { label: t('start_power_title'), glyph: '🔄', danger: true, fn: async () => {
        showToast(t('reboot_starting'), '🔄', 'info', 6000);
        try {
          const r = await fetch('/api/reboot', { method: 'POST' });
          if (!r.ok) throw new Error(r.status);
          showToast(t('reboot_reconnect'), '⏻', 'success', 6000);
        } catch { showToast(t('reboot_fail'), '⚠️', 'error'); }
      } },
  ]);
}

function wireChrome() {
  document.getElementById('start-btn').addEventListener('click', toggleStart);
  
  const toggleAc = () => { document.getElementById('action-center').classList.toggle('hidden'); closeStart(); closeSearch(); };
  const closeAc = () => document.getElementById('action-center').classList.add('hidden');
  const trayNet = document.getElementById('tray-net');
  const trayStorage = document.getElementById('tray-storage');
  if (trayNet) { trayNet.addEventListener('click', toggleAc); trayNet.style.cursor = 'pointer'; }
  if (trayStorage) { trayStorage.addEventListener('click', toggleAc); trayStorage.style.cursor = 'pointer'; }

  // Action Center: Theme toggle — persiste su SD del Cardputer
  const acTheme = document.getElementById('ac-theme');
  if (acTheme) {
    acTheme.addEventListener('click', () => {
      const isDark = document.documentElement.dataset.theme !== 'light';
      const next = isDark ? 'light' : 'dark';
      applyTheme(next);
      acTheme.classList.toggle('active', next === 'dark');
      persistTheme(next); // → scrive /system/config/settings.json sul Cardputer
    });
  }

  // Action Center: Wi-Fi tile reflects the REAL connection (driven by renderNetwork) and opens
  // Settings to manage it — no more fake on/off toggle.
  const acWifi = document.getElementById('ac-wifi');
  if (acWifi) {
    acWifi.classList.remove('active');           // honest until refreshStatus reports a link
    acWifi.addEventListener('click', () => { const s = byId('settings'); if (s) { WM.open(s); closeAc(); } });
  }

  // Action Center: Eco Mode — really trims cosmetic animations (persisted per console).
  const acEco = document.getElementById('ac-eco');
  if (acEco) {
    acEco.classList.toggle('active', localStorage.getItem('nucleo.eco') === '1');
    acEco.addEventListener('click', () => { const on = !acEco.classList.contains('active'); acEco.classList.toggle('active', on); applyEco(on); });
  }

  // Brightness slider — really dims the operator console (persisted per browser).
  const slider = document.getElementById('ac-brightness');
  if (slider) {
    const saved = parseInt(localStorage.getItem('nucleo.brightness') || '100', 10);
    slider.value = String(isFinite(saved) ? saved : 100);
    slider.addEventListener('input', () => applyBrightness(slider.value));
  }


  // Far-right sliver minimizes everything (and restores on a second click), like Windows.
  document.getElementById('show-desktop').addEventListener('click', () => WM.showDesktop());
  // Clicking the clock opens the Calendar app (Win11-style), if it's installed.
  document.getElementById('clock').addEventListener('click', () => { const cal = byId('calendar'); if (cal) WM.open(cal); });
  document.getElementById('clock').style.cursor = 'pointer';

  // Start menu navigation: All apps / Back / Power / Account.
  const allBtn = document.getElementById('sm-allapps-btn');
  if (allBtn) allBtn.addEventListener('click', () => showStartView('sm-allapps'));
  const backBtn = document.getElementById('sm-back-btn');
  if (backBtn) backBtn.addEventListener('click', () => showStartView('sm-home'));
  const powerBtn = document.getElementById('sm-power');
  if (powerBtn) powerBtn.addEventListener('click', (e) => { e.stopPropagation(); rebootDevice(); });
  const acct = document.getElementById('sm-account');
  if (acct) acct.addEventListener('click', () => { const s = byId('settings'); if (s) { WM.open(s); closeStart(); } });
  // Collapsible "Consigliati": persist on the device so the choice follows the user across clients.
  const recoToggle = document.getElementById('sm-reco-toggle');
  if (recoToggle) recoToggle.addEventListener('click', () => { state.recoCollapsed = !state.recoCollapsed; applyRecoCollapsed(); saveUiState(); });
  // Keyboard navigation over the menu (capture phase so it runs before the search box's own keys).
  const smEl = document.getElementById('start-menu');
  if (smEl) smEl.addEventListener('keydown', onStartKey, true);

  wireSearch();
  document.addEventListener('click', (e) => {
    const sm = document.getElementById('start-menu');
    const ac = document.getElementById('action-center');
    if (!sm.contains(e.target) && !document.getElementById('start-btn').contains(e.target)) closeStart();
    if (!ac.contains(e.target) && (!trayNet || !trayNet.contains(e.target)) && (!trayStorage || !trayStorage.contains(e.target))) closeAc();
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
function toggleStart() {
  const sm = document.getElementById('start-menu');
  const opening = sm.classList.contains('hidden');
  sm.classList.toggle('hidden');
  if (opening) {
    closeSearch();                                   // don't leave the taskbar popover open behind Start
    resetStartView();
    // Focus the in-menu search after the open animation so typing immediately searches (Win11).
    const inp = document.getElementById('sm-search-input');
    if (inp) setTimeout(() => inp.focus(), 60);
  }
}
function closeStart() {
  document.getElementById('start-menu').classList.add('hidden');
  resetStartView();
  if (SEARCH.target === 'start') { SEARCH.results = []; SEARCH.target = null; }
}

function tickClock() {
  const d = new Date();
  const time = String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0');
  const date = d.toLocaleDateString(undefined, { day: '2-digit', month: '2-digit', year: 'numeric' });
  const c = document.getElementById('clock');
  const t = c.querySelector('.ck-time'), dt = c.querySelector('.ck-date');
  if (t) t.textContent = time; if (dt) dt.textContent = date;
  c.title = d.toLocaleDateString(undefined, { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' });
}

let _statusTimer = null;
function refreshStatus() {
  clearTimeout(_statusTimer);
  _statusTimer = setTimeout(doRefreshStatus, 400);
}

async function doRefreshStatus() {
  try {
    const s = await fetchJSON('/api/status');
    const txt = `SD ${fmtSize(s.storage.free_bytes)} free`;
    document.querySelector('#tray-storage .v').textContent = txt;
    const smStorage = document.getElementById('sm-storage');
    if (smStorage) smStorage.textContent = `${s.storage.fs} · ${txt}`;
    renderNetwork(s.network);
    // Rebroadcast the snapshot to every open app so they don't each poll /api/status themselves
    // (the shell already fetches it every 15 s; N embedded apps doing the same hammered the
    // single httpd task). Same {t,d} envelope as the WS forward below, so an app consumes it
    // through the same message listener. Apps keep their own fetch only when run standalone.
    for (const w of WM.list()) { const f = w.el.querySelector('iframe'); if (f) try { f.contentWindow.postMessage({ t: 'status.snapshot', d: s }, '*'); } catch {} }
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
  // Keep the Action Center Wi-Fi tile honest: lit only when actually linked in station mode.
  const acWifi = document.getElementById('ac-wifi');
  if (acWifi) acWifi.classList.toggle('active', !!(net && net.mode === 'sta' && net.ssid));
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

// Device-busy indicator. The firmware's heavy-work arbiter publishes system.busy when it serializes a
// heavy job (outbound TLS, transcribe, model pull) so two can't both OOM the PSRAM-less heap. We show a
// subtle, debounced tray pill — informative only (the arbiter degrades gracefully on its own). The job
// label is mapped to a friendly word; we NEVER inject the raw event string into innerHTML (XSS-safe).
const BUSY_JOB_LABELS = { proxy: 'Navigazione', llm: 'AI online', 'anima-get': 'ANIMA online',
  'anima-post': 'ANIMA online', transcribe: 'Trascrizione' };
function renderBusy(busy, job) {
  const el = document.getElementById('tray-busy');
  if (!el) return;
  el.hidden = !busy;
  if (!busy) { el.classList.remove('pulse'); return; }
  const label = BUSY_JOB_LABELS[job] || 'Occupato';   // mapped (safe) label only — never the raw job
  el.innerHTML = `⚙️<span class="v">${label}</span>`;
  el.title = 'Il dispositivo sta dando precedenza a un lavoro pesante; le app potrebbero rispondere con un breve ritardo.';
  el.classList.add('pulse');
}
// debounceMs: short TLS fetches flap busy on/off fast; coalesce them so the pill doesn't flicker.
// Tune this once the real device's busy cadence is observed (proxy fetches vs long model pulls).
const busyCtl = createBusyController({
  show: (job) => renderBusy(true, job),
  hide: () => renderBusy(false),
  debounceMs: 600,
});

// Register the SW for offline caching. We deliberately do NOT auto-reload on controllerchange:
// that reloads the page mid-boot during an update and can trap the user on the splash. The cache
// version bump in sw.js already drops stale caches on activate; new content lands on the next load.
if ('serviceWorker' in navigator) navigator.serviceWorker.register('sw.js').catch(() => {});
boot();

// Optional first-boot pre-copy of ANIMA Local (the ~13 MB offline WASM brain). OFF by default:
// NucleoOS never starts a download on its own — the device is a single-task server, so it pulls one
// thing at a time (see dlgate.js) and only when the user asks. The brain is provisioned explicitly
// from ANIMA ▸ Impostazioni ▸ Modelli. A power user can opt back into background pre-copy by setting
// localStorage 'nucleo.autoDownload'='1'; even then it funnels through the gate with {ifAvailable}, so
// it yields to (never queues ahead of) any user-initiated download and stays self-healing/error-safe.
function warmAnimaLocal() {
  if (localStorage.getItem('nucleo.autoDownload') !== '1') return;        // default: no automatic download
  try {
    import('/apps/anima/local/engine.js').then(async (m) => {
      if (await m.packCached()) return;                                   // already provisioned
      const a = await fetch('/api/auth/status').then((r) => r.json()).catch(() => ({ paired: false }));
      if (a && a.required && !a.paired) return;                           // not paired yet -> retry next boot
      await m.prefetchPack(null, null, { ifAvailable: true });            // gated; yields to user downloads
    }).catch(() => {});
  } catch {}
}
if ('requestIdleCallback' in window) requestIdleCallback(() => warmAnimaLocal(), { timeout: 8000 });
else setTimeout(warmAnimaLocal, 4000);

// --- OS File Dialog Logic ---
let osDialogCaller = null;
let osDialogReqId = null;
let osDialogMode = 'open'; // 'open' or 'save'
let osDialogFilter = null;
let osDialogCwd = '/data';
let osDialogFiles = [];

const osDialogScrim = document.getElementById('os-dialog-scrim');
const osDialogTitle = document.getElementById('os-dialog-title-text');
const osDialogCloseBtn = document.getElementById('os-dialog-close');
const osDialogSearch = document.getElementById('os-dialog-search');
const osDialogCrumbs = document.getElementById('os-dialog-crumbs');
const osDialogList = document.getElementById('os-dialog-list');
const osDialogFilename = document.getElementById('os-dialog-filename');
const osDialogBtnCancel = document.getElementById('os-dialog-btn-cancel');
const osDialogBtnConfirm = document.getElementById('os-dialog-btn-confirm');

function openOsFileDialog(sourceWindow, opts) {
  osDialogCaller = sourceWindow;
  osDialogReqId = opts.id || null;
  osDialogMode = opts.mode || 'open';
  osDialogFilter = opts.filter || null; // e.g. ['.png', '.jpg']
  osDialogTitle.textContent = opts.title || t(osDialogMode === 'save' ? 'osdlg_save_title' : 'osdlg_open_title');

  if (osDialogMode === 'save') {
    osDialogFilename.style.display = 'block';
    osDialogFilename.value = opts.defaultName || 'untitled.txt';
    osDialogBtnConfirm.textContent = t('save');
  } else {
    osDialogFilename.style.display = 'none';
    osDialogFilename.value = '';
    osDialogBtnConfirm.textContent = t('open');
  }
  
  osDialogCwd = opts.defaultPath || '/data';
  osDialogScrim.classList.add('visible');
  loadOsDialogDir(osDialogCwd);
}

function closeOsFileDialog(resultPath) {
  osDialogScrim.classList.remove('visible');
  if (osDialogCaller && osDialogReqId) {
    try {
      osDialogCaller.postMessage({
        type: 'os-file-dialog-result',
        id: osDialogReqId,
        action: resultPath ? osDialogMode : 'cancel',
        path: resultPath
      }, '*');
    } catch {}
  }
  osDialogCaller = null;
}

async function loadOsDialogDir(path) {
  osDialogCwd = path;
  osDialogCrumbs.innerHTML = '';
  const segs = path.split('/').filter(Boolean);
  let acc = '';
  const rootSeg = document.createElement('span');
  rootSeg.className = 'os-crumbs-seg'; rootSeg.textContent = 'SD';
  rootSeg.onclick = () => loadOsDialogDir('/');
  osDialogCrumbs.appendChild(rootSeg);
  
  for (const s of segs) {
    acc += '/' + s;
    const slash = document.createTextNode(' / ');
    const span = document.createElement('span');
    span.className = 'os-crumbs-seg'; span.textContent = s;
    const target = acc;
    span.onclick = () => loadOsDialogDir(target);
    osDialogCrumbs.appendChild(slash);
    osDialogCrumbs.appendChild(span);
  }
  
  osDialogList.innerHTML = '<div class="os-dialog-msg">Caricamento in corso...</div>';
  osDialogFiles = [];
  try {
    const r = await fetch('/api/fs/list?path=' + encodeURIComponent(path));
    if (!r.ok) throw new Error();
    const data = await r.json();
    osDialogFiles = data.entries || [];
    renderOsDialogList();
  } catch {
    osDialogList.innerHTML = '<div class="os-dialog-msg">Impossibile leggere la directory.</div>';
  }
}

function renderOsDialogList(filterQuery = '') {
  osDialogList.innerHTML = '';
  let entries = osDialogFiles;
  
  if (osDialogCwd !== '/' && osDialogCwd !== '') {
    entries = [{ name: '..', type: 'dir' }, ...entries];
  }
  
  if (filterQuery) {
    const q = filterQuery.toLowerCase();
    entries = entries.filter(e => e.type === 'dir' || e.name.toLowerCase().includes(q));
  }

  if (osDialogMode === 'open' && Array.isArray(osDialogFilter) && osDialogFilter.length > 0) {
    entries = entries.filter(e => {
      if (e.type === 'dir') return true;
      const lw = e.name.toLowerCase();
      return osDialogFilter.some(ext => lw.endsWith(ext.toLowerCase()));
    });
  }

  entries.sort((a, b) => {
    if (a.name === '..') return -1;
    if (b.name === '..') return 1;
    if (a.type !== b.type) return a.type === 'dir' ? -1 : 1;
    return a.name.localeCompare(b.name);
  });

  if (entries.length === 0) {
    osDialogList.innerHTML = '<div class="os-dialog-msg">Nessun file trovato.</div>';
    return;
  }

  entries.forEach(e => {
    const li = document.createElement('li');
    li.className = 'os-dialog-item';
    const icon = e.type === 'dir' 
      ? '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M10 4H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8l-2-2z"/></svg>'
      : '<svg viewBox="0 0 24 24" fill="currentColor"><path d="M6 2c-1.1 0-1.99.9-1.99 2L4 20c0 1.1.89 2 1.99 2H18c1.1 0 2-.9 2-2V8l-6-6H6zm7 7V3.5L18.5 9H13z"/></svg>';
    
    // The fs/list API reports the byte size as `size` (see nucleo_fsapi.c / serve-shell.mjs).
    const sizeStr = e.type === 'file' && e.size != null ? Math.round(e.size/1024) + ' KB' : '';
    li.innerHTML = `<div class="os-dialog-icon ${e.type}">${icon}</div><div class="os-dialog-name">${escapeHtml(e.name)}</div><div class="os-dialog-size">${sizeStr}</div>`;
    
    li.onclick = () => {
      document.querySelectorAll('.os-dialog-item').forEach(i => i.classList.remove('active'));
      li.classList.add('active');
      if (e.type === 'file') {
        osDialogFilename.value = e.name;
      }
    };
    
    li.ondblclick = () => {
      if (e.type === 'dir') {
        const next = e.name === '..' ? osDialogCwd.substring(0, osDialogCwd.lastIndexOf('/')) || '/' : (osDialogCwd === '/' ? '/' + e.name : osDialogCwd + '/' + e.name);
        loadOsDialogDir(next);
      } else {
        osDialogFilename.value = e.name;
        const finalPath = (osDialogCwd === '/' ? '' : osDialogCwd) + '/' + e.name;
        closeOsFileDialog(finalPath);
      }
    };
    
    osDialogList.appendChild(li);
  });
}

if (osDialogSearch) {
  osDialogSearch.addEventListener('input', (e) => renderOsDialogList(e.target.value));
}
if (osDialogCloseBtn) osDialogCloseBtn.onclick = () => closeOsFileDialog(null);
if (osDialogBtnCancel) osDialogBtnCancel.onclick = () => closeOsFileDialog(null);
if (osDialogBtnConfirm) osDialogBtnConfirm.onclick = () => {
  const val = osDialogFilename.value.trim();
  if (osDialogMode === 'save' && !val) return;
  if (!val && osDialogMode === 'open') return;
  const finalPath = (osDialogCwd === '/' ? '' : osDialogCwd) + '/' + val;
  closeOsFileDialog(finalPath);
};

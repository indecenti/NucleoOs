// Canvas renderer + keyboard driver for the on-device launcher simulator.
// Draws to a logical 240x135 framebuffer using primitives that map 1:1 to M5GFX
// (fillRect / fillRoundRect / fillCircle / text), so the firmware port stays faithful.
import { Launcher, handleKey } from './nav.js';
import { ROOT } from './menu-data.js';
import { calculatorApp } from './apps/calculator.js';
import { recorderApp } from './apps/recorder.js';
import { filesApp } from './apps/files.js';
import { photosApp } from './apps/photos.js';
import { playerApp } from './apps/player.js';
import { videoApp } from './apps/video.js';
import { notesApp } from './apps/notes.js';
// Shared bold square icon set (same source the firmware ui_icon ports). Cache-bust so edits show.
const { drawIcon, iconGfx } = await import('./icons.js?ts=' + Date.now());

// Map the simulator's mock menu-data ids to the real firmware icon ids (most match directly).
const ICON_MAP = {
  'video-player': 'video', calculator: 'calc', dosbox: 'Games', 'automation-studio': 'Tools',
  status: 'sysmon', network: 'info', settings: 'wifi', 'log-viewer': 'notepad',
  companion: 'link', swarm: 'link', media: 'Media', tools: 'Tools', system: 'System', connect: 'Connect',
};
const iconId = (it) => ICON_MAP[it.id] || it.id;

// Native apps with a real on-device implementation, keyed by menu node id. Anything
// not listed falls back to the "open in the web companion" placeholder. Add one entry
// (and one small file under apps/) per app — this mirrors the firmware's find_app().
const APPS = { [calculatorApp.id]: calculatorApp, [recorderApp.id]: recorderApp, [filesApp.id]: filesApp, [photosApp.id]: photosApp, [playerApp.id]: playerApp, [videoApp.id]: videoApp, [notesApp.id]: notesApp };

const W = 240, H = 135, STATUS = 16, HINT = 14, INSTR = 0;   // INSTR retired: the desc now lives in the hero card
const CONTENT_TOP = STATUS, CONTENT_H = H - STATUS - HINT - INSTR;
const COL = { bg: '#0b1020', panel: '#0000', fg: '#ffffff', muted: '#8aa0c8', line: '#243250', dim: '#46557e', amber: '#ffd166', green: '#7CFC9A' };
const pad2 = (n) => String(n).padStart(2, '0');
const IT_WD = ['dom', 'lun', 'mar', 'mer', 'gio', 'ven', 'sab'];
const IT_MO = ['gen', 'feb', 'mar', 'apr', 'mag', 'giu', 'lug', 'ago', 'set', 'ott', 'nov', 'dic'];
const NET = 'nonnoBob', RSSI_LVL = 3;   // mock signal for layout sign-off

const cv = document.getElementById('screen');
const ctx = cv.getContext('2d');
const logEl = document.getElementById('log');
ctx.textBaseline = 'middle';

const launcher = new Launcher(ROOT);
let running = null;          // node of the launched app, or null in the launcher
let nowPlaying = null;       // id of the audio app "playing in the background" (drives the equalizer)
let smoothY = 0;             // animated scroll offset (selected item index space)
let runTime = 0;             // seconds the running app has been "open"

// Control Center: a quick-settings sheet raised with Tab (mirrors the firmware overlay).
// Up/Down pick a row, Left/Right adjust it. Brightness dims a global scrim so its effect is
// visible here; volume is a value the audio apps read. The firmware also shows a live
// "Now Playing" row + network/PIN — represented here with placeholders for layout sign-off.
let ccOpen = false, ccSel = 0;
let ccBrightness = 100, ccVolume = 80;
const CC_ROWS = ['Brightness', 'Volume'];

function log(msg) { logEl.textContent = msg; }

// ---- low-level helpers (mirror M5GFX) ----
function rrPath(x, y, w, h, r) {
  r = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}
function roundRect(x, y, w, h, r, fill) { rrPath(x, y, w, h, r); ctx.fillStyle = fill; ctx.fill(); }
function circle(x, y, r, fill) { ctx.beginPath(); ctx.arc(x, y, r, 0, 7); ctx.fillStyle = fill; ctx.fill(); }
function text(s, x, y, color, px = 8, weight = 'normal', align = 'left') {
  ctx.fillStyle = color; ctx.textAlign = align;
  ctx.font = `${weight} ${px}px ui-monospace, "Segoe UI", system-ui, sans-serif`;
  ctx.fillText(s, x, y);
}
function clamp(s, max) { return s.length <= max ? s : s.slice(0, max - 1) + '…'; }
function measure(s, px = 8, weight = 'normal') { ctx.font = `${weight} ${px}px ui-monospace, "Segoe UI", system-ui, sans-serif`; return ctx.measureText(s).width; }
// Hard-truncate to the widest prefix that fits `w` px (mirrors firmware fit_text).
function fit(s, w, px = 8, weight = 'normal') { if (measure(s, px, weight) <= w) return s; for (let n = s.length - 1; n > 0; n--) { const t = s.slice(0, n); if (measure(t, px, weight) <= w) return t; } return ''; }
// Parse #rgb / #rrggbb -> [r,g,b]. Linear blend a->b by t (mirrors firmware mix565), returns rgb().
function rgb3(s) { s = s.replace('#', ''); if (s.length === 3) s = s.split('').map((c) => c + c).join(''); return [parseInt(s.slice(0, 2), 16), parseInt(s.slice(2, 4), 16), parseInt(s.slice(4, 6), 16)]; }
function mix(a, b, t) { const A = rgb3(a), B = rgb3(b); return `rgb(${Math.round(A[0] + (B[0] - A[0]) * t)},${Math.round(A[1] + (B[1] - A[1]) * t)},${Math.round(A[2] + (B[2] - A[2]) * t)})`; }
// Wi-Fi strength gauge: four rising bars, lit up to `lvl`. Mirrors firmware draw_wifi (19x9 box).
function wifiBars(x, y, lvl, sta) {
  const on = sta ? COL.green : COL.amber;
  for (let i = 0; i < 4; i++) { const bh = 2 + i * 2; ctx.fillStyle = i < lvl ? on : COL.line; ctx.fillRect(x + i * 5, y + 9 - bh, 3, bh); }
}

// ---- chrome ----
// Smartwatch face: a bold clock anchors the LEFT; date + network + signal pack into ONE
// right-aligned cluster next to the antenna (mirrors firmware launcher_render_status_bar).
function drawStatus(filter) {
  ctx.fillStyle = '#000'; ctx.fillRect(0, 0, W, STATUS);
  const t = new Date();

  if (!running && !filter && launcher.depth > 0) {       // inside a category: colour-chip breadcrumb
    const node = launcher.top.node;
    roundRect(2, 1, 14, 14, 3, node.color);
    drawIcon(iconGfx(ctx, '#0b1020', node.color), iconId({ id: node.id }), 9, 8, 5.5);
    text(clamp(node.label, 12), 20, STATUS / 2, COL.fg, 12, 'bold');
    let rx = W - 4;
    wifiBars(rx - 16, 4, RSSI_LVL, true); rx -= 16 + 6;
    const cnt = (node.items || []).length;
    if (cnt) text(cnt + ' app', rx, STATUS / 2, COL.muted, 8, 'normal', 'right');
    ctx.strokeStyle = COL.line; ctx.beginPath(); ctx.moveTo(0, STATUS - .5); ctx.lineTo(W, STATUS - .5); ctx.stroke();
    return;
  }

  const hhmm = `${pad2(t.getHours())}:${pad2(t.getMinutes())}`;
  text(hhmm, 6, STATUS / 2, COL.fg, 12, 'bold');
  const clockR = 6 + measure(hhmm, 12, 'bold');

  if (!filter) {
    let rx = W - 4;
    wifiBars(rx - 16, 4, RSSI_LVL, true); rx -= 16 + 6;
    const nw = measure(NET, 8);
    if (rx - nw > clockR + 8) { text(NET, rx, STATUS / 2, COL.muted, 8, 'normal', 'right'); rx -= nw + 8; }
    const dt = `${IT_WD[t.getDay()]} ${t.getDate()} ${IT_MO[t.getMonth()]}`;
    const dw = measure(dt, 8);
    if (rx - dw > clockR + 6) text(dt, rx, STATUS / 2, COL.amber, 8, 'normal', 'right');
  } else {
    const fb = '/' + filter, fw = measure(fb, 8) + 8;
    roundRect(W - fw - 2, 1, fw, STATUS - 3, 3, '#19264a');
    text(fb, W - 5, STATUS / 2, COL.green, 8, 'normal', 'right');
  }
  ctx.strokeStyle = COL.line; ctx.beginPath(); ctx.moveTo(0, STATUS - .5); ctx.lineTo(W, STATUS - .5); ctx.stroke();
}

function drawHint(s) {
  const y = H - HINT;
  ctx.fillStyle = '#000'; ctx.fillRect(0, y, W, HINT);
  ctx.strokeStyle = COL.line; ctx.beginPath(); ctx.moveTo(0, y + .5); ctx.lineTo(W, y + .5); ctx.stroke();
  text(clamp(s.hint, 44), W / 2, y + HINT / 2, COL.muted, 8, 'normal', 'center');
}

// ---- Control Center overlay (interactive quick settings) ----
function ccMeter(x, y, w, val, col) {
  const segs = 10, gap = 2, sw = (w - (segs - 1) * gap) / segs, on = Math.round(val / 100 * segs);
  for (let i = 0; i < segs; i++) roundRect(x + i * (sw + gap), y, sw, 6, 1, i < on ? col : '#243250');
}

function drawControlCenter() {
  if (ccBrightness < 100) { ctx.save(); ctx.globalAlpha = (1 - ccBrightness / 100) * 0.8; ctx.fillStyle = '#000'; ctx.fillRect(0, 0, W, H); ctx.restore(); }
  const CX = 6, CY = 6, CW = W - 12, CH = H - 12;
  roundRect(CX, CY, CW, CH, 8, '#0c1426'); ctx.strokeStyle = '#4d1fff'; ctx.strokeRect(CX + .5, CY + .5, CW - 1, CH - 1);

  text('Control Center', CX + 8, CY + 11, COL.fg, 9, 'bold');
  const t = new Date();
  text(`${String(t.getHours()).padStart(2, '0')}:${String(t.getMinutes()).padStart(2, '0')}`, CX + CW - 8, CY + 11, COL.muted, 8, 'normal', 'right');
  ctx.strokeStyle = COL.line; ctx.beginPath(); ctx.moveTo(CX + 8, CY + 18); ctx.lineTo(CX + CW - 8, CY + 18); ctx.stroke();

  const vals = [ccBrightness, ccVolume], cols = ['#ffd166', '#7CFC9A'];
  CC_ROWS.forEach((label, i) => {
    const y = CY + 22 + i * 22, focus = i === ccSel;
    if (focus) roundRect(CX + 4, y, CW - 8, 19, 4, '#19264a');
    text(label, 20, y + 11, focus ? COL.fg : COL.muted, 8);
    ccMeter(96, y + 7, 100, vals[i], cols[i]);
    text(vals[i] + '%', 204, y + 11, focus ? COL.fg : COL.muted, 8);
  });

  const fy = CY + CH - 26;
  text('@ NucleoOS  192.168.4.1', CX + 8, fy, '#7CFC9A', 8);
  text('PIN 689614', CX + 8, fy + 10, '#ffd166', 8);
  text('scroll · < > adjust · tab close', CX + 8, CY + CH - 6, COL.muted, 7);
}

function ccKey(e) {
  if (e.key === 'Tab' || e.key === 'Escape' || e.key === '`') { e.preventDefault(); ccOpen = false; return; }
  if (e.key === ';' || e.key === 'ArrowUp')   { e.preventDefault(); ccSel = (ccSel + CC_ROWS.length - 1) % CC_ROWS.length; return; }
  if (e.key === '.' || e.key === 'ArrowDown') { e.preventDefault(); ccSel = (ccSel + 1) % CC_ROWS.length; return; }
  const d = (e.key === '/' || e.key === 'ArrowRight') ? 10 : (e.key === ',' || e.key === 'ArrowLeft') ? -10 : 0;
  if (d) {
    e.preventDefault();
    if (ccSel === 0) ccBrightness = Math.max(10, Math.min(100, ccBrightness + d));
    else ccVolume = Math.max(0, Math.min(100, ccVolume + d));
  }
}

// ---- launcher (horizontal icon carousel, smartwatch idiom) ----
// Three icons in a row: the centered one is the big, bright SELECTED app/category; its
// neighbours peek in smaller and dimmer on each side, so the focus reads instantly. The
// title sits below the icon in a large font; a secondary line gives the description (apps)
// or the item count (categories); a dot rail marks the position. The whole rail slides
// horizontally (eased via smoothY) when you move. Mirrors firmware draw_list.
const CAR_SLOT = 84;          // horizontal spacing between adjacent icons
const CAR_R_C  = 32;          // centered badge radius (fills the band top-down)
const CAR_R_S  = 19;          // side badge radius (neighbours stay prominent)

function carItemColor(it, t) { return mix(COL.line, it.color, 0.40 + 0.60 * t); }

// Count "rosette": a dark pill with an accent rim + white number, straddling the icon's top-right
// corner (notification-badge idiom). Replaces the "N app" subtitle row for categories.
function drawBadge(cx, cy, count, accent) {
  const s = String(count), h = 17, w = Math.max(17, measure(s, 12, 'bold') + 11);
  roundRect(cx - w / 2, cy - h / 2, w, h, h / 2, accent);                       // accent rim
  roundRect(cx - w / 2 + 1.5, cy - h / 2 + 1.5, w - 3, h - 3, (h - 3) / 2, '#0b1020');  // dark fill
  text(s, cx, cy + 0.5, '#ffffff', 12, 'bold', 'center');
}

// Now-playing equalizer: dark pill + 3 dancing green bars, tucked in the icon's bottom-right corner.
function drawEq(cx, cy, r, ph) {
  const w = r * 0.55, h = w * 0.72, base = cy + h / 2 - 2, step = (w - 4) / 3;
  roundRect(cx - w / 2, cy - h / 2, w, h, h / 3, '#0b1020');
  ctx.fillStyle = COL.green;
  for (let i = 0; i < 3; i++) {
    const bh = h * (0.3 + 0.5 * Math.abs(Math.sin(ph * 0.006 + i * 1.4)));
    ctx.fillRect(cx - w / 2 + 3 + i * step, base - bh, 2, bh);
  }
}

function drawDots(items, sel, cx, y, accent) {
  const n = items.length;
  if (n <= 1) return;
  if (n > 13) { text(`${sel + 1} / ${n}`, cx, y, COL.muted, 8, 'normal', 'center'); return; }
  const gap = 10, x0 = cx - (n - 1) * gap / 2;
  for (let i = 0; i < n; i++) {
    const dx = x0 + i * gap;
    if (i === sel) roundRect(dx - 3.5, y - 1.5, 7, 3, 1.5, accent);   // active = capsule "you are here"
    else circle(dx, y, 1.5, COL.dim);                                  // inactive = dot
  }
}

function drawLauncher() {
  const s = launcher.screen();
  ctx.fillStyle = COL.bg; ctx.fillRect(0, STATUS, W, H - STATUS - HINT);

  const bandTop = STATUS, bandH = H - HINT - STATUS;       // 16 .. 121  (105 px tall)
  if (!s.items.length) {
    text('Nessuna app', W / 2, bandTop + bandH / 2, COL.dim, 12, 'normal', 'center');
    return s;
  }

  const n = s.items.length, cx = W / 2;
  const iconCY = bandTop + 34;                              // icon-row centre, pushed up to use the band top
  const wrap = n >= 3;                                      // 3+ items -> infinite rail (neighbours wrap around)
  const pos = wrap ? smoothY : Math.max(0, Math.min(n - 1, smoothY));   // continuous carousel index (eased)
  // Signed distance from the focus to item i; for a wrapping rail, the NEAREST copy (so the last item
  // peeks to the left of the first, and the carousel is never visually "empty" on one side).
  const cd = (i) => { let d = i - pos; if (wrap) d -= n * Math.round(d / n); return d; };

  // Draw far icons first so the centred one always lands on top, even mid-slide.
  const slots = [];
  for (let i = 0; i < n; i++) if (Math.abs(cd(i)) <= 1.8) slots.push(i);
  slots.sort((a, b) => Math.abs(cd(b)) - Math.abs(cd(a)));

  ctx.save(); ctx.beginPath(); ctx.rect(0, bandTop, W, bandH); ctx.clip();
  for (const i of slots) {
    const it = s.items[i], d = cd(i);
    const t = Math.max(0, 1 - Math.abs(d));                 // 1 centred, 0 a full slot away
    const x = cx + d * CAR_SLOT;
    const r = CAR_R_S + (CAR_R_C - CAR_R_S) * t;
    const cy = iconCY + (1 - t) * 6;                        // neighbours drop a touch -> arc/depth
    const cr = r * 0.34, badge = carItemColor(it, t);      // rounded-square corner + badge fill
    if (t > 0.6) {                                          // soft square halo behind the focus
      ctx.globalAlpha = (t - 0.6) / 0.4; roundRect(x - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8, cr + 3, mix(COL.bg, it.color, 0.5)); ctx.globalAlpha = 1;
    }
    roundRect(x - r, cy - r, 2 * r, 2 * r, cr, badge);      // square icon badge (bigger than a disc)
    if (t > 0.85) {                                         // crisp square accent ring
      ctx.lineWidth = 2; ctx.strokeStyle = mix(it.color, '#ffffff', 0.4);
      rrPath(x - r - 2.5, cy - r - 2.5, 2 * r + 5, 2 * r + 5, cr + 2); ctx.stroke();
    }
    const gs = r * 0.68, ink = t > 0.5 ? '#0b1020' : mix(COL.bg, '#ffffff', 0.62);
    if (!drawIcon(iconGfx(ctx, ink, badge), iconId(it), x, cy, gs))     // bold vector icon (ink + badge cut-outs)
      text(it.icon, x, cy, ink, Math.round(gs * 1.3), 'bold', 'center'); // fallback: the unicode glyph
    if (it.type === 'menu' && t > 0.85)                    // category: count rosette on the top-right corner
      drawBadge(x + r * 0.82, cy - r * 0.82, (it.items || []).length, it.color);
    if (it.id === nowPlaying) drawEq(x + r * 0.6, cy + r * 0.6, r * 0.55, performance.now());   // now playing
  }
  ctx.restore();

  // Title only — big and centred. The category count moved to the icon rosette, so no subtitle row
  // steals vertical space; the title gets the freed band and a roomy block of its own.
  const cur = s.items[s.sel];
  const tf = measure(cur.label, 22, 'bold') > W - 20 ? 14 : 22;   // shrink long names instead of overflowing
  text(fit(cur.label, W - 12, tf, 'bold'), cx, bandTop + 84, COL.fg, tf, 'bold', 'center');

  drawDots(s.items, s.sel, cx, bandTop + 102, cur.color);
  return s;
}

// Drawing bundle handed to native apps so they don't touch canvas plumbing — the
// primitives map 1:1 to M5GFX, like the firmware. (See apps/calculator.js.)
const G = { text, roundRect, circle, clamp, W, H, COL, contentTop: CONTENT_TOP, contentH: CONTENT_H, ctx, now: 0 };

// ---- a launched app (native view if registered, else placeholder) ----
function drawRunningApp() {
  G.now = performance.now();                 // animation clock for marquee/auto-scroll
  ctx.fillStyle = COL.bg; ctx.fillRect(0, STATUS, W, H - STATUS - HINT);
  const app = APPS[running.id];
  if (app?.draw) return app.draw(G) || { instruction: running.desc, hint: 'esc · back' };

  circle(W / 2, 52, 16, running.color);
  text(running.icon, W / 2, 52, '#000', 16, 'bold', 'center');
  text(clamp(running.label, 22), W / 2, 78, COL.fg, 11, 'bold', 'center');
  text('running · ' + runTime + 's', W / 2, 92, COL.muted, 8, 'normal', 'center');
  text('Open in the web companion for now', W / 2, 108, COL.dim, 8, 'normal', 'center');
  return { instruction: running.desc, hint: 'esc · back to launcher' };
}

// ---- frame ----
function render() {
  drawStatus(running ? '' : launcher.top.filter);
  const s = running ? drawRunningApp() : drawLauncher();
  const full = s && s.fullscreen;
  // A fullscreen app (e.g. Video playback) owns the entire panel — it has already painted
  // over the status bar, so skip the hint chrome that would overlay its bottom.
  if (!full) drawHint(s);
  // Global brightness scrim from the Control Center (the video app dims itself). When the
  // sheet is open it paints its own scrim + panel on top.
  if (ccBrightness < 100 && !full && !ccOpen) {
    ctx.save(); ctx.globalAlpha = (1 - ccBrightness / 100) * 0.8; ctx.fillStyle = '#000'; ctx.fillRect(0, 0, W, H); ctx.restore();
  }
  if (ccOpen) drawControlCenter();
}

function animate() {
  if (!running) {
    const n = launcher.visibleItems().length, target = launcher.top.sel;
    if (n >= 3) {                                  // circular rail: ease along the SHORTEST way round
      let d = target - smoothY; d -= n * Math.round(d / n);
      if (Math.abs(d) < 0.01) smoothY = target;
      else smoothY = (((smoothY + d * 0.3) % n) + n) % n;
    } else {                                        // 1-2 items: plain ease, no wrap
      const gap = Math.abs(smoothY - target);
      if (gap > 0.01) smoothY += (target - smoothY) * 0.3; else smoothY = target;
    }
  }
  render();
  requestAnimationFrame(animate);
}

// running-app clock
setInterval(() => { if (running) { runTime++; } }, 1000);

// ---- keyboard ----
// Horizontal carousel: Left/Right step the rail (prev/next); ; . and , keep working as the
// Cardputer scroll keys. Enter opens, Esc/` go back, `/` still raises an app's options.
const MAP = {
  ';': 'up', '.': 'down', ',': 'up',
  'ArrowUp': 'up', 'ArrowDown': 'down', 'ArrowLeft': 'up', 'ArrowRight': 'down',
  'Enter': 'enter', '/': 'context',
  'Escape': 'back', '`': 'back', 'Backspace': 'backspace',
};

function onLaunch(node) {
  running = node; runTime = 0; smoothY = launcher.top.sel;
  if (node.id === 'music' || node.id === 'radio') nowPlaying = node.id;   // mock background playback
  APPS[node.id]?.enter?.();
  log(`launch → ${node.id}`);
}
if (typeof window !== 'undefined') window.__play = (id) => { nowPlaying = id; };   // dev hook to preview the equalizer

function onAction(action, node) {
  log(`action → ${action.id} on ${node.id}`);
  launcher.back();                  // close the context submenu
  if (action.id === 'open') onLaunch(node);
}

window.addEventListener('keydown', (e) => {
  // Control Center owns the keyboard while it is up.
  if (ccOpen) { ccKey(e); return; }
  // in a launched app, Back returns to the launcher; other keys go to the app
  if (running) {
    if (['Escape', '`', 'ArrowLeft'].includes(e.key)) { e.preventDefault(); log(`exit ← ${running.id}`); APPS[running.id]?.exit?.(); running = null; return; }
    const app = APPS[running.id];
    if (app?.key) {
      // Translate to the firmware's NK_* semantics so sim apps and app_*.cpp share
      // the same key contract. The Cardputer maps ; . / to up/down/right but still
      // carries the char (so e.g. the calculator reads '.' and '/' from `ch`).
      let k = 'char', ch = null;
      if (e.key === 'Enter') k = 'enter';
      else if (e.key === 'Backspace') k = 'backspace';
      else if (e.key === 'Tab') { k = 'tab'; ch = '\t'; }
      else if (e.key === ';' || e.key === 'ArrowUp') { k = 'up'; ch = ';'; }
      else if (e.key === '.' || e.key === 'ArrowDown') { k = 'down'; ch = '.'; }
      else if (e.key === '/' || e.key === 'ArrowRight') { k = 'right'; ch = '/'; }
      else if (e.key.length === 1) ch = e.key;
      else return;
      e.preventDefault(); app.key(k, ch);
    }
    return;
  }
  // Tab raises the Control Center from the launcher (the firmware can raise it from anywhere;
  // here we keep it out of the running apps so it never fights an app's own Tab).
  if (e.key === 'Tab') { e.preventDefault(); ccOpen = true; ccSel = 0; return; }
  let key = MAP[e.key];
  if (!key) {
    // Any printable character — digits included — types into the filter. No numeric
    // quick-select (it used to hijack digits meant for the filter).
    if (e.key.length === 1 && /[a-zA-Z0-9 ]/.test(e.key)) key = e.key.toLowerCase();
    else return;
  }
  e.preventDefault();
  const d0 = launcher.depth;
  const ev = handleKey(launcher, key);
  if (launcher.depth !== d0) smoothY = launcher.top.sel;   // level change: snap the rail, don't sweep
  if (ev?.type === 'launch') onLaunch(ev.node);
  else if (ev?.type === 'action') onAction(ev.action, ev.node);
});

cv.setAttribute('tabindex', '0');
animate();

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
function roundRect(x, y, w, h, r, fill) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
  ctx.fillStyle = fill; ctx.fill();
}
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

// ---- launcher (Wear-OS hero carousel) ----
// The selected app is a tall HERO card (icon + bold title + its description, right in the card);
// neighbours flow above/below as slim fading rows. Mirrors firmware draw_list.
function drawLauncher() {
  const s = launcher.screen();
  ctx.fillStyle = COL.bg; ctx.fillRect(0, STATUS, W, H - STATUS - HINT);

  const bandTop = STATUS, bandH = H - HINT - STATUS;
  if (!s.items.length) {
    text('Nessuna app', W / 2, bandTop + bandH / 2, COL.dim, 11, 'normal', 'center');
    return s;
  }

  const HERO_H = 38, ROW_H = 19;
  const cy = bandTop + bandH / 2, heroTop = cy - HERO_H / 2;

  ctx.save(); ctx.beginPath(); ctx.rect(0, bandTop, W, bandH); ctx.clip();
  for (let i = 0; i < s.items.length; i++) {
    const it = s.items[i], dist = i - s.sel;
    let y0, rh;
    if (dist === 0) { y0 = heroTop; rh = HERO_H; }
    else if (dist < 0) { rh = ROW_H; y0 = heroTop - (-dist) * ROW_H; }
    else { rh = ROW_H; y0 = heroTop + HERO_H + (dist - 1) * ROW_H; }
    if (y0 + rh <= bandTop || y0 >= bandTop + bandH) continue;

    if (dist === 0) {
      roundRect(6, y0, W - 12, HERO_H, 11, it.color);
      const bcy = y0 + HERO_H / 2;
      circle(24, bcy, 13, '#000');
      text(it.icon, 24, bcy, it.color, 12, 'bold', 'center');
      // right affordance: submenu shows "N ›", apps a lone chevron
      let rx = W - 16;
      if (it.type === 'menu') {
        const cnt = (it.items || []).length;
        text('›', W - 18, bcy, '#000', 13, 'bold', 'center');
        if (cnt) { const cw = measure(String(cnt), 8); text(String(cnt), W - 26 - cw, bcy, '#000', 8); rx = W - 26 - cw - 4; }
        else rx = W - 24;
      } else { text('›', W - 14, bcy, '#000', 9, 'bold', 'center'); rx = W - 18; }

      const lx = 44, availw = rx - lx;
      if (it.desc) {
        text(fit(it.label, availw, 11, 'bold'), lx, y0 + 12, '#000', 11, 'bold');
        text(fit(it.desc, availw, 8), lx, y0 + 28, '#000a', 8);
      } else {
        text(fit(it.label, availw, 11, 'bold'), lx, bcy, '#000', 11, 'bold');
      }
    } else {
      const near = Math.abs(dist) === 1, ny = y0 + ROW_H / 2;
      circle(24, ny, near ? 4 : 3, near ? it.color : COL.dim);
      text(fit(it.label, W - 48, 8), 36, ny, near ? COL.fg : COL.dim, 8);
    }
  }
  ctx.restore();

  // scroll indicator on the right edge
  if (s.items.length > 3) {
    const trackH = bandH - 10, kh = Math.max(10, trackH / s.items.length);
    const ky = bandTop + 5 + (trackH - kh) * (s.sel / (s.items.length - 1));
    roundRect(W - 4, bandTop + 5, 3, trackH, 1, COL.line);
    roundRect(W - 4, ky, 3, kh, 1, s.color);
  }
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
    const target = launcher.top.sel;
    if (Math.abs(smoothY - target) > 0.01) smoothY += (target - smoothY) * 0.35;
    else smoothY = target;
  }
  render();
  requestAnimationFrame(animate);
}

// running-app clock
setInterval(() => { if (running) { runTime++; } }, 1000);

// ---- keyboard ----
const MAP = {
  ';': 'up', '.': 'down', 'ArrowUp': 'up', 'ArrowDown': 'down',
  'Enter': 'enter', '/': 'context', 'ArrowRight': 'context',
  'Escape': 'back', '`': 'back', 'ArrowLeft': 'back', 'Backspace': 'backspace',
};

function onLaunch(node) {
  running = node; runTime = 0; smoothY = launcher.top.sel;
  APPS[node.id]?.enter?.();
  log(`launch → ${node.id}`);
}

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
  const ev = handleKey(launcher, key);
  if (ev?.type === 'launch') onLaunch(ev.node);
  else if (ev?.type === 'action') onAction(ev.action, ev.node);
});

cv.setAttribute('tabindex', '0');
animate();

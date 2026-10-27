// Video — device-simulator app. Faithful mirror of firmware app_video.cpp: browse
// /data/Videos for .nfv v4 clips (whole-frame JPEGs + IMA ADPCM sound in one file) and play them on the screen.
// The decoding is the web Video Player's NfvMedia (apps/video-player/www/nfv4.js): JPEGs onto a canvas, the
// ADPCM through Web Audio; the device does the same with esp_new_jpeg + the I2S. Older .nfv (v1-v3) no longer
// play, as on the device. The whole control surface is exercised here so the UX is signed off before it ships.
//
// Controls (identical on device): enter/space play-pause · , / seek 10s · ; . volume ·
// [ ] brightness · 0-9 jump to % · TAB hold controls · esc stop (saves resume position).
import { makeListState, drawFocusList } from './_list.js';
import { NfvMedia } from '/apps/video-player/nfv4.js';
const lst = makeListState();

const DIR = '/data/Videos';
const NFV_RE = /\.nfv$/i;
const ACCENT = '#4ea1ff', GOOD = '#7CFC9A', WARM = '#ffd166';

let names = [];
let sel = 0;

// --- playback state -----------------------------------------------------------
let mode = 'list';                 // 'list' | 'resume' | 'play'
let clip = null;                   // the open clip's header (NfvMedia.h) once loaded
const frame = document.createElement('canvas');                // NfvMedia paints the picture here
const audio = new NfvMedia(frame);                             // same surface as <audio>: play/pause/currentTime/volume
let overlayUntil = 0;              // controller auto-hides at this ts (ms); Infinity = pinned
let brightness = 100;             // 0..100 (device: panel backlight; here: a dark scrim)
let loadingName = '', playingName = '';
let resumeSel = 0, resumeSec = 0, resumeIdx = -1;   // Netflix-style resume sheet state

const nowMs = () => performance.now();
const fmt = s => `${(s / 60) | 0}:${String((s | 0) % 60).padStart(2, '0')}`;
const clampi = (v, a, b) => Math.max(a, Math.min(b, v));

// --- resume store ("Continue watching"): per-clip seconds in localStorage. We keep a
// position only when stopped part-way, and clear it once a clip is finished — so reopening
// offers a sensible Resume / Start-over choice, mirroring firmware app_video.cpp's .nfv-resume.
const RESUME_KEY = 'nucleo.video.resume';
function resumeAll() { try { return JSON.parse(localStorage.getItem(RESUME_KEY) || '{}'); } catch { return {}; } }
function resumeLoad(name) { return resumeAll()[name] || 0; }
function resumeSave(name, sec, total) {
  const m = resumeAll();
  if (sec >= 5 && !(total && sec + 5 >= total)) m[name] = Math.round(sec);
  else delete m[name];
  try { localStorage.setItem(RESUME_KEY, JSON.stringify(m)); } catch { /* ignore quota */ }
}

async function scan() {
  names = []; sel = 0;
  try {
    const r = await (await fetch('/api/fs/list?path=' + encodeURIComponent(DIR))).json();
    names = (r.entries || []).filter(e => e.type === 'file' && NFV_RE.test(e.name))
      .map(e => e.name).sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }));
  } catch { names = []; }
}

let playingNfv = '';               // file name of the clip currently open (for resume saving)

async function open(i, startSec = 0) {
  if (i < 0 || i >= names.length) return;
  loadingName = names[i]; playingName = names[i].replace(/\.nfv$/i, ''); playingNfv = names[i];
  mode = 'play'; clip = null;
  overlayUntil = nowMs() + 2500;
  await audio.open('/api/fs/read?path=' + encodeURIComponent(DIR + '/' + names[i]));
  if (audio.error) {
    loadingName = audio.error.code === 'NFV_OLD' ? `Old format (v${audio.error.version}): convert it again` : 'Error: ' + audio.error.message;
    return;
  }
  clip = audio.h;
  if (startSec > 0) audio.currentTime = startSec;
  audio.play().catch(() => {});
}

function curSec() { return audio.currentTime || 0; }
function totalSec() { return clip ? clip.durationMs / 1000 : 0; }

function stop() {
  if (playingNfv) resumeSave(playingNfv, curSec(), totalSec());   // remember where we stopped
  audio.close(); mode = 'list'; clip = null; playingNfv = '';
}

// Clip played to the end -> clear its resume mark and fall back to the list.
audio.addEventListener('ended', () => {
  if (playingNfv) { resumeSave(playingNfv, totalSec(), totalSec()); playingNfv = ''; }
  audio.close(); mode = 'list'; clip = null;
});

function bumpOverlay() { if (overlayUntil !== Infinity) overlayUntil = nowMs() + 3500; }

export const videoApp = {
  id: 'video-player',
  enter() { scan(); mode = 'list'; return { hint: 'scroll  enter play  esc back' }; },
  exit() { stop(); },

  key(key, ch) {
    if (mode === 'list') {
      if (key === 'up') { if (names.length) sel = (sel + names.length - 1) % names.length; }
      else if (key === 'down') { if (names.length) sel = (sel + 1) % names.length; }
      else if (key === 'enter' && names.length) tryOpen(sel);
      return;
    }
    if (mode === 'resume') {                                   // Netflix-style Resume / Start over
      if (key === 'up' || key === 'down' || key === 'left' || key === 'right' || ch === ',' || ch === '/') resumeSel ^= 1;
      else if (key === 'enter') { const i = resumeIdx; mode = 'list'; open(i, resumeSel === 0 ? resumeSec : 0); }
      else if (key === 'backspace') mode = 'list';
      return;
    }
    // playback controls (stop checked first, mirroring the firmware's NK_BACK/NK_DEL guard)
    const dur = totalSec();
    const isDigit = typeof ch === 'string' && ch >= '0' && ch <= '9';
    if (key === 'backspace') { stop(); }
    else if (key === 'enter' || ch === ' ' || ch === 'p') { audio.paused ? audio.play().catch(() => {}) : audio.pause(); bumpOverlay(); }
    else if (key === 'tab' || ch === '\t') { overlayUntil = overlayUntil === Infinity ? nowMs() + 250 : Infinity; }
    else if (key === 'right' || ch === '/') { audio.currentTime = clampi(curSec() + 10, 0, dur || 1e9); bumpOverlay(); }   // seek +10s
    else if (ch === ',') { audio.currentTime = clampi(curSec() - 10, 0, dur || 1e9); bumpOverlay(); }                       // seek -10s
    else if (key === 'up' || ch === ';') { audio.volume = clampi(Math.round((audio.volume + 0.1) * 10) / 10, 0, 1); bumpOverlay(); }
    else if (key === 'down' || ch === '.') { audio.volume = clampi(Math.round((audio.volume - 0.1) * 10) / 10, 0, 1); bumpOverlay(); }
    else if (ch === ']') { brightness = clampi(brightness + 10, 10, 100); bumpOverlay(); }
    else if (ch === '[') { brightness = clampi(brightness - 10, 10, 100); bumpOverlay(); }
    else if (isDigit && dur) { audio.currentTime = clampi((+ch) * dur / 10, 0, dur); bumpOverlay(); }                       // jump 0..90%
  },

  draw(g) { return mode === 'list' ? drawList(g) : mode === 'resume' ? drawResume(g) : drawPlayer(g); },
};

// Enter on a clip: if it has a saved position, raise the resume sheet; otherwise play from 0.
function tryOpen(i) {
  const r = resumeLoad(names[i]);
  if (r > 0) { mode = 'resume'; resumeIdx = i; resumeSec = r; resumeSel = 0; }
  else open(i, 0);
}

// Netflix-style resume sheet: Resume where you left off, or Start over.
function drawResume(g) {
  const top = g.contentTop;
  g.text('Resume playback?', 12, top + 9, ACCENT, 9, 'bold');
  g.text(g.clamp(names[resumeIdx].replace(/\.nfv$/i, ''), 32), 12, top + 24, g.COL.fg, 9);
  g.text('Stopped at ' + fmt(resumeSec), 12, top + 38, g.COL.muted, 8);
  ['Resume', 'Start over'].forEach((o, b) => {
    const bx = 12 + b * 112, by = top + 52;
    g.roundRect(bx, by, 104, 22, 6, resumeSel === b ? ACCENT : '#16203a');
    g.text(o, bx + 52, by + 11, resumeSel === b ? '#04122b' : g.COL.fg, 9, 'bold', 'center');
  });
  return { instruction: 'Resume where you left off', hint: '< > select · enter ok · esc back' };
}

function drawList(g) {
  const top = g.contentTop, h = g.contentH;
  g.text('Video', 10, top + 9, ACCENT, 11, 'bold');
  g.text(`${names.length} clip${names.length === 1 ? '' : 's'}`, g.W - 8, top + 9, g.COL.muted, 8, 'normal', 'right');
  if (!names.length) {
    g.text('No .nfv clips in /data/Videos', 12, top + 40, g.COL.dim, 9);
    g.text('Convert with Video Studio (web app)', 12, top + 54, g.COL.dim, 8);
    return { instruction: 'Play .nfv clips on the screen', hint: 'esc back' };
  }
  drawFocusList(g, lst, {
    top: top + 20, h: h - 20, count: names.length, sel, now: g.now,
    label: i => names[i].replace(/\.nfv$/i, ''), color: () => ACCENT,
    marked: i => resumeLoad(names[i]) > 0,                       // ▶ Continue watching
    right: i => { const r = resumeLoad(names[i]); return r ? fmt(r) : ''; },
  });
  return { instruction: 'Play .nfv clips on the screen', hint: 'scroll  enter play  esc back' };
}

function drawPlayer(g) {
  const ctx = g.ctx, W = g.W, H = g.H;
  ctx.fillStyle = '#000'; ctx.fillRect(0, 0, W, H);

  if (!clip) {
    g.text(audio.error ? loadingName : 'Loading…', W / 2, H / 2, ACCENT, 10, 'bold', 'center');
    return { fullscreen: true };
  }

  // current frame, cover-fit (fills screen, no bars — the device crops a 320x240 clip the same way)
  const s = Math.max(W / frame.width, H / frame.height);
  ctx.drawImage(frame, (W - frame.width * s) / 2, (H - frame.height * s) / 2, frame.width * s, frame.height * s);
  // brightness scrim (device dims the backlight; here we darken)
  if (brightness < 100) { ctx.save(); ctx.globalAlpha = (1 - brightness / 100) * 0.85; ctx.fillStyle = '#000'; ctx.fillRect(0, 0, W, H); ctx.restore(); }

  if (nowMs() < overlayUntil) drawController(g);
  return { fullscreen: true };
}

// Auto-hiding bottom controller: title row, progress, and a row with play state, time,
// volume and brightness meters. One cohesive panel (kept to the bottom so the firmware can
// clip the video above it and never overdraw -> no flicker on the real panel).
function drawController(g) {
  const ctx = g.ctx, W = g.W, H = g.H, BARH = 42, y = H - BARH;
  ctx.save();
  ctx.globalAlpha = 0.9; ctx.fillStyle = '#060912'; ctx.fillRect(0, y, W, BARH); ctx.globalAlpha = 1;
  ctx.strokeStyle = '#243250'; ctx.beginPath(); ctx.moveTo(0, y + .5); ctx.lineTo(W, y + .5); ctx.stroke();

  const paused = audio.paused;
  const el = audio.currentTime || 0;
  const du = totalSec();

  // Row 1: play chip + title (marquee) + time
  g.roundRect(6, y + 5, 16, 16, 4, paused ? '#1f2c4a' : ACCENT);
  g.text(paused ? '▶' : '❚❚', 14, y + 13, paused ? '#cfe0ff' : '#04122b', 8, 'bold', 'center');
  marquee(g, 28, y + 9, W - 28 - 78, playingName, '#eaf2ff', g.now);
  g.text(`${fmt(el)} / ${fmt(du)}`, W - 6, y + 9, g.COL.muted, 8, 'normal', 'right');

  // Row 2: progress with playhead
  const px = 6, pw = W - 12, py = y + 22;
  g.roundRect(px, py, pw, 3, 1, '#243250');
  const pct = du ? Math.min(1, el / du) : 0;
  if (pct > 0) g.roundRect(px, py, pw * pct, 3, 1, ACCENT);
  g.circle(px + pw * pct, py + 1, 3, '#fff');

  // Row 3: volume + brightness meters (both fit within 240px)
  meter(g, 6, y + 30, 'VOL', Math.round(audio.volume * 100), GOOD);
  meter(g, 122, y + 30, 'LUM', brightness, WARM);
  ctx.restore();
}

// 6-segment meter with a label and value, ~96px wide.
function meter(g, x, y, label, val, col) {
  g.text(label, x, y + 4, g.COL.muted, 7, 'bold');
  const bx = x + 22, segs = 6, sw = 8, gap = 2, on = Math.round(val / 100 * segs);
  for (let i = 0; i < segs; i++) g.roundRect(bx + i * (sw + gap), y, sw, 8, 1, i < on ? col : '#1b2740');
  g.text(val + '', bx + segs * (sw + gap) + 3, y + 4, g.COL.muted, 7, 'bold');
}

// Horizontal scrolling text when it doesn't fit (firmware mirrors this clip behaviour).
function marquee(g, x, y, w, text, col, now) {
  const tw = (text || '').length * 6;
  if (tw <= w) { g.text(text, x, y, col, 8, 'bold'); return; }
  const over = tw - w + 8, period = over * 22 + 1400;
  const t = (now % (period * 2));
  let off = t < 700 ? 0 : t < period ? (t - 700) / (period - 700) * over : t < period + 700 ? over : over - (t - period - 700) / (period - 700) * over;
  off = clampi(off, 0, over);
  g.ctx.save(); g.ctx.beginPath(); g.ctx.rect(x, y - 6, w, 12); g.ctx.clip();
  g.text(text, x - off, y, col, 8, 'bold');
  g.ctx.restore();
}

// Test/inspection hook (preview harness; not part of the app contract). Mirrors player.js.
export function _debugState() {
  return { mode, n: names.length, sel, frames: clip?.frames, t: Math.round((audio.currentTime || 0) * 10) / 10,
    vol: audio.volume, bri: brightness, paused: audio.paused, overlayPinned: overlayUntil === Infinity };
}

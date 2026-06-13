// Video — device-simulator app. Faithful mirror of firmware app_video.cpp: browse
// /data/Videos for .nfv clips (MJPEG 240x136 + sibling .mp3) and play them on the screen.
// The browser decodes JPEG frames + plays the mp3; the device does the same with M5GFX
// drawJpg + the Helix MP3 task. The whole control surface is exercised here so the UX is
// signed off before it ships.
//
// Controls (identical on device): enter/space play-pause · , / seek 10s · ; . volume ·
// [ ] brightness · 0-9 jump to % · TAB hold controls · esc stop (saves resume position).
import { makeListState, drawFocusList } from './_list.js';
const lst = makeListState();

const DIR = '/data/Videos';
const NFV_RE = /\.nfv$/i;
const ACCENT = '#4ea1ff', GOOD = '#7CFC9A', WARM = '#ffd166';

let names = [];
let sel = 0;

// --- playback state -----------------------------------------------------------
let mode = 'list';                 // 'list' | 'resume' | 'play'
let clip = null;                   // { fps, frameCount, durationMs, frames:[{off,size}], buf }
let frameIdx = -1, bitmap = null, decoding = false;
const audio = new Audio();
audio.preload = 'auto';
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

// Parse NFV1 into a frame offset/size index (see tools/nfv/encode.py).
function parseNfv(arrayBuf) {
  const dv = new DataView(arrayBuf);
  const magic = String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3));
  if (magic !== 'NFV1') throw new Error('not an NFV1 clip');
  const fps = dv.getUint16(10, true) || 12;
  const frameCount = dv.getUint32(14, true);
  const durationMs = dv.getUint32(18, true);
  const frames = [];
  let p = 32;
  for (let i = 0; i < frameCount && p + 4 <= dv.byteLength; i++) {
    const size = dv.getUint32(p, true); p += 4;
    frames.push({ off: p, size }); p += size;
  }
  return { fps, frameCount: frames.length, durationMs, frames, buf: new Uint8Array(arrayBuf) };
}

let playingNfv = '';               // file name of the clip currently open (for resume saving)

async function open(i, startSec = 0) {
  if (i < 0 || i >= names.length) return;
  loadingName = names[i]; playingName = names[i].replace(/\.nfv$/i, ''); playingNfv = names[i];
  mode = 'play'; clip = null; bitmap = null; frameIdx = -1;
  overlayUntil = nowMs() + 2500;
  try {
    const buf = await (await fetch('/api/fs/read?path=' + encodeURIComponent(DIR + '/' + names[i]))).arrayBuffer();
    clip = parseNfv(buf);
  } catch (e) { loadingName = 'Error: ' + e.message; return; }
  audio.src = '/api/fs/read?path=' + encodeURIComponent(DIR + '/' + names[i].replace(/\.nfv$/i, '.mp3'));
  const begin = () => { try { audio.currentTime = startSec || 0; } catch { /* not seekable yet */ } audio.play().catch(() => {}); };
  if (startSec > 0) audio.addEventListener('loadedmetadata', begin, { once: true });
  else { audio.currentTime = 0; audio.play().catch(() => {}); }
}

function curSec() { return audio.currentTime || 0; }
function totalSec() { return (clip && clip.durationMs ? clip.durationMs / 1000 : 0) || (isFinite(audio.duration) ? audio.duration : 0); }

function stop() {
  if (playingNfv) resumeSave(playingNfv, curSec(), totalSec());   // remember where we stopped
  audio.pause(); audio.src = ''; mode = 'list'; clip = null; bitmap = null; frameIdx = -1; playingNfv = '';
}

// Clip played to the end -> clear its resume mark and fall back to the list.
audio.addEventListener('ended', () => {
  if (playingNfv) { resumeSave(playingNfv, totalSec(), totalSec()); playingNfv = ''; }
  mode = 'list'; clip = null; bitmap = null; frameIdx = -1;
});

function targetFrame() {
  if (!clip) return 0;
  return clampi(Math.floor(audio.currentTime * clip.fps), 0, clip.frameCount - 1);
}
function ensureFrame(i) {
  if (!clip || decoding || i === frameIdx) return;
  decoding = true;
  const fr = clip.frames[i];
  createImageBitmap(new Blob([clip.buf.subarray(fr.off, fr.off + fr.size)], { type: 'image/jpeg' }))
    .then(bm => { if (bitmap) bitmap.close?.(); bitmap = bm; frameIdx = i; decoding = false; })
    .catch(() => { decoding = false; });
}
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
    g.text('Encode with tools/nfv/encode.py', 12, top + 54, g.COL.dim, 8);
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
    g.text(loadingName.startsWith('Error') ? loadingName : 'Loading…', W / 2, H / 2, ACCENT, 10, 'bold', 'center');
    return { fullscreen: true };
  }

  // current frame, cover-fit (fills screen, no bars)
  ensureFrame(targetFrame());
  if (bitmap) {
    const s = Math.max(W / bitmap.width, H / bitmap.height);
    ctx.drawImage(bitmap, (W - bitmap.width * s) / 2, (H - bitmap.height * s) / 2, bitmap.width * s, bitmap.height * s);
  }
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
  const du = (clip.durationMs || 0) / 1000 || (isFinite(audio.duration) ? audio.duration : 0);

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
  return { mode, n: names.length, sel, frame: frameIdx, frames: clip?.frameCount, t: Math.round((audio.currentTime || 0) * 10) / 10,
    vol: audio.volume, bri: brightness, paused: audio.paused, overlayPinned: overlayUntil === Infinity };
}

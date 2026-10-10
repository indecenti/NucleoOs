// stelle/hud.js — the pilot's interface: a resolution-independent canvas HUD drawn every frame plus a
// thin DOM layer (comms with portraits, mission card, banners, pause menu, touch controls), and all
// flight input (mouse aim, keyboard, gamepad, touch) written into the sim's F.input / cmd().
//
// HUD language: cyan-white instruments, red hostiles, green friendlies, gold objectives. Centre:
// reticle with heat and lock arcs, mouse-aim cursor, lead pip, hit markers, damage-direction arcs.
// Edges: target box, off-screen arrows, missile warnings. Bottom: throttle/speed with the sweet-spot
// band, power pips, dual-hemisphere radar, front/back shields + hull, missiles and flares. Top-left:
// the mission tracker. Five languages through the games catalog (cz_* keys).
import I18N from '/nucleo-i18n.js';
import { cmd, cycleSub, leadPoint, CLS, TEAM_P, TEAM_E, F_GILDA, F_CUSTODI, F_RELITTI, ST } from './sim.js';
import * as A from './assets.js';
import { flavorText, ACE_PORTRAIT } from '../constellations-ui.js';

const t = I18N.scope('games');
const tr = (k, v) => { const s = t(k, v); return s === k ? '' : s; };
const COL = { hud: '#bdf3ff', dim: 'rgba(189,243,255,0.45)', faint: 'rgba(189,243,255,0.18)', hostile: '#ff5b4a', friend: '#78f0a0', gold: '#ffd66b', warn: '#ffb347', shield: '#6fd8ff', hull: '#9cf5b0', bad: '#ff4d3d', eng: '#ffb35c', wpn: '#ff6b6b', shd: '#62c9ff' };
const FAC_COL = ['#7fb0ff', '#e8c069', '#ff8a3c', '#a98bff', '#ffd66b'];
const CONTACT = { [F_GILDA]: 'broker', [F_CUSTODI]: 'abbess', [F_RELITTI]: 'matriarch' };
const EMBLEM = ['emblem_gilda', 'emblem_custodi', 'emblem_relitti', 'emblem_eco'];

const CSS = `
.sh-root{position:absolute;inset:0;pointer-events:none;z-index:25;font-family:'Segoe UI',system-ui,sans-serif;color:#bdf3ff;user-select:none}
.sh-root canvas{position:absolute;inset:0;width:100%;height:100%;background:transparent!important;border:0!important;border-radius:0!important;box-shadow:none!important}
.sh-comms{position:absolute;left:18px;top:38%;width:min(420px,40vw);display:flex;flex-direction:column;gap:8px}
.sh-msg{display:flex;gap:10px;align-items:flex-start;padding:8px 12px 8px 8px;background:linear-gradient(90deg,rgba(6,14,24,.78),rgba(6,14,24,.35));border-left:2px solid var(--c,#bdf3ff);border-radius:4px;animation:shin .25s ease-out;transition:opacity .6s}
.sh-msg.out{opacity:0}
.sh-pt{flex:0 0 44px;height:44px;border-radius:4px;background:#0b1622 center/cover;border:1px solid rgba(189,243,255,.25);display:grid;place-items:center;font-weight:800;font-size:18px;color:var(--c)}
.sh-msg b{display:block;font-size:11px;letter-spacing:.14em;text-transform:uppercase;color:var(--c)}
.sh-msg span{font-size:14px;line-height:1.3;color:#e8f6ff}
@keyframes shin{from{opacity:0;transform:translateX(-14px)}to{opacity:1;transform:none}}
.sh-card{position:absolute;left:50%;top:18%;transform:translateX(-50%);width:min(620px,86vw);padding:20px 24px;background:linear-gradient(180deg,rgba(6,12,22,.86),rgba(6,12,22,.62));border:1px solid rgba(189,243,255,.22);border-radius:10px;text-align:center;transition:opacity .8s,transform .8s;overflow:hidden}
.sh-card.out{opacity:0;transform:translateX(-50%) translateY(-12px)}
.sh-card .art{position:absolute;inset:0;background:center/cover;opacity:.28}
.sh-card .k{position:relative;font-size:11px;letter-spacing:.3em;color:#ffd66b;text-transform:uppercase}
.sh-card h2{position:relative;margin:6px 0 8px;font-size:clamp(22px,3.4vw,34px);letter-spacing:.04em;font-weight:800;color:#fff}
.sh-card p{position:relative;margin:0;font-size:15px;color:#cfe8f5}
.sh-card .o{position:relative;margin-top:12px;font-size:13px;letter-spacing:.12em;color:#ffd66b;text-transform:uppercase}
.sh-ban{position:absolute;left:0;right:0;top:24%;text-align:center;font-weight:800;font-size:clamp(20px,3.6vw,38px);letter-spacing:.18em;text-transform:uppercase;opacity:0;transition:opacity .35s;text-shadow:0 0 18px currentColor}
.sh-pause{position:absolute;inset:0;display:none;align-items:center;justify-content:center;background:rgba(2,6,12,.55);pointer-events:auto;backdrop-filter:blur(3px)}
.sh-pause.on{display:flex}
.sh-pbox{width:min(560px,92vw);max-height:90%;overflow:auto;padding:22px;background:rgba(7,14,26,.94);border:1px solid rgba(189,243,255,.25);border-radius:12px}
.sh-pbox h3{margin:0 0 14px;letter-spacing:.2em;text-transform:uppercase;font-size:14px;color:#ffd66b}
.sh-btn{display:block;width:100%;margin:6px 0;padding:11px 14px;border-radius:8px;border:1px solid rgba(189,243,255,.28);background:#0c1a2c;color:#e8f6ff;font:600 15px inherit;text-align:left;cursor:pointer}
.sh-btn:hover,.sh-btn:focus{border-color:#bdf3ff;outline:none;background:#12263e}
.sh-btn.bad{border-color:rgba(255,91,74,.5);color:#ffb0a6}
.sh-keys{display:grid;grid-template-columns:auto 1fr;gap:4px 14px;margin-top:12px;font-size:13px;color:#cfe8f5}
.sh-keys kbd{font:600 12px inherit;padding:1px 6px;border-radius:4px;border:1px solid rgba(189,243,255,.3);background:#0b1726;color:#fff;white-space:nowrap}
.sh-touch{position:absolute;inset:0;display:none;pointer-events:none}
.sh-touch.on{display:block}
.sh-tb{position:absolute;pointer-events:auto;border-radius:50%;border:2px solid rgba(189,243,255,.35);background:rgba(10,24,40,.35);color:#e8f6ff;font:700 12px inherit;display:grid;place-items:center;touch-action:none;letter-spacing:.06em}
.sh-tb.on{background:rgba(120,220,255,.35)}
.sh-stick{position:absolute;left:4%;bottom:6%;width:150px;height:150px;border-radius:50%;border:2px solid rgba(189,243,255,.25);pointer-events:auto;touch-action:none}
.sh-knob{position:absolute;left:50%;top:50%;width:56px;height:56px;margin:-28px 0 0 -28px;border-radius:50%;background:rgba(189,243,255,.3)}
.sh-thr{position:absolute;right:2.5%;top:22%;width:46px;height:44%;border-radius:23px;border:2px solid rgba(189,243,255,.25);pointer-events:auto;touch-action:none}
.sh-thr i{position:absolute;left:4px;right:4px;bottom:4px;border-radius:18px;background:rgba(255,179,92,.45)}
@media (prefers-reduced-motion:reduce){.sh-msg,.sh-card{animation:none;transition:none}}
`;

export function createHud(canvas) {
  const host = canvas.parentNode;
  if (!document.getElementById('sh-style')) { const st = document.createElement('style'); st.id = 'sh-style'; st.textContent = CSS; document.head.appendChild(st); }
  const root = document.createElement('div'); root.className = 'sh-root'; root.style.display = 'none';
  root.innerHTML = `<canvas></canvas><div class="sh-comms"></div><div class="sh-ban"></div><div class="sh-card out"></div>
    <div class="sh-touch"><div class="sh-stick"><div class="sh-knob"></div></div><div class="sh-thr"><i></i></div></div>
    <div class="sh-pause"><div class="sh-pbox"></div></div>`;
  host.appendChild(root);
  const cv = root.querySelector('canvas'), g = cv.getContext('2d');
  const commsEl = root.querySelector('.sh-comms'), banEl = root.querySelector('.sh-ban'), cardEl = root.querySelector('.sh-card');
  const pauseEl = root.querySelector('.sh-pause'), pbox = root.querySelector('.sh-pbox'), touchEl = root.querySelector('.sh-touch');
  root.addEventListener('pointerdown', (e) => { if (e.target.closest('.sh-pause,.sh-tb,.sh-stick,.sh-thr')) e.stopPropagation(); }, true);
  let W = 960, H = 540, S = 1, dpr = 1, F = null, model = null, cam = null, opts = {}, isPaused = false, active = false;
  A.loadManifest().catch(() => {});

  // ---------------------------------------------------------------- input state
  const keys = new Set();
  let aim = { x: 0, y: 0, z: -1 }, aimInit = false, mouseLocked = false, mouseFire = false, curX = -1, curY = -1, curT = 0, lastPadT = 0;
  const pad = { fire: false, boost: false, drift: false, prev: {} };
  const touch = { on: false, sx: 0, sy: 0, fire: false, boost: false, id: -1 };
  const KEY_FLY = new Set([' ', 'Shift', 'w', 's', 'a', 'd', 'q', 'e', 'W', 'S', 'A', 'D', 'Q', 'E', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Enter', 'j', 'J']);
  function onKeyDown(e) {
    if (!active || !F) return;
    if (e.key === 'Tab') return;   // the Game Center's own menu key
    if (e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA')) return;
    const k = e.key.length === 1 ? e.key.toLowerCase() : e.key;
    if (KEY_FLY.has(e.key) || /^[1-4]$/.test(k)) e.preventDefault();
    if (e.repeat) { keys.add(k); return; }
    keys.add(k);
    F.input.touched = true;
    if (k === 'Escape') { setPause(!isPaused); return; }
    if (isPaused) { if (k === 'Enter') setPause(false); return; }
    if (F.outcome && k === 'Enter') return;
    switch (k) {
      case '1': cmd(F, 'pips', 0); break; case '2': cmd(F, 'pips', 1); break; case '3': cmd(F, 'pips', 2); break; case '4': cmd(F, 'pips', 3); break;
      case 'z': cmd(F, 'shields', 1); break; case 'x': cmd(F, 'shields', -1); break;
      case 'f': cmd(F, 'flare'); break; case 'm': cmd(F, 'missile'); break;
      case 't': cmd(F, 'target', 'ahead'); break; case 'r': cmd(F, 'target', 'attacker'); break;
      case 'g': cmd(F, 'target', 'next'); break; case 'b': cycleSub(F); break;
      case 'c': cmd(F, 'cruise'); break;
      case 'v': if (opts.setCamera) opts.setCamera(opts.camera() === 'chase' ? 'cockpit' : 'chase'); break;
      case 'h': toggleHelp(); break;
    }
  }
  function onKeyUp(e) { const k = e.key.length === 1 ? e.key.toLowerCase() : e.key; keys.delete(k); }
  function onMouseMove(e) {
    if (!active || !F || isPaused) return;
    mouseLocked = document.pointerLockElement === canvas;
    if (mouseLocked) { if (cam) rotateAim(-e.movementX * 0.0021, -e.movementY * 0.0021); F.input.touched = true; }
    else { const r = canvas.getBoundingClientRect(); curX = e.clientX - r.left; curY = e.clientY - r.top; curT = performance.now(); }
  }
  function onMouseDown(e) {
    if (!active || !F || isPaused) return;
    if (e.button === 0) mouseFire = true;
    if (e.button === 2) cmd(F, 'missile');
    F.input.touched = true;
  }
  function onMouseUp(e) { if (e.button === 0) mouseFire = false; }
  function onWheel(e) { if (!active || !F) return; F.input.thr = Math.max(0, Math.min(1, F.input.thr - Math.sign(e.deltaY) * 0.06)); e.preventDefault(); }
  window.addEventListener('keydown', onKeyDown, true); window.addEventListener('keyup', onKeyUp, true);
  document.addEventListener('mousemove', onMouseMove); canvas.addEventListener('mousedown', onMouseDown); window.addEventListener('mouseup', onMouseUp);
  canvas.addEventListener('wheel', onWheel, { passive: false });
  const blur = () => { keys.clear(); mouseFire = false; };
  window.addEventListener('blur', blur);
  let wasLocked = false;
  const onLockChange = () => {
    const now = document.pointerLockElement === canvas;
    if (wasLocked && !now && active && F && !F.outcome && !isPaused) setPause(true);
    wasLocked = now; mouseLocked = now;
  };
  document.addEventListener('pointerlockchange', onLockChange);

  // aim vector: rotate about the camera's up / right axes (mouse aim), clamped to 38 degrees off the nose
  const CU = { x: 0, y: 1, z: 0 }, CR = { x: 1, y: 0, z: 0 };
  function rot(v, a, ang) { const c = Math.cos(ang), s = Math.sin(ang), d = v.x * a.x + v.y * a.y + v.z * a.z; const cx = a.y * v.z - a.z * v.y, cy = a.z * v.x - a.x * v.z, cz = a.x * v.y - a.y * v.x; v.x = v.x * c + cx * s + a.x * d * (1 - c); v.y = v.y * c + cy * s + a.y * d * (1 - c); v.z = v.z * c + cz * s + a.z * d * (1 - c); }
  function rotateAim(yaw, pitch) {
    const m = cam.matrixWorld.elements; CU.x = m[4]; CU.y = m[5]; CU.z = m[6]; CR.x = m[0]; CR.y = m[1]; CR.z = m[2];
    rot(aim, CU, yaw); rot(aim, CR, pitch); clampAim();
  }
  const NF = { x: 0, y: 0, z: 0 };
  function shipFwd(s, o) { const q = s.q, x = 0, y = 0, z = -1; const ix = q.w * x + q.y * z - q.z * y, iy = q.w * y + q.z * x - q.x * z, iz = q.w * z + q.x * y - q.y * x, iw = -q.x * x - q.y * y - q.z * z; o.x = ix * q.w + iw * -q.x + iy * -q.z - iz * -q.y; o.y = iy * q.w + iw * -q.y + iz * -q.x - ix * -q.z; o.z = iz * q.w + iw * -q.z + ix * -q.y - iy * -q.x; return o; }
  function clampAim() {
    const l = Math.hypot(aim.x, aim.y, aim.z) || 1; aim.x /= l; aim.y /= l; aim.z /= l;
    if (!F) return;
    shipFwd(F.player, NF);
    const c = aim.x * NF.x + aim.y * NF.y + aim.z * NF.z, MAX = Math.cos(0.66);
    if (c < MAX) {   // pull back onto the cone
      const k = 0.66 / Math.acos(Math.max(-1, Math.min(1, c)));
      aim.x = NF.x + (aim.x - NF.x) * k; aim.y = NF.y + (aim.y - NF.y) * k; aim.z = NF.z + (aim.z - NF.z) * k;
      const l2 = Math.hypot(aim.x, aim.y, aim.z) || 1; aim.x /= l2; aim.y /= l2; aim.z /= l2;
    }
  }

  function pollInput(dt) {
    const I = F.input, p = F.player;
    if (!aimInit) { shipFwd(p, aim); aimInit = true; }
    let pitch = 0, yaw = 0, roll = 0, thrRate = 0, rateMode = false;
    const kd = (k) => keys.has(k);
    if (kd('ArrowUp')) { pitch += 1; rateMode = true; } if (kd('ArrowDown')) { pitch -= 1; rateMode = true; }
    if (kd('ArrowLeft')) { yaw -= 1; rateMode = true; } if (kd('ArrowRight')) { yaw += 1; rateMode = true; }
    if (kd('a')) roll -= 1; if (kd('d')) roll += 1;
    if (kd('w')) thrRate += 1; if (kd('s')) thrRate -= 1;
    if (kd('q')) rotateAim(dt * 1.1, 0); if (kd('e')) rotateAim(-dt * 1.1, 0);
    let boost = kd('Shift'), drift = kd(' '), fire = mouseFire || kd('Enter') || kd('j');
    // gamepad (standard mapping)
    const gp = activePad();
    if (gp) {
      const ax = (i) => { const v = gp.axes[i] || 0; return Math.abs(v) < 0.14 ? 0 : (v - Math.sign(v) * 0.14) / 0.86; };
      const bt = (i) => !!(gp.buttons[i] && (gp.buttons[i].pressed || gp.buttons[i].value > 0.35));
      const lx = ax(0), ly = ax(1), rx = ax(2), ry = ax(3);
      if (lx || ly) { yaw += lx; pitch += -ly; rateMode = true; lastPadT = performance.now(); }
      if (rx) roll += rx;
      if (ry) thrRate += -ry;
      fire = fire || bt(7); boost = boost || bt(5); drift = drift || bt(10);
      const edge = (i, fn) => { const d = bt(i); if (d && !pad.prev[i]) fn(); pad.prev[i] = d; };
      edge(6, () => cmd(F, 'missile')); edge(4, () => cmd(F, 'flare')); edge(0, () => cmd(F, 'target', 'ahead')); edge(1, () => cmd(F, 'target', 'attacker'));
      edge(2, () => cmd(F, 'target', 'next')); edge(3, () => opts.setCamera && opts.setCamera(opts.camera() === 'chase' ? 'cockpit' : 'chase'));
      edge(14, () => cmd(F, 'pips', 0)); edge(12, () => cmd(F, 'pips', 1)); edge(15, () => cmd(F, 'pips', 2)); edge(13, () => cmd(F, 'pips', 3)); edge(11, () => cmd(F, 'cruise'));
      if (fire || boost || lx || ly || rx || ry) I.touched = true;
    }
    // touch
    if (touch.on) { if (touch.sx || touch.sy) { yaw += touch.sx; pitch += -touch.sy; rateMode = true; } fire = fire || touch.fire; boost = boost || touch.boost; }
    // mouse flight without pointer lock: steer toward the cursor
    if (!rateMode && !mouseLocked && curX >= 0 && performance.now() - curT < 4000 && cam) {
      const nx = (curX / W) * 2 - 1, ny = 1 - (curY / H) * 2, tanH = Math.tan(cam.fov * Math.PI / 360);
      const vx = nx * tanH * cam.aspect, vy = ny * tanH, m = cam.matrixWorld.elements;
      aim.x = m[0] * vx + m[4] * vy - m[8]; aim.y = m[1] * vx + m[5] * vy - m[9]; aim.z = m[2] * vx + m[6] * vy - m[10]; clampAim();
    }
    if (rateMode) { shipFwd(p, aim); I.aimMode = 0; }
    else I.aimMode = 1;
    I.aim = aim; I.pitch = Math.max(-1, Math.min(1, pitch)); I.yaw = Math.max(-1, Math.min(1, yaw)); I.roll = Math.max(-1, Math.min(1, roll));
    I.thrRate = thrRate; I.boost = boost; I.drift = drift; I.fire = fire && !isPaused;
    if (touch.on && touch.thr != null) I.thr = touch.thr;
  }
  function activePad() { const pads = (navigator.getGamepads && navigator.getGamepads()) || []; for (const p of pads) if (p && p.connected && p.mapping === 'standard') return p; for (const p of pads) if (p && p.connected) return p; return null; }

  // ---------------------------------------------------------------- touch controls
  const isTouch = ('ontouchstart' in window) || (window.matchMedia && matchMedia('(pointer:coarse)').matches);
  if (isTouch) {
    touch.on = true; touchEl.classList.add('on');
    const btn = (label, css, down, up) => { const b = document.createElement('div'); b.className = 'sh-tb'; b.textContent = label; b.style.cssText = css;
      b.addEventListener('pointerdown', (e) => { e.preventDefault(); b.classList.add('on'); if (F) F.input.touched = true; down(); });
      const rel = () => { b.classList.remove('on'); if (up) up(); }; b.addEventListener('pointerup', rel); b.addEventListener('pointercancel', rel); b.addEventListener('pointerleave', rel); touchEl.appendChild(b); return b; };
    btn('FIRE', 'right:10%;bottom:8%;width:96px;height:96px', () => { touch.fire = true; }, () => { touch.fire = false; });
    btn('MSL', 'right:22%;bottom:6%;width:62px;height:62px', () => F && cmd(F, 'missile'));
    btn('BST', 'right:9%;bottom:28%;width:62px;height:62px', () => { touch.boost = true; }, () => { touch.boost = false; });
    btn('FLR', 'right:22%;bottom:22%;width:54px;height:54px', () => F && cmd(F, 'flare'));
    btn('TGT', 'right:3%;bottom:40%;width:54px;height:54px', () => F && cmd(F, 'target', 'ahead'));
    btn('II', 'right:2%;top:2%;width:44px;height:44px;border-radius:10px', () => setPause(!isPaused));
    const stick = touchEl.querySelector('.sh-stick'), knob = stick.querySelector('.sh-knob');
    const move = (e) => { const r = stick.getBoundingClientRect(), x = (e.clientX - r.left) / r.width * 2 - 1, y = (e.clientY - r.top) / r.height * 2 - 1, l = Math.hypot(x, y), k = l > 1 ? 1 / l : 1; touch.sx = x * k; touch.sy = y * k; knob.style.transform = `translate(${touch.sx * 45}px,${touch.sy * 45}px)`; };
    stick.addEventListener('pointerdown', (e) => { e.preventDefault(); stick.setPointerCapture(e.pointerId); move(e); if (F) F.input.touched = true; });
    stick.addEventListener('pointermove', (e) => { if (e.buttons || e.pointerType === 'touch') move(e); });
    const stickUp = () => { touch.sx = touch.sy = 0; knob.style.transform = ''; };
    stick.addEventListener('pointerup', stickUp); stick.addEventListener('pointercancel', stickUp);
    const thr = touchEl.querySelector('.sh-thr'), thrI = thr.querySelector('i');
    const tmove = (e) => { const r = thr.getBoundingClientRect(); touch.thr = Math.max(0, Math.min(1, 1 - (e.clientY - r.top) / r.height)); thrI.style.height = (touch.thr * 100) + '%'; };
    thr.addEventListener('pointerdown', (e) => { e.preventDefault(); thr.setPointerCapture(e.pointerId); tmove(e); });
    thr.addEventListener('pointermove', (e) => { if (e.buttons || e.pointerType === 'touch') tmove(e); });
    touch.thr = 0.55; thrI.style.height = '55%';
  }

  // ---------------------------------------------------------------- pause menu
  let helpOn = false;
  function setPause(on) {
    if (!F || F.outcome) on = false;
    isPaused = on; pauseEl.classList.toggle('on', on);
    if (on) { mouseFire = false; keys.clear(); if (document.pointerLockElement) document.exitPointerLock(); buildPause(); const b = pbox.querySelector('.sh-btn'); if (b) b.focus(); }
    else { helpOn = false; try { const r = canvas.requestPointerLock && canvas.requestPointerLock(); if (r && r.catch) r.catch(() => {}); } catch {} }
  }
  function toggleHelp() { helpOn = !helpOn; if (!isPaused) setPause(true); else buildPause(); }
  function keysHtml() {
    const rows = [['🖱', 'cz_k_aim'], ['LMB / Enter', 'cz_k_fire'], ['RMB / M', 'cz_k_missile'], ['W / S · wheel', 'cz_k_throttle'], ['A / D', 'cz_k_roll'], ['Q / E', 'cz_k_yaw'], ['↑ ↓ ← →', 'cz_k_pitchyaw'],
      ['Shift', 'cz_k_boost'], ['Space', 'cz_k_drift'], ['C', 'cz_k_cruise'], ['1 2 3 · 4', 'cz_k_pips'], ['Z / X', 'cz_k_shields'], ['F', 'cz_k_flares'], ['T · R · G · B', 'cz_k_target'], ['V', 'cz_k_camera'], ['Esc', 'cz_k_pause'],
      ['🎮', 'cz_k_pad']];
    return '<div class="sh-keys">' + rows.map(([k, s]) => `<kbd>${esc(k)}</kbd><span>${esc(tr(s))}</span>`).join('') + '</div>';
  }
  function buildPause() {
    const q = opts.quality ? opts.quality() : 'auto';
    pbox.innerHTML = `<h3>${esc(tr('cz_pause_title'))}</h3>
      <button class="sh-btn" data-a="resume">▶ ${esc(tr('cz_pause_resume'))}</button>
      <button class="sh-btn" data-a="cam">🎥 ${esc(tr('cz_pause_camera'))}: ${esc(tr(opts.camera && opts.camera() === 'cockpit' ? 'cz_cam_cockpit' : 'cz_cam_chase'))}</button>
      <button class="sh-btn" data-a="q">✦ ${esc(tr('cz_pause_quality'))}: ${esc(tr('cz_q_' + q))}</button>
      <button class="sh-btn" data-a="help">⌨ ${esc(tr('cz_pause_controls'))}</button>
      <button class="sh-btn bad" data-a="retreat">⇥ ${esc(tr('cz_pause_retreat'))}</button>
      ${helpOn ? keysHtml() : ''}`;
  }
  pbox.addEventListener('click', (e) => {
    const b = e.target.closest('[data-a]'); if (!b) return;
    const a = b.dataset.a;
    if (a === 'resume') setPause(false);
    else if (a === 'cam') { opts.setCamera(opts.camera() === 'chase' ? 'cockpit' : 'chase'); buildPause(); }
    else if (a === 'q') { const order = ['auto', 'low', 'medium', 'high'], cur = opts.quality(), nx = order[(order.indexOf(cur) + 1) % order.length]; opts.setQuality(nx); qPending = nx; buildPause(); note('cz_hud_quality_next', 'info'); }
    else if (a === 'help') { helpOn = !helpOn; buildPause(); }
    else if (a === 'retreat') { cmd(F, 'retreat'); setPause(false); }
  });
  let qPending = null;

  // ---------------------------------------------------------------- comms / notes / banners
  const msgs = [];
  function speaker(e) {
    const s = e.s;
    if (e.v && e.v.voice) return { name: tr('cz_spk_control_3'), col: '#9fe8ff', portrait: 'echo', fac: 3 };
    if (!s) { const fac = F && F.mission ? (e.k === 'cz_c_capital' || e.k === 'cz_c_contacts' || e.k === 'cz_c_ambush' ? F.mission.offer : F.mission.offer) : 0; return { name: tr('cz_spk_control_' + fac), col: FAC_COL[fac] || COL.hud, portrait: CONTACT[fac], fac }; }
    if (s.ace) return { name: s.name, col: '#ff6a5a', portrait: s.aceId >= 0 ? ACE_PORTRAIT[s.aceId] : null, fac: s.fac, emblem: EMBLEM[s.fac] };
    return { name: tr(s.nameKey || 'cz_cs_' + s.fac) + (s.name ? ' ' + s.name : ''), col: s.team === TEAM_P ? COL.friend : COL.hostile, portrait: null, fac: s.fac, emblem: EMBLEM[s.fac] };
  }
  function commsLine(e) {
    const sp = speaker(e);
    const v = e.v ? { ...e.v } : {};
    if (v.t) v.t = tr(v.t);
    const text = tr(e.k, v); if (!text) return;
    const el = document.createElement('div'); el.className = 'sh-msg'; el.style.setProperty('--c', sp.col);
    const pt = document.createElement('div'); pt.className = 'sh-pt'; pt.textContent = (sp.name || '?').trim().charAt(0).toUpperCase();
    const img = sp.portrait || sp.emblem;
    if (img && A.has('images', img)) { pt.style.backgroundImage = A.placeholder(img); A.imageUrl(img).then((u) => { pt.textContent = ''; pt.style.backgroundImage = `url("${u}")`; pt.style.backgroundSize = 'cover'; pt.style.backgroundPosition = 'center 25%'; }).catch(() => {}); }
    const tx = document.createElement('div'); const b = document.createElement('b'); b.textContent = sp.name; const sp2 = document.createElement('span'); sp2.textContent = text;
    tx.appendChild(b); tx.appendChild(sp2); el.appendChild(pt); el.appendChild(tx);
    commsEl.appendChild(el); msgs.push({ el, t: 0, life: 3.4 + text.length * 0.035 });
    while (msgs.length > 3) { const m = msgs.shift(); m.el.remove(); }
  }
  let banT = 0;
  function banner(text, color, life = 2.2) { banEl.textContent = text; banEl.style.color = color || COL.hud; banEl.style.opacity = 1; banT = life; }
  const notes = [];
  function note(key, kind, v) { const s = tr(key, v); if (!s) return; notes.push({ s, kind, t: 2.4 }); if (notes.length > 3) notes.shift(); }
  // mission card
  let cardT = 0;
  function showCard(F, model) {
    const M = F.mission, fl = model && model.flavor ? flavorText({ flavor: model.flavor }) : null;
    const kindKey = 'cz_mk_' + M.kind;
    const title = (fl && fl.title) || tr(kindKey);
    const brief = (fl && fl.brief) || tr('cz_mb_' + M.kind);
    const art = { patrol: 'brief_patrol', ambush: 'brief_patrol', hunt: 'brief_bounty', duel: 'brief_bounty', escort: 'brief_escort', defend: 'brief_defend', sweep: 'brief_defend' }[M.kind];
    cardEl.innerHTML = `<div class="art"></div><div class="k"></div><h2></h2><p></p><div class="o"></div>`;
    cardEl.querySelector('.k').textContent = (model && model.sysName ? model.sysName + ' · ' : '') + tr(kindKey);
    cardEl.querySelector('h2').textContent = title; cardEl.querySelector('p').textContent = brief;
    cardEl.querySelector('.o').textContent = objText(F);
    const artEl = cardEl.querySelector('.art');
    if (art && A.has('images', art)) { artEl.style.backgroundImage = A.placeholder(art); A.imageUrl(art).then((u) => { artEl.style.backgroundImage = `url("${u}")`; }).catch(() => {}); }
    cardEl.classList.remove('out'); cardT = 4.2;
  }
  function objText(F) {
    const O = F.obj; if (!O.key) return '';
    const a = { ...O.a }; if (a.d != null) a.d = fmtDist(a.d);
    return tr(O.key, a);
  }
  const fmtDist = (d) => d >= 1000 ? (d / 1000).toFixed(1) + ' km' : Math.round(d) + ' m';

  // ---------------------------------------------------------------- projection
  const P = { x: 0, y: 0, z: 0, behind: false, vx: 0, vy: 0, vz: 0, ok: false };
  function proj(x, y, z) {
    const v = cam.matrixWorldInverse.elements, pm = cam.projectionMatrix.elements;
    const vx = v[0] * x + v[4] * y + v[8] * z + v[12], vy = v[1] * x + v[5] * y + v[9] * z + v[13], vz = v[2] * x + v[6] * y + v[10] * z + v[14];
    const cx = pm[0] * vx + pm[4] * vy + pm[8] * vz + pm[12], cy = pm[1] * vx + pm[5] * vy + pm[9] * vz + pm[13], cw = pm[3] * vx + pm[7] * vy + pm[11] * vz + pm[15];
    P.vx = vx; P.vy = vy; P.vz = vz; P.behind = vz > -0.1;
    if (!P.behind) { P.x = (cx / cw * 0.5 + 0.5) * W; P.y = (0.5 - cy / cw * 0.5) * H; } else { P.x = -1e4; P.y = -1e4; }
    P.ok = !P.behind && P.x > 0 && P.x < W && P.y > 0 && P.y < H;
    return P;
  }
  const lerp = (a, b, k) => a + (b - a) * k;
  function sp(s, al) { return proj(lerp(s.ppos.x, s.pos.x, al), lerp(s.ppos.y, s.pos.y, al), lerp(s.ppos.z, s.pos.z, al)); }
  function pxRadius(r, d) { return r / Math.max(1, d) * (H / 2) / Math.tan(cam.fov * Math.PI / 360); }

  // ---------------------------------------------------------------- drawing helpers
  function line(x0, y0, x1, y1) { g.beginPath(); g.moveTo(x0, y0); g.lineTo(x1, y1); g.stroke(); }
  function brackets(x, y, r, col, k = 0.32, lw = 1.5) { g.strokeStyle = col; g.lineWidth = lw * S; const a = r * k; g.beginPath(); for (const [sx, sy] of [[-1, -1], [1, -1], [1, 1], [-1, 1]]) { g.moveTo(x + sx * r, y + sy * (r - a)); g.lineTo(x + sx * r, y + sy * r); g.lineTo(x + sx * (r - a), y + sy * r); } g.stroke(); }
  function bar(x, y, w, h, f, col, bg = COL.faint) { g.fillStyle = bg; g.fillRect(x, y, w, h); g.fillStyle = col; g.fillRect(x, y, w * Math.max(0, Math.min(1, f)), h); }
  const fonts = new Map();   // cached font strings (no per-call template allocation)
  function font(size, weight) { const k = weight * 1000 + Math.round(size * S); let f = fonts.get(k); if (!f) { f = `${weight} ${Math.round(size * S)}px 'Segoe UI',system-ui,sans-serif`; fonts.set(k, f); } return f; }
  function text(s, x, y, size, col, align = 'left', weight = 600) { g.font = font(size, weight); g.textAlign = align; g.fillStyle = col; g.fillText(s, x, y); }
  function arc(x, y, r, a0, a1, col, lw) { g.strokeStyle = col; g.lineWidth = lw * S; g.beginPath(); g.arc(x, y, r, a0, a1); g.stroke(); }
  function chevron(x, y, ang, size, col, fill) { g.save(); g.translate(x, y); g.rotate(ang); g.beginPath(); g.moveTo(size, 0); g.lineTo(-size * 0.7, size * 0.7); g.lineTo(-size * 0.3, 0); g.lineTo(-size * 0.7, -size * 0.7); g.closePath(); if (fill) { g.fillStyle = col; g.fill(); } else { g.strokeStyle = col; g.lineWidth = 1.5 * S; g.stroke(); } g.restore(); }
  function edgePoint(vx, vy, inset) {   // view-space direction -> point on an inset ellipse
    const a = Math.atan2(-vy, vx), rx = W / 2 - inset, ry = H / 2 - inset;
    return { x: W / 2 + Math.cos(a) * rx, y: H / 2 + Math.sin(a) * ry, a };
  }

  // ---------------------------------------------------------------- effects state
  let hitT = 0, hitKill = false, lockFlash = 0, endT = -1, endRes = 0;
  const pops = [];       // kill credit popups {x,y,z,v,t}
  const hurts = [];      // damage direction {x,y,z,t,hull}
  const subs = [];
  let flick = 0;

  // ---------------------------------------------------------------- main draw
  function draw(dt) {
    const p = F.player, al = F.alpha;
    g.setTransform(1, 0, 0, 1, 0, 0); g.clearRect(0, 0, W, H);
    if (!p) return;
    const crit = p.alive && p.hull < p.hullMax * 0.3;
    flick = crit && Math.random() < 0.06 ? 0.4 + Math.random() * 0.4 : Math.max(0, flick - dt * 4);
    g.globalAlpha = 1 - flick * 0.6;
    const cx = W / 2, cy = H / 2;
    const briefing = F.briefT > 0.6;
    // ---- reticle at the gun convergence point
    const conv = p.target && p.target.alive ? Math.min(1500, Math.max(150, Math.hypot(p.target.pos.x - p.pos.x, p.target.pos.y - p.pos.y, p.target.pos.z - p.pos.z))) : 500;
    shipFwd(p, NF);
    const px = lerp(p.ppos.x, p.pos.x, al), py = lerp(p.ppos.y, p.pos.y, al), pz = lerp(p.ppos.z, p.pos.z, al);
    proj(px + NF.x * conv, py + NF.y * conv, pz + NF.z * conv);
    const rx = P.behind ? cx : P.x, ry = P.behind ? cy : P.y, R0 = 20 * S;
    if (p.alive && !briefing) {
      const heat = p.heat, over = p.overT > 0;
      g.strokeStyle = over ? COL.bad : COL.hud; g.lineWidth = 1.5 * S;
      for (let i = 0; i < 4; i++) { const a = i * Math.PI / 2 + Math.PI / 4; arc(rx, ry, R0, a - 0.5, a + 0.5, over ? COL.bad : COL.hud, 1.5); }
      g.fillStyle = COL.hud; g.fillRect(rx - 1.5 * S, ry - 1.5 * S, 3 * S, 3 * S);
      // heat arc (left)
      arc(rx, ry, R0 + 7 * S, Math.PI * 0.7, Math.PI * 1.3, COL.faint, 3);
      if (heat > 0.01) arc(rx, ry, R0 + 7 * S, Math.PI * 1.3 - heat * Math.PI * 0.6, Math.PI * 1.3, heat > 0.75 || over ? COL.bad : COL.warn, 3);
      // missile lock arc (right)
      if (p.target && p.target.alive && p.msl > 0) {
        const lk = Math.min(1, p.lockT / p.lockTime);
        arc(rx, ry, R0 + 7 * S, -Math.PI * 0.3, Math.PI * 0.3, COL.faint, 3);
        if (lk > 0) arc(rx, ry, R0 + 7 * S, -Math.PI * 0.3, -Math.PI * 0.3 + lk * Math.PI * 0.6, lk >= 1 ? COL.hostile : COL.gold, 3);
      }
      // mouse-aim cursor
      if (F.input.aimMode === 1 && F.input.aim) {
        const a = F.input.aim; proj(px + a.x * 600, py + a.y * 600, pz + a.z * 600);
        if (!P.behind) { const dx = P.x - rx, dy = P.y - ry; if (dx * dx + dy * dy > 30 * 30 * S * S) { g.strokeStyle = COL.faint; g.lineWidth = 1 * S; line(rx, ry, P.x, P.y); } arc(P.x, P.y, 6 * S, 0, 6.29, COL.dim, 1.5); }
      }
    }
    // ---- ships: brackets, labels, off-screen arrows
    const tgt = p.target && p.target.alive ? p.target : null;
    let tgtScreen = null;
    for (const s of F.ships) {
      if (!s.alive || s.isPlayer || s.st === ST.DARK) continue;
      if (s.st === ST.WARP && s.warpT > 0.25) continue;
      const d = Math.hypot(s.pos.x - p.pos.x, s.pos.y - p.pos.y, s.pos.z - p.pos.z);
      const hostile = s.team === TEAM_E, obj = s.objective;
      const col = obj ? COL.gold : hostile ? COL.hostile : COL.friend;
      sp(s, al);
      if (P.ok) {
        const r = Math.max(9 * S, Math.min(H * 0.4, pxRadius(s.cls.rad * 1.2, -P.vz)));
        if (s === tgt) { tgtScreen = { x: P.x, y: P.y, r, d }; continue; }
        if (!hostile && !obj && d > 2500) continue;
        if (hostile && d > 4500) { g.fillStyle = col; g.fillRect(P.x - 1.5 * S, P.y - 1.5 * S, 3 * S, 3 * S); continue; }
        brackets(P.x, P.y, r, col, 0.3, hostile ? 1.4 : 1.1);
        if (obj) { bar(P.x - r, P.y + r + 4 * S, r * 2, 3 * S, s.hull / s.hullMax, COL.gold); }
        if (s.ace) text(s.name, P.x, P.y - r - 5 * S, 11, '#ff8a7a', 'center', 700);
        else if (s.cap) text(CLS[s.ck].name, P.x, P.y - r - 5 * S, 11, col, 'center');
        if (s.tetheredBy || (s.board)) text('⛓', P.x + r + 4 * S, P.y, 12, COL.warn);
      } else if (hostile || obj) {
        if (d > 6000 && !obj && s !== tgt) continue;
        const e = edgePoint(P.behind ? -P.vx : P.vx, P.behind ? -P.vy : P.vy, 44 * S);
        if (s === tgt) continue;
        chevron(e.x, e.y, e.a, (obj ? 8 : 6) * S, col, obj || s.cap);
      }
    }
    // ---- target: full box, lead pip, lock diamond, info panel
    if (tgt) {
      if (tgtScreen) {
        const { x, y, r } = tgtScreen;
        brackets(x, y, r + 3 * S, COL.hostile, 0.45, 2);
        if (p.lockT >= p.lockTime) { g.save(); g.translate(x, y); g.rotate(F.t * 2); g.strokeStyle = COL.hostile; g.lineWidth = 2 * S; const q = r + 10 * S; g.beginPath(); g.moveTo(0, -q); g.lineTo(q, 0); g.lineTo(0, q); g.lineTo(-q, 0); g.closePath(); g.stroke(); g.restore(); }
        if (lockFlash > 0) { arc(x, y, r + 18 * S * (1 - lockFlash), 0, 6.29, `rgba(255,91,74,${lockFlash})`, 2); }
        // capital subsystems
        if (tgt.sub) {
          for (let k = 0; k < tgt.sub.length; k++) {
            const sb = tgt.sub[k]; const alive = tgt.subHp[k] > 0;
            localPoint(tgt, sb.p, al); if (!P.ok) continue;
            const sel = p.subSel === k;
            g.strokeStyle = !alive ? 'rgba(255,255,255,0.25)' : sel ? COL.gold : 'rgba(255,140,120,0.7)'; g.lineWidth = (sel ? 2 : 1) * S;
            g.beginPath(); g.arc(P.x, P.y, (sel ? 9 : 6) * S, 0, 6.29); g.stroke();
            if (sel && alive) { text(tr('cz_sub_' + sb.k), P.x + 12 * S, P.y + 4 * S, 11, COL.gold); bar(P.x + 12 * S, P.y + 8 * S, 40 * S, 3 * S, tgt.subHp[k] / tgt.subMax[k], COL.gold); }
          }
        }
      } else {
        sp(tgt, al);
        const e = edgePoint(P.behind ? -P.vx : P.vx, P.behind ? -P.vy : P.vy, 56 * S);
        chevron(e.x, e.y, e.a, 12 * S, COL.hostile, true);
        text(fmtDist(Math.hypot(tgt.pos.x - p.pos.x, tgt.pos.y - p.pos.y, tgt.pos.z - p.pos.z)), e.x - Math.cos(e.a) * 26 * S, e.y - Math.sin(e.a) * 26 * S + 4 * S, 11, COL.hostile, 'center');
      }
      // lead pip
      const LP = { x: 0, y: 0, z: 0 };
      leadPoint(p.pos.x, p.pos.y, p.pos.z, p.vel.x, p.vel.y, p.vel.z, tgt, CLS.lucciola.bolt, LP);
      const dT = Math.hypot(tgt.pos.x - p.pos.x, tgt.pos.y - p.pos.y, tgt.pos.z - p.pos.z);
      if (dT < 2400) {
        proj(LP.x, LP.y, LP.z);
        if (P.ok) {
          const on = Math.hypot(P.x - rx, P.y - ry) < R0 * 0.9;
          g.save(); g.translate(P.x, P.y); g.rotate(Math.PI / 4);
          const q = 6 * S; if (on) { g.fillStyle = COL.hostile; g.fillRect(-q, -q, q * 2, q * 2); } else { g.strokeStyle = COL.hostile; g.lineWidth = 1.6 * S; g.strokeRect(-q, -q, q * 2, q * 2); }
          g.restore();
          if (tgtScreen && !on) { g.strokeStyle = 'rgba(255,91,74,0.35)'; g.lineWidth = 1 * S; line(tgtScreen.x, tgtScreen.y, P.x, P.y); }
        }
      }
      drawTargetPanel(tgt, dT);
    }
    // ---- objective marker (nav / jump gate)
    if (F.obj.marker && !briefing) {
      const m = F.obj.marker; proj(m[0], m[1], m[2]);
      const d = Math.hypot(m[0] - p.pos.x, m[1] - p.pos.y, m[2] - p.pos.z);
      if (P.ok) { g.save(); g.translate(P.x, P.y); g.rotate(Math.PI / 4); g.strokeStyle = COL.gold; g.lineWidth = 2 * S; g.strokeRect(-8 * S, -8 * S, 16 * S, 16 * S); g.restore(); text(fmtDist(d), P.x, P.y + 24 * S, 12, COL.gold, 'center'); }
      else { const e = edgePoint(P.behind ? -P.vx : P.vx, P.behind ? -P.vy : P.vy, 70 * S); chevron(e.x, e.y, e.a, 10 * S, COL.gold, true); text(fmtDist(d), e.x - Math.cos(e.a) * 26 * S, e.y - Math.sin(e.a) * 26 * S + 4 * S, 11, COL.gold, 'center'); }
    }
    // ---- incoming missiles
    let warnTxt = null, warnCol = COL.bad;
    for (const m of F.msls) {
      if (!m.alive || m.tgt !== p || m.flare) continue;
      proj(m.pos.x, m.pos.y, m.pos.z);
      const blink = (F.t * 6) % 1 < 0.6;
      if (P.ok) { if (blink) { g.strokeStyle = COL.bad; g.lineWidth = 2 * S; g.beginPath(); g.arc(P.x, P.y, 9 * S, 0, 6.29); g.stroke(); } }
      else { const e = edgePoint(P.behind ? -P.vx : P.vx, P.behind ? -P.vy : P.vy, 32 * S); if (blink) chevron(e.x, e.y, e.a, 11 * S, COL.bad, true); }
    }
    if (p.incoming) warnTxt = tr('cz_hud_incoming') + ' ' + fmtDist(p.incoming);
    else if (p.lockWarn > 0.15) { warnTxt = tr('cz_hud_lockwarn'); warnCol = COL.warn; }
    if (p.tetheredBy) { warnTxt = tr('cz_hud_tethered'); warnCol = COL.warn; }
    if (warnTxt && !briefing && ((F.t * 4) % 1 < 0.7)) text(warnTxt, cx, H * 0.2, 17, warnCol, 'center', 800);
    if (p.tetheredBy) bar(cx - 80 * S, H * 0.2 + 8 * S, 160 * S, 4 * S, p.breakT / 1.5, COL.warn);
    // ---- damage direction
    for (let i = hurts.length - 1; i >= 0; i--) {
      const h = hurts[i]; h.t -= dt; if (h.t <= 0) { hurts.splice(i, 1); continue; }
      proj(h.x, h.y, h.z);
      const a = Math.atan2(-(P.behind ? -P.vy : P.vy), (P.behind ? -P.vx : P.vx));
      arc(cx, cy, Math.min(W, H) * 0.2, a - 0.35, a + 0.35, h.hull ? `rgba(255,70,50,${h.t})` : `rgba(110,200,255,${h.t * 0.8})`, 5);
    }
    // ---- hit marker
    if (hitT > 0) {
      hitT -= dt; const k = hitT / 0.22, o = (8 + (1 - k) * 4) * S, l = 6 * S;
      g.strokeStyle = hitKill ? `rgba(255,91,74,${k})` : `rgba(255,255,255,${k})`; g.lineWidth = (hitKill ? 2.5 : 1.8) * S;
      for (const [sx, sy] of [[1, 1], [-1, 1], [1, -1], [-1, -1]]) line(rx + sx * o, ry + sy * o, rx + sx * (o + l), ry + sy * (o + l));
    }
    if (lockFlash > 0) lockFlash -= dt * 2;
    // ---- kill credit popups
    for (let i = pops.length - 1; i >= 0; i--) {
      const q = pops[i]; q.t -= dt; if (q.t <= 0) { pops.splice(i, 1); continue; }
      proj(q.x, q.y, q.z); if (!P.ok) continue;
      const k = q.t / 1.6;
      text('+' + q.v + ' cr', P.x, P.y - (1 - k) * 40 * S, 14, `rgba(255,214,107,${Math.min(1, k * 1.6)})`, 'center', 800);
    }
    if (!briefing) { drawBottom(p); drawRadar(p); drawTracker(); }
    // ---- notes (right of centre)
    let ny = H * 0.62;
    for (let i = notes.length - 1; i >= 0; i--) { const n = notes[i]; n.t -= dt; if (n.t <= 0) { notes.splice(i, 1); continue; } text(n.s, cx, ny, 13, n.kind === 'bad' ? COL.bad : n.kind === 'good' ? COL.friend : COL.hud, 'center', 700); ny += 18 * S; }
    // ---- outside the area
    if (F.outT > 0) text(tr('cz_hud_leaving', { s: Math.max(0, Math.ceil(12 - F.outT)) }), cx, H * 0.28, 16, COL.warn, 'center', 800);
    g.globalAlpha = 1;
  }
  const LPW = { x: 0, y: 0, z: 0 };
  function localPoint(s, p3, al) {
    const q = s.q, x = p3[0], y = p3[1], z = p3[2];
    const ix = q.w * x + q.y * z - q.z * y, iy = q.w * y + q.z * x - q.x * z, iz = q.w * z + q.x * y - q.y * x, iw = -q.x * x - q.y * y - q.z * z;
    LPW.x = ix * q.w + iw * -q.x + iy * -q.z - iz * -q.y; LPW.y = iy * q.w + iw * -q.y + iz * -q.x - ix * -q.z; LPW.z = iz * q.w + iw * -q.z + ix * -q.y - iy * -q.x;
    return proj(lerp(s.ppos.x, s.pos.x, al) + LPW.x, lerp(s.ppos.y, s.pos.y, al) + LPW.y, lerp(s.ppos.z, s.pos.z, al) + LPW.z);
  }
  function shipLabel(s) { if (s.ace) return s.name; if (s.nameKey === 'cz_ship_hauler') return tr('cz_ship_hauler') + ' ' + (s.name || ''); return CLS[s.ck] ? CLS[s.ck].name : s.ck; }
  function drawTargetPanel(s, d) {
    const w = 230 * S, x = W - w - 18 * S, y = 70 * S, h = 86 * S;
    g.fillStyle = 'rgba(6,14,24,0.6)'; g.fillRect(x, y, w, h);
    g.fillStyle = COL.hostile; g.fillRect(x, y, 3 * S, h);
    text(shipLabel(s), x + 12 * S, y + 22 * S, 16, '#fff', 'left', 800);
    text(tr('cz_fac_' + s.fac) + (s.ace ? ' · ' + tr('cz_hud_ace') : '') , x + 12 * S, y + 39 * S, 11, FAC_COL[s.fac] || COL.hud, 'left', 700);
    text(fmtDist(d), x + w - 10 * S, y + 22 * S, 14, COL.hud, 'right', 700);
    const sh = s.shMax > 0 ? (s.shF + s.shB) / s.shMax : 0;
    if (s.shMax > 0) { text(tr('cz_hud_shd'), x + 12 * S, y + 59 * S, 10, COL.shield); bar(x + 50 * S, y + 52 * S, w - 62 * S, 6 * S, sh, COL.shield); }
    text(tr('cz_hud_hull'), x + 12 * S, y + 76 * S, 10, COL.hull); bar(x + 50 * S, y + 69 * S, w - 62 * S, 6 * S, s.hull / s.hullMax, s.hull < s.hullMax * 0.3 ? COL.bad : COL.hull);
  }
  function drawBottom(p) {
    const y0 = H - 22 * S;
    // throttle (left)
    const bx = 26 * S, bh = 130 * S, bw = 12 * S, by = y0 - bh;
    g.fillStyle = COL.faint; g.fillRect(bx, by, bw, bh);
    g.fillStyle = 'rgba(120,240,160,0.22)'; g.fillRect(bx - 3 * S, by + bh * (1 - 0.65), bw + 6 * S, bh * 0.3);
    g.fillStyle = p.boosting ? COL.warn : COL.hud; g.fillRect(bx, by + bh * (1 - p.thr), bw, bh * p.thr);
    const sf = Math.min(1, p.spd / (p.cls.boostSpd || 300));
    g.fillStyle = '#fff'; g.fillRect(bx - 5 * S, by + bh * (1 - sf) - 1 * S, bw + 10 * S, 2 * S);
    const kms = p.spd >= 1000;
    text(kms ? (p.spd / 1000).toFixed(2) : String(Math.round(p.spd)), bx + bw + 10 * S, by + 14 * S, 22, F.cruise === 2 ? COL.shield : '#fff', 'left', 800);
    text(kms ? 'km/s' : 'm/s', bx + bw + 10 * S, by + 28 * S, 10, COL.dim);
    if (F.cruise) {
      const k = F.cruise === 1 ? 1 - Math.max(0, F.cruiseT) / 1.4 : 1;
      text(tr(F.cruise === 1 ? 'cz_hud_cruise_spool' : 'cz_hud_cruise'), W / 2, H * 0.68, 15, COL.shield, 'center', 800);
      bar(W / 2 - 70 * S, H * 0.68 + 7 * S, 140 * S, 3 * S, k, COL.shield);
    }
    text(tr('cz_hud_boost'), bx + bw + 10 * S, by + 50 * S, 10, COL.dim);
    bar(bx + bw + 10 * S, by + 55 * S, 70 * S, 4 * S, p.boost, p.boostLock ? COL.bad : COL.warn);
    if (p.drift) text(tr('cz_hud_drift'), bx + bw + 10 * S, by + 74 * S, 12, COL.gold, 'left', 800);
    // power pips
    const px0 = bx + bw + 96 * S, labels = ['cz_hud_eng', 'cz_hud_wpn', 'cz_hud_shd'], cols = [COL.eng, COL.wpn, COL.shd];
    for (let k = 0; k < 3; k++) {
      const x = px0 + k * 34 * S;
      for (let i = 0; i < 4; i++) { g.fillStyle = i < p.pips[k] ? cols[k] : COL.faint; g.fillRect(x, y0 - 20 * S - i * 13 * S, 22 * S, 9 * S); }
      text(tr(labels[k]), x + 11 * S, y0, 10, cols[k], 'center', 700);
      text(String(k + 1), x + 11 * S, y0 - 76 * S, 10, COL.dim, 'center');
    }
    // ship status (right): shields front/back arcs around a hull icon
    const sx = W - 110 * S, sy = y0 - 62 * S, sr = 44 * S;
    const ff = p.shF / Math.max(1, p.shMax * (p.shFocus === 1 ? 0.7 : p.shFocus === -1 ? 0.3 : 0.5)), fb = p.shB / Math.max(1, p.shMax * (p.shFocus === -1 ? 0.7 : p.shFocus === 1 ? 0.3 : 0.5));
    arc(sx, sy, sr, Math.PI * 1.1, Math.PI * 1.9, COL.faint, 6); if (ff > 0.01) arc(sx, sy, sr, Math.PI * 1.1, Math.PI * 1.1 + Math.PI * 0.8 * Math.min(1, ff), COL.shield, 6);
    arc(sx, sy, sr, Math.PI * 0.1, Math.PI * 0.9, COL.faint, 6); if (fb > 0.01) arc(sx, sy, sr, Math.PI * 0.9 - Math.PI * 0.8 * Math.min(1, fb), Math.PI * 0.9, COL.shield, 6);
    if (p.shFocus) text(p.shFocus === 1 ? '▲' : '▼', sx + sr + 8 * S, sy + 4 * S, 11, COL.shield);
    g.fillStyle = COL.hud; g.beginPath(); g.moveTo(sx, sy - 16 * S); g.lineTo(sx + 11 * S, sy + 12 * S); g.lineTo(sx, sy + 7 * S); g.lineTo(sx - 11 * S, sy + 12 * S); g.closePath(); g.fill();
    const hf = p.hull / p.hullMax;
    text(Math.max(0, Math.round(hf * 100)) + '%', sx, sy + 38 * S, 20, hf < 0.3 ? COL.bad : '#fff', 'center', 800);
    text(tr('cz_hud_hull'), sx, sy + 51 * S, 10, COL.dim, 'center');
    bar(sx - 50 * S, y0 + 2 * S, 100 * S, 4 * S, hf, hf < 0.3 ? COL.bad : COL.hull);
    // ordnance
    const ox = W - 230 * S;
    text(tr('cz_hud_msl'), ox, y0 - 54 * S, 10, COL.dim);
    for (let i = 0; i < Math.max(p.msl, 4); i++) { g.fillStyle = i < p.msl ? COL.gold : COL.faint; g.fillRect(ox + i * 12 * S, y0 - 48 * S, 8 * S, 16 * S); }
    text(tr('cz_hud_flr'), ox, y0 - 16 * S, 10, COL.dim);
    for (let i = 0; i < 3; i++) { g.fillStyle = i < p.flares ? '#ff7adf' : COL.faint; g.beginPath(); g.arc(ox + 5 * S + i * 13 * S, y0 - 4 * S, 4.5 * S, 0, 6.29); g.fill(); }
  }
  function drawRadar(p) {
    const r = 46 * S, cy = H - r - 18 * S, cxs = [W / 2 - r - 8 * S, W / 2 + r + 8 * S];
    for (let k = 0; k < 2; k++) { g.fillStyle = 'rgba(6,14,24,0.5)'; g.beginPath(); g.arc(cxs[k], cy, r, 0, 6.29); g.fill(); arc(cxs[k], cy, r, 0, 6.29, COL.faint, 1); arc(cxs[k], cy, r * 0.5, 0, 6.29, COL.faint, 1); }
    text(tr('cz_hud_front'), cxs[0], cy - r - 5 * S, 9, COL.dim, 'center'); text(tr('cz_hud_rear'), cxs[1], cy - r - 5 * S, 9, COL.dim, 'center');
    const q = p.q;
    for (const s of F.ships) {
      if (!s.alive || s.isPlayer || s.st === ST.DARK) continue;
      invRot(q, s.pos.x - p.pos.x, s.pos.y - p.pos.y, s.pos.z - p.pos.z);
      const d = Math.hypot(RV.x, RV.y, RV.z) || 1; if (d > 6000) continue;
      const front = RV.z < 0, k = front ? 0 : 1;
      const ang = Math.acos(Math.min(1, Math.abs(RV.z) / d)) / (Math.PI / 2);   // 0 = dead ahead/behind
      const pl = Math.hypot(RV.x, RV.y) || 1;
      const x = cxs[k] + RV.x / pl * ang * r * (front ? 1 : -1), y = cy - RV.y / pl * ang * r;
      const col = s.objective ? COL.gold : s.team === TEAM_E ? COL.hostile : COL.friend;
      const sz = (s.cap ? 3.5 : s === p.target ? 3 : 2) * S;
      g.fillStyle = col; g.globalAlpha = Math.max(0.35, 1 - d / 6000);
      g.fillRect(x - sz, y - sz, sz * 2, sz * 2);
      if (s === p.target) { g.strokeStyle = '#fff'; g.lineWidth = 1 * S; g.strokeRect(x - sz - 2 * S, y - sz - 2 * S, sz * 2 + 4 * S, sz * 2 + 4 * S); }
    }
    g.globalAlpha = 1 - flick * 0.6;
  }
  const RV = { x: 0, y: 0, z: 0 };
  function invRot(q, x, y, z) { const qx = -q.x, qy = -q.y, qz = -q.z, qw = q.w; const ix = qw * x + qy * z - qz * y, iy = qw * y + qz * x - qx * z, iz = qw * z + qx * y - qy * x, iw = -qx * x - qy * y - qz * z; RV.x = ix * qw + iw * -qx + iy * -qz - iz * -qy; RV.y = iy * qw + iw * -qy + iz * -qx - ix * -qz; RV.z = iz * qw + iw * -qz + ix * -qy - iy * -qx; }
  function drawTracker() {
    const x = 18 * S; let y = 30 * S;
    const M = F.mission;
    text(tr('cz_mk_' + M.kind), x, y, 11, COL.gold, 'left', 800); y += 20 * S;
    const ot = objText(F); if (ot) { text(ot, x, y, 15, '#fff', 'left', 700); y += 18 * S; }
    if (F.waves > 1 && M.kind !== 'escort') { text(tr('cz_hud_wave', { w: Math.max(1, Math.min(F.wave, F.waves)), m: F.waves }), x, y, 11, COL.dim); y += 16 * S; }
    for (const s of F.obj.bars) {
      const lab = s.ck === 'hauler' ? tr('cz_ship_hauler') + ' ' + s.name : tr(s.nameKey);
      text(lab, x, y + 9 * S, 11, s.alive ? COL.gold : COL.dim); bar(x + 90 * S, y + 3 * S, 120 * S, 5 * S, s.alive ? s.hull / s.hullMax : 0, s.alive ? COL.gold : COL.faint); y += 16 * S;
    }
    const p = F.player;
    text(tr('cz_hud_kills', { n: F.kills }) + '   ' + '+' + F.earned + ' cr', x, y + 10 * S, 11, COL.dim);
  }

  // ---------------------------------------------------------------- api
  function frame(F_, camera, dt, paused) {
    F = F_; cam = camera; active = true;
    root.style.display = 'block';
    if (!cam) { g.setTransform(1, 0, 0, 1, 0, 0); g.clearRect(0, 0, W, H); return; }
    if (!paused && !F.outcome) pollInput(dt); else if (F.input) { F.input.fire = false; F.input.boost = false; }
    if (F.outcome) { F.input.fire = false; }
    for (let i = msgs.length - 1; i >= 0; i--) { const m = msgs[i]; m.t += dt; if (m.t > m.life) m.el.classList.add('out'); if (m.t > m.life + 0.7) { m.el.remove(); msgs.splice(i, 1); } }
    if (banT > 0) { banT -= dt; if (banT <= 0) banEl.style.opacity = 0; }
    if (cardT > 0) { cardT -= dt; if (cardT <= 0 || (F.input.touched && F.briefT <= 0.4)) { cardEl.classList.add('out'); cardT = 0; } }
    draw(dt);
  }
  function resize(w, h) {
    dpr = Math.min(2, window.devicePixelRatio || 1);
    const r = canvas.getBoundingClientRect();
    const cw = Math.max(2, Math.round((r.width || w) * dpr)), ch = Math.max(2, Math.round((r.height || h) * dpr));
    cv.width = cw; cv.height = ch; W = cw; H = ch;
    S = Math.max(0.8, Math.min(2.4, Math.min(cw / 1280, ch / 760)));
    fonts.clear();
  }
  function begin(F_, m) {
    F = F_; model = m; active = true; isPaused = false; pauseEl.classList.remove('on'); aimInit = false; keys.clear(); mouseFire = false;
    commsEl.innerHTML = ''; msgs.length = 0; notes.length = 0; pops.length = 0; hurts.length = 0; endT = -1; banEl.style.opacity = 0;
    root.style.display = 'block';
    showCard(F_, m);
  }
  return {
    setOptions(o) { opts = o || {}; },
    resize, begin, frame,
    hide() { root.style.display = 'none'; active = false; },
    stop() { active = false; F = null; isPaused = false; pauseEl.classList.remove('on'); root.style.display = 'none'; },
    paused: () => isPaused,
    hitMarker(kill) { hitT = 0.22; hitKill = !!kill; },
    kill(s, v) { hitT = 0.3; hitKill = true; if (v) pops.push({ x: s.pos.x, y: s.pos.y, z: s.pos.z, v, t: 1.6 }); if (s.cap) banner(tr('cz_hud_capkill', { name: CLS[s.ck].name }), COL.gold, 2.4); else if (s.ace) banner(tr('cz_hud_acekill', { name: s.name }), COL.gold, 2.4); },
    flashLock() { lockFlash = 1; },
    comms: commsLine,
    subsys(s, k) { note('cz_hud_subdown', 'good', { sub: tr('cz_sub_' + k) }); },
    hurt(x, y, z, hull) { hurts.push({ x, y, z, t: 1, hull }); if (hurts.length > 6) hurts.shift(); },
    note, pickup(k) { note(['cz_pick_missile', 'cz_pick_shield', 'cz_pick_repair'][k] || 'cz_pick_repair', 'good'); },
    wave(a, b) { if (a > 1 || b > 1) banner(tr('cz_hud_wave', { w: a, m: b }), COL.warn, 1.8); },
    end(r) { if (r == null) return; endRes = r; banner(tr(r === 1 ? 'cz_hud_complete' : r === 2 ? 'cz_hud_destroyed' : 'cz_hud_retreat'), r === 1 ? COL.gold : r === 2 ? COL.bad : COL.hud, 4); },
    dispose() {
      document.removeEventListener('pointerlockchange', onLockChange);
      window.removeEventListener('keydown', onKeyDown, true); window.removeEventListener('keyup', onKeyUp, true); document.removeEventListener('mousemove', onMouseMove);
      canvas.removeEventListener('mousedown', onMouseDown); window.removeEventListener('mouseup', onMouseUp); canvas.removeEventListener('wheel', onWheel); window.removeEventListener('blur', blur);
      root.remove();
    },
  };
}
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

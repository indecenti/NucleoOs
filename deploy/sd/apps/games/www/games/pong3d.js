// pong3d.js — the Three.js WebGL renderer for Pong. Driven purely from the host-authoritative state in
// pong.js: it maps the 200×120 logic field onto a neon 3D arena, interpolates the ball/paddles for
// 60fps smoothness over 30Hz snapshots, and turns per-tick events (st.ev) into particle bursts, camera
// shake and procedural SFX. Real WebGL 3D — perspective, lighting, emissive bloom-style glow, fog,
// additive trails. If WebGL is unavailable it transparently falls back to an enhanced 2D canvas.
//
//   const r = await createPongRenderer(canvas, api, geom);  // geom = {FW,FH,PH,PW,R,X0,X1}
//   r.frame(state, api);   // called every animation frame by the harness (state may be null)
//   r.resize(cssW, cssH);  // CSS px; renderer applies devicePixelRatio
//   r.dispose();
import { SFX } from '/apps/games/games/pong-sfx.js';

const COL0 = 0x22d3ee, COL1 = 0xe879f9;   // seat colours (cyan / magenta)
const KIND = {
  grow:   { color: 0x34d399, label: 'INGRANDISCI', icon: '⬆' },
  shrink: { color: 0xfb7185, label: 'RIMPICCIOLISCI', icon: '⬇' },
  fast:   { color: 0xfbbf24, label: 'PALLA VELOCE', icon: '⚡' },
  shield: { color: 0x60a5fa, label: 'SCUDO', icon: '🛡' },
};

export async function createPongRenderer(canvas, api, geom) {
  // First getContext wins: try WebGL, else hand off to the 2D fallback (it grabs '2d' itself).
  let gl = null;
  try { gl = canvas.getContext('webgl2', { antialias: true, alpha: false }) || canvas.getContext('webgl', { antialias: true, alpha: false }); } catch {}
  if (!gl) return createFallback2D(canvas, geom);
  let THREE;
  try { THREE = await import('/apps/games/vendor/three.module.min.js'); }
  catch (e) { console.warn('three.js unavailable → 2D fallback', e); return createFallback2D(canvas, geom); }
  return build3D(THREE, canvas, gl, api, geom);
}

// ── world mapping ──────────────────────────────────────────────────────────────────────────────
function build3D(THREE, canvas, gl, api, geom) {
  const { FW, FH, X0, X1, PH, PW, R } = geom;
  const S = 30 / FW;                       // logic-units → world (arena ≈30 wide × 18 deep)
  const wx = x => (x - FW / 2) * S, wz = y => (y - FH / 2) * S;
  const HW = wx(FW), HD = wz(FH);           // half extents in world

  const renderer = new THREE.WebGLRenderer({ canvas, context: gl, antialias: true });
  renderer.setPixelRatio(Math.min(2, window.devicePixelRatio || 1));
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.ACESFilmicToneMapping; renderer.toneMappingExposure = 1.2;

  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0x05070d);
  scene.fog = new THREE.Fog(0x05070d, 34, 70);

  const camera = new THREE.PerspectiveCamera(52, 16 / 9, 0.1, 200);
  const camBase = new THREE.Vector3(0, 17, 23), camLook = new THREE.Vector3(0, 0, -1.5);

  // glow sprite texture (reused for halos, trail, particles)
  const glowTex = makeGlow(THREE);
  const sprite = (color, scale, opacity = 1) => { const m = new THREE.SpriteMaterial({ map: glowTex, color, blending: THREE.AdditiveBlending, transparent: true, depthWrite: false, opacity }); const s = new THREE.Sprite(m); s.scale.setScalar(scale); return s; };

  // lights
  scene.add(new THREE.AmbientLight(0x33415c, 0.7));
  const key = new THREE.DirectionalLight(0x88aaff, 0.5); key.position.set(6, 20, 10); scene.add(key);
  const lL = new THREE.PointLight(COL0, 18, 26, 2); scene.add(lL);
  const lR = new THREE.PointLight(COL1, 18, 26, 2); scene.add(lR);
  const lB = new THREE.PointLight(0xffffff, 14, 22, 2); scene.add(lB);

  // floor + grid + centre line  (HW/HD are positive half-extents; arena = 2·HW wide × 2·HD deep)
  const floor = new THREE.Mesh(new THREE.PlaneGeometry(HW * 2 + 6, HD * 2 + 4), new THREE.MeshStandardMaterial({ color: 0x0a0f1a, metalness: 0.6, roughness: 0.35 }));
  floor.rotation.x = -Math.PI / 2; floor.position.y = -0.05; scene.add(floor);
  const grid = new THREE.GridHelper(Math.max(HW, HD) * 2.2, 26, 0x1d6fa5, 0x122436); grid.position.y = 0.0; grid.material.transparent = true; grid.material.opacity = 0.5; scene.add(grid);
  const net = new THREE.Mesh(new THREE.PlaneGeometry(0.16, HD * 2), new THREE.MeshBasicMaterial({ color: 0x2a4f6a, transparent: true, opacity: 0.5, side: THREE.DoubleSide })); net.rotation.x = -Math.PI / 2; net.position.y = 0.02; scene.add(net);

  // side rails (where the ball bounces, z = ±HD)
  const railMat = new THREE.MeshStandardMaterial({ color: 0x0e7490, emissive: 0x22d3ee, emissiveIntensity: 1.1, metalness: 0.5, roughness: 0.3 });
  for (const z of [HD, -HD]) { const rail = new THREE.Mesh(new THREE.BoxGeometry(HW * 2 + 2, 0.5, 0.5), railMat.clone()); rail.position.set(0, 0.25, z); scene.add(rail); }
  // goal glows (left/right out-zones, tinted per side)
  const goalL = new THREE.Mesh(new THREE.PlaneGeometry(2.4, HD * 2 + 1), new THREE.MeshBasicMaterial({ color: COL0, transparent: true, opacity: 0.12, side: THREE.DoubleSide })); goalL.rotation.y = Math.PI / 2; goalL.position.set(-HW - 0.6, 1.2, 0); scene.add(goalL);
  const goalR = new THREE.Mesh(new THREE.PlaneGeometry(2.4, HD * 2 + 1), new THREE.MeshBasicMaterial({ color: COL1, transparent: true, opacity: 0.12, side: THREE.DoubleSide })); goalR.rotation.y = Math.PI / 2; goalR.position.set(HW + 0.6, 1.2, 0); scene.add(goalR);

  // paddles
  const mkPaddle = (col) => { const g = new THREE.Group(); const body = new THREE.Mesh(new THREE.BoxGeometry(0.55, 2.0, PH * S), new THREE.MeshStandardMaterial({ color: col, emissive: col, emissiveIntensity: 0.9, metalness: 0.4, roughness: 0.25 })); g.add(body); const halo = sprite(col, 5, 0.6); halo.position.set(0, 0.6, 0); g.add(halo); g.userData = { body, halo }; scene.add(g); return g; };
  const padL = mkPaddle(COL0), padR = mkPaddle(COL1);
  padL.position.set(wx(X0), 1.0, 0); padR.position.set(wx(X1), 1.0, 0);

  // ball + halo + light + trail
  const ball = new THREE.Mesh(new THREE.SphereGeometry(R * S * 1.15, 24, 24), new THREE.MeshStandardMaterial({ color: 0xffffff, emissive: 0xbfe9ff, emissiveIntensity: 1.4, metalness: 0.2, roughness: 0.1 }));
  ball.position.set(0, R * S + 0.4, 0); scene.add(ball);
  const ballHalo = sprite(0xbfe9ff, 4.2, 0.9); scene.add(ballHalo);
  const TRAIL = 16, trail = []; for (let i = 0; i < TRAIL; i++) { const s = sprite(0x9fe4ff, 2.2, 0); scene.add(s); trail.push(s); }

  // particle pool
  const PMAX = 170, pool = []; for (let i = 0; i < PMAX; i++) { const s = sprite(0xffffff, 1, 0); s.visible = false; scene.add(s); pool.push({ s, life: 0, max: 1, vx: 0, vy: 0, vz: 0 }); }
  let pHead = 0;
  function burst(pos, color, n, power) {
    for (let k = 0; k < n; k++) {
      const p = pool[pHead++ % PMAX]; const a = Math.random() * Math.PI * 2, e = Math.random() * Math.PI - Math.PI / 2, sp = power * (0.4 + Math.random());
      p.vx = Math.cos(a) * Math.cos(e) * sp; p.vy = Math.abs(Math.sin(e)) * sp * 0.8 + 1; p.vz = Math.sin(a) * Math.cos(e) * sp;
      p.life = p.max = 0.5 + Math.random() * 0.5; p.s.material.color.setHex(color); p.s.position.copy(pos); p.s.visible = true; p.s.scale.setScalar(0.6 + Math.random() * 0.8);
    }
  }

  // power-up orbs (keyed by orb id)
  const orbMap = new Map();
  function syncOrbs(orbs) {
    const seen = new Set();
    for (const o of (orbs || [])) {
      seen.add(o.id); let m = orbMap.get(o.id);
      if (!m) { const cfg = KIND[o.kind] || KIND.grow; const core = new THREE.Mesh(new THREE.IcosahedronGeometry(0.8, 0), new THREE.MeshStandardMaterial({ color: cfg.color, emissive: cfg.color, emissiveIntensity: 1.3, metalness: 0.3, roughness: 0.2, flatShading: true })); const halo = sprite(cfg.color, 4.5, 0.8); const g = new THREE.Group(); g.add(core); g.add(halo); scene.add(g); m = { g, core }; orbMap.set(o.id, m); }
      m.g.position.set(wx(o.x), 1.1 + Math.sin(performance.now() * 0.003 + o.id) * 0.3, wz(o.y));
      m.core.rotation.x += 0.03; m.core.rotation.y += 0.04;
    }
    for (const [id, m] of orbMap) if (!seen.has(id)) { scene.remove(m.g); m.core.geometry.dispose(); orbMap.delete(id); }
  }

  // starfield depth
  const stars = (() => { const n = 220, g = new THREE.BufferGeometry(), pos = new Float32Array(n * 3); for (let i = 0; i < n; i++) { pos[i * 3] = (Math.random() - 0.5) * 120; pos[i * 3 + 1] = Math.random() * 40 - 6; pos[i * 3 + 2] = (Math.random() - 0.5) * 120 - 30; } g.setAttribute('position', new THREE.BufferAttribute(pos, 3)); const pts = new THREE.Points(g, new THREE.PointsMaterial({ color: 0x335577, size: 0.18, transparent: true, opacity: 0.7 })); scene.add(pts); return pts; })();

  // HUD overlay (crisp DOM text over the canvas)
  const hud = makeHud(canvas);
  const sfx = new SFX(); hud.bindSfx(sfx);

  // interpolation + bookkeeping
  const disp = { bx: 0, by: 0, pL: 0, pR: 0 };
  let lastTk = -1, lastT = performance.now(), shake = 0, idle = 0, finished = false, ready = false;

  function fit(cssW, cssH) {
    const w = Math.max(2, cssW | 0), h = Math.max(2, cssH | 0);
    renderer.setPixelRatio(Math.min(2, window.devicePixelRatio || 1));
    renderer.setSize(w, h, false);
    camera.aspect = w / h; camera.updateProjectionMatrix();
  }
  fit(canvas.clientWidth || 960, canvas.clientHeight || 540);

  function events(st) {
    for (const e of (st.ev || [])) {
      const col = e.seat === st.seats[1] ? COL1 : COL0;
      const at = new THREE.Vector3(wx(e.x), (R * S + 0.4), wz(e.y));
      if (e.t === 'hit') { burst(at, col, 14, 7 + (e.speed || 130) / 30); sfx.hit(e.speed, e.combo); shake = Math.min(1, shake + 0.25); if (e.combo >= 4) hud.flashCombo(e.combo); }
      else if (e.t === 'wall') { burst(new THREE.Vector3(wx(e.x ?? FW / 2), R * S + 0.4, wz(e.y)), 0x22d3ee, 6, 5); sfx.wall(); shake = Math.min(1, shake + 0.08); }
      else if (e.t === 'orb') { const cfg = KIND[e.kind] || KIND.grow; burst(at, cfg.color, 28, 9); sfx.orb(e.kind); hud.banner(`${cfg.icon} ${cfg.label}`, '#' + cfg.color.toString(16).padStart(6, '0')); shake = Math.min(1, shake + 0.18); }
      else if (e.t === 'shield') { burst(at, 0x60a5fa, 24, 8); sfx.shield(); hud.banner('🛡 SCUDO!', '#60a5fa'); shake = Math.min(1, shake + 0.2); }
      else if (e.t === 'score') { const c = e.who === 1 ? COL1 : COL0; burst(new THREE.Vector3(wx(e.x), 1.2, wz(e.y)), c, 46, 12); sfx.score(true); shake = Math.min(1.4, shake + 0.7); hud.flashScore(e.who); }
    }
  }

  function frame(st) {
    const now = performance.now(), dt = Math.min(0.05, (now - lastT) / 1000); lastT = now; idle += dt;
    ready = true;
    if (st) {
      if (st.tk !== lastTk) { events(st); lastTk = st.tk; hud.update(st, api); syncOrbs(st.orbs); }
      // interpolate toward authoritative positions
      const bx = wx(st.ball.x), by = wz(st.ball.y), s0 = st.seats[0], s1 = st.seats[1];
      disp.bx += (bx - disp.bx) * 0.5; disp.by += (by - disp.by) * 0.5;
      disp.pL += (wz(st.paddles[s0].y) - disp.pL) * 0.4; disp.pR += (wz(st.paddles[s1].y) - disp.pR) * 0.4;
      ball.position.set(disp.bx, R * S + 0.4, disp.by);
      // paddle scale (power-ups)
      const scL = (st.fx[s0] && st.fx[s0].paddleScale) || 1, scR = (st.fx[s1] && st.fx[s1].paddleScale) || 1;
      padL.position.z = disp.pL; padR.position.z = disp.pR; padL.userData.body.scale.z = scL; padR.userData.body.scale.z = scR;
      padL.userData.body.material.emissiveIntensity = (st.fx[s0] && st.fx[s0].shield) ? 1.8 : 0.9;
      padR.userData.body.material.emissiveIntensity = (st.fx[s1] && st.fx[s1].shield) ? 1.8 : 0.9;
      const fast = st.clock < st.ballFastUntil; ball.material.emissive.setHex(fast ? 0xffae3b : 0xbfe9ff); ballHalo.material.color.setHex(fast ? 0xffae3b : 0xbfe9ff);
      // finale
      if (!finished && (st.scores[0] >= st.target || st.scores[1] >= st.target)) { finished = true; const win = st.seats[st.scores[0] >= st.target ? 0 : 1] === api.mySeat(); win ? sfx.win() : sfx.lose(); for (let i = 0; i < 5; i++) setTimeout(() => burst(new THREE.Vector3((Math.random() - 0.5) * HW * 1.6, 2 + Math.random() * 3, (Math.random() - 0.5) * HD * 1.6), win ? 0x34d399 : 0xfb7185, 30, 12), i * 140); }
      if (st.scores[0] < st.target && st.scores[1] < st.target) finished = false;
    } else {
      // idle attract: ball drifts in a lazy circle
      ball.position.set(Math.cos(idle * 0.7) * 6, R * S + 0.4, Math.sin(idle * 0.9) * 4);
    }
    // lights track paddles/ball
    lL.position.set(padL.position.x, 3, padL.position.z); lR.position.set(padR.position.x, 3, padR.position.z);
    lB.position.set(ball.position.x, 3, ball.position.z);
    ballHalo.position.copy(ball.position);
    // trail
    for (let i = trail.length - 1; i > 0; i--) { trail[i].position.copy(trail[i - 1].position); trail[i].material.opacity = (1 - i / trail.length) * 0.5; trail[i].scale.setScalar(2.2 * (1 - i / trail.length) + 0.4); }
    trail[0].position.copy(ball.position); trail[0].material.opacity = 0.5;
    // particles
    for (const p of pool) { if (p.life <= 0) { if (p.s.visible) p.s.visible = false; continue; } p.life -= dt; const k = Math.max(0, p.life / p.max); p.vy -= 9 * dt; p.s.position.x += p.vx * dt; p.s.position.y += p.vy * dt; p.s.position.z += p.vz * dt; if (p.s.position.y < 0.1) { p.s.position.y = 0.1; p.vy *= -0.4; } p.s.material.opacity = k; p.s.scale.setScalar(0.3 + k * 0.9); }
    // halo pulse + star drift
    ballHalo.material.opacity = 0.7 + Math.sin(now * 0.012) * 0.2; stars.rotation.y += dt * 0.01;
    // camera: gentle idle orbit + decaying shake
    shake *= Math.pow(0.0008, dt);
    const ang = Math.sin(idle * 0.18) * 0.06;
    camera.position.set(camBase.x + Math.sin(ang) * 4 + (Math.random() - 0.5) * shake * 2.2, camBase.y + (Math.random() - 0.5) * shake * 1.6, camBase.z);
    camera.lookAt(camLook.x, camLook.y + (Math.random() - 0.5) * shake, camLook.z);
    renderer.render(scene, camera);
  }

  function dispose() {
    try { hud.el.remove(); } catch {}
    try { sfx.stopMusic(); } catch {}
    scene.traverse(o => { if (o.geometry) o.geometry.dispose(); if (o.material) { const m = o.material; (Array.isArray(m) ? m : [m]).forEach(x => { if (x.map) x.map.dispose(); x.dispose(); }); } });
    glowTex.dispose(); renderer.dispose();
  }

  return { frame, resize: fit, dispose, isReady: () => ready, sfx, mode: 'webgl' };
}

// radial white glow → CanvasTexture (additive sprites tint it).
function makeGlow(THREE) {
  const c = document.createElement('canvas'); c.width = c.height = 128; const g = c.getContext('2d');
  const grd = g.createRadialGradient(64, 64, 0, 64, 64, 64);
  grd.addColorStop(0, 'rgba(255,255,255,1)'); grd.addColorStop(0.25, 'rgba(255,255,255,0.85)'); grd.addColorStop(0.6, 'rgba(255,255,255,0.18)'); grd.addColorStop(1, 'rgba(255,255,255,0)');
  g.fillStyle = grd; g.fillRect(0, 0, 128, 128);
  const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace; return t;
}

// ── HUD overlay (DOM) ────────────────────────────────────────────────────────────────────────
function makeHud(canvas) {
  const host = canvas.parentNode; const el = document.createElement('div');
  el.style.cssText = 'position:absolute;inset:0;pointer-events:none;font-family:system-ui,Segoe UI,sans-serif;overflow:hidden;z-index:5';
  el.innerHTML = `
    <div data-score style="position:absolute;top:14px;left:0;right:0;text-align:center;font-weight:800;font-size:clamp(28px,7vw,64px);letter-spacing:.06em;text-shadow:0 0 22px rgba(0,0,0,.6)">
      <span data-s0 style="color:#22d3ee">0</span><span style="color:#3a475a;margin:0 .3em">:</span><span data-s1 style="color:#e879f9">0</span>
    </div>
    <div data-names style="position:absolute;top:calc(14px + clamp(34px,8vw,72px));left:0;right:0;text-align:center;font-size:13px;color:#8b97a6;letter-spacing:.04em"></div>
    <div data-combo style="position:absolute;top:34%;left:0;right:0;text-align:center;font-weight:800;font-size:clamp(20px,5vw,40px);color:#fbbf24;opacity:0;transition:opacity .25s;text-shadow:0 0 24px #fbbf24"></div>
    <div data-banner style="position:absolute;bottom:18%;left:0;right:0;text-align:center;font-weight:700;font-size:clamp(16px,4vw,28px);opacity:0;transition:opacity .3s,transform .3s"></div>
    <div data-brain style="position:absolute;left:12px;bottom:10px;font-size:12px;color:#9aa7b6;display:flex;gap:6px;align-items:center"></div>
    <div style="position:absolute;right:10px;top:10px;display:flex;gap:6px;pointer-events:auto">
      <button data-mute title="Audio" style="background:rgba(20,26,34,.7);border:1px solid #2a3340;color:#e6edf3;border-radius:8px;padding:4px 8px;cursor:pointer">🔊</button>
      <button data-music title="Musica" style="background:rgba(20,26,34,.7);border:1px solid #2a3340;color:#8b97a6;border-radius:8px;padding:4px 8px;cursor:pointer">🎵</button>
    </div>`;
  if (getComputedStyle(host).position === 'static') host.style.position = 'relative';
  host.appendChild(el);
  const $ = s => el.querySelector(s);
  let comboTimer = 0, bannerTimer = 0, sfxRef = null;
  const muteBtn = $('[data-mute]'), musicBtn = $('[data-music]');
  muteBtn.onclick = () => { if (!sfxRef) return; const m = muteBtn.textContent === '🔊'; sfxRef.setMuted(m); muteBtn.textContent = m ? '🔇' : '🔊'; };
  musicBtn.onclick = () => { if (!sfxRef) return; const on = sfxRef.toggleMusic(); musicBtn.style.color = on ? '#22d3ee' : '#8b97a6'; };
  let names = '';
  return {
    el, bindSfx(s) { sfxRef = s; },
    update(st, api) {
      $('[data-s0]').textContent = st.scores[0]; $('[data-s1]').textContent = st.scores[1];
      const nm = (st.names || []).join('  ·  '); if (nm !== names) { names = nm; $('[data-names]').textContent = (st.names ? `${st.names[0]}  vs  ${st.names[1]}` : ''); }
      const aiSeat = (st.aiSeats || [])[0];
      const brain = $('[data-brain]'), b = st.brain;
      const persona = (aiSeat != null && st.persona && st.persona[aiSeat]) || '';
      // Authoritative: only claim a live LLM when a real call just succeeded; shout when it's down.
      if (b && b.status === 'live') brain.innerHTML = `<span style="color:#34d399">🟢 ${escapeHtml(b.label || 'LLM')}</span>${b.ms ? ` <span style="color:#5b6b7d">${b.ms}ms</span>` : ''}${persona ? ' · ' + escapeHtml(persona) : ''}`;
      else if (b && b.status === 'down') brain.innerHTML = `<span style="color:#fb7185">⛔ ${escapeHtml(b.label || 'LLM')} non raggiungibile — partita in pausa</span>`;
      else if (b && b.status === 'connecting') brain.innerHTML = `<span style="color:#fbbf24">… connessione ${escapeHtml(b.label || 'LLM')}</span>`;
      else if (aiSeat != null) brain.innerHTML = `<span style="color:#8b97a6">🤖</span> Avversario classico (offline)`;
      else brain.textContent = '';
    },
    flashScore(who) { const e = $(who === 1 ? '[data-s1]' : '[data-s0]'); e.animate([{ transform: 'scale(1.6)', filter: 'brightness(2)' }, { transform: 'scale(1)', filter: 'brightness(1)' }], { duration: 500, easing: 'ease-out' }); },
    flashCombo(n) { const c = $('[data-combo]'); c.textContent = `RALLY ×${n}`; c.style.opacity = 1; clearTimeout(comboTimer); comboTimer = setTimeout(() => c.style.opacity = 0, 700); },
    banner(text, color) { const b = $('[data-banner]'); b.textContent = text; b.style.color = color; b.style.textShadow = `0 0 24px ${color}`; b.style.opacity = 1; b.style.transform = 'translateY(0)'; clearTimeout(bannerTimer); bannerTimer = setTimeout(() => { b.style.opacity = 0; b.style.transform = 'translateY(10px)'; }, 1400); },
  };
}
function escapeHtml(s) { return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }

// ── 2D fallback (no WebGL): still pretty — glow, trail, shake — so the game never breaks ─────────
function createFallback2D(canvas, geom) {
  const { FW, FH, X0, X1, PH, PW, R } = geom; const ctx = canvas.getContext('2d');
  const trail = []; let shake = 0, lastTk = -1; const sfx = new SFX();
  function fit() {}
  function frame(st) {
    const W = canvas.width, H = canvas.height, sx = W / FW, sy = H / FH;
    ctx.fillStyle = '#05070d'; ctx.fillRect(0, 0, W, H);
    const ox = (Math.random() - 0.5) * shake * 10, oy = (Math.random() - 0.5) * shake * 10; shake *= 0.9;
    ctx.save(); ctx.translate(ox, oy);
    ctx.strokeStyle = 'rgba(42,79,106,.6)'; ctx.setLineDash([8, 10]); ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(W / 2, 0); ctx.lineTo(W / 2, H); ctx.stroke(); ctx.setLineDash([]);
    if (st) {
      if (st.tk !== lastTk) { for (const e of (st.ev || [])) { if (e.t === 'hit') { sfx.hit(e.speed, e.combo); shake = .6; } else if (e.t === 'wall') sfx.wall(); else if (e.t === 'score') { sfx.score(); shake = 1; } else if (e.t === 'orb') sfx.orb(e.kind); else if (e.t === 'shield') sfx.shield(); } lastTk = st.tk; }
      const s0 = st.seats[0], s1 = st.seats[1];
      const drawP = (seat, col, px) => { const p = st.paddles[seat], ph = PH * ((st.fx[seat] && st.fx[seat].paddleScale) || 1); ctx.shadowColor = col; ctx.shadowBlur = 24; ctx.fillStyle = col; ctx.fillRect((px - PW / 2) * sx, (p.y - ph / 2) * sy, PW * sx, ph * sy); };
      drawP(s0, '#22d3ee', X0); drawP(s1, '#e879f9', X1);
      for (const o of (st.orbs || [])) { ctx.shadowColor = '#fbbf24'; ctx.shadowBlur = 20; ctx.fillStyle = '#fbbf24'; ctx.beginPath(); ctx.arc(o.x * sx, o.y * sy, R * 2 * sx, 0, 7); ctx.fill(); }
      trail.push({ x: st.ball.x * sx, y: st.ball.y * sy }); if (trail.length > 14) trail.shift();
      trail.forEach((t, i) => { ctx.globalAlpha = i / trail.length * 0.5; ctx.fillStyle = '#9fe4ff'; ctx.beginPath(); ctx.arc(t.x, t.y, R * sx * (i / trail.length), 0, 7); ctx.fill(); }); ctx.globalAlpha = 1;
      ctx.shadowColor = '#bfe9ff'; ctx.shadowBlur = 26; ctx.fillStyle = '#fff'; ctx.beginPath(); ctx.arc(st.ball.x * sx, st.ball.y * sy, R * sx, 0, 7); ctx.fill(); ctx.shadowBlur = 0;
      ctx.fillStyle = '#e6edf3'; ctx.font = `bold ${Math.round(H * 0.12)}px system-ui`; ctx.textAlign = 'center';
      ctx.fillText(`${st.scores[0]} : ${st.scores[1]}`, W / 2, H * 0.16);
    }
    ctx.restore();
  }
  return { frame, resize: fit, dispose() {}, isReady: () => true, sfx, mode: '2d' };
}

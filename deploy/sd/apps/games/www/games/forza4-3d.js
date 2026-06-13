// forza4-3d.js — the Three.js/WebGL renderer for Forza 4, built on the SAME engine as Pong (pong3d.js):
// a neon 3D board, emissive discs that drop and bounce, particle bursts, a glowing winning-line tube,
// camera shake and the procedural Web-Audio SFX. Driven purely from the host-authoritative state in
// forza4.js — it animates the single freshly-dropped disc (state.last + state.moves) and celebrates the
// winning four (state.win). If WebGL is unavailable it transparently falls back to an enhanced 2D canvas.
//
//   const r = await createForza4Renderer(canvas, api, { COLS, ROWS });
//   r.frame(state, api);        // every animation frame (state may be null → idle attract)
//   r.screenToCol(nx, ny);      // normalized canvas coords → column under the cursor (or -1)
//   r.resize(cssW, cssH); r.dispose();
import { SFX } from '/apps/games/games/pong-sfx.js';

const COL0 = 0xff5a6e, COL1 = 0xffd23f;   // seat colours (neon red / amber)
const colOf = (st, v) => (v === st.seats[0] ? COL0 : COL1);

export async function createForza4Renderer(canvas, api, geom) {
  let gl = null;
  try { gl = canvas.getContext('webgl2', { antialias: true, alpha: false }) || canvas.getContext('webgl', { antialias: true, alpha: false }); } catch {}
  if (!gl) return createFallback2D(canvas, geom);
  let THREE;
  try { THREE = await import('/apps/games/vendor/three.module.min.js'); }
  catch (e) { console.warn('three.js unavailable → 2D fallback', e); return createFallback2D(canvas, geom); }
  return build3D(THREE, canvas, gl, api, geom);
}

function build3D(THREE, canvas, gl, api, geom) {
  const { COLS, ROWS } = geom;
  const CELL = 2.0, W = COLS * CELL, H = ROWS * CELL;
  const LEFT = -W / 2, TOP = H / 2;
  const DISC_R = CELL * 0.4, DISC_Z = 0.25, HOLE_Z = 0.05, PANEL_Z = -0.55;
  const cellX = c => LEFT + (c + 0.5) * CELL, cellY = r => TOP - (r + 0.5) * CELL;
  const idx = (r, c) => r * COLS + c;

  const renderer = new THREE.WebGLRenderer({ canvas, context: gl, antialias: true });
  renderer.setPixelRatio(Math.min(2, window.devicePixelRatio || 1));
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.ACESFilmicToneMapping; renderer.toneMappingExposure = 1.2;

  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0x05070d);
  scene.fog = new THREE.Fog(0x05070d, 30, 80);

  const camera = new THREE.PerspectiveCamera(50, 16 / 9, 0.1, 200);
  const camBase = new THREE.Vector3(0, 0.4, 16.5), camLook = new THREE.Vector3(0, 0, 0);

  const glowTex = makeGlow(THREE);
  const sprite = (color, scale, opacity = 1) => { const m = new THREE.SpriteMaterial({ map: glowTex, color, blending: THREE.AdditiveBlending, transparent: true, depthWrite: false, opacity }); const s = new THREE.Sprite(m); s.scale.setScalar(scale); return s; };

  // lights
  scene.add(new THREE.AmbientLight(0x33415c, 0.85));
  const key = new THREE.DirectionalLight(0x9fc0ff, 0.55); key.position.set(5, 14, 14); scene.add(key);
  const lL = new THREE.PointLight(COL0, 12, 36, 2); lL.position.set(LEFT - 2, 0, 8); scene.add(lL);
  const lR = new THREE.PointLight(COL1, 12, 36, 2); lR.position.set(-LEFT + 2, 0, 8); scene.add(lR);
  const dropLight = new THREE.PointLight(0xffffff, 0, 18, 2); scene.add(dropLight);

  // backboard panel (blue), a darker frame behind it, a floor for grounding
  const panel = new THREE.Mesh(new THREE.BoxGeometry(W + 1.0, H + 1.0, 0.9), new THREE.MeshStandardMaterial({ color: 0x1d4ed8, emissive: 0x1e3a8a, emissiveIntensity: 0.45, metalness: 0.55, roughness: 0.35 }));
  panel.position.z = PANEL_Z; scene.add(panel);
  const bezel = new THREE.Mesh(new THREE.BoxGeometry(W + 1.8, H + 1.8, 0.6), new THREE.MeshStandardMaterial({ color: 0x0b1f4d, metalness: 0.6, roughness: 0.4 }));
  bezel.position.z = PANEL_Z - 0.5; scene.add(bezel);
  const floor = new THREE.Mesh(new THREE.PlaneGeometry(60, 40), new THREE.MeshStandardMaterial({ color: 0x070b14, metalness: 0.6, roughness: 0.5 }));
  floor.rotation.x = -Math.PI / 2; floor.position.y = -TOP - 1.4; scene.add(floor);
  const grid = new THREE.GridHelper(60, 30, 0x1d6fa5, 0x122436); grid.position.y = -TOP - 1.38; grid.material.transparent = true; grid.material.opacity = 0.45; scene.add(grid);

  // empty sockets (dark recessed discs) — the "holes". Static; filled discs sit in front of them.
  const holeGeo = new THREE.CylinderGeometry(DISC_R * 1.04, DISC_R * 1.04, 0.4, 26);
  const holeMat = new THREE.MeshStandardMaterial({ color: 0x070b12, emissive: 0x0a1226, emissiveIntensity: 0.4, metalness: 0.3, roughness: 0.7 });
  for (let r = 0; r < ROWS; r++) for (let c = 0; c < COLS; c++) { const m = new THREE.Mesh(holeGeo, holeMat); m.rotation.x = Math.PI / 2; m.position.set(cellX(c), cellY(r), HOLE_Z); scene.add(m); }

  // shared disc geometry (per-disc material so we can pulse the winning four)
  const discGeo = new THREE.CylinderGeometry(DISC_R, DISC_R, 0.5, 30);
  function mkDisc(colorHex) {
    const mat = new THREE.MeshStandardMaterial({ color: colorHex, emissive: colorHex, emissiveIntensity: 0.85, metalness: 0.45, roughness: 0.25 });
    const mesh = new THREE.Mesh(discGeo, mat); mesh.rotation.x = Math.PI / 2;
    const halo = sprite(colorHex, DISC_R * 4.2, 0.45); halo.position.z = -0.1;
    const g = new THREE.Group(); g.add(mesh); g.add(halo); scene.add(g);
    return { g, mat, mesh, halo, anim: null, landed: true, targetY: 0 };
  }

  // hover ghost (current player's translucent disc above a column) + column highlight bar
  const ghostMat = new THREE.MeshBasicMaterial({ color: COL0, transparent: true, opacity: 0.45, blending: THREE.AdditiveBlending, depthWrite: false });
  const ghost = new THREE.Mesh(discGeo, ghostMat); ghost.rotation.x = Math.PI / 2; ghost.visible = false; scene.add(ghost);
  const colBar = new THREE.Mesh(new THREE.BoxGeometry(CELL * 0.92, H, 0.05), new THREE.MeshBasicMaterial({ color: COL0, transparent: true, opacity: 0.10, blending: THREE.AdditiveBlending, depthWrite: false }));
  colBar.position.z = DISC_Z - 0.2; colBar.visible = false; scene.add(colBar);

  // starfield depth
  const stars = (() => { const n = 200, g = new THREE.BufferGeometry(), pos = new Float32Array(n * 3); for (let i = 0; i < n; i++) { pos[i * 3] = (Math.random() - 0.5) * 120; pos[i * 3 + 1] = (Math.random() - 0.5) * 80; pos[i * 3 + 2] = (Math.random() - 0.5) * 80 - 30; } g.setAttribute('position', new THREE.BufferAttribute(pos, 3)); const pts = new THREE.Points(g, new THREE.PointsMaterial({ color: 0x335577, size: 0.18, transparent: true, opacity: 0.7 })); scene.add(pts); return pts; })();

  // particle pool
  const PMAX = 150, pool = []; for (let i = 0; i < PMAX; i++) { const s = sprite(0xffffff, 1, 0); s.visible = false; scene.add(s); pool.push({ s, life: 0, max: 1, vx: 0, vy: 0, vz: 0 }); }
  let pHead = 0;
  function burst(pos, color, n, power) {
    for (let k = 0; k < n; k++) {
      const p = pool[pHead++ % PMAX]; const a = Math.random() * Math.PI * 2, e = Math.random() * Math.PI - Math.PI / 2, sp = power * (0.4 + Math.random());
      p.vx = Math.cos(a) * Math.cos(e) * sp; p.vy = Math.sin(e) * sp + 1.5; p.vz = Math.sin(a) * Math.cos(e) * sp;
      p.life = p.max = 0.5 + Math.random() * 0.5; p.s.material.color.setHex(color); p.s.position.copy(pos); p.s.visible = true; p.s.scale.setScalar(0.6 + Math.random() * 0.8);
    }
  }

  const hud = makeHud(canvas);
  const sfx = new SFX(); hud.bindSfx(sfx);

  // ── pick plane (disc face) for screen→column ───────────────────────────────────────────────
  const pickPlane = new THREE.Plane(new THREE.Vector3(0, 0, 1), -DISC_Z);   // z = DISC_Z
  const ray = new THREE.Raycaster();
  function screenToCol(nx, ny) {
    ray.setFromCamera(new THREE.Vector2(nx * 2 - 1, -(ny * 2 - 1)), camera);
    const hit = new THREE.Vector3();
    if (!ray.ray.intersectPlane(pickPlane, hit)) return -1;
    if (hit.x < LEFT - CELL * 0.4 || hit.x > -LEFT + CELL * 0.4) return -1;
    if (hit.y < -TOP - CELL || hit.y > TOP + CELL * 4) return -1;     // allow above the board (drop zone)
    return Math.max(0, Math.min(COLS - 1, Math.floor((hit.x - LEFT) / CELL)));
  }
  // local hover (mouse): purely cosmetic, never crosses the wire
  let hoverCol = -1;
  const onMove = e => { const r = canvas.getBoundingClientRect(); if (r.width < 2) return; hoverCol = screenToCol((e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height); };
  const onLeave = () => { hoverCol = -1; };
  canvas.addEventListener('pointermove', onMove); canvas.addEventListener('pointerleave', onLeave);

  // ── disc bookkeeping ────────────────────────────────────────────────────────────────────────
  const discMap = new Map();
  let lastMoves = -1, winTube = null, winCells = null, celebrated = false;
  function clearWin() { if (winTube) { scene.remove(winTube); winTube.geometry.dispose(); winTube.material.dispose(); winTube = null; } winCells = null; celebrated = false; }
  function syncBoard(st) {
    const board = st.board, seen = new Set();
    const fresh = (st.last && st.moves === lastMoves + 1) ? idx(st.last.row, st.last.col) : -1;
    for (let i = 0; i < board.length; i++) {
      const v = board[i]; if (v == null) continue; seen.add(i);
      const r = (i / COLS) | 0, c = i % COLS;
      let d = discMap.get(i);
      if (!d) {
        d = mkDisc(colOf(st, v)); discMap.set(i, d);
        d.targetY = cellY(r); d.g.position.set(cellX(c), d.targetY, DISC_Z);
        if (i === fresh) { d.anim = { t0: performance.now(), from: TOP + CELL * 2.4 }; d.landed = false; d.g.position.y = d.anim.from; }
      }
    }
    for (const [i, d] of discMap) if (!seen.has(i)) { scene.remove(d.g); d.mat.dispose(); discMap.delete(i); }
  }
  function celebrate(st) {
    winCells = st.win; celebrated = true;
    const v = st.board[idx(st.win[0].r, st.win[0].c)], color = colOf(st, v);
    const a = st.win[0], b = st.win[3];
    const p0 = new THREE.Vector3(cellX(a.c), cellY(a.r), DISC_Z + 0.1), p1 = new THREE.Vector3(cellX(b.c), cellY(b.r), DISC_Z + 0.1);
    const dir = new THREE.Vector3().subVectors(p1, p0), len = dir.length();
    winTube = new THREE.Mesh(new THREE.CylinderGeometry(0.18, 0.18, len + DISC_R, 14), new THREE.MeshBasicMaterial({ color, transparent: true, opacity: 0.95, blending: THREE.AdditiveBlending, depthWrite: false }));
    winTube.position.copy(p0).addScaledVector(dir, 0.5);
    winTube.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), dir.clone().normalize());
    scene.add(winTube);
    const won = v === api.mySeat(); won ? sfx.win() : sfx.lose();
    hud.banner(won ? '🏆 Hai vinto!' : 'Forza 4!', '#' + color.toString(16).padStart(6, '0'));
    for (let i = 0; i < 5; i++) setTimeout(() => burst(new THREE.Vector3((Math.random() - 0.5) * W, (Math.random() - 0.4) * H, DISC_Z + 1), color, 26, 11), i * 130);
    shake = Math.min(1.4, shake + 0.8);
  }

  let lastT = performance.now(), shake = 0, idle = 0, ready = false;

  function fit(cssW, cssH) {
    const w = Math.max(2, cssW | 0), h = Math.max(2, cssH | 0);
    renderer.setPixelRatio(Math.min(2, window.devicePixelRatio || 1));
    renderer.setSize(w, h, false);
    camera.aspect = w / h; camera.updateProjectionMatrix();
  }
  fit(canvas.clientWidth || 960, canvas.clientHeight || 540);

  function frame(st) {
    const now = performance.now(), dt = Math.min(0.05, (now - lastT) / 1000); lastT = now; idle += dt;
    ready = true;

    if (st) {
      hud.update(st, api);
      if (st.moves !== lastMoves) {
        if (st.moves < lastMoves) { for (const [, d] of discMap) { scene.remove(d.g); d.mat.dispose(); } discMap.clear(); clearWin(); }   // rematch / reset
        syncBoard(st);
        if (st.last && st.moves === lastMoves + 1) { /* fresh drop: SFX fires on landing in the anim loop */ }
        lastMoves = st.moves;
      }
      if (!st.win && celebrated) clearWin();
      if (st.win && !celebrated) celebrate(st);

      // animate the falling disc + the drop light that tracks it
      let lit = false;
      for (const [, d] of discMap) {
        if (!d.anim) continue;
        const p = Math.min(1, (now - d.anim.t0) / 1000 / 0.5);
        d.g.position.y = d.targetY + (d.anim.from - d.targetY) * (1 - easeOutBounce(p));
        if (!d.landed && p >= 0.36) { d.landed = true; burst(new THREE.Vector3(d.g.position.x, d.targetY, DISC_Z + 0.4), d.mat.color.getHex(), 14, 7); sfx.drop(); shake = Math.min(1, shake + 0.22); }
        dropLight.position.set(d.g.position.x, d.g.position.y, 3); dropLight.intensity = 16; lit = true;
        if (p >= 1) { d.g.position.y = d.targetY; d.anim = null; }
      }
      if (!lit) dropLight.intensity *= 0.85;

      // winning four pulse
      if (winCells) { const k = 1.4 + Math.sin(now * 0.012) * 0.5; for (const cell of winCells) { const d = discMap.get(idx(cell.r, cell.c)); if (d) { d.mat.emissiveIntensity = k; d.g.scale.setScalar(1 + Math.sin(now * 0.012) * 0.08); } } }

      // hover / gamepad cursor ghost — only on my turn, before the game ends
      const myTurn = st.seats[st.turn] === api.mySeat();
      const cur = api.cursor ? api.cursor() : null; const col = (cur != null && cur >= 0) ? cur : hoverCol;
      const showGhost = myTurn && !st.win && col >= 0 && col < COLS && st.board[idx(0, col)] == null;
      ghost.visible = colBar.visible = showGhost;
      if (showGhost) {
        const hex = colOf(st, api.mySeat());
        ghostMat.color.setHex(hex); colBar.material.color.setHex(hex);
        ghost.position.set(cellX(col), TOP + CELL * 0.6 + Math.sin(now * 0.005) * 0.15, DISC_Z);
        colBar.position.x = cellX(col);
      }
    } else {
      dropLight.intensity *= 0.85; ghost.visible = colBar.visible = false;
    }

    // particles
    for (const p of pool) { if (p.life <= 0) { if (p.s.visible) p.s.visible = false; continue; } p.life -= dt; const k = Math.max(0, p.life / p.max); p.vy -= 9 * dt; p.s.position.x += p.vx * dt; p.s.position.y += p.vy * dt; p.s.position.z += p.vz * dt; p.s.material.opacity = k; p.s.scale.setScalar(0.3 + k * 0.9); }

    stars.rotation.z += dt * 0.008;
    shake *= Math.pow(0.0009, dt);
    const sway = Math.sin(idle * 0.22) * 0.8;
    camera.position.set(camBase.x + sway + (Math.random() - 0.5) * shake * 2.2, camBase.y + (Math.random() - 0.5) * shake * 1.6, camBase.z);
    camera.lookAt(camLook.x, camLook.y + (Math.random() - 0.5) * shake, camLook.z);
    renderer.render(scene, camera);
  }

  function dispose() {
    try { canvas.removeEventListener('pointermove', onMove); canvas.removeEventListener('pointerleave', onLeave); } catch {}
    try { hud.el.remove(); } catch {}
    try { sfx.stopMusic(); } catch {}
    scene.traverse(o => { if (o.geometry) o.geometry.dispose(); if (o.material) { const m = o.material; (Array.isArray(m) ? m : [m]).forEach(x => { if (x.map) x.map.dispose(); x.dispose(); }); } });
    glowTex.dispose(); renderer.dispose();
  }

  return { frame, resize: fit, dispose, screenToCol, isReady: () => ready, sfx, mode: 'webgl' };
}

function easeOutBounce(x) { const n1 = 7.5625, d1 = 2.75; if (x < 1 / d1) return n1 * x * x; if (x < 2 / d1) { x -= 1.5 / d1; return n1 * x * x + 0.75; } if (x < 2.5 / d1) { x -= 2.25 / d1; return n1 * x * x + 0.9375; } x -= 2.625 / d1; return n1 * x * x + 0.984375; }

// radial white glow → CanvasTexture (additive sprites tint it).
function makeGlow(THREE) {
  const c = document.createElement('canvas'); c.width = c.height = 128; const g = c.getContext('2d');
  const grd = g.createRadialGradient(64, 64, 0, 64, 64, 64);
  grd.addColorStop(0, 'rgba(255,255,255,1)'); grd.addColorStop(0.25, 'rgba(255,255,255,0.85)'); grd.addColorStop(0.6, 'rgba(255,255,255,0.18)'); grd.addColorStop(1, 'rgba(255,255,255,0)');
  g.fillStyle = grd; g.fillRect(0, 0, 128, 128);
  const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace; return t;
}

// ── HUD overlay (DOM): turn indicator, names, LLM/brain badge, audio toggles ─────────────────────
function makeHud(canvas) {
  const host = canvas.parentNode; const el = document.createElement('div');
  el.style.cssText = 'position:absolute;inset:0;pointer-events:none;font-family:system-ui,Segoe UI,sans-serif;overflow:hidden;z-index:5';
  el.innerHTML = `
    <div data-turn style="position:absolute;top:14px;left:0;right:0;text-align:center;font-weight:800;font-size:clamp(18px,4.5vw,34px);letter-spacing:.03em;text-shadow:0 0 20px rgba(0,0,0,.7);display:flex;align-items:center;justify-content:center;gap:.4em">
      <span data-dot style="width:.7em;height:.7em;border-radius:50%;display:inline-block;box-shadow:0 0 12px currentColor;background:currentColor"></span><span data-tt></span>
    </div>
    <div data-names style="position:absolute;top:calc(14px + clamp(24px,6vw,44px));left:0;right:0;text-align:center;font-size:13px;color:#8b97a6;letter-spacing:.04em"></div>
    <div data-banner style="position:absolute;bottom:20%;left:0;right:0;text-align:center;font-weight:800;font-size:clamp(20px,5vw,40px);opacity:0;transition:opacity .3s,transform .3s"></div>
    <div data-brain style="position:absolute;left:12px;bottom:10px;font-size:12px;color:#9aa7b6;display:flex;gap:6px;align-items:center"></div>
    <div style="position:absolute;right:10px;top:10px;display:flex;gap:6px;pointer-events:auto">
      <button data-mute title="Audio" style="background:rgba(20,26,34,.7);border:1px solid #2a3340;color:#e6edf3;border-radius:8px;padding:4px 8px;cursor:pointer">🔊</button>
      <button data-music title="Musica" style="background:rgba(20,26,34,.7);border:1px solid #2a3340;color:#8b97a6;border-radius:8px;padding:4px 8px;cursor:pointer">🎵</button>
    </div>`;
  if (getComputedStyle(host).position === 'static') host.style.position = 'relative';
  host.appendChild(el);
  const $ = s => el.querySelector(s);
  let bannerTimer = 0, sfxRef = null, names = '';
  const muteBtn = $('[data-mute]'), musicBtn = $('[data-music]');
  muteBtn.onclick = () => { if (!sfxRef) return; const m = muteBtn.textContent === '🔊'; sfxRef.setMuted(m); muteBtn.textContent = m ? '🔇' : '🔊'; };
  musicBtn.onclick = () => { if (!sfxRef) return; const on = sfxRef.toggleMusic(); musicBtn.style.color = on ? '#22d3ee' : '#8b97a6'; };
  return {
    el, bindSfx(s) { sfxRef = s; },
    update(st, api) {
      const seatCol = seat => seat === st.seats[0] ? '#ff5a6e' : '#ffd23f';
      const turnSeat = st.seats[st.turn], dot = $('[data-dot]'), tt = $('[data-tt]');
      const myTurn = turnSeat === api.mySeat();
      dot.style.color = seatCol(turnSeat);
      tt.textContent = st.win ? 'Partita finita' : (myTurn ? 'Tocca a te' : `Turno: ${st.names[st.turn]}`);
      tt.style.color = st.win ? '#cbd5e1' : seatCol(turnSeat);
      const nm = `${st.names[0]}  vs  ${st.names[1]}`; if (nm !== names) { names = nm; $('[data-names]').innerHTML = `<span style="color:#ff5a6e">${escapeHtml(st.names[0])}</span>  vs  <span style="color:#ffd23f">${escapeHtml(st.names[1])}</span>`; }
      const brain = $('[data-brain]'), b = st.brain;
      if (b && b.status === 'live') brain.innerHTML = `<span style="color:#34d399">🟢 ${escapeHtml(b.label || 'LLM')}</span>${b.ms ? ` <span style="color:#5b6b7d">${b.ms}ms</span>` : ''}`;
      else if (b && b.status === 'down') brain.innerHTML = `<span style="color:#fb7185">⛔ ${escapeHtml(b.label || 'LLM')} non raggiungibile — in attesa</span>`;
      else if (b && b.status === 'connecting') brain.innerHTML = `<span style="color:#fbbf24">… connessione ${escapeHtml(b.label || 'LLM')}</span>`;
      else brain.textContent = '';
    },
    banner(text, color) { const b = $('[data-banner]'); b.textContent = text; b.style.color = color; b.style.textShadow = `0 0 24px ${color}`; b.style.opacity = 1; b.style.transform = 'translateY(0)'; clearTimeout(bannerTimer); bannerTimer = setTimeout(() => { b.style.opacity = 0; b.style.transform = 'translateY(10px)'; }, 2200); },
  };
}
function escapeHtml(s) { return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }

// ── 2D fallback (no WebGL): still neat — glow, drop animation, winning-line highlight ────────────
function createFallback2D(canvas, geom) {
  const { COLS, ROWS } = geom; const ctx = canvas.getContext('2d');
  const sfx = new SFX(); let lastMoves = -1, hoverCol = -1, anim = null, lastT = performance.now();
  const idx = (r, c) => r * COLS + c;
  function G() { const W = canvas.width, H = canvas.height, cell = Math.min(W / (COLS + 0.5), (H - 40) / (ROWS + 1.0)); const w = cell * COLS, h = cell * ROWS; return { W, H, cell, w, h, ox: (W - w) / 2, oy: (H - h) / 2 + 18 }; }
  function screenToCol(nx, ny) { const g = G(), x = nx * canvas.width; if (x < g.ox || x > g.ox + g.w) return -1; return Math.max(0, Math.min(COLS - 1, Math.floor((x - g.ox) / g.cell))); }
  const onMove = e => { const r = canvas.getBoundingClientRect(); if (r.width < 2) return; hoverCol = screenToCol((e.clientX - r.left) / r.width, 0); };
  const onLeave = () => { hoverCol = -1; };
  canvas.addEventListener('pointermove', onMove); canvas.addEventListener('pointerleave', onLeave);
  function disc(g, r, c, color, rad) { const cx = g.ox + c * g.cell + g.cell / 2, cy = g.oy + r * g.cell + g.cell / 2; ctx.shadowColor = color; ctx.shadowBlur = 22; ctx.fillStyle = color; ctx.beginPath(); ctx.arc(cx, cy, rad ?? g.cell * 0.38, 0, 7); ctx.fill(); ctx.shadowBlur = 0; }
  function frame(st) {
    const now = performance.now(), dt = Math.min(0.05, (now - lastT) / 1000); lastT = now;
    const g = G();
    ctx.fillStyle = '#05070d'; ctx.fillRect(0, 0, g.W, g.H);
    if (!st) return;
    if (st.moves !== lastMoves) { if (st.moves === lastMoves + 1 && st.last) { anim = { col: st.last.col, row: st.last.row, seat: st.last.seat, t0: now }; sfx.drop(); } else if (st.moves < lastMoves) anim = null; lastMoves = st.moves; }
    ctx.fillStyle = '#1d4ed8'; roundRect(ctx, g.ox - 8, g.oy - 8, g.w + 16, g.h + 16, 16); ctx.fill();
    const colHex = v => v === st.seats[0] ? '#ff5a6e' : '#ffd23f';
    // hover ghost (above the hovered column, in the current player's colour)
    if (!st.win && hoverCol >= 0 && st.board[idx(0, hoverCol)] == null) { ctx.globalAlpha = .5; disc(g, -0.7, hoverCol, colHex(st.seats[st.turn]), g.cell * 0.34); ctx.globalAlpha = 1; }
    for (let r = 0; r < ROWS; r++) for (let c = 0; c < COLS; c++) {
      const v = st.board[idx(r, c)];
      if (v == null) { const cx = g.ox + c * g.cell + g.cell / 2, cy = g.oy + r * g.cell + g.cell / 2; ctx.fillStyle = '#0a0f1a'; ctx.beginPath(); ctx.arc(cx, cy, g.cell * 0.38, 0, 7); ctx.fill(); continue; }
      if (anim && anim.col === c && anim.row === r) continue;   // drawn animating below
      disc(g, r, c, colHex(v));
    }
    if (anim) { const p = Math.min(1, (now - anim.t0) / 500); const yr = -1 + (anim.row + 1) * easeOutBounce(p); disc(g, yr, anim.col, colHex(anim.seat)); if (p >= 1) anim = null; }
    if (st.win) { ctx.strokeStyle = '#fff'; ctx.lineWidth = 4; ctx.shadowColor = '#fff'; ctx.shadowBlur = 16; const a = st.win[0], b = st.win[3]; ctx.beginPath(); ctx.moveTo(g.ox + a.c * g.cell + g.cell / 2, g.oy + a.r * g.cell + g.cell / 2); ctx.lineTo(g.ox + b.c * g.cell + g.cell / 2, g.oy + b.r * g.cell + g.cell / 2); ctx.stroke(); ctx.shadowBlur = 0; }
    ctx.fillStyle = '#cbd5e1'; ctx.font = 'bold 18px system-ui'; ctx.textAlign = 'center';
    ctx.fillText(st.win ? 'Forza 4!' : `Turno: ${st.names[st.turn]}`, g.W / 2, Math.max(20, g.oy - 16));
  }
  return { frame, resize() {}, dispose() { try { canvas.removeEventListener('pointermove', onMove); canvas.removeEventListener('pointerleave', onLeave); } catch {} }, screenToCol, isReady: () => true, sfx, mode: '2d' };
}
function roundRect(ctx, x, y, w, h, r) { ctx.beginPath(); ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r); ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath(); }

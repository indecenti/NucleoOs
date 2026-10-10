// stelle/galaxy.js — the galaxy map of Costellazioni (web), drawn in 3D and turned with the mouse / a finger.
//   · the galaxy: a spiral of ~40k stars (warm core, blue arms, pink star-forming knots) the sectors hang on;
//   · the current sector in detail: its ten systems from the SHARED generator (positions, factions, beacons) as
//     stars coloured by their star class (stelle/world.js), faction territories as soft light on the galactic plane,
//     beacons as crystal spires (gold when lit, cold cyan when dark), golden threads ONLY between two lit beacons
//     (dim, broken threads otherwise), the routes in jump range, the jump-range ring, charted / uncharted systems
//     (the web save's visited mask) and floating labels;
//   · the neighbouring sectors as small clusters further along the arm (older ones carry the threads you lit).
// Input: drag = orbit, wheel / pinch = zoom, click = pick a system (sent as the hub's 'Tgt:i' key, like the 2D map).
// Web-only layout (heights, the spiral) comes from a dedicated hash domain (VDOM.GALAXY): the shared numbers are untouched.
import { genSector } from '../constellations-gen.js';
import { systemBlueprint, vhash, VDOM, rng } from './world.js';

const FAC_COL = [[0.5, 0.66, 0.94], [0.25, 0.75, 0.65], [0.88, 0.47, 0.24], [0.66, 0.51, 0.9]];
const FAC_CSS = ['#7fa8f0', '#3fbfa6', '#e0773c', '#a882e6'];
const SECTOR_STEP = 0.62, ARM_R0 = 520, ARM_DR = 70;   // sectors along a logarithmic-ish arm

// where sector s sits in the galaxy (map units; a sector spans 100 of them)
export function sectorCenter(s, out = [0, 0, 0]) {
  const a = 0.6 + s * SECTOR_STEP, r = ARM_R0 + s * ARM_DR;
  out[0] = Math.cos(a) * r; out[1] = 0; out[2] = Math.sin(a) * r;
  return out;
}
// a system's place in its sector cluster (shared x,y -> plane; web-only height)
export function systemPos(seed, sector, i, sys, out = [0, 0, 0]) {
  const c = sectorCenter(sector), h = rng(vhash(seed, sector, i, VDOM.GALAXY, 0));
  out[0] = c[0] + (sys.x - 50); out[1] = (h() - 0.5) * 18; out[2] = c[2] + (sys.y - 50);
  return out;
}

const STAR_V = /* glsl */`attribute float aSize; attribute vec3 aCol; uniform float uScale, uTime; varying vec3 vCol;
void main() { vec4 mv = modelViewMatrix * vec4(position, 1.0); gl_Position = projectionMatrix * mv; float z = -mv.z;
  gl_PointSize = clamp(aSize * uScale / z, 1.0, 5.0); vCol = aCol * smoothstep(60.0, 420.0, z); }`;
const STAR_F = /* glsl */`varying vec3 vCol; void main() { vec2 c = gl_PointCoord - 0.5; float d = dot(c, c) * 4.0; float a = exp(-d * 5.0) + exp(-d * 40.0) * 1.5; gl_FragColor = vec4(vCol * a, 1.0); }`;
const SPRITE_V = /* glsl */`varying vec2 vUv; void main() { vUv = uv * 2.0 - 1.0; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;
const LINE_V = /* glsl */`attribute float aT; attribute vec4 aC; varying float vT; varying vec4 vC; void main() { vT = aT; vC = aC; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;
// aC.a: 0..1 lit thread (running light), 2 = dark broken thread, 3 = route, 4 = selected route
const LINE_F = /* glsl */`uniform float uTime; varying float vT; varying vec4 vC;
void main() {
  float a = 1.0;
  if (vC.a > 3.5) a = 0.55 + 0.45 * step(0.5, fract(vT * 14.0 - uTime * 1.6));
  else if (vC.a > 2.5) a = 0.35;
  else if (vC.a > 1.5) { a = step(0.42, fract(vT * 9.0)) * 0.5; }
  else a = 0.55 + pow(0.5 + 0.5 * sin(vT * 18.0 - uTime * 2.4), 10.0) * 2.2;
  gl_FragColor = vec4(vC.rgb * a, 1.0);
}`;

export function createGalaxy(THREE, canvas, tr) {
  const scene = new THREE.Scene();
  const cam = new THREE.PerspectiveCamera(50, 1.6, 1, 20000);
  const U = { uTime: { value: 0 }, uScale: { value: 1 } };
  const host = canvas.parentNode;
  const labelsEl = document.createElement('div');
  labelsEl.style.cssText = 'position:absolute;inset:0;pointer-events:none;z-index:28;display:none;font-family:"Segoe UI",system-ui,sans-serif;overflow:hidden';
  host.appendChild(labelsEl);
  const disposables = [];
  const keep = (x) => { disposables.push(x); return x; };

  // ---- the galaxy disc (static) --------------------------------------------------------------------------------
  {
    const N = 42000, pos = new Float32Array(N * 3), col = new Float32Array(N * 3), size = new Float32Array(N), r = rng(0xC057E11A);
    for (let i = 0; i < N; i++) {
      const arm = r.int(2), core = r() < 0.18;
      let R, a, y;
      if (core) { R = Math.pow(r(), 1.6) * 420; a = r() * Math.PI * 2; y = (r() - 0.5) * 90 * (1 - R / 500); }
      else { R = 120 + Math.pow(r(), 0.75) * 2600; a = Math.log(R / 60) / 0.32 + arm * Math.PI + (r() - 0.5) * (0.55 + 260 / R); y = (r() - 0.5) * 50 * Math.exp(-R / 2400); }
      pos[i * 3] = Math.cos(a) * R; pos[i * 3 + 1] = y; pos[i * 3 + 2] = Math.sin(a) * R;
      const t = Math.min(1, R / 2200), knot = !core && r() < 0.035;
      const c = knot ? [1.0, 0.45, 0.75] : core ? [1.0, 0.82, 0.55] : [0.55 + 0.45 * (1 - t), 0.62 + 0.25 * (1 - t), 0.85 + 0.15 * t];
      const b = (core ? 0.8 : 0.45) * (0.4 + r() * 0.9) * (knot ? 2 : 1);
      col[i * 3] = c[0] * b; col[i * 3 + 1] = c[1] * b; col[i * 3 + 2] = c[2] * b; size[i] = (core ? 5 : 3.4) * (0.6 + r() * r() * 2.4) * (knot ? 2 : 1);
    }
    const g = keep(new THREE.BufferGeometry()); g.setAttribute('position', new THREE.BufferAttribute(pos, 3)); g.setAttribute('aCol', new THREE.BufferAttribute(col, 3)); g.setAttribute('aSize', new THREE.BufferAttribute(size, 1));
    const m = keep(new THREE.ShaderMaterial({ vertexShader: STAR_V, fragmentShader: STAR_F, uniforms: U, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending }));
    const pts = new THREE.Points(g, m); pts.frustumCulled = false; scene.add(pts);
    // the core glow
    const cm = keep(new THREE.ShaderMaterial({ vertexShader: SPRITE_V, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending,
      fragmentShader: `varying vec2 vUv; void main() { float d = dot(vUv, vUv); gl_FragColor = vec4(vec3(1.0, 0.78, 0.5) * exp(-d * 4.0) * 0.55, 1.0); }` }));
    const core = new THREE.Mesh(keep(new THREE.PlaneGeometry(1300, 1300)), cm); core.rotation.x = -Math.PI / 2; scene.add(core);
  }

  // ---- the sector layer (rebuilt per sector / visited set / lit set) ---------------------------------------------------------------
  const layer = new THREE.Group(); scene.add(layer);
  const sprite = keep(new THREE.PlaneGeometry(1, 1));
  const terrMat = keep(new THREE.ShaderMaterial({ vertexShader: `attribute vec3 iCol; varying vec2 vUv; varying vec3 vCol; void main() { vUv = uv * 2.0 - 1.0; vCol = iCol; gl_Position = projectionMatrix * modelViewMatrix * instanceMatrix * vec4(position, 1.0); }`,
    fragmentShader: `varying vec2 vUv; varying vec3 vCol; void main() { float d = dot(vUv, vUv); if (d > 1.0) discard; float a = exp(-d * 2.6) * 0.16 + smoothstep(1.0, 0.9, d) * smoothstep(0.82, 0.92, d) * 0.06; gl_FragColor = vec4(vCol * a, 1.0); }`,
    transparent: true, depthWrite: false, blending: THREE.AdditiveBlending }));
  const sunMat = keep(new THREE.ShaderMaterial({ vertexShader: `attribute vec4 iCol; varying vec2 vUv; varying vec4 vCol; void main() { vUv = uv * 2.0 - 1.0; vCol = iCol; vec4 mv = modelViewMatrix * vec4(instanceMatrix[3].xyz, 1.0); float s = length(instanceMatrix[0].xyz); mv.xy += position.xy * s; gl_Position = projectionMatrix * mv; }`,
    fragmentShader: `varying vec2 vUv; varying vec4 vCol; void main() { float d = dot(vUv, vUv); float a = exp(-d * 12.0) * 3.0 + exp(-d * 3.0) * 0.35; float sp = (exp(-abs(vUv.y) * 40.0) + exp(-abs(vUv.x) * 40.0)) * exp(-d * 2.0) * 0.5; gl_FragColor = vec4(vCol.rgb * (a + sp) * vCol.a, 1.0); }`,
    transparent: true, depthWrite: false, blending: THREE.AdditiveBlending }));
  const crystalMat = keep(new THREE.MeshBasicMaterial({ vertexColors: true, transparent: true, opacity: 0.95, blending: THREE.AdditiveBlending, depthWrite: false }));
  const crystalGeo = keep(new THREE.OctahedronGeometry(1, 0)); crystalGeo.scale(0.9, 3.2, 0.9);
  const ringGeo = keep(new THREE.RingGeometry(0.985, 1, 160)), ringMat = keep(new THREE.MeshBasicMaterial({ color: new THREE.Color(0.25, 0.75, 0.95), transparent: true, opacity: 0.4, side: THREE.DoubleSide, blending: THREE.AdditiveBlending, depthWrite: false }));
  const discMat = keep(new THREE.MeshBasicMaterial({ color: new THREE.Color(0.05, 0.16, 0.22), transparent: true, opacity: 0.12, side: THREE.DoubleSide, blending: THREE.AdditiveBlending, depthWrite: false }));
  const markGeo = keep(new THREE.RingGeometry(2.2, 2.6, 48)), curMat = keep(new THREE.MeshBasicMaterial({ color: new THREE.Color(0.5, 1.6, 0.8), transparent: true, side: THREE.DoubleSide, blending: THREE.AdditiveBlending, depthWrite: false }));
  const tgtMat = keep(new THREE.MeshBasicMaterial({ color: new THREE.Color(1.6, 1.3, 0.6), transparent: true, side: THREE.DoubleSide, blending: THREE.AdditiveBlending, depthWrite: false }));
  const lineMat = keep(new THREE.ShaderMaterial({ vertexShader: LINE_V, fragmentShader: LINE_F, uniforms: U, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending }));
  const curMark = new THREE.Mesh(markGeo, curMat), tgtMark = new THREE.Mesh(markGeo, tgtMat);
  const rangeRing = new THREE.Mesh(ringGeo, ringMat), rangeDisc = new THREE.Mesh(keep(new THREE.CircleGeometry(1, 96)), discMat);
  for (const m of [curMark, tgtMark, rangeRing, rangeDisc]) { m.rotation.x = -Math.PI / 2; scene.add(m); }
  let built = null, P = [], labels = [], lines = null, lineN = 0, lineMax = 0;
  const LP = [], LC = [], LT = [];
  const SG = { valid: false, seed: -1, sector: -1, lit: 0, vis: 0, sys: -1, range: 0, wn: -1 };

  function addLine(a, b, c, mode, seg = 1) {
    for (let k = 0; k < seg; k++) {
      const t0 = k / seg, t1 = (k + 1) / seg;
      LP.push(a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0, a[2] + (b[2] - a[2]) * t0, a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1, a[2] + (b[2] - a[2]) * t1);
      LT.push(t0, t1); LC.push(c[0], c[1], c[2], mode, c[0], c[1], c[2], mode);
    }
  }
  // (re)build the sector layer: stars, territories, spires, threads, neighbours, labels
  function build(m) {
    const r = m.run, S = m.sector, W = m.web;
    const lit = r.beacon_lit >>> 0, vis = W ? (W.visited[r.sector] >>> 0) || 0 : 0, wn = W ? W.n | 0 : -1;
    // rebuild only when something the map shows changed (plain comparisons: nothing allocated per frame)
    if (SG.valid && SG.seed === r.seed && SG.sector === r.sector && SG.lit === lit && SG.vis === vis && SG.sys === r.sys && SG.range === r.jump_range && SG.wn === wn) return;
    Object.assign(SG, { valid: true, seed: r.seed, sector: r.sector, lit, vis, sys: r.sys, range: r.jump_range, wn });
    for (const c of layer.children.slice()) { c.removeFromParent(); if (c.userData.own) { c.geometry.dispose(); } }
    labelsEl.textContent = ''; labels = []; P = [];
    LP.length = 0; LC.length = 0; LT.length = 0;
    const seed = r.seed >>> 0, sector = r.sector >>> 0;
    // systems of this sector
    const bps = S.map((s, i) => systemBlueprint(seed, sector, i, s, ((lit >>> i) & 1) === 1));
    for (let i = 0; i < S.length; i++) P.push(systemPos(seed, sector, i, S[i]));
    const terr = new THREE.InstancedMesh(sprite, terrMat, S.length); terr.userData.own = false;
    const sun = new THREE.InstancedMesh(sprite, sunMat, S.length + 60);
    const tc = new Float32Array(S.length * 3), sc = new Float32Array((S.length + 60) * 4), mm = new THREE.Matrix4(), qq = new THREE.Quaternion(), ss = new THREE.Vector3(), pp = new THREE.Vector3();
    const flat = new THREE.Quaternion().setFromEuler(new THREE.Euler(-Math.PI / 2, 0, 0));
    for (let i = 0; i < S.length; i++) {
      const charted = ((vis >>> i) & 1) === 1 || i === r.sys, f = S[i].faction, c = FAC_COL[f];
      mm.compose(pp.set(P[i][0], 0, P[i][2]), flat, ss.setScalar(charted ? 48 : 36)); terr.setMatrixAt(i, mm);
      const k = charted ? 1 : 0.45; tc.set([c[0] * k, c[1] * k, c[2] * k], i * 3);
      const st = bps[i].star, size = Math.min(7, 2.2 + st.size * 0.9);
      mm.compose(pp.set(P[i][0], P[i][1], P[i][2]), qq, ss.setScalar(size)); sun.setMatrixAt(i, mm);
      const sk = charted ? 1.4 : 0.6; sc.set([st.color[0] * sk, st.color[1] * sk, st.color[2] * sk, 1], i * 4);
      // spires and the vertical drop-line to the plane
      addLine([P[i][0], 0, P[i][2]], P[i], charted ? [0.18, 0.3, 0.42] : [0.1, 0.14, 0.2], 3);
      if (S[i].beacon) {
        const on = ((lit >>> i) & 1) === 1;
        const cg = crystalGeo.clone(); cg.translate(0, 0, 0);
        const cols = new Float32Array(cg.attributes.position.count * 3);
        for (let v = 0; v < cols.length; v += 3) { cols[v] = on ? 1.6 : 0.25; cols[v + 1] = on ? 1.15 : 0.7; cols[v + 2] = on ? 0.45 : 1.1; }
        cg.setAttribute('color', new THREE.BufferAttribute(cols, 3));
        const cm = new THREE.Mesh(cg, crystalMat); cm.position.set(P[i][0], P[i][1] + 7, P[i][2]); cm.userData.own = true; cm.userData.spin = on ? 0.8 : 0.15; layer.add(cm);
        if (on) addLine([P[i][0], P[i][1] + 10, P[i][2]], [P[i][0], P[i][1] + 46, P[i][2]], [1.2, 0.85, 0.4], 0.5, 6);
      }
    }
    // golden threads: each beacon to its two nearest beacons; lit only when BOTH ends are lit
    const B = []; for (let i = 0; i < S.length; i++) if (S[i].beacon) B.push(i);
    const done = new Set();
    for (const i of B) {
      const near = B.filter((j) => j !== i).sort((a, b) => (dist(P[a], P[i]) - dist(P[b], P[i]))).slice(0, 2);
      for (const j of near) {
        const k = i < j ? i + ':' + j : j + ':' + i; if (done.has(k)) continue; done.add(k);
        const both = ((lit >>> i) & 1) && ((lit >>> j) & 1);
        const a = [P[i][0], P[i][1] + 7, P[i][2]], b = [P[j][0], P[j][1] + 7, P[j][2]];
        addLine(a, b, both ? [1.4, 1.0, 0.45] : [0.22, 0.42, 0.6], both ? 0.5 : 2, 24);
      }
    }
    // routes in jump range from here
    for (let j = 0; j < S.length; j++) {
      if (j === r.sys) continue;
      const d = Math.hypot(S[j].x - S[r.sys].x, S[j].y - S[r.sys].y);
      if (d <= r.jump_range) addLine(P[r.sys], P[j], [0.12, 0.4, 0.55], 3, 1);
    }
    // neighbouring sectors: small clusters along the arm; the ones behind carry the threads you lit there
    let nn = S.length;
    for (let ds = -2; ds <= 3; ds++) {
      const s2 = sector + ds; if (ds === 0 || s2 < 0) continue;
      const S2 = genSector(seed, s2), relit = W ? (W.relit[s2] >>> 0) || 0 : 0, c = sectorCenter(s2);
      const pts = S2.map((x, i) => systemPos(seed, s2, i, x));
      for (let i = 0; i < S2.length && nn < S.length + 60; i++) {
        mm.compose(pp.set(pts[i][0], pts[i][1], pts[i][2]), qq, ss.setScalar(ds < 0 ? 2.6 : 2.0)); sun.setMatrixAt(nn, mm);
        const b = ds < 0 ? 0.9 : 0.4; sc.set([0.7 * b, 0.8 * b, 1.0 * b, 1], nn * 4); nn++;
      }
      if (ds < 0) {
        const lb = []; for (let i = 0; i < S2.length; i++) if (S2[i].beacon && ((relit >>> i) & 1)) lb.push(i);
        for (let a = 0; a + 1 < lb.length; a++) addLine(pts[lb[a]], pts[lb[a + 1]], [1.2, 0.85, 0.4], 0.5, 12);
      }
      label(c[0], 26, c[2], `<span style="color:#8fa3bf;font-size:12px;letter-spacing:.12em">${tr('cz_ui_sector', { n: s2 })}</span>`, -1);
    }
    // the way on: the sector's thread runs to the next cluster, lit once every beacon here burns
    { const allLit = B.length && B.every((i) => (lit >>> i) & 1), cn = sectorCenter(sector + 1), last = B.length ? P[B[B.length - 1]] : P[0];
      addLine(last, [cn[0], 0, cn[2]], allLit ? [1.4, 1.0, 0.45] : [0.18, 0.32, 0.46], allLit ? 0.5 : 2, 40); }
    sun.count = nn;
    terr.geometry = sprite; terr.geometry.setAttribute('iCol', new THREE.InstancedBufferAttribute(tc, 3));
    sun.geometry = sprite.clone(); sun.geometry.setAttribute('iCol', new THREE.InstancedBufferAttribute(sc, 4)); sun.userData.own = true;
    terr.instanceMatrix.needsUpdate = true; sun.instanceMatrix.needsUpdate = true; terr.frustumCulled = false; sun.frustumCulled = false;
    layer.add(terr); layer.add(sun);
    // lines
    const lg = new THREE.BufferGeometry(); lg.setAttribute('position', new THREE.Float32BufferAttribute(LP, 3)); lg.setAttribute('aC', new THREE.Float32BufferAttribute(LC, 4)); lg.setAttribute('aT', new THREE.Float32BufferAttribute(LT, 1));
    lines = new THREE.LineSegments(lg, lineMat); lines.userData.own = true; lines.frustumCulled = false; layer.add(lines);
    // labels
    for (let i = 0; i < S.length; i++) {
      const charted = ((vis >>> i) & 1) === 1 || i === r.sys;
      label(P[i][0], P[i][1], P[i][2], `<b style="color:${charted ? '#fff' : '#8794a6'};font-size:13px">${charted ? esc(S[i].it) : '? ? ?'}</b><i style="display:inline-block;width:7px;height:7px;border-radius:50%;margin-left:6px;background:${FAC_CSS[S[i].faction]};opacity:${charted ? 1 : 0.5}"></i>`, i);
    }
    // jump range around here
    rangeRing.position.set(P[r.sys][0], 0.2, P[r.sys][2]); rangeRing.scale.setScalar(r.jump_range);
    rangeDisc.position.copy(rangeRing.position); rangeDisc.scale.setScalar(r.jump_range);
    built = { sector, seed, sys: r.sys, S };
  }
  const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  function label(x, y, z, html, i) {
    const d = document.createElement('div');
    d.style.cssText = 'position:absolute;left:0;top:0;white-space:nowrap;text-shadow:0 1px 4px #000,0 0 10px #000;will-change:transform;transition:opacity .2s';
    d.innerHTML = html; labelsEl.appendChild(d); labels.push({ d, x, y, z, i, x2: NaN, y2: NaN, o: -1, w: -1 });
  }

  // ---- camera: orbit around a focus that eases between here and the target ----------------------------------------------------
  const view = { yaw: 0.6, pitch: 0.82, dist: 230, fx: 0, fy: 0, fz: 0, idle: 0, drag: false };
  const tgt = new THREE.Vector3();
  let active = false, W = 1, H = 1, target = -1, routeT = -2, routeL = null;
  const ptrs = new Map(); let pinch0 = 0, down = null;
  const onDown = (e) => { if (!active) return; ptrs.set(e.pointerId, { x: e.clientX, y: e.clientY }); down = { x: e.clientX, y: e.clientY, t: performance.now() }; view.drag = true; view.idle = 0; if (ptrs.size === 2) { const [a, b] = [...ptrs.values()]; pinch0 = Math.hypot(a.x - b.x, a.y - b.y); } };
  const onMove = (e) => {
    if (!active || !ptrs.has(e.pointerId)) return;
    const p = ptrs.get(e.pointerId), dx = e.clientX - p.x, dy = e.clientY - p.y; p.x = e.clientX; p.y = e.clientY;
    if (ptrs.size === 2) { const [a, b] = [...ptrs.values()], d = Math.hypot(a.x - b.x, a.y - b.y); if (pinch0) view.dist = clamp(view.dist * pinch0 / d, 70, 2600); pinch0 = d; return; }
    view.yaw -= dx * 0.006; view.pitch = clamp(view.pitch + dy * 0.005, 0.12, 1.45); view.idle = 0;
  };
  const onUp = (e) => {
    if (!active) return;
    ptrs.delete(e.pointerId); if (ptrs.size < 2) pinch0 = 0; view.drag = ptrs.size > 0;
    if (down && Math.hypot(e.clientX - down.x, e.clientY - down.y) < 6 && performance.now() - down.t < 450) pick(e.clientX, e.clientY);
    down = null;
  };
  const onWheel = (e) => { if (!active) return; view.dist = clamp(view.dist * Math.exp(Math.sign(e.deltaY) * 0.12), 70, 2600); view.idle = 0; e.preventDefault(); };
  canvas.addEventListener('pointerdown', onDown); window.addEventListener('pointermove', onMove); window.addEventListener('pointerup', onUp); canvas.addEventListener('wheel', onWheel, { passive: false });
  const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
  const _p = new THREE.Vector3();
  function pick(cx, cy) {
    if (!built) return;
    const rc = canvas.getBoundingClientRect(); let best = -1, bd = 34 * 34;
    for (let i = 0; i < P.length; i++) {
      _p.set(P[i][0], P[i][1], P[i][2]).project(cam); if (_p.z > 1) continue;
      const x = (_p.x * 0.5 + 0.5) * rc.width + rc.left, y = (0.5 - _p.y * 0.5) * rc.height + rc.top, d = (x - cx) ** 2 + (y - cy) ** 2;
      if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0 && best !== built.sys) window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tgt:' + best, bubbles: true }));
  }

  function update(dt, m, st) {
    U.uTime.value += dt;
    if (!m || !m.run || !m.sector) return;
    build(m);
    const r = m.run, here = P[r.sys] || [0, 0, 0];
    target = st && st.target >= 0 && st.target < P.length ? st.target : -1;
    const there = target >= 0 && target !== r.sys ? P[target] : here;
    // focus: between here and there; zoom to fit both
    tgt.set((here[0] + there[0]) / 2, (here[1] + there[1]) / 2, (here[2] + there[2]) / 2);
    const k = 1 - Math.exp(-3 * dt);
    view.fx += (tgt.x - view.fx) * k; view.fy += (tgt.y - view.fy) * k; view.fz += (tgt.z - view.fz) * k;
    view.idle += dt; if (!view.drag && view.idle > 5) view.yaw += dt * 0.04;
    const cp = Math.cos(view.pitch), sp = Math.sin(view.pitch);
    cam.position.set(view.fx + Math.sin(view.yaw) * cp * view.dist, view.fy + sp * view.dist, view.fz + Math.cos(view.yaw) * cp * view.dist);
    cam.lookAt(view.fx, view.fy, view.fz);
    cam.updateMatrixWorld();
    U.uScale.value = (H * 0.5) / Math.tan(cam.fov * Math.PI / 360);
    curMark.position.set(here[0], here[1], here[2]); curMark.quaternion.copy(cam.quaternion); curMark.scale.setScalar(1 + Math.sin(U.uTime.value * 3) * 0.12);
    tgtMark.visible = target >= 0 && target !== r.sys;
    if (tgtMark.visible) { tgtMark.position.set(there[0], there[1], there[2]); tgtMark.quaternion.copy(cam.quaternion); tgtMark.rotateZ(U.uTime.value * 1.2); tgtMark.scale.setScalar(1.5); }
    for (const c of layer.children) if (c.userData.spin) c.rotation.y += dt * c.userData.spin;
    // labels follow their stars; the target's and here's are always on, far ones fade
    const cw = canvas.clientWidth || 1, ch = canvas.clientHeight || 1;
    for (const L of labels) {
      _p.set(L.x, L.y, L.z).project(cam);
      // write the DOM only when a label actually moved or changed state (no per-frame style churn)
      if (_p.z > 1 || Math.abs(_p.x) > 1.2 || Math.abs(_p.y) > 1.2) { if (L.o !== 0) { L.o = 0; L.d.style.opacity = 0; } continue; }
      const x = Math.round((_p.x * 0.5 + 0.5) * cw + 10), y = Math.round((0.5 - _p.y * 0.5) * ch - 8);
      if (x !== L.x2 || y !== L.y2) { L.x2 = x; L.y2 = y; L.d.style.transform = `translate(${x}px,${y}px)`; }
      const o = L.i < 0 ? 0.85 : (L.i === target || L.i === r.sys ? 1 : 0.78), wgt = L.i === target ? 1 : 0;
      if (o !== L.o) { L.o = o; L.d.style.opacity = o; }
      if (wgt !== L.w) { L.w = wgt; L.d.style.fontWeight = wgt ? '800' : '600'; }
    }
    // the selected route glows: rewrite its colour mode in place (cheap: a handful of segments)
    if (lines && built && (routeT !== target || routeL !== lines)) {
      routeT = target; routeL = lines;
      const a = lines.geometry.attributes.aC, arr = a.array, pos = lines.geometry.attributes.position.array;
      for (let s = 0; s < arr.length / 4; s += 2) {
        if (arr[s * 4 + 3] < 2.5) continue;
        const isRoute = arr[s * 4 + 3] > 2.5 && Math.abs(pos[s * 3 + 1] - here[1]) < 1e-3 && Math.abs(pos[s * 3] - here[0]) < 1e-3;
        if (!isRoute) continue;
        const sel = target >= 0 && Math.abs(pos[s * 3 + 3] - there[0]) < 1e-3 && Math.abs(pos[s * 3 + 5] - there[2]) < 1e-3;
        const mode = sel ? 4 : 3, cr = sel ? 0.5 : 0.12, cg = sel ? 1.4 : 0.4, cb = sel ? 1.7 : 0.55;
        for (const v of [s, s + 1]) { arr[v * 4] = cr; arr[v * 4 + 1] = cg; arr[v * 4 + 2] = cb; arr[v * 4 + 3] = mode; }
      }
      a.needsUpdate = true;
    }
  }
  return {
    scene, cam,
    setActive(on) { active = on; labelsEl.style.display = on ? 'block' : 'none'; if (!on) { ptrs.clear(); view.drag = false; } },
    resize(w, h) { W = w; H = h; cam.aspect = w / h; cam.updateProjectionMatrix(); },
    update, invalidate() { SG.valid = false; },
    dispose() {
      canvas.removeEventListener('pointerdown', onDown); window.removeEventListener('pointermove', onMove); window.removeEventListener('pointerup', onUp); canvas.removeEventListener('wheel', onWheel);
      for (const c of layer.children) if (c.userData.own) c.geometry.dispose();
      for (const d of disposables) d.dispose(); labelsEl.remove();
    },
  };
}

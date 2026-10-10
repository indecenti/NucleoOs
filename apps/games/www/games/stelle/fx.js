// stelle/fx.js — combat effects, all pooled and drawn in a handful of instanced draw calls:
//   · billboard particles (additive fire/glow/sparks + an alpha-blended smoke layer) with velocity
//     stretch, from a procedural 2x2 texture atlas (no image files);
//   · laser bolts as camera-facing HDR ribbons (white-hot core, faction-coloured edge);
//   · instanced debris that keeps the dead ship's momentum, some of it burning;
//   · expanding shockwave shells, shield bubbles that ripple from the exact hit point, engine plumes,
//     fading engine trails, tether beams, missiles with smoke, explosion light flashes.
// Nothing here allocates after creation: every effect takes a slot from a ring and writes into
// preallocated typed arrays.
import { debrisGeometry, hullMaterial } from './kit.js';

const BOLT_VERT = /* glsl */`
attribute vec3 iA; attribute vec3 iB; attribute vec4 iCol;
varying vec2 vUv; varying vec3 vCol;
void main() {
  vec4 a = modelViewMatrix * vec4(iA, 1.0), b = modelViewMatrix * vec4(iB, 1.0);
  vec2 d = b.xy / max(-b.z, 0.1) - a.xy / max(-a.z, 0.1); float L = length(d);
  vec2 dir = L > 1e-5 ? d / L : vec2(0.0, 1.0); vec2 n = vec2(-dir.y, dir.x);
  vec4 p = mix(a, b, position.y);
  p.xy += n * position.x * iCol.a;
  gl_Position = projectionMatrix * p;
  vUv = vec2(position.x * 2.0, position.y); vCol = iCol.rgb;
}`;
const BOLT_FRAG = /* glsl */`
varying vec2 vUv; varying vec3 vCol;
void main() {
  float d = abs(vUv.x); float core = exp(-d * d * 10.0), edge = exp(-d * d * 2.5);
  float tail = smoothstep(1.0, 0.15, vUv.y) * smoothstep(0.0, 0.06, vUv.y);
  vec3 c = vCol * edge * 1.4 + vec3(1.0) * core * 2.4;
  gl_FragColor = vec4(c * tail, 1.0);
}`;
const PART_VERT = /* glsl */`
attribute vec3 iPos; attribute vec3 iVel; attribute vec4 iCol; attribute vec4 iSize;
varying vec2 vUv; varying vec4 vCol;
void main() {
  vec4 mv = modelViewMatrix * vec4(iPos, 1.0);
  vec2 c = position.xy; float s = iSize.x; vec2 off;
  if (iSize.z > 0.0) {
    vec2 d = (modelViewMatrix * vec4(iVel, 0.0)).xy; float L = length(d);
    vec2 dir = L > 1e-4 ? d / L : vec2(1.0, 0.0); vec2 pp = vec2(-dir.y, dir.x);
    off = dir * c.x * (s + L * iSize.z) + pp * c.y * s;
  } else { float cr = cos(iSize.y), sr = sin(iSize.y); off = vec2(c.x * cr - c.y * sr, c.x * sr + c.y * cr) * s; }
  mv.xy += off;
  gl_Position = projectionMatrix * mv;
  vUv = (uv + vec2(mod(iSize.w, 2.0), floor(iSize.w / 2.0))) * 0.5; vCol = iCol;
}`;
const PART_FRAG_ADD = /* glsl */`uniform sampler2D uMap; varying vec2 vUv; varying vec4 vCol;
void main() { vec4 t = texture2D(uMap, vUv); gl_FragColor = vec4(vCol.rgb * t.rgb * t.a * vCol.a, 1.0); }`;
const PART_FRAG_ALPHA = /* glsl */`uniform sampler2D uMap; varying vec2 vUv; varying vec4 vCol;
void main() { vec4 t = texture2D(uMap, vUv); gl_FragColor = vec4(vCol.rgb * t.rgb, t.a * vCol.a); }`;
const SHIELD_FRAG = /* glsl */`
uniform vec3 uCol, uHit; uniform float uAge, uK, uTime; varying vec3 vN; varying vec3 vO; varying vec3 vV;
void main() {
  vec3 n = normalize(vN); float fr = pow(1.0 - min(abs(dot(n, vV / max(length(vV), 1e-4))), 1.0), 2.2);
  float d = distance(normalize(vO), uHit);
  float ring = smoothstep(0.16, 0.0, abs(d - uAge * 1.8)) * (1.0 - uAge);
  float spot = exp(-d * d * 14.0) * (1.0 - uAge) * 2.2;
  vec3 h = abs(fract(normalize(vO) * 9.0 + uTime * 0.2) - 0.5);
  float hex = smoothstep(0.42, 0.5, max(h.x, max(h.y, h.z))) * exp(-d * d * 5.0) * (1.0 - uAge);
  gl_FragColor = vec4(uCol * (fr * 0.35 * (1.0 - uAge) + ring * 1.4 + spot + hex * 0.8) * uK, 1.0);
}`;
const SHELL_VERT = /* glsl */`varying vec3 vN; varying vec3 vO; varying vec3 vV;
void main() { vO = position; vN = normalize(mat3(modelMatrix) * normal); vec4 w = modelMatrix * vec4(position, 1.0); vV = cameraPosition - w.xyz; gl_Position = projectionMatrix * viewMatrix * w; }`;
const SHOCK_FRAG = /* glsl */`uniform vec3 uCol; uniform float uAge; varying vec3 vN; varying vec3 vO; varying vec3 vV;
void main() { float fr = pow(1.0 - min(abs(dot(normalize(vN), vV / max(length(vV), 1e-4))), 1.0), 3.0); float a = max(1.0 - uAge, 0.0); gl_FragColor = vec4(uCol * fr * a * a * 2.5, 1.0); }`;
// Engine exhaust, two instanced layers: a tight hot core sprite at each nozzle (bright only when you look
// into the nozzle) and a camera-facing ribbon along the exhaust axis that tapers and cools with distance.
// Peak HDR stays a few units on a few pixels, so the bloom gives a tight glow instead of a white-out.
const NOZ_VERT = /* glsl */`
attribute vec3 iP; attribute vec3 iAx; attribute vec4 iC; attribute vec4 iD;
varying vec2 vUv; varying vec3 vCol; varying float vK;
void main() {
  vec3 toCam = cameraPosition - iP; float dl = length(toCam);
  float face = dl > 1e-3 ? dot(iAx, toCam / dl) : 0.0;
  vec4 mv = modelViewMatrix * vec4(iP + iAx * iD.x * 0.12, 1.0);
  float sz = iD.x * (0.8 + 0.22 * iC.a + 0.45 * iD.y);
  mv.xy += position.xy * sz;
  gl_Position = projectionMatrix * mv;
  vUv = position.xy * 2.0; vCol = iC.rgb; vK = iC.a * smoothstep(-0.2, 0.55, face);
}`;
const NOZ_FRAG = /* glsl */`
varying vec2 vUv; varying vec3 vCol; varying float vK;
void main() {
  float r2 = dot(vUv, vUv); if (r2 > 1.0) discard;
  float core = exp(-r2 * 20.0), halo = exp(-r2 * 4.5) * 0.18;
  vec3 c = mix(vec3(1.0, 0.97, 0.9), vCol, smoothstep(0.02, 0.35, r2));
  gl_FragColor = vec4(c * (core * 1.7 + halo) * vK, 1.0);
}`;
const RIB_VERT = /* glsl */`
attribute vec3 iP; attribute vec3 iAx; attribute vec4 iC; attribute vec4 iD;
varying vec2 vUv; varying vec3 vCol; varying float vK; varying float vB; varying float vSeed;
void main() {
  vec4 a = modelViewMatrix * vec4(iP, 1.0), b = modelViewMatrix * vec4(iP + iAx * iD.z, 1.0);
  float za = max(-a.z, 0.1), zb = max(-b.z, 0.1);
  vec2 d = b.xy / zb - a.xy / za; float L = length(d);
  vec2 dir = L > 1e-6 ? d / L : vec2(0.0, 1.0); vec2 n = vec2(-dir.y, dir.x);
  float wA = iD.x * 1.15, wB = iD.x * 0.3;
  vec4 p = mix(a, b, position.y);
  p.xy += n * position.x * mix(wA, wB, position.y);
  gl_Position = projectionMatrix * p;
  // seen end-on the ribbon collapses: fade it out and let the nozzle core carry the look
  float ratio = L / max(wA / za, 1e-4);
  vUv = vec2(position.x * 2.0, position.y); vCol = iC.rgb; vK = iC.a * smoothstep(0.35, 1.6, ratio); vB = iD.y; vSeed = iD.w;
}`;
const RIB_FRAG = /* glsl */`
uniform float uTime; varying vec2 vUv; varying vec3 vCol; varying float vK; varying float vB; varying float vSeed;
void main() {
  float x = abs(vUv.x), t = vUv.y;
  float core = exp(-x * x * 10.0), edge = exp(-x * x * 2.6) * 0.3;
  float fall = pow(1.0 - t, 1.7) * smoothstep(0.0, 0.05, t);
  float fl = 0.86 + 0.14 * sin(uTime * 57.0 + t * 26.0 + vSeed * 6.0);
  float diamonds = 1.0 + vB * 0.8 * pow(0.5 + 0.5 * cos(t * 34.0 - uTime * 24.0), 8.0) * (1.0 - t);
  vec3 c = mix(vec3(1.0, 0.95, 0.88), vCol, smoothstep(0.0, 0.3, t + x * 0.45));
  gl_FragColor = vec4(c * (core * 1.05 + edge) * fall * fl * diamonds * vK, 1.0);
}`;
const TRAIL_VERT = /* glsl */`attribute vec3 aNext; attribute float aSide; attribute float aT; attribute vec4 aCol; varying vec4 vCol; varying float vT; varying float vD;
void main() { vec4 a = modelViewMatrix * vec4(position, 1.0), b = modelViewMatrix * vec4(aNext, 1.0);
  vec2 d = b.xy / max(-b.z, 0.1) - a.xy / max(-a.z, 0.1); float L = length(d); vec2 dir = L > 1e-6 ? d / L : vec2(0.0, 1.0);
  a.xy += vec2(-dir.y, dir.x) * aSide * aCol.a * (1.0 - aT * 0.7);
  gl_Position = projectionMatrix * a; vCol = aCol; vT = aT; vD = -a.z; }`;
const TRAIL_FRAG = /* glsl */`varying vec4 vCol; varying float vT; varying float vD;
void main() { float a = (1.0 - vT); a *= a * smoothstep(12.0, 70.0, vD); gl_FragColor = vec4(vCol.rgb * a, 1.0); }`;
const BEAM_FRAG = /* glsl */`uniform vec3 uCol; uniform float uTime; varying vec2 vUv;
void main() { float x = abs(vUv.x - 0.5) * 2.0; float wob = sin(vUv.y * 60.0 - uTime * 30.0) * 0.5 + 0.5;
  float core = exp(-x * x * 18.0) * (0.7 + 0.6 * wob), glow = exp(-x * x * 3.0) * 0.35;
  gl_FragColor = vec4(uCol * (core * 3.0 + glow), 1.0); }`;

// procedural 2x2 atlas: glow, fire puff, smoke puff, star glint
function makeAtlas(THREE) {
  const S = 128, c = document.createElement('canvas'); c.width = c.height = S * 2;
  const g = c.getContext('2d');
  const radial = (x, y, r, stops) => { const gr = g.createRadialGradient(x, y, 0, x, y, r); stops.forEach(([o, col]) => gr.addColorStop(o, col)); g.fillStyle = gr; g.beginPath(); g.arc(x, y, r, 0, 7); g.fill(); };
  radial(S / 2, S / 2, S / 2, [[0, 'rgba(255,255,255,1)'], [0.25, 'rgba(255,255,255,0.55)'], [1, 'rgba(255,255,255,0)']]);
  let seed = 7; const rnd = () => { seed = (seed * 16807) % 2147483647; return seed / 2147483647; };
  for (let i = 0; i < 26; i++) { const a = rnd() * 6.28, d = rnd() * S * 0.22, r = S * (0.1 + rnd() * 0.22); radial(S * 1.5 + Math.cos(a) * d, S / 2 + Math.sin(a) * d, r, [[0, 'rgba(255,255,255,0.45)'], [1, 'rgba(255,255,255,0)']]); }
  for (let i = 0; i < 30; i++) { const a = rnd() * 6.28, d = rnd() * S * 0.24, r = S * (0.12 + rnd() * 0.2); radial(S / 2 + Math.cos(a) * d, S * 1.5 + Math.sin(a) * d, r, [[0, 'rgba(255,255,255,0.22)'], [1, 'rgba(255,255,255,0)']]); }
  radial(S * 1.5, S * 1.5, S * 0.18, [[0, 'rgba(255,255,255,1)'], [1, 'rgba(255,255,255,0)']]);
  g.fillStyle = 'rgba(255,255,255,0.9)';
  for (const [w, h] of [[S * 0.9, 3], [3, S * 0.9]]) { const gr = g.createRadialGradient(S * 1.5, S * 1.5, 0, S * 1.5, S * 1.5, S * 0.45); gr.addColorStop(0, 'rgba(255,255,255,1)'); gr.addColorStop(1, 'rgba(255,255,255,0)'); g.fillStyle = gr; g.fillRect(S * 1.5 - w / 2, S * 1.5 - h / 2, w, h); }
  const t = new THREE.CanvasTexture(c); t.generateMipmaps = true; return t;
}

// upload only the live prefix of a dynamic instanced attribute
function rng(a, count) { a.clearUpdateRanges(); a.addUpdateRange(0, count); a.needsUpdate = true; }

// ---- particle layer -----------------------------------------------------------------------------------
class Particles {
  constructor(THREE, scene, N, additive, atlas) {
    this.N = N; this.n = 0; this.head = 0;
    const F = (k) => new Float32Array(N * k);
    Object.assign(this, { p: F(3), v: F(3), life: F(1), max: F(1), s0: F(1), s1: F(1), rot: F(1), rv: F(1), c0: F(4), c1: F(4), drag: F(1), tile: F(1), str: F(1) });
    const g = new THREE.InstancedBufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute([-0.5, -0.5, 0, 0.5, -0.5, 0, 0.5, 0.5, 0, -0.5, 0.5, 0], 3));
    g.setAttribute('uv', new THREE.Float32BufferAttribute([0, 0, 1, 0, 1, 1, 0, 1], 2));
    g.setIndex([0, 1, 2, 0, 2, 3]);
    const IA = (k) => { const a = new THREE.InstancedBufferAttribute(new Float32Array(N * k), k); a.setUsage(THREE.DynamicDrawUsage); return a; };
    this.aPos = IA(3); this.aVel = IA(3); this.aCol = IA(4); this.aSize = IA(4);
    g.setAttribute('iPos', this.aPos); g.setAttribute('iVel', this.aVel); g.setAttribute('iCol', this.aCol); g.setAttribute('iSize', this.aSize);
    g.instanceCount = 0;
    this.mat = new THREE.ShaderMaterial({ vertexShader: PART_VERT, fragmentShader: additive ? PART_FRAG_ADD : PART_FRAG_ALPHA, uniforms: { uMap: { value: atlas } },
      transparent: true, depthWrite: false, blending: additive ? THREE.AdditiveBlending : THREE.NormalBlending });
    this.mesh = new THREE.Mesh(g, this.mat); this.mesh.frustumCulled = false; this.mesh.renderOrder = additive ? 5 : 4;
    this.geo = g; scene.add(this.mesh);
    this.alive = new Uint8Array(N);
  }
  // spawn: position, velocity, life, size from->to, colour rgba from->to, tile, stretch, drag
  add(x, y, z, vx, vy, vz, life, s0, s1, r0, g0, b0, a0, r1, g1, b1, a1, tile = 0, str = 0, drag = 0) {
    const i = this.head; this.head = (this.head + 1) % this.N;
    this.alive[i] = 1;
    this.p[i * 3] = x; this.p[i * 3 + 1] = y; this.p[i * 3 + 2] = z; this.v[i * 3] = vx; this.v[i * 3 + 1] = vy; this.v[i * 3 + 2] = vz;
    this.life[i] = life; this.max[i] = life; this.s0[i] = s0; this.s1[i] = s1; this.rot[i] = Math.random() * 6.28; this.rv[i] = (Math.random() - 0.5) * 2;
    this.c0.set([r0, g0, b0, a0], i * 4); this.c1.set([r1, g1, b1, a1], i * 4); this.tile[i] = tile; this.str[i] = str; this.drag[i] = drag;
  }
  update(dt) {
    let n = 0;
    const P = this.aPos.array, V = this.aVel.array, C = this.aCol.array, S = this.aSize.array;
    for (let i = 0; i < this.N; i++) {
      if (!this.alive[i]) continue;
      const l = this.life[i] - dt; this.life[i] = l;
      if (l <= 0) { this.alive[i] = 0; continue; }
      const k = 1 - l / this.max[i], dr = this.drag[i] ? Math.exp(-this.drag[i] * dt) : 1;
      const i3 = i * 3, i4 = i * 4;
      this.v[i3] *= dr; this.v[i3 + 1] *= dr; this.v[i3 + 2] *= dr;
      this.p[i3] += this.v[i3] * dt; this.p[i3 + 1] += this.v[i3 + 1] * dt; this.p[i3 + 2] += this.v[i3 + 2] * dt;
      this.rot[i] += this.rv[i] * dt;
      const o3 = n * 3, o4 = n * 4;
      P[o3] = this.p[i3]; P[o3 + 1] = this.p[i3 + 1]; P[o3 + 2] = this.p[i3 + 2];
      V[o3] = this.v[i3]; V[o3 + 1] = this.v[i3 + 1]; V[o3 + 2] = this.v[i3 + 2];
      const c0 = this.c0, c1 = this.c1;
      C[o4] = c0[i4] + (c1[i4] - c0[i4]) * k; C[o4 + 1] = c0[i4 + 1] + (c1[i4 + 1] - c0[i4 + 1]) * k; C[o4 + 2] = c0[i4 + 2] + (c1[i4 + 2] - c0[i4 + 2]) * k; C[o4 + 3] = c0[i4 + 3] + (c1[i4 + 3] - c0[i4 + 3]) * k;
      S[o4] = this.s0[i] + (this.s1[i] - this.s0[i]) * (1 - (1 - k) * (1 - k)); S[o4 + 1] = this.rot[i]; S[o4 + 2] = this.str[i]; S[o4 + 3] = this.tile[i];
      n++;
    }
    this.geo.instanceCount = n; this.n = n;
    if (n) { rng(this.aPos, n * 3); rng(this.aVel, n * 3); rng(this.aCol, n * 4); rng(this.aSize, n * 4); }
  }
  clear() { this.alive.fill(0); this.geo.instanceCount = 0; }
  dispose() { this.geo.dispose(); this.mat.dispose(); this.mesh.removeFromParent(); }
}

export function createFx(THREE, scene, Q) {
  const atlas = makeAtlas(THREE);
  const add = new Particles(THREE, scene, Q.particles, true, atlas);
  const smoke = new Particles(THREE, scene, Math.round(Q.particles * 0.45), false, atlas);
  const R = Math.random;
  const rs = () => { let x, y, z, l; do { x = R() * 2 - 1; y = R() * 2 - 1; z = R() * 2 - 1; l = x * x + y * y + z * z; } while (l > 1 || l < 0.01); l = Math.sqrt(l); _r[0] = x / l; _r[1] = y / l; _r[2] = z / l; return _r; };
  const _r = [0, 0, 0];

  // ---- bolts ------------------------------------------------------------------------------------------------
  const NB = 700;
  const bg = new THREE.InstancedBufferGeometry();
  bg.setAttribute('position', new THREE.Float32BufferAttribute([-0.5, 0, 0, 0.5, 0, 0, 0.5, 1, 0, -0.5, 1, 0], 3)); bg.setIndex([0, 1, 2, 0, 2, 3]);
  const bA = new THREE.InstancedBufferAttribute(new Float32Array(NB * 3), 3), bB = new THREE.InstancedBufferAttribute(new Float32Array(NB * 3), 3), bC = new THREE.InstancedBufferAttribute(new Float32Array(NB * 4), 4);
  [bA, bB, bC].forEach((a) => a.setUsage(THREE.DynamicDrawUsage));
  bg.setAttribute('iA', bA); bg.setAttribute('iB', bB); bg.setAttribute('iCol', bC); bg.instanceCount = 0;
  const bMat = new THREE.ShaderMaterial({ vertexShader: BOLT_VERT, fragmentShader: BOLT_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending });
  const bolts = new THREE.Mesh(bg, bMat); bolts.frustumCulled = false; bolts.renderOrder = 6; scene.add(bolts);
  let nb = 0;
  function boltsBegin() { nb = 0; }
  function bolt(hx, hy, hz, tx, ty, tz, r, g, b, w) {
    if (nb >= NB) return;
    bA.array[nb * 3] = hx; bA.array[nb * 3 + 1] = hy; bA.array[nb * 3 + 2] = hz;
    bB.array[nb * 3] = tx; bB.array[nb * 3 + 1] = ty; bB.array[nb * 3 + 2] = tz;
    bC.array[nb * 4] = r; bC.array[nb * 4 + 1] = g; bC.array[nb * 4 + 2] = b; bC.array[nb * 4 + 3] = w;
    nb++;
  }
  function boltsEnd() { bg.instanceCount = nb; if (nb) { rng(bA, nb * 3); rng(bB, nb * 3); rng(bC, nb * 4); } }

  // ---- debris -----------------------------------------------------------------------------------------------
  const ND = Q.debris;
  const dMat = hullMaterial(THREE, { metal: 0.6, rough: 0.5, panel: 2.2, env: 0.6 });
  dMat.vertexColors = true;
  const deb = new THREE.InstancedMesh(debrisGeometry(THREE), dMat, ND); deb.frustumCulled = false; deb.count = 0;
  deb.instanceColor = new THREE.InstancedBufferAttribute(new Float32Array(ND * 3), 3); scene.add(deb);
  const D = { p: new Float32Array(ND * 3), v: new Float32Array(ND * 3), ax: new Float32Array(ND * 3), ang: new Float32Array(ND), av: new Float32Array(ND), life: new Float32Array(ND), sc: new Float32Array(ND), burn: new Float32Array(ND), col: new Float32Array(ND * 3), head: 0, alive: new Uint8Array(ND) };
  const _m = new THREE.Matrix4(), _q = new THREE.Quaternion(), _s = new THREE.Vector3(), _p = new THREE.Vector3(), _a = new THREE.Vector3(), _col = new THREE.Color();
  function debris(x, y, z, vx, vy, vz, n, size, col, spread = 40, burnP = 0.4) {
    for (let k = 0; k < n; k++) {
      const i = D.head; D.head = (D.head + 1) % ND; D.alive[i] = 1;
      const d = rs(), sp = spread * (0.3 + R() * 0.9);
      D.p.set([x + d[0] * size * 0.5, y + d[1] * size * 0.5, z + d[2] * size * 0.5], i * 3);
      D.v.set([vx * 0.85 + d[0] * sp, vy * 0.85 + d[1] * sp, vz * 0.85 + d[2] * sp], i * 3);
      const a = rs(); D.ax.set([a[0], a[1], a[2]], i * 3); D.ang[i] = R() * 6; D.av[i] = (R() - 0.5) * 9;
      D.life[i] = 2.5 + R() * 4; D.sc[i] = size * (0.12 + R() * 0.3); D.burn[i] = R() < burnP ? 1 : 0;
      const t = 0.7 + R() * 0.4; D.col.set([col[0] * t, col[1] * t, col[2] * t], i * 3);
    }
  }
  function debrisUpdate(dt) {
    let n = 0;
    for (let i = 0; i < ND; i++) {
      if (!D.alive[i]) continue;
      D.life[i] -= dt; if (D.life[i] <= 0) { D.alive[i] = 0; continue; }
      const i3 = i * 3;
      D.p[i3] += D.v[i3] * dt; D.p[i3 + 1] += D.v[i3 + 1] * dt; D.p[i3 + 2] += D.v[i3 + 2] * dt; D.ang[i] += D.av[i] * dt;
      const s = D.sc[i] * Math.min(1, D.life[i] * 1.5);
      _p.set(D.p[i3], D.p[i3 + 1], D.p[i3 + 2]); _a.set(D.ax[i3], D.ax[i3 + 1], D.ax[i3 + 2]); _q.setFromAxisAngle(_a, D.ang[i]); _s.setScalar(s);
      _m.compose(_p, _q, _s); deb.setMatrixAt(n, _m);
      _col.setRGB(D.col[i3], D.col[i3 + 1], D.col[i3 + 2]); deb.setColorAt(n, _col);
      if (D.burn[i] && R() < dt * 30 && D.life[i] > 1) {
        add.add(_p.x, _p.y, _p.z, D.v[i3] * 0.2, D.v[i3 + 1] * 0.2, D.v[i3 + 2] * 0.2, 0.5, s * 0.8, s * 2.2, 4, 1.8, 0.5, 1, 0.8, 0.15, 0.03, 0, 1);
        if (R() < 0.5) smoke.add(_p.x, _p.y, _p.z, 0, 0, 0, 1.6, s, s * 4, 0.05, 0.05, 0.055, 0.5, 0.02, 0.02, 0.02, 0, 2);
      }
      n++;
    }
    deb.count = n; if (n) { deb.instanceMatrix.needsUpdate = true; deb.instanceColor.needsUpdate = true; }
  }

  // ---- shells: shockwaves + shield bubbles ------------------------------------------------------------------------
  const shellGeo = new THREE.SphereGeometry(1, 32, 20);
  const shocks = [];
  for (let i = 0; i < 8; i++) { const m = new THREE.Mesh(shellGeo, new THREE.ShaderMaterial({ vertexShader: SHELL_VERT, fragmentShader: SHOCK_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, uniforms: { uCol: { value: new THREE.Color() }, uAge: { value: 1 } } })); m.visible = false; m.frustumCulled = false; m.userData = { t: 0, dur: 1, r: 1 }; scene.add(m); shocks.push(m); }
  let shockI = 0;
  function shock(x, y, z, r, cr, cg, cb, dur = 0.6) { const m = shocks[shockI++ % shocks.length]; m.position.set(x, y, z); m.userData.t = 0; m.userData.dur = dur; m.userData.r = r; m.material.uniforms.uCol.value.setRGB(cr, cg, cb); m.visible = true; }
  const shields = [];
  for (let i = 0; i < 14; i++) { const m = new THREE.Mesh(shellGeo, new THREE.ShaderMaterial({ vertexShader: SHELL_VERT, fragmentShader: SHIELD_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, uniforms: { uCol: { value: new THREE.Color() }, uHit: { value: new THREE.Vector3() }, uAge: { value: 1 }, uK: { value: 1 }, uTime: { value: 0 } } })); m.visible = false; m.frustumCulled = false; m.userData = { t: 1, dur: 0.55, owner: null }; shields.push(m); }
  let shieldI = 0;
  // host: the ship's Object3D; sx,sy,sz: bubble radii; (hx,hy,hz): hit dir in ship space
  function shieldHit(host, sx, sy, sz, hx, hy, hz, col, k) {
    let m = null;
    for (let i = 0; i < shields.length; i++) if (shields[i].userData.owner === host && shields[i].visible) { m = shields[i]; break; }
    if (!m) m = shields[shieldI++ % shields.length];
    if (m.parent !== host) host.add(m);
    m.userData.owner = host; m.userData.t = 0; m.visible = true; m.scale.set(sx, sy, sz);
    const l = Math.hypot(hx / sx, hy / sy, hz / sz) || 1; m.material.uniforms.uHit.value.set(hx / sx / l, hy / sy / l, hz / sz / l);
    m.material.uniforms.uCol.value.setRGB(col[0], col[1], col[2]); m.material.uniforms.uK.value = Math.min(2.2, 0.7 + k);
  }

  // ---- engine plumes: nozzle cores + exhaust ribbons, one instanced draw each ------------------------------------------
  const NPL = 320;
  const mkPlumeGeo = (verts, idx) => {
    const g = new THREE.InstancedBufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(verts, 3)); g.setIndex(idx);
    return g;
  };
  const noz = mkPlumeGeo([-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], [0, 1, 2, 0, 2, 3]);
  const rib = mkPlumeGeo([-0.5, 0, 0, 0.5, 0, 0, 0.5, 1, 0, -0.5, 1, 0], [0, 1, 2, 0, 2, 3]);
  const pA = { P: new THREE.InstancedBufferAttribute(new Float32Array(NPL * 3), 3), Ax: new THREE.InstancedBufferAttribute(new Float32Array(NPL * 3), 3),
    C: new THREE.InstancedBufferAttribute(new Float32Array(NPL * 4), 4), D: new THREE.InstancedBufferAttribute(new Float32Array(NPL * 4), 4) };
  for (const a of Object.values(pA)) a.setUsage(THREE.DynamicDrawUsage);
  for (const g of [noz, rib]) { g.setAttribute('iP', pA.P); g.setAttribute('iAx', pA.Ax); g.setAttribute('iC', pA.C); g.setAttribute('iD', pA.D); g.instanceCount = 0; }
  const T = { value: 0 };
  const plumeMatOf = (v, f, ro) => new THREE.ShaderMaterial({ vertexShader: v, fragmentShader: f, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, side: THREE.DoubleSide, uniforms: { uTime: T } });
  const nozMesh = new THREE.Mesh(noz, plumeMatOf(NOZ_VERT, NOZ_FRAG)), ribMesh = new THREE.Mesh(rib, plumeMatOf(RIB_VERT, RIB_FRAG));
  for (const m of [nozMesh, ribMesh]) { m.frustumCulled = false; m.renderOrder = 3; scene.add(m); }
  let npl = 0;
  function plumesBegin() { npl = 0; }
  // nozzle exit (x,y,z), exhaust axis (unit, pointing away from the ship), radius, ribbon length, colour, intensity k (0..2), boost 0..1
  function plume(x, y, z, ax, ay, az, r, len, cr, cg, cb, k, boost = 0) {
    if (npl >= NPL || k <= 0.001) return;
    const i3 = npl * 3, i4 = npl * 4;
    pA.P.array[i3] = x; pA.P.array[i3 + 1] = y; pA.P.array[i3 + 2] = z;
    pA.Ax.array[i3] = ax; pA.Ax.array[i3 + 1] = ay; pA.Ax.array[i3 + 2] = az;
    pA.C.array[i4] = cr; pA.C.array[i4 + 1] = cg; pA.C.array[i4 + 2] = cb; pA.C.array[i4 + 3] = k;
    pA.D.array[i4] = r; pA.D.array[i4 + 1] = boost; pA.D.array[i4 + 2] = len; pA.D.array[i4 + 3] = npl * 0.618;
    npl++;
  }
  function plumesEnd() {
    noz.instanceCount = npl; rib.instanceCount = npl;
    if (npl) { rng(pA.P, npl * 3); rng(pA.Ax, npl * 3); rng(pA.C, npl * 4); rng(pA.D, npl * 4); }
  }

  // ---- trails (ribbons) ------------------------------------------------------------------------------------------------
  const NT = Q.trails, TP = 22, SEG = TP - 1;
  const tv = NT * SEG * 6;
  const tg = new THREE.BufferGeometry();
  const tPos = new Float32Array(tv * 3), tNext = new Float32Array(tv * 3), tSide = new Float32Array(tv), tT = new Float32Array(tv), tCol = new Float32Array(tv * 4);
  tg.setAttribute('position', new THREE.BufferAttribute(tPos, 3).setUsage(THREE.DynamicDrawUsage)); tg.setAttribute('aNext', new THREE.BufferAttribute(tNext, 3).setUsage(THREE.DynamicDrawUsage));
  tg.setAttribute('aSide', new THREE.BufferAttribute(tSide, 1)); tg.setAttribute('aT', new THREE.BufferAttribute(tT, 1)); tg.setAttribute('aCol', new THREE.BufferAttribute(tCol, 4).setUsage(THREE.DynamicDrawUsage));
  for (let t = 0; t < NT; t++) for (let s = 0; s < SEG; s++) { const o = (t * SEG + s) * 6; const sides = [-1, 1, 1, -1, 1, -1], ts = [0, 0, 1, 0, 1, 1]; for (let k = 0; k < 6; k++) { tSide[o + k] = sides[k]; tT[o + k] = (s + ts[k]) / SEG; } }
  tg.setDrawRange(0, 0);
  const trailMesh = new THREE.Mesh(tg, new THREE.ShaderMaterial({ vertexShader: TRAIL_VERT, fragmentShader: TRAIL_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, side: THREE.DoubleSide }));
  trailMesh.frustumCulled = false; trailMesh.renderOrder = 2; scene.add(trailMesh);
  const trails = [];   // { pts: Float32Array(TP*3), n, t, col:[r,g,b], w, owner }
  for (let i = 0; i < NT; i++) trails.push({ pts: new Float32Array(TP * 3), n: 0, acc: 0, col: [1, 1, 1], w: 1, used: false, key: null });
  function trailFor(key, col, w) {
    let t = null;
    for (let i = 0; i < trails.length; i++) if (trails[i].used && trails[i].key === key) { t = trails[i]; break; }
    if (!t) { for (let i = 0; i < trails.length; i++) if (!trails[i].used) { t = trails[i]; break; } if (!t) return null; t.used = true; t.key = key; t.n = 0; t.acc = 0; }
    t.col = col; t.w = w; t.seen = true;
    return t;
  }
  function trailPush(t, x, y, z, dt) {
    t.acc += dt;
    if (t.n > 0 && t.acc < 0.035) { t.pts[0] = x; t.pts[1] = y; t.pts[2] = z; return; }   // head follows the ship
    t.acc = 0;
    t.pts.copyWithin(3, 0, (TP - 1) * 3); t.pts[0] = x; t.pts[1] = y; t.pts[2] = z; t.n = Math.min(TP, t.n + 1);
  }
  function trailsBuild(fade) {
    let v = 0;
    for (const t of trails) {
      if (!t.used) continue;
      if (!t.seen) { t.fade = (t.fade || 1) - 0.05; if (t.fade <= 0) { t.used = false; t.key = null; t.fade = 1; continue; } } else t.fade = 1;
      t.seen = false;
      for (let s = 0; s < Math.min(SEG, t.n - 1); s++) {
        const a = s * 3, b = (s + 1) * 3;
        for (let k = 0; k < 6; k++) {
          const atB = k === 2 || k === 4 || k === 5, e = atB ? b : a, o = (v + k) * 3;
          tPos[o] = t.pts[e]; tPos[o + 1] = t.pts[e + 1]; tPos[o + 2] = t.pts[e + 2];
          // ribbon direction = this segment (extrapolated past its far end)
          tNext[o] = t.pts[e] + (t.pts[b] - t.pts[a]); tNext[o + 1] = t.pts[e + 1] + (t.pts[b + 1] - t.pts[a + 1]); tNext[o + 2] = t.pts[e + 2] + (t.pts[b + 2] - t.pts[a + 2]);
          const c4 = (v + k) * 4; tCol[c4] = t.col[0] * t.fade * fade; tCol[c4 + 1] = t.col[1] * t.fade * fade; tCol[c4 + 2] = t.col[2] * t.fade * fade; tCol[c4 + 3] = t.w;
          tT[v + k] = (s + (atB ? 1 : 0)) / SEG;
        }
        v += 6;
      }
    }
    tg.setDrawRange(0, v);
    tg.attributes.position.needsUpdate = true; tg.attributes.aNext.needsUpdate = true; tg.attributes.aCol.needsUpdate = true; tg.attributes.aT.needsUpdate = true;
  }
  function trailsClear() { for (const t of trails) { t.used = false; t.key = null; t.n = 0; } tg.setDrawRange(0, 0); }

  // ---- tether beams ---------------------------------------------------------------------------------------------------------
  const beamGeo = new THREE.PlaneGeometry(1, 1, 1, 8); beamGeo.translate(0, 0.5, 0);
  const beams = [];
  for (let i = 0; i < 8; i++) { const m = new THREE.Mesh(beamGeo, new THREE.ShaderMaterial({ vertexShader: `varying vec2 vUv; void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`, fragmentShader: BEAM_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, side: THREE.DoubleSide, uniforms: { uCol: { value: new THREE.Color(1.2, 0.55, 0.15) }, uTime: T } })); m.visible = false; m.frustumCulled = false; scene.add(m); beams.push(m); }
  const _ba = new THREE.Vector3(), _bb = new THREE.Vector3(), _bd = new THREE.Vector3(), _bc = new THREE.Vector3(), _bx = new THREE.Vector3(), _bz = new THREE.Vector3();
  function beamsBegin() { for (const b of beams) b.visible = false; }
  function beam(i, ax, ay, az, bx, by, bz, cam, w = 3, cr = 1.2, cg = 0.55, cb = 0.15) {
    const m = beams[i]; if (!m) return;
    m.material.uniforms.uCol.value.setRGB(cr, cg, cb);
    _ba.set(ax, ay, az); _bb.set(bx, by, bz); _bd.subVectors(_bb, _ba); const L = _bd.length(); if (L < 1) return;
    _bd.divideScalar(L); _bc.subVectors(cam.position, _ba); _bx.crossVectors(_bd, _bc).normalize();
    _bz.crossVectors(_bx, _bd);
    _m.makeBasis(_bx, _bd, _bz); m.quaternion.setFromRotationMatrix(_m);
    m.position.copy(_ba); m.scale.set(w, L, 1); m.visible = true;
  }

  // ---- point-light flashes ---------------------------------------------------------------------------------------------------
  const lights = [new THREE.PointLight(0xffaa66, 0, 600, 1), new THREE.PointLight(0xffaa66, 0, 600, 1)];   // decay 1: no singular highlights next to the blast
  lights.forEach((l) => { l.userData = { t: 1, dur: 0.4, k: 0 }; scene.add(l); });
  let lightI = 0;
  function flash(x, y, z, r, g, b, k, range, dur = 0.45) { const l = lights[lightI++ % lights.length]; l.position.set(x, y, z); l.color.setRGB(r, g, b); l.distance = range; l.userData = { t: 0, dur, k }; }

  // ---- composite effects -----------------------------------------------------------------------------------------------------------
  function explosion(x, y, z, vx, vy, vz, size, col, big = false) {
    const s = size;
    add.add(x, y, z, vx, vy, vz, big ? 0.35 : 0.18, s * 3, s * (big ? 14 : 9), 9, 8, 6, 1, 3, 1.4, 0.4, 0, 0);
    const nf = Math.min(46, (big ? 26 : 12) + (s | 0));
    for (let i = 0; i < nf; i++) {
      const d = rs(), sp = s * (1.5 + R() * 4) * (big ? 1.4 : 1);
      add.add(x + d[0] * s * 0.3, y + d[1] * s * 0.3, z + d[2] * s * 0.3, vx + d[0] * sp, vy + d[1] * sp, vz + d[2] * sp, 0.5 + R() * 0.8 * (big ? 1.6 : 1), s * (0.6 + R() * 0.5), s * (1.8 + R() * 1.8), 7, 4.5, 1.6, 1, 1.4, 0.22, 0.04, 0, 1, 0, 1.2);
    }
    const ns = big ? 60 : 28;
    for (let i = 0; i < ns; i++) { const d = rs(), sp = 60 + R() * 220; add.add(x, y, z, vx + d[0] * sp, vy + d[1] * sp, vz + d[2] * sp, 0.35 + R() * 0.7, 0.35 + R() * 0.4, 0.15, 5, 3.6, 1.8, 1, 2.4, 0.7, 0.15, 0, 3, 0.05, 0.6); }
    for (let i = 0; i < (big ? 14 : 7); i++) { const d = rs(), sp = s * (0.6 + R() * 1.4); smoke.add(x + d[0] * s, y + d[1] * s, z + d[2] * s, vx * 0.6 + d[0] * sp, vy * 0.6 + d[1] * sp, vz * 0.6 + d[2] * sp, 2.2 + R() * 2, s * 1.2, s * (4 + R() * 3), 0.06, 0.055, 0.06, 0.55, 0.02, 0.02, 0.025, 0, 2, 0, 0.4); }
    debris(x, y, z, vx, vy, vz, big ? 18 : 7, s * 2, col, s * (big ? 9 : 6), big ? 0.6 : 0.35);
    shock(x, y, z, s * (big ? 16 : 9), 1.4, 0.9, 0.55, big ? 0.9 : 0.55);
    flash(x, y, z, 1, 0.65, 0.35, big ? 120 : 45, s * (big ? 110 : 60), big ? 0.8 : 0.4);
  }
  function hitSparks(x, y, z, nx, ny, nz, r, g, b, n = 10) {
    for (let i = 0; i < n; i++) { const d = rs(), sp = 30 + R() * 140; add.add(x, y, z, (nx + d[0] * 0.8) * sp, (ny + d[1] * 0.8) * sp, (nz + d[2] * 0.8) * sp, 0.15 + R() * 0.35, 0.25 + R() * 0.25, 0.1, r * 3, g * 3, b * 3, 1, r, g * 0.4, b * 0.2, 0, 3, 0.05, 1.5); }
    add.add(x, y, z, 0, 0, 0, 0.12, 2.5, 4, r * 4, g * 4, b * 4, 1, r, g, b, 0, 0);
  }
  function muzzle(x, y, z, vx, vy, vz, r, g, b, s = 1.6) { add.add(x, y, z, vx, vy, vz, 0.06, s, s * 1.6, r * 5, g * 5, b * 5, 1, r, g, b, 0, 0); }
  function warp(x, y, z, dx, dy, dz, big) {
    const L = big ? 1400 : 700;
    add.add(x - dx * L * 0.5, y - dy * L * 0.5, z - dz * L * 0.5, dx * L * 3, dy * L * 3, dz * L * 3, 0.32, big ? 14 : 5, big ? 4 : 2, 2.5, 3.5, 5, 1, 1, 1.5, 3, 0, 3, 0.16, 0);
    add.add(x, y, z, 0, 0, 0, 0.5, big ? 60 : 14, big ? 160 : 40, 2.5, 3.2, 5, 1, 0.4, 0.6, 1.4, 0, 0);
    shock(x, y, z, big ? 260 : 45, 0.5, 0.8, 1.6, 0.6);
  }

  function update(dt, cam) {
    T.value += dt;
    add.update(dt); smoke.update(dt); debrisUpdate(dt);
    for (const m of shocks) if (m.visible) { const u = m.userData; u.t += dt; const k = u.t / u.dur; if (k >= 1) { m.visible = false; continue; } m.scale.setScalar(u.r * (0.15 + 0.85 * (1 - (1 - k) * (1 - k)))); m.material.uniforms.uAge.value = k; }
    for (const m of shields) if (m.visible) { const u = m.userData; u.t += dt; const k = u.t / u.dur; m.material.uniforms.uTime.value = T.value; if (k >= 1) { m.visible = false; m.removeFromParent(); u.owner = null; continue; } m.material.uniforms.uAge.value = k; }
    for (const l of lights) { const u = l.userData; if (u.t < u.dur) { u.t += dt; const k = 1 - u.t / u.dur; l.intensity = u.k * k * k; } else l.intensity = 0; }
  }
  // a spinning world carries its air: move what lives inside radius sqrt(r2) of (cx, cy, cz) with the frame's rotation q
  // over the last frame (p' = c1 + q (p - c0)), so dust, smoke, debris and trails stay put on the ground
  function rotP(arr, i, q, c0, c1) {
    const x = arr[i] - c0[0], y = arr[i + 1] - c0[1], z = arr[i + 2] - c0[2];
    const ix = q[3] * x + q[1] * z - q[2] * y, iy = q[3] * y + q[2] * x - q[0] * z, iz = q[3] * z + q[0] * y - q[1] * x, iw = -q[0] * x - q[1] * y - q[2] * z;
    arr[i] = ix * q[3] + iw * -q[0] + iy * -q[2] - iz * -q[1] + c1[0];
    arr[i + 1] = iy * q[3] + iw * -q[1] + iz * -q[0] - ix * -q[2] + c1[1];
    arr[i + 2] = iz * q[3] + iw * -q[2] + ix * -q[1] - iy * -q[0] + c1[2];
  }
  const Z3 = [0, 0, 0];
  function frameDrag(c0, c1, q, r2) {
    const inR = (arr, i) => { const x = arr[i] - c0[0], y = arr[i + 1] - c0[1], z = arr[i + 2] - c0[2]; return x * x + y * y + z * z < r2; };
    for (const P of [add, smoke]) for (let i = 0; i < P.N; i++) if (P.alive[i] && inR(P.p, i * 3)) { rotP(P.p, i * 3, q, c0, c1); rotP(P.v, i * 3, q, Z3, Z3); }
    for (let i = 0; i < ND; i++) if (D.alive[i] && inR(D.p, i * 3)) { rotP(D.p, i * 3, q, c0, c1); rotP(D.v, i * 3, q, Z3, Z3); }
    for (const t of trails) if (t.used && t.n && inR(t.pts, 0)) for (let k = 0; k < t.n; k++) rotP(t.pts, k * 3, q, c0, c1);
  }
  function clear() { add.clear(); smoke.clear(); D.alive.fill(0); deb.count = 0; for (const m of shocks) m.visible = false; for (const m of shields) { m.visible = false; m.removeFromParent(); } trailsClear(); for (const l of lights) l.intensity = 0; }
  function dispose() {
    add.dispose(); smoke.dispose(); bg.dispose(); bMat.dispose(); deb.dispose(); dMat.dispose(); shellGeo.dispose();
    shocks.forEach((m) => m.material.dispose()); shields.forEach((m) => m.material.dispose()); noz.dispose(); rib.dispose(); nozMesh.material.dispose(); ribMesh.material.dispose();
    tg.dispose(); trailMesh.material.dispose(); beamGeo.dispose(); beams.forEach((m) => m.material.dispose()); atlas.dispose();
  }
  return { add, smoke, explosion, hitSparks, muzzle, warp, shock, shieldHit, flash, debris, plume, plumesBegin, plumesEnd, trailFor, trailPush, trailsBuild, trailsClear,
    boltsBegin, bolt, boltsEnd, beamsBegin, beam, update, clear, dispose, frameDrag, T };
}

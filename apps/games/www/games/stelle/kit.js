// stelle/kit.js — the procedural model kit: every ship, station, beacon and rock is assembled in
// code from a few primitives (tapered boxes, cylinders, ellipsoids, extruded plates, tori) into ONE
// merged BufferGeometry per model, with per-vertex faction colour and a glow channel (aGlow) that the
// hull material turns into emissive light (windows, running lights, engine nozzles) for the bloom.
// Silhouettes are distinct per class on purpose: needle (Lancer), H (Bastion), lopsided wing
// (Scrapwing), fork (Harpoon), drill (Gutter), halo ring (Votive), box train (Hauler), collared
// corvette (Warden), welded freighter (Hulk). Gun/engine/turret points come from sim.js CLS, so
// muzzle flashes and plumes sit exactly where the simulation fires from.
import { CLS, F_GILDA, F_CUSTODI, F_RELITTI, F_ECO, F_PLAYER } from './sim.js';
import { rng } from './world.js';

const lin = (c) => [Math.pow(c[0], 2.2), Math.pow(c[1], 2.2), Math.pow(c[2], 2.2)];
export const PALETTES = {
  [F_PLAYER]: { hull: [0.68, 0.65, 0.45], hull2: [0.38, 0.39, 0.31], accent: [0.97, 0.64, 0.13], dark: [0.10, 0.10, 0.09], glass: [0.14, 0.2, 0.18], glow: [1.0, 0.86, 0.5], engine: [0.86, 1.0, 0.5], light: [1.0, 0.8, 0.42], shield: [0.55, 1.0, 0.75] },
  [F_GILDA]: { hull: [0.88, 0.85, 0.76], hull2: [0.74, 0.57, 0.29], accent: [0.17, 0.33, 0.80], dark: [0.10, 0.11, 0.15], glass: [0.12, 0.20, 0.38], glow: [0.62, 0.80, 1.0], engine: [0.50, 0.74, 1.0], light: [0.66, 0.84, 1.0], shield: [0.45, 0.7, 1.0] },
  [F_CUSTODI]: { hull: [0.86, 0.85, 0.81], hull2: [0.31, 0.57, 0.50], accent: [0.88, 0.68, 0.27], dark: [0.12, 0.14, 0.13], glass: [0.28, 0.24, 0.12], glow: [1.0, 0.82, 0.48], engine: [1.0, 0.80, 0.42], light: [1.0, 0.86, 0.55], shield: [1.0, 0.85, 0.45] },
  [F_RELITTI]: { hull: [0.50, 0.28, 0.16], hull2: [0.17, 0.15, 0.14], accent: [1.0, 0.52, 0.10], dark: [0.07, 0.06, 0.06], glass: [0.30, 0.16, 0.06], glow: [1.0, 0.58, 0.18], engine: [1.0, 0.48, 0.14], light: [1.0, 0.62, 0.22], shield: [1.0, 0.6, 0.25],
    patch: [[0.52, 0.50, 0.47], [0.36, 0.31, 0.26], [0.58, 0.34, 0.19], [0.28, 0.30, 0.27]] },
  [F_ECO]: { hull: [0.06, 0.07, 0.10], hull2: [0.13, 0.15, 0.22], accent: [0.42, 0.92, 1.0], dark: [0.03, 0.03, 0.05], glass: [0.3, 0.6, 0.9], glow: [0.52, 0.92, 1.0], engine: [0.72, 0.52, 1.0], light: [0.6, 0.9, 1.0], shield: [0.6, 0.85, 1.0] },
};
// glow strength per role (fed to the emissive channel; >1 blooms)
const GLOW = { glow: 2.4, engine: 1.3, light: 3.0, window: 1.7, glass: 0.32, core: 4.5, panel: 0.6 };

// ---- builder ----------------------------------------------------------------------------------
class Builder {
  constructor(pal, seed = 1) {
    this.P = []; this.N = []; this.C = []; this.G = [];
    this.pal = pal; this.r = rng(seed);
    this.m = null;   // current transform: { o:[x,y,z], R: 3x3 }
  }
  col(role) {
    let c;
    if (role === 'patch' && this.pal.patch) c = this.pal.patch[this.r.int(this.pal.patch.length)];
    else if (role === 'window' || role === 'core') c = this.pal.light;
    else if (role === 'panel') c = this.pal.glass;
    else c = this.pal[role] || this.pal.hull;
    const j = role === 'hull' || role === 'hull2' || role === 'patch' ? 0.94 + this.r() * 0.1 : 1;
    const l = lin(c);
    return [l[0] * j, l[1] * j, l[2] * j];
  }
  // transform helpers: euler XYZ rotation + offset applied to every primitive vertex until reset()
  at(o = [0, 0, 0], rot = [0, 0, 0]) {
    const [a, b, c] = rot, ca = Math.cos(a), sa = Math.sin(a), cb = Math.cos(b), sb = Math.sin(b), cc = Math.cos(c), sc = Math.sin(c);
    // R = Rz * Ry * Rx
    this.m = { o, R: [cb * cc, sa * sb * cc - ca * sc, ca * sb * cc + sa * sc, cb * sc, sa * sb * sc + ca * cc, ca * sb * sc - sa * cc, -sb, sa * cb, ca * cb] };
    return this;
  }
  reset() { this.m = null; return this; }
  xp(v) { if (!this.m) return v; const R = this.m.R, o = this.m.o; return [R[0] * v[0] + R[1] * v[1] + R[2] * v[2] + o[0], R[3] * v[0] + R[4] * v[1] + R[5] * v[2] + o[1], R[6] * v[0] + R[7] * v[1] + R[8] * v[2] + o[2]]; }
  xn(v) { if (!this.m) return v; const R = this.m.R; return [R[0] * v[0] + R[1] * v[1] + R[2] * v[2], R[3] * v[0] + R[4] * v[1] + R[5] * v[2], R[6] * v[0] + R[7] * v[1] + R[8] * v[2]]; }
  vtx(p, n, c, g) { const q = this.xp(p), m = this.xn(n); this.P.push(q[0], q[1], q[2]); this.N.push(m[0], m[1], m[2]); this.C.push(c[0], c[1], c[2]); this.G.push(g); }
  // flat polygon (convex quad/tri) oriented outward from `ctr`
  face(pts, ctr, role) {
    const c = this.col(role), g = GLOW[role] || 0;
    const a = pts[0], b = pts[1], d = pts[2];
    let nx = (b[1] - a[1]) * (d[2] - a[2]) - (b[2] - a[2]) * (d[1] - a[1]), ny = (b[2] - a[2]) * (d[0] - a[0]) - (b[0] - a[0]) * (d[2] - a[2]), nz = (b[0] - a[0]) * (d[1] - a[1]) - (b[1] - a[1]) * (d[0] - a[0]);
    const l = Math.hypot(nx, ny, nz) || 1; nx /= l; ny /= l; nz /= l;
    let mx = 0, my = 0, mz = 0; for (const p of pts) { mx += p[0]; my += p[1]; mz += p[2]; } mx /= pts.length; my /= pts.length; mz /= pts.length;
    const out = (mx - ctr[0]) * nx + (my - ctr[1]) * ny + (mz - ctr[2]) * nz >= 0;
    const P = out ? pts : pts.slice().reverse(), n = out ? [nx, ny, nz] : [-nx, -ny, -nz];
    for (let i = 1; i < P.length - 1; i++) { this.vtx(P[0], n, c, g); this.vtx(P[i], n, c, g); this.vtx(P[i + 1], n, c, g); }
  }
  // tapered box: back face (+Z) sx×sy, front face (-Z) scaled by (fx, fy) and shifted by fyo
  box(cx, cy, cz, sx, sy, sz, role, o = {}) {
    const fx = o.fx != null ? o.fx : 1, fy = o.fy != null ? o.fy : 1, fyo = o.fyo || 0, bx = o.bx != null ? o.bx : 1, by = o.by != null ? o.by : 1;
    const hx = sx / 2, hy = sy / 2, hz = sz / 2;
    const F = [[cx - hx * fx, cy - hy * fy + fyo, cz - hz], [cx + hx * fx, cy - hy * fy + fyo, cz - hz], [cx + hx * fx, cy + hy * fy + fyo, cz - hz], [cx - hx * fx, cy + hy * fy + fyo, cz - hz]];
    const K = [[cx - hx * bx, cy - hy * by, cz + hz], [cx + hx * bx, cy - hy * by, cz + hz], [cx + hx * bx, cy + hy * by, cz + hz], [cx - hx * bx, cy + hy * by, cz + hz]];
    const ctr = [cx, cy + fyo * 0.5, cz];
    const rs = o.roles || {};
    this.face(F, ctr, rs.front || role); this.face(K, ctr, rs.back || role);
    this.face([F[0], F[1], K[1], K[0]], ctr, rs.bottom || role); this.face([F[3], F[2], K[2], K[3]], ctr, rs.top || role);
    this.face([F[0], F[3], K[3], K[0]], ctr, rs.side || role); this.face([F[1], F[2], K[2], K[1]], ctr, rs.side || role);
    return this;
  }
  // truncated cone between two points (smooth sides)
  cyl(a, b, ra, rb, seg, role, caps = true, capRole) {
    const c = this.col(role), g = GLOW[role] || 0;
    const dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2], L = Math.hypot(dx, dy, dz) || 1, ax = [dx / L, dy / L, dz / L];
    let u = Math.abs(ax[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
    let v = [ax[1] * u[2] - ax[2] * u[1], ax[2] * u[0] - ax[0] * u[2], ax[0] * u[1] - ax[1] * u[0]]; let vl = Math.hypot(...v); v = v.map((x) => x / vl);
    u = [v[1] * ax[2] - v[2] * ax[1], v[2] * ax[0] - v[0] * ax[2], v[0] * ax[1] - v[1] * ax[0]];
    const slope = (ra - rb) / L;
    const ring = (i, r, p) => { const t = i / seg * Math.PI * 2, cs = Math.cos(t), sn = Math.sin(t); return [p[0] + (u[0] * cs + v[0] * sn) * r, p[1] + (u[1] * cs + v[1] * sn) * r, p[2] + (u[2] * cs + v[2] * sn) * r]; };
    const nrm = (i) => { const t = i / seg * Math.PI * 2, cs = Math.cos(t), sn = Math.sin(t); const n = [u[0] * cs + v[0] * sn + ax[0] * slope, u[1] * cs + v[1] * sn + ax[1] * slope, u[2] * cs + v[2] * sn + ax[2] * slope]; const l = Math.hypot(...n); return n.map((x) => x / l); };
    for (let i = 0; i < seg; i++) {
      const p0 = ring(i, ra, a), p1 = ring(i + 1, ra, a), p2 = ring(i + 1, rb, b), p3 = ring(i, rb, b), n0 = nrm(i), n1 = nrm(i + 1);
      this.vtx(p0, n0, c, g); this.vtx(p1, n1, c, g); this.vtx(p2, n1, c, g);
      this.vtx(p0, n0, c, g); this.vtx(p2, n1, c, g); this.vtx(p3, n0, c, g);
    }
    if (caps) {
      const cc = this.col(capRole || role), cg = GLOW[capRole || role] || 0;
      if (ra > 0.001) { const n = [-ax[0], -ax[1], -ax[2]]; for (let i = 0; i < seg; i++) { this.vtx(a, n, cc, cg); this.vtx(ring(i + 1, ra, a), n, cc, cg); this.vtx(ring(i, ra, a), n, cc, cg); } }
      if (rb > 0.001) { for (let i = 0; i < seg; i++) { this.vtx(b, ax, cc, cg); this.vtx(ring(i, rb, b), ax, cc, cg); this.vtx(ring(i + 1, rb, b), ax, cc, cg); } }
    }
    return this;
  }
  // ellipsoid (optionally only the upper half: half=1)
  ell(cx, cy, cz, rx, ry, rz, ws, hs, role, half = 0) {
    const c = this.col(role), g = GLOW[role] || 0;
    const v0 = half ? 0 : 0, h0 = half ? hs / 2 : 0;
    const P = (i, j) => { const th = j / hs * Math.PI, ph = i / ws * Math.PI * 2; const st = Math.sin(th), ct = Math.cos(th); return [[cx + rx * st * Math.cos(ph), cy + ry * ct, cz + rz * st * Math.sin(ph)], [st * Math.cos(ph) / rx, ct / ry, st * Math.sin(ph) / rz]]; };
    for (let j = v0; j < (half ? h0 : hs); j++) for (let i = 0; i < ws; i++) {
      const A = P(i, j), B = P(i + 1, j), C = P(i + 1, j + 1), D = P(i, j + 1);
      const nn = (q) => { const l = Math.hypot(...q[1]); return q[1].map((x) => x / l); };
      this.vtx(A[0], nn(A), c, g); this.vtx(B[0], nn(B), c, g); this.vtx(C[0], nn(C), c, g);
      this.vtx(A[0], nn(A), c, g); this.vtx(C[0], nn(C), c, g); this.vtx(D[0], nn(D), c, g);
    }
    return this;
  }
  // flat convex plate from [x,z] points at height y with thickness t (wings, fins with rot)
  plate(pts, y, t, role, edgeRole) {
    const top = pts.map((p) => [p[0], y + t / 2, p[1]]), bot = pts.map((p) => [p[0], y - t / 2, p[1]]);
    let cx = 0, cz = 0; for (const p of pts) { cx += p[0]; cz += p[1]; } cx /= pts.length; cz /= pts.length;
    const ctr = [cx, y, cz];
    this.face(top, ctr, role); this.face(bot, ctr, role);
    for (let i = 0; i < pts.length; i++) { const j = (i + 1) % pts.length; this.face([top[i], top[j], bot[j], bot[i]], ctr, edgeRole || role); }
    return this;
  }
  torus(cx, cy, cz, R, r, seg, tseg, role, axis = 'z', arc = Math.PI * 2) {
    const c = this.col(role), g = GLOW[role] || 0;
    const pt = (i, j) => {
      const a = i / seg * arc, b = j / tseg * Math.PI * 2, x = (R + r * Math.cos(b)) * Math.cos(a), y = (R + r * Math.cos(b)) * Math.sin(a), z = r * Math.sin(b);
      const nx = Math.cos(b) * Math.cos(a), ny = Math.cos(b) * Math.sin(a), nz = Math.sin(b);
      if (axis === 'z') return [[cx + x, cy + y, cz + z], [nx, ny, nz]];
      if (axis === 'y') return [[cx + x, cy + z, cz + y], [nx, nz, ny]];
      return [[cx + z, cy + y, cz + x], [nz, ny, nx]];
    };
    for (let i = 0; i < seg; i++) for (let j = 0; j < tseg; j++) {
      const A = pt(i, j), B = pt(i + 1, j), C = pt(i + 1, j + 1), D = pt(i, j + 1);
      if (axis === 'z') { this.vtx(A[0], A[1], c, g); this.vtx(B[0], B[1], c, g); this.vtx(C[0], C[1], c, g); this.vtx(A[0], A[1], c, g); this.vtx(C[0], C[1], c, g); this.vtx(D[0], D[1], c, g); }
      else { this.vtx(A[0], A[1], c, g); this.vtx(C[0], C[1], c, g); this.vtx(B[0], B[1], c, g); this.vtx(A[0], A[1], c, g); this.vtx(D[0], D[1], c, g); this.vtx(C[0], C[1], c, g); }
    }
    return this;
  }
  // glowing disc facing +Z (engine nozzle) or along an axis
  disc(cx, cy, cz, r, role, seg = 12, nz = 1) {
    const c = this.col(role), g = GLOW[role] || 0, n = [0, 0, nz];
    for (let i = 0; i < seg; i++) {
      const a0 = i / seg * Math.PI * 2, a1 = (i + 1) / seg * Math.PI * 2;
      const p0 = [cx, cy, cz], p1 = [cx + Math.cos(a0) * r, cy + Math.sin(a0) * r, cz], p2 = [cx + Math.cos(a1) * r, cy + Math.sin(a1) * r, cz];
      if (nz > 0) { this.vtx(p0, n, c, g); this.vtx(p1, n, c, g); this.vtx(p2, n, c, g); } else { this.vtx(p0, n, c, g); this.vtx(p2, n, c, g); this.vtx(p1, n, c, g); }
    }
    return this;
  }
  // engine: housing ring + recessed glowing nozzle facing +Z
  engine(x, y, z, r, housing = 'hull2') {
    this.cyl([x, y, z - r * 1.4], [x, y, z], r * 1.08, r * 1.15, 12, housing, false);
    this.cyl([x, y, z - 0.02], [x, y, z - r * 0.5], r * 0.98, r * 0.8, 12, 'dark', false);
    this.disc(x, y, z - r * 0.45, r * 0.82, 'engine', 12);
    return this;
  }
  gun(g, len = 2.6, r = 0.13) {
    this.cyl([g[0], g[1], g[2] + len], [g[0], g[1], g[2]], r * 1.5, r, 6, 'dark', true);
    this.cyl([g[0], g[1], g[2] + 0.15], [g[0], g[1], g[2] - 0.05], r * 0.9, r * 0.9, 6, 'glow', true);
    return this;
  }
  greebles(n, x0, x1, y, z0, z1, s = 0.4, role = 'hull2') {
    for (let i = 0; i < n; i++) { const w = s * (0.5 + this.r()), h = s * (0.3 + this.r() * 0.6), l = s * (0.6 + this.r() * 1.4); this.box(this.r.range(x0, x1), y + h / 2, this.r.range(z0, z1), w, h, l, this.r() < 0.15 ? 'accent' : role); }
    return this;
  }
  lights(list, size = 0.18, role = 'light') { for (const p of list) this.box(p[0], p[1], p[2], size, size, size, role); return this; }
  build(THREE) {
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(this.P, 3));
    g.setAttribute('normal', new THREE.Float32BufferAttribute(this.N, 3));
    g.setAttribute('color', new THREE.Float32BufferAttribute(this.C, 3));
    g.setAttribute('aGlow', new THREE.Float32BufferAttribute(this.G, 1));
    g.computeBoundingSphere();
    return g;
  }
}
const mir = (pts) => pts.map((p) => [-p[0], p[1]]);

// ---- ship designs ---------------------------------------------------------------------------------
const DESIGN = {
  lucciola(b) {   // the old courier (art: ship_courier): faceted olive wedge, bridge block, two big engine pods on pylons
    b.box(0, 0, -0.4, 5.0, 2.6, 11.6, 'hull', { fx: 0.6, fy: 0.5, fyo: -0.4 });
    b.box(0, -0.7, -6.6, 2.7, 1.0, 1.4, 'hull2', { fx: 0.75, fy: 0.7 });
    b.box(0, 1.55, -0.6, 3.4, 0.9, 7.2, 'hull', { fx: 0.66, fy: 0.75 });
    b.box(0, 2.25, -2.2, 2.2, 0.75, 2.6, 'hull2', { fx: 0.8, fy: 0.8 });
    b.box(0, 2.32, -3.53, 1.7, 0.2, 0.05, 'window'); for (const sx of [-1, 1]) b.box(sx * 1.12, 2.3, -2.2, 0.05, 0.18, 1.8, 'window');
    for (const sx of [-1, 1]) b.at([sx * 0.75, 1.05, -4.55], [0.62, 0, 0]).box(0, 0, 0, 1.2, 0.08, 1.3, 'glass').reset();
    b.box(0, 0.1, 5.6, 4.1, 2.1, 0.9, 'hull2');
    for (const sx of [-1, 1]) {
      b.box(sx * 2.53, -0.35, -0.6, 0.06, 0.34, 9.5, 'accent');
      b.box(sx * 1.75, 1.95, -0.6, 0.05, 0.18, 6.2, 'accent');
      b.box(sx * 3.15, 0.75, 0.4, 2.3, 0.55, 2.8, 'hull2');
      b.cyl([sx * 4.35, 0.9, -2.9], [sx * 4.35, 0.9, 3.7], 1.36, 1.26, 16, 'hull', false);
      b.cyl([sx * 4.35, 0.9, -3.15], [sx * 4.35, 0.9, -2.75], 1.44, 1.44, 16, 'accent', false);
      b.disc(sx * 4.35, 0.9, -3.0, 1.2, 'dark', 16, -1);
      b.cyl([sx * 4.35, 0.9, 0.2], [sx * 4.35, 0.9, 0.7], 1.4, 1.4, 16, 'hull2', false);
      b.engine(sx * 4.35, 0.9, 4.6, 1.08, 'hull2');
      b.lights([[sx * 4.35, 2.3, 0.4], [sx * 2.55, 0.2, 5.0]], 0.2, 'light');
    }
    b.box(0.7, 3.0, -1.6, 0.08, 1.0, 0.08, 'dark'); b.box(-0.5, 2.85, -1.2, 0.06, 0.7, 0.06, 'dark');
    for (const g of CLS.lucciola.guns) b.gun([g[0], g[1], g[2]], 2.6, 0.16);
    b.greebles(9, -1.3, 1.3, 2.0, 0.6, 3.6, 0.38);
    b.greebles(6, -1.8, 1.8, -1.32, -3, 3, 0.4, 'dark');
  },
  lancer(b) {   // Gilda interceptor: brass-ringed needle, forward-swept blades
    b.cyl([0, 0, -8.6], [0, 0, -2.5], 0.08, 0.82, 8, 'hull');
    b.cyl([0, 0, -2.5], [0, 0, 5.2], 0.82, 0.95, 8, 'hull');
    b.cyl([0, 0.05, 5.2], [0, 0.05, 7.6], 1.15, 1.1, 10, 'hull2', false);
    b.engine(0, 0.05, 7.9, 0.95, 'hull2');
    for (const z of [-4.2, 0.6, 3.6]) b.cyl([0, 0, z - 0.18], [0, 0, z + 0.18], 0.9 - (z < 0 ? 0.25 : 0), 0.92 - (z < 0 ? 0.25 : 0), 8, 'hull2', false);
    b.ell(0, 0.5, -2.6, 0.48, 0.42, 1.55, 10, 8, 'glass', 1);
    const W = [[0.7, 2.2], [4.9, -1.6], [5.3, -0.7], [0.8, 4.6]];
    b.plate(W, -0.1, 0.13, 'hull', 'hull2'); b.plate(mir(W), -0.1, 0.13, 'hull', 'hull2');
    for (const sx of [-1, 1]) { b.box(sx * 3.2, -0.08, 0.9, 1.2, 0.05, 0.6, 'accent', {}); b.lights([[sx * 5.2, -0.1, -1.1]], 0.2, 'light'); }
    b.plate([[0.6, -4.6], [1.7, -3.8], [1.6, -3.4], [0.6, -3.5]], 0, 0.08, 'hull2'); b.plate(mir([[0.6, -4.6], [1.7, -3.8], [1.6, -3.4], [0.6, -3.5]]), 0, 0.08, 'hull2');
    b.box(0, 1.0, 4.6, 0.12, 1.5, 2.6, 'accent', { fy: 0.3, fyo: -0.5 });
    for (const g of CLS.lancer.guns) b.gun([g[0], g[1], g[2]], 4.2, 0.12);
    b.lights([[0, -0.9, -1], [0, -0.9, 2.5], [0, 0.95, 1.5]], 0.17, 'light');
  },
  bastion(b) {   // Gilda heavy fighter: armoured core, twin pods, dorsal turret
    b.box(0, 0, -0.8, 3.6, 2.4, 10, 'hull', { fx: 0.62, fy: 0.55, fyo: -0.2 });
    b.box(0, 0.75, -5.2, 1.6, 0.35, 1.2, 'glass', { fx: 0.8 });
    for (const sx of [-1, 1]) {
      b.box(sx * 4.7, 0, -0.2, 1.9, 1.9, 11.2, 'hull2', { fx: 0.7, fy: 0.7 });
      b.box(sx * 2.9, 0, 0.9, 2.2, 0.7, 3.2, 'hull');
      b.engine(sx * 4.7, 0, 6.6, 0.8, 'hull2');
      b.box(sx * 4.7, 1.0, -1.5, 1.4, 0.14, 5, 'accent');
      b.box(sx * 2.2, -1.45, -1.0, 0.6, 0.5, 3.4, 'dark');
      b.lights([[sx * 5.7, 0, -4], [sx * 5.7, 0, 3]], 0.2, 'light');
    }
    b.ell(0, 1.25, 0.6, 1.2, 0.7, 1.4, 10, 8, 'hull2', 1);
    b.cyl([-0.35, 1.55, -0.2], [-0.35, 1.55, -2.8], 0.12, 0.1, 6, 'dark'); b.cyl([0.35, 1.55, -0.2], [0.35, 1.55, -2.8], 0.12, 0.1, 6, 'dark');
    for (const g of CLS.bastion.guns) b.gun([g[0], g[1], g[2]], 4.4, 0.2);
    b.greebles(8, -1.3, 1.3, 1.15, 1.5, 4, 0.45);
  },
  scrapwing(b) {   // Relitti swarm fighter: a pod, one big patched wing, one strut, one roaring engine
    b.ell(0, 0, -1.4, 1.05, 0.95, 2.6, 10, 8, 'hull2');
    b.box(0, 0.45, -3.2, 0.9, 0.3, 0.9, 'glass', { fx: 0.7 });
    b.plate([[0.8, -1.4], [5.6, 0.6], [5.4, 2.6], [0.8, 2.2]], 0, 0.22, 'patch', 'hull');
    b.plate([[2.0, -0.4], [3.6, 0.2], [3.5, 1.4], [2.0, 1.4]], 0.16, 0.08, 'patch');
    b.plate(mir([[0.7, 0.2], [3.0, 1.2], [3.0, 1.8], [0.7, 1.6]]), -0.1, 0.12, 'hull');
    b.cyl([-3.0, -0.1, 1.5], [-3.0, -0.1, -1.0], 0.18, 0.18, 6, 'dark');
    b.cyl([-0.45, 0.1, 1.0], [-0.45, 0.1, 4.2], 0.62, 0.72, 10, 'hull', false);
    b.engine(-0.45, 0.1, 5.0, 0.6, 'hull2');
    b.box(0.6, -0.8, 0.5, 0.5, 0.4, 2, 'patch');
    for (const g of CLS.scrapwing.guns) b.gun([g[0], g[1], g[2]], 3.2, 0.15);
    b.lights([[5.5, 0, 1.6], [-3.0, -0.1, -1.1], [0, 1.0, 0.5]], 0.2, 'light');
    b.box(4.8, 0.18, 1.8, 1.0, 0.06, 0.3, 'accent');
  },
  harpoon(b) {   // Relitti tether ship: forked bow with a glowing emitter
    b.box(0, 0, 1.0, 2.4, 1.9, 8.2, 'hull', { fx: 0.85, fy: 0.8 });
    for (const sx of [-1, 1]) {
      b.box(sx * 1.05, 0, -5.6, 0.55, 0.65, 6.2, 'hull2', { fx: 0.6, fy: 0.7 });
      b.box(sx * 0.82, 0, -8.7, 0.3, 0.45, 0.8, 'accent', { fx: 0.3 });
      b.cyl([sx * 1.9, 0, 3.0], [sx * 1.9, 0, 5.8], 0.6, 0.66, 10, 'hull2', false);
      b.engine(sx * 1.9, 0, 6.5, 0.6, 'hull');
    }
    b.ell(0, 0, -8.2, 0.42, 0.42, 0.42, 8, 6, 'core');
    b.cyl([0, 0, -7.6], [0, 0, -3.0], 0.18, 0.3, 6, 'dark');
    b.box(0, 0.95, -0.6, 1.2, 0.4, 2.0, 'glass', { fx: 0.7 });
    b.box(0, 1.1, 2.6, 0.6, 0.5, 3, 'patch');
    for (const g of CLS.harpoon.guns) b.gun([g[0], g[1], g[2]], 3.2, 0.15);
    b.lights([[0, -1.0, 3], [1.2, 0.95, 4], [-1.2, 0.95, 4]], 0.2, 'light');
  },
  gutter(b) {   // Relitti boarding craft: stubby can with a drill nose and grapple claws
    b.cyl([0, 0, -3.2], [0, 0, 4.8], 1.6, 1.7, 10, 'hull', true, 'hull2');
    b.cyl([0, 0, -3.2], [0, 0, -7.4], 1.45, 0.15, 10, 'dark', false);
    for (let i = 0; i < 3; i++) { const a = i / 3 * Math.PI * 2 + 0.5; b.at([Math.cos(a) * 1.45, Math.sin(a) * 1.45, -4.2], [0, 0, a]).box(0, 0, 0, 0.3, 0.35, 3.4, 'accent', { fx: 0.4 }).reset(); }
    for (const sx of [-1, 1]) b.engine(sx * 1.6, 0, 5.9, 0.55, 'hull2');
    b.box(0, 1.55, 0.2, 1.0, 0.4, 2.4, 'glass');
    b.box(0, 0, 1, 3.8, 0.3, 1.2, 'patch');
    b.lights([[1.7, 0, 3], [-1.7, 0, 3], [0, -1.7, 2]], 0.22, 'light');
  },
  votive(b) {   // Custodi light fighter: white-stone spindle in a halo wing
    b.cyl([0, 0, -6.4], [0, 0, -1.8], 0.12, 0.85, 10, 'hull');
    b.cyl([0, 0, -1.8], [0, 0, 5.2], 0.85, 0.7, 10, 'hull');
    b.engine(0, 0, 6.1, 0.75, 'hull2');
    b.ell(0, 0.55, -1.6, 0.45, 0.4, 1.3, 10, 8, 'glass', 1);
    b.torus(0, 0, 1.2, 4.3, 0.26, 32, 6, 'hull2', 'z');
    for (let i = 0; i < 4; i++) { const a = i / 4 * Math.PI * 2 + Math.PI / 4; b.at([0, 0, 1.2], [0, 0, a]).box(2.5, 0, 0, 3.7, 0.16, 0.7, 'hull').reset(); }
    for (let i = 0; i < 8; i++) { const a = i / 8 * Math.PI * 2; b.lights([[Math.cos(a) * 4.3, Math.sin(a) * 4.3, 1.55]], 0.22, 'glow'); }
    b.cyl([0, 0, -0.4], [0, 0, 0.0], 0.95, 0.95, 10, 'accent', false);
    for (const g of CLS.votive.guns) b.gun([g[0], g[1], g[2]], 3, 0.12);
  },
  shard(b) {   // Echo drone: a dark crystal heart in three lattice blades, cyan-violet light
    const cr = 2.2;
    b.cyl([0, 0, -4.4], [0, 0, -0.6], 0.01, cr, 6, 'hull2', false); b.cyl([0, 0, -0.6], [0, 0, 3.2], cr, 0.01, 6, 'hull2', false);
    b.ell(0, 0, -0.6, 0.9, 0.9, 0.9, 8, 6, 'core');
    for (let i = 0; i < 3; i++) {
      const a = i / 3 * Math.PI * 2;
      b.at([0, 0, 0], [0, 0, a]).plate([[0.4, -3.2], [4.2, -0.4], [4.4, 1.2], [0.6, 2.4]], 0, 0.1, 'hull', 'glow').reset();
      b.at([0, 0, 0], [0, 0, a]).box(4.3, 0, 0.4, 0.14, 0.14, 1.6, 'glow').reset();
    }
    b.disc(0, 0, 3.25, 0.7, 'engine', 10);
  },
  hauler(b) {   // convoy hauler: cab, spine, three cargo blocks, engine block
    b.box(0, 1, -38, 14, 11, 16, 'hull', { fx: 0.7, fy: 0.6, fyo: -1 });
    b.box(0, 3.4, -46.2, 8, 2.2, 0.4, 'window');
    b.box(0, 0, 4, 4.5, 4.5, 72, 'hull2');
    const cc = [[-26, 'accent'], [-6, 'hull'], [14, 'accent'], [32, 'hull2']];
    for (const [z, r] of cc) { b.box(0, 0, z, 15, 13, 16, r); b.box(0, 6.6, z, 13, 0.3, 14, 'dark'); }
    b.box(0, 0, 42, 20, 12, 8, 'hull');
    for (const sx of [-1, 1]) { b.engine(sx * 7, 0, 47, 3.6, 'hull2'); b.lights([[sx * 7.6, 0, -46], [sx * 10.2, 6, 42], [sx * 8, -6.6, -26], [sx * 8, -6.6, 14]], 0.7, 'light'); }
    for (let z = -28; z < 38; z += 6) b.lights([[0, 2.4, z]], 0.4, 'window');
  },
  warden(b) {   // Gilda gunship: tapered ivory hull, brass collar, window strips, bridge tower
    b.box(0, 0, -6, 22, 17, 120, 'hull', { fx: 0.35, fy: 0.42, fyo: -1.5, bx: 0.85, by: 0.9 });
    b.torus(0, 0, -6, 15, 2.6, 28, 6, 'hull2', 'z');
    b.box(0, 9, 26, 12, 4, 40, 'hull2', { fx: 0.6 });
    b.box(0, 14, 48, 10, 7, 12, 'hull', { fx: 0.8 });
    b.box(0, 15.8, 42.1, 8, 1.2, 0.3, 'window');
    b.ell(0, 16, 32, 5.5, 4, 5.5, 12, 8, 'accent', 1);
    for (const sx of [-1, 1]) {
      for (let z = -40; z < 50; z += 5) b.box(sx * 10.6, 2.5, z, 0.3, 1.1, 2.6, 'window');
      b.box(sx * 13, -3, 30, 5, 6, 30, 'hull2');
      b.lights([[sx * 11, 0, -55], [sx * 15.6, -3, 44]], 0.9, 'light');
    }
    b.box(0, 0, 58, 26, 14, 10, 'hull2');
    for (const e of CLS.warden.eng) b.engine(e[0], e[1], e[2], e[3] * 0.85, 'hull');
    b.box(0, -8.8, -10, 6, 1.2, 60, 'accent');
    b.greebles(26, -8, 8, 8.4, -50, 20, 1.8);
  },
  hulk(b) {   // Relitti converted freighter: welded box chain, armour patches, exposed reactor
    const segs = [[-72, 20, 18, 26], [-44, 30, 26, 30], [-12, 34, 30, 34], [18, 30, 28, 26], [76, 32, 26, 26]];
    for (const [z, w, h, l] of segs) b.at([b.r.range(-2, 2), b.r.range(-2, 2), z], [b.r.range(-0.05, 0.05), b.r.range(-0.06, 0.06), b.r.range(-0.08, 0.08)]).box(0, 0, 0, w, h, l, 'hull').reset();
    b.box(0, 0, -86, 14, 12, 8, 'hull2', { fx: 0.6, fy: 0.6 });
    b.box(0, 9, -88, 9, 1.2, 0.4, 'window');
    b.torus(0, 0, 46, 16, 3, 20, 6, 'hull2', 'z');
    b.ell(0, 0, 46, 9, 9, 9, 14, 10, 'core');
    for (let i = 0; i < 14; i++) b.box(b.r.range(-17, 17), b.r.range(-16, 16), b.r.range(-80, 80), b.r.range(6, 14), b.r.range(4, 10), b.r.range(10, 22), 'patch');
    for (let i = 0; i < 4; i++) { const sx = i % 2 ? 1 : -1; b.box(sx * 22, b.r.range(-8, 8), -50 + i * 26, 10, 9, 16, i % 3 ? 'accent' : 'patch'); }
    b.at([12, 22, -30], [0, 0, -0.4]).box(0, 10, 0, 2, 22, 2, 'dark').reset();
    b.at([-10, 20, 20], [0.3, 0, 0.5]).box(0, 9, 0, 2, 18, 2, 'dark').reset();
    for (const e of CLS.hulk.eng) b.engine(e[0], e[1], e[2], e[3], 'hull2');
    b.box(0, 0, 88, 34, 22, 6, 'hull2');
    for (let i = 0; i < 26; i++) b.lights([[b.r.range(-17, 17), b.r.sign() * b.r.range(15, 17), b.r.range(-80, 80)]], 0.9, i % 3 ? 'light' : 'window');
  },
};

const CACHE = new Map();
// the cast aces' liveries (hull, accent, trim) — the same colours as the firmware's ACE_PAL
const c8 = (r, g, b) => [r / 255, g / 255, b / 255];
const ACE_LIVERY = [[c8(240, 244, 255), c8(40, 80, 230), c8(220, 180, 60)], [c8(255, 244, 210), c8(240, 190, 50), c8(60, 170, 140)],
  [c8(220, 30, 30), c8(255, 200, 0), c8(40, 24, 10)], [c8(70, 20, 140), c8(120, 240, 255), c8(255, 255, 255)], [c8(80, 76, 70), c8(160, 84, 40), c8(255, 140, 30)]];
// Merged geometry for a ship class in a faction palette (ace: true = generic crimson-and-gold, 0..4 = a cast livery).
export function shipGeometry(THREE, ck, fac, ace = false) {
  const key = ck + ':' + fac + ':' + (typeof ace === 'number' ? 'c' + ace : ace ? 1 : 0);
  if (CACHE.has(key)) return CACHE.get(key);
  let pal = PALETTES[fac] || PALETTES[F_GILDA];
  if (typeof ace === 'number' && ACE_LIVERY[ace]) { const L = ACE_LIVERY[ace]; pal = { ...pal, hull: L[0], accent: L[1], hull2: L[2], light: L[1] }; }
  else if (ace) pal = { ...pal, accent: [0.85, 0.12, 0.1], hull2: [0.8, 0.62, 0.22], light: [1, 0.35, 0.25] };
  const b = new Builder(pal, (ck.length * 7919 + fac * 31) | 0);
  (DESIGN[ck] || DESIGN.lancer)(b);
  const g = b.build(THREE);
  CACHE.set(key, g);
  return g;
}

// Capital turret: base (static), head (yaw) and barrels (pitch) as three geometries.
export function turretGeometry(THREE, fac) {
  const key = 'turret:' + fac;
  if (CACHE.has(key)) return CACHE.get(key);
  const pal = PALETTES[fac] || PALETTES[F_GILDA];
  const base = new Builder(pal, 3); base.cyl([0, 0, 0], [0, 1.6, 0], 4.2, 3.6, 12, 'hull2'); base.lights([[0, 1.7, 3.4]], 0.5, 'light');
  const head = new Builder(pal, 4); head.box(0, 1.2, 0, 4.4, 2.4, 4.8, 'hull', { fx: 0.8, fy: 0.7 }); head.box(0, 2.45, 0.6, 2.4, 0.2, 2.0, 'accent');
  const gun = new Builder(pal, 5); for (const sx of [-0.8, 0.8]) { gun.cyl([sx, 0, 0], [sx, 0, -7.5], 0.42, 0.3, 8, 'dark'); gun.cyl([sx, 0, -7.4], [sx, 0, -7.6], 0.32, 0.32, 8, 'glow'); }
  const out = { base: base.build(THREE), head: head.build(THREE), gun: gun.build(THREE) };
  CACHE.set(key, out);
  return out;
}

// ---- stations ------------------------------------------------------------------------------------
// Returns { geo, lights: [[x,y,z,r,g,b,phase]...] } from a world.js station blueprint.
export function stationGeometry(THREE, st) {
  const pal = PALETTES[st.faction] || PALETTES[F_GILDA];
  const b = new Builder(pal, st.seed);
  const lights = [];
  const L = (x, y, z) => lights.push([x, y, z, pal.light[0], pal.light[1], pal.light[2], b.r()]);
  const windows = (cx, cy, cz, sx, sy, sz, n) => { for (let i = 0; i < n; i++) { const f = b.r.int(4), u = b.r.range(-0.45, 0.45), v = b.r.range(-0.4, 0.4); const p = f === 0 ? [cx + sx / 2 + 0.3, cy + v * sy, cz + u * sz] : f === 1 ? [cx - sx / 2 - 0.3, cy + v * sy, cz + u * sz] : f === 2 ? [cx + u * sx, cy + sy / 2 + 0.3, cz + v * sz] : [cx + u * sx, cy - sy / 2 - 0.3, cz + v * sz]; b.box(p[0], p[1], p[2], f < 2 ? 0.4 : 3, f < 2 ? 1.6 : 0.4, f < 2 ? 3 : 1.6, 'window'); } };
  for (const m of st.mods) {
    const [x, y, z] = m.p, [sx, sy, sz] = m.size, rot = m.rot || [0, 0, 0];
    switch (m.kind) {
      case 'hub': b.cyl([x, y, z - sz / 2], [x, y, z + sz / 2], sx / 2, sx / 2, 20, 'hull', true, 'hull2');
        for (let k = -2; k <= 2; k++) b.cyl([x, y, z + k * sz * 0.18 - 3], [x, y, z + k * sz * 0.18 + 3], sx / 2 + 4, sx / 2 + 4, 20, 'hull2', false);
        for (let k = 0; k < 40; k++) { const a = b.r() * Math.PI * 2, zz = z + b.r.range(-0.45, 0.45) * sz; b.box(x + Math.cos(a) * (sx / 2 + 0.4), y + Math.sin(a) * (sx / 2 + 0.4), zz, 2.4, 2.4, 3, 'window'); }
        L(x, y + sx / 2 + 6, z - sz / 2); L(x, y - sx / 2 - 6, z + sz / 2); break;
      case 'ring': b.torus(x, y, z, sx, sy / 2, 72, 8, 'hull', 'z');
        b.torus(x, y, z + sz * 0.3, sx, sy * 0.3, 72, 6, 'hull2', 'z');
        for (let k = 0; k < 72; k++) { const a = k / 72 * Math.PI * 2; b.box(x + Math.cos(a) * (sx + sy * 0.52), y + Math.sin(a) * (sx + sy * 0.52), z, 1.2, 1.2, sz * 0.4, k % 3 ? 'window' : 'hull2'); }
        for (let k = 0; k < 8; k++) { const a = k / 8 * Math.PI * 2; L(x + Math.cos(a) * (sx + sy * 0.6), y + Math.sin(a) * (sx + sy * 0.6), z); } break;
      case 'spoke': { const a = m.a || 0; b.cyl([x, y, z], [x + Math.cos(a) * sx, y + Math.sin(a) * sx, z], sy / 2, sy / 2, 8, 'hull2'); break; }
      case 'arm': b.box(x, y, z, sx, sy, sz, 'hull2'); windows(x, y, z, sx, sy, sz, 10); L(x + sx / 2, y, z - sz / 2); L(x - sx / 2, y, z - sz / 2); break;
      case 'dock': b.box(x, y, z, sx, sy, sz, 'hull', { roles: { front: 'dark' } }); b.box(x, y, z - sz / 2 - 0.4, sx * 0.7, sy * 0.55, 0.6, 'glow'); L(x + sx / 2, y + sy / 2, z - sz / 2); L(x - sx / 2, y - sy / 2, z - sz / 2); break;
      case 'panel': b.box(x, y, z, sx, sy, sz, 'panel'); for (let k = 0; k < 6; k++) b.box(x - sx / 2 + (k + 0.5) * sx / 6, y + sy / 2 + 0.1, z, 0.6, 0.2, sz, 'hull2'); break;
      case 'spire': b.at([x, y, z], rot).cyl([0, -sy / 2, 0], [0, sy / 2, 0], sx / 2, sx * 0.08, 12, 'hull', true, 'hull2');
        for (let k = 0; k < 6; k++) b.cyl([0, -sy / 2 + k * sy / 7, 0], [0, -sy / 2 + k * sy / 7 + 4, 0], sx / 2 * (1 - k / 7) + 3, sx / 2 * (1 - k / 7) + 3, 12, 'hull2', false);
        b.ell(0, sy / 2, 0, 3, 3, 3, 8, 6, 'core'); b.reset(); if (!m.rot) L(x, y + sy / 2 + 4, z); break;
      case 'dome': b.ell(x, y, z, sx / 2, sy / 2, sz / 2, 20, 12, 'hull2'); windows(x, y, z, sx * 0.9, sy * 0.4, sz * 0.9, 18); break;
      case 'halo': b.torus(x, y, z, sx / 2, 2.2, 64, 6, 'hull2', 'y'); for (let k = 0; k < 24; k++) { const a = k / 24 * Math.PI * 2; b.box(x + Math.cos(a) * sx / 2, y + 2.5, z + Math.sin(a) * sx / 2, 1.6, 1.6, 1.6, 'glow'); } break;
      case 'chapel': b.box(x, y, z, sx, sy, sz, 'hull'); b.ell(x, y + sy / 2, z, sx / 2, sx / 2.5, sz / 2, 12, 8, 'hull2', 1); windows(x, y, z, sx, sy, sz, 8); L(x, y + sy / 2 + sx / 2.5 + 3, z); break;
      case 'hulk': b.at([x, y, z], rot).box(0, 0, 0, sx, sy, sz, b.r() < 0.5 ? 'hull' : 'patch', { fx: 0.8, fy: 0.85 });
        for (let k = 0; k < 6; k++) b.box(b.r.range(-sx / 2, sx / 2), b.r.sign() * (sy / 2 + 0.6), b.r.range(-sz / 2, sz / 2), b.r.range(6, 18), 1.2, b.r.range(8, 24), 'patch');
        for (let k = 0; k < 14; k++) b.box(b.r.sign() * (sx / 2 + 0.3), b.r.range(-sy / 2, sy / 2) * 0.8, b.r.range(-sz / 2, sz / 2), 0.4, 1.6, 2.4, 'window');
        b.reset(); L(x, y + sy / 2 + 4, z); break;
      case 'can': b.at([x, y, z], rot).cyl([0, 0, -sz / 2], [0, 0, sz / 2], sx / 2, sx / 2, 12, b.r() < 0.5 ? 'hull2' : 'patch').reset(); break;
      case 'crane': b.at([x, y, z], rot).box(0, sy / 2, 0, sx, sy, sz, 'dark').box(0, sy, sz * 2, sx * 0.8, sx * 0.8, sz * 6, 'accent').reset(); L(x, y + sy, z); break;
      case 'tank': b.ell(x, y, z, sx / 2, sy / 2, sz / 2, 14, 10, 'hull2'); b.torus(x, y, z, sx / 2, 1.2, 24, 4, 'accent', 'y'); break;
    }
  }
  return { geo: b.build(THREE), lights };
}

// The Costellatori beacon: a twisting lattice spire with a crystal heart.
export function beaconGeometry(THREE, bc) {
  const pal = { ...PALETTES[F_ECO], hull: [0.16, 0.17, 0.22], hull2: [0.3, 0.32, 0.38], light: bc.lit ? [1, 0.8, 0.4] : [0.45, 0.85, 1] };
  const b = new Builder(pal, 77);
  const H = bc.height, R = bc.radius * 0.55, rings = 9;
  for (let i = 0; i <= rings; i++) {
    const y = i / rings * H - H * 0.1, r = R * (1 - i / rings * 0.55), tw = i * 0.35;
    b.torus(0, y, 0, r, 1.4, 6, 4, 'hull2', 'y');
    if (i < rings) {
      const y2 = (i + 1) / rings * H - H * 0.1, r2 = R * (1 - (i + 1) / rings * 0.55);
      for (let k = 0; k < 6; k++) {
        const a = k / 6 * Math.PI * 2 + tw, a2 = a + 0.6;
        b.cyl([Math.cos(a) * r, y, Math.sin(a) * r], [Math.cos(a2) * r2, y2, Math.sin(a2) * r2], 1.0, 1.0, 5, 'hull', false);
      }
    }
  }
  b.cyl([0, -H * 0.1, 0], [0, H * 0.75, 0], 3.2, 2, 8, 'hull');
  // crystal heart (octahedron as two cones)
  const cy = H * 0.82, cr = 16;
  b.cyl([0, cy - cr * 1.6, 0], [0, cy, 0], 0.01, cr, 4, 'core', false); b.cyl([0, cy, 0], [0, cy + cr * 1.6, 0], cr, 0.01, 4, 'core', false);
  for (let k = 0; k < 6; k++) { const a = k / 6 * Math.PI * 2; b.lights([[Math.cos(a) * R * 0.98, -H * 0.1, Math.sin(a) * R * 0.98]], 2.2, 'glow'); }
  return { geo: b.build(THREE), core: [0, cy, 0] };
}

// ---- rocks ------------------------------------------------------------------------------------------
// Four base asteroids: an icosphere pushed around by layered value noise with a few craters.
function vnoise(x, y, z, s) {
  const h = (i, j, k) => { let n = (i * 374761393 + j * 668265263 + k * 1274126177 + s * 1442695041) | 0; n = Math.imul(n ^ (n >>> 13), 1274126177); return ((n ^ (n >>> 16)) >>> 0) / 4294967295; };
  const ix = Math.floor(x), iy = Math.floor(y), iz = Math.floor(z), fx = x - ix, fy = y - iy, fz = z - iz;
  const u = fx * fx * (3 - 2 * fx), v = fy * fy * (3 - 2 * fy), w = fz * fz * (3 - 2 * fz);
  const l = (a, b, t) => a + (b - a) * t;
  return l(l(l(h(ix, iy, iz), h(ix + 1, iy, iz), u), l(h(ix, iy + 1, iz), h(ix + 1, iy + 1, iz), u), v), l(l(h(ix, iy, iz + 1), h(ix + 1, iy, iz + 1), u), l(h(ix, iy + 1, iz + 1), h(ix + 1, iy + 1, iz + 1), u), v), w);
}
export function rockGeometry(THREE, shape, seed = 1) {
  const key = 'rock:' + shape + ':' + seed;
  if (CACHE.has(key)) return CACHE.get(key);
  const g = new THREE.IcosahedronGeometry(1, 4);
  const pos = g.attributes.position, n = pos.count, col = new Float32Array(n * 3);
  const r = rng(seed * 31 + shape);
  const craters = []; for (let i = 0; i < 5; i++) { const z = r.range(-1, 1), a = r.range(0, 6.28), s = Math.sqrt(1 - z * z); craters.push([s * Math.cos(a), z, s * Math.sin(a), r.range(0.2, 0.45)]); }
  const stretch = [r.range(0.75, 1.3), r.range(0.6, 1.0), r.range(0.8, 1.4)];
  const base = [r.range(0.5, 0.6), r.range(0.45, 0.53), r.range(0.4, 0.47)];
  for (let i = 0; i < n; i++) {
    let x = pos.getX(i), y = pos.getY(i), z = pos.getZ(i);
    let d = 0.72 + 0.32 * vnoise(x * 1.6 + 7, y * 1.6, z * 1.6, seed + shape) + 0.14 * vnoise(x * 4, y * 4, z * 4, seed + 9) + 0.05 * vnoise(x * 9, y * 9, z * 9, seed + 3);
    let cr = 0;
    for (const c of craters) { const dd = Math.hypot(x - c[0], y - c[1], z - c[2]); if (dd < c[3]) { const t = dd / c[3]; d -= (1 - t * t) * 0.16; cr = Math.max(cr, 1 - t); } else if (dd < c[3] * 1.25) d += 0.03; }
    x *= d * stretch[0]; y *= d * stretch[1]; z *= d * stretch[2];
    pos.setXYZ(i, x, y, z);
    const vein = vnoise(x * 3 + 11, y * 3, z * 3, seed + 21), k = 0.75 + 0.35 * vnoise(x * 2.5, y * 2.5, z * 2.5, seed + 5) - cr * 0.25;
    const hot = vein > 0.78 ? (vein - 0.78) * 2.5 : 0;
    col[i * 3] = Math.pow(base[0] * k + hot * 0.3, 2.2); col[i * 3 + 1] = Math.pow(base[1] * k + hot * 0.18, 2.2); col[i * 3 + 2] = Math.pow(base[2] * k + hot * 0.05, 2.2);
  }
  g.setAttribute('color', new THREE.BufferAttribute(col, 3));
  g.setAttribute('aGlow', new THREE.BufferAttribute(new Float32Array(n), 1));
  // the icosphere is unindexed (flat facets): weld equal positions and average their face normals
  const nrm = new Float32Array(n * 3), acc = new Map(), keyOf = (i) => Math.round(pos.getX(i) * 1e4) + ',' + Math.round(pos.getY(i) * 1e4) + ',' + Math.round(pos.getZ(i) * 1e4);
  for (let i = 0; i < n; i += 3) {
    const ax = pos.getX(i), ay = pos.getY(i), az = pos.getZ(i), bx = pos.getX(i + 1) - ax, by = pos.getY(i + 1) - ay, bz = pos.getZ(i + 1) - az, cx = pos.getX(i + 2) - ax, cy = pos.getY(i + 2) - ay, cz = pos.getZ(i + 2) - az;
    const fx = by * cz - bz * cy, fy = bz * cx - bx * cz, fz = bx * cy - by * cx;
    for (let k = 0; k < 3; k++) { const kk = keyOf(i + k); let a = acc.get(kk); if (!a) acc.set(kk, a = [0, 0, 0]); a[0] += fx; a[1] += fy; a[2] += fz; }
  }
  for (let i = 0; i < n; i++) { const a = acc.get(keyOf(i)), l = Math.hypot(a[0], a[1], a[2]) || 1; nrm[i * 3] = a[0] / l; nrm[i * 3 + 1] = a[1] / l; nrm[i * 3 + 2] = a[2] / l; }
  g.setAttribute('normal', new THREE.BufferAttribute(nrm, 3));
  CACHE.set(key, g);
  return g;
}
// Debris shards (a few irregular chunks, instanced by the FX layer).
export function debrisGeometry(THREE) {
  if (CACHE.has('debris')) return CACHE.get('debris');
  const b = new Builder({ hull: [0.6, 0.6, 0.6], hull2: [0.35, 0.35, 0.37], dark: [0.15, 0.15, 0.16] }, 9);
  b.box(0, 0, 0, 1, 0.25, 1.6, 'hull', { fx: 0.4, fy: 0.6 });
  b.box(0.4, 0.2, 0.3, 0.5, 0.5, 0.8, 'hull2', { fx: 0.5 });
  const g = b.build(THREE);
  CACHE.set('debris', g);
  return g;
}

// The player's cockpit (camera-attached in cockpit view): canopy frame, dashboard, side consoles.
export function cockpitGeometry(THREE) {
  if (CACHE.has('cockpit')) return CACHE.get('cockpit');
  const pal = PALETTES[F_PLAYER];
  const b = new Builder({ ...pal, hull: [0.34, 0.33, 0.27], hull2: [0.16, 0.17, 0.15], panel: [0.1, 0.35, 0.3] }, 11);
  // slim canopy frame (camera at the origin looking -Z): two A-pillars, a top bow, a centre spine
  for (const sx of [-1, 1]) {
    b.at([sx * 1.05, 0.05, -1.25], [0, 0, sx * -0.32]).box(0, 0.2, 0, 0.028, 1.9, 0.04, 'hull').reset();
    b.at([sx * 0.62, 0.86, -1.3], [0, 0, sx * -1.15]).box(0, 0, 0, 0.026, 0.9, 0.035, 'hull').reset();
  }
  b.box(0, 1.02, -1.25, 0.8, 0.026, 0.035, 'hull');
  b.box(0, 1.04, -0.5, 0.024, 0.024, 1.5, 'hull2');
  // dashboard with lit screens and status lights
  b.box(0, -0.74, -1.18, 2.4, 0.3, 0.42, 'hull2', { fy: 0.7, fyo: 0.06 });
  b.box(0, -0.58, -1.36, 2.1, 0.02, 0.12, 'dark');
  for (const sx of [-0.62, 0, 0.62]) { b.box(sx, -0.6, -1.12, 0.44, 0.17, 0.015, 'panel'); b.box(sx, -0.52, -1.13, 0.38, 0.008, 0.008, 'window'); }
  b.lights([[-1.0, -0.56, -1.32], [1.0, -0.56, -1.32], [-0.33, -0.66, -1.12], [0.33, -0.66, -1.12]], 0.022, 'light');
  for (const sx of [-1, 1]) b.box(sx * 1.25, -0.55, -0.6, 0.3, 0.32, 1.1, 'hull2');
  const g = b.build(THREE);
  CACHE.set('cockpit', g);
  return g;
}

// ---- hull material ------------------------------------------------------------------------------------
// MeshStandardMaterial + vertex colours, with three injections: object-space panel lines and per-panel
// tint (detail without textures), the aGlow channel as emissive light, and a hit flash / scorch uniform.
export function hullMaterial(THREE, o = {}) {
  const m = new THREE.MeshStandardMaterial({ vertexColors: true, metalness: o.metal != null ? o.metal : 0.5, roughness: o.rough != null ? o.rough : 0.46, envMapIntensity: o.env != null ? o.env : 0.9 });
  const U = { uFlash: { value: 0 }, uFlashCol: { value: new THREE.Color(1, 1, 1) }, uGlowMul: { value: 1 }, uPanel: { value: o.panel || 0.55 }, uPanelK: { value: o.panelK != null ? o.panelK : 1 }, uScorch: { value: 0 },
    uRock: { value: o.rock ? 1 : 0 }, uBump: { value: o.bump != null ? o.bump : (o.rock ? 1.0 : 0.35) }, uNearFade: { value: o.nearFade || 0 } };
  m.userData.U = U;
  m.onBeforeCompile = (sh) => {
    Object.assign(sh.uniforms, U);
    sh.vertexShader = sh.vertexShader
      .replace('#include <common>', '#include <common>\nattribute float aGlow;\nvarying float vGlow;\nvarying vec3 vObj;\nvarying vec3 vObjN;')
      .replace('#include <begin_vertex>', '#include <begin_vertex>\nvGlow = aGlow; vObj = position; vObjN = objectNormal;');
    sh.fragmentShader = sh.fragmentShader
      .replace('#include <common>', `#include <common>
uniform float uFlash; uniform vec3 uFlashCol; uniform float uGlowMul; uniform float uPanel; uniform float uPanelK; uniform float uScorch; uniform float uRock; uniform float uBump; uniform float uNearFade;
float hullH = 0.0;
varying float vGlow; varying vec3 vObj; varying vec3 vObjN;
float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float h31(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
float vn3(vec3 p) { vec3 i = floor(p), f = fract(p); f = f * f * (3.0 - 2.0 * f);
  return mix(mix(mix(h31(i), h31(i + vec3(1,0,0)), f.x), mix(h31(i + vec3(0,1,0)), h31(i + vec3(1,1,0)), f.x), f.y),
             mix(mix(h31(i + vec3(0,0,1)), h31(i + vec3(1,0,1)), f.x), mix(h31(i + vec3(0,1,1)), h31(i + vec3(1,1,1)), f.x), f.y), f.z); }`)
      .replace('#include <color_fragment>', `#include <color_fragment>
if (uNearFade > 0.0) {   // dissolve geometry that sits right in front of the camera (screen-door, no sorting)
  float dz = length(vViewPosition), k = clamp((dz - uNearFade * 0.35) / (uNearFade * 0.65), 0.0, 1.0);
  if (k < 1.0 && fract(sin(dot(floor(gl_FragCoord.xy), vec2(12.9898, 78.233))) * 43758.5453) > k) discard;
}
{
  vec3 an = abs(normalize(vObjN));
  vec3 pp = vObj * uPanel;
  vec2 q = an.x > an.y && an.x > an.z ? pp.yz : (an.y > an.z ? pp.xz * vec2(1.0, 0.5) : pp.xy);
  vec2 cell = floor(q), f = abs(fract(q) - 0.5);
  float line = smoothstep(0.44, 0.49, max(f.x, f.y)) * step(vGlow, 0.5);
  float tint = 0.9 + 0.16 * h21(cell);
  float grime = 0.86 + 0.14 * vn3(vObj * 0.35);
  diffuseColor.rgb *= mix(1.0, tint * grime * (1.0 - line * 0.5), uPanelK);
  float sc = smoothstep(0.35, 0.75, vn3(vObj * 0.5 + 3.0)) * uScorch;
  diffuseColor.rgb *= 1.0 - sc * 0.85;
  if (uRock > 0.5) {   // rock: layered noise albedo + height for the bump below
    vec3 rp = vObj * 2.2;
    float n1 = vn3(rp), n2 = vn3(rp * 3.1 + 7.0), n3 = vn3(rp * 9.0 + 3.0);
    hullH = n1 * 0.55 + n2 * 0.3 + n3 * 0.15;
    diffuseColor.rgb *= 0.62 + 0.55 * hullH;
  } else hullH = -line * 0.6 + grime * 0.3;
}`)
      .replace('#include <normal_fragment_maps>', `#include <normal_fragment_maps>
{
  vec3 dpx = dFdx(-vViewPosition), dpy = dFdy(-vViewPosition);
  float hx = dFdx(hullH), hy = dFdy(hullH);
  vec3 r1 = cross(dpy, normal), r2 = cross(normal, dpx);
  float det = dot(dpx, r1);
  vec3 grad = sign(det) * (hx * r1 + hy * r2);
  vec3 nb = abs(det) * normal - grad * uBump;
  float nl = length(nb);
  if (nl > 1e-12) normal = nb / nl;   // sub-pixel / edge-on triangles have zero derivatives: keep the normal
}`)
      .replace('#include <emissivemap_fragment>', `#include <emissivemap_fragment>
totalEmissiveRadiance += vColor.rgb * vGlow * uGlowMul + uFlashCol * uFlash;`);
  };
  m.customProgramCacheKey = () => 'stelle-hull';
  return m;
}

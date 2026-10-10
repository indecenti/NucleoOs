// stelle/kit.js — the procedural model kit: every ship, station, beacon and rock is assembled in
// code from a few hard-surface primitives into ONE merged BufferGeometry per model (one draw call),
// with per-vertex faction colour and a glow channel (aGlow) that the hull material turns into light:
//   aGlow > 9.5      engine role: emissive (aGlow - 10) scaled by the material's uEngine (throttle/boost)
//   0 < aGlow < 9.5  windows, running lights, crystal edges, glyphs (constant emissive)
//   aGlow <= 0       painted surface; -aGlow is how worn the paint is (chips to bare metal at the seams)
// Primitives: chamfered tapered boxes, lofts between chamfered-octagon sections (faceted hulls), lathes
// (nacelles, nozzles, domes, rings, spires), skins (plates, stripes, windows, hazard bands that follow a
// loft surface), extruded plates, mirrored detail (sym). Every triangle has non-zero area and a unit
// normal that agrees with its winding — a zero normal turns into NaN pixels in the shader.
// Silhouettes are distinct per class on purpose: rugged wedge courier (Lucciola), needle (Lancer), H
// (Bastion), lopsided wing (Scrapwing), fork (Harpoon), drill (Gutter), halo ring (Votive), censer
// bomber, crystal lance (Lattice), chandelier (Choir), box train (Hauler), collared corvette (Warden),
// welded freighter (Hulk), cathedral barge (Reliquary). Gun/engine/turret points come from sim.js CLS,
// so muzzle flashes, plumes and turrets sit exactly where the simulation fires from.
import { CLS, F_GILDA, F_CUSTODI, F_RELITTI, F_ECO, F_PLAYER } from './sim.js';
import { rng } from './world.js';

const TAU = Math.PI * 2;
const lin = (c) => [Math.pow(c[0], 2.2), Math.pow(c[1], 2.2), Math.pow(c[2], 2.2)];
const cmix = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
export const PALETTES = {
  [F_PLAYER]: { hull: [0.74, 0.69, 0.47], hull2: [0.31, 0.32, 0.27], paint2: [0.42, 0.45, 0.25], accent: [0.97, 0.64, 0.13], stripe: [0.88, 0.62, 0.24], hazard: [0.96, 0.72, 0.12],
    metal: [0.58, 0.56, 0.5], dark: [0.10, 0.10, 0.09], glass: [0.14, 0.2, 0.18], glow: [1.0, 0.86, 0.5], engine: [0.86, 1.0, 0.5], light: [1.0, 0.8, 0.42], shield: [0.55, 1.0, 0.75], wear: 0.85 },
  [F_GILDA]: { hull: [0.88, 0.85, 0.76], hull2: [0.74, 0.57, 0.29], paint2: [0.80, 0.77, 0.68], accent: [0.17, 0.33, 0.80], decal: [0.86, 0.66, 0.30], trim: [0.80, 0.62, 0.30], metal: [0.62, 0.6, 0.55],
    dark: [0.10, 0.11, 0.15], glass: [0.12, 0.20, 0.38], glow: [0.62, 0.80, 1.0], engine: [0.50, 0.74, 1.0], light: [0.66, 0.84, 1.0], shield: [0.45, 0.7, 1.0], wear: 0.18, metals: ['hull2', 'trim', 'decal'] },
  [F_CUSTODI]: { hull: [0.86, 0.85, 0.81], hull2: [0.31, 0.57, 0.50], paint2: [0.74, 0.73, 0.68], accent: [0.88, 0.68, 0.27], decal: [0.90, 0.70, 0.30], trim: [0.86, 0.66, 0.28], metal: [0.64, 0.61, 0.54],
    dark: [0.12, 0.14, 0.13], glass: [0.28, 0.24, 0.12], glow: [1.0, 0.82, 0.48], engine: [1.0, 0.80, 0.42], light: [1.0, 0.86, 0.55], shield: [1.0, 0.85, 0.45], wear: 0.35, metals: ['trim', 'decal', 'accent'] },
  [F_RELITTI]: { hull: [0.50, 0.28, 0.16], hull2: [0.17, 0.15, 0.14], paint2: [0.40, 0.30, 0.22], accent: [1.0, 0.52, 0.10], hazard: [1.0, 0.56, 0.08], metal: [0.52, 0.49, 0.45],
    dark: [0.07, 0.06, 0.06], glass: [0.30, 0.16, 0.06], glow: [1.0, 0.58, 0.18], engine: [1.0, 0.48, 0.14], light: [1.0, 0.62, 0.22], shield: [1.0, 0.6, 0.25], wear: 1.0,
    patch: [[0.52, 0.50, 0.47], [0.36, 0.31, 0.26], [0.58, 0.34, 0.19], [0.28, 0.30, 0.27], [0.62, 0.40, 0.18]] },
  [F_ECO]: { hull: [0.06, 0.07, 0.10], hull2: [0.13, 0.15, 0.22], paint2: [0.10, 0.10, 0.16], accent: [0.42, 0.92, 1.0], edge: [0.42, 0.9, 1.0], edge2: [0.6, 0.44, 1.0], crysA: [0.08, 0.3, 0.44], crysB: [0.22, 0.12, 0.42], metal: [0.2, 0.22, 0.3],
    dark: [0.03, 0.03, 0.05], glass: [0.3, 0.6, 0.9], glow: [0.52, 0.92, 1.0], engine: [0.72, 0.52, 1.0], light: [0.6, 0.9, 1.0], shield: [0.6, 0.85, 1.0], wear: 0, gloss: true },
};
// glow strength per role (fed to the emissive channel; >1 blooms). Engine roles are stored as 10 + glow
// so the material can scale them with the throttle (uEngine) without touching windows and lights.
const GLOW = { glow: 2.4, light: 3.0, window: 1.7, winDim: 0.8, glass: 0.32, core: 4.5, panel: 0.6, navR: 2.6, navG: 2.6, edge: 1.35, edge2: 1.25, glyph: 1.2, lantern: 2.8, crysA: 0.3, crysB: 0.26,
  engine: 11.15, engineHot: 11.3, engineDim: 10.4 };
const ROLE_COL = {
  window: (p) => p.light, core: (p) => p.light, winDim: (p) => p.light, lantern: (p) => p.glow, panel: (p) => p.glass, glyph: (p) => p.glow,
  bone: () => [0.93, 0.9, 0.82], navR: () => [1, 0.14, 0.08], navG: () => [0.25, 1, 0.4], engine: (p) => p.engine, engineDim: (p) => p.engine, engineHot: (p) => cmix(p.engine, [1, 1, 1], 0.5),
  edge: (p) => p.edge || p.accent, edge2: (p) => p.edge2 || p.engine, stripe: (p) => p.stripe || cmix(p.accent, p.hull, 0.3), hazard: (p) => p.hazard || p.accent,
  metal: (p) => p.metal || [0.56, 0.56, 0.57], crysA: (p) => p.crysA || p.glass, crysB: (p) => p.crysB || p.glass, paint2: (p) => p.paint2 || p.hull2, decal: (p) => p.decal || p.accent, trim: (p) => p.trim || p.hull2,
};
const JIT = { hull: 0.1, hull2: 0.1, paint2: 0.1, patch: 0.14, metal: 0.12, stripe: 0.24, trim: 0.06, accent: 0.05, shell: 0.45 };

// ---- small vector / matrix helpers ------------------------------------------------------------------
const sub3 = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const lerp3 = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
const cross3 = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
const dot3 = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const unit3 = (v) => { const l = Math.hypot(v[0], v[1], v[2]); return l > 1e-9 && Number.isFinite(l) ? [v[0] / l, v[1] / l, v[2] / l] : null; };
const ctr3 = (pts) => { let x = 0, y = 0, z = 0; for (const p of pts) { x += p[0]; y += p[1]; z += p[2]; } return [x / pts.length, y / pts.length, z / pts.length]; };
const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
// euler XYZ rotation (R = Rz * Ry * Rx, applied to column vectors)
function rotM(rot) {
  const [a, b, c] = rot, ca = Math.cos(a), sa = Math.sin(a), cb = Math.cos(b), sb = Math.sin(b), cc = Math.cos(c), sc = Math.sin(c);
  return [cb * cc, sa * sb * cc - ca * sc, ca * sb * cc + sa * sc, cb * sc, sa * sb * sc + ca * cc, ca * sb * sc - sa * cc, -sb, sa * cb, ca * cb];
}
function mcompose(A, B) {   // A after B
  const a = A.R, b = B.R, o = B.o;
  return { R: [a[0] * b[0] + a[1] * b[3] + a[2] * b[6], a[0] * b[1] + a[1] * b[4] + a[2] * b[7], a[0] * b[2] + a[1] * b[5] + a[2] * b[8],
    a[3] * b[0] + a[4] * b[3] + a[5] * b[6], a[3] * b[1] + a[4] * b[4] + a[5] * b[7], a[3] * b[2] + a[4] * b[5] + a[5] * b[8],
    a[6] * b[0] + a[7] * b[3] + a[8] * b[6], a[6] * b[1] + a[7] * b[4] + a[8] * b[7], a[6] * b[2] + a[7] * b[5] + a[8] * b[8]],
  o: [a[0] * o[0] + a[1] * o[1] + a[2] * o[2] + A.o[0], a[3] * o[0] + a[4] * o[1] + a[5] * o[2] + A.o[1], a[6] * o[0] + a[7] * o[1] + a[8] * o[2] + A.o[2]] };
}
function basis(ax) {
  let u = Math.abs(ax[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0];
  const v = unit3(cross3(ax, u)); u = cross3(v, ax);
  return [u, v];
}
// chamfered-octagon cross-section [z, w, h, y, ct, cb, x] -> 8 points (CCW seen from +Z): edge k runs
// from point k to k+1: 0 bottom, 1 bottom-right chamfer, 2 right side, 3 top-right chamfer, 4 top,
// 5 top-left chamfer, 6 left side, 7 bottom-left chamfer
const fullSec = (s) => [s[0], s[1], s[2], s[3], s[4], s[5] != null ? s[5] : s[4], s[6] || 0];
function octPts(s) {
  const [z, w, h, y, ct, cb, x] = fullSec(s), hw = w / 2, hh = h / 2;
  let t = Math.min(ct, hw * 0.98), d = Math.min(cb, hw * 0.98);
  const k = Math.min(1, h * 0.98 / Math.max(1e-6, t + d)); t *= k; d *= k;
  return [[x - hw + d, y - hh, z], [x + hw - d, y - hh, z], [x + hw, y - hh + d, z], [x + hw, y + hh - t, z], [x + hw - t, y + hh, z], [x - hw + t, y + hh, z], [x - hw, y + hh - t, z], [x - hw, y - hh + d, z]];
}
// a section interpolated at depth z along a list of sections
function secAt(secs, z) {
  const S = secs.map(fullSec).sort((a, b) => a[0] - b[0]);
  if (z <= S[0][0]) return [z, ...S[0].slice(1)];
  for (let i = 0; i < S.length - 1; i++) if (z <= S[i + 1][0]) { const t = (z - S[i][0]) / ((S[i + 1][0] - S[i][0]) || 1); return S[i].map((v, j) => (j ? v + (S[i + 1][j] - v) * t : z)); }
  return [z, ...S[S.length - 1].slice(1)];
}
const secTop = (secs, z) => { const s = secAt(secs, z); return s[3] + s[2] / 2; };
const secBot = (secs, z) => { const s = secAt(secs, z); return s[3] - s[2] / 2; };

// ---- builder ----------------------------------------------------------------------------------
class Builder {
  constructor(pal, seed = 1) {
    this.P = []; this.N = []; this.C = []; this.G = [];
    this.pal = pal; this.r = rng(seed);
    this.base = null; this.m = null;   // current transform { o, R } (base = enclosing frame)
  }
  get tris() { return this.P.length / 9; }
  col(role) {
    const p = this.pal; let c;
    if (role === 'patch' && p.patch) c = p.patch[this.r.int(p.patch.length)];
    else c = ROLE_COL[role] ? ROLE_COL[role](p) : (p[role] || p.hull);
    const j = JIT[role] ? 1 - JIT[role] * 0.6 + this.r() * JIT[role] : 1;
    const l = lin(c);
    return [l[0] * j, l[1] * j, l[2] * j];
  }
  // non-glowing roles: -wear for paint, -1.5 bare metal, -2.5 glossy (glass, crystal, polished)
  glow(role) { const p = this.pal; return GLOW[role] || (p.gloss ? -2.5 : role === 'metal' || role === 'dark' || (p.metals && p.metals.indexOf(role) >= 0) ? -1.5 : -(p.wear || 0)); }
  // transform helpers: euler XYZ rotation + offset applied to every primitive vertex until reset()
  at(o = [0, 0, 0], rot = [0, 0, 0]) { const t = { o, R: rotM(rot) }; this.m = this.base ? mcompose(this.base, t) : t; return this; }
  reset() { this.m = this.base; return this; }
  // run fn inside a nested local frame (at() calls inside compose with it)
  frame(o, rot, fn) {
    const sb = this.base, sm = this.m, t = { o, R: rotM(rot || [0, 0, 0]) };
    this.base = sm ? mcompose(sm, t) : t; this.m = this.base;
    fn(this);
    this.base = sb; this.m = sm;
    return this;
  }
  xp(v) { const m = this.m; if (!m) return v; const R = m.R, o = m.o; return [R[0] * v[0] + R[1] * v[1] + R[2] * v[2] + o[0], R[3] * v[0] + R[4] * v[1] + R[5] * v[2] + o[1], R[6] * v[0] + R[7] * v[1] + R[8] * v[2] + o[2]]; }
  xn(v) { const m = this.m; if (!m) return v; const R = m.R; return [R[0] * v[0] + R[1] * v[1] + R[2] * v[2], R[3] * v[0] + R[4] * v[1] + R[5] * v[2], R[6] * v[0] + R[7] * v[1] + R[8] * v[2]]; }
  _v(p, n, c, g) { this.P.push(p[0], p[1], p[2]); this.N.push(n[0], n[1], n[2]); this.C.push(c[0], c[1], c[2]); this.G.push(g); }
  // One triangle (local coordinates). Smooth: per-vertex normals na/nb/nc. Flat: nb == null and na is the
  // outward hint. Zero-area triangles are dropped; the winding is made to agree with the normal.
  tri(a, b, c, na, nb, nc, col, g) {
    const A = this.xp(a); let B = this.xp(b), C = this.xp(c);
    let f = cross3(sub3(B, A), sub3(C, A));
    const fl = Math.hypot(f[0], f[1], f[2]);
    if (!(fl > 2e-6)) return this;
    f = [f[0] / fl, f[1] / fl, f[2] / fl];
    if (nb == null) {
      const h = na ? this.xn(na) : f;
      if (dot3(f, h) < 0) { f = [-f[0], -f[1], -f[2]]; const t = B; B = C; C = t; }
      this._v(A, f, col, g); this._v(B, f, col, g); this._v(C, f, col, g);
      return this;
    }
    const NA = unit3(this.xn(na)) || f; let NB = unit3(this.xn(nb)) || f, NC = unit3(this.xn(nc)) || f;
    if (dot3(f, [NA[0] + NB[0] + NC[0], NA[1] + NB[1] + NC[1], NA[2] + NB[2] + NC[2]]) < 0) { let t = B; B = C; C = t; t = NB; NB = NC; NC = t; }
    this._v(A, NA, col, g); this._v(B, NB, col, g); this._v(C, NC, col, g);
    return this;
  }
  // flat convex polygon oriented outward from `ctr` (Newell normal, robust to collapsed corners)
  face(pts, ctr, role, c, g) {
    if (c == null) c = this.col(role);
    if (g == null) g = this.glow(role);
    let nx = 0, ny = 0, nz = 0;
    for (let i = 0; i < pts.length; i++) { const p = pts[i], q = pts[(i + 1) % pts.length]; nx += (p[1] - q[1]) * (p[2] + q[2]); ny += (p[2] - q[2]) * (p[0] + q[0]); nz += (p[0] - q[0]) * (p[1] + q[1]); }
    const l = Math.hypot(nx, ny, nz);
    if (!(l > 1e-9)) return this;
    let n = [nx / l, ny / l, nz / l];
    if (ctr) { const m = ctr3(pts); if (dot3(sub3(m, ctr), n) < 0) n = [-n[0], -n[1], -n[2]]; }
    for (let i = 1; i < pts.length - 1; i++) this.tri(pts[0], pts[i], pts[i + 1], n, null, null, c, g);
    return this;
  }
  // faceted loft between rings of points (same count, convex, consistently ordered), optional end caps
  loft(rings, role, o = {}) {
    const n = rings[0].length, cen = rings.map(ctr3), last = rings.length - 1, cache = {};
    const jit = o.jit != null ? o.jit : 0.03;
    const colOf = (r) => cache[r] || (cache[r] = this.col(r));
    for (let i = 0; i < last; i++) {
      const A = rings[i], B = rings[i + 1], mid = lerp3(cen[i], cen[i + 1], 0.5);
      for (let k = 0; k < n; k++) {
        const k1 = (k + 1) % n, r = (o.roleAt && o.roleAt(i, k)) || role, g = this.glow(r), c0 = colOf(r);
        const j = g > 0 || !jit ? 1 : 1 - jit + this.r() * jit * 2;
        this.face([A[k], A[k1], B[k1], B[k]], mid, r, [c0[0] * j, c0[1] * j, c0[2] * j], g);
      }
    }
    if (o.cap0 !== false) this.face(rings[0], cen[Math.min(1, last)], o.capRole0 || o.capRole || role);
    if (o.cap1 !== false) this.face(rings[last], cen[Math.max(0, last - 1)], o.capRole1 || o.capRole || role);
    return this;
  }
  // faceted hull through chamfered-octagon sections [z, w, h, y, ct, cb, x]
  hull(secs, role, o) { return this.loft(secs.map(octPts), role, o); }
  // a raised strip that follows face k of a section hull: u0..u1 across the face, z0..z1 along it,
  // thickness t (o.shear slides u along z for slanted stripes, o.lift floats it off the surface)
  skin(secs, k, u0, u1, z0, z1, t, role, o = {}) {
    const lo = Math.min(z0, z1), hi = Math.max(z0, z1), S = secs.map(fullSec).sort((a, b) => a[0] - b[0]);
    const list = [secAt(S, lo), ...S.filter((s) => s[0] > lo + 1e-3 && s[0] < hi - 1e-3), secAt(S, hi)];
    const sh = o.shear || 0, cl = o.clip || [0, 1], e = o.lift || 0, rings = [];
    for (const s of list) {
      const p = octPts(s), a = p[k & 7], c = p[(k + 1) & 7];
      let nx = c[1] - a[1], ny = -(c[0] - a[0]); const l = Math.hypot(nx, ny);
      if (l < 1e-4) return this;
      nx /= l; ny /= l;
      const d = sh * (s[0] - lo) / ((hi - lo) || 1), ua = clamp(u0 + d, cl[0], cl[1]), ub = clamp(u1 + d, cl[0], cl[1]);
      const P0 = lerp3(a, c, ua), P1 = lerp3(a, c, ub), z = s[0];
      rings.push([[P0[0] + nx * e, P0[1] + ny * e, z], [P1[0] + nx * e, P1[1] + ny * e, z], [P1[0] + nx * (e + t), P1[1] + ny * (e + t), z], [P0[0] + nx * (e + t), P0[1] + ny * (e + t), z]]);
    }
    return this.loft(rings, role, { jit: 0 });
  }
  // slanted hazard stripes over a dark band, following a section hull face
  skinHazard(secs, k, u0, u1, z0, z1, t = 0.04, n = 8, role = 'hazard') {
    this.skin(secs, k, u0, u1, z0, z1, t, 'dark');
    const w = (u1 - u0) / n;
    for (let i = -1; i < n; i++) { const a = u0 + i * w; this.skin(secs, k, a, a + w * 0.5, z0, z1, t + 0.014, role, { shear: w * 0.6, clip: [u0, u1] }); }
    return this;
  }
  // lathe: profile [[r, t], ...] revolved about the axis A->B (t in metres from A). Normals are flat
  // along the profile and smooth around it (crisp breaks, round shells); o.flat facets around too.
  // A closed profile must run counter-clockwise in the (r, t) plane. o.roles[j] / o.roleAt(j, i, dir)
  // pick the role of profile segment j (and angular step i); o.phase / o.arc for partial revolutions.
  lathe(A, B, prof, seg, role, o = {}) {
    const ax = unit3(sub3(B, A)); if (!ax) return this;
    const [u, v] = basis(ax), ph = o.phase || 0, arc = o.arc || TAU, dirs = [], cache = {};
    for (let i = 0; i <= seg; i++) { const a = ph + i / seg * arc, c = Math.cos(a), s = Math.sin(a); dirs.push([u[0] * c + v[0] * s, u[1] * c + v[1] * s, u[2] * c + v[2] * s]); }
    const pt = (r, t, d) => [A[0] + ax[0] * t + d[0] * r, A[1] + ax[1] * t + d[1] * r, A[2] + ax[2] * t + d[2] * r];
    for (let j = 0; j < prof.length - 1; j++) {
      const r0 = prof[j][0], t0 = prof[j][1], r1 = prof[j + 1][0], t1 = prof[j + 1][1], dr = r1 - r0, dt = t1 - t0, L = Math.hypot(dr, dt);
      if (L < 1e-7) continue;
      const nr = dt / L, nt = -dr / L, rj = (o.roles && o.roles[j]) || role;
      for (let i = 0; i < seg; i++) {
        const d0 = dirs[i], d1 = dirs[i + 1], dm = unit3([d0[0] + d1[0], d0[1] + d1[1], d0[2] + d1[2]]) || d0;
        const rr = (o.roleAt && o.roleAt(j, i, dm)) || rj, key = rr + ':' + j, c = cache[key] || (cache[key] = this.col(rr)), g = this.glow(rr);
        const p00 = pt(r0, t0, d0), p01 = pt(r0, t0, d1), p10 = pt(r1, t1, d0), p11 = pt(r1, t1, d1);
        if (o.flat) {
          const h = [dm[0] * nr + ax[0] * nt, dm[1] * nr + ax[1] * nt, dm[2] * nr + ax[2] * nt];
          this.tri(p00, p01, p11, h, null, null, c, g); this.tri(p00, p11, p10, h, null, null, c, g);
        } else {
          const n0 = [d0[0] * nr + ax[0] * nt, d0[1] * nr + ax[1] * nt, d0[2] * nr + ax[2] * nt], n1 = [d1[0] * nr + ax[0] * nt, d1[1] * nr + ax[1] * nt, d1[2] * nr + ax[2] * nt];
          this.tri(p00, p01, p11, n0, n1, n1, c, g); this.tri(p00, p11, p10, n0, n1, n0, c, g);
        }
      }
    }
    return this;
  }
  // flat annulus band (r0..r1) between t0 and t1 along A->B
  ring(A, B, r0, r1, t0, t1, seg, role, o = {}) { return this.lathe(A, B, [[r0, t0], [r1, t0], [r1, t1], [r0, t1], [r0, t0]], seg, role, o); }
  // chamfered tapered box: back face (+Z) sx×sy (scaled bx, by), front face (-Z) scaled (fx, fy) and
  // shifted fyo; o.ch chamfer on every edge (o.ct / o.cb override the top / bottom long edges)
  bbox(cx, cy, cz, sx, sy, sz, role, o = {}) {
    const fx = o.fx != null ? o.fx : 1, fy = o.fy != null ? o.fy : 1, fyo = o.fyo || 0, bx = o.bx != null ? o.bx : 1, by = o.by != null ? o.by : 1;
    const c = Math.min(o.ch != null ? o.ch : Math.min(sx, sy, sz) * 0.16, sz * 0.45), hz = sz / 2;
    const sec = (z, inset) => {
      const t = (cz + hz - z) / sz, kx = bx + (fx - bx) * t, ky = by + (fy - by) * t;
      const ct = (o.ct != null ? o.ct : c) * (inset ? 0.4 : 1), cb = (o.cb != null ? o.cb : c) * (inset ? 0.4 : 1);
      return octPts([z, Math.max(sx * kx - inset * 2, 1e-3), Math.max(sy * ky - inset * 2, 1e-3), cy + fyo * t, ct, cb, cx]);
    };
    const rings = [sec(cz + hz, c), sec(cz + hz - c, 0), sec(cz - hz + c, 0), sec(cz - hz, c)], rs = o.roles || {};
    const roleAt = (i, k) => (i !== 1 ? rs.edge || null : k === 4 ? rs.top : k === 0 ? rs.bottom : k === 2 || k === 6 ? rs.side : rs.chamfer || null);
    return this.loft(rings, role, { jit: o.jit != null ? o.jit : 0.02, roleAt, capRole0: rs.back, capRole1: rs.front });
  }
  // tapered box (sharp edges): back face (+Z) sx×sy, front face (-Z) scaled by (fx, fy) and shifted by fyo
  box(cx, cy, cz, sx, sy, sz, role, o = {}) {
    const fx = o.fx != null ? o.fx : 1, fy = o.fy != null ? o.fy : 1, fyo = o.fyo || 0, bx = o.bx != null ? o.bx : 1, by = o.by != null ? o.by : 1;
    const hx = sx / 2, hy = sy / 2, hz = sz / 2;
    const F = [[cx - hx * fx, cy - hy * fy + fyo, cz - hz], [cx + hx * fx, cy - hy * fy + fyo, cz - hz], [cx + hx * fx, cy + hy * fy + fyo, cz - hz], [cx - hx * fx, cy + hy * fy + fyo, cz - hz]];
    const K = [[cx - hx * bx, cy - hy * by, cz + hz], [cx + hx * bx, cy - hy * by, cz + hz], [cx + hx * bx, cy + hy * by, cz + hz], [cx - hx * bx, cy + hy * by, cz + hz]];
    const ctr = [cx, cy + fyo * 0.5, cz], rs = o.roles || {};
    this.face(F, ctr, rs.front || role); this.face(K, ctr, rs.back || role);
    this.face([F[0], F[1], K[1], K[0]], ctr, rs.bottom || role); this.face([F[3], F[2], K[2], K[3]], ctr, rs.top || role);
    this.face([F[0], F[3], K[3], K[0]], ctr, rs.side || role); this.face([F[1], F[2], K[2], K[1]], ctr, rs.side || role);
    return this;
  }
  // truncated cone between two points (smooth sides)
  cyl(a, b, ra, rb, seg, role, caps = true, capRole) {
    const L = Math.hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
    if (L < 1e-6) return this;
    return caps ? this.lathe(a, b, [[0, 0], [ra, 0], [rb, L], [0, L]], seg, role, { roles: { 0: capRole || role, 2: capRole || role } }) : this.lathe(a, b, [[ra, 0], [rb, L]], seg, role);
  }
  // ellipsoid (optionally only the upper half: half=1)
  ell(cx, cy, cz, rx, ry, rz, ws, hs, role, half = 0) {
    const c = this.col(role), g = this.glow(role);
    const P = (i, j) => { const th = j / hs * Math.PI, ph = i / ws * TAU, st = Math.sin(th), ct = Math.cos(th); return [[cx + rx * st * Math.cos(ph), cy + ry * ct, cz + rz * st * Math.sin(ph)], [st * Math.cos(ph) / rx, ct / ry, st * Math.sin(ph) / rz]]; };
    const jn = half ? Math.ceil(hs / 2) : hs;
    for (let j = 0; j < jn; j++) for (let i = 0; i < ws; i++) {
      const A = P(i, j), B = P(i + 1, j), C = P(i + 1, j + 1), D = P(i, j + 1);
      this.tri(A[0], B[0], C[0], A[1], B[1], C[1], c, g); this.tri(A[0], C[0], D[0], A[1], C[1], D[1], c, g);
    }
    return this;
  }
  // flat convex plate from [x,z] points at height y with thickness t (wings, fins with at())
  plate(pts, y, t, role, edgeRole) {
    const top = pts.map((p) => [p[0], y + t / 2, p[1]]), bot = pts.map((p) => [p[0], y - t / 2, p[1]]), ctr = [ctr3(top)[0], y, ctr3(top)[2]];
    this.face(top, ctr, role); this.face(bot, ctr, role);
    for (let i = 0; i < pts.length; i++) { const j = (i + 1) % pts.length; this.face([top[i], top[j], bot[j], bot[i]], ctr, edgeRole || role); }
    return this;
  }
  torus(cx, cy, cz, R, r, seg, tseg, role, axis = 'z', arc = TAU) {
    const A = [cx, cy, cz], B = axis === 'z' ? [cx, cy, cz + 1] : axis === 'y' ? [cx, cy + 1, cz] : [cx + 1, cy, cz], prof = [];
    for (let j = 0; j <= tseg; j++) { const b = j / tseg * TAU; prof.push([R + r * Math.cos(b), r * Math.sin(b)]); }
    return this.lathe(A, B, prof, seg, role, { arc });
  }
  // glowing disc facing +Z (nz = 1) or -Z
  disc(cx, cy, cz, r, role, seg = 12, nz = 1) { return this.lathe([cx, cy, cz], [cx, cy, cz + 1], nz > 0 ? [[r, 0], [0, 0]] : [[0, 0], [r, 0]], seg, role); }
  // engine: armoured housing and lip, a recessed dark bell with stator vanes; only a small hot core (~40% of
  // the nozzle radius) and a thin inner rim glow — the renderer adds its own nozzle sprite and exhaust. Exit at z.
  engine(x, y, z, r, housing = 'hull2', seg = 16) {
    this.lathe([x, y, z - r * 1.5], [x, y, z], [[0, 0], [r * 1.02, 0], [r * 1.14, r * 0.12], [r * 1.14, r * 0.95], [r * 1.08, r * 1.02], [r * 1.08, r * 1.5], [r * 0.92, r * 1.5], [r * 0.82, r * 1.14], [r * 0.52, r * 0.86], [0, r * 0.8]], seg, housing,
      { roles: { 3: 'metal', 4: 'metal', 5: 'dark', 6: 'dark', 7: 'dark', 8: 'dark' } });
    this.nozzleCore(x, y, z - r * 0.7, r, seg);
    return this;
  }
  // the glowing heart of a nozzle whose dark back plate sits at depth z0 (exit radius r): vanes, core, rim
  nozzleCore(x, y, z0, r, seg = 16) {
    const n = Math.max(6, Math.round(seg / 2));
    for (let k = 0; k < n; k++) { const a = (k + 0.5) / n * TAU; this.at([x + Math.cos(a) * r * 0.64, y + Math.sin(a) * r * 0.64, z0 + r * 0.42], [0, 0, a]).box(0, 0, 0, r * 0.38, r * 0.05, r * 0.26, 'metal').reset(); }
    this.lathe([x, y, z0], [x, y, z0 + 1], [[r * 0.4, 0], [r * 0.3, r * 0.16], [r * 0.12, r * 0.3], [0, r * 0.34]], 12, 'engine', { roles: { 2: 'engineHot' } });
    this.ring([x, y, z0], [x, y, z0 + 1], r * 0.47, r * 0.53, 0.0, r * 0.03, seg, 'engineDim');
    return this;
  }
  // gun barrel pointing -Z with the muzzle exactly at g (glowing bore, muzzle brake, breech)
  gun(g, len = 2.6, r = 0.13, seg = 8) {
    const [x, y, z] = g;
    this.lathe([x, y, z], [x, y, z + 1], [[0, 0.06], [r * 0.55, 0.06], [r * 0.55, 0], [r * 1.3, 0], [r * 1.3, len * 0.1], [r * 0.95, len * 0.12], [r * 0.95, len * 0.55], [r * 1.25, len * 0.58], [r * 1.5, len * 0.7], [r * 1.5, len], [0, len]], seg, 'dark',
      { roles: { 0: 'glow', 3: 'metal', 7: 'hull2', 8: 'hull2' } });
    return this;
  }
  greebles(n, x0, x1, y, z0, z1, s = 0.4, role = 'hull2') {
    for (let i = 0; i < n; i++) { const w = s * (0.5 + this.r()), h = s * (0.3 + this.r() * 0.6), l = s * (0.6 + this.r() * 1.4); this.box(this.r.range(x0, x1), y + h / 2, this.r.range(z0, z1), w, h, l, this.r() < 0.15 ? 'accent' : role); }
    return this;
  }
  lights(list, size = 0.18, role = 'light') { for (const p of list) this.box(p[0], p[1], p[2], size, size, size, role); return this; }
  // ---- surface details in a local frame (surface at y = 0, facing +y); place them with frame()/at()
  hazard(len, w, n = 6, t = 0.03, role = 'hazard') {
    this.box(0, t / 2, 0, len, t, w, 'dark');
    const step = len / n, sl = w * 0.7, lo = -len / 2, hi = len / 2, c = (v) => clamp(v, lo, hi);
    for (let k = -1; k < n; k++) { const x0 = lo + k * step; this.plate([[c(x0), w / 2], [c(x0 + step * 0.5), w / 2], [c(x0 + step * 0.5 + sl), -w / 2], [c(x0 + sl), -w / 2]], t + 0.006, 0.01, role); }
    return this;
  }
  vent(w, l, n = 5, role = 'hull2') {
    this.box(0, 0.015, 0, w, 0.03, l, 'dark');
    for (let k = 0; k < n; k++) this.box(0, 0.05, -l / 2 + (k + 0.5) * l / n, w * 0.9, 0.045, l / n * 0.42, role);
    return this;
  }
  hatch(w, l, role = 'paint2', frame = 'hull2', h = 0.05) {
    const f = Math.min(w, l) * 0.09;
    this.box(0, h * 0.4, 0, w - f, h * 0.8, l - f, role);
    this.box(-w / 2 + f / 2, h * 0.6, 0, f, h * 1.2, l, frame); this.box(w / 2 - f / 2, h * 0.6, 0, f, h * 1.2, l, frame);
    this.box(0, h * 0.6, -l / 2 + f / 2, w - 2 * f, h * 1.2, f, frame); this.box(0, h * 0.6, l / 2 - f / 2, w - 2 * f, h * 1.2, f, frame);
    return this;
  }
  // row of n lit windows along local X (facing +y), each w x l, with gaps
  windowRow(n, w, l, gap, role = 'window') { const span = n * w + (n - 1) * gap; for (let k = 0; k < n; k++) this.box(-span / 2 + w / 2 + k * (w + gap), 0.02, 0, w, 0.04, l, role); return this; }
  // faction insignia (local frame facing +y): Gilda brass ring + blue field, Custodi glyph wheel, Relitti
  // crude chevron, Echo eye
  insignia(fac, r) {
    const A = [0, 0, 0], B = [0, 1, 0];
    if (fac === F_GILDA) { this.ring(A, B, 0, r * 0.7, 0, 0.03, 18, 'accent'); this.ring(A, B, r * 0.7, r, 0, 0.05, 18, 'decal'); this.ring(A, B, r * 0.28, r * 0.42, 0, 0.06, 12, 'decal'); for (let k = 0; k < 4; k++) this.at([0, 0.045, 0], [0, k * Math.PI / 2, 0]).box(0, 0, -r * 0.55, r * 0.1, 0.03, r * 0.26, 'decal').reset(); }
    else if (fac === F_CUSTODI) { this.ring(A, B, r * 0.82, r, 0, 0.04, 20, 'decal'); for (let k = 0; k < 8; k++) this.at([0, 0.03, 0], [0, k * TAU / 8, 0]).box(0, 0, -r * 0.5, r * 0.08, 0.03, r * (k % 2 ? 0.45 : 0.62), 'glyph').reset(); this.ring(A, B, 0, r * 0.18, 0, 0.05, 10, 'decal'); }
    else if (fac === F_RELITTI) { for (let k = 0; k < 2; k++) { const z = (k - 0.5) * r * 0.8 + r * 0.25; this.plate([[-r, z + r * 0.2], [-r, z - r * 0.15], [0, z - r * 0.75], [0, z - r * 0.4]], 0.02, 0.03, 'hazard'); this.plate([[0, z - r * 0.4], [0, z - r * 0.75], [r, z - r * 0.15], [r, z + r * 0.2]], 0.02, 0.03, 'hazard'); } }
    else if (fac === F_ECO) { this.ring(A, B, r * 0.5, r * 0.66, 0, 0.03, 6, 'edge', { flat: true }); this.ring(A, B, 0, r * 0.22, 0, 0.05, 6, 'edge2', { flat: true }); }
    return this;
  }
  // a collar that hugs a section hull between z0 and z1, standing t proud of it
  band(secs, z0, z1, t, role) {
    const lo = Math.min(z0, z1), hi = Math.max(z0, z1), S = secs.map(fullSec).sort((a, b) => a[0] - b[0]);
    const list = [secAt(S, lo), ...S.filter((s) => s[0] > lo + 1e-3 && s[0] < hi - 1e-3), secAt(S, hi)];
    return this.hull(list.map((s) => [s[0], s[1] + 2 * t, s[2] + 2 * t, s[3], s[4] + t * 0.41, s[5] + t * 0.41, s[6]]), role, { jit: 0 });
  }
  // thin prism between two points (glowing crystal edges, cables, struts)
  edge(a, b, r, role = 'edge', seg = 3) { const L = Math.hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]); return L > 1e-4 ? this.lathe(a, b, [[r, 0], [r, L]], seg, role, { flat: true }) : this; }
  // faceted crystal from A to B: point, waist (r) at k0, shoulder at k1, point; o.edge = glowing edge role
  crystal(A, B, r, seg, role, o = {}) {
    const L = Math.hypot(B[0] - A[0], B[1] - A[1], B[2] - A[2]), k0 = o.k0 != null ? o.k0 : 0.3, k1 = o.k1 != null ? o.k1 : 0.62, tp = o.taper != null ? o.taper : 0.8, ph = o.phase || 0;
    if (L < 1e-4) return this;
    this.lathe(A, B, [[0, 0], [r, L * k0], [r * tp, L * k1], [0, L]], seg, role, { flat: true, phase: ph, roleAt: o.face2 ? (j, i) => ((i + j) % 2 ? o.face2 : null) : null });
    if (o.edge) {
      const ax = unit3(sub3(B, A)), [u, v] = basis(ax), er = o.er || r * 0.06;
      for (let i = 0; i < seg; i++) {
        const a = ph + i / seg * TAU, d = [u[0] * Math.cos(a) + v[0] * Math.sin(a), u[1] * Math.cos(a) + v[1] * Math.sin(a), u[2] * Math.cos(a) + v[2] * Math.sin(a)];
        const p1 = [A[0] + ax[0] * L * k0 + d[0] * r, A[1] + ax[1] * L * k0 + d[1] * r, A[2] + ax[2] * L * k0 + d[2] * r], p2 = [A[0] + ax[0] * L * k1 + d[0] * r * tp, A[1] + ax[1] * L * k1 + d[1] * r * tp, A[2] + ax[2] * L * k1 + d[2] * r * tp];
        this.edge(A, p1, er, o.edge); this.edge(p1, p2, er, o.edge); this.edge(p2, B, er, o.edge);
      }
    }
    return this;
  }
  // turret barbette: a ringed drum from `depth` metres inside the hull up (or down) to the mount point p
  barbette(p, down, r, depth, role = 'hull2') {
    const d = down ? -1 : 1, A = [p[0], p[1] - d * depth, p[2]], B = [p[0], p[1] + d, p[2]];
    this.lathe(A, B, [[r * 0.9, 0], [r, depth * 0.35], [r, depth - 0.6], [r * 1.12, depth - 0.5], [r * 1.12, depth], [0, depth]], 18, role, { roles: { 3: 'metal', 4: 'metal', 5: 'dark' } });
    return this;
  }
  // a lit porthole on a surface at p facing n: metal rim and a glowing pane
  porthole(p, n, r, rim = 'trim', pane = 'lantern') {
    const B = [p[0] + n[0], p[1] + n[1], p[2] + n[2]];
    this.ring(p, B, r * 0.72, r, -0.05, r * 0.22, 12, rim);
    this.lathe(p, B, [[r * 0.74, r * 0.08], [0, r * 0.08]], 12, pane);
    return this;
  }
  // mirror everything fn() emits across x = 0 (detail built once for both sides)
  sym(fn) {
    const s = this.P.length; fn(this); const e = this.P.length, P = this.P, N = this.N, C = this.C, G = this.G;
    for (let i = s; i < e; i += 9) for (const k of [0, 2, 1]) { const q = i + k * 3; P.push(-P[q], P[q + 1], P[q + 2]); N.push(-N[q], N[q + 1], N[q + 2]); C.push(C[q], C[q + 1], C[q + 2]); G.push(G[q / 3]); }
    return this;
  }
  append(o) { for (let i = 0; i < o.P.length; i++) { this.P.push(o.P[i]); this.N.push(o.N[i]); this.C.push(o.C[i]); } for (let i = 0; i < o.G.length; i++) this.G.push(o.G[i]); return this; }
  build(THREE, off) {
    let P = this.P;
    if (off) { P = P.slice(); for (let i = 0; i < P.length; i += 3) { P[i] += off[0]; P[i + 1] += off[1]; P[i + 2] += off[2]; } }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(P, 3));
    g.setAttribute('normal', new THREE.Float32BufferAttribute(this.N, 3));
    g.setAttribute('color', new THREE.Float32BufferAttribute(this.C, 3));
    g.setAttribute('aGlow', new THREE.Float32BufferAttribute(this.G, 1));
    g.computeBoundingSphere(); g.computeBoundingBox();
    return g;
  }
}
const mir = (pts) => pts.map((p) => [-p[0], p[1]]);

// ---- the player's courier: Lucciola -----------------------------------------------------------------
// Hull sections [z, w, h, y, ct, cb]: a wide, low, bevelled wedge; the nose slope (z -6.05..-4.4) is the
// big cargo ramp, the raised brow above it carries the windscreen. The nacelles are separate parts that
// swivel about X through their pylon hinges (shipParts); the nozzle exits sit on CLS.lucciola.eng.
const LUC = [[5.75, 3.7, 1.7, 0.2, 0.45, 0.35], [5.2, 4.5, 2.4, 0.12, 0.7, 0.45], [2.6, 5.1, 2.8, 0.05, 0.9, 0.55], [-2.0, 5.1, 2.8, 0, 0.9, 0.55],
  [-4.4, 4.7, 2.5, -0.15, 0.95, 0.5], [-5.6, 4.0, 1.5, -0.63, 0.6, 0.4], [-6.05, 3.5, 1.0, -0.75, 0.32, 0.28]];
const LUC_CK = [[0.7, 2.4, 0.45, 1.42, 0.18, 0.04], [-0.5, 3.2, 1.3, 1.72, 0.45, 0.04], [-3.5, 3.3, 1.3, 1.66, 0.45, 0.04], [-4.5, 2.9, 0.26, 1.1, 0.08, 0.02]];
const LUC_SP = [[3.9, 0.6, 0.8, -0.6, 0.2, 0.2, 2.5], [3.3, 1.0, 1.3, -0.5, 0.35, 0.3, 2.55], [-2.4, 1.0, 1.3, -0.5, 0.35, 0.3, 2.55], [-3.5, 0.55, 0.8, -0.6, 0.2, 0.2, 2.4]];
const LUC_PIV = [2.95, 0.9, 1.3];   // pylon hinge (|x|, y, z): the nacelles swivel about X through it
const NAC_R = ['dark', 'metal', 'metal', 'metal', 'S', 'hull2', 'hull2', 'hull2', 'T', 'hull2', 'hull2', 'hull2', 'paint2', 'hull2', 'metal', 'metal', 'hull2', 'metal', 'metal', 'dark', 'dark', 'dark', 'dark'];
function lucNacelle(b, sx) {
  const e = CLS.lucciola.eng[sx < 0 ? 0 : 1], x = e[0], y = e[1], z0 = e[2] - 6.6, A = [x, y, z0], B = [x, y, z0 + 1];
  // shell: intake lip, cowl band, grooves, main body (cream over olive), aft body, nozzle collar and petals
  b.lathe(A, B, [[0.92, 0.95], [0.98, 0.5], [1.1, 0.12], [1.27, 0], [1.37, 0.2], [1.37, 0.95], [1.31, 1.01], [1.31, 1.11], [1.38, 1.17], [1.38, 3.3],
    [1.32, 3.36], [1.32, 3.46], [1.36, 3.52], [1.36, 4.7], [1.25, 5.15], [1.22, 5.55], [1.28, 5.6], [1.28, 5.85], [1.18, 5.9], [1.14, 6.6], [0.98, 6.6], [0.88, 6.15], [0.54, 5.85], [0, 5.8]], 22, 'hull',
  { roleAt: (j, i, d) => { const r = NAC_R[j]; return r === 'S' ? (d[1] > 0.1 ? 'stripe' : 'paint2') : r === 'T' ? (d[1] > -0.3 ? 'hull' : 'paint2') : r; } });
  // turbine intake: spinner, fan blades, back wall and a glow ring that follows the throttle
  b.lathe(A, B, [[0, 0.3], [0.17, 0.42], [0.3, 0.68], [0.34, 0.98], [0, 0.98]], 12, 'metal', { roles: { 1: 'hull2' } });
  b.lathe(A, B, [[0, 1.0], [0.95, 1.0]], 18, 'dark');
  b.lathe(A, B, [[0.36, 0.99], [0.62, 0.99]], 18, 'engineDim');
  for (let k = 0; k < 13; k++) { const a = k / 13 * TAU; b.at([x + Math.cos(a) * 0.63, y + Math.sin(a) * 0.63, z0 + 0.78], [0, 0.62, a - Math.PI / 2]).box(0, 0, 0, 0.24, 0.6, 0.04, 'metal').reset(); }
  // exhaust: petals around the nozzle, hot cone inside
  for (let k = 0; k < 14; k++) { const a = (k + 0.5) / 14 * TAU; b.at([x + Math.cos(a) * 1.185, y + Math.sin(a) * 1.185, z0 + 6.26], [0.06, 0, a - Math.PI / 2]).box(0, 0, 0, 0.44, 0.05, 0.72, 'metal').reset(); }
  b.nozzleCore(x, y, z0 + 5.81, 1.0, 20);
  // the faded courier stripe band, access panels, conduit, lamps, vents
  b.lathe(A, B, [[1.36, 3.75], [1.395, 3.77], [1.395, 4.3], [1.36, 4.32]], 12, 'stripe', { phase: -Math.PI / 2, arc: Math.PI });
  b.frame([x, y + 1.395, z0 + 4.55], [0, 0, 0], () => b.vent(0.7, 0.9, 4));
  b.frame([x + sx * 1.38, y + 0.15, z0 + 2.2], [0, 0, sx * -Math.PI / 2], () => b.hatch(0.8, 1.5, 'hull', 'hull2', 0.04));
  b.cyl([x + sx * 1.3, y - 0.45, z0 + 1.2], [x + sx * 1.3, y - 0.45, z0 + 4.8], 0.11, 0.11, 6, 'hull2');
  b.cyl([x + sx * 1.18, y - 0.75, z0 + 1.6], [x + sx * 1.18, y - 0.75, z0 + 4.4], 0.08, 0.08, 6, 'metal');
  b.lights([[x, y + 1.42, z0 + 1.35]], 0.17, 'light');
  b.lights([[x + sx * 1.42, y + 0.25, z0 + 1.45]], 0.15, sx < 0 ? 'navR' : 'navG');
  b.box(x, y + 1.41, z0 + 2.2, 0.1, 0.05, 1.1, 'accent');
  // pylon from the hinge to the nacelle, with its hinge knuckle and an actuator
  const px = LUC_PIV[0], py = LUC_PIV[1], pz = LUC_PIV[2];
  b.bbox(sx * (px + 0.33), py, pz, 0.66, 1.0, 2.5, 'paint2', { ch: 0.15, fy: 0.75 });
  b.cyl([sx * px, py, pz], [sx * (px + 0.3), py, pz], 0.46, 0.46, 14, 'metal');
  b.cyl([sx * (px + 0.15), py + 0.55, pz - 0.9], [sx * (px + 0.9), py + 1.0, pz - 1.3], 0.09, 0.09, 6, 'metal');
}
// section tables [z, w, h, y, ct, cb, x] and lathe profiles [r, t] of the ship classes
const LAN = [[7.0, 1.9, 1.9, 0.05, 0.55], [4.6, 2.2, 2.1, 0.08, 0.64, 0.6], [0.8, 2.25, 2.2, 0.1, 0.66, 0.62], [-2.6, 1.95, 1.9, 0.05, 0.58, 0.56], [-5.4, 1.15, 1.1, -0.06, 0.34, 0.33], [-7.8, 0.42, 0.42, -0.12, 0.12], [-8.7, 0.08, 0.08, -0.14, 0.02]];
const LAN_CAN = [[-0.2, 0.7, 0.5, 1.1, 0.22, 0.05], [-1.4, 1.05, 0.95, 1.15, 0.38, 0.05], [-3.3, 1.0, 0.85, 1.05, 0.34, 0.05], [-4.3, 0.5, 0.3, 0.72, 0.12, 0.05]];
const BAS = [[5.6, 3.0, 2.0, 0.0, 0.6, 0.5], [3.0, 3.6, 2.4, 0.05, 0.75, 0.55], [-2.5, 3.6, 2.4, 0.0, 0.75, 0.55], [-5.0, 2.8, 1.8, -0.15, 0.6, 0.45], [-6.4, 1.8, 1.0, -0.3, 0.35, 0.3]];
const BAS_POD = [[5.4, 1.9, 1.9, 0, 0.55, 0.55, 4.7], [-3.5, 2.0, 2.0, 0, 0.6, 0.6, 4.7], [-5.6, 1.5, 1.5, -0.03, 0.42, 0.42, 4.7], [-6.4, 1.2, 1.2, -0.05, 0.32, 0.32, 4.7]];
const SCR = [[3.0, 1.5, 1.4, 0, 0.45, 0.45], [0.5, 2.1, 1.9, 0, 0.6, 0.6], [-2.2, 2.0, 1.8, -0.05, 0.6, 0.55], [-3.6, 1.5, 1.4, -0.1, 0.45, 0.45], [-4.4, 0.8, 0.8, -0.15, 0.25, 0.25]];
const HAR = [[5.4, 2.6, 2.0, 0.0, 0.7, 0.5], [2.5, 2.8, 2.2, 0.05, 0.75, 0.55], [-2.0, 2.6, 2.0, 0.0, 0.7, 0.5], [-3.6, 2.0, 1.4, -0.05, 0.5, 0.4]];
const HAR_PR = [[-2.6, 1.1, 1.3, 0, 0.32, 0.32, 1.2], [-6.0, 0.9, 1.1, 0, 0.26, 0.26, 1.3], [-8.4, 0.55, 0.7, 0, 0.16, 0.16, 1.2], [-9.2, 0.16, 0.26, 0, 0.05, 0.05, 1.0]];
const GUT = [[4.8, 3.3, 3.3, 0, 1.0, 1.0], [0.8, 3.3, 3.3, 0, 1.0, 1.0], [-3.0, 3.2, 3.2, 0, 0.95, 0.95]];
const VOT = [[0, 0], [0.3, 0.8], [0.62, 2.0], [0.82, 3.6], [0.88, 5.4], [0.98, 5.5], [0.98, 6.2], [0.86, 6.3], [0.86, 9.6], [0.95, 9.7], [0.95, 10.4], [0.78, 10.6], [0.72, 11.4], [0.75, 11.6]];
const CEN = [[0, 1.3], [0.5, 1.3], [0.95, 1.2], [1.55, 0.7], [1.7, 0.08], [2.2, 0.0], [2.5, 0.2], [2.55, 0.9], [2.48, 1.0], [2.48, 1.1], [2.6, 1.2], [2.6, 7.5], [2.5, 7.6], [2.5, 7.9], [2.6, 8.0], [2.6, 14.6], [2.45, 15.4], [2.2, 16.8], [1.6, 17.6], [0, 17.9]];
const CHOIR_RINGS = [[-12, 10.5, 10], [0, 13.5, 12], [12, 10.5, 10]];
const HAU_CAB = [[-30, 14, 11, 1, 1.6, 1.2], [-40, 13.2, 10.4, 0.6, 2.6, 1.2], [-46, 10, 7.0, 0.2, 2.2, 1.0]];
const WAR = [[62, 22, 16, 2, 5, 4], [50, 24, 20, 3, 6, 5], [20, 24, 22, 1.5, 6.5, 5], [-20, 22, 20, 0, 6, 5], [-45, 16, 15, -1, 4.5, 3.5], [-60, 9, 9, -1.5, 2.5, 2], [-66, 3, 4, -2, 0.8, 0.6]];
const WAR_SUP = [[56, 12, 5, 12.5, 2, 0.5], [22, 13, 4.2, 12.0, 2, 0.5], [-14, 10, 3.5, 10.6, 1.5, 0.5], [-44, 5, 2.4, 7.6, 1, 0.3]];
const WAR_KEEL = [[44, 7, 3.5, -9.5, 1, 1.2], [-36, 6, 3, -9.2, 1, 1]];
const REL = [[80, 30, 14, 0, 4, 3], [62, 36, 20, 1, 6, 4], [-30, 36, 20, 0, 6, 4], [-58, 26, 16, -1, 5, 3], [-74, 12, 10, -2, 3, 2], [-81, 4, 5, -2, 1, 0.8]];
const REL_NAVE = [[50, 15, 12, 16, 6.4, 0.3], [-14, 15, 12, 16, 6.4, 0.3]];
const REL_FORE = [[-16, 20, 6, 13, 1.5, 0.3], [-48, 12, 5, 11.5, 1.5, 0.3]];
const DESIGN = {
  lucciola(b, P) {
    const ckTop = (z) => secTop(LUC_CK, z);
    // hull: cream wedge over an olive belly; raised cockpit brow; side sponsons
    b.hull(LUC, 'hull', { roleAt: (i, k) => (k === 0 || k === 1 || k === 7 ? 'paint2' : null), capRole0: 'hull2', capRole1: 'paint2' });
    b.hull(LUC_CK, 'hull', { roleAt: (i, k) => (k === 3 || k === 5 ? 'paint2' : null) });
    // windscreen row on the brow slope, a lit header strip and the overhanging visor
    for (let k = 0; k < 4; k++) { const u0 = 0.06 + k * 0.225; b.skin(LUC_CK, 4, u0, u0 + 0.2, -4.4, -3.62, 0.025, 'glass'); }
    b.skin(LUC_CK, 4, 0.05, 0.95, -3.64, -3.56, 0.035, 'winDim');
    b.bbox(0, 2.37, -3.6, 3.05, 0.14, 0.62, 'hull2', { ch: 0.05, fx: 0.94 });
    for (const k of [3, 5]) { b.skin(LUC_CK, k, 0.2, 0.8, -3.25, -2.45, 0.02, 'glass'); b.skin(LUC_CK, k, 0.2, 0.8, -2.25, -1.45, 0.02, 'glass'); b.skin(LUC_CK, k, 0.05, 0.95, -0.9, 0.3, 0.04, 'hull'); }
    // roof: hatch, beacon, sensor dish, whip antennas
    b.frame([0, ckTop(-1.7), -1.7], [0, 0, 0], () => b.hatch(1.15, 1.0, 'paint2'));
    b.lights([[0, ckTop(-2.8) + 0.08, -2.8]], 0.16, 'light');
    b.cyl([1.0, ckTop(-0.9) - 0.05, -0.9], [1.0, ckTop(-0.9) + 0.45, -0.9], 0.07, 0.06, 6, 'metal');
    { const D = [1.0, ckTop(-0.9) + 0.5, -0.9], ax = [0.35, 0.75, -0.55], B = [D[0] + ax[0], D[1] + ax[1], D[2] + ax[2]];
      b.lathe(D, B, [[0, 0], [0.42, 0.14], [0.47, 0.18], [0, 0.06]], 14, 'hull', { roles: { 1: 'metal' } }); b.lathe(D, B, [[0.04, 0], [0.04, 0.42], [0.08, 0.42], [0, 0.5]], 6, 'metal'); }
    b.cyl([-0.75, ckTop(-0.4) - 0.05, -0.4], [-0.95, ckTop(-0.4) + 1.35, 0.35], 0.035, 0.015, 4, 'dark');
    b.cyl([-1.05, ckTop(-1.2) - 0.05, -1.2], [-1.2, ckTop(-1.2) + 0.85, -0.7], 0.03, 0.015, 4, 'dark');
    b.lights([[-0.95, ckTop(-0.4) + 1.36, 0.36]], 0.07, 'navR');
    // nose: the big cargo ramp (dark recess, treads, framed, hazard lip), chin guns, headlamps
    b.skin(LUC, 4, 0.1, 0.9, -5.86, -4.6, 0.02, 'dark');
    for (let k = 0; k < 5; k++) { const z = -5.72 + k * 0.24; b.skin(LUC, 4, 0.13, 0.87, z, z + 0.08, 0.055, 'metal'); }
    b.skin(LUC, 4, 0.03, 0.1, -6.05, -4.45, 0.13, 'hull2'); b.skin(LUC, 4, 0.9, 0.97, -6.05, -4.45, 0.13, 'hull2');
    b.skin(LUC, 4, 0.03, 0.97, -4.62, -4.45, 0.13, 'hull2');
    b.skinHazard(LUC, 4, 0.1, 0.9, -6.05, -5.88, 0.05, 9);
    for (const k of [3, 5]) b.skinHazard(LUC, k, 0.15, 0.85, -6.0, -5.2, 0.04, 3);
    for (const g of CLS.lucciola.guns) { b.bbox(g[0], g[1], -6.0, 0.64, 0.56, 0.45, 'hull2', { ch: 0.1 }); b.gun(g, 2.0, 0.14); }
    b.box(-0.75, -0.48, -6.07, 0.5, 0.13, 0.04, 'window'); b.box(0.75, -0.48, -6.07, 0.5, 0.13, 0.04, 'window');
    b.bbox(0, -1.18, -6.0, 2.2, 0.22, 0.3, 'hull2', { ch: 0.06 });
    // faded courier stripe in worn segments along both flanks + chamfer armour plates
    for (const [z0, z1] of [[-5.5, -3.3], [-3.1, 0.3], [0.55, 3.4], [3.6, 5.15]]) { b.skin(LUC, 2, 0.78, 0.96, z0, z1, 0.03, 'stripe'); b.skin(LUC, 6, 0.04, 0.22, z0, z1, 0.03, 'stripe'); }
    for (const k of [3, 5]) { b.skin(LUC, k, 0.08, 0.92, -4.2, -0.7, 0.04, 'paint2'); b.skin(LUC, k, 0.08, 0.6, -0.5, 1.4, 0.04, 'hull'); }
    // dorsal deck: round top hatch, vents, the big rear cargo hatch (framed, ribbed, hazard edges)
    b.lathe([0, secTop(LUC, 1.25) - 0.02, 1.25], [0, secTop(LUC, 1.25) + 1, 1.25], [[0.52, 0], [0.52, 0.07], [0.44, 0.1], [0.44, 0.05], [0, 0.05]], 18, 'hull2', { roles: { 3: 'paint2' } });
    b.sym(() => b.frame([1.15, secTop(LUC, 1.2) - 0.01, 1.2], [0, 0, 0], () => b.vent(0.55, 1.0, 5)));
    b.skin(LUC, 4, 0.14, 0.86, 1.95, 4.85, 0.025, 'paint2');
    for (let k = 0; k < 6; k++) b.skin(LUC, 4, 0.17, 0.83, 2.15 + k * 0.45, 2.24 + k * 0.45, 0.065, 'hull2');
    b.skin(LUC, 4, 0.07, 0.14, 1.78, 5.02, 0.09, 'hull2'); b.skin(LUC, 4, 0.86, 0.93, 1.78, 5.02, 0.09, 'hull2');
    b.skinHazard(LUC, 4, 0.14, 0.86, 1.78, 1.95, 0.05, 9); b.skinHazard(LUC, 4, 0.14, 0.86, 4.85, 5.02, 0.05, 9);
    b.lights([[-1.3, secTop(LUC, 5.05) + 0.1, 5.05], [1.3, secTop(LUC, 5.05) + 0.1, 5.05]], 0.12, 'window');
    // heat-sink fins on the rear top chamfers
    for (const k of [3, 5]) for (let i = 0; i < 8; i++) { const z = 3.2 + i * 0.24; b.skin(LUC, k, 0.22, 0.78, z, z + 0.05, 0.28, 'metal'); }
    // sponsons with the stripe, flank vents, landing lamp, aft vectoring port; hinge barrels for the pylons
    b.sym(() => {
      b.hull(LUC_SP, 'paint2', { roleAt: (i, k) => (k === 3 || k === 4 ? 'hull' : null) });
      b.skin(LUC_SP, 2, 0.58, 0.8, -2.2, 3.2, 0.03, 'stripe');
      b.frame([3.05, -0.78, 0.9], [0, 0, -Math.PI / 2], () => b.vent(0.36, 1.7, 7));
      b.lights([[2.55, -0.55, -3.55]], 0.15, 'window');
      b.lathe([2.55, -0.58, 3.85], [2.55, -0.58, 4.85], [[0, 0], [0.24, 0], [0.24, 0.16], [0.17, 0.16], [0.17, 0.06], [0, 0.06]], 10, 'metal', { roles: { 4: 'engineDim' } });
      b.cyl([2.1, LUC_PIV[1], LUC_PIV[2]], [LUC_PIV[0], LUC_PIV[1], LUC_PIV[2]], 0.52, 0.52, 14, 'hull2');
      b.cyl([2.55, LUC_PIV[1], LUC_PIV[2]], [2.68, LUC_PIV[1], LUC_PIV[2]], 0.6, 0.6, 14, 'metal');
      // folded landing skids, belly plates
      b.bbox(1.6, -1.53, -0.2, 0.24, 0.14, 5.4, 'metal', { ch: 0.05 });
      for (const zz of [-1.9, 1.6]) b.bbox(1.6, -1.45, zz, 0.36, 0.12, 0.9, 'hull2', { ch: 0.03 });
    });
    b.skin(LUC, 0, 0.3, 0.7, -2.4, 1.0, 0.03, 'hull2'); b.skin(LUC, 0, 0.36, 0.64, 1.4, 3.8, 0.03, 'paint2');
    b.lights([[0, secBot(LUC, 3.0) - 0.08, 3.0]], 0.14, 'navR');
    // rear face: cargo door with frame and hazard sill, tail lamps, RCS blocks
    const zr = 5.75;
    b.box(0, 0.22, zr + 0.02, 2.0, 1.0, 0.04, 'paint2');
    b.box(0, 0.75, zr + 0.04, 2.2, 0.08, 0.08, 'hull2'); b.box(-1.06, 0.22, zr + 0.04, 0.09, 1.1, 0.08, 'hull2'); b.box(1.06, 0.22, zr + 0.04, 0.09, 1.1, 0.08, 'hull2');
    b.frame([0, -0.38, zr + 0.01], [Math.PI / 2, 0, 0], () => b.hazard(2.2, 0.2, 7, 0.04));
    b.box(-1.5, 0.75, zr + 0.03, 0.28, 0.16, 0.06, 'navR'); b.box(1.5, 0.75, zr + 0.03, 0.28, 0.16, 0.06, 'navR');
    b.box(-1.45, -0.3, zr + 0.03, 0.3, 0.14, 0.06, 'window'); b.box(1.45, -0.3, zr + 0.03, 0.3, 0.14, 0.06, 'window');
    b.sym(() => { b.bbox(1.75, 0.95, 5.35, 0.42, 0.3, 0.55, 'hull2', { ch: 0.06 }); b.disc(1.75, 0.95, 5.63, 0.1, 'dark', 6, 1); });
    for (let i = 0; i < 4; i++) b.box(-0.6 + i * 0.4, 0.22, zr + 0.06, 0.06, 0.8, 0.04, 'hull2');
    // the swivelling nacelles (parts)
    for (const sx of [-1, 1]) P.part([sx * LUC_PIV[0], LUC_PIV[1], LUC_PIV[2]], sx, (pb) => lucNacelle(pb, sx));
  },
  lancer(b) {   // Gilda interceptor: an ivory needle with brass collars, royal-blue bands, forward-swept blades
    b.hull(LAN, 'hull', { roleAt: (i, k) => (k === 0 || k === 1 || k === 7 ? 'paint2' : null), capRole0: 'hull2' });
    for (const [z0, z1] of [[-4.7, -4.35], [0.3, 0.7], [3.3, 3.7], [6.2, 6.6]]) b.band(LAN, z0, z1, 0.05, 'hull2');
    for (const [k, u0, u1] of [[2, 0.55, 0.8], [6, 0.2, 0.45]]) { b.skin(LAN, k, u0, u1, -6.6, -4.8, 0.03, 'accent'); b.skin(LAN, k, u0, u1, -4.2, 0.2, 0.03, 'accent'); b.skin(LAN, k, u0, u1, 0.8, 3.2, 0.03, 'accent'); }
    b.hull(LAN_CAN, 'glass'); b.band(LAN_CAN, -3.35, -3.2, 0.03, 'hull2'); b.band(LAN_CAN, -1.5, -1.35, 0.03, 'hull2'); b.skin(LAN_CAN, 4, 0.44, 0.56, -4.2, -0.3, 0.04, 'hull2');
    b.engine(0, 0.05, 7.9, 1.05, 'hull2');
    b.sym(() => {
      const W = [[0.7, 2.0], [5.6, -1.9], [6.05, -0.95], [0.8, 4.9]];
      b.plate(W, -0.15, 0.16, 'hull', 'hull2');
      b.plate([[1.2, 2.1], [5.15, -1.35], [5.45, -0.85], [1.3, 4.2]], -0.05, 0.04, 'paint2');
      b.plate([[1.35, 3.5], [5.2, -0.65], [5.45, -0.35], [1.4, 4.1]], -0.03, 0.04, 'accent');
      b.frame([3.2, -0.01, 1.0], [0, 0, 0], () => b.insignia(F_GILDA, 0.48));
      b.lathe([5.85, -0.15, -2.5], [5.85, -0.15, -1.5], [[0, 0], [0.16, 0.12], [0.26, 0.6], [0.3, 1.2], [0.3, 2.3], [0.22, 2.6], [0, 2.6]], 10, 'hull', { roles: { 0: 'engineDim', 3: 'hull2' } });
      b.lights([[5.85, 0.18, -0.9]], 0.14, 'light');
      // side intake: dark mouth framed in brass
      b.skin(LAN, 2, 0.15, 0.55, 0.9, 2.6, 0.02, 'dark'); b.skin(LAN, 2, 0.1, 0.15, 0.8, 2.7, 0.06, 'hull2'); b.skin(LAN, 2, 0.55, 0.6, 0.8, 2.7, 0.06, 'hull2');
      b.frame([0.62, 0.95, 4.4], [0, 0, -0.75], () => b.vent(0.36, 1.4, 5));
    });
    b.lights([[-5.85, -0.42, -0.9]], 0.12, 'navR'); b.lights([[5.85, -0.42, -0.9]], 0.12, 'navG');
    b.at([0, 0.95, 0], [0, 0, Math.PI / 2]).plate([[0, 3.4], [1.85, 5.5], [2.0, 6.7], [0, 6.9]], 0, 0.12, 'hull', 'hull2').reset();
    b.at([0, 0.95, 0], [0, 0, Math.PI / 2]).plate([[0.55, 4.25], [1.35, 5.15], [1.42, 5.7], [0.55, 5.0]], 0, 0.15, 'accent').reset();
    b.lights([[0, 2.95, 6.75]], 0.12, 'light');
    for (const g of CLS.lancer.guns) {
      b.box(g[0] * 0.62, g[1] + 0.05, -4.2, Math.abs(g[0]) * 0.8, 0.12, 1.4, 'hull2');
      b.lathe([g[0], g[1], -5.6], [g[0], g[1], -4.6], [[0, 0], [0.16, 0.25], [0.21, 0.8], [0.21, 2.3], [0.15, 2.6], [0, 2.6]], 8, 'hull', { roles: { 1: 'hull2', 3: 'metal' } });
      b.gun(g, 2.8, 0.1);
    }
    b.lights([[0, -1.0, -1.2], [0, -0.95, 2.6]], 0.15, 'light');
  },
  bastion(b) {   // Gilda heavy fighter: an armoured core and two engine pods joined in an H, twin heavy cannons
    b.hull(BAS, 'hull', { roleAt: (i, k) => (k === 0 || k === 1 || k === 7 ? 'paint2' : null), capRole0: 'hull2' });
    for (const k of [4, 3, 5]) b.skin(BAS, k, 0.08, 0.92, -6.2, -4.3, 0.025, 'glass');
    b.band(BAS, -4.35, -4.1, 0.05, 'hull2'); b.band(BAS, 2.7, 3.1, 0.06, 'hull2');
    b.skin(BAS, 4, 0.2, 0.8, -3.6, -1.2, 0.04, 'paint2'); b.skin(BAS, 4, 0.42, 0.58, -3.8, 5.4, 0.06, 'accent');
    b.sym(() => {
      b.hull(BAS_POD, 'hull', { roleAt: (i, k) => (k === 0 || k === 1 || k === 7 ? 'paint2' : null), capRole1: 'dark' });
      b.lathe([4.7, 0, -6.45], [4.7, 0, -5.45], [[0.72, 0.4], [0.62, 0.05], [0.0, 0.05]], 14, 'dark', { roles: { 1: 'engineDim' } });
      b.ring([4.7, 0, -6.5], [4.7, 0, -5.5], 0.62, 0.78, 0, 0.18, 14, 'hull2');
      b.engine(4.7, 0, 6.6, 0.95, 'hull2');
      for (const [z0, z1] of [[-4.0, -3.7], [0.9, 1.3], [4.4, 4.8]]) b.band(BAS_POD, z0, z1, 0.05, 'hull2');
      b.skin(BAS_POD, 3, 0.25, 0.75, -3.4, 4.2, 0.035, 'accent'); b.skin(BAS_POD, 2, 0.3, 0.7, -3.4, 0.6, 0.03, 'paint2');
      b.bbox(2.75, 0, 0.7, 2.4, 0.7, 4.4, 'hull', { ch: 0.15, fx: 0.9, roles: { bottom: 'paint2' } });
      b.frame([2.75, 0.35, 0.9], [0, 0, 0], () => b.insignia(F_GILDA, 0.55));
      // heavy cannon under the crossbar: shroud, recoil sleeve, barrel
      b.bbox(3.3, -0.62, -0.6, 0.62, 0.62, 3.4, 'hull2', { ch: 0.12, fx: 0.8, fy: 0.8 });
      b.lathe([3.3, -0.75, -3.3], [3.3, -0.75, -2.3], [[0, 0], [0.3, 0.1], [0.3, 1.2], [0.24, 1.3], [0, 1.3]], 10, 'metal');
      b.at([4.7, 0.95, 0], [0, 0, Math.PI / 2]).plate([[0, 0.6], [2.0, 3.4], [2.15, 4.3], [0, 4.4]], 0, 0.14, 'hull', 'hull2').reset();
      b.at([4.7, 0.95, 0], [0, 0, Math.PI / 2]).plate([[0.7, 1.7], [1.5, 2.9], [1.6, 3.4], [0.7, 2.4]], 0, 0.17, 'accent').reset();
      b.lights([[5.75, 0.0, -3.8], [5.75, 0.0, 3.0]], 0.16, 'light');
      b.frame([1.15, secTop(BAS, 3.9) - 0.02, 3.9], [0, 0, 0], () => b.vent(0.7, 1.6, 6));
    });
    for (const g of CLS.bastion.guns) b.gun(g, 4.2, 0.22);
    b.lights([[-5.7, 0.4, 5.5]], 0.15, 'navR'); b.lights([[5.7, 0.4, 5.5]], 0.15, 'navG');
    // dorsal turret: armoured dome with twin barrels
    b.lathe([0, 1.15, 0.6], [0, 2.15, 0.6], [[1.25, 0], [1.25, 0.2], [1.05, 0.55], [0.6, 0.8], [0, 0.85]], 12, 'hull2', { flat: true });
    for (const sx of [-0.32, 0.32]) b.gun([sx, 1.62, -2.4], 2.6, 0.1);
    b.greebles(6, -1.2, 1.2, secTop(BAS, 4) - 0.02, 3.2, 4.8, 0.35);
  },
  scrapwing(b, P) {   // Relitti swarm fighter: shark-mouthed pod, one big patched wing, one strut, one roaring engine
    b.hull(SCR, 'hull', { roleAt: (i, k) => (i >= 2 ? 'accent' : k === 0 ? 'hull2' : null), capRole1: 'accent', capRole0: 'hull2' });
    // the shark mouth: a dark maw round the lower nose with two rows of teeth
    for (const k of [1, 0, 7]) b.skin(SCR, k, 0, 1, -4.25, -2.6, 0.02, 'dark');
    b.skin(SCR, 2, 0, 0.42, -4.1, -2.6, 0.02, 'dark', { shear: 0 }); b.skin(SCR, 6, 0.58, 1, -4.1, -2.6, 0.02, 'dark');
    b.sym(() => { for (let i = 0; i < 6; i++) { const z = -3.95 + i * 0.24, s = secAt(SCR, z), x = s[1] / 2 + 0.05, lo = s[3] - s[2] / 2 + s[5], span = s[2] - s[4] - s[5], yT = lo + span * 0.42, yB = lo + span * 0.03;
      b.cyl([x, yT, z], [x + 0.02, yT - 0.3, z - 0.03], 0.085, 0.0, 3, 'bone', false); if (i < 5) b.cyl([x, yB, z + 0.12], [x + 0.02, yB + 0.26, z + 0.1], 0.075, 0.0, 3, 'bone', false); } });
    b.skin(SCR, 4, 0.12, 0.88, -2.5, -1.0, 0.03, 'glass'); b.skin(SCR, 3, 0.1, 0.9, -2.5, -1.0, 0.03, 'glass'); b.skin(SCR, 5, 0.1, 0.9, -2.5, -1.0, 0.03, 'glass');
    b.band(SCR, -2.62, -2.48, 0.04, 'metal'); b.band(SCR, -1.05, -0.9, 0.04, 'metal');
    // the big wing on the right: patched plates, a hazard tip and a stripe of welds
    b.plate([[0.9, -1.4], [5.6, 0.6], [5.4, 2.6], [0.9, 2.2]], 0, 0.22, 'hull', 'hull2');
    b.at([0, 0, 0], [0, 0.04, 0]).plate([[2.0, -0.4], [3.6, 0.25], [3.5, 1.4], [2.0, 1.4]], 0.14, 0.06, 'patch').reset();
    b.at([0, 0, 0], [0, -0.05, 0]).plate([[3.7, 0.5], [5.0, 1.0], [4.9, 2.1], [3.7, 2.0]], 0.13, 0.05, 'patch').reset();
    b.plate([[1.1, 0.7], [2.2, 1.6], [2.2, 2.05], [1.1, 1.9]], 0.13, 0.05, 'patch');
    b.plate([[1.2, -0.5], [1.8, -0.3], [1.8, 2.0], [1.2, 2.0]], -0.13, 0.05, 'paint2');
    b.frame([5.1, 0.12, 1.6], [0, Math.PI / 2 + 0.38, 0], () => b.hazard(1.7, 0.5, 5, 0.03));
    for (let i = 0; i < 7; i++) b.box(1.3 + i * 0.6, 0.12, -0.6 + i * 0.26 + 0.9, 0.08, 0.04, 0.08, 'metal');
    // the strut on the left: a boom with a sensor pod; a stub wing
    b.cyl([-0.9, -0.1, 1.3], [-3.1, -0.1, 0.6], 0.16, 0.13, 6, 'metal');
    b.lathe([-3.1, -0.1, -1.0], [-3.1, -0.1, 0.0], [[0, 0], [0.18, 0.3], [0.28, 0.9], [0.28, 1.8], [0.2, 2.1], [0, 2.2]], 8, 'hull2', { roles: { 2: 'accent' } });
    b.plate(mir([[0.7, 0.2], [2.8, 1.1], [2.8, 1.8], [0.7, 1.6]]), -0.1, 0.12, 'patch', 'hull2');
    // exposed engine: casing, pipes, exhaust stacks
    b.lathe([-0.45, 0.1, 0.6], [-0.45, 0.1, 1.6], [[0, 0], [0.55, 0.2], [0.7, 0.6], [0.72, 2.6], [0.66, 2.7], [0.66, 2.9], [0.74, 3.0], [0.74, 3.8], [0, 3.8]], 12, 'hull2', { roles: { 1: 'metal', 3: 'metal', 4: 'metal' } });
    b.engine(-0.45, 0.1, 5.0, 0.68, 'metal');
    for (let i = 0; i < 4; i++) { const a = 0.6 + i * 0.7; b.cyl([-0.45 + Math.cos(a) * 0.78, 0.1 + Math.sin(a) * 0.78, 0.9], [-0.45 + Math.cos(a + 0.5) * 0.8, 0.1 + Math.sin(a + 0.5) * 0.8, 4.1], 0.06, 0.06, 4, i % 2 ? 'metal' : 'dark'); }
    for (const x of [0.35, 0.75]) { b.cyl([x, 0.7, 2.2], [x + 0.05, 1.5, 2.5], 0.12, 0.1, 6, 'metal'); b.ring([x + 0.05, 1.5, 2.5], [x + 0.06, 2.5, 2.8], 0.06, 0.12, 0, 0.06, 6, 'dark'); }
    b.box(0.6, -0.8, 0.5, 0.5, 0.4, 2, 'patch');
    for (const g of CLS.scrapwing.guns) { b.bbox(g[0], g[1], -3.4, 0.42, 0.4, 1.6, 'hull2', { ch: 0.08 }); b.gun(g, 2.4, 0.13); }
    b.lights([[5.5, 0, 1.6]], 0.18, 'navG'); b.lights([[-3.1, -0.1, -1.05]], 0.16, 'navR'); b.lights([[0, 1.05, 0.5]], 0.16, 'light');
  },
  harpoon(b) {   // Relitti tether ship: forked bow, a glowing emitter caged between the prongs, twin engines
    b.hull(HAR, 'hull', { roleAt: (i, k) => (k === 0 || k === 1 || k === 7 ? 'hull2' : null), capRole0: 'hull2' });
    b.sym(() => {
      b.hull(HAR_PR, 'hull2', { roleAt: (i, k) => (k === 3 || k === 4 ? 'hull' : null) });
      b.skinHazard(HAR_PR, 4, 0.0, 1.0, -9.0, -7.6, 0.03, 4); b.skinHazard(HAR_PR, 2, 0.0, 1.0, -9.0, -7.6, 0.03, 4);
      b.cyl([1.9, 0, 2.6], [1.9, 0, 5.8], 0.62, 0.7, 12, 'hull2', false);
      b.engine(1.9, 0, 6.5, 0.72, 'metal');
      b.cyl([1.0, 0.2, -7.9], [0.45, 0.05, -8.3], 0.09, 0.09, 4, 'metal');
      b.skin(HAR, 2, 0.2, 0.8, -1.5, 3.4, 0.05, 'patch'); b.skin(HAR, 3, 0.2, 0.9, 1.0, 4.6, 0.04, 'patch');
      b.frame([1.4, -0.2, 2.0], [0, 0, -Math.PI / 2], () => b.vent(0.5, 1.6, 6));
      b.lights([[1.9, 0.75, 4.2]], 0.16, 'light');
    });
    // the emitter: glowing core in coil rings, the tether spool running back into the hull
    b.band(HAR, 0.4, 0.8, 0.05, 'metal');
    b.ell(0, 0, -8.35, 0.42, 0.42, 0.42, 10, 8, 'core');
    for (const z of [-8.6, -8.1]) b.torus(0, 0, z, 0.62, 0.07, 14, 4, 'metal', 'z');
    b.cyl([0, 0, -7.7], [0, 0, -3.4], 0.2, 0.32, 8, 'dark');
    for (let i = 0; i < 5; i++) b.torus(0, 0, -7.0 + i * 0.75, 0.3, 0.05, 8, 3, 'accent', 'z');
    b.skin(HAR, 4, 0.15, 0.85, -3.3, -1.4, 0.03, 'glass'); b.skin(HAR, 3, 0.15, 0.85, -3.3, -1.4, 0.03, 'glass'); b.skin(HAR, 5, 0.15, 0.85, -3.3, -1.4, 0.03, 'glass');
    b.skinHazard(HAR, 4, 0.1, 0.9, -1.2, -0.85, 0.04, 6);
    for (const g of CLS.harpoon.guns) { b.bbox(g[0], g[1] + 0.05, -2.6, 0.75, 0.6, 1.5, 'hull2', { ch: 0.12 }); b.gun(g, 3.8, 0.15); }
    b.greebles(6, -0.8, 0.8, secTop(HAR, 3.0) - 0.02, 1.6, 4.6, 0.32);
    b.lights([[0, -1.1, 3.0]], 0.16, 'light'); b.lights([[-1.45, 0.4, -3.4]], 0.13, 'navR'); b.lights([[1.45, 0.4, -3.4]], 0.13, 'navG');
  },
  gutter(b) {   // Relitti boarding craft: armoured can, twisted drill nose, three grapple claws
    b.hull(GUT, 'hull', { roleAt: (i, k) => (k % 2 ? 'hull2' : null), capRole0: 'hull2' });
    for (const [z0, z1] of [[-2.9, -2.4], [0.6, 1.0], [3.6, 4.0]]) b.band(GUT, z0, z1, 0.07, 'metal');
    b.skin(GUT, 2, 0.15, 0.85, -1.8, 0.2, 0.05, 'patch'); b.skin(GUT, 5, 0.1, 0.9, 1.4, 3.2, 0.05, 'patch'); b.skin(GUT, 0, 0.2, 0.8, -1.0, 2.4, 0.05, 'patch');
    // drill: stacked twisted frustums with a hardened tip
    for (let i = 0; i < 6; i++) { const r0 = 1.42 * (1 - i / 6 * 0.88), r1 = 1.42 * (1 - (i + 1) / 6 * 0.88), z0 = -3.1 - i / 6 * 4.3, L = 4.3 / 6; b.lathe([0, 0, z0 + 0.1], [0, 0, z0 - 1], [[r0 * 0.96, 0], [r1 + 0.12, L * 0.55 + 0.1], [r1, L + 0.1]], 8, i % 2 ? 'metal' : 'dark', { flat: true, phase: i * 0.32 }); }
    b.cyl([0, 0, -7.4], [0, 0, -7.9], 0.18, 0.0, 6, 'metal', false);
    b.ring([0, 0, -3.1], [0, 0, -4.1], 1.3, 1.62, 0, 0.3, 16, 'hazard', { roleAt: (j, i) => (i % 2 ? 'hazard' : 'dark') });
    for (let i = 0; i < 3; i++) {
      const a = i / 3 * TAU + 0.5;
      b.frame([Math.cos(a) * 1.5, Math.sin(a) * 1.5, -2.6], [0, 0, a - Math.PI / 2], () => {
        b.at([0, 0.15, -0.9], [0.28, 0, 0]).bbox(0, 0, 0, 0.36, 0.34, 2.2, 'accent', { ch: 0.06 }).reset();
        b.at([0, 0.62, -2.7], [-0.45, 0, 0]).bbox(0, 0, 0, 0.3, 0.3, 1.8, 'hull2', { ch: 0.05, fx: 0.4, fy: 0.5 }).reset();
        b.at([0, 0.15, -1.9], [0, 0, Math.PI / 2]).cyl([0, -0.25, 0], [0, 0.25, 0], 0.2, 0.2, 8, 'metal').reset();
      });
    }
    for (const sx of [-1, 1]) { b.cyl([sx * 1.6, 0, 4.0], [sx * 1.6, 0, 5.2], 0.66, 0.68, 12, 'hull2', false); b.engine(sx * 1.6, 0, 5.9, 0.62, 'metal'); }
    b.skin(GUT, 4, 0.12, 0.88, -1.2, 0.8, 0.12, 'hull2'); b.skin(GUT, 4, 0.2, 0.8, -1.05, 0.65, 0.16, 'glass');
    b.frame([0, secTop(GUT, 2.4) - 0.01, 2.4], [0, 0, 0], () => b.hatch(1.1, 1.1, 'paint2', 'hazard'));
    b.lights([[1.75, 0, 3], [-1.75, 0, 3], [0, -1.75, 2]], 0.2, 'light');
  },
  votive(b) {   // Custodi light fighter: a white-stone spindle in a halo of glyph-lit stone, gold trims
    b.lathe([0, 0, -6.6], [0, 0, -5.6], VOT, 12, 'hull', { roles: { 4: 'trim', 5: 'trim', 6: 'trim', 7: 'hull2', 8: 'trim', 9: 'trim', 10: 'trim' } });
    b.engine(0, 0, 6.1, 0.76, 'trim');
    b.ell(0, 0.72, -1.7, 0.45, 0.42, 1.25, 12, 8, 'glass', 1);
    b.box(0, 1.15, -1.7, 0.05, 0.05, 2.0, 'trim');
    // the halo: a chamfered stone band with gold faces and glowing glyphs, four verdigris vanes
    b.lathe([0, 0, 1.2], [0, 0, 2.2], [[4.0, -0.22], [4.12, -0.34], [4.48, -0.34], [4.6, -0.22], [4.6, 0.22], [4.48, 0.34], [4.12, 0.34], [4.0, 0.22], [4.0, -0.22]], 40, 'hull', { roles: { 0: 'trim', 6: 'trim' } });
    for (let i = 0; i < 20; i++) { const a = i / 20 * TAU; b.at([Math.cos(a) * 4.3, Math.sin(a) * 4.3, 1.2 - 0.36], [0, 0, a]).box(0, 0, 0, 0.12, i % 2 ? 0.22 : 0.34, 0.03, 'glyph').reset(); }
    for (let i = 0; i < 4; i++) { const a = i / 4 * TAU + Math.PI / 4; b.at([0, 0, 1.2], [0, 0, a]).bbox(2.45, 0, 0, 3.4, 0.16, 0.9, 'hull2', { ch: 0.05, fx: 1, roles: { top: 'hull2' } }).box(2.45, 0, -0.47, 3.2, 0.06, 0.04, 'trim').reset(); }
    for (let i = 0; i < 8; i++) { const a = i / 8 * TAU; b.lights([[Math.cos(a) * 4.3, Math.sin(a) * 4.3, 1.58]], 0.2, 'lantern'); }
    b.sym(() => {
      b.plate([[0.25, -5.4], [1.55, -4.95], [1.55, -4.55], [0.3, -4.2]], 0, 0.1, 'hull', 'trim');
      b.lathe([1.45, 0, -5.0], [1.45, 0, -4.0], [[0, 0], [0.14, 0.2], [0.18, 0.6], [0.18, 1.5], [0, 1.6]], 8, 'trim');
    });
    for (const g of CLS.votive.guns) b.gun(g, 1.4, 0.09);
    b.frame([0, 0.87, 2.4], [0, 0, 0], () => b.insignia(F_CUSTODI, 0.3));
  },
  shard(b) {   // Echo drone: a glowing heart in a cage of black crystal, three crystal fins with lit seams, a spike
    b.ell(0, 0, -0.4, 0.72, 0.72, 0.9, 10, 8, 'glow');
    for (let i = 0; i < 6; i++) { const a = i / 6 * TAU + 0.3, c = Math.cos(a), s = Math.sin(a); b.crystal([c * 0.95, s * 0.95, -2.7], [c * 1.05, s * 1.05, 1.9], 0.34, 4, 'hull', { edge: i % 2 ? 'edge' : 'edge2', er: 0.022, phase: a, face2: 'crysA' }); }
    b.crystal([0, 0, -4.45], [0, 0, -1.6], 0.5, 5, 'hull2', { edge: 'edge', er: 0.025, k0: 0.6, k1: 0.8, face2: 'crysB' });
    for (let i = 0; i < 3; i++) {
      const a = i / 3 * TAU + Math.PI / 2, c = Math.cos(a), s = Math.sin(a);
      b.crystal([c * 0.9, s * 0.9, -2.4], [c * 4.3, s * 4.3, 1.5], 0.5, 4, 'hull', { edge: 'edge', er: 0.026, k0: 0.35, k1: 0.7, phase: a, face2: 'crysA' });
      b.crystal([c * 1.2, s * 1.2, 0.2], [c * 3.0, s * 3.0, 2.6], 0.28, 4, 'hull2', { edge: 'edge2', er: 0.02, phase: a + 0.4, face2: 'crysB' });
      b.ell(c * 4.35, s * 4.35, 1.55, 0.16, 0.16, 0.16, 6, 4, 'glow');
      b.edge([c * 1.0, s * 1.0, -0.4], [c * 3.4, s * 3.4, 0.8], 0.03, 'edge', 3);
    }
    b.crystal([0, 0, 3.75], [0, 0, 2.4], 0.4, 5, 'engine', {});
    for (const [p, q] of [[[2.6, 2.4, 1.0], [3.2, 2.9, 1.8]], [[-2.9, 1.9, -1.6], [-3.6, 2.2, -0.8]], [[0.5, -3.3, 1.8], [0.8, -3.9, 2.5]]]) b.crystal(p, q, 0.16, 4, 'hull', { edge: 'edge', er: 0.016, face2: 'crysA' });
  },
  censer(b) {   // Custodi bomber: a stout ivory hull with verdigris ornament, a gold intake lens, portholes, banners
    const A = [0, 0, -9.2], B = [0, 0, -8.2];
    b.lathe(A, B, CEN, 20, 'hull', { roles: { 0: 'lantern', 1: 'dark', 2: 'dark', 3: 'dark', 4: 'trim', 5: 'trim', 6: 'trim', 7: 'hull2', 8: 'hull2', 9: 'trim', 11: 'trim', 12: 'dark', 13: 'trim', 15: 'hull2', 16: 'hull2', 17: 'trim', 18: 'hull2' } });
    b.ring(A, B, 1.3, 1.6, 0.72, 0.82, 20, 'trim');
    // the torpedo launcher in the lens and the chin gun
    b.lathe([0, -1.4, -10.4], [0, -1.4, -9.4], [[0, 0.12], [0.24, 0.12], [0.24, 0], [0.36, 0], [0.38, 0.5], [0.4, 1.7], [0, 1.7]], 10, 'trim', { roles: { 0: 'glow', 1: 'dark', 5: 'hull2' } });
    b.bbox(0, -2.2, -8.4, 0.8, 0.6, 1.4, 'trim', { ch: 0.15 }); b.gun(CLS.censer.guns[0], 1.6, 0.16);
    // ornament: verdigris filigree plates (raised arcs) edged in gold; a glowing glyph girdle
    for (const [t0, t1, ph, arc] of [[1.5, 3.5, 0.5, 1.2], [1.5, 3.5, Math.PI + 0.5, 1.2], [8.3, 10.1, -0.4, 0.8], [8.3, 10.1, Math.PI - 0.4, 0.8], [13.2, 14.4, 0.9, 1.0], [13.2, 14.4, Math.PI - 1.9, 1.0], [6.3, 7.3, -0.5, 1.0]]) {
      b.lathe(A, B, [[2.6, t0], [2.68, t0 + 0.06], [2.68, t1 - 0.06], [2.6, t1]], 8, 'hull2', { phase: ph, arc });
      b.lathe(A, B, [[2.6, t0 - 0.08], [2.71, t0 - 0.04], [2.71, t0 + 0.04], [2.6, t0 + 0.08]], 8, 'trim', { phase: ph, arc });
    }
    b.lathe(A, B, [[2.5, 7.62], [2.54, 7.65], [2.54, 7.85], [2.5, 7.88]], 24, 'glyph', { roleAt: (j, i) => (i % 3 === 2 ? 'trim' : null) });
    // window houses: raised blisters with four gold-rimmed lit portholes each side
    b.sym(() => {
      for (const t0 of [3.8, 10.4]) {
        b.lathe(A, B, [[2.6, t0], [2.92, t0 + 0.3], [2.92, t0 + 2.2], [2.6, t0 + 2.5]], 10, 'hull', { phase: -Math.PI / 2 - 0.62, arc: 1.24, roles: { 0: 'trim', 2: 'trim' } });
        for (const [da, dt] of [[-0.26, 0.85], [0.26, 0.85], [-0.26, 1.65], [0.26, 1.65]]) { const a = -Math.PI / 2 + da, d = [-Math.sin(a), Math.cos(a), 0]; b.porthole([d[0] * 2.93, d[1] * 2.93, -9.2 + t0 + dt], d, 0.34); }
      }
      // engines with gilded housings, tail fins
      b.cyl([2.6, 0.2, 5.6], [2.6, 0.2, 8.2], 0.95, 1.12, 14, 'hull', false); b.engine(2.6, 0.2, 9.6, 1.05, 'trim');
      b.at([0, 0, 0], [0, 0, -Math.PI / 2 - 0.75]).plate([[2.2, 4.6], [4.4, 7.8], [4.5, 8.8], [2.2, 8.4]], 0, 0.14, 'hull', 'trim').reset();
      b.at([0, 0, 0], [0, 0, -Math.PI / 2 - 0.75]).lights([[4.55, 0, 8.65]], 0.13, 'lantern').reset();
    });
    b.at([0, 0, 0], [0, 0, Math.PI / 2]).plate([[2.4, 4.2], [4.6, 7.4], [4.7, 8.6], [2.4, 8.4]], 0, 0.16, 'hull', 'trim').reset();
    // top: a cupola with a lit band, banners on three masts
    b.lathe([0, 2.4, 2.4], [0, 3.4, 2.4], [[1.15, 0], [1.15, 0.35], [1.1, 0.42], [0.95, 0.75], [0.55, 1.05], [0.2, 1.15], [0.06, 1.5], [0, 1.5]], 14, 'hull2', { roles: { 0: 'hull', 1: 'trim', 5: 'trim', 6: 'trim' } });
    b.lathe([0, 2.4, 2.4], [0, 3.4, 2.4], [[1.17, 0.12], [1.17, 0.26]], 14, 'lantern');
    for (const [z, h] of [[-4.6, 3.4], [-0.6, 4.2], [5.6, 3.0]]) {
      b.cyl([0, 2.4, z], [0, 2.6 + h, z], 0.06, 0.045, 5, 'trim'); b.cyl([0, 2.5 + h, z - 0.05], [0, 2.5 + h, z + 0.9], 0.04, 0.04, 4, 'trim');
      b.at([0, 2.5 + h, z + 0.42], [0, 0, Math.PI / 2]).plate([[-1.6, -0.4], [0, -0.42], [0, 0.42], [-1.6, 0.4]], 0, 0.04, 'hull2', 'trim').reset();
      b.at([0, 2.5 + h - 0.8, z + 0.42], [0, 0, Math.PI / 2]).box(0, 0, 0, 0.3, 0.05, 0.3, 'glyph').reset();
      b.lights([[0, 2.65 + h, z]], 0.09, 'lantern');
    }
  },
  lattice(b) {   // Echo interceptor: a crystal lance of faceted prisms with lit seams and floating shards
    b.crystal([0, 0, -6.9], [0, 0, 5.4], 1.25, 6, 'hull', { edge: 'edge', er: 0.028, k0: 0.48, k1: 0.78, taper: 0.75, phase: Math.PI / 6, face2: 'crysA' });
    b.crystal([0, 0, 6.3], [0, 0, 4.4], 0.55, 6, 'engine', { k0: 0.4, k1: 0.7 });
    for (const [p, q, r, e] of [[[0, 0.7, -2.6], [0, 2.1, 1.6], 0.45, 'edge2'], [[0, 0.8, 0.2], [0, 2.6, 3.8], 0.5, 'edge'], [[0, 0.7, 2.2], [0, 1.9, 5.4], 0.38, 'edge2'],
      [[0.7, 0.2, -1.4], [2.4, -0.2, 2.6], 0.36, 'edge'], [[-0.7, 0.2, -1.4], [-2.4, -0.2, 2.6], 0.36, 'edge'], [[0.6, -0.5, 1.0], [1.9, -1.4, 4.2], 0.3, 'edge2'], [[-0.6, -0.5, 1.0], [-1.9, -1.4, 4.2], 0.3, 'edge2'], [[0, -0.7, -0.6], [0, -1.8, 2.8], 0.34, 'edge']])
      b.crystal(p, q, r, 4, 'hull2', { edge: e, er: 0.022, face2: e === 'edge' ? 'crysA' : 'crysB' });
    for (const g of CLS.lattice.guns) { b.crystal([g[0] * 0.5, g[1], -1.0], [g[0], g[1], g[2] - 0.05], 0.24, 4, 'hull', { edge: 'edge', er: 0.02, k0: 0.2, k1: 0.5, face2: 'crysB' }); b.crystal([g[0], g[1], g[2] + 0.6], [g[0], g[1], g[2] - 0.08], 0.09, 4, 'glow', {}); }
    const r = b.r;
    for (let i = 0; i < 7; i++) { const z = r.range(1.5, 6.5), a = r.range(0, TAU), d = r.range(1.9, 3.2), p = [Math.cos(a) * d, Math.sin(a) * d * 0.8, z], q = [p[0] + r.range(-0.6, 0.6), p[1] + r.range(-0.6, 0.6), p[2] + r.range(0.5, 1.3)]; b.crystal(p, q, r.range(0.1, 0.22), 4, 'hull2', { edge: i % 2 ? 'edge' : 'edge2', er: 0.016, face2: 'crysA' }); }
  },
  choir(b) {   // Echo support: a floating chandelier of lattice — rings of crystal spokes round a glowing heart
    b.crystal([0, 0, -19.6], [0, 0, 20.6], 2.4, 8, 'hull', { edge: 'edge', er: 0.06, k0: 0.4, k1: 0.62, taper: 0.9, face2: 'crysA' });
    b.ell(0, 0, 0, 3.4, 3.4, 4.2, 14, 10, 'glow');
    b.crystal([0, 0, -16.8], [0, 0, -18.3], 0.8, 6, 'glow', {});
    b.crystal([0, 0, 21.0], [0, 0, 18.6], 1.0, 8, 'engine', {});
    for (let r = 0; r < 3; r++) {
      const [z, R, n] = CHOIR_RINGS[r];
      b.lathe([0, 0, z], [0, 0, z + 1], [[R - 0.55, -0.45], [R + 0.55, -0.45], [R + 0.55, 0.45], [R - 0.55, 0.45], [R - 0.55, -0.45]], n * 2, 'hull2', { flat: true });
      b.torus(0, 0, z, R + 0.58, 0.07, n * 2, 3, r === 1 ? 'edge2' : 'edge', 'z');
      for (let i = 0; i < n; i++) {
        const a = (i + (r === 1 ? 0.5 : 0)) / n * TAU, c = Math.cos(a), s = Math.sin(a), up = r === 0 ? -1 : r === 2 ? 1 : (i % 2 ? 1 : -1);
        b.crystal([c * 2.0, s * 2.0, z - up * 1.2], [c * (R - 0.4), s * (R - 0.4), z], 0.42, 4, 'hull', { edge: i % 2 ? 'edge2' : 'edge', er: 0.035, k0: 0.55, k1: 0.8, phase: a, face2: 'crysB' });
        b.ell(c * R, s * R, z, 0.5, 0.5, 0.5, 6, 4, 'glow');
        const L = i % 3 === 0 ? 7 : 4.2;
        b.crystal([c * (R + 0.3), s * (R + 0.3), z], [c * (R + L), s * (R + L), z + up * L * 0.45], 0.36, 4, 'hull', { edge: 'edge', er: 0.03, k0: 0.2, k1: 0.55, phase: a, face2: 'crysA' });
      }
    }
    for (let r = 0; r < 2; r++) {
      const [z0, R0, n0] = CHOIR_RINGS[r], [z1, R1, n1] = CHOIR_RINGS[r + 1];
      for (let i = 0; i < n0; i++) { const a0 = i / n0 * TAU + (r === 1 ? 0.5 / n0 * TAU : 0), a1 = a0 + 0.55; b.edge([Math.cos(a0) * R0, Math.sin(a0) * R0, z0], [Math.cos(a1) * R1, Math.sin(a1) * R1, z1], 0.06, r ? 'edge2' : 'edge'); }
    }
    for (let i = 0; i < 8; i++) { const a = i / 8 * TAU + 0.2, c = Math.cos(a) * 4.8, s = Math.sin(a) * 4.8, L = 5 + (i % 4) * 2.2; b.crystal([c, s, 6.2], [c, s, 6.2 + L], 0.62, 5, 'hull2', { edge: 'edge2', er: 0.035, k0: 0.08, k1: 0.9, taper: 1, face2: 'crysB' }); b.ell(c, s, 6.4 + L, 0.3, 0.3, 0.3, 5, 4, 'glow'); }
  },
  hauler(b, P) {   // convoy hauler: armoured cab, truss spine, ribbed cargo containers, engine block with radiators
    b.hull(HAU_CAB, 'hull', { roleAt: (i, k) => (k === 0 || k === 1 || k === 7 ? 'paint2' : null), capRole0: 'hull2' });
    b.box(0, 1.8, -46.1, 7.4, 1.5, 0.2, 'window');
    for (const k of [3, 5]) b.skin(HAU_CAB, k, 0.2, 0.8, -45.6, -41.0, 0.08, 'window');
    b.skin(HAU_CAB, 4, 0.1, 0.9, -38, -31, 0.25, 'hull2'); b.skinHazard(HAU_CAB, 0, 0.1, 0.9, -46, -44.5, 0.1, 10);
    b.bbox(0, 0, 4, 4.5, 4.5, 72, 'hull2', { ch: 0.7 });
    for (const [sx, sy] of [[-1, -1], [1, -1], [-1, 1], [1, 1]]) b.bbox(sx * 2.6, sy * 2.6, 4, 0.9, 0.9, 70, 'metal', { ch: 0.2 });
    for (let z = -32; z < 40; z += 6) b.edge([-2.6, 2.6, z], [2.6, -2.6, z + 6], 0.25, 'metal', 4);
    const cols = ['accent', 'hull', 'paint2', 'accent'];
    [-26, -6, 14, 32].forEach((z, i) => {
      const role = cols[i];
      b.bbox(0, 0, z, 15, 13, 16, role, { ch: 0.5 });
      for (let k = 0; k < 7; k++) { const zz = z - 6.6 + k * 2.2; b.box(-7.62, 0, zz, 0.3, 12, 0.55, role); b.box(7.62, 0, zz, 0.3, 12, 0.55, role); b.box(0, 6.62, zz, 14, 0.3, 0.55, role); }
      for (const sx of [-1, 1]) for (const sy of [-1, 1]) for (const sz of [-1, 1]) b.box(sx * 7.3, sy * 6.3, z + sz * 7.7, 1.0, 1.0, 1.0, 'dark');
      b.frame([7.78, 1.5, z], [0, 0, -Math.PI / 2], () => b.insignia(P.fac, 2.4));
      b.frame([0, -1, z - 8.05], [-Math.PI / 2, 0, 0], () => b.hazard(13, 1.2, 8, 0.08));
    });
    b.bbox(0, 0, 42, 20, 12, 8, 'hull', { ch: 1.2 });
    for (let k = 0; k < 9; k++) b.box(-8 + k * 2, 7.6, 42, 0.25, 3.2, 6.5, 'metal');
    for (const sx of [-1, 1]) { b.engine(sx * 7, 0, 47, 3.6, 'hull2'); b.lights([[sx * 7.4, 0, -46.2], [sx * 10.2, 6, 42], [sx * 8, -6.6, -26], [sx * 8, -6.6, 14]], 0.7, 'light'); }
    b.lights([[-10.2, 0, 38]], 0.6, 'navR'); b.lights([[10.2, 0, 38]], 0.6, 'navG');
    for (let z = -16; z < 38; z += 20) b.lights([[0, 2.9, z]], 0.4, 'window');
  },
  warden(b) {   // Gilda gunship: tapered ivory hull, brass collar, ranks of lit windows, bridge tower, shield dome
    b.hull(WAR, 'hull', { roleAt: (i, k) => (k === 0 || k === 1 || k === 7 ? 'paint2' : null), capRole0: 'hull2', capRole1: 'trim' });
    b.hull(WAR_SUP, 'paint2', { roleAt: (i, k) => (k === 4 ? 'hull' : null) });
    b.hull(WAR_KEEL, 'hull2');
    // the brass collar and the prow ram
    b.lathe([0, 0, -6], [0, 0, -5], [[11.5, -2.6], [12.6, -3.2], [15.2, -3.2], [15.8, -2.4], [15.8, 2.4], [15.2, 3.2], [12.6, 3.2], [11.5, 2.6], [11.5, -2.6]], 36, 'hull2', { roles: { 3: 'accent' } });
    for (let i = 0; i < 12; i++) { const a = i / 12 * TAU; b.lights([[Math.cos(a) * 15.9, Math.sin(a) * 15.9, -6]], 0.7, 'light'); }
    b.lathe([0, -2, -66.5], [0, -2, -65.5], [[0, -2.2], [1.6, 0], [2.0, 2.0], [1.6, 3.0]], 8, 'hull2', { flat: true });
    // royal-blue bands, ranks of windows, insignia
    for (const k of [3, 5]) { b.skin(WAR, k, 0.35, 0.65, -58, -10, 0.12, 'accent'); b.skin(WAR, k, 0.35, 0.65, -2, 58, 0.12, 'accent'); }
    for (const k of [2, 6]) for (const u of [0.22, 0.5, 0.78]) for (let z = -42; z < 54; z += 3.4) { if (z > -9.5 && z < -2.5) continue; const r = b.r(); if (r < 0.12) continue; b.skin(WAR, k, u - 0.035, u + 0.035, z, z + 1.7, 0.08, r < 0.3 ? 'winDim' : 'window'); }
    b.sym(() => b.frame([secAt(WAR, 12)[1] / 2 + 0.02, 1.5, 12], [0, 0, -Math.PI / 2], () => b.insignia(F_GILDA, 3.4)));
    // bridge tower, shield dome, deck detail
    b.bbox(0, 15.5, 48, 9.5, 5.6, 10, 'hull', { ch: 0.9, fx: 0.85, roles: { top: 'paint2' } });
    b.box(0, 16.6, 42.95, 7.2, 1.3, 0.3, 'window'); for (const sx of [-1, 1]) b.box(sx * 4.6, 16.6, 48, 0.25, 1.2, 7.5, 'window');
    b.cyl([2.5, 18.2, 50], [2.5, 24, 51], 0.18, 0.1, 5, 'metal'); b.cyl([-2.2, 18.2, 51], [-2.2, 21.5, 52], 0.15, 0.08, 5, 'metal'); b.lights([[2.5, 24.1, 51]], 0.35, 'navR');
    b.lathe([0, 13.6, 32], [0, 14.6, 32], [[5.4, 0], [5.4, 0.8], [5.0, 1.0], [4.6, 1.4], [3.8, 2.6], [2.6, 3.4], [1.0, 3.9], [0, 4.0]], 20, 'accent', { roles: { 0: 'hull2', 1: 'hull2' } });
    for (const s of CLS.warden.sub) if (s.k === 'turret') b.barbette(s.p, s.down, 5.2, s.down ? 3.2 : 2.6, 'hull2');
    b.greebles(18, -4, 4, secTop(WAR_SUP, -20) - 0.05, -36, -4, 1.4, 'paint2');
    b.sym(() => { for (let k = 0; k < 6; k++) b.box(6.2, 13.2, 14 + k * 2.2, 0.3, 2.8, 1.2, 'metal'); b.frame([3.2, secTop(WAR_SUP, 0) - 0.05, 0], [0, 0, 0], () => b.vent(2.6, 6, 7)); });
    // armour plating on the lower flanks, brass bands, radiator fins aft, a sensor mast on the prow deck
    for (const k of [1, 7]) for (let z = -50; z < 52; z += 13) b.skin(WAR, k, 0.12, 0.88, z, z + 11, 0.18, 'paint2');
    for (const z of [-40, 30, 53]) b.band(WAR, z, z + 1.6, 0.14, 'hull2');
    for (const k of [2, 6]) for (let i = 0; i < 7; i++) b.skin(WAR, k, 0.06, 0.16, 38 + i * 2, 38.6 + i * 2, 1.6, 'metal');
    b.cyl([0, secTop(WAR_SUP, -40) - 0.1, -40], [0, secTop(WAR_SUP, -40) + 6, -40], 0.35, 0.2, 6, 'metal');
    b.lathe([0, secTop(WAR_SUP, -40) + 6, -40], [0.6, secTop(WAR_SUP, -40) + 6.8, -41], [[0, 0], [2.4, 0.7], [2.6, 0.9], [0, 0.3]], 14, 'hull', { roles: { 1: 'hull2' } });
    for (const k of [2, 6]) for (let z = -54; z < 56; z += 9) b.skin(WAR, k, 0.05, 0.08, z, z + 0.8, 0.2, 'light');
    // stern: engine block, the three drives, running lights
    b.bbox(0, 1, 60, 30, 18, 8, 'hull2', { ch: 1.5 });
    for (const e of CLS.warden.eng) b.engine(e[0], e[1], e[2], e[3] * 0.85, 'hull');
    b.lights([[-15.2, 0, 60], [-11, 0, -55]], 0.9, 'navR'); b.lights([[15.2, 0, 60], [11, 0, -55]], 0.9, 'navG');
    for (let z = -40; z < 40; z += 10) b.lights([[0, secBot(WAR_KEEL, z) - 0.3, z]], 0.6, 'light');
  },
  hulk(b) {   // Relitti converted freighter: welded box chain, armour patches, turret towers, an exposed reactor
    const segs = [[-72, 20, 18, 26], [-44, 30, 26, 30], [-12, 34, 30, 34], [17, 30, 28, 26], [76, 32, 26, 26]];
    for (const [z, w, h, l] of segs) {
      b.frame([b.r.range(-2, 2), b.r.range(-1.5, 1.5), z], [b.r.range(-0.04, 0.04), b.r.range(-0.05, 0.05), b.r.range(-0.06, 0.06)], () => {
        b.bbox(0, 0, 0, w, h, l, 'hull', { ch: 2.4, roles: { bottom: 'hull2' } });
        for (let i = 0; i < 4; i++) b.box(b.r.range(-w * 0.35, w * 0.35), h / 2 + 0.4, b.r.range(-l * 0.35, l * 0.35), b.r.range(5, 11), 0.8, b.r.range(5, 12), 'patch');
        for (const sx of [-1, 1]) {
          for (let i = 0; i < 3; i++) b.box(sx * (w / 2 + 0.35), b.r.range(-h * 0.3, h * 0.3), b.r.range(-l * 0.35, l * 0.35), 0.7, b.r.range(4, 9), b.r.range(5, 10), 'patch');
          for (let i = 0; i < 6; i++) b.box(sx * (w / 2 + 0.3), h * 0.18, -l * 0.4 + i * l * 0.16, 0.4, 1.4, 1.8, b.r() < 0.75 ? 'window' : 'winDim');
          b.frame([sx * (w / 2 + 0.02), -h * 0.28, 0], [0, 0, sx * -Math.PI / 2], () => b.hazard(l * 0.8, 1.6, 9, 0.12));
        }
        b.frame([0, h / 2 + 0.01, -l / 2 + 1.2], [0, 0, 0], () => b.hazard(w * 0.7, 1.4, 8, 0.1));
        b.lights([[-w * 0.4, h / 2 + 0.5, l * 0.3], [w * 0.4, h / 2 + 0.5, -l * 0.3]], 0.8, 'light');
        if (z > -60 && z < 60) b.bbox((z > 0 ? 1 : -1) * (w / 2 + 3.5), b.r.range(-4, 4), 0, 7, 8, l * 0.5, 'accent', { ch: 0.4 });
      });
    }
    // bow bridge, keel spine through the reactor gap, the exposed reactor
    b.bbox(0, 2, -88, 14, 12, 8, 'hull2', { fx: 0.6, fy: 0.6, ch: 1.2 });
    b.box(0, 5.5, -92.05, 7, 1.4, 0.3, 'window');
    b.bbox(0, -4, 47, 7, 7, 40, 'hull2', { ch: 0.8 });
    for (const sx of [-1, 1]) b.bbox(sx * 9, 8, 47, 2, 2, 36, 'metal', { ch: 0.3 });
    b.torus(0, 0, 46, 15, 2.6, 28, 6, 'hull2', 'z');
    b.ell(0, 0, 46, 8.5, 8.5, 8.5, 16, 12, 'core');
    for (let i = 0; i < 6; i++) { const a = i / 6 * TAU + 0.3; b.edge([Math.cos(a) * 8, Math.sin(a) * 8, 46], [Math.cos(a) * 13, Math.sin(a) * 13, 46], 0.9, 'metal', 4); }
    b.torus(0, 0, 40, 15, 1.0, 28, 4, 'hazard', 'z'); b.torus(0, 0, 52, 15, 1.0, 28, 4, 'hazard', 'z');
    // turret towers welded on the hull
    for (const s of CLS.hulk.sub) if (s.k === 'turret') {
      const d = s.down ? -1 : 1, base = d * 9, h = Math.abs(s.p[1] - base) - 1.4;
      b.bbox(s.p[0], base + d * h / 2, s.p[2], 8.5, h, 8.5, 'hull2', { ch: 1.0 });
      if (Math.abs(s.p[0]) > 4) b.bbox(s.p[0] / 2, base + d * 1.5, s.p[2], Math.abs(s.p[0]) + 6, 3, 7, 'hull2', { ch: 0.6 });
      b.frame([s.p[0], base + d * h * 0.6, s.p[2] - 4.3], [d < 0 ? Math.PI : 0, 0, 0], () => b.hazard(7, 1, 5, 0.1));
      b.barbette(s.p, s.down, 5.2, 1.8, 'metal');
    }
    // cranes and masts
    b.at([12, 15, -30], [0, 0, -0.4]).bbox(0, 9, 0, 1.6, 20, 1.6, 'accent', { ch: 0.3 }).reset();
    b.at([12, 15, -30], [0, 0, -0.4]).bbox(-6, 18.5, 0, 12, 1.2, 1.2, 'accent', { ch: 0.2 }).reset();
    b.at([-10, 15, 20], [0.3, 0, 0.5]).bbox(0, 8, 0, 1.6, 17, 1.6, 'dark', { ch: 0.3 }).reset();
    b.edge([6, 38, -30], [6, 18, -30], 0.15, 'metal', 4);
    b.bbox(0, 0, 88, 34, 22, 6, 'hull2', { ch: 1.5 });
    for (const e of CLS.hulk.eng) b.engine(e[0], e[1], e[2], e[3], 'metal');
    b.lights([[-17.5, 0, 88], [-11, 0, -86]], 1.0, 'navR'); b.lights([[17.5, 0, 88], [11, 0, -86]], 1.0, 'navG');
  },
  reliquary(b) {   // Custodi capital barge: a white-stone cathedral on a hull — nave, bell tower, verdigris domes, candle windows
    b.hull(REL, 'hull', { roleAt: (i, k) => (k === 0 || k === 1 || k === 7 ? 'paint2' : null), capRole0: 'hull2', capRole1: 'trim' });
    b.hull(REL_NAVE, 'hull', { roleAt: (i, k) => (k === 3 || k === 4 || k === 5 ? 'hull2' : null), capRole0: 'hull' });
    b.hull(REL_FORE, 'hull', { roleAt: (i, k) => (k === 4 ? 'paint2' : null) });
    // nave: tall candle-gold windows between flying buttresses, a rose window on the west front
    for (const k of [2, 6]) for (let z = -10; z < 48; z += 4.4) { b.skin(REL_NAVE, k, 0.12, 0.88, z, z + 1.5, 0.12, 'window'); b.skin(REL_NAVE, k, 0.9, 0.97, z - 0.2, z + 1.7, 0.18, 'trim'); }
    b.sym(() => { for (let z = -12.2; z < 50; z += 4.4) b.at([11.6, 12.4, z], [0, 0, 0.62]).bbox(0, 0, 0, 9.5, 1.1, 1.2, 'hull', { ch: 0.3, fx: 0.7 }).reset(); });
    b.porthole([0, 16, -14.1], [0, 0, -1], 3.0, 'trim', 'window');
    // bell tower (its belfry is the bridge), its spire; verdigris dome over the shield generator
    b.bbox(0, 22, 4, 9, 24, 9, 'hull', { ch: 0.6 });
    b.bbox(0, 34.5, 4, 10.4, 5.4, 10.4, 'paint2', { ch: 0.5 });
    for (const [dx, dz, ry] of [[0, -5.25, 0], [0, 5.25, 0], [-5.25, 0, Math.PI / 2], [5.25, 0, Math.PI / 2]]) { b.at([dx, 34.5, 4 + dz], [0, ry, 0]).box(0, 0, 0, 3.6, 3.8, 0.2, 'dark').box(0, 0.4, 0, 1.6, 1.6, 0.3, 'lantern').reset(); }
    for (let i = 0; i < 4; i++) { const a = i * Math.PI / 2; b.at([0, 26 - i * 0.01, 4], [0, a, 0]).box(0, 0, -4.56, 1.2, 6, 0.2, 'window').reset(); }
    b.lathe([0, 37.2, 4], [0, 38.2, 4], [[7.4, 0], [7.4, 0.6], [5.8, 1.2], [1.0, 8.6], [0, 9.6]], 4, 'hull2', { flat: true, phase: Math.PI / 4, roles: { 0: 'trim', 1: 'trim' } });
    b.cyl([0, 46.8, 4], [0, 49.5, 4], 0.25, 0.12, 4, 'trim'); b.lights([[0, 49.6, 4]], 0.6, 'lantern');
    b.lathe([0, 15.5, -24], [0, 16.5, -24], [[7.2, 0], [7.2, 1.2], [6.8, 1.5], [6.2, 2.6], [5.0, 4.4], [3.2, 5.8], [1.2, 6.6], [0.5, 6.8], [0.3, 8.8], [0, 9.0]], 22, 'hull2', { roles: { 0: 'hull', 1: 'trim', 7: 'trim', 8: 'trim' } });
    b.lathe([0, 15.5, -24], [0, 16.5, -24], [[7.25, 0.3], [7.25, 0.9]], 22, 'window', { roleAt: (j, i) => (i % 2 ? 'hull' : null) });
    // corner towers carrying the turrets, the keel tower below
    for (const s of CLS.reliquary.sub) if (s.k === 'turret') {
      const top = s.p[1], foot = s.down ? -6 : 2, d = Math.abs(top - foot);
      b.barbette(s.p, s.down, 4.8, d, 'hull');
      b.lathe([s.p[0], top - (s.down ? -1 : 1) * 1.6, s.p[2]], [s.p[0], top + (s.down ? -1 : 1) * 0.4, s.p[2]], [[5.15, 0], [5.15, 0.9]], 16, 'window', { roleAt: (j, i) => (i % 2 ? 'trim' : null) });
    }
    // glyph bands, hull windows, bow ornament, stern drives
    for (const k of [3, 5]) for (let z = -66; z < 74; z += 6) b.skin(REL, k, 0.35, 0.65, z, z + 3.2, 0.12, 'glyph');
    for (const k of [2, 6]) for (let z = -54; z < 74; z += 4) b.skin(REL, k, 0.55, 0.62, z, z + 1.4, 0.1, b.r() < 0.7 ? 'winDim' : 'window');
    b.frame([0, 2, -81.2], [-Math.PI / 2, 0, 0], () => b.insignia(F_CUSTODI, 2.6));
    for (const k of [3, 5]) b.skin(REL, k, 0.05, 0.95, -81, -74, 0.2, 'trim');
    b.bbox(0, 0, 76, 34, 18, 8, 'hull2', { ch: 1.5 });
    for (const e of CLS.reliquary.eng) b.engine(e[0], e[1], e[2], e[3] * 0.84, 'trim');
    for (let i = 0; i < 10; i++) b.lights([[b.r.range(-16, 16), secTop(REL, 0) + 0.4, b.r.range(-60, 70)]], 0.7, 'lantern');
  },
};

const CACHE = new Map();
// the cast aces' liveries (hull, accent, trim) — the same colours as the firmware's ACE_PAL
const c8 = (r, g, b) => [r / 255, g / 255, b / 255];
const ACE_LIVERY = [[c8(240, 244, 255), c8(40, 80, 230), c8(220, 180, 60)], [c8(255, 244, 210), c8(240, 190, 50), c8(60, 170, 140)],
  [c8(220, 30, 30), c8(255, 200, 0), c8(40, 24, 10)], [c8(70, 20, 140), c8(120, 240, 255), c8(255, 255, 255)], [c8(80, 76, 70), c8(160, 84, 40), c8(255, 140, 30)]];
function shipPalette(fac, ace) {
  let pal = PALETTES[fac] || PALETTES[F_GILDA];
  if (typeof ace === 'number' && ACE_LIVERY[ace]) { const L = ACE_LIVERY[ace]; pal = { ...pal, hull: L[0], accent: L[1], hull2: L[2], light: L[1], paint2: cmix(L[0], L[2], 0.35), decal: L[1], trim: L[2], stripe: L[1], edge: L[1] }; }
  else if (ace) pal = { ...pal, accent: [0.85, 0.12, 0.1], hull2: [0.8, 0.62, 0.22], light: [1, 0.35, 0.25], decal: [0.85, 0.12, 0.1], trim: [0.8, 0.62, 0.22], stripe: [0.85, 0.12, 0.1], paint2: cmix(pal.hull, [0.85, 0.12, 0.1], 0.3) };
  return pal;
}
// Builds (once) a ship class in a faction palette: { full, body, parts } (parts in pivot-local space).
function buildShip(THREE, ck, fac, ace) {
  const key = 'ship:' + ck + ':' + fac + ':' + (typeof ace === 'number' ? 'c' + ace : ace ? 1 : 0);
  if (CACHE.has(key)) return CACHE.get(key);
  const pal = shipPalette(fac, ace), seed = (ck.length * 7919 + fac * 31) | 0;
  const b = new Builder(pal, seed), raw = [];
  const ctx = { fac, ace, part(pivot, side, fn, kind = 'pod') { const pb = new Builder(pal, seed + 101 * (raw.length + 1)); fn(pb); raw.push({ pb, pivot, side, kind }); } };
  (DESIGN[ck] || DESIGN.lancer)(b, ctx);
  const body = b.build(THREE);
  let full = body;
  if (raw.length) { const all = new Builder(pal, 1).append(b); for (const p of raw) all.append(p.pb); full = all.build(THREE); }
  const out = { full, body, parts: raw.map((p) => ({ geo: p.pb.build(THREE, [-p.pivot[0], -p.pivot[1], -p.pivot[2]]), pivot: p.pivot.slice(), kind: p.kind, side: p.side })) };
  CACHE.set(key, out);
  return out;
}
// Merged geometry for a ship class in a faction palette (ace: true = generic crimson-and-gold, 0..4 = a cast
// livery). The FULL model, moving parts at rest — for the hub, the showcase, debris and any static use.
export function shipGeometry(THREE, ck, fac, ace = false) { return buildShip(THREE, ck, fac, ace).full; }
// The same model split for animation: body (everything static) + parts, each built around its own pivot
// (mount the part mesh at `pivot` in ship space; kind 'pod' swivels about its local X axis). Classes
// without moving parts return { body: full model, parts: [] }.
export function shipParts(THREE, ck, fac, ace = false) { const s = buildShip(THREE, ck, fac, ace); return { body: s.body, parts: s.parts }; }

// Capital turret in its faction's style: base (static, y 0..1.6), head (yaws; mounted 1.4 above the base) and
// barrels (pitch; mounted at (0, 1.3, -0.6) on the head, muzzles at z = -7.5) as three geometries.
export function turretGeometry(THREE, fac) {
  const key = 'turret:' + fac;
  if (CACHE.has(key)) return CACHE.get(key);
  const f = PALETTES[fac] ? fac : F_GILDA, pal = PALETTES[f], A = [0, 0, 0], Y = [0, 1, 0];
  const base = new Builder(pal, 3), head = new Builder(pal, 4), gun = new Builder(pal, 5);
  if (f === F_RELITTI) {   // welded drum, boxy patched head, mismatched barrels and an ammo drum
    base.lathe(A, Y, [[4.4, 0], [4.4, 1.0], [4.0, 1.25], [3.6, 1.6], [0, 1.6]], 8, 'hull2', { flat: true, phase: Math.PI / 8 });
    base.lathe(A, Y, [[4.45, 0.3], [4.45, 0.8]], 16, 'hazard', { roleAt: (j, i) => (i % 2 ? 'dark' : null) });
    head.bbox(0, 1.25, 0.3, 4.6, 2.5, 5.0, 'patch', { ch: 0.35, fx: 0.85, fy: 0.8 });
    head.box(1.0, 2.6, 1.2, 1.8, 0.3, 2.0, 'patch'); head.box(-1.4, 1.4, 2.85, 1.2, 1.0, 0.3, 'hazard'); head.box(-1.2, 2.5, -1.4, 1.2, 0.4, 0.8, 'metal');
    head.lights([[1.9, 2.6, -1.6]], 0.35, 'light');
    gun.gun([-0.85, 0, -7.5], 7.5, 0.34); gun.gun([0.85, 0.1, -6.3], 6.3, 0.4);
    gun.cyl([0.85, -0.75, -1.5], [0.85, -0.75, 0.3], 0.7, 0.7, 10, 'metal');
  } else if (f === F_CUSTODI) {   // white-stone drum with a gold band, verdigris-capped dome, one heavy gilded barrel
    base.lathe(A, Y, [[4.3, 0], [4.3, 1.1], [3.9, 1.3], [3.7, 1.6], [0, 1.6]], 16, 'hull');
    base.lathe(A, Y, [[4.33, 0.35], [4.33, 0.75]], 16, 'trim');
    head.lathe(A, Y, [[3.6, 0], [3.6, 0.9], [3.3, 1.6], [2.5, 2.4], [1.2, 2.9], [0.3, 3.05], [0, 3.1]], 14, 'hull', { roles: { 3: 'hull2', 4: 'hull2', 5: 'hull2' } });
    head.cyl([0, 3.0, 0], [0, 4.0, 0], 0.25, 0.05, 6, 'trim'); head.box(0, 1.5, -3.45, 1.6, 0.35, 0.3, 'window'); head.ring([0, 0, 0], [0, 1, 0], 3.55, 3.75, 0.85, 1.15, 14, 'trim');
    gun.gun([0, 0, -7.5], 7.5, 0.48, 10);
    for (const z of [-2.2, -4.6]) gun.ring([0, 0, z], [0, 0, z - 1], 0.5, 0.78, 0, 0.35, 10, 'trim');
  } else if (f === F_ECO) {   // a crystal ring, a crystal cluster for a head, two emitter prisms
    base.lathe(A, Y, [[4.2, 0], [4.4, 0.8], [3.6, 1.6], [0, 1.6]], 6, 'hull', { flat: true });
    for (let k = 0; k < 6; k++) { const a = k / 6 * TAU; base.edge([Math.cos(a) * 4.4, 0.8, Math.sin(a) * 4.4], [Math.cos(a + TAU / 6) * 4.4, 0.8, Math.sin(a + TAU / 6) * 4.4], 0.08, 'edge', 3); }
    head.crystal([0, 0.1, 0], [0, 3.4, 0], 2.2, 6, 'crysA', { edge: 'edge', er: 0.07, face2: 'hull' });
    for (const sx of [-1, 1]) head.crystal([sx * 1.4, 0.4, 0.8], [sx * 2.6, 2.6, 1.6], 0.7, 4, 'crysB', { edge: 'edge2', er: 0.05 });
    for (const sx of [-0.8, 0.8]) { gun.crystal([sx, 0, 0.4], [sx, 0, -7.6], 0.36, 4, 'hull', { edge: 'edge2', er: 0.04, k0: 0.1, k1: 0.8, face2: 'crysA' }); gun.crystal([sx, 0, -6.7], [sx, 0, -7.7], 0.18, 4, 'glow', {}); }
  } else {   // Gilda: brass ring with a royal-blue band, ivory armoured dome and mantlet, twin brass-sleeved barrels
    base.lathe(A, Y, [[4.4, 0], [4.4, 0.9], [4.0, 1.2], [3.8, 1.6], [0, 1.6]], 16, 'hull2');
    base.lathe(A, Y, [[4.42, 0.22], [4.42, 0.66]], 16, 'accent');
    head.lathe(A, Y, [[3.5, 0], [3.5, 1.0], [3.1, 1.9], [2.0, 2.6], [0, 2.8]], 10, 'hull', { flat: true });
    head.bbox(0, 1.3, -2.7, 3.4, 1.8, 1.6, 'hull2', { ch: 0.25 }); head.ring([0, 0, 0], [0, 1, 0], 3.45, 3.7, 0.3, 0.7, 10, 'hull2', { flat: true });
    head.box(1.2, 2.1, -1.95, 0.7, 0.35, 0.3, 'window'); head.frame([0, 2.62, 0.8], [0, 0, 0], () => head.insignia(F_GILDA, 0.7));
    for (const sx of [-0.8, 0.8]) { gun.gun([sx, 0, -7.5], 7.5, 0.3); gun.cyl([sx, 0, -1.2], [sx, 0, 0.4], 0.5, 0.5, 10, 'hull2'); }
  }
  base.lights([[0, 1.7, 3.4]], 0.45, 'light');
  const out = { base: base.build(THREE), head: head.build(THREE), gun: gun.build(THREE) };
  CACHE.set(key, out);
  return out;
}

// ---- stations ------------------------------------------------------------------------------------
// A world.js station blueprint is a list of faction modules; each kind below builds its part of ONE merged
// geometry. Returns { geo, lights: [[x,y,z,r,g,b,phase]...] } (blinking points: running lights and the guide
// lights down the docking lane). The blueprint's yaw turns the whole layout (spheres and dock are turned the
// same way in world.js).
const rotY = (v, a) => { const c = Math.cos(a), s = Math.sin(a); return [c * v[0] + s * v[2], v[1], -s * v[0] + c * v[2]]; };
const add3 = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
// point, outward normal and edge tangent on face k of a section hull at (u, z)
function secFace(secs, k, u, z) {
  const p = octPts(secAt(secs, z)), a = p[k & 7], c = p[(k + 1) & 7], l = Math.hypot(c[0] - a[0], c[1] - a[1]) || 1;
  return { p: lerp3(a, c, u), n: [(c[1] - a[1]) / l, -(c[0] - a[0]) / l, 0], t: [(c[0] - a[0]) / l, (c[1] - a[1]) / l, 0] };
}
const ST_EXTRA = { [F_CUSTODI]: { shell: [0.2, 0.29, 0.26] } };
// a small moored boat along local Z (engines cold), for piers and cables
function boat(b, p, len, role) {
  const [x, y, z] = p, w = len * 0.3, h = len * 0.22;
  b.bbox(x, y, z, w, h, len, role, { ch: w * 0.12, fx: 0.62, fy: 0.7 });
  b.bbox(x, y + h * 0.55, z + len * 0.12, w * 0.55, h * 0.5, len * 0.32, 'hull2', { ch: w * 0.06 });
  b.box(x, y + h * 0.62, z - len * 0.04 - 0.05, w * 0.42, h * 0.16, 0.12, 'window');
  for (const sx of [-1, 1]) b.lathe([x + sx * w * 0.3, y, z + len * 0.5 - 0.6], [x + sx * w * 0.3, y, z + len * 0.5 + 0.4], [[0, 0], [w * 0.2, 0], [w * 0.22, 1.0], [w * 0.15, 1.0], [0, 0.6]], 8, 'metal', { roles: { 3: 'dark' } });
}
const STATION = {
  // docking bay house: open towards local +Z (mouth at m.p), lit frame, lit interior, hazard sill, two
  // approach booms with lamp posts, guide lights down the lane. style 0 Gilda, 1 Custodi gate, 2 Relitti
  dockhouse(b, m, L) {
    const [W, H, D] = m.size, t = 4, st = m.style || 0, a = m.a || 0, foot = m.foot || 0;
    const wall = st === 2 ? 'patch' : 'hull', trim = st === 0 ? 'hull2' : st === 1 ? 'trim' : 'hazard';
    b.frame(m.p, [0, a, 0], () => {
      b.bbox(0, -H / 2 - (t + foot) / 2, -D / 2 - t / 2, W + 2 * t, t + foot, D + t, st === 2 ? 'hull2' : 'paint2', { ch: 1 });
      b.bbox(0, H / 2 + t / 2, -D / 2 - t / 2, W + 2 * t, t, D + t, wall, { ch: 1, roles: { top: st === 1 ? 'hull2' : 'paint2' } });
      for (const sx of [-1, 1]) b.bbox(sx * (W / 2 + t / 2), 0, -D / 2 - t / 2, t, H, D + t, wall, { ch: 0.8 });
      b.box(0, 0, -D - t / 2, W, H, t, 'hull2');
      for (const sx of [-1, 1]) { b.box(sx * (W / 2 - 0.25), H * 0.28, -D / 2, 0.4, 1.0, D * 0.92, 'winDim'); b.box(sx * (W / 2 - 0.25), -H * 0.3, -D / 2, 0.4, 0.6, D * 0.92, 'winDim'); }
      for (let k = 0; k < 4; k++) b.box(0, H / 2 - 0.3, -D * (0.15 + k * 0.22), W * 0.7, 0.4, 1.4, 'window');
      b.box(0, 0, -D + 0.3, W * 0.5, H * 0.5, 0.3, 'glow');
      b.ring([0, -H / 2, -D * 0.55], [0, -H / 2 + 1, -D * 0.55], W * 0.2, W * 0.24, 0, 0.12, 24, 'light');
      b.frame([0, -H / 2, -1.8], [0, 0, 0], () => b.hazard(W, 2.8, 12, 0.1));
      b.box(0, H / 2 + 0.5, 0.4, W + 2 * t, 1.0, 0.8, 'light'); b.box(0, -H / 2 - 0.5, 0.4, W + 2 * t, 1.0, 0.8, 'light');
      for (const sx of [-1, 1]) b.box(sx * (W / 2 + 0.5), 0, 0.4, 1.0, H, 0.8, 'light');
      b.bbox(0, H / 2 + t + 1.5, -3, W * 0.6, 3, 4, trim, { ch: 0.4 });
      if (st === 1) { for (const sx of [-1, 1]) b.lathe([sx * (W / 2 + t / 2), H / 2 + t, 0], [sx * (W / 2 + t / 2), H / 2 + t + 1, 0], [[3.2, -0.5], [3.2, 0], [2.6, 0.8], [1.0, 3.2], [0, 4.6]], 4, 'hull2', { flat: true, phase: Math.PI / 4, roles: { 0: 'trim' } }); }
      for (const sx of [-1, 1]) {
        b.bbox(sx * (W / 2 + 7), -H / 2 - 3, 34, 3.2, 3.2, 68, st === 2 ? 'metal' : 'hull2', { ch: 0.5 });
        for (let z = 10; z < 68; z += 14) b.box(sx * (W / 2 + 7), -H / 2 - 0.6, z, 1.0, 2.6, 1.0, 'light');
      }
    });
    for (let z = 8; z <= 560; z += z < 68 ? 14 : 45) for (const sx of [-1, 1]) { L(add3(m.p, rotY([sx * (W / 2 + 7), -H / 2 + 1.6, z], a))); if (z >= 68) L(add3(m.p, rotY([sx * (W / 2 + 7), H / 2 + 6, z], a))); }
  },
  // ---- Gilda: the Ardali Exchange --------------------------------------------------------------------
  gring(b, m, L) {
    const [R, W, H] = m.size, A = [0, 0, 0], B = [0, 1, 0], hw = W / 2, hh = H / 2, N = m.n, da = m.da;
    b.lathe(A, B, [[R - hw + 4, -hh], [R + hw - 4, -hh], [R + hw, -hh + 4], [R + hw, hh - 3], [R + hw - 3, hh], [R - hw + 3, hh], [R - hw, hh - 3], [R - hw, -hh + 4], [R - hw + 4, -hh]], 160, 'hull',
      { roles: { 0: 'paint2', 1: 'hull2', 3: 'hull2', 4: 'paint2', 5: 'hull2', 7: 'hull2' } });
    for (const [rr, y0, y1] of [[R + hw + 0.3, -6, -2.5], [R + hw + 0.3, 2, 5.5], [R - hw - 0.3, -2, 2]]) b.lathe(A, B, rr > R ? [[rr, y0], [rr, y1]] : [[rr, y1], [rr, y0]], 360, 'window', { roleAt: (j, i) => (i % 4 === 3 ? 'hull' : null) });
    b.lathe(A, B, [[R + hw + 0.35, -11], [R + hw + 0.35, -8.5]], 160, 'accent');
    b.ring(A, B, R + hw - 2.2, R + hw - 0.6, hh, hh + 1.4, 160, 'hull2'); b.ring(A, B, R - hw + 0.6, R - hw + 2.2, hh, hh + 1.4, 160, 'hull2');
    for (let k = 0; k < 24; k++) { const a = da + k / 24 * TAU; L([(R + hw + 1) * Math.cos(a), hh + 1.5, -(R + hw + 1) * Math.sin(a)]); }
    for (let k = 0; k < 12; k++) { const a = da + (k + 0.25) / 12 * TAU, p = [R * Math.cos(a), -hh, -R * Math.sin(a)]; b.lathe(p, add3(p, [0, -1, 0]), [[6, 0], [6, 2], [4, 4], [1.2, 16], [0, 22]], 8, 'hull2', { flat: true, roles: { 0: 'paint2' } }); L(add3(p, [0, -23, 0])); }
    for (let k = 0; k < N; k++) {
      const th = da + (k + 0.5) / N * TAU, o = [R * Math.cos(th), hh, -R * Math.sin(th)];
      b.frame(o, [0, th, 0], () => gDistrict(b, b.r.int(5), W, TAU * R / N, (v) => L(add3(o, rotY(v, th)))));
    }
  },
  ghub(b, m, L) {
    const A = [0, 0, 0], B = [0, 1, 0];
    b.lathe(A, B, [[0, -76], [6, -72], [14, -62], [28, -46], [46, -30], [60, -18], [64, -14], [64, 14], [60, 18], [60, 30], [64, 34], [64, 40], [52, 46], [50, 50]], 56, 'hull',
      { roles: { 0: 'hull2', 1: 'hull2', 2: 'paint2', 3: 'paint2', 5: 'hull2', 7: 'hull2', 9: 'hull2', 10: 'hull2', 11: 'paint2' } });
    const DOME = [[50, 50], [49, 60], [44, 72], [35, 83], [22, 91], [9, 95], [8, 98], [8, 104], [5, 106], [1, 116], [0, 118]];
    b.lathe(A, B, DOME, 56, 'hull2', { roles: { 6: 'window' } });
    for (let k = 0; k < 16; k++) { const a = k / 16 * TAU, c = Math.cos(a), s = Math.sin(a); for (let j = 0; j < 5; j++) { const p = DOME[j], q = DOME[j + 1]; b.edge([c * (p[0] + 0.6), p[1], s * (p[0] + 0.6)], [c * (q[0] + 0.6), q[1], s * (q[0] + 0.6)], 0.9, 'paint2', 4); } }
    for (const [rr, y0, y1] of [[64.3, -10, -6], [64.3, 2, 6], [60.3, 22, 26]]) b.lathe(A, B, [[rr, y0], [rr, y1]], 240, 'window', { roleAt: (j, i) => (i % 3 === 2 ? 'hull' : null) });
    b.lathe(A, B, [[64.35, 8], [64.35, 10.5]], 56, 'accent');
    for (let k = 0; k < 8; k++) { const a = (k + 0.5) / 8 * TAU, p = [Math.cos(a) * 57, 40, Math.sin(a) * 57]; b.lathe(p, add3(p, [0, 1, 0]), [[0, 0], [3, 0], [3, 12], [2.4, 13], [0.4, 32], [0, 33]], 8, 'paint2', { roles: { 2: 'hull2' } }); L(add3(p, [0, 34, 0])); }
    b.cyl([0, -76, 0], [0, -112, 0], 1.2, 0.3, 6, 'metal'); L([0, -114, 0]); L([0, 120, 0]);
  },
  gpier(b, m, L) {
    const [Rin, h, w] = m.size, a = m.a, r0 = 63, len = Rin - r0, cx = (Rin + r0) / 2;
    b.frame([0, 0, 0], [0, a, 0], () => {
      b.bbox(cx, 0, 0, len, h, w, 'hull', { ch: 1, roles: { top: 'paint2' } });
      b.box(cx, h / 2 + 0.2, 0, len, 0.4, w * 0.35, 'accent');
      for (const sz of [-1, 1]) { b.cyl([r0, 2.5, sz * (w / 2 + 1.6)], [Rin, 2.5, sz * (w / 2 + 1.6)], 1.4, 1.4, 8, 'hull2'); b.box(cx, -0.5, sz * (w / 2 + 0.15), len * 0.9, 1.0, 0.3, 'window'); }
      for (let k = 0; k < 3; k++) { const x = r0 + 30 + k * (len - 60) / 2, sz = k % 2 ? 1 : -1; boat(b, [x, -h / 2 - 6, sz * (w / 2 + 9)], b.r.range(22, 34), k % 2 ? 'hull' : 'paint2'); }
    });
    for (let k = 0; k < 4; k++) L(rotY([r0 + (k + 0.5) * len / 4, h / 2 + 1, 0], a));
  },
  // ---- Custodi: the Lamp Monastery inside the dead beacon Lumen ----------------------------------------
  shell(b, m, L) {
    const [Rs, T] = m.size, open = m.open, r = b.r, NI = 22, NJ = 36;
    const P = (al, be, rad) => [rad * Math.sin(al) * Math.cos(be), rad * Math.sin(al) * Math.sin(be), rad * Math.cos(al)];
    for (let i = 0; i < NI; i++) for (let j = 0; j < NJ; j++) {
      const a0 = i / NI * Math.PI, a1 = (i + 1) / NI * Math.PI, b0 = j / NJ * TAU, b1 = (j + 1) / NJ * TAU, am = (a0 + a1) / 2;
      const jag = open + (r() - 0.5) * 0.22 + 0.08 * Math.sin(b0 * 3 + 1);
      if (am < jag) continue;
      const rim = am < jag + 0.2, s = rim ? 0.05 : 0.014, ia = a0 + (a1 - a0) * s, ib = a1 - (a1 - a0) * s, ja = b0 + (b1 - b0) * s, jb = b1 - (b1 - b0) * s;
      const lift = rim ? r.range(-2, 7) : r.range(-1.4, 1.4), ro = Rs + lift, ri = Rs - T * (rim ? r.range(0.5, 1) : 1) + lift;
      const roll = r(), role = roll < 0.012 ? 'paint2' : roll < 0.03 ? 'hull2' : 'shell';
      const outer = [P(ia, ja, ro), P(ia, jb, ro), P(ib, jb, ro), P(ib, ja, ro)], inner = [P(ia, ja, ri), P(ia, jb, ri), P(ib, jb, ri), P(ib, ja, ri)];
      b.loft([inner, outer], role, { jit: 0 });
      if (!rim && r() < 0.05) { const n = unit3(ctr3(outer)), l0 = lerp3(outer[0], outer[3], 0.45), r0 = lerp3(outer[1], outer[2], 0.45), l1 = lerp3(outer[0], outer[3], 0.55), r1 = lerp3(outer[1], outer[2], 0.55), q = [lerp3(l0, r0, 0.12), lerp3(l0, r0, 0.88), lerp3(l1, r1, 0.88), lerp3(l1, r1, 0.12)]; b.loft([q, q.map((v) => [v[0] + n[0] * 0.4, v[1] + n[1] * 0.4, v[2] + n[2] * 0.4])], 'glyph', { jit: 0 }); }
      if (rim && r() < 0.25) { const c = P(am, (b0 + b1) / 2, Rs + r.range(10, 26)); b.at(c, [r() * TAU, r() * TAU, 0]).bbox(0, 0, 0, r.range(3, 8), r.range(2, 5), r.range(4, 10), 'shell', { ch: 0.6 }).reset(); }
    }
    for (let k = 0; k < 8; k++) {
      const be = k / 8 * TAU + 0.2; let prev = null;
      for (let i = 0; i <= 24; i++) { const al = Math.PI - i / 24 * (Math.PI - open - 0.3), p = P(al, be, Rs + 1.8); if (prev) b.edge(prev, p, 2.2, 'metal', 4); prev = p; }
    }
    for (const al of [1.8, 2.5]) for (let j = 0; j < 36; j++) b.edge(P(al, j / 36 * TAU, Rs + 1.6), P(al, (j + 1) / 36 * TAU, Rs + 1.6), 1.8, 'metal', 4);
  },
  ledge(b, m) {
    const fy = m.p[1], r = b.r;
    for (let i = 0; i < 8; i++) {
      const x = r.range(-55, 55), z = r.range(-55, 40), w = r.range(40, 66), d = r.range(36, 60), hh = r.range(26, 46);
      b.at([x, fy - hh / 2, z], [r.range(-0.06, 0.06), r.range(0, TAU), r.range(-0.06, 0.06)]).bbox(0, 0, 0, w, hh, d, 'shell', { ch: 3, fx: 0.86, fy: 0.9 }).reset();
    }
    for (let i = 0; i < 5; i++) { const x = r.range(-45, 45), z = r.range(-45, 30); b.lathe([x, fy - 30, z], [x, fy - 31, z], [[r.range(12, 20), 0], [r.range(6, 10), r.range(14, 22)], [0, r.range(28, 40)]], 5, 'shell', { flat: true, phase: r() }); }
  },
  terrace(b, m, L) {
    const [x, y, z] = m.p, [w, h, d] = m.size;
    b.bbox(x, y, z, w, h, d, 'hull', { ch: 1.2, roles: { top: 'paint2' } });
    b.box(x, y + h / 2 + 0.5, z + d / 2 - 0.6, w, 1.0, 1.2, 'trim');
    for (let k = 0; k < w / 4 - 1; k++) b.box(x - w / 2 + 3 + k * 4, y + h / 2 + 1.6, z + d / 2 - 0.8, 0.8, 2.2, 0.8, 'hull');
    b.box(x, y + h / 2 + 2.9, z + d / 2 - 0.8, w - 2, 0.6, 1.0, 'hull');
    for (let k = 0; k < w / 7 - 1; k++) b.box(x - w / 2 + 5 + k * 7, y - 0.5, z + d / 2 + 0.15, 2.4, h * 0.5, 0.3, b.r() < 0.82 ? 'window' : 'dark');
    for (const sx of [-1, 1]) for (let k = 0; k < d / 9 - 1; k++) b.box(x + sx * (w / 2 + 0.15), y - 0.5, z - d / 2 + 6 + k * 9, 0.3, h * 0.5, 2.4, b.r() < 0.82 ? 'window' : 'dark');
    for (const sx of [-1, 1]) L([x + sx * (w / 2 - 1), y + h / 2 + 4, z + d / 2 - 1]);
  },
  pagoda(b, m, L) {
    const [x, y0, z] = m.p, [w0, , d0] = m.size, tiers = m.tiers || 3; let y = y0;
    for (let t = 0; t < tiers; t++) {
      const w = w0 * (1 - t * 0.24), d = d0 * (1 - t * 0.24), h = 16 - t * 2.5, Ev = (Math.max(w, d) / 2 + 5) * Math.SQRT2;
      b.bbox(x, y + h / 2, z, w, h, d, 'hull', { ch: 0.6 });
      for (let k = 0; k < w / 6 - 1; k++) for (const sz of [-1, 1]) b.box(x - w / 2 + 4 + k * 6, y + h * 0.5, z + sz * (d / 2 + 0.15), 1.8, h * 0.55, 0.3, 'window');
      for (let k = 0; k < d / 6 - 1; k++) for (const sx of [-1, 1]) b.box(x + sx * (w / 2 + 0.15), y + h * 0.5, z - d / 2 + 4 + k * 6, 0.3, h * 0.55, 1.8, 'window');
      const top = t === tiers - 1;
      b.lathe([x, y + h, z], [x, y + h + 1, z], top ? [[Ev, -0.8], [Ev, 0], [Ev * 0.84, 1.4], [Ev * 0.45, 6], [Ev * 0.12, 11], [0, 12]] : [[Ev, -0.8], [Ev, 0], [Ev * 0.84, 1.4], [Ev * 0.5, 5], [Ev * 0.34, 7]], 4, 'hull2', { flat: true, phase: Math.PI / 4, roles: { 0: 'trim' } });
      for (const [sx, sz] of [[-1, -1], [1, -1], [-1, 1], [1, 1]]) L([x + sx * Ev * 0.7, y + h + 0.2, z + sz * Ev * 0.7]);
      y += top ? h + 12 : h + 6;
    }
    b.cyl([x, y - 1, z], [x, y + 9, z], 0.8, 0.2, 6, 'trim'); b.lights([[x, y + 9.4, z]], 1.4, 'lantern'); L([x, y + 10, z]);
  },
  ctower(b, m, L) {
    const [x, y, z] = m.p, [w, h] = m.size, Ev = (w / 2 + 2.5) * Math.SQRT2;
    b.bbox(x, y + h / 2, z, w, h, w, 'hull', { ch: 0.5 });
    for (let k = 0; k < 3; k++) b.box(x, y + h * (0.3 + k * 0.22), z, w + 1.2, 0.8, w + 1.2, 'trim');
    for (let k = 0; k < 4; k++) { const yy = y + h * (0.2 + k * 0.2); b.box(x + w / 2 + 0.15, yy, z, 0.3, 3, 1.4, 'window'); b.box(x - w / 2 - 0.15, yy, z, 0.3, 3, 1.4, 'window'); b.box(x, yy, z + w / 2 + 0.15, 1.4, 3, 0.3, 'window'); b.box(x, yy, z - w / 2 - 0.15, 1.4, 3, 0.3, 'window'); }
    b.lathe([x, y + h, z], [x, y + h + 1, z], [[Ev, -0.5], [Ev, 0], [Ev * 0.8, 1.2], [Ev * 0.32, 6], [0, 10]], 4, 'hull2', { flat: true, phase: Math.PI / 4, roles: { 0: 'trim' } });
    b.cyl([x, y + h + 9, z], [x, y + h + 15, z], 0.4, 0.1, 5, 'trim'); L([x, y + h + 15.5, z]);
  },
  hall(b, m, L) {   // a chapel hall with a verdigris pitched roof, candle windows and a rose window
    const [x, y, z] = m.p, [w, h, d] = m.size, hr = w * 0.6;
    b.bbox(x, y + h * 0.35, z, w, h * 0.7, d, 'hull', { ch: 0.6 });
    b.hull([[z + d / 2 + 1, w + 2, hr, y + h * 0.7 + hr / 2 - 1, w * 0.5, 0.2, x], [z - d / 2 - 1, w + 2, hr, y + h * 0.7 + hr / 2 - 1, w * 0.5, 0.2, x]], 'hull2', { capRole0: 'hull', capRole1: 'hull' });
    for (const sx of [-1, 1]) for (let k = 0; k < d / 7 - 1; k++) b.box(x + sx * (w / 2 + 0.15), y + h * 0.35, z - d / 2 + 5 + k * 7, 0.3, h * 0.42, 2.0, 'window');
    b.porthole([x, y + h * 0.5, z + d / 2 + 0.2], [0, 0, 1], w * 0.2, 'trim', 'window');
    L([x, y + h * 0.7 + hr, z + d / 2]); L([x, y + h * 0.7 + hr, z - d / 2]);
  },
  stairs(b, m, L) {
    const [x, y, z] = m.p, [w, hg, d] = m.size, n = 12;
    for (let k = 0; k < n; k++) { const sh = (k + 1) * hg / n; b.box(x, y + sh / 2, z + d / 2 - (k + 0.5) * d / n, w, sh, d / n, 'paint2'); }
    for (const sx of [-1, 1]) { b.box(x + sx * (w / 2 + 1), y + hg / 2 + 1, z, 2, hg + 2, d, 'hull'); for (let k = 0; k < 4; k++) L([x + sx * (w / 2 + 1), y + (k + 1) * hg / 4 + 3, z + d / 2 - (k + 1) * d / 4]); }
  },
  lanterns(b, m, L) {
    const Rs = m.size[0], fy = m.p[1], lane = m.lane, r = b.r, cosO = Math.cos(0.8);
    for (let i = 0; i < m.n; i++) {
      let p = null;
      for (let t = 0; t < 20 && !p; t++) {
        const q = [r.range(-130, 130), r.range(fy + 8, 95), r.range(80, Rs * 1.25)], d = Math.hypot(q[0], q[1], q[2]);
        if (Math.hypot(q[0] - lane[0], q[1] - lane[1]) < 26 || d > Rs * 1.22) continue;
        if (d > Rs - 16 && q[2] / d < cosO) continue;   // not inside the shell wall
        p = q;
      }
      if (!p) continue;
      b.frame(p, [0, r() * TAU, 0], () => {
        b.lathe([0, -1.4, 0], [0, -0.4, 0], [[0, 0], [1.0, 0.25], [1.1, 0.5], [0, 0.5]], 6, 'trim', { flat: true });
        b.box(0, 0, 0, 1.2, 1.7, 1.2, 'lantern');
        for (const [sx, sz] of [[-1, -1], [1, -1], [-1, 1], [1, 1]]) b.edge([sx * 0.75, -0.95, sz * 0.75], [sx * 0.75, 0.95, sz * 0.75], 0.09, 'trim', 3);
        b.lathe([0, 0.9, 0], [0, 1.9, 0], [[1.6, 0], [1.3, 0.4], [0.3, 1.3], [0, 1.6]], 6, 'hull2', { flat: true });
        b.edge([0, 2.4, 0], [0, 6, 0], 0.05, 'metal', 3);
      });
      if (i % 2 === 0) L(p);
    }
  },
  // ---- Relitti: Gutterdeep, the hulk-town inside the freighter Patience ----------------------------------
  freighter(b, m, L) {
    // a bulging, dented elliptical hull inside the blueprint's section boxes (so its spheres stay conservative)
    const secs = m.secs, h2 = m.size[2] / 2, r = b.r, NJ = 30, dz0 = m.dz || 0;
    const bump = (u, z) => 1 + 0.022 * Math.sin(3 * u + z * 0.05) + 0.016 * Math.sin(5 * u - z * 0.11 + 1.3) - 0.012 * Math.cos(7 * u + z * 0.031);
    const S = (u, z) => { const s = secAt(secs, z), k = bump(u, z), cu = Math.cos(u), su = Math.sin(u); return [s[1] / 2 * cu * k * 0.97, s[2] / 2 * su * k * (su < 0 ? 0.92 : 0.97) + s[3], z]; };
    const N = (u, z) => { const e = 0.02, a = S(u - e, z), c = S(u + e, z), d = S(u, z - 1), f = S(u, z + 1); return unit3(cross3(sub3(c, a), sub3(f, d))) || [Math.cos(u), Math.sin(u), 0]; };
    const zs = []; for (let i = 0; i <= 40; i++) { const t = i / 40; zs.push(-h2 + (h2 * 2) * (0.5 - 0.5 * Math.cos(t * Math.PI))); }
    const rings = zs.map((z) => { const out = []; for (let j = 0; j < NJ; j++) out.push(S(j / NJ * TAU, z)); return out; });
    b.loft(rings, 'hull', { jit: 0.07, roleAt: (i, k) => { const u = (k + 0.5) / NJ * TAU; return Math.sin(u) < -0.55 ? 'hull2' : null; }, capRole0: 'hull2', capRole1: 'hull2' });
    // a curved plate following the hull: u0..u1 around, z0..z1 along, floating `lift` and `t` thick
    const plate = (u0, u1, z0, z1, lift, t, role) => {
      const nu = 2, nz = 2, T = [], B = [];
      for (let i = 0; i <= nz; i++) { const z = z0 + (z1 - z0) * i / nz, rowT = [], rowB = []; for (let j = 0; j <= nu; j++) { const u = u0 + (u1 - u0) * j / nu, p = S(u, z), n = N(u, z); rowB.push([p[0] + n[0] * lift, p[1] + n[1] * lift, p[2]]); rowT.push([p[0] + n[0] * (lift + t), p[1] + n[1] * (lift + t), p[2]]); } T.push(rowT); B.push(rowB); }
      const c = b.col(role), g = b.glow(role), mid = S((u0 + u1) / 2, (z0 + z1) / 2), mn = N((u0 + u1) / 2, (z0 + z1) / 2), ctr = [mid[0] - mn[0] * 30, mid[1] - mn[1] * 30, mid[2]];
      for (let i = 0; i < nz; i++) for (let j = 0; j < nu; j++) b.face([T[i][j], T[i][j + 1], T[i + 1][j + 1], T[i + 1][j]], ctr, role, c, g);
      const cc = [mid[0] + mn[0] * (lift + t / 2), mid[1] + mn[1] * (lift + t / 2), mid[2]];
      for (let j = 0; j < nu; j++) { b.face([B[0][j], B[0][j + 1], T[0][j + 1], T[0][j]], cc, role, c, g); b.face([B[nz][j], B[nz][j + 1], T[nz][j + 1], T[nz][j]], cc, role, c, g); }
      for (let i = 0; i < nz; i++) { b.face([B[i][0], B[i + 1][0], T[i + 1][0], T[i][0]], cc, role, c, g); b.face([B[i][nu], B[i + 1][nu], T[i + 1][nu], T[i][nu]], cc, role, c, g); }
    };
    const nearDock = (u, z) => Math.abs(Math.atan2(Math.sin(u), Math.cos(u))) < 0.62 && Math.abs(z - dz0) < 60;
    // weld seams, a hide of patched plates, rivet strips
    for (let z = -h2 + 26; z < h2 - 16; z += r.range(24, 36)) for (let j = 0; j < NJ; j++) { const u = j / NJ * TAU; if (!nearDock(u, z)) plate(u, (j + 1) / NJ * TAU, z, z + 1.2, 0.05, 0.5, 'dark'); }
    for (let i = 0; i < 240; i++) { const u0 = r() * TAU, z0 = r.range(-h2 + 14, h2 - 24); if (nearDock(u0, z0)) continue; const du = r.range(0.08, 0.3), dz = r.range(6, 24); plate(u0, u0 + du, z0, z0 + dz, r.range(0.05, 0.3), r.range(0.3, 1.0), r() < 0.5 ? 'patch' : r() < 0.55 ? 'paint2' : 'hull2'); }
    // lit openings with balconies and scaffolds (sodium light inside the hulk-town)
    for (let i = 0; i < 26; i++) {
      const side = r() < 0.5 ? 0 : Math.PI, u = side + r.range(-0.75, 0.75), du = r.range(0.1, 0.2), z0 = r.range(-h2 + 50, h2 - 70), dz = r.range(10, 24);
      if (nearDock(u, z0) || nearDock(u + du, z0 + dz)) continue;
      plate(u, u + du, z0, z0 + dz, 0.2, 0.4, 'dark');
      plate(u + du * 0.15, u + du * 0.85, z0 + dz * 0.15, z0 + dz * 0.85, 0.2, 0.55, r() < 0.7 ? 'lantern' : 'window');
      plate(u - 0.035, u - 0.005, z0 - 3, z0 + dz + 3, 0.2, 3.2, 'hull2');
      const at = (uu, zz, s) => { const p = S(uu, zz), n = N(uu, zz); return [p[0] + n[0] * s, p[1] + n[1] * s, p[2]]; };
      for (const zz of [z0 - 2, z0 + dz + 2]) { b.edge(at(u - 0.03, zz, 4), at(u + du + 0.03, zz, 4), 0.32, 'metal', 4); b.edge(at(u - 0.03, zz, 0), at(u - 0.03, zz, 4), 0.28, 'metal', 4); }
      b.edge(at(u - 0.03, z0 - 2, 4), at(u - 0.03, z0 + dz + 2, 4), 0.32, 'metal', 4); b.edge(at(u + du + 0.03, z0 - 2, 4), at(u + du + 0.03, z0 + dz + 2, 4), 0.32, 'metal', 4);
      L(at(u + du / 2, z0 + dz / 2, 7));
    }
    // stern: dead drive bells; aft bridge tower with sodium windows and masts; bow ram
    const top = (z) => S(Math.PI / 2, z)[1];
    for (const [x, y, rr] of [[-20, -10, 13], [20, -10, 13], [0, 16, 10]]) b.lathe([x, y, h2 - 24], [x, y, h2 - 23], [[0, 0], [rr * 0.9, 0], [rr, 4], [rr * 1.12, 22], [rr * 0.94, 22.5], [rr * 0.7, 8], [0, 7]], 16, 'metal', { roles: { 4: 'dark', 5: 'dark' } });
    const zb = h2 - 74, yb = top(zb);
    b.bbox(0, yb + 10, zb, 40, 26, 30, 'hull2', { ch: 2, roles: { top: 'patch' } });
    for (let k = 0; k < 8; k++) for (const sx of [-1, 1]) b.box(sx * 20.2, yb + 4 + (k % 2) * 8, zb - 11 + (k >> 1) * 7, 0.4, 2.2, 3.5, r() < 0.8 ? 'lantern' : 'dark');
    b.box(0, yb + 15, zb - 15.2, 30, 3, 0.4, 'window');
    for (const [x, hh] of [[-8, 26], [6, 34], [14, 18]]) { b.edge([x, yb + 23, zb + 6], [x, yb + 23 + hh, zb + 6], 0.4, 'metal', 4); L([x, yb + 24 + hh, zb + 6]); }
    b.lathe([0, S(0, -h2)[1], -h2 + 8], [0, S(0, -h2)[1], -h2 - 1], [[9, 0], [7, 8], [2, 14], [0, 15]], 8, 'metal', { flat: true });
    for (let k = 0; k < 12; k++) { const z = -h2 + 40 + k * (h2 * 2 - 80) / 11; L([0, top(z) + 3, z]); }
  },
  rcrane(b, m, L) {
    const [x, y, z] = m.p, [boom, mast] = m.size, Hm = mast * 0.55;
    let tip = null;
    b.frame([x, y, z], [0, m.a || 0, 0], () => {
      b.bbox(0, 3, 0, 10, 6, 10, 'hull2', { ch: 1 }); b.bbox(-3, 9, 3, 5, 6, 4, 'patch', { ch: 0.5 }); b.box(-3, 9.5, 5.05, 3.5, 1.4, 0.2, 'lantern');
      for (const [sx, sz] of [[-1, -1], [1, -1], [-1, 1], [1, 1]]) b.edge([sx * 2.2, 6, sz * 2.2], [sx * 1.2, 6 + Hm, sz * 1.2], 0.45, 'accent', 4);
      for (let k = 0; k < 6; k++) { const y0 = 6 + k * Hm / 6, y1 = y0 + Hm / 6; b.edge([-1.9, y0, 1.9], [1.9, y1, 1.9], 0.22, 'accent', 3); b.edge([1.9, y0, -1.9], [-1.9, y1, -1.9], 0.22, 'accent', 3); }
      const T = [0, 6 + Hm, 0], E = [boom, 6 + Hm + boom * 0.3, 0];
      b.edge(T, E, 0.9, 'accent', 4); b.edge([0, 6 + Hm - 3, 1.5], E, 0.5, 'accent', 4); b.edge([0, 6 + Hm - 3, -1.5], E, 0.5, 'accent', 4);
      b.edge([-boom * 0.25, 6 + Hm - 2, 0], T, 0.6, 'accent', 4); b.bbox(-boom * 0.25, 6 + Hm - 4, 0, 5, 5, 5, 'dark', { ch: 0.5 });
      const hang = E[1] - boom * 0.4;
      b.edge(E, [boom, hang + 5, 0], 0.12, 'dark', 3); b.bbox(boom, hang, 0, 8, 6, 16, 'patch', { ch: 0.4 });
      tip = E;
    });
    L(add3([x, y, z], rotY(tip, m.a || 0)));
  },
  stack(b, m, L) {
    const [x, y, z] = m.p, [w, h, d] = m.size, r = b.r, rows = Math.max(2, Math.round(h / 7));
    for (let k = 0; k < rows; k++) for (let j = 0; j < 3; j++) {
      const yy = y + 3.5 + k * 7, zz = z - d / 3 + j * d / 3, ww = w * r.range(0.6, 1.0);
      b.bbox(x + r.range(-2, 2), yy, zz, ww, 6.6, d / 3 - 0.6, r() < 0.3 ? 'accent' : 'patch', { ch: 0.3 });
      for (const sx of [-1, 1]) if (r() < 0.7) b.box(x + sx * (ww / 2 + 0.15), yy, zz, 0.3, 1.8, 2.6, r() < 0.75 ? 'lantern' : 'window');
    }
    b.cyl([x + w * 0.3, y + rows * 7, z], [x + w * 0.3, y + rows * 7 + 10, z], 1.2, 1.0, 6, 'metal'); L([x, y + rows * 7 + 2, z]);
  },
  moored(b, m, L) {
    const [x, y, z] = m.p, l = m.size[2], hp = m.hull;
    boat(b, [x, y, z], l, b.r() < 0.5 ? 'patch' : 'hull');
    const mid = [(x + hp[0]) / 2, (y + hp[1]) / 2 - 6, (z + hp[2]) / 2];
    for (const dz of [-l * 0.3, l * 0.3]) { b.edge([x, y + l * 0.1, z + dz], [mid[0], mid[1], mid[2] + dz], 0.3, 'dark', 3); b.edge([mid[0], mid[1], mid[2] + dz], [hp[0], hp[1], hp[2] + dz], 0.3, 'dark', 3); }
    L([x, y + l * 0.2, z]);
  },
};
// one ring-city district on the Gilda ring (local frame: x radial out, y up from the deck, z along the ring)
function gDistrict(b, type, W, arc, L) {
  const r = b.r, d = Math.min(arc * 0.78, 40), w = W * 0.8;
  const dome = (p, rd, role = 'hull2') => { b.lathe(p, add3(p, [0, 1, 0]), [[rd, 0], [rd, rd * 0.4], [rd * 0.95, rd * 0.5], [rd * 0.85, rd * 0.85], [rd * 0.62, rd * 1.15], [rd * 0.3, rd * 1.38], [rd * 0.1, rd * 1.45], [rd * 0.08, rd * 1.7], [0, rd * 1.9]], 16, role, { roles: { 0: 'paint2', 1: 'paint2' }, roleAt: (j, i) => (j === 0 && i % 2 ? 'window' : null) }); };
  const spire = (p, h, rr = 2) => { b.lathe(p, add3(p, [0, 1, 0]), [[0, 0], [rr, 0], [rr, h * 0.35], [rr * 0.75, h * 0.4], [rr * 0.15, h], [0, h * 1.04]], 8, 'paint2', { roles: { 3: 'hull2', 4: 'hull2' } }); };
  const winX = (h, rows, y0 = 3) => { for (let k = 0; k < rows; k++) for (let j = 0; j < Math.floor(d / 4); j++) for (const sx of [-1, 1]) b.box(sx * (w / 2 + 0.15), y0 + k * 4.5, -d / 2 + 2 + j * 4, 0.3, 2.2, 1.6, r() < 0.85 ? 'window' : 'dark'); };
  if (type === 0) {   // palace: block, cornice, drum and gold dome, corner spires
    const h = r.range(12, 18);
    b.bbox(0, h / 2, 0, w, h, d, 'hull', { ch: 0.8, roles: { top: 'paint2' } }); b.box(0, h + 0.4, 0, w + 1, 0.8, d + 1, 'hull2'); winX(h, 2);
    dome([0, h + 0.8, 0], Math.min(w, d) * 0.3);
    for (const [sx, sz] of [[-1, -1], [1, 1]]) spire([sx * (w / 2 - 2.5), h + 0.8, sz * (d / 2 - 2.5)], r.range(14, 22));
    L([0, h + Math.min(w, d) * 0.62, 0]);
  } else if (type === 1) {   // tower on a plinth with a brass spire
    const h = r.range(30, 52);
    b.bbox(0, 4, 0, w * 0.75, 8, d * 0.75, 'paint2', { ch: 0.6 });
    b.bbox(0, h / 2, 0, 12, h, 12, 'hull', { ch: 0.6 });
    for (const [sx, sz] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) b.box(sx * 6.15, h * 0.55, sz * 6.15, sx ? 0.3 : 1.4, h * 0.8, sz ? 0.3 : 1.4, 'window');
    b.box(0, h + 0.5, 0, 13.4, 1, 13.4, 'hull2'); spire([0, h + 1, 0], r.range(18, 26), 4.6); L([0, h + 28, 0]);
  } else if (type === 2) {   // arcade terrace with three small domes
    b.bbox(0, 5, 0, w, 10, d, 'paint2', { ch: 0.5 });
    for (let j = 0; j < Math.floor(d / 5); j++) for (const sx of [-1, 1]) { b.box(sx * (w / 2 + 0.1), 3.5, -d / 2 + 2.5 + j * 5, 0.3, 5, 2.6, 'dark'); b.box(sx * (w / 2 + 0.15), 8, -d / 2 + 2.5 + j * 5, 0.3, 1.2, 1.6, 'window'); }
    for (const zz of [-d / 3, 0, d / 3]) dome([0, 10, zz], 4.5);
  } else if (type === 3) {   // twin towers joined by a sky bridge
    const h = r.range(26, 40);
    for (const sz of [-1, 1]) { b.bbox(0, h / 2, sz * d * 0.3, 10, h, 10, 'hull', { ch: 0.5 }); b.box(5.15, h * 0.5, sz * d * 0.3, 0.3, h * 0.8, 1.2, 'window'); b.box(-5.15, h * 0.5, sz * d * 0.3, 0.3, h * 0.8, 1.2, 'window'); spire([0, h, sz * d * 0.3], r.range(10, 16), 3.6); L([0, h + 14, sz * d * 0.3]); }
    b.bbox(0, h * 0.7, 0, 6, 4, d * 0.6, 'paint2', { ch: 0.4 }); b.box(3.1, h * 0.7, 0, 0.3, 1.2, d * 0.55, 'window'); b.box(-3.1, h * 0.7, 0, 0.3, 1.2, d * 0.55, 'window');
  } else {   // great hall with a wide shallow dome and a lantern
    b.bbox(0, 6, 0, w, 12, d * 0.9, 'hull', { ch: 0.8, roles: { top: 'paint2' } }); winX(12, 2);
    b.lathe([0, 12, 0], [0, 13, 0], [[w * 0.42, 0], [w * 0.42, 1], [w * 0.36, 4], [w * 0.2, 7.5], [3, 9], [3, 12], [2, 12.5], [0, 14]], 20, 'hull2', { roles: { 4: 'window' } });
    L([0, 27, 0]);
  }
}
export function stationGeometry(THREE, st) {
  const base = PALETTES[st.faction] || PALETTES[F_GILDA];
  const pal = { ...base, ...(ST_EXTRA[st.faction] || {}), wear: (base.wear || 0) * 0.7 };
  const b = new Builder(pal, st.seed), lights = [], yaw = st.yaw || 0;
  const L = (p, c = pal.light) => { const q = rotY(p, yaw); lights.push([q[0], q[1], q[2], c[0], c[1], c[2], b.r()]); };
  b.frame([0, 0, 0], [0, yaw, 0], () => { for (const m of st.mods) { const f = STATION[m.kind]; if (f) f(b, m, L, st); } });
  return { geo: b.build(THREE), lights };
}

// The Costellatori beacon: a lattice lighthouse — stepped plinth, six twisting struts braced into a lattice and
// ringed every few storeys, a crystal column, and near the top the crystal heart inside three tilted rings.
// Lit: the heart and the seams burn gold. Dead: cold dark glass with a faint cyan seam. Everything stays inside
// the sim's volume (radius bc.radius, 0 <= y <= bc.height); `core` is the heart's centre.
export function beaconGeometry(THREE, bc) {
  const lit = !!bc.lit, H = bc.height, Rmax = bc.radius;
  const gold = [1, 0.78, 0.4], cyan = [0.36, 0.74, 0.95];
  const pal = { ...PALETTES[F_ECO], gloss: false, wear: 0.25, metals: ['hull2', 'trim'], hull: [0.2, 0.21, 0.24], hull2: [0.44, 0.41, 0.36], paint2: [0.3, 0.31, 0.34], trim: [0.72, 0.58, 0.3],
    light: lit ? gold : cyan, glow: lit ? gold : cyan, edge: lit ? [1, 0.82, 0.46] : [0.3, 0.62, 0.8], edge2: lit ? [1, 0.66, 0.3] : [0.42, 0.4, 0.7],
    glass: lit ? [0.95, 0.8, 0.5] : [0.16, 0.36, 0.5], crysA: lit ? [0.9, 0.72, 0.42] : [0.1, 0.24, 0.34], crysB: lit ? [0.8, 0.6, 0.35] : [0.14, 0.16, 0.3] };
  const b = new Builder(pal, 77), A = [0, 0, 0], Y = [0, 1, 0], seam = lit ? 'edge' : 'edge2';
  // plinth: stepped octagon with a glyph band and four buttresses
  b.lathe(A, Y, [[0, 0], [Rmax * 0.66, 0], [Rmax * 0.66, 5], [Rmax * 0.6, 6], [Rmax * 0.6, 10], [Rmax * 0.52, 11], [Rmax * 0.52, 15], [Rmax * 0.4, 17], [0, 17]], 8, 'paint2', { flat: true, phase: Math.PI / 8, roles: { 2: 'hull2', 4: 'hull2', 6: 'hull' } });
  b.lathe(A, Y, [[Rmax * 0.605, 6.6], [Rmax * 0.605, 9.4]], 32, lit ? 'glyph' : 'hull', { roleAt: (j, i) => (i % 2 ? 'hull2' : null) });
  for (let k = 0; k < 4; k++) b.at([0, 0, 0], [0, k * Math.PI / 2 + Math.PI / 4, 0]).bbox(Rmax * 0.62, 9, 0, 10, 18, 6, 'hull', { ch: 1.2, fy: 0.5, fyo: -3 }).reset();
  // the lattice: six struts twisting one turn up to the heart, braced diagonally, ringed every storey
  const y0 = 16, y1 = H * 0.66, n = 6, st = 9, R0 = Rmax * 0.56, R1 = Rmax * 0.2, tw = TAU * 0.55;
  const P = (k, i) => { const t = i / st, a = k / n * TAU + t * tw, r = R0 + (R1 - R0) * Math.pow(t, 0.8); return [Math.cos(a) * r, y0 + (y1 - y0) * t, Math.sin(a) * r]; };
  for (let i = 0; i < st; i++) for (let k = 0; k < n; k++) {
    const p = P(k, i), q = P(k, i + 1), q2 = P((k + 1) % n, i + 1);
    b.edge(p, q, 1.5, 'hull2', 4);
    b.edge(p, q2, 0.7, 'hull', 3);
    if (i % 2 === 0) b.edge(lerp3(p, q, 0.5), lerp3(P((k + 1) % n, i), q2, 0.5), 0.35, seam, 3);
  }
  for (let i = 1; i <= st; i++) { const t = i / st, r = R0 + (R1 - R0) * Math.pow(t, 0.8), y = y0 + (y1 - y0) * t; b.lathe([0, y, 0], [0, y + 1, 0], [[r - 1.2, -1.1], [r + 1.4, -1.1], [r + 1.4, 1.1], [r - 1.2, 1.1], [r - 1.2, -1.1]], 24, 'hull2', { flat: true, roles: { 1: lit ? 'trim' : 'hull2' } }); }
  for (let i = 1; i < st; i += 2) for (let k = 0; k < n; k++) { const p = P(k, i), d = unit3([p[0], 0, p[2]]); b.crystal(p, [p[0] + d[0] * 7, p[1] + 5, p[2] + d[2] * 7], 1.1, 4, 'crysA', { edge: seam, er: 0.12, k0: 0.4, k1: 0.7 }); }
  // the crystal column rising through the lattice
  b.crystal([0, y0, 0], [0, y1 + 20, 0], 6, 6, 'crysA', { edge: seam, er: 0.25, k0: 0.08, k1: 0.9, taper: 0.55, face2: 'crysB' });
  // the heart and its rings
  const cy = H * 0.8, ch = H * 0.24, cr = Math.min(18, Rmax * 0.26);
  b.crystal([0, cy - ch / 2, 0], [0, cy + ch / 2, 0], cr, 8, lit ? 'core' : 'glass', { edge: seam, er: 0.35, k0: 0.42, k1: 0.66, taper: 0.82, face2: lit ? 'glow' : 'crysB', phase: 0.2 });
  for (let k = 0; k < 6; k++) { const a = k / 6 * TAU + 0.5, p = [Math.cos(a) * cr * 0.7, cy - ch * 0.12, Math.sin(a) * cr * 0.7]; b.crystal(p, [p[0] * 2.1, cy + ch * 0.22, p[2] * 2.1], cr * 0.28, 5, lit ? 'glow' : 'crysA', { edge: seam, er: 0.18 }); }
  const RINGS = [[Rmax * 0.5, 0.35, 0.2], [Rmax * 0.58, -0.5, 1.4], [Rmax * 0.66, 0.22, 2.6]];
  for (const [rr, tilt, yawR] of RINGS) {
    b.frame([0, cy, 0], [tilt, yawR, 0], () => {
      b.lathe([0, 0, 0], [0, 1, 0], [[rr - 2.4, -1.6], [rr + 2.4, -1.6], [rr + 2.4, 1.6], [rr - 2.4, 1.6], [rr - 2.4, -1.6]], 48, 'hull2', { flat: true, roles: { 0: 'trim', 2: 'trim' } });
      b.lathe([0, 0, 0], [0, 1, 0], [[rr + 2.5, -0.6], [rr + 2.5, 0.6]], 48, lit ? 'lantern' : 'hull', { roleAt: (j, i) => (i % 3 ? 'hull2' : null) });
    });
  }
  // the crown: a fan of spikes above the heart, and the struts' finials holding the rings
  for (let k = 0; k < 6; k++) { const a = k / 6 * TAU, p = [Math.cos(a) * 6, cy + ch * 0.38, Math.sin(a) * 6]; b.crystal(p, [Math.cos(a) * 16, H - 4, Math.sin(a) * 16], 1.6, 4, 'hull2', { edge: seam, er: 0.15, k0: 0.2, k1: 0.5 }); }
  for (let k = 0; k < n; k++) b.edge(P(k, st), [Math.cos(k / n * TAU + tw) * Rmax * 0.48, cy - 6, Math.sin(k / n * TAU + tw) * Rmax * 0.48], 0.9, 'hull2', 4);
  for (let k = 0; k < 8; k++) { const a = k / 8 * TAU; b.lights([[Math.cos(a) * Rmax * 0.6, 12, Math.sin(a) * Rmax * 0.6]], 1.6, lit ? 'lantern' : 'winDim'); }
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
  for (let i = 0; i < n; i++) {
    const a = acc.get(keyOf(i)), l = Math.hypot(a[0], a[1], a[2]);
    if (l > 1e-12) { nrm[i * 3] = a[0] / l; nrm[i * 3 + 1] = a[1] / l; nrm[i * 3 + 2] = a[2] / l; }
    else { const p = Math.hypot(pos.getX(i), pos.getY(i), pos.getZ(i)) || 1; nrm[i * 3] = pos.getX(i) / p; nrm[i * 3 + 1] = pos.getY(i) / p; nrm[i * 3 + 2] = pos.getZ(i) / p; }
  }
  g.setAttribute('normal', new THREE.BufferAttribute(nrm, 3));
  CACHE.set(key, g);
  return g;
}
// Debris shards (a few irregular chunks, instanced by the FX layer).
export function debrisGeometry(THREE) {
  if (CACHE.has('debris')) return CACHE.get('debris');
  const b = new Builder({ hull: [0.6, 0.6, 0.6], hull2: [0.35, 0.35, 0.37], dark: [0.15, 0.15, 0.16], wear: 0.6 }, 9);
  b.bbox(0, 0, 0, 1, 0.25, 1.6, 'hull', { fx: 0.4, fy: 0.6, ch: 0.05 });
  b.bbox(0.4, 0.2, 0.3, 0.5, 0.5, 0.8, 'hull2', { fx: 0.5, ch: 0.06 });
  b.box(-0.2, -0.16, -0.2, 0.3, 0.08, 0.7, 'dark', { fx: 0.3 });
  const g = b.build(THREE);
  CACHE.set('debris', g);
  return g;
}

// The player's cockpit (camera-attached in cockpit view): canopy frame, dashboard, side consoles.
export function cockpitGeometry(THREE) {
  if (CACHE.has('cockpit')) return CACHE.get('cockpit');
  const pal = PALETTES[F_PLAYER];
  const b = new Builder({ ...pal, hull: [0.34, 0.33, 0.27], hull2: [0.16, 0.17, 0.15], panel: [0.1, 0.35, 0.3], wear: 0.3 }, 11);
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
// MeshStandardMaterial + vertex colours, with injections: object-space panel lines (anti-aliased, fading
// out before they alias), per-panel tint, grime streaks, edge wear (paint chipped to bare metal along the
// seams, amount from -aGlow), the aGlow channel as emissive light (engine roles scaled by uEngine), and a
// hit flash / scorch uniform. One shared program for every hull ('stelle-hull'): all knobs are uniforms in
// material.userData.U — uFlash, uFlashCol, uGlowMul, uEngine (engine glow: ~0.25 idle, 1 cruise, 2 boost),
// uPanel, uPanelK, uScorch, uRock, uBump, uNearFade.
export function hullMaterial(THREE, o = {}) {
  const m = new THREE.MeshStandardMaterial({ vertexColors: true, metalness: o.metal != null ? o.metal : 0.5, roughness: o.rough != null ? o.rough : 0.46, envMapIntensity: o.env != null ? o.env : 0.9 });
  const U = { uFlash: { value: 0 }, uFlashCol: { value: new THREE.Color(1, 1, 1) }, uGlowMul: { value: 1 }, uEngine: { value: o.engine != null ? o.engine : 1 }, uPanel: { value: o.panel || 0.55 }, uPanelK: { value: o.panelK != null ? o.panelK : 1 }, uScorch: { value: 0 },
    uRock: { value: o.rock ? 1 : 0 }, uBump: { value: o.bump != null ? o.bump : (o.rock ? 1.0 : 0.35) }, uNearFade: { value: o.nearFade || 0 } };
  m.userData.U = U;
  m.onBeforeCompile = (sh) => {
    Object.assign(sh.uniforms, U);
    sh.vertexShader = sh.vertexShader
      .replace('#include <common>', '#include <common>\nattribute float aGlow;\nvarying float vGlow;\nvarying vec3 vObj;\nvarying vec3 vObjN;')
      .replace('#include <begin_vertex>', '#include <begin_vertex>\nvGlow = aGlow; vObj = position; vObjN = objectNormal;');
    sh.fragmentShader = sh.fragmentShader
      .replace('#include <common>', `#include <common>
uniform float uFlash; uniform vec3 uFlashCol; uniform float uGlowMul; uniform float uEngine; uniform float uPanel; uniform float uPanelK; uniform float uScorch; uniform float uRock; uniform float uBump; uniform float uNearFade;
float hullH = 0.0, hullWear = 0.0, hullPaint = 1.0, hullBare = 0.0, hullGloss = 0.0;
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
  // aGlow decodes the surface: >= 0.5 glowing (engine roles 10+), (0, 0.5) glass, -1.5 bare metal, -2.5 glossy, [-1, 0] paint (wear)
  float lit = step(0.5, vGlow);
  hullBare = step(vGlow, -1.25) * step(-1.75, vGlow);
  hullGloss = clamp(step(vGlow, -2.25) + step(0.01, vGlow) * (1.0 - lit), 0.0, 1.0);
  hullPaint = clamp(1.0 - lit - hullBare - hullGloss, 0.0, 1.0);
  vec3 an = abs(vObjN) / max(length(vObjN), 1e-5);
  vec3 pp = vObj * uPanel;
  vec2 q = an.x > an.y && an.x > an.z ? pp.yz : (an.y > an.z ? pp.xz * vec2(1.0, 0.5) : pp.xy);
  vec2 cell = floor(q), f = abs(fract(q) - 0.5), fw = fwidth(q);
  vec2 ln = 1.0 - smoothstep(vec2(0.012), vec2(0.012) + fw * 1.5, 0.5 - f);
  float line = max(ln.x, ln.y) * (1.0 - lit) * clamp(1.6 - max(fw.x, fw.y) * 5.0, 0.0, 1.0);
  float tint = 0.9 + 0.16 * h21(cell);
  float grime = 0.86 + 0.14 * vn3(vObj * 0.35);
  float streak = 0.93 + 0.1 * vn3(vObj * vec3(2.6, 2.6, 0.16) + 9.0);
  diffuseColor.rgb *= mix(1.0, tint * grime * streak * (1.0 - line * 0.45), uPanelK);
  hullWear = clamp(-vGlow, 0.0, 1.0) * hullPaint * uPanelK * (1.0 - uRock) * smoothstep(0.43, 0.49, max(f.x, f.y) + 0.04 * h21(cell + 7.3)) * smoothstep(0.56, 0.8, vn3(vObj * 3.3 + 1.7));
  diffuseColor.rgb = mix(diffuseColor.rgb, vec3(0.42, 0.41, 0.39) * (0.8 + 0.4 * h21(cell + 3.1)), hullWear * 0.65);
  float sc = smoothstep(0.35, 0.75, vn3(vObj * 0.5 + 3.0)) * uScorch;
  diffuseColor.rgb *= 1.0 - sc * 0.85;
  if (uRock > 0.5) {   // rock: layered noise albedo + height for the bump below
    vec3 rp = vObj * 2.2;
    float n1 = vn3(rp), n2 = vn3(rp * 3.1 + 7.0), n3 = vn3(rp * 9.0 + 3.0);
    hullH = n1 * 0.55 + n2 * 0.3 + n3 * 0.15;
    diffuseColor.rgb *= 0.62 + 0.55 * hullH;
  } else hullH = -line * 0.6 + grime * 0.3 - hullWear * 0.25;
}`)
      .replace('#include <roughnessmap_fragment>', '#include <roughnessmap_fragment>\nfloat hullMk = max(hullBare, hullWear * 0.8);\nroughnessFactor = mix(mix(mix(roughnessFactor, max(roughnessFactor, 0.58), hullPaint), 0.4, hullMk), 0.14, hullGloss);')
      .replace('#include <metalnessmap_fragment>', '#include <metalnessmap_fragment>\nmetalnessFactor = mix(mix(metalnessFactor * mix(1.0, 0.35, hullPaint), 0.62, hullMk), 0.55, hullGloss);')
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
float gK = vGlow > 9.5 ? (vGlow - 10.0) * uEngine : max(vGlow, 0.0);
totalEmissiveRadiance += vColor.rgb * gK * uGlowMul + uFlashCol * uFlash;`);
  };
  m.customProgramCacheKey = () => 'stelle-hull';
  return m;
}

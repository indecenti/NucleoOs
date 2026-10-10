// stelle/props.js — procedural geometry for the surfaces: the flora kit of every biome and the sites you discover.
//
// Flora meshes carry  color (vertex albedo), aMat = (tint weight, glow, sway): tint 1 takes the planet's per-kind
// colour from the instance, glow is emissive (fruit, coral tips, crystal veins), sway bends with the wind.
// Site meshes are built for the hull material (kit.js): color + aGlow (>= 0.5 emits: the Costellatori's gold
// lines, lenses, lanterns). Everything is built in metres, base at y = 0, up = +Y; deterministic from a seed.
import { rng } from './world.js';

class G {
  constructor(site = false) { this.p = []; this.n = []; this.c = []; this.m = []; this.i = []; this.site = site; }
  v(x, y, z, nx, ny, nz, r, g, b, t = 0, gl = 0, sw = 0) {
    this.p.push(x, y, z); const l = Math.hypot(nx, ny, nz) || 1; this.n.push(nx / l, ny / l, nz / l); this.c.push(r, g, b);
    if (this.site) this.m.push(gl); else this.m.push(t, gl, sw);
    return this.p.length / 3 - 1;
  }
  tri(a, b, c) { this.i.push(a, b, c); }
  build(THREE) {
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(this.p, 3));
    g.setAttribute('normal', new THREE.Float32BufferAttribute(this.n, 3));
    g.setAttribute('color', new THREE.Float32BufferAttribute(this.c, 3));
    if (this.site) g.setAttribute('aGlow', new THREE.Float32BufferAttribute(this.m, 1));
    else g.setAttribute('aMat', new THREE.Float32BufferAttribute(this.m, 3));
    g.setIndex(this.i); g.computeBoundingSphere();
    return g;
  }
}
const lerp = (a, b, t) => a + (b - a) * t;
const norm = (v) => { const l = Math.hypot(v[0], v[1], v[2]) || 1; return [v[0] / l, v[1] / l, v[2] / l]; };
const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];

// a tube along a polyline (parallel-transported rings); col(t) -> [r,g,b, tint, glow, sway(optional)]
function tube(G0, pts, radii, sides, col, cap = true) {
  if (LO && pts.length > 3) { const keep = pts.map((_, k) => k % 2 === 0 || k === pts.length - 1); pts = pts.filter((_, k) => keep[k]); radii = radii.filter((_, k) => keep[k]); }
  if (LO) sides = Math.max(3, Math.ceil(sides / 2));
  let prevN = null; const rings = [];
  for (let k = 0; k < pts.length; k++) {
    const a = pts[Math.max(0, k - 1)], b = pts[Math.min(pts.length - 1, k + 1)];
    const T = norm([b[0] - a[0], b[1] - a[1], b[2] - a[2]]);
    let N = prevN ? prevN : (Math.abs(T[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0]);
    const B = norm(cross(T, N)); N = norm(cross(B, T)); prevN = N;
    const t = k / (pts.length - 1), cc = col(t), ring = [];
    for (let s = 0; s < sides; s++) {
      const ang = s / sides * Math.PI * 2, ca = Math.cos(ang), sa = Math.sin(ang);
      const nx = N[0] * ca + B[0] * sa, ny = N[1] * ca + B[1] * sa, nz = N[2] * ca + B[2] * sa, r = radii[k];
      ring.push(G0.v(pts[k][0] + nx * r, pts[k][1] + ny * r, pts[k][2] + nz * r, nx, ny, nz, cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, cc[5] != null ? cc[5] : t));
    }
    rings.push(ring);
  }
  for (let k = 0; k < rings.length - 1; k++) for (let s = 0; s < sides; s++) {
    const a = rings[k][s], b = rings[k][(s + 1) % sides], c = rings[k + 1][s], d = rings[k + 1][(s + 1) % sides];
    G0.tri(a, c, b); G0.tri(b, c, d);
  }
  if (cap) {
    const e = pts[pts.length - 1], ring = rings[rings.length - 1], cc = col(1), T = norm([e[0] - pts[pts.length - 2][0], e[1] - pts[pts.length - 2][1], e[2] - pts[pts.length - 2][2]]);
    const ci = G0.v(e[0] + T[0] * radii[radii.length - 1] * 0.6, e[1] + T[1] * radii[radii.length - 1] * 0.6, e[2] + T[2] * radii[radii.length - 1] * 0.6, T[0], T[1], T[2], cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, cc[5] != null ? cc[5] : 1);
    for (let s = 0; s < sides; s++) G0.tri(ring[s], ci, ring[(s + 1) % sides]);
  }
}
// a lumpy low-poly blob (icosahedron, one subdivision, displaced)
const ICO = (() => {
  const t = (1 + Math.sqrt(5)) / 2;
  let v = [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t], [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]].map(norm);
  let f = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8], [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1]];
  const mid = new Map(), m = (a, b) => { const k = a < b ? a * 100 + b : b * 100 + a; if (!mid.has(k)) { v.push(norm([(v[a][0] + v[b][0]) / 2, (v[a][1] + v[b][1]) / 2, (v[a][2] + v[b][2]) / 2])); mid.set(k, v.length - 1); } return mid.get(k); };
  const f2 = []; for (const [a, b, c] of f) { const ab = m(a, b), bc = m(b, c), ca = m(c, a); f2.push([a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]); }
  return { v, f: f2 };
})();
// the plain icosahedron (20 faces): tiny blobs (fruit, nuts) and the far LOD
const ICO0 = { v: ICO.v.slice(0, 12), f: [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8], [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1]] };
let LO = false;   // building the far LOD of a flora kind
function blob(G0, cx, cy, cz, rx, ry, rz, r, amp, col, flat = true) {
  const M = LO || Math.max(rx, ry, rz) < 0.05 ? ICO0 : ICO;
  const base = G0.p.length / 3, sh = M.v.map(() => 1 + (r() - 0.5) * amp);
  const P = M.v.map((d, k) => [cx + d[0] * rx * sh[k], cy + d[1] * ry * sh[k], cz + d[2] * rz * sh[k]]);
  if (flat) {   // faceted: one normal per face (rock, crystal)
    for (const [a, b, c] of M.f) {
      const A = P[a], B = P[b], C = P[c], n = norm(cross([B[0] - A[0], B[1] - A[1], B[2] - A[2]], [C[0] - A[0], C[1] - A[1], C[2] - A[2]]));
      const cc = col((A[1] + B[1] + C[1]) / 3 - cy);
      const i0 = G0.v(A[0], A[1], A[2], n[0], n[1], n[2], cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, cc[5] || 0);
      const i1 = G0.v(B[0], B[1], B[2], n[0], n[1], n[2], cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, cc[5] || 0);
      const i2 = G0.v(C[0], C[1], C[2], n[0], n[1], n[2], cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, cc[5] || 0);
      G0.tri(i0, i1, i2);
    }
  } else {
    M.v.forEach((d, k) => { const cc = col(P[k][1] - cy); G0.v(P[k][0], P[k][1], P[k][2], d[0], d[1], d[2], cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, cc[5] || 0); });
    for (const [a, b, c] of M.f) G0.tri(base + a, base + b, base + c);
  }
}
// an n-sided prism with a pointed (crystal) or flat top, axis from base point along dir
function prism(G0, x, y, z, dir, r, h, n, tip, col) {
  const d = norm(dir), a = norm(cross(d, Math.abs(d[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0])), b = cross(d, a);
  const top = [x + d[0] * h, y + d[1] * h, z + d[2] * h], apex = [top[0] + d[0] * tip, top[1] + d[1] * tip, top[2] + d[2] * tip];
  const ring = (c, rr) => { const o = []; for (let k = 0; k < n; k++) { const t = k / n * Math.PI * 2; o.push([c[0] + (a[0] * Math.cos(t) + b[0] * Math.sin(t)) * rr, c[1] + (a[1] * Math.cos(t) + b[1] * Math.sin(t)) * rr, c[2] + (a[2] * Math.cos(t) + b[2] * Math.sin(t)) * rr]); } return o; };
  const r0 = ring([x, y, z], r), r1 = ring(top, r * 0.92);
  for (let k = 0; k < n; k++) {
    const k1 = (k + 1) % n, A = r0[k], B = r0[k1], C = r1[k1], D = r1[k];
    const nn = norm(cross([B[0] - A[0], B[1] - A[1], B[2] - A[2]], [D[0] - A[0], D[1] - A[1], D[2] - A[2]]));
    const c0 = col(0), c1 = col(1);
    const i0 = G0.v(...A, ...nn, c0[0], c0[1], c0[2], c0[3] || 0, c0[4] || 0, 0), i1 = G0.v(...B, ...nn, c0[0], c0[1], c0[2], c0[3] || 0, c0[4] || 0, 0);
    const i2 = G0.v(...C, ...nn, c1[0], c1[1], c1[2], c1[3] || 0, c1[4] || 0, 0.5), i3 = G0.v(...D, ...nn, c1[0], c1[1], c1[2], c1[3] || 0, c1[4] || 0, 0.5);
    G0.tri(i0, i1, i2); G0.tri(i0, i2, i3);
    const E = apex, n2 = norm(cross([C[0] - D[0], C[1] - D[1], C[2] - D[2]], [E[0] - D[0], E[1] - D[1], E[2] - D[2]])), c2 = col(1.2);
    const j0 = G0.v(...D, ...n2, c1[0], c1[1], c1[2], c1[3] || 0, c1[4] || 0, 0.5), j1 = G0.v(...C, ...n2, c1[0], c1[1], c1[2], c1[3] || 0, c1[4] || 0, 0.5), j2 = G0.v(...E, ...n2, c2[0], c2[1], c2[2], c2[3] || 0, c2[4] || 0, 1);
    G0.tri(j0, j1, j2);
  }
}
function box(G0, cx, cy, cz, sx, sy, sz, ry, col) {
  const c = Math.cos(ry), s = Math.sin(ry), R = (x, z) => [cx + x * c + z * s, cz - x * s + z * c];
  const F = [[[1, 0, 0], [[1, -1, -1], [1, 1, -1], [1, 1, 1], [1, -1, 1]]], [[-1, 0, 0], [[-1, -1, 1], [-1, 1, 1], [-1, 1, -1], [-1, -1, -1]]],
    [[0, 1, 0], [[-1, 1, -1], [-1, 1, 1], [1, 1, 1], [1, 1, -1]]], [[0, -1, 0], [[-1, -1, 1], [-1, -1, -1], [1, -1, -1], [1, -1, 1]]],
    [[0, 0, 1], [[1, -1, 1], [1, 1, 1], [-1, 1, 1], [-1, -1, 1]]], [[0, 0, -1], [[-1, -1, -1], [-1, 1, -1], [1, 1, -1], [1, -1, -1]]]];
  for (const [n, q] of F) {
    const nx = n[0] * c + n[2] * s, nz = -n[0] * s + n[2] * c, cc = col(n[1]), ids = [];
    for (const v of q) { const [x, z] = R(v[0] * sx / 2, v[2] * sz / 2); ids.push(G0.v(x, cy + v[1] * sy / 2, z, nx, n[1], nz, cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, cc[5] || 0)); }
    G0.tri(ids[0], ids[1], ids[2]); G0.tri(ids[0], ids[2], ids[3]);
  }
}
// a curved leaf / frond strip (double-sided): from base along dir, bending down, width w
function leaf(G0, base, dir, len, w, droop, col, seg = 5) {
  const d = norm(dir), side = norm(cross(d, [0, 1, 0])), up = cross(side, d);
  const L = [], Rr = [];
  for (let k = 0; k <= seg; k++) {
    const t = k / seg, dd = droop * t * t;
    const px = base[0] + d[0] * len * t - up[0] * dd * len, py = base[1] + d[1] * len * t - up[1] * dd * len, pz = base[2] + d[2] * len * t - up[2] * dd * len;
    const ww = w * Math.sin(Math.PI * Math.min(1, t * 1.1 + 0.08)) * 0.5, cc = col(t);
    L.push(G0.v(px - side[0] * ww, py - side[1] * ww, pz - side[2] * ww, up[0], up[1], up[2], cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, t));
    Rr.push(G0.v(px + side[0] * ww, py + side[1] * ww, pz + side[2] * ww, up[0], up[1], up[2], cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, t));
  }
  for (let k = 0; k < seg; k++) { G0.tri(L[k], Rr[k], L[k + 1]); G0.tri(Rr[k], Rr[k + 1], L[k + 1]); G0.tri(L[k], L[k + 1], Rr[k]); G0.tri(Rr[k], L[k + 1], Rr[k + 1]); }
}

// ---- the flora kit (unit-ish size: scaled per instance; heights roughly 1) ---------------------------------------------
const K = (r, g, b, t = 0, gl = 0, sw) => [r, g, b, t, gl, sw];
// lod: the far version of a big kind (half the rings and sides, plain icosahedra, no small parts) for the skyline
export function floraGeometry(THREE, kind, lod = false) {
  LO = !!lod;
  try { return floraBuild(THREE, kind); } finally { LO = false; }
}
function floraBuild(THREE, kind) {
  const g = new G(), r = rng(0xF10A ^ kind.length * 977 ^ kind.charCodeAt(0) * 31);
  switch (kind) {
    case 'rock': blob(g, 0, 0.3, 0, 0.6, 0.45, 0.55, r, 0.5, (y) => K(0.78 + y * 0.2, 0.76 + y * 0.2, 0.74 + y * 0.2, 1, 0, 0)); break;
    case 'shrub': for (let k = 0; k < 4; k++) blob(g, r.range(-0.3, 0.3), 0.35 + r() * 0.2, r.range(-0.3, 0.3), 0.4, 0.32, 0.4, r, 0.6, () => K(0.8, 0.85, 0.7, 1, 0, 0.6), false); break;
    case 'hoodoo': {
      const pts = [], rad = []; for (let k = 0; k <= 6; k++) { pts.push([r.range(-0.03, 0.03), k / 6 * 0.86, r.range(-0.03, 0.03)]); rad.push(0.13 * (1.2 - Math.sin(k / 6 * Math.PI) * 0.35 + r.range(-0.05, 0.05))); }
      tube(g, pts, rad, 7, (t) => K(0.85 + t * 0.1, 0.62, 0.48, 1, 0, 0));
      blob(g, 0, 0.92, 0, 0.22, 0.09, 0.2, r, 0.3, () => K(0.62, 0.5, 0.44, 0, 0, 0)); break;
    }
    case 'rib': {   // a fossil rib arc: base in the sand, curling over
      const pts = [], rad = [];
      for (let k = 0; k <= 12; k++) { const t = k / 12, a = t * Math.PI * 0.82; pts.push([Math.sin(a) * 0.45, Math.sin(a) * 0.95 - t * t * 0.1, 0]); rad.push(0.05 * (1 - t * 0.7)); }
      tube(g, pts, rad, 6, () => K(0.93, 0.88, 0.76, 0, 0, 0));
      const p2 = pts.map(([x, y, z]) => [x * 0.9 - 0.08, y * 0.9, z + 0.32]); tube(g, p2, rad.map((x) => x * 0.85), 6, () => K(0.9, 0.85, 0.74, 0, 0, 0)); break;
    }
    case 'spire': {
      const pts = [], rad = []; for (let k = 0; k <= 8; k++) { const t = k / 8; pts.push([r.range(-0.02, 0.02) + t * t * 0.06, t, r.range(-0.02, 0.02)]); rad.push(0.12 * (1 - t * 0.85) + 0.004); }
      tube(g, pts, rad, 6, (t) => K(0.95 - t * 0.15, 0.7 - t * 0.1, 0.5, 1, 0, 0)); break;
    }
    case 'palm': {
      const pts = [], rad = []; for (let k = 0; k <= 8; k++) { const t = k / 8; pts.push([t * t * 0.18, t * 0.92, 0]); rad.push(0.028 * (1 - t * 0.4)); }
      tube(g, pts, rad, 6, (t) => K(0.45, 0.34, 0.24, 0, 0, t * 0.6));
      const top = pts[pts.length - 1];
      for (let k = 0; k < 8; k++) { const a = k / 8 * Math.PI * 2 + r() * 0.3; leaf(g, top, [Math.cos(a), 0.35, Math.sin(a)], 0.42, 0.13, 0.8, (t) => K(0.55, 0.85, 0.4, 1, 0, 0.7 + t * 0.3)); }
      for (let k = 0; k < 3; k++) blob(g, top[0] + r.range(-0.03, 0.03), top[1] - 0.03, top[2] + r.range(-0.03, 0.03), 0.025, 0.025, 0.025, r, 0.2, () => K(0.35, 0.25, 0.12, 0, 0, 0.7), false);
      break;
    }
    case 'coral': {
      for (let b = 0; b < 6; b++) {
        const a = r() * Math.PI * 2, lean = r.range(0.2, 0.7), h = r.range(0.5, 1), pts = [], rad = [];
        for (let k = 0; k <= 5; k++) { const t = k / 5; pts.push([Math.cos(a) * lean * t * 0.6, t * h, Math.sin(a) * lean * t * 0.6]); rad.push(0.06 * (1 - t * 0.6)); }
        tube(g, pts, rad, 5, (t) => K(0.55 + t * 0.4, 0.75 + t * 0.2, 1.0, 1, t > 0.75 ? 1.6 : 0.15, 0.2));
      }
      break;
    }
    case 'icespire':
      prism(g, 0, 0, 0, [0.04, 1, 0.02], 0.1, 0.8, 6, 0.2, (t) => K(0.78, 0.9, 1.0, 1, t > 1 ? 0.35 : 0.06, 0));
      prism(g, 0.12, 0, 0.05, [0.35, 1, 0.1], 0.06, 0.42, 6, 0.12, () => K(0.72, 0.86, 1.0, 1, 0.08, 0));
      prism(g, -0.09, 0, -0.08, [-0.3, 1, -0.2], 0.05, 0.3, 5, 0.1, () => K(0.75, 0.88, 1.0, 1, 0.08, 0)); break;
    case 'frost': for (let k = 0; k < 7; k++) { const a = r() * 6.28; prism(g, 0, 0, 0, [Math.cos(a) * 0.6, 1, Math.sin(a) * 0.6], 0.05, r.range(0.4, 0.8), 4, 0.15, () => K(0.85, 0.93, 1.0, 1, 0.05, 0.3)); } break;
    case 'spiral': {   // the jungle's spiral tree: a twisted trunk, a heavy canopy, teal fruit glowing in the mist
      const pts = [], rad = [];
      for (let k = 0; k <= 16; k++) { const t = k / 16, a = t * Math.PI * 3.2; pts.push([Math.cos(a) * 0.07 * (1 - t * 0.5), t * 0.78, Math.sin(a) * 0.07 * (1 - t * 0.5)]); rad.push(0.07 * (1.25 - t * 0.75)); }
      tube(g, pts, rad, 7, (t) => K(0.4, 0.31, 0.22, 0, 0, t * 0.3));
      for (let k = 0; k < 3; k++) { const a = k / 3 * 6.28 + r(); const b = [[0, 0.5, 0], [Math.cos(a) * 0.12, 0.62, Math.sin(a) * 0.12], [Math.cos(a) * 0.26, 0.7, Math.sin(a) * 0.26]]; if (!LO) tube(g, b, [0.03, 0.022, 0.012], 5, () => K(0.38, 0.3, 0.2, 0, 0, 0.5), false); }
      for (let k = 0; k < 7; k++) { const a = k / 7 * 6.28 + r() * 0.5, d = k ? 0.26 + r() * 0.08 : 0; blob(g, Math.cos(a) * d, 0.8 + r() * 0.12, Math.sin(a) * d, 0.32, 0.17, 0.32, r, 0.5, (y) => K(0.42 + y * 0.6, 0.72 + y * 0.5, 0.38, 1, 0, 0.8), false); }
      for (let k = 0; k < (LO ? 4 : 9); k++) { const a = r() * 6.28, d = r.range(0.08, 0.3); blob(g, Math.cos(a) * d, r.range(0.62, 0.78), Math.sin(a) * d, LO ? 0.045 : 0.028, LO ? 0.045 : 0.028, LO ? 0.045 : 0.028, r, 0.1, () => K(0.35, 1.0, 0.85, 0, 2.6, 0.8), false); }
      break;
    }
    case 'fern': for (let k = 0; k < 7; k++) { const a = k / 7 * 6.28 + r() * 0.4; leaf(g, [0, 0.02, 0], [Math.cos(a), r.range(0.9, 1.6), Math.sin(a)], 0.95, 0.28, 0.55, (t) => K(0.6 - t * 0.1, 0.9, 0.5, 1, 0, 0.4 + t * 0.6)); } break;
    case 'glowpod': {
      for (let k = 0; k < 3; k++) { const a = k / 3 * 6.28 + r(), x = Math.cos(a) * 0.15, z = Math.sin(a) * 0.15, h = r.range(0.45, 0.9);
        tube(g, [[x, 0, z], [x * 1.4, h * 0.6, z * 1.4], [x * 1.2, h, z * 1.2]], [0.025, 0.02, 0.015], 4, () => K(0.35, 0.55, 0.3, 1, 0, 0.6), false);
        blob(g, x * 1.2, h + 0.05, z * 1.2, 0.08, 0.1, 0.08, r, 0.2, () => K(0.4, 1.0, 0.8, 0, 2.2, 0.8), false); }
      break;
    }
    case 'basalt': for (let k = 0; k < 7; k++) { const a = k / 7 * 6.28, d = k ? 0.17 : 0; prism(g, Math.cos(a) * d, 0, Math.sin(a) * d, [0, 1, 0], 0.09, r.range(0.4, 1), 6, 0, () => K(0.32, 0.31, 0.32, 1, 0, 0)); } break;
    case 'deadtree': {
      tube(g, [[0, 0, 0], [0.03, 0.4, 0], [0.01, 0.8, 0.02], [0.04, 1, 0.01]], [0.06, 0.045, 0.03, 0.012], 5, () => K(0.16, 0.13, 0.12, 0, 0, 0));
      for (let k = 0; k < 4; k++) { const a = r() * 6.28, y = r.range(0.45, 0.85); tube(g, [[0, y, 0], [Math.cos(a) * 0.16, y + 0.12, Math.sin(a) * 0.16], [Math.cos(a) * 0.28, y + 0.12, Math.sin(a) * 0.28]], [0.025, 0.015, 0.006], 4, () => K(0.15, 0.12, 0.11, 0, 0, 0.2), false); }
      break;
    }
    case 'ember': blob(g, 0, 0.3, 0, 0.55, 0.38, 0.5, r, 0.6, (y) => (y > 0.05 && r() < 0.25 ? K(1.0, 0.45, 0.12, 0, 2.4, 0) : K(0.22, 0.2, 0.2, 1, 0, 0))); break;
    case 'shard':
      for (let k = 0; k < 6; k++) { const a = r() * 6.28; prism(g, Math.cos(a) * 0.08, 0, Math.sin(a) * 0.08, [Math.cos(a) * r.range(0.1, 0.7), 1, Math.sin(a) * r.range(0.1, 0.7)], r.range(0.05, 0.1), r.range(0.35, 0.9), 5, 0.12, (t) => K(0.62, 0.55, 1.0, 1, t > 0.9 ? 1.4 : 0.35, 0)); }
      break;
    case 'lattice': {
      prism(g, 0, 0, 0, [0.03, 1, 0], 0.035, 0.62, 5, 0.04, () => K(0.5, 0.45, 0.85, 1, 0.4, 0));
      for (let k = 0; k < 8; k++) { const a = r() * 6.28, y = r.range(0.5, 0.75); prism(g, 0, y, 0, [Math.cos(a), r.range(0.3, 1.2), Math.sin(a)], 0.03, r.range(0.15, 0.32), 4, 0.08, () => K(0.55, 0.85, 1.0, 1, 1.2, 0.2)); }
      break;
    }
    default: blob(g, 0, 0.3, 0, 0.5, 0.4, 0.5, r, 0.4, () => K(0.7, 0.7, 0.7, 1, 0, 0));
  }
  return g.build(THREE);
}
// big kinds are seen from further away (they shape the skyline)
export const FLORA_BIG = new Set(['hoodoo', 'rib', 'spire', 'icespire', 'spiral', 'basalt', 'lattice', 'palm']);

// ---- sites -------------------------------------------------------------------------------------------------------------
const STONE = [0.86, 0.82, 0.74], STONE_D = [0.62, 0.58, 0.52], GOLD = [1.0, 0.72, 0.32];
const S0 = (c, gl = 0) => () => [c[0], c[1], c[2], 0, gl];
// returns { geo, crystal?: geo (the relic, animated by the renderer), lights: [[x,y,z, r,g,b]], h: height }
export function siteGeometry(THREE, kind, seed, fac = 0, KIT = null) {
  const g = new G(true), r = rng(seed >>> 0), lights = [];
  const gl = (c, k) => () => [c[0], c[1], c[2], 0, k];
  let crystal = null, h = 30;
  // a sunken foundation: the pad's edge goes into the ground
  box(g, 0, -2.5, 0, 34, 6, 34, 0, S0(STONE_D));
  if (kind === 'gate') {   // the Gate of Threads: two pillars, a carved arch, a glyph ring, gold threads
    h = 52;
    for (const sx of [-1, 1]) { box(g, sx * 14, 20, 0, 7, 40, 8, 0, S0(STONE)); box(g, sx * 14, 1.5, 0, 10, 3, 11, 0, S0(STONE_D)); for (let k = 0; k < 5; k++) box(g, sx * 10.4, 6 + k * 7.5, 0, 0.4, 1.2, 5, 0, gl(GOLD, 2.2)); }
    const pts = [], rad = []; for (let k = 0; k <= 16; k++) { const a = Math.PI * k / 16; pts.push([Math.cos(a) * 14, 40 + Math.sin(a) * 12, 0]); rad.push(3.8); }
    tube(g, pts, rad, 6, S0(STONE), false);
    const ring = []; for (let k = 0; k <= 40; k++) { const a = Math.PI * 2 * k / 40; ring.push([Math.cos(a) * 9, 26 + Math.sin(a) * 9, 0]); }
    tube(g, ring, ring.map(() => 0.45), 5, gl(GOLD, 3.2), false);
    for (let k = 0; k < 12; k++) { const a = Math.PI * 2 * k / 12; tube(g, [[Math.cos(a) * 9, 26 + Math.sin(a) * 9, 0], [Math.cos(a) * 4, 26 + Math.sin(a) * 4, 0]], [0.18, 0.12], 4, gl(GOLD, 2.6), false); }
    for (let k = 0; k < 6; k++) { const a = r() * 6.28, d = r.range(18, 30); tube(g, [[Math.cos(a) * 12, 46, 0], [Math.cos(a) * d, 0.2, Math.sin(a) * d * 0.6]], [0.12, 0.08], 3, gl(GOLD, 2.0), false); }
    lights.push([0, 26, 0, 1.0, 0.75, 0.35]);
    for (let k = 0; k < 7; k++) blob(g, r.range(-30, 30), 0.5, r.range(-30, 30), r.range(1.5, 3.5), r.range(1, 2.5), r.range(1.5, 3.5), r, 0.4, S0(STONE_D));
  } else if (kind === 'archive') {   // the Silent Archive: a monolith with a portico, crystal tablets glowing inside
    h = 44;
    box(g, 0, 18, -8, 46, 36, 18, 0, S0(STONE_D));
    box(g, 0, 37, 2, 50, 4, 24, 0, S0(STONE));
    for (let k = 0; k < 6; k++) { const x = -20 + k * 8; tube(g, [[x, 0, 9], [x, 35, 9]], [1.6, 1.5], 8, S0(STONE), false); }
    box(g, 0, 10, 1.2, 10, 20, 0.6, 0, S0([0.05, 0.05, 0.06]));
    for (let k = 0; k < 10; k++) box(g, r.range(-3.5, 3.5), r.range(2, 16), 0.6, 0.9, 1.4, 0.2, 0, gl([0.5, 1.0, 0.9], 2.4));
    for (let k = 0; k < 3; k++) box(g, 0, 2 + k * 1.2, 6 + k * 1.5, 22 - k * 3, 1.2, 3, 0, S0(STONE));
    lights.push([0, 10, 3, 0.4, 1.0, 0.9]);
  } else if (kind === 'observatory') {   // the Last Observatory: a cracked dome, its lens still aimed at the sky
    h = 34;
    tube(g, [[0, 0, 0], [0, 12, 0]], [14, 13], 20, S0(STONE), false);
    const open = 0.9;
    for (let k = 0; k < 18; k++) {   // dome ribs and panels, a wedge missing
      const a0 = k / 18 * Math.PI * 2; if (Math.abs(((a0 + Math.PI) % (Math.PI * 2)) - Math.PI) < open * 0.5) continue;
      const pts = []; for (let j = 0; j <= 8; j++) { const b = j / 8 * Math.PI / 2; pts.push([Math.cos(a0) * Math.cos(b) * 13, 12 + Math.sin(b) * 13, Math.sin(a0) * Math.cos(b) * 13]); }
      tube(g, pts, pts.map(() => 1.6), 4, S0(k % 3 ? STONE : STONE_D), false);
    }
    const lc = [0, 18, 0], ld = norm([0.3, 1, 0.2]);
    tube(g, [lc, [lc[0] + ld[0] * 8, lc[1] + ld[1] * 8, lc[2] + ld[2] * 8]], [2.4, 3.2], 12, S0([0.4, 0.4, 0.42]), true);
    blob(g, lc[0] + ld[0] * 8.6, lc[1] + ld[1] * 8.6, lc[2] + ld[2] * 8.6, 3.0, 0.5, 3.0, r, 0.02, gl([0.55, 0.85, 1.0], 1.6), false);
    lights.push([lc[0] + ld[0] * 8.6, lc[1] + ld[1] * 8.6, lc[2] + ld[2] * 8.6, 0.5, 0.85, 1.0]);
    for (let k = 0; k < 2; k++) { const x = (k ? 1 : -1) * 18; tube(g, [[x, 0, 6], [x, 16, 6]], [0.25, 0.2], 4, S0([0.3, 0.3, 0.3]), false); box(g, x + 1.6, 13, 6, 3, 4, 0.1, 0, S0(k ? [0.75, 0.3, 0.2] : [0.25, 0.4, 0.7])); }
  } else if (kind === 'relic') {   // a shrine: stepped plinth, the relic crystal floating above it
    h = 14;
    for (let k = 0; k < 3; k++) box(g, 0, k * 1.4 + 0.7, 0, 14 - k * 4, 1.4, 14 - k * 4, k * 0.4, S0(k === 2 ? STONE : STONE_D));
    for (let k = 0; k < 4; k++) { const a = k / 4 * Math.PI * 2 + 0.785; tube(g, [[Math.cos(a) * 6, 0, Math.sin(a) * 6], [Math.cos(a) * 6, 7, Math.sin(a) * 6]], [0.5, 0.4], 6, S0(STONE), false); box(g, Math.cos(a) * 6, 7.2, Math.sin(a) * 6, 1.2, 0.6, 1.2, 0, gl(GOLD, 2.4)); }
    const cg = new G(true); prism(cg, 0, -1.4, 0, [0, 1, 0], 0.7, 1.4, 6, 1.2, () => [1.0, 0.85, 0.5, 0, 3.0]); prism(cg, 0, -1.4, 0, [0, -1, 0], 0.7, 0.01, 6, 1.4, () => [1.0, 0.85, 0.5, 0, 3.0]);
    crystal = cg.build(THREE); lights.push([0, 6.5, 0, 1.0, 0.8, 0.45]);
  } else if (kind === 'outpost') {
    h = 22;
    if (fac === 0) {   // Guild: a brass hab dome on a landing pad, a mast with a blue light
      box(g, 0, 0.3, 0, 26, 0.6, 26, 0, S0([0.35, 0.36, 0.4]));
      blob(g, -6, 2, -6, 7, 6, 7, r, 0.02, S0([0.78, 0.62, 0.32]), false); box(g, -6, 1, 1.5, 3, 2, 2, 0, gl([0.6, 0.8, 1.0], 1.4));
      tube(g, [[8, 0, -8], [8, 20, -8]], [0.35, 0.25], 5, S0([0.6, 0.6, 0.62]), true); lights.push([8, 20.5, -8, 0.4, 0.7, 1.4]);
      for (let k = 0; k < 4; k++) box(g, (k & 1 ? 1 : -1) * 12, 0.7, (k & 2 ? 1 : -1) * 12, 0.8, 0.4, 0.8, 0, gl([0.5, 0.7, 1.2], 2.0));
    } else if (fac === 2) {   // Wrecks: scrap shacks, a crane, a sodium light
      for (let k = 0; k < 4; k++) box(g, r.range(-10, 10), 2.5, r.range(-10, 10), r.range(4, 8), 5, r.range(4, 7), r() * 6.28, S0([0.42 + r() * 0.1, 0.27, 0.16]));
      tube(g, [[6, 0, 6], [6, 16, 6], [-4, 18, 6]], [0.4, 0.3, 0.2], 4, S0([0.5, 0.3, 0.15]), false); lights.push([-4, 17, 6, 1.4, 0.6, 0.2]);
      box(g, 0, 6, 0, 1, 1, 1, 0, gl([1.3, 0.55, 0.15], 2.2));
    } else {   // Keepers: a white tower with a lantern
      tube(g, [[0, 0, 0], [0, 16, 0]], [3.2, 2.4], 8, S0([0.9, 0.9, 0.86]), false);
      blob(g, 0, 17.5, 0, 2.2, 2.2, 2.2, r, 0.02, gl([1.0, 0.75, 0.35], 2.0), false); lights.push([0, 17.5, 0, 1.0, 0.75, 0.35]);
      box(g, 0, 0.6, 0, 12, 1.2, 12, 0.785, S0([0.7, 0.68, 0.62]));
    }
  } else if (kind === 'wreck') {
    h = 20;
    for (let k = 0; k < 10; k++) blob(g, r.range(-25, 25), 0.3, r.range(-25, 25), r.range(1, 4), r.range(0.5, 2), r.range(1, 4), r, 0.6, S0([0.22, 0.2, 0.19]));
  }
  return { geo: g.build(THREE), crystal, lights, h };
}

// stelle/props.js — procedural geometry for the surfaces: the flora kit of every biome and the sites you discover.
//
// Flora meshes carry  color (vertex albedo), aMat = (tint weight, glow, sway): tint 1 takes the planet's per-kind
// colour from the instance, glow is emissive (fruit, coral tips, crystal veins), sway bends with the wind.
// Site meshes are built for the hull material (kit.js): color + aGlow (>= 0.5 emits: the Costellatori's gold
// lines, lenses, lanterns). Everything is built in metres, base at y = 0, up = +Y; deterministic from a seed.
// Winding: every closed surface here is wound counter-clockwise seen from outside (front faces out), so it lights
// right under a DoubleSide flora material (which flips the normal on back faces) and survives FrontSide culling.
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
const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
const add = (a, b) => [a[0] + b[0], a[1] + b[1], a[2] + b[2]];
const scl = (a, k) => [a[0] * k, a[1] * k, a[2] * k];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const mad = (a, b, k) => [a[0] + b[0] * k, a[1] + b[1] * k, a[2] + b[2] * k];
const len3 = (a) => Math.hypot(a[0], a[1], a[2]);
const hash = (a, b = 0) => { const x = Math.sin(a * 127.1 + b * 311.7) * 43758.5453; return x - Math.floor(x); };
// an orthonormal frame round an axis: [D, A, B] with A x B = D (sides running A -> B turn counter-clockwise about D)
const basis = (d) => { const D = norm(d), A = norm(cross(D, Math.abs(D[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0])); return [D, A, cross(D, A)]; };
const rotX = (a) => (v) => [v[0], v[1] * Math.cos(a) - v[2] * Math.sin(a), v[1] * Math.sin(a) + v[2] * Math.cos(a)];
const rotY = (a) => (v) => [v[0] * Math.cos(a) + v[2] * Math.sin(a), v[1], -v[0] * Math.sin(a) + v[2] * Math.cos(a)];
const rotZ = (a) => (v) => [v[0] * Math.cos(a) - v[1] * Math.sin(a), v[0] * Math.sin(a) + v[1] * Math.cos(a), v[2]];
// one vertex from a colour record cc = [r, g, b, tint, glow, sway?]
const V = (G0, p, n, cc, sw) => G0.v(p[0], p[1], p[2], n[0], n[1], n[2], cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, sw != null ? sw : (cc[5] || 0));

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
    G0.tri(a, b, c); G0.tri(b, d, c);
  }
  if (cap) {
    const e = pts[pts.length - 1], ring = rings[rings.length - 1], cc = col(1), T = norm([e[0] - pts[pts.length - 2][0], e[1] - pts[pts.length - 2][1], e[2] - pts[pts.length - 2][2]]);
    const ci = G0.v(e[0] + T[0] * radii[radii.length - 1] * 0.6, e[1] + T[1] * radii[radii.length - 1] * 0.6, e[2] + T[2] * radii[radii.length - 1] * 0.6, T[0], T[1], T[2], cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, cc[5] != null ? cc[5] : 1);
    for (let s = 0; s < sides; s++) G0.tri(ring[s], ring[(s + 1) % sides], ci);
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
    M.v.forEach((d, k) => { const cc = col(P[k][1] - cy, k); G0.v(P[k][0], P[k][1], P[k][2], d[0] / rx, d[1] / ry, d[2] / rz, cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, cc[5] || 0); });
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
// a curved leaf / frond strip (double-sided): from base along dir, bending down, width w
function leaf(G0, base, dir, len, w, droop, col, seg = 5) {
  const d = norm(dir), side = norm(cross(d, [0, 1, 0])), up = cross(side, d);
  const L = [], Rr = [], Lb = [], Rb = [];   // front (normal up) and back (normal down) copies: lit right from both sides
  for (let k = 0; k <= seg; k++) {
    const t = k / seg, dd = droop * t * t;
    const px = base[0] + d[0] * len * t - up[0] * dd * len, py = base[1] + d[1] * len * t - up[1] * dd * len, pz = base[2] + d[2] * len * t - up[2] * dd * len;
    const ww = w * Math.sin(Math.PI * Math.min(1, t * 1.1 + 0.08)) * 0.5, cc = col(t);
    for (const [arr, sx, sn] of [[L, -1, 1], [Rr, 1, 1], [Lb, -1, -1], [Rb, 1, -1]]) arr.push(G0.v(px + sx * side[0] * ww, py + sx * side[1] * ww, pz + sx * side[2] * ww, up[0] * sn, up[1] * sn, up[2] * sn, cc[0], cc[1], cc[2], cc[3] || 0, cc[4] || 0, t));
  }
  for (let k = 0; k < seg; k++) { G0.tri(L[k], Rr[k], L[k + 1]); G0.tri(Rr[k], Rr[k + 1], L[k + 1]); G0.tri(Lb[k], Lb[k + 1], Rb[k]); G0.tri(Rb[k], Lb[k + 1], Rb[k + 1]); }
}

// ---- surfaces of rings, ribbons, crystals, fractured rock ----------------------------------------------------------------
// a flat convex polygon (Newell normal); with `ref` it is turned to face away from that point. cc: record or fn(n, centroid)
function poly(G0, P, cc, ref = null) {
  let n = [0, 0, 0];
  for (let i = 0; i < P.length; i++) { const a = P[i], b = P[(i + 1) % P.length]; n[0] += (a[1] - b[1]) * (a[2] + b[2]); n[1] += (a[2] - b[2]) * (a[0] + b[0]); n[2] += (a[0] - b[0]) * (a[1] + b[1]); }
  if (len3(n) < 1e-12) return null;
  n = norm(n);
  let c = [0, 0, 0]; for (const p of P) c = add(c, p); c = scl(c, 1 / P.length);
  if (ref && dot(n, sub(c, ref)) < 0) { P = P.slice().reverse(); n = scl(n, -1); }
  const k = typeof cc === 'function' ? cc(n, c) : cc, ids = P.map((p) => V(G0, p, n, k));
  for (let j = 1; j < ids.length - 1; j++) G0.tri(ids[0], ids[j], ids[j + 1]);
  return n;
}
// a closed grid of rings R[k][s]: sides run counter-clockwise about the axis and rings advance along it, so
// cross(d side, d ring) faces out. col(t, s, n, k) -> record; o.flat facets it, o.cap0 / o.cap1 close the ends
// (true = flat, a number = a cone of that height in ring radii), o.ts overrides the t of each ring.
function gridEmit(G0, R, T0, T1, col, o = {}) {
  const nk = R.length, ns = R[0].length, tOf = (k) => (o.ts ? o.ts[k] : k / (nk - 1));
  if (o.flat) {
    for (let k = 0; k < nk - 1; k++) for (let s = 0; s < ns; s++) {
      const s1 = (s + 1) % ns, A = R[k][s], B = R[k][s1], C = R[k + 1][s], D = R[k + 1][s1], ta = tOf(k), tb = tOf(k + 1);
      for (const [P0, P1, P2, t0, t1, t2] of [[A, B, C, ta, ta, tb], [B, D, C, ta, tb, tb]]) {
        const n = cross(sub(P1, P0), sub(P2, P0)), l = len3(n); if (l < 1e-12) continue;
        const m = scl(n, 1 / l), cc = col((t0 + t1 + t2) / 3, s, m, k), sw = cc[5];
        G0.tri(V(G0, P0, m, cc, sw != null ? sw : t0), V(G0, P1, m, cc, sw != null ? sw : t1), V(G0, P2, m, cc, sw != null ? sw : t2));
      }
    }
  } else {
    const ids = [];
    for (let k = 0; k < nk; k++) {
      const row = [], t = tOf(k), kn = Math.min(nk - 1, k + 1), kp = Math.max(0, k - 1);
      for (let s = 0; s < ns; s++) {
        let n = cross(sub(R[k][(s + 1) % ns], R[k][(s + ns - 1) % ns]), sub(R[kn][s], R[kp][s]));
        n = len3(n) < 1e-12 ? (k < nk / 2 ? scl(T0, -1) : T1) : norm(n);
        const cc = col(t, s, n, k);
        row.push(V(G0, R[k][s], n, cc, cc[5] != null ? cc[5] : t));
      }
      ids.push(row);
    }
    for (let k = 0; k < nk - 1; k++) for (let s = 0; s < ns; s++) {
      const a = ids[k][s], b = ids[k][(s + 1) % ns], c = ids[k + 1][s], d = ids[k + 1][(s + 1) % ns];
      G0.tri(a, b, c); G0.tri(b, d, c);
    }
  }
  if (o.cap1) capFan(G0, R[nk - 1], T1, col(tOf(nk - 1), -1, T1, nk - 1), o.cap1, false, tOf(nk - 1));
  if (o.cap0) capFan(G0, R[0], scl(T0, -1), col(tOf(0), -1, scl(T0, -1), 0), o.cap0, true, tOf(0));
}
function capFan(G0, ring, n, cc, h, rev, t) {
  let c = [0, 0, 0]; for (const p of ring) c = add(c, p); c = scl(c, 1 / ring.length);
  let rr = 0; for (const p of ring) rr += len3(sub(p, c)); rr /= ring.length;
  const sw = cc[5] != null ? cc[5] : t, ci = V(G0, mad(c, n, typeof h === 'number' ? h * rr : 0), n, cc, sw), ids = ring.map((p) => V(G0, p, n, cc, sw));
  for (let s = 0; s < ring.length; s++) { const a = ids[s], b = ids[(s + 1) % ring.length]; if (rev) G0.tri(b, a, ci); else G0.tri(a, b, ci); }
}
// a surface of revolution about axis d through c: prof = [[radius, height], ...] from the base up, n sides.
// o: rib (alternating radius: ribs / flutes), twist (radians over the height), phase, rmod(k, s, a) / hmod(k, s, a)
// (radius factor / height offset per vertex), shift(k) (ring centre offset), flat, cap0/cap1, loN (sides in the far LOD)
function lathe(G0, c, d, prof, n, col, o = {}) {
  if (LO && !o.keep) { n = Math.max(3, o.loN || Math.ceil(n / 2)); if (o.rib && n % 2) n++; }
  const [D, A, B] = basis(d), R = [], h0 = prof[0][1], H = (prof[prof.length - 1][1] - h0) || 1, ts = [];
  prof.forEach(([rad, h], k) => {
    const ring = [], t = (h - h0) / H, sh = o.shift ? o.shift(k, t) : null;
    ts.push(Math.max(0, Math.min(1, t)));
    for (let s = 0; s < n; s++) {
      const a = (o.phase || 0) + (s / n) * Math.PI * 2 + (o.twist || 0) * t;
      const rr = rad * (o.rib ? 1 + (s % 2 ? -o.rib : o.rib) : 1) * (o.rmod ? o.rmod(k, s, a) : 1), hh = h + (o.hmod ? o.hmod(k, s, a) : 0);
      const ca = Math.cos(a) * rr, sa = Math.sin(a) * rr;
      let p = [c[0] + D[0] * hh + A[0] * ca + B[0] * sa, c[1] + D[1] * hh + A[1] * ca + B[1] * sa, c[2] + D[2] * hh + A[2] * ca + B[2] * sa];
      if (sh) p = add(p, sh);
      ring.push(p);
    }
    R.push(ring);
  });
  gridEmit(G0, R, D, D, col, { ts, ...o });
  return R;
}
// a tube along a polyline (parallel-transported rings, outward winding); same options as lathe plus squash (an oval
// section) and rmod(k, s, a). Returns the rings.
function sweep(G0, pts, radii, n, col, o = {}) {
  if (LO && !o.keep) {
    if (pts.length > 3) { const keep = pts.map((_, k) => k % 2 === 0 || k === pts.length - 1); pts = pts.filter((_, k) => keep[k]); radii = radii.filter((_, k) => keep[k]); }
    n = Math.max(3, o.loN || Math.ceil(n / 2)); if (o.rib && n % 2) n++;
  }
  let prevN = null, T0 = null, T1 = null; const R = [];
  for (let k = 0; k < pts.length; k++) {
    const T = norm(sub(pts[Math.min(pts.length - 1, k + 1)], pts[Math.max(0, k - 1)])); if (!k) T0 = T; T1 = T;
    let N = prevN ? prevN : (Math.abs(T[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0]);
    const B = norm(cross(T, N)); N = norm(cross(B, T)); prevN = N;
    const t = k / (pts.length - 1), ring = [];
    for (let s = 0; s < n; s++) {
      const a = (o.phase || 0) + s / n * Math.PI * 2 + (o.twist || 0) * t, ca = Math.cos(a), sa = Math.sin(a) * (o.squash || 1);
      const rr = radii[k] * (o.rib ? 1 + (s % 2 ? -o.rib : o.rib) : 1) * (o.rmod ? o.rmod(k, s, a) : 1);
      ring.push([pts[k][0] + (N[0] * ca + B[0] * sa) * rr, pts[k][1] + (N[1] * ca + B[1] * sa) * rr, pts[k][2] + (N[2] * ca + B[2] * sa) * rr]);
    }
    R.push(ring);
  }
  gridEmit(G0, R, T0, T1, col, o);
  return R;
}
// a double-sided ribbon (front and back carry their own normals): centre P, side unit vectors S, half widths W.
// o.fold lifts the midrib (a ridged section that catches the light), o.ruffle waves the edges.
function ribbon(G0, P, S, W, col, o = {}) {
  const n = P.length, fold = o.fold || 0, rows = [[], []];
  for (let k = 0; k < n; k++) {
    const T = norm(sub(P[Math.min(n - 1, k + 1)], P[Math.max(0, k - 1)])), up = norm(cross(S[k], T)), t = k / (n - 1), cc = col(t, k), sw = cc[5] != null ? cc[5] : t;
    const ru = o.ruffle ? (k % 2 ? 1 : -1) * o.ruffle * W[k] : 0;
    const l = mad(mad(P[k], S[k], -W[k]), up, ru), r = mad(mad(P[k], S[k], W[k]), up, ru);
    let pts = [l, r], nrm = [up, up];
    if (fold) { const m = mad(P[k], up, fold * W[k]); pts = [l, m, r]; nrm = [norm(cross(sub(m, l), T)), up, norm(cross(sub(r, m), T))]; }
    for (const side of [0, 1]) rows[side].push(pts.map((p, j) => V(G0, p, side ? scl(nrm[j], -1) : nrm[j], cc, sw)));
  }
  const w = fold ? 2 : 1, f = rows[0], b = rows[1];
  for (let k = 0; k < n - 1; k++) for (let j = 0; j < w; j++) {
    G0.tri(f[k][j], f[k][j + 1], f[k + 1][j]); G0.tri(f[k][j + 1], f[k + 1][j + 1], f[k + 1][j]);
    G0.tri(b[k][j], b[k + 1][j], b[k][j + 1]); G0.tri(b[k][j + 1], b[k + 1][j], b[k + 1][j + 1]);
  }
}
// a frond / blade: rises along dir, arches over and droops; a serrated edge reads as leaflets
function frond(G0, base, dir, len, w, droop, col, seg = 8, o = {}) {
  if (LO) seg = Math.max(3, Math.ceil(seg / 2));
  const d = norm(dir); let side = cross(d, [0, 1, 0]); side = len3(side) < 1e-4 ? [1, 0, 0] : norm(side);
  const P = [], S = [], W = [];
  for (let k = 0; k <= seg; k++) {
    const t = k / seg;
    P.push([base[0] + d[0] * len * t, base[1] + d[1] * len * t - droop * len * t * t, base[2] + d[2] * len * t]);
    S.push(side);
    W.push(w * 0.5 * Math.sin(Math.PI * Math.min(1, t * 1.05 + 0.06)) * (o.serr && !LO && k % 2 ? 0.5 : 1));
  }
  ribbon(G0, P, S, W, col, { fold: LO ? 0 : o.fold || 0 });
}
// a double-sided flat triangle (front faces up along cross(B - A, C - A), the back carries the reversed normal)
function dtri(G0, A, B, C, cc, sw = [0, 0, 0]) {
  const n = cross(sub(B, A), sub(C, A)), l = len3(n); if (l < 1e-12) return;
  const m = scl(n, 1 / l), b = scl(m, -1);
  G0.tri(V(G0, A, m, cc, sw[0]), V(G0, B, m, cc, sw[1]), V(G0, C, m, cc, sw[2]));
  G0.tri(V(G0, A, b, cc, sw[0]), V(G0, C, b, cc, sw[2]), V(G0, B, b, cc, sw[1]));
}
// a pinnate frond: a drooping rachis lined on both sides with pointed leaflets raked toward the tip (feathery)
function pinnate(G0, base, dir, len, w, droop, col, seg = 10, o = {}) {
  if (LO) seg = Math.max(4, Math.ceil(seg / 2));
  const d = norm(dir); let side = cross(d, [0, 1, 0]); side = len3(side) < 1e-4 ? [1, 0, 0] : norm(side);
  const P = []; for (let k = 0; k <= seg; k++) { const t = k / seg; P.push([base[0] + d[0] * len * t, base[1] + d[1] * len * t - droop * len * t * t, base[2] + d[2] * len * t]); }
  const rake = o.rake != null ? o.rake : 1.3, dip = o.dip != null ? o.dip : 0.4, sw0 = o.sw0 != null ? o.sw0 : 0.3;
  for (let k = 0; k < seg; k++) {
    const t = (k + 0.5) / seg, T = sub(P[k + 1], P[k]), up = norm(cross(side, norm(T))), W = w * (0.3 + 0.7 * Math.sin(Math.PI * Math.min(1, t * 1.05 + 0.12))), cc = col(t);
    const s0 = sw0 + (1 - sw0) * k / seg, s1 = sw0 + (1 - sw0) * (k + 1) / seg;
    for (const sg of [-1, 1]) {
      const tip = add(add(P[k], scl(side, sg * W)), add(scl(T, rake), scl(up, -W * dip)));
      if (sg > 0) dtri(G0, P[k], tip, P[k + 1], cc, [s0, s1, s1]); else dtri(G0, P[k], P[k + 1], tip, cc, [s0, s1, s1]);
    }
  }
}
// a crystal: an n-sided prism (flat facets) with a pointed tip, from base along dir. col(t, side, normal): t is 0 at
// the base ring, 1 at the shoulder, 1.2 at the apex. o: taper, twist, phase, jag (a broken top: no tip, jagged rim)
function xprism(G0, base, dir, r, h, n, tip, col, o = {}) {
  const [D, A, B] = basis(dir), tp = o.taper != null ? o.taper : 0.92, ph = o.phase || 0, tw = o.twist || 0, js = o.seed || 0;
  const top = mad(base, D, h), apex = mad(top, D, tip);
  const ring = (c, rr, a0, j) => { const out = []; for (let k = 0; k < n; k++) { const a = a0 + k / n * Math.PI * 2; out.push(mad(add(c, add(scl(A, Math.cos(a) * rr), scl(B, Math.sin(a) * rr))), D, j ? (hash(k * 7.7 + js, j) - 0.5) * j : 0)); } return out; };
  const r0 = ring(base, r, ph, 0), r1 = ring(top, r * tp, ph + tw, o.jag || 0);
  const e = (P, t, s, nn) => { const cc = col(t, s, nn); return V(G0, P, nn, cc, cc[5] != null ? cc[5] : 0); };
  for (let k = 0; k < n; k++) {
    const k1 = (k + 1) % n, Ap = r0[k], Bp = r0[k1], Cp = r1[k1], Dp = r1[k], nn = norm(cross(sub(Bp, Ap), sub(Dp, Ap)));
    const i0 = e(Ap, 0, k, nn), i1 = e(Bp, 0, k, nn), i2 = e(Cp, 1, k, nn), i3 = e(Dp, 1, k, nn);
    G0.tri(i0, i1, i2); G0.tri(i0, i2, i3);
    if (tip > 0) { const n2 = norm(cross(sub(Cp, Dp), sub(apex, Dp))); G0.tri(e(Dp, 1, k, n2), e(Cp, 1, k, n2), e(apex, 1.2, k, n2)); }
  }
  if (tip <= 0) { const nn = norm(cross(sub(r1[1], r1[0]), sub(r1[2], r1[0]))), ids = r1.map((p) => e(p, 1, -1, nn)); for (let k = 1; k < n - 1; k++) G0.tri(ids[0], ids[k], ids[k + 1]); }
  return apex;
}
// a fractured rock: a displaced icosphere cut by planes (flat fracture faces), faceted; col(centroid, normal) per face.
// cuts = [[normal, distance], ...] in the rock's frame; o.floor flattens it on the ground, o.rot turns it, o.lo = 20 faces
function rock(G0, c, rad, rn, amp, cuts, col, o = {}) {
  const M = LO || o.lo ? ICO0 : ICO, sh = M.v.map(() => 1 + (rn() - 0.5) * amp);
  const P = M.v.map((d, k) => {
    let p = [d[0] * rad[0] * sh[k], d[1] * rad[1] * sh[k], d[2] * rad[2] * sh[k]];
    for (const [cn, cd] of cuts) { const nn = norm(cn), e = dot(p, nn) - cd; if (e > 0) p = mad(p, nn, -e); }
    if (o.floor != null && p[1] < o.floor) p[1] = o.floor;
    if (o.rot) p = o.rot(p);
    return add(p, c);
  });
  for (const [a, b, d] of M.f) {
    const A = P[a], B = P[b], C = P[d], n = cross(sub(B, A), sub(C, A)), l = len3(n); if (l < 1e-10) continue;
    const m = scl(n, 1 / l), cc = col(scl(add(add(A, B), C), 1 / 3), m);
    G0.tri(V(G0, A, m, cc), V(G0, B, m, cc), V(G0, C, m, cc));
  }
}
// move the vertices emitted since `start` (a sub-assembly): f maps a position, fn a normal
function xform(G0, start, f, fn = f) {
  for (let i = start * 3; i < G0.p.length; i += 3) {
    const p = f([G0.p[i], G0.p[i + 1], G0.p[i + 2]]), n = fn([G0.n[i], G0.n[i + 1], G0.n[i + 2]]);
    G0.p[i] = p[0]; G0.p[i + 1] = p[1]; G0.p[i + 2] = p[2]; G0.n[i] = n[0]; G0.n[i + 1] = n[1]; G0.n[i + 2] = n[2];
  }
}

// ---- the flora kit (unit-ish size: scaled per instance; heights roughly 1) ---------------------------------------------
const K = (r, g, b, t = 0, gl = 0, sw) => [r, g, b, t, gl, sw];
// a foliage mass: lit tops, deep undersides (the painterly read of a canopy from the ground and the air)
const crown = (lo, hi, rgb, tint = 1, sw = 0.5) => (y, ry) => { const k = lerp(lo, hi, Math.max(0, Math.min(1, 0.5 + y / (2 * ry)))); return K(rgb[0] * k, rgb[1] * k, rgb[2] * k, tint, 0, sw); };
// lod: the far version of a big kind (half the rings and sides, plain icosahedra, no small parts) for the skyline
export function floraGeometry(THREE, kind, lod = false) {
  LO = !!lod;
  try { return floraBuild(THREE, kind); } finally { LO = false; }
}
function floraBuild(THREE, kind) {
  const g = new G(), r = rng(0xF10A ^ kind.length * 977 ^ kind.charCodeAt(0) * 31);
  // shape noise draws from its own stream, so the far LOD (fewer vertices) keeps the near one's structure
  const rn = rng(0x5EED ^ kind.charCodeAt(1) * 131 ^ kind.length * 7919);
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
    case 'palm': {   // a ringed, curving trunk; a crown of folded, serrated fronds; a cluster of nuts
      const pts = [], rad = []; for (let k = 0; k <= 8; k++) { const t = k / 8; pts.push([t * t * 0.18, t * 0.92, 0]); rad.push(0.028 * (1 - t * 0.4) + (k === 0 ? 0.012 : 0)); }
      sweep(g, pts, rad, 6, (t, s) => { const k = (Math.floor(t * 16) % 2 ? 0.72 : 1) * (s % 2 ? 0.9 : 1); return K(0.42 * k, 0.33 * k, 0.23 * k, 0, 0, t * 0.6); }, { rmod: (k) => (k % 2 ? 0.92 : 1.06) });
      const top = pts[pts.length - 1];
      for (let k = 0; k < 10; k++) {
        const a = k / 10 * Math.PI * 2 + r() * 0.3, el = k % 3 === 0 ? 0.7 : 0.28;
        pinnate(g, top, [Math.cos(a), el, Math.sin(a)], 0.5, 0.15, 0.85, (t) => K(lerp(0.48, 0.72, t), lerp(0.78, 0.92, t), lerp(0.34, 0.42, t), 1, 0, 0.7 + t * 0.3), 10, { sw0: 0.7 });
      }
      if (!LO) for (let k = 0; k < 4; k++) blob(g, top[0] + r.range(-0.035, 0.035), top[1] - 0.035, top[2] + r.range(-0.035, 0.035), 0.026, 0.028, 0.026, r, 0.2, () => K(0.42, 0.3, 0.14, 0, 0, 0.7), false);
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

    // ---- the second kit: one signature silhouette more per biome ------------------------------------------------------
    case 'boulder': {   // a fractured boulder split by a crack, strata banding its faces, chips at its foot (tint: the rock)
      const strata = (sx) => (p, n) => { let s = 0.8 + 0.13 * Math.sin(p[1] * 31 + p[0] * 2.5) + 0.07 * Math.sin(p[1] * 87 + 1.7); if (n[0] * sx > 0.85) s *= 0.55; return K(s, s * 0.96, s * 0.9, 1, 0, 0); };
      rock(g, [-0.17, 0.27, 0.02], [0.37, 0.34, 0.43], rn, 0.3, [[[1, 0.08, 0.05], 0.155], [[-0.3, 1, 0.35], 0.25], [[-0.7, 0.35, -0.7], 0.3], [[0.1, -0.15, 1], 0.36]], strata(1), { floor: -0.3 });
      rock(g, [0.21, 0.22, -0.03], [0.31, 0.27, 0.37], rn, 0.3, [[[-1, -0.06, -0.05], 0.165], [[0.35, 1, -0.25], 0.2], [[0.75, 0.25, 0.7], 0.25]], strata(-1), { floor: -0.25 });
      for (let k = 0; k < 4; k++) { const a = k * 1.7 + r() * 0.8, d = r.range(0.5, 0.64), s = r.range(0.05, 0.1); rock(g, [Math.cos(a) * d, s * 0.35, Math.sin(a) * d], [s * 1.4, s, s * 1.15], rn, 0.4, [[[r.range(-0.5, 0.5), 1, r.range(-0.5, 0.5)], s * 0.45]], strata(0), { lo: true, floor: -s * 0.5 }); }
      break;
    }
    case 'cactus': {   // an alien columnar cactus: twisted ribs, upturned arms, glowing buds (warm pink) at the tips
      const body = (t, s) => { const c = s % 2 ? 0.5 : 1.0, b = 0.74 + t * 0.26; return K(c * b * 0.92, c * b, c * b * 0.88, 1, 0, 0); };
      const bud = (p, s) => lathe(g, p, [0, 1, 0], [[s * 0.45, -s * 0.5], [s, s * 0.35]], 6, (t, k, n, kk) => (kk ? K(1.0, 0.3, 0.1, 0, 1.3, 0) : K(1.0, 0.14, 0.36, 0, 1.2, 0)), { rib: 0.42, cap1: 0.9, keep: true });
      // segmented (constricted) like a stack of swollen joints, ribs spiralling up
      lathe(g, [0, 0, 0], [0, 1, 0], [[0.112, -0.04], [0.118, 0.3], [0.098, 0.36], [0.116, 0.43], [0.108, 0.64], [0.09, 0.7], [0.096, 0.78], [0.068, 0.92], [0.032, 0.97], [0, 0.985]], 10, body, { rib: 0.13, twist: 0.7 });
      bud([0, 0.975, 0], 0.045);
      const na = 1 + r.int(3);
      for (let i = 0; i < na; i++) {
        const a = i / na * 6.28 + r.range(-0.5, 0.5), y0 = r.range(0.26, 0.52), out = r.range(0.2, 0.27), up = r.range(0.16, 0.3) * (1.15 - y0 * 0.6), ca = Math.cos(a), sa = Math.sin(a);
        const P = [[ca * 0.05, y0, sa * 0.05], [ca * out, y0 + 0.06, sa * out], [ca * out * 1.02, y0 + up, sa * out * 1.02], [ca * out * 1.02, y0 + up + 0.05, sa * out * 1.02]];
        sweep(g, P, [0.07, 0.064, 0.056, 0.024], 8, body, { rib: 0.15 });
        bud([ca * out * 1.02, y0 + up + 0.055, sa * out * 1.02], 0.034);
      }
      break;
    }
    case 'juniper': {   // a gnarled, twisting dry tree: crooked limbs ending in flat dark-olive cloud pads (bonsai style)
      // silver weathered wood twisted with red-brown living bark
      const bark = (t, s) => { const k = 0.7 + t * 0.25; return (s + Math.floor(t * 9)) % 3 === 0 ? K(0.4 * k, 0.22 * k, 0.14 * k, 0, 0, t * 0.25) : K(0.5 * k, 0.48 * k, 0.44 * k, 0, 0, t * 0.25); };
      const lx = r.range(-0.12, 0.12), lz = r.range(-0.12, 0.12), pts = [], rad = [];
      for (let k = 0; k <= 7; k++) { const t = k / 7; pts.push([lx * t + Math.sin(t * 5.5 + 1) * 0.06 * t, t * 0.6, lz * t + Math.cos(t * 4.5) * 0.06 * t]); rad.push(0.06 * (1 - t * 0.5) + (k === 0 ? 0.04 : k === 1 ? 0.012 : 0)); }
      sweep(g, pts, rad, 7, bark, { twist: 3.2, rmod: (k, s) => 1 + (k < 2 ? (s % 2 ? 0.32 : -0.08) * (2 - k) * 0.5 : 0) + (hash(k, s) - 0.5) * 0.14, cap1: 0.4 });
      const pads = [[pts[7][0], 0.66, pts[7][2], 0.23], [pts[5][0] + 0.1, 0.5, pts[5][2] - 0.06, 0.13]];
      const nb = 2 + r.int(3);
      for (let i = 0; i < nb; i++) {
        const a = i / nb * 6.28 + r.range(-0.4, 0.4), k0 = 3 + r.int(4), p0 = pts[k0], ln = r.range(0.24, 0.36), ca = Math.cos(a), sa = Math.sin(a), kink = r.range(-0.06, 0.06);
        const P = [p0, [p0[0] + ca * ln * 0.38 - sa * kink, p0[1] + ln * 0.3, p0[2] + sa * ln * 0.38 + ca * kink], [p0[0] + ca * ln * 0.7 + sa * kink, p0[1] + ln * 0.28 + r.range(0, 0.08), p0[2] + sa * ln * 0.7 - ca * kink], [p0[0] + ca * ln, p0[1] + ln * 0.5, p0[2] + sa * ln]];
        sweep(g, P, [rad[k0] * 0.62, rad[k0] * 0.46, rad[k0] * 0.36, rad[k0] * 0.24], 5, bark, { cap1: 0.5 });
        pads.push([P[3][0], P[3][1] + 0.025, P[3][2], r.range(0.15, 0.21)]);
      }
      for (const [x, y, z, s] of pads) {
        const c = crown(0.28, 1.0, [0.62, 0.7, 0.42], 1, 0.5);
        blob(g, x, y, z, s, s * 0.36, s * 0.88, rn, 0.42, (yy) => c(yy, s * 0.36), false);
        blob(g, x + s * 0.35, y + s * 0.2, z - s * 0.2, s * 0.62, s * 0.3, s * 0.58, rn, 0.42, (yy) => c(yy + s * 0.12, s * 0.36), false);
      }
      break;
    }
    case 'kelp': {   // a clump of tall amber ribbon fronds rooted at the water line, twisting and waving as they rise
      const nf = 5 + r.int(4);
      for (let i = 0; i < nf; i++) {
        const a0 = r() * 6.28, d0 = r.range(0, 0.08), bx = Math.cos(a0) * d0, bz = Math.sin(a0) * d0, H = r.range(0.6, 1.0), ph = r() * 6.28, lean = r.range(0.06, 0.2), la = a0 + r.range(-0.6, 0.6), tw = r.range(1.8, 3.6) * r.sign(), w0 = r.range(0.065, 0.095), seg = 9;
        const P = [], S = [], W = [];
        for (let k = 0; k <= seg; k++) {
          const t = k / seg, wave = Math.sin(t * 7 + ph) * 0.05 * t;
          P.push([bx + Math.cos(la) * lean * t * t - Math.sin(la) * wave, t * H, bz + Math.sin(la) * lean * t * t + Math.cos(la) * wave]);
          const sa = a0 + tw * t; S.push([Math.cos(sa), 0, Math.sin(sa)]);
          W.push(w0 * (k === 0 ? 0.2 : Math.min(1, t * 4) * (1 - t * 0.5)) * (k === seg ? 0.2 : 1));
        }
        ribbon(g, P, S, W, (t) => K(lerp(0.2, 0.86, t * t), lerp(0.19, 0.6, t * t), lerp(0.04, 0.16, t), 0.5, 0, Math.pow(t, 1.3)), { ruffle: 0.3 });
        if (i < 3) blob(g, P[2][0], P[2][1], P[2][2], 0.024, 0.032, 0.024, r, 0.1, () => K(0.82, 0.58, 0.16, 0.3, 0, 0.25), false);
      }
      break;
    }
    case 'mangrove': {   // a shoreline tree on arching stilt roots, a dense rounded crown, a few drop roots
      const bark = (t) => K(0.3 + t * 0.06, 0.25 + t * 0.04, 0.19, 0, 0, 0.05 + t * 0.25);
      const top = [r.range(-0.04, 0.04), 0.64, r.range(-0.04, 0.04)];
      sweep(g, [[0, 0.24, 0], [0.008, 0.4, 0], [top[0] * 0.6, 0.53, top[2] * 0.6], top], [0.04, 0.038, 0.033, 0.028], 6, bark, { cap0: true });
      const nr = 6 + r.int(5);
      for (let i = 0; i < nr; i++) {
        const a = i / nr * 6.28 + r.range(-0.25, 0.25), ca = Math.cos(a), sa = Math.sin(a), h0 = r.range(0.26, 0.42), R0 = r.range(0.25, 0.42), apex = r.range(0.04, 0.1), P = [];
        for (let k = 0; k <= 5; k++) { const t = k / 5, d = 0.025 + (R0 - 0.025) * Math.pow(t, 0.8), y = h0 * (1 - t * t) + apex * Math.sin(t * Math.PI) * (1 - t) - 0.03 * t; P.push([ca * d, y, sa * d]); }
        sweep(g, P, P.map((_, k) => 0.017 - k * 0.0014), 4, (t) => K(0.3 - t * 0.08, 0.25 - t * 0.07, 0.19 - t * 0.05, 0, 0, 0.05));
      }
      const blobs = [[0, 0.82, 0, 0.3, 0.17]];
      for (let k = 0; k < 5; k++) { const a = k / 5 * 6.28 + r.range(-0.3, 0.3), d = r.range(0.2, 0.26); blobs.push([Math.cos(a) * d, r.range(0.7, 0.78), Math.sin(a) * d, r.range(0.18, 0.23), r.range(0.12, 0.15)]); }
      for (let k = 1; k < 4; k++) { const [x, y, z] = blobs[k * 2 - 1]; sweep(g, [top, [x * 0.5, y - 0.04, z * 0.5], [x * 0.8, y, z * 0.8]], [0.02, 0.014, 0.008], 4, bark); }
      const c = crown(0.3, 1.0, [0.62, 0.86, 0.5], 1, 0.45);
      for (const [x, y, z, s, sy] of blobs) blob(g, x, y, z, s, sy, s, rn, 0.45, (yy) => c(yy, sy), false);
      if (!LO) for (let k = 0; k < 4; k++) { const [x, y, z] = blobs[1 + k], f = 0.55; sweep(g, [[x * f, y - 0.08, z * f], [x * f * 1.05, (y - 0.08) * 0.5, z * f * 1.05], [x * f * 1.08, -0.02, z * f * 1.08]], [0.005, 0.005, 0.006], 3, () => K(0.36, 0.3, 0.24, 0, 0, 0.1)); }
      break;
    }
    case 'coralspire': {   // a large blue-violet branching coral formation; its tips glow cyan
      const cc = (term) => (t) => (term && t > 0.5 ? K(0.55, 1.0, 1.0, 0.2, 1.4, 0.15) : K(lerp(0.2, 0.42, t), lerp(0.18, 0.5, t), lerp(0.5, 0.98, t), 0.6, 0.04, 0.1));
      const br = (p, d, ln, rad, depth) => {
        const term = depth === 0 || (LO && depth === 1), L = LO && depth === 1 ? ln * 1.65 : ln;
        const e = add(p, scl(d, L)), m = add(add(p, scl(d, L * 0.5)), [r.range(-0.08, 0.08) * L, 0, r.range(-0.08, 0.08) * L]);
        if (!(LO && depth === 0)) sweep(g, [p, m, e], [rad, rad * 0.8, rad * (term ? 0.55 : 0.66)], depth >= 2 ? 5 : 4, cc(term), { cap1: term ? 0.9 : false });
        if (depth === 0) return;
        const nc = 2 + (r() < 0.4 ? 1 : 0);
        for (let c = 0; c < nc; c++) { const az = r() * 6.28, sp = r.range(0.4, 0.75); br(e, norm([d[0] + Math.cos(az) * sp, d[1] + 0.2, d[2] + Math.sin(az) * sp]), ln * r.range(0.62, 0.78), rad * 0.66, depth - 1); }
      };
      br([0, -0.02, 0], [0.02, 1, 0.01], 0.44, 0.07, 2);
      const nt = 3 + r.int(2);
      for (let i = 0; i < nt; i++) { const a = i / nt * 6.28 + r.range(-0.3, 0.3), lean = r.range(0.25, 0.6); br([Math.cos(a) * 0.05, -0.02, Math.sin(a) * 0.05], norm([Math.cos(a) * lean, 1, Math.sin(a) * lean]), r.range(0.28, 0.38), 0.055, 2); }
      blob(g, 0, 0.02, 0, 0.16, 0.08, 0.16, rn, 0.5, () => K(0.24, 0.2, 0.5, 0.6, 0.02, 0), true);
      break;
    }
    case 'bush': {   // a dense leafy shrub: overlapping speckled masses and a few blades breaking the outline
      const c = crown(0.22, 0.85, [0.62, 0.84, 0.48], 1, 0.45);
      blob(g, 0, 0.3, 0, 0.34, 0.28, 0.34, rn, 0.45, (yy) => c(yy, 0.28), false);   // the dense, shadowed core
      const nl = 18;
      for (let k = 0; k < nl; k++) {   // broad folded leaves over a dome (golden-angle spiral), lit from the top down
        const u = 1 - (k + 0.5) / nl * 1.15, a = k * 2.399 + r() * 0.4, q = Math.sqrt(Math.max(0, 1 - u * u)), f = 0.78 + r() * 0.34, yk = 0.6 + u * 0.4;
        const p = [Math.cos(a) * q * 0.27, 0.3 + u * 0.23, Math.sin(a) * q * 0.27], dir = [Math.cos(a) * q + r.range(-0.25, 0.25), u + 0.75, Math.sin(a) * q + r.range(-0.25, 0.25)];
        frond(g, p, dir, r.range(0.22, 0.3), r.range(0.15, 0.21), 0.3, (t) => K(0.62 * f * yk, 0.86 * f * yk, 0.48 * f * yk, 1, 0, 0.35 + t * 0.4), 2, { fold: 0.4 });
      }
      break;
    }
    case 'treefern': {   // a tall, slightly curved fibrous trunk; a crown of long arching serrated fronds; two fiddleheads
      const H = r.range(0.72, 0.78), cx = r.range(0.05, 0.1) * r.sign(), cz = r.range(-0.05, 0.05), pts = [], rad = [];
      for (let k = 0; k <= 7; k++) { const t = k / 7; pts.push([cx * t * t, t * H, cz * t * t]); rad.push(0.036 * (1.2 - t * 0.3) + (k === 0 ? 0.016 : 0)); }
      sweep(g, pts, rad, 6, (t, s) => { const k = (Math.floor(t * 15) % 2 ? 0.7 : 1) * (s % 2 ? 0.86 : 1); return K(0.24 * k, 0.18 * k, 0.12 * k, 0, 0, t * 0.35); }, { rmod: (k, s) => 1 + ((k + s) % 2 ? 0.16 : 0) });
      const top = pts[7];
      blob(g, top[0], top[1], top[2], 0.048, 0.034, 0.048, rn, 0.3, () => K(0.3, 0.24, 0.14, 0, 0, 0.35), false);
      const nf = 10 + r.int(3);
      for (let i = 0; i < nf; i++) {
        const a = i / nf * 6.28 + r.range(-0.2, 0.2), el = r.range(0.35, 0.9), f = 0.85 + r() * 0.25;
        pinnate(g, top, [Math.cos(a), el, Math.sin(a)], r.range(0.52, 0.64), r.range(0.1, 0.13), r.range(0.85, 1.15), (t) => K(lerp(0.46, 0.74, t) * f, lerp(0.74, 0.94, t) * f, lerp(0.32, 0.42, t) * f, 1, 0, 0.35 + t * 0.65), 12, { sw0: 0.35, rake: 1.1, dip: 0.3 });
      }
      if (!LO) for (let i = 0; i < 2; i++) {   // young fronds still coiled
        const a = i * 3.1 + r(), P = [], R0 = 0.035; for (let k = 0; k <= 6; k++) { const t = k / 6, b = t * 4.2; P.push([top[0] + Math.cos(a) * (0.02 + R0 * Math.sin(b) * t), top[1] + 0.02 + 0.07 * t + R0 * (1 - Math.cos(b)) * t * 0.5, top[2] + Math.sin(a) * (0.02 + R0 * Math.sin(b) * t)]); }
        sweep(g, P, P.map((_, k) => 0.009 - k * 0.0008), 4, () => K(0.5, 0.72, 0.36, 1, 0, 0.5), { cap1: true });
      }
      break;
    }
    case 'canopy': {   // the jungle's hero: a buttressed umbrella tree, two broad crown tiers, glowing teal pods beneath
      const fins = LO ? 2 : 3, nT = 5 * fins;   // five buttress fins: a ridge every `fins` sides of the trunk
      const lean = [r.range(-0.03, 0.03), r.range(-0.03, 0.03)];
      const prof = LO ? [[0.05, -0.03], [0.047, 0.08], [0.04, 0.3], [0.032, 0.64]] : [[0.05, -0.03], [0.048, 0.06], [0.045, 0.13], [0.042, 0.25], [0.038, 0.42], [0.034, 0.56], [0.031, 0.65]];
      const fl = LO ? [3.4, 1.6, 0.25, 0] : [3.4, 2.2, 0.9, 0.2, 0, 0, 0];
      lathe(g, [0, 0, 0], [0, 1, 0], prof, nT, (t, s) => { const k = 0.82 + (hash(s, 3) - 0.5) * 0.12; return t < 0.12 ? K(0.4 * k, 0.42 * k, 0.28 * k, 0, 0, 0) : K(0.58 * k, 0.53 * k, 0.45 * k, 0, 0, t * 0.15); },
        { keep: true, rmod: (k, s) => 1 + (s % fins === 0 ? fl[k] : fl[k] * 0.08), shift: (k, t) => [lean[0] * t * t, 0, lean[1] * t * t] });
      const tp = [lean[0], 0.62, lean[1]];
      // limbs into the lower tier
      const limbs = [];
      for (let i = 0; i < 5; i++) { const a = i / 5 * 6.28 + r.range(-0.3, 0.3), d = r.range(0.24, 0.36), ca = Math.cos(a), sa = Math.sin(a); limbs.push([ca * d, 0.75, sa * d]);
        sweep(g, [[tp[0], 0.52 + i * 0.02, tp[1]], [ca * d * 0.45, 0.66, sa * d * 0.45], [ca * d * 0.8, 0.72, sa * d * 0.8], [ca * d, 0.76, sa * d]], [0.022, 0.016, 0.011, 0.007], 5, () => K(0.52, 0.47, 0.4, 0, 0, 0.2)); }
      const c1 = crown(0.22, 1.0, [0.6, 0.82, 0.5], 1, 0.3), c2 = crown(0.3, 1.05, [0.66, 0.88, 0.52], 1, 0.35);
      blob(g, 0, 0.78, 0, 0.46, 0.085, 0.46, rn, 0.35, (y) => c1(y, 0.085), false);
      for (let i = 0; i < 6; i++) { const a = i / 6 * 6.28 + 0.6 + r.range(-0.3, 0.3), d = r.range(0.4, 0.48); blob(g, Math.cos(a) * d, 0.77 + r.range(-0.02, 0.02), Math.sin(a) * d, r.range(0.18, 0.24), 0.075, r.range(0.18, 0.24), rn, 0.45, (y) => c1(y, 0.075), false); }
      const ox = r.range(-0.06, 0.06), oz = r.range(-0.06, 0.06);
      sweep(g, [[tp[0], 0.6, tp[1]], [ox * 0.5, 0.78, oz * 0.5], [ox, 0.88, oz]], [0.02, 0.014, 0.008], 5, () => K(0.52, 0.47, 0.4, 0, 0, 0.3));
      blob(g, ox, 0.91, oz, 0.27, 0.07, 0.27, rn, 0.35, (y) => c2(y, 0.07), false);
      for (let i = 0; i < 3; i++) { const a = i / 3 * 6.28 + r.range(-0.3, 0.3), d = r.range(0.17, 0.22); blob(g, ox + Math.cos(a) * d, 0.9, oz + Math.sin(a) * d, r.range(0.12, 0.15), 0.06, r.range(0.12, 0.15), rn, 0.45, (y) => c2(y, 0.06), false); }
      // glowing pods hanging under the lower tier
      for (let i = 0; i < 8; i++) {
        const a = i / 8 * 6.28 + r.range(-0.2, 0.2), d = r.range(0.14, 0.4), x = Math.cos(a) * d, z = Math.sin(a) * d, hang = r.range(0.04, 0.09);
        if (LO && i % 2) continue;
        if (!LO) sweep(g, [[x, 0.73, z], [x, 0.73 - hang, z]], [0.003, 0.003], 3, () => K(0.3, 0.36, 0.22, 0, 0, 0.6));
        blob(g, x, 0.71 - hang, z, LO ? 0.03 : 0.022, LO ? 0.042 : 0.034, LO ? 0.03 : 0.022, rn, 0.15, () => K(0.35, 1.0, 0.85, 0, 2.2, 0.6), false);
      }
      if (!LO) for (let i = 0; i < 4; i++) {   // lianas from the crown's rim
        const a = i / 4 * 6.28 + r.range(-0.3, 0.3), d = r.range(0.4, 0.55), x = Math.cos(a) * d, z = Math.sin(a) * d, h = r.range(0.18, 0.4);
        sweep(g, [[x, 0.74, z], [x * 1.02, 0.74 - h * 0.6, z * 1.02], [x * 0.98, 0.74 - h, z * 0.98]], [0.004, 0.004, 0.003], 3, () => K(0.3, 0.44, 0.22, 0.5, 0, 0.7));
      }
      break;
    }
    case 'mushroom': {   // a cluster of bioluminescent mushrooms: pale stems, soft glowing gills and rims
      const n = 3 + r.int(3);
      for (let i = 0; i < n; i++) {
        const big = i === 0, s = big ? 1 : r.range(0.38, 0.7), a = i * 2.4 + r.range(-0.4, 0.4), d = big ? 0 : r.range(0.16, 0.3), x = Math.cos(a) * d, z = Math.sin(a) * d;
        const h = 0.6 * s * r.range(0.85, 1.15), cr = 0.26 * s * r.range(0.85, 1.15), sr = 0.034 * s + 0.008, lx = big ? 0.05 : Math.cos(a) * 0.3, lz = big ? 0.02 : Math.sin(a) * 0.3;
        const dir = norm([lx, 1, lz]), top = [x + dir[0] * h, dir[1] * h, z + dir[2] * h];
        lathe(g, [x, 0, z], dir, [[sr * 1.4, -0.03], [sr, h * 0.45], [sr * 0.85, h]], 4, (t) => K(0.84, 0.84, 0.8, 0.15, 0.1 + t * 0.2, t * 0.2), { keep: true });
        const ctr = mad(top, dir, -cr * 0.04), R = lathe(g, ctr, dir, big ? [[sr * 0.9, 0], [cr * 0.92, cr * 0.04], [cr, cr * 0.14], [cr * 0.78, cr * 0.42], [cr * 0.38, cr * 0.64], [0, cr * 0.7]] : [[sr * 0.9, 0], [cr, cr * 0.1], [cr * 0.74, cr * 0.42], [cr * 0.3, cr * 0.62], [0, cr * 0.66]], big ? 8 : 6,
          (t, s2, nn, k) => (k === 0 ? K(0.5, 0.95, 1.0, 0, 1.8, 0.2) : k === 1 ? K(0.6, 0.95, 1.0, 0, big ? 1.5 : 1.3, 0.2) : k === 2 && big ? K(0.62, 0.55, 1.0, 0.4, 0.9, 0.2) : K(0.32, 0.17, 0.72, 0.6, 0.3, 0.2)), { keep: true });
        // glowing freckles on the cap
        for (let j = 0; j < (big ? 7 : 3); j++) {
          const k = big ? 3 : 2, s2 = (j * 3 + i) % R[0].length, p = lerp3(lerp3(R[k][s2], R[k + 1][s2], 0.5), lerp3(R[k][(s2 + 1) % R[0].length], R[k + 1][(s2 + 1) % R[0].length], 0.5), 0.5);
          const nn = norm(sub(p, mad(ctr, dir, -cr * 0.2))), t1 = norm(cross(nn, dir)), t2 = cross(nn, t1), q = mad(p, nn, 0.004), sz = cr * 0.09;
          poly(g, [mad(q, t1, sz), mad(q, t2, sz), mad(q, t1, -sz), mad(q, t2, -sz)], K(0.6, 1.0, 1.0, 0, 1.6, 0.2), ctr);
        }
      }
      break;
    }
    case 'icecrystal': {   // a cluster of hexagonal ice prisms leaning out of a common base, pale and faintly lit inside
      const ice = (i) => (t, s, n) => {
        if (n[1] > 0.6) return K(0.96, 0.98, 1.0, 0.3, 0.04, 0);   // frost on the upward facets
        const h = hash(s * 3.1 + i * 7.7), f = 0.6 + h * 0.45, deep = (s + i) % 3 === 0;
        return deep ? K(0.38 * f, 0.62 * f, 0.95 * f, 1, 0.18, 0) : K(0.72 * f, 0.86 * f, 1.0 * f, 1, t > 1 ? 0.3 : 0.15, 0);
      };
      xprism(g, [0, -0.06, 0], [0.04, 1, 0.03], 0.13, 0.76, 6, 0.26, ice(0), { taper: 0.94, phase: 0.3 });
      const nc = 5 + r.int(4);
      for (let i = 1; i <= nc; i++) {
        const a = i / nc * 6.28 + r.range(-0.35, 0.35), lean = r.range(0.25, 1.0), rr = r.range(0.05, 0.1), h = r.range(0.22, 0.58) * (1.1 - lean * 0.35), broken = r() < 0.25;
        const base = [Math.cos(a) * 0.07, -0.05, Math.sin(a) * 0.07], dir = [Math.cos(a) * lean, 1, Math.sin(a) * lean];
        xprism(g, base, dir, rr, h, 6, broken ? 0 : rr * r.range(1.4, 2.2), ice(i), { taper: 0.9, phase: r() * 3, jag: broken ? rr * 0.8 : 0, seed: i });
      }
      if (!LO) {
        blob(g, 0, 0.0, 0, 0.26, 0.07, 0.24, rn, 0.5, () => K(0.9, 0.95, 1.0, 0.5, 0.02, 0), true);
        for (let i = 0; i < 4; i++) { const a = r() * 6.28, d = r.range(0.2, 0.3); xprism(g, [Math.cos(a) * d, -0.02, Math.sin(a) * d], [Math.cos(a) * 0.8, 1, Math.sin(a) * 0.8], 0.025, r.range(0.06, 0.12), 4, 0.04, ice(10 + i)); }
      } else for (let i = 0; i < 4; i++) { r(); r(); r(); }
      break;
    }
    case 'ashtree': {   // a dead tree under grey-white ash (upward faces), char-black elsewhere, a few ember cracks
      const ash = (t, s, n) => { const up = n[1]; if (up > 0.2) { const k = 0.6 + up * 0.34; return K(0.86 * k, 0.84 * k, 0.8 * k, 0.6, 0, t * 0.15); } return K(0.075, 0.066, 0.062, 0.6, 0, t * 0.15); };
      const pts = [], rad = [], lx = r.range(-0.1, 0.1), lz = r.range(-0.1, 0.1);
      for (let k = 0; k <= 7; k++) { const t = k / 7; pts.push([lx * t + Math.sin(t * 6 + 2) * 0.045 * t, t * 0.74, lz * t + Math.cos(t * 5) * 0.045 * t]); rad.push(0.06 * (1 - t * 0.55) + (k === 0 ? 0.032 : 0)); }
      const R = sweep(g, pts, rad, 7, ash, { flat: true, twist: 2.4, rmod: (k, s) => 1 + (k === 0 ? (s % 2 ? 0.4 : -0.05) : 0) + (hash(k * 3, s) - 0.5) * 0.22, cap1: r.range(-0.2, 0.6) });
      // a second leader splits off high on the trunk
      const p5 = pts[5], la = r() * 6.28, L2 = [p5, [p5[0] + Math.cos(la) * 0.06, p5[1] + 0.1, p5[2] + Math.sin(la) * 0.06], [p5[0] + Math.cos(la) * 0.1 + r.range(-0.03, 0.03), p5[1] + 0.2, p5[2] + Math.sin(la) * 0.1], [p5[0] + Math.cos(la) * 0.12, p5[1] + 0.28, p5[2] + Math.sin(la) * 0.12]];
      sweep(g, L2, [rad[5] * 0.7, rad[5] * 0.55, rad[5] * 0.42, rad[5] * 0.3], 5, ash, { flat: true, cap1: r.range(-0.3, 0.9) });
      const nb = 4 + r.int(2);
      for (let i = 0; i < nb; i++) {
        const a = i / nb * 6.28 + r.range(-0.5, 0.5), k0 = 2 + r.int(5), p0 = pts[k0], ln = r.range(0.2, 0.34) * (1.2 - k0 * 0.07), ca = Math.cos(a), sa = Math.sin(a), broken = r() < 0.4;
        const P = [p0, [p0[0] + ca * ln * 0.4, p0[1] + ln * 0.22, p0[2] + sa * ln * 0.4], [p0[0] + ca * ln * 0.72 + r.range(-0.05, 0.05), p0[1] + ln * 0.3, p0[2] + sa * ln * 0.72 + r.range(-0.05, 0.05)], [p0[0] + ca * ln, p0[1] + ln * (0.6 + r.range(-0.15, 0.25)), p0[2] + sa * ln]];
        const rr = rad[k0] * 0.58, Pb = broken ? P.slice(0, 3) : P, cap = broken ? r.range(-0.3, 0.8) : 0.6;
        sweep(g, Pb, [rr, rr * 0.75, rr * (broken ? 0.65 : 0.5), rr * 0.28].slice(0, Pb.length), 5, ash, { flat: true, cap1: cap });
        for (let j = 0; j < 2; j++) {   // crooked twigs (still drawn from the stream in the far LOD, so both agree)
          const q = P[1 + j + (broken ? 0 : 1)], b2 = a + r.range(-1.3, 1.3), l2 = ln * r.range(0.3, 0.5), up = r.range(0.2, 0.7);
          if (!LO) sweep(g, [q, [q[0] + Math.cos(b2) * l2 * 0.6, q[1] + l2 * up * 0.5, q[2] + Math.sin(b2) * l2 * 0.6], [q[0] + Math.cos(b2 + 0.4) * l2, q[1] + l2 * up, q[2] + Math.sin(b2 + 0.4) * l2]], [rr * 0.34, rr * 0.2, rr * 0.07], 3, ash, { flat: true, cap1: 0.4 });
        }
      }
      if (!LO) for (let i = 0; i < 8; i++) {   // ember cracks: thin glowing slits along the grain of the trunk
        const k = 1 + r.int(4), s = r.int(R[0].length), s1 = (s + 1) % R[0].length, a = R[k][s], b = R[k][s1], c = R[k + 1][s], d = R[k + 1][s1];
        const m0 = scl(add(a, b), 0.5), m1 = scl(add(c, d), 0.5), ctr = scl(add(add(a, b), add(c, d)), 0.25), ax = scl(add(pts[k], pts[k + 1]), 0.5), nn = norm(sub(ctr, ax)), sd = norm(sub(b, a));
        const p0 = mad(lerp3(m0, m1, 0.05), nn, 0.004), pm = mad(lerp3(m0, m1, 0.5), add(nn, scl(sd, 0.3)), 0.004), p1 = mad(lerp3(m0, m1, 0.95), nn, 0.004), w = 0.006;
        poly(g, [mad(p0, sd, -w * 0.3), mad(pm, sd, w), mad(p1, sd, w * 0.3), mad(pm, sd, -w)], K(1.0, 0.36, 0.06, 0, 1.8, 0.1), ax);
      }
      break;
    }
    case 'obsidian': {   // a cluster of black glass shards: sharp facets, sky glints, a faint red glow from the base
      const glass = (i) => (t, s, n) => { const hi = hash(i * 5.7 + s * 1.3) > 0.7 || n[1] > 0.8, g0 = Math.max(0, 0.85 - t * 1.3); return hi ? K(0.3 + g0 * 0.2, 0.3, 0.36, 0.3, g0 * 0.5, 0) : K(0.05 + g0 * 0.3, 0.045 + g0 * 0.03, 0.06 + g0 * 0.02, 0.3, g0 * 0.9, 0); };
      rock(g, [0, 0.03, 0], [0.32, 0.1, 0.28], rn, 0.4, [[[0, 1, 0], 0.05]], (p, n) => K(0.09, 0.08, 0.09, 0.4, 0, 0), { floor: -0.06 });
      const ns = 6 + r.int(3);
      for (let i = 0; i < ns; i++) {
        const a = i / ns * 6.28 + r.range(-0.4, 0.4), lean = i ? r.range(0.15, 0.8) : 0.08, d = i ? r.range(0.04, 0.16) : 0, rr = i ? r.range(0.04, 0.085) : 0.1;
        xprism(g, [Math.cos(a) * d, -0.02, Math.sin(a) * d], [Math.cos(a) * lean, 1, Math.sin(a) * lean], rr, (i ? r.range(0.2, 0.55) : 0.7) * (1.1 - lean * 0.3), r() < 0.5 ? 4 : 5, rr * r.range(1.6, 3.2), glass(i), { taper: r.range(0.55, 0.8), twist: r.range(-0.4, 0.4), phase: r() * 3 });
      }
      break;
    }
    case 'crystree': {   // a slender dark trunk crowned with a cloud of faceted crystal blades (cyan-blue, softly lit)
      const pts = [], rad = [], lx = r.range(-0.06, 0.06);
      for (let k = 0; k <= 7; k++) { const t = k / 7; pts.push([lx * t + Math.sin(t * 4) * 0.03, t * 0.66, Math.cos(t * 3.4) * 0.025 - 0.025]); rad.push(0.034 * (1 - t * 0.45) + (k === 0 ? 0.016 : 0)); }
      sweep(g, pts, rad, 5, (t, s) => K(0.12 + (s % 2) * 0.03, 0.1, 0.16 + (s % 2) * 0.03, 0, t > 0.5 && s === 2 ? 0.5 : 0, t * 0.15), {});
      const cy = 0.8, C = [pts[7][0], cy, pts[7][2]];
      for (let i = 0; i < 4; i++) { const a = i / 4 * 6.28 + r.range(-0.4, 0.4), p0 = pts[4 + (i % 3)]; sweep(g, [p0, [C[0] + Math.cos(a) * 0.1, cy - 0.06, C[2] + Math.sin(a) * 0.1], [C[0] + Math.cos(a) * 0.2, cy + 0.02, C[2] + Math.sin(a) * 0.2]], [0.014, 0.009, 0.005], 4, () => K(0.14, 0.11, 0.18, 0, 0, 0.4)); }
      const nb = 60;
      for (let i = 0; i < nb; i++) {
        const u = r() * 2 - 1, a = r() * 6.28, q = Math.sqrt(1 - u * u), dr = Math.pow(r(), 0.35), dir = [q * Math.cos(a), u * 0.75, q * Math.sin(a)];
        const p = [C[0] + dir[0] * 0.3 * dr, cy + dir[1] * 0.22 * dr, C[2] + dir[2] * 0.3 * dr], h = r.range(0.06, 0.12), rr = r.range(0.016, 0.026), tint = r(), tipL = r.range(0.04, 0.07), up = r.range(0.2, 0.8);
        if (LO && i % 2) continue;
        const k = LO ? 1.35 : 1, col = (t) => { const v = tint < 0.15 ? [0.66, 0.5, 1.0] : tint < 0.6 ? [0.42, 0.8, 1.0] : [0.55, 0.92, 1.0]; return K(v[0] * (0.7 + t * 0.3), v[1] * (0.7 + t * 0.3), v[2], 0.6, t > 1 ? 1.0 : 0.55, 0.3); };
        xprism(g, p, [dir[0], dir[1] + up, dir[2]], rr * k, h * k, 3, tipL * k, col, { taper: 0.8, phase: a });
      }
      break;
    }
    case 'geode': {   // a geode split in two: rock half-shells tipped open back to back, banded rims, glowing crystal linings
      const half = (ox, oz, yaw, sc, i0) => {
        const start = g.p.length / 3;
        lathe(g, [0, 0, 0], [0, 1, 0], [[0.02, -0.02], [0.32, 0.03], [0.4, 0.18], [0.34, 0.36], [0.26, 0.4], [0.27, 0.28], [0.2, 0.14], [0.02, 0.1]], 8,
          (t, s, nn, k) => (k <= 2 ? K(0.52, 0.48, 0.46, 1, 0, 0) : k <= 4 ? (s % 2 ? K(0.92, 0.88, 0.96, 0.2, 0.1, 0) : K(0.7, 0.6, 0.85, 0.2, 0.15, 0)) : K(0.34, 0.18, 0.5, 0, 0.5, 0)),
          { flat: true, keep: true, hmod: (k, s) => (k === 3 || k === 4 ? (hash(s * 2.3 + i0, 1) - 0.5) * 0.14 - (s === 5 || s === 6 ? 0.08 : 0) : 0), rmod: (k, s) => 1 + (hash(k + i0, s) - 0.5) * 0.12 });
        for (let i = 0; i < 6; i++) {   // the lining: crystal points grow inward and up from the inner wall
          const a = i / 6 * 6.28 + r.range(-0.3, 0.3), y = r.range(0.14, 0.3), rr0 = 0.23 - (0.3 - y) * 0.2, b = [Math.cos(a) * rr0, y, Math.sin(a) * rr0];
          const dir = [-Math.cos(a) * r.range(0.6, 1), r.range(0.5, 1.1), -Math.sin(a) * r.range(0.6, 1)], cyan = r() < 0.4;
          xprism(g, b, dir, r.range(0.03, 0.045), r.range(0.05, 0.1), 4, r.range(0.035, 0.06), (t) => (cyan ? K(0.45, 0.9, 1.0, 0, 1.2 + t * 0.6, 0) : K(0.72, 0.42, 1.0, 0, 1.2 + t * 0.6, 0)), { taper: 0.85 });
        }
        const R = (v) => rotY(yaw)(rotX(0.55)(v));
        xform(g, start, (p) => add(R(scl(p, sc)), [ox, 0.11 * sc, oz]), R);
      };
      half(-0.04, -0.14, Math.PI, 1, 0);
      half(0.1, 0.2, 0.3, 0.82, 5);
      break;
    }
    default: blob(g, 0, 0.3, 0, 0.5, 0.4, 0.5, r, 0.4, () => K(0.7, 0.7, 0.7, 1, 0, 0));
  }
  return g.build(THREE);
}
const lerp3 = (a, b, t) => [lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)];
// big kinds are seen from further away (they shape the skyline)
export const FLORA_BIG = new Set(['hoodoo', 'rib', 'spire', 'icespire', 'spiral', 'basalt', 'lattice', 'palm',
  'juniper', 'mangrove', 'coralspire', 'treefern', 'canopy', 'icecrystal', 'ashtree', 'crystree']);

// ---- sites -------------------------------------------------------------------------------------------------------------
// Palettes (linear albedo). The Costellatori built in a pale warm ivory stone with verdigris bronze and candle-gold
// glyphs; the Guild in brass, ivory and royal blue; the Wreckers in rust, sodium orange and oil black; the Keepers in
// white stone, verdigris and candle gold. Site records are [r, g, b, 0, aGlow]: aGlow >= 0.5 emits, (0, 0.5) is glass,
// -1.5 bare metal, [-1, 0) worn paint (kit.js hullMaterial).
const IVORY = [0.8, 0.75, 0.64], IVORY_D = [0.5, 0.46, 0.4], VERD = [0.2, 0.46, 0.4], VERD_D = [0.1, 0.25, 0.21], GOLD = [1.0, 0.52, 0.14], DARK = [0.025, 0.025, 0.035];
const BRASS = [0.78, 0.55, 0.25], ROYAL = [0.08, 0.16, 0.52], WHITE = [0.88, 0.87, 0.82], RUST = [0.28, 0.13, 0.065], OIL = [0.045, 0.042, 0.04];
const SODIUM = [1.0, 0.48, 0.1], CANDLE = [1.0, 0.72, 0.36], TABLET = [0.45, 1.0, 0.9], LENS = [0.55, 0.85, 1.0], EARTH = [0.12, 0.1, 0.085];
const SC = (c, gl = 0) => [c[0], c[1], c[2], 0, gl];
const ID = (v) => v;
// an orientation: roll about z, then tilt about x, then yaw about y (yaw turns local +z toward (sin, 0, cos))
const Q = (yaw = 0, tx = 0, tz = 0) => { const a = rotZ(tz), b = rotX(tx), c = rotY(yaw); return (v) => c(b(a(v))); };
// carved stone: lit tops, shadowed undersides, a grime line at the foot, a little variation face to face
const stone = (c, gl = 0) => (n, p) => {
  let k = n[1] > 0.7 ? 1.05 : n[1] < -0.7 ? 0.55 : 0.92 + 0.08 * n[1];
  if (p[1] < 1.6) k *= 0.76 + Math.max(0, p[1]) * 0.15;
  k *= 0.95 + hash(p[0] * 0.37 + p[2] * 0.13, p[1] * 0.29) * 0.1;
  return [c[0] * k, c[1] * k, c[2] * k, 0, gl];
};
// painted / bare surfaces: a flat light falloff by facing, the material decides the rest
const flat = (c, gl = 0) => (n) => { const k = n[1] > 0.7 ? 1.04 : n[1] < -0.7 ? 0.62 : 0.95; return [c[0] * k, c[1] * k, c[2] * k, 0, gl]; };
// lathe / sweep adapters: (t, side, normal) -> a face colour at a nominal height
const at = (fn, y = 5) => (t, s, n) => fn(n, [0, y, 0]);
// a block's own tone: ivory that drifts warmer, cooler, lighter or darker from block to block
const tone = (r, c = IVORY, a = 0.08) => { const k = 1 + (r() - 0.5) * 2 * a, w = (r() - 0.5) * 0.05; return [c[0] * k + w, c[1] * k, c[2] * k - w]; };
const centroid = (P) => { let c = [0, 0, 0]; for (const p of P) c = add(c, p); return scl(c, 1 / P.length); };

// constellation glyphs: little star maps (dots joined by fine threads) inlaid along a band O + s U + t V (normal n)
function glyphs(G0, O, U, Vv, n, r, cc, o = {}) {
  const lu = len3(U), lv = len3(Vv); if (lu < 0.25 || lv < 0.25) return;
  const along = lu >= lv, L = along ? lu : lv, S = along ? lv : lu, ax = scl(along ? U : Vv, 1 / L), bx = scl(along ? Vv : U, 1 / S);
  const nc = Math.max(1, Math.round(L / (S * (o.aspect || 1)))), step = L / nc, lift = o.lift || 0.09, w = Math.max(0.035, S * (o.w || 0.035)), ref = mad(O, n, -10);
  const line = (a, b, ww, lf = lift) => { const d = norm(sub(b, a)), s = cross(n, d); poly(G0, [mad(mad(a, s, -ww), n, lf), mad(mad(b, s, -ww), n, lf), mad(mad(b, s, ww), n, lf), mad(mad(a, s, ww), n, lf)], cc, ref); };
  for (let i = 0; i < nc; i++) {
    const c0 = mad(mad(O, ax, step * (i + 0.5)), bx, S * 0.5), np = 3 + r.int(3), P = [];
    for (let k = 0; k < np; k++) P.push(mad(mad(c0, ax, (r() - 0.5) * step * 0.7), bx, (r() - 0.5) * S * 0.66));
    for (let k = 0; k < np - 1; k++) line(P[k], P[k + 1], w * 0.45);
    if (np > 3 && r() < 0.5) line(P[0], P[2], w * 0.45);
    for (const p of P) { const s = w * (1.3 + r() * 0.8), q = mad(p, n, lift * 1.15); poly(G0, [mad(q, ax, s), mad(q, bx, s), mad(q, ax, -s), mad(q, bx, -s)], cc, ref); }
  }
  if (o.rules) for (const t of [0.06, 0.94]) line(mad(O, bx, S * t), mad(mad(O, bx, S * t), ax, L), w * 0.4);
}
// a recessed panel replacing a flat face Q (4 corners): a frame, a bevelled recess, a darker floor, glyphs inlaid in it
function panelFace(G0, Q4, ref, sp, col) {
  let P = Q4, n = [0, 0, 0];
  for (let i = 0; i < 4; i++) { const a = P[i], b = P[(i + 1) % 4]; n[0] += (a[1] - b[1]) * (a[2] + b[2]); n[1] += (a[2] - b[2]) * (a[0] + b[0]); n[2] += (a[0] - b[0]) * (a[1] + b[1]); }
  n = norm(n); if (dot(n, sub(centroid(P), ref)) < 0) { P = P.slice().reverse(); n = scl(n, -1); }
  const U = sub(P[1], P[0]), Vv = sub(P[3], P[0]), lu = len3(U), lv = len3(Vv), f = (s, t) => add(P[0], add(scl(U, s), scl(Vv, t)));
  const bu = Math.min(0.42, sp.b / lu), bv = Math.min(0.42, (sp.bv || sp.b) / lv), eu = Math.min(0.45, bu + sp.d * 0.5 / lu), ev = Math.min(0.45, bv + sp.d * 0.5 / lv);
  const I = [f(bu, bv), f(1 - bu, bv), f(1 - bu, 1 - bv), f(bu, 1 - bv)], F = [f(eu, ev), f(1 - eu, ev), f(1 - eu, 1 - ev), f(eu, 1 - ev)].map((p) => mad(p, n, -sp.d));
  for (let i = 0; i < 4; i++) { const j = (i + 1) % 4; poly(G0, [P[i], P[j], I[j], I[i]], col); poly(G0, [I[i], I[j], F[j], F[i]], col); }
  poly(G0, F, sp.floor || col);
  if (sp.glyph) glyphs(G0, F[0], sub(F[1], F[0]), sub(F[3], F[0]), n, sp.glyph.r, sp.glyph.cc, sp.glyph);
}
// a chamfered block: centre c, half sizes h, bevel b, orientation q (a vector map), col(n, centroid) -> record.
// o.jit roughens the corners (weathered; seeded by o.seed); o.panels = { '+z': spec, '-x': spec, ... } recesses faces
function cbox(G0, c, h, b, q, col, o = {}) {
  const C = [], sd = o.seed || 0;
  for (let i = 0; i < 8; i++) {
    const sx = i & 1 ? 1 : -1, sy = i & 2 ? 1 : -1, sz = i & 4 ? 1 : -1;
    const j = o.jit ? [(hash(i, sd) - 0.5) * o.jit, (hash(i + 9, sd) - 0.5) * o.jit, (hash(i + 17, sd) - 0.5) * o.jit] : [0, 0, 0];
    const w = (v) => add(c, q(add(v, j)));
    C.push([w([sx * h[0], sy * (h[1] - b), sz * (h[2] - b)]), w([sx * (h[0] - b), sy * h[1], sz * (h[2] - b)]), w([sx * (h[0] - b), sy * (h[1] - b), sz * h[2]])]);
  }
  const R4 = [[0, 0], [1, 0], [1, 1], [0, 1]];
  for (let a = 0; a < 3; a++) for (const s of [0, 1]) {
    const a1 = (a + 1) % 3, a2 = (a + 2) % 3, P = R4.map(([u, v]) => C[(s << a) | (u << a1) | (v << a2)][a]), key = (s ? '+' : '-') + 'xyz'[a];
    if (o.panels && o.panels[key]) panelFace(G0, P, c, o.panels[key], col); else poly(G0, P, col, c);
  }
  if (b > 0) {
    for (let a = 0; a < 3; a++) { const a1 = (a + 1) % 3, a2 = (a + 2) % 3; for (const s1 of [0, 1]) for (const s2 of [0, 1]) { const i0 = (s1 << a1) | (s2 << a2), i1 = i0 | (1 << a); poly(G0, [C[i0][a1], C[i1][a1], C[i1][a2], C[i0][a2]], col, c); } }
    for (let i = 0; i < 8; i++) poly(G0, [C[i][0], C[i][1], C[i][2]], col, c);
  }
}
// a convex hexahedron from 8 corners (bit 1 = +x side, 2 = +y, 4 = +z): voussoirs, wedges, roofs
function hexa(G0, P, col) { const c = centroid(P); for (const f of [[0, 2, 6, 4], [1, 3, 7, 5], [0, 1, 5, 4], [2, 3, 7, 6], [0, 1, 3, 2], [4, 5, 7, 6]]) poly(G0, f.map((i) => P[i]), col, c); }
// a fluted column drum, both ends cut, axis dir through centre c
function drum(G0, c, dir, R, L, col, n = 16) {
  const d = norm(dir), b = mad(c, d, -L / 2);
  lathe(G0, b, d, [[R, 0], [R, L]], n, (t, s, nn) => col(nn, add(b, scl(d, t * L))), { keep: true, rib: 0.05, cap0: true, cap1: true });
}
// a fluted column on a square plinth: base moulding, entasis, echinus and abacus. o.broken (0..1) snaps the shaft at that
// fraction of H with a jagged top. Returns the top.
function column(G0, x, y0, z, H, R, col, o = {}) {
  cbox(G0, [x, y0 + R * 0.32, z], [R * 1.4, R * 0.32, R * 1.4], R * 0.1, ID, col);
  const yb = y0 + R * 0.64;
  lathe(G0, [x, yb, z], [0, 1, 0], [[R * 1.25, 0], [R * 1.28, R * 0.16], [R * 1.08, R * 0.3], [R * 1.02, R * 0.42]], 16, (t, s, n) => col(n, [x, yb + 1, z]), { keep: true });
  const h0 = yb + R * 0.42, top = o.broken ? y0 + H * o.broken : y0 + H - R * 0.85, prof = [];
  for (let k = 0; k <= 4; k++) { const t = k / 4; prof.push([R * (1 - 0.1 * t + 0.03 * Math.sin(t * Math.PI)), h0 + (top - h0) * t]); }
  lathe(G0, [x, 0, z], [0, 1, 0], prof, 20, (t, s, n) => col(n, [x, h0 + (top - h0) * t, z]), { keep: true, rib: 0.045, hmod: o.broken ? (k, s) => (k === 4 ? (hash(s * 1.7 + x, z) - 0.5) * R * 1.6 : 0) : null, cap1: o.broken ? 0.1 : false });
  if (o.broken) return top;
  lathe(G0, [x, top, z], [0, 1, 0], [[R * 0.9, 0], [R * 1.0, R * 0.12], [R * 1.3, R * 0.45]], 16, (t, s, n) => col(n, [x, top + 1, z]), { keep: true });
  cbox(G0, [x, top + R * 0.65, z], [R * 1.42, R * 0.2, R * 1.42], R * 0.06, ID, col);
  return top + R * 0.85;
}
// the paved court: a sunken foundation (its edge goes into the ground) under weathered, uneven slabs, a few missing
function court(G0, r, half, slab, o = {}) {
  cbox(G0, [0, -2.6, 0], [half + 0.4, 2.65, half + 0.4], 0.3, ID, stone(o.base || IVORY_D));
  const n = Math.round(half * 2 / slab), s = half * 2 / n;
  for (let i = 0; i < n; i++) for (let j = 0; j < n; j++) {
    const x = -half + (i + 0.5) * s, z = -half + (j + 0.5) * s, miss = r() < (o.miss != null ? o.miss : 0.05), t = tone(r, o.c || IVORY, 0.07), dy = r.range(-0.05, 0.06), tl = r.range(-0.015, 0.015);
    if (miss || (o.skip && o.skip(x, z))) continue;
    cbox(G0, [x, 0.25 + dy, z], [s / 2 - 0.08, 0.3, s / 2 - 0.08], 0.09, Q(0, tl, -tl), stone(t), { jit: 0.05, seed: i * 31 + j });
  }
}
// a debris field between radii r0..r1: fallen blocks (tilted, half buried), column drums, rubble. o.keep(x, z) vetoes spots
function debris(G0, r, n, r0, r1, o = {}) {
  const k = o.k || 1;
  for (let i = 0; i < n; i++) {
    const a = o.a0 != null ? o.a0 + r.range(-o.spread, o.spread) : r() * 6.2832, d = r0 + (r1 - r0) * Math.sqrt(r()), x = Math.cos(a) * d, z = Math.sin(a) * d, kind = r(), t = tone(r, o.c || IVORY, 0.1);
    const hx = r.range(0.5, 1.6) * k, hy = r.range(0.35, 1.0) * k, hz = r.range(0.5, 1.4) * k, fallen = r() < 0.35, yaw = r() * 6.2832, sink = r.range(0.2, 0.85), tx = r.range(-0.6, 0.6), tz = r.range(-0.15, 0.15);
    if (o.keep && !o.keep(x, z)) continue;
    if (kind < 0.55 || o.blocks) cbox(G0, [x, hy * sink, z], [hx, hy, hz], Math.min(hx, hy, hz) * 0.15, Q(yaw, fallen ? tx : tx * 0.2, tz), (o.col || stone)(t), { jit: 0.14 * k, seed: i + (o.seed || 0) });
    else if (kind < 0.75) drum(G0, [x, hx * 0.75, z], [Math.cos(yaw), tz * 0.5, Math.sin(yaw)], hx * 0.85, hz * 1.6, (o.col || stone)(t));
    else for (let m = 0; m < 4; m++) { const s = (0.3 + hash(i, m) * 0.45) * k; rock(G0, [x + (hash(m, i) - 0.5) * 3 * k, s * 0.3, z + (hash(i + 5, m) - 0.5) * 3 * k], [s * 1.3, s, s * 1.1], r, 0.5, [], (p, nn) => (o.col || stone)(t)(nn, p), { lo: true, floor: -s * 0.4 }); }
  }
}
// a thin straight thread / rod between two points
const rod = (G0, a, b, rad, cc, n = 3) => sweep(G0, [a, b], [rad, rad], n, () => cc, { keep: true });
// a closed loop of points round centre c in the plane of unit vectors u, v
const loop = (c, u, v, rad, n = 40) => { const P = []; for (let k = 0; k <= n; k++) { const a = k / n * Math.PI * 2; P.push(add(c, add(scl(u, Math.cos(a) * rad), scl(v, Math.sin(a) * rad)))); } return P; };
const ring = (G0, c, u, v, rad, tr, cc, n = 40, sides = 4) => { const P = loop(c, u, v, rad, n); sweep(G0, P, P.map(() => tr), sides, typeof cc === 'function' ? cc : () => cc, { keep: true }); };
// a recessed-panel spec with a glyph band (gold, glowing)
const gp = (r, b = 0.5, d = 0.25, o = {}) => ({ b, d, floor: flat(VERD_D, -0.4), glyph: { r, cc: SC(GOLD, 2.4), rules: true, ...o }, ...o });

// returns { geo, crystal?: geo (the relic, animated by the renderer), lights: [[x,y,z, r,g,b]], h: height }
export function siteGeometry(THREE, kind, seed, fac = 0, KIT = null) {
  const g = new G(true), r = rng(seed >>> 0), lights = [];
  let crystal = null, h = 30;
  if (kind === 'gate') h = gate(g, r, lights);
  else if (kind === 'archive') h = archive(g, r, lights);
  else if (kind === 'observatory') h = observatory(g, r, lights);
  else if (kind === 'relic') {
    h = relic(g, r, lights);
    const cg = new G(true); prism(cg, 0, -1.4, 0, [0, 1, 0], 0.7, 1.4, 6, 1.2, () => [1.0, 0.85, 0.5, 0, 3.0]); prism(cg, 0, -1.4, 0, [0, -1, 0], 0.7, 0.01, 6, 1.4, () => [1.0, 0.85, 0.5, 0, 3.0]);
    crystal = cg.build(THREE); lights.push([0, 6.5, 0, 1.0, 0.8, 0.45]);
  } else if (kind === 'outpost') h = fac === 0 ? guildPost(g, r, lights) : fac === 2 ? wreckerPost(g, r, lights) : keeperPost(g, r, lights);
  else if (kind === 'wreck') h = wreckField(g, r, lights);
  else court(g, r, 17, 4.25);
  void KIT;
  return { geo: g.build(THREE), crystal, lights, h };
}

// ---- the Gate of Threads: twin carved pylons, a voussoir arch (fallen on half the gates), the glyph ring and its threads
function gate(g, r, lights) {
  court(g, r, 17, 4.25);
  const T0 = tone(r, IVORY, 0.04), broken = r() < 0.5, side = r.sign(), y0 = 2.05;
  for (let k = 0; k < 3; k++) { const top = 1.05 + k * 0.5; cbox(g, [0, (top - 0.5) / 2, 0], [21.5 - k * 1.5, (top + 0.5) / 2, 8.5 - k * 1.3], 0.12, ID, stone(tone(r, IVORY, 0.03)), { jit: 0.05, seed: 50 + k }); }
  const NC = 5, CH = 6.9;
  let ys = 0;
  for (const sx of [-1, 1]) {
    const px = sx * 14, outer = sx > 0 ? '+x' : '-x';
    cbox(g, [px, y0 + 1.7, 0], [5.4, 1.7, 5.8], 0.35, ID, stone(T0), { panels: { '+z': gp(r, 0.6), '-z': gp(r, 0.6), [outer]: gp(r, 0.6) } });
    let y = y0 + 3.4;
    cbox(g, [px, y + NC * CH / 2, 0], [3.25, NC * CH / 2, 3.55], 0.2, ID, stone(IVORY_D));   // the core, seen in the joints
    for (let k = 0; k < NC; k++) {
      const t = k / (NC - 1), hx = lerp(3.75, 3.25, t), hz = lerp(4.05, 3.55, t), cracked = broken && sx === side && k >= NC - 2;
      cbox(g, [px + (cracked ? sx * 0.35 * (k - NC + 3) : 0), y + CH / 2, 0], [hx, CH / 2 - 0.13, hz], 0.3, cracked ? Q(0.04 * (k - 2), 0.015, -sx * 0.025) : ID, stone(tone(r, T0, 0.04)),
        { jit: 0.12, seed: k * 7 + (sx > 0 ? 3 : 0), panels: k > 0 && k < NC - 1 ? { '+z': gp(r, 0.7, 0.3), '-z': gp(r, 0.7, 0.3) } : null });
      // the gold thread running up the inner face
      const xi = px - sx * (hx + 0.06), yb = y + 0.4, yt = y + CH - 0.4;
      poly(g, [[xi, yb, -0.22], [xi, yb, 0.22], [xi, yt, 0.22], [xi, yt, -0.22]], SC(GOLD, 2.6), [px, y + CH / 2, 0]);
      y += CH;
    }
    cbox(g, [px, y + 0.5, 0], [4.2, 0.5, 4.5], 0.18, ID, flat(tone(r, VERD, 0.06), -0.5));
    cbox(g, [px, y + 1.45, 0], [4.6, 0.45, 4.9], 0.2, ID, stone(T0), { panels: { '+z': gp(r, 0.25, 0.15, { aspect: 1.6 }), '-z': gp(r, 0.25, 0.15, { aspect: 1.6 }) } });
    ys = y + 1.9;
  }
  // the arch: thirteen voussoirs and a keystone; a run of them lies fallen on broken gates
  const NV = 13, Ri = 9.6, Ro = 18.6, dz = 4.2, gap = 0.014, fallen = [];
  const k0 = side > 0 ? 3 : 6;
  for (let k = 0; k < NV; k++) {
    const a0 = Math.PI * k / NV + gap, a1 = Math.PI * (k + 1) / NV - gap, key = k === 6, ro = key ? Ro + 1.4 : Ro, z = key ? dz + 0.5 : dz, T = tone(r, T0, 0.05);
    const P = []; for (let i = 0; i < 8; i++) { const a = i & 1 ? a1 : a0, rr = i & 2 ? ro : Ri, zz = i & 4 ? z : -z; P.push([Math.cos(a) * rr, ys + Math.sin(a) * rr, zz]); }
    if (broken && k >= k0 && k < k0 + 4) { fallen.push([P, T]); continue; }
    hexa(g, P, stone(T));
    const am = (a0 + a1) / 2;
    for (const zs of [-1, 1]) {   // an inlaid gold thread following the arch, on both faces
      const zz = zs * (z + 0.08), p = (a, rr) => [Math.cos(a) * rr, ys + Math.sin(a) * rr, zz];
      poly(g, [p(a0, Ri + 1.0), p(a1, Ri + 1.0), p(a1, Ri + 1.45), p(a0, Ri + 1.45)], SC(GOLD, 2.6), [0, ys, 0]);
      if (key) { const q = p(am, Ri + 5.5), s = 1.2; poly(g, [add(q, [s, 0, 0]), add(q, [0, s * 1.4, 0]), add(q, [-s, 0, 0]), add(q, [0, -s * 1.4, 0])], SC(GOLD, 3.0), [0, ys, 0]); }
    }
    const ri = Ri - 0.08, sp = (a, zz) => [Math.cos(a) * ri, ys + Math.sin(a) * ri, zz];   // the soffit thread
    poly(g, [sp(a0, -0.3), sp(a1, -0.3), sp(a1, 0.3), sp(a0, 0.3)], SC(GOLD, 2.4), [Math.cos(am) * (Ri + 6), ys + Math.sin(am) * (Ri + 6), 0]);
  }
  if (!broken) lights.push([0, ys + Ri + 5.5, dz + 1.2, 1.0, 0.75, 0.35]);
  for (const [P, T] of fallen) {   // the fallen voussoirs: tumbled out of the opening, half buried
    const c = centroid(P), qq = Q(r() * 6.28, r.range(-1.2, 1.2), r.range(-0.6, 0.6)), x = side * r.range(3, 17), z = r.sign() * r.range(7, 15);
    hexa(g, P.map((p) => add(qq(sub(p, c)), [x, r.range(0.8, 2.0), z])), stone(T));
  }
  // the glyph ring: a carved stone ring inlaid with gold, spokes of thread to a floating star
  const RC = [0, 24, 0], RR = 8.4, X = [1, 0, 0], Y = [0, 1, 0];
  ring(g, RC, X, Y, RR, 0.95, at(stone(T0), 20), 48, 6);
  for (const zz of [-0.92, 0.92]) ring(g, add(RC, [0, 0, zz]), X, Y, RR, 0.2, SC(GOLD, 3.0), 48, 4);
  ring(g, RC, X, Y, 2.8, 0.16, SC(GOLD, 3.0), 24, 4);
  for (let k = 0; k < 12; k++) { const a = k / 12 * 6.2832; rod(g, add(RC, [Math.cos(a) * (RR - 0.9), Math.sin(a) * (RR - 0.9), 0]), add(RC, [Math.cos(a) * 2.9, Math.sin(a) * 2.9, 0]), 0.09, SC(GOLD, 2.6)); }
  const star = []; for (let k = 0; k < 16; k++) { const a = k / 16 * 6.2832 + Math.PI / 2, rr = k % 2 ? 0.55 : k % 4 ? 1.3 : 2.1; star.push(add(RC, [Math.cos(a) * rr, Math.sin(a) * rr, 0])); }
  for (const zs of [-1, 1]) { const S = star.map((p) => add(p, [0, 0, zs * 0.12])); for (let k = 0; k < 16; k++) poly(g, [add(RC, [0, 0, zs * 0.3]), S[k], S[(k + 1) % 16]], SC(GOLD, 3.2), add(RC, [0, 0, -zs])); }
  for (const sx of [-1, 1]) cbox(g, [sx * (RR + 1.0), RC[1], 0], [0.75, 0.55, 0.55], 0.12, ID, stone(T0));   // the struts
  lights.push([0, 24, 0, 1.0, 0.75, 0.35]);
  // the threads: taut gold lines from the ring to the pylons and to anchor stones on the court
  for (let k = 0; k < 10; k++) {
    const a = r() * 6.2832, from = add(RC, [Math.cos(a) * (RR + 0.6), Math.sin(a) * (RR + 0.6), r.range(-0.6, 0.6)]);
    let to;
    if (k < 4) to = [(Math.cos(a) > 0 ? 1 : -1) * 10.3, r.range(6, 36), r.range(-3, 3)];
    else { const sx = r.range(-12, 12), sz = (k % 2 ? 1 : -1) * r.range(10, 15.5); to = [sx, 1.9, sz]; cbox(g, [sx, 1.05, sz], [0.6, 0.85, 0.6], 0.15, Q(r()), stone(tone(r)), {}); cbox(g, [sx, 2.0, sz], [0.32, 0.12, 0.32], 0.05, ID, flat(GOLD, 2.2)); }
    rod(g, from, to, 0.07, SC(GOLD, 2.3));
  }
  // steles at the front of the court, one toppled on a broken gate
  for (const sx of [-1, 1]) {
    const down = broken && sx === -side, T = tone(r);
    if (down) cbox(g, [sx * 12.5, 1.0, 15], [0.95, 3.6, 0.45], 0.12, Q(sx * 0.4, 1.5, 0), stone(T), { jit: 0.1, seed: 9, panels: { '+z': gp(r, 0.3, 0.12), '-z': gp(r, 0.3, 0.12) } });
    else { cbox(g, [sx * 11, 0.55 + 3.6, 14], [0.95, 3.6, 0.45], 0.12, Q(sx * 0.2), stone(T), { jit: 0.06, seed: 7, panels: { '+z': gp(r, 0.3, 0.12), '-z': gp(r, 0.3, 0.12) } }); cbox(g, [sx * 11, 8.0, 14], [0.4, 0.25, 0.4], 0.08, Q(sx * 0.2 + 0.785), flat(GOLD, 2.4)); lights.push([sx * 11, 8.3, 14, 1.0, 0.75, 0.35]); }
  }
  // fallen courses at the foot of the cracked pylon, a debris field round the court
  if (broken) for (let k = 0; k < 3; k++) cbox(g, [side * r.range(18, 24), r.range(0.8, 1.6), r.range(-9, 9)], [r.range(2, 3.4), r.range(1.4, 2.6), r.range(2, 3.4)], 0.3, Q(r() * 6.28, r.range(-0.5, 0.5), r.range(-0.4, 0.4)), stone(tone(r, T0, 0.05)), { jit: 0.3, seed: 80 + k });
  debris(g, r, 30, 17.5, 28, { keep: (x, z) => Math.abs(z) > 9 || Math.abs(x) > 22 });
  return Math.round(ys + Ro + 1.4);
}

// ---- the Silent Archive: a monolithic hall on a stepped podium, a colonnade, a dark doorway full of glowing tablets
function archive(g, r, lights) {
  const T0 = tone(r, IVORY, 0.04), Tc = stone(T0), broken = r() < 0.6;
  for (let k = 0; k < 3; k++) cbox(g, [0, (1.2 * (k + 1) - 0.6) / 2, -2], [26 - k, (1.2 * (k + 1) + 0.6) / 2, 21 - k], 0.18, ID, stone(tone(r, IVORY, 0.03)), { jit: 0.06, seed: 11 + k });
  const Y0 = 3.6;
  // the stair up to the colonnade
  for (let k = 0; k < 6; k++) cbox(g, [0, (0.6 * (k + 1) - 0.6) / 2, (34 + (6 - k) * 1.4) / 2], [10, (0.6 * (k + 1) + 0.6) / 2, (4 + (6 - k) * 1.4) / 2], 0.08, ID, stone(tone(r, IVORY, 0.03)), { jit: 0.04, seed: 20 + k });
  // the cella: a back block whose front shows only in the doorway (dark), two front blocks and the lintel
  const backCol = (n, p) => (n[2] > 0.9 ? SC(DARK) : Tc(n, p));
  cbox(g, [0, Y0 + 14.2, -10.4], [20, 14.2, 7.6], 0.3, ID, backCol);
  for (const sx of [-1, 1]) cbox(g, [sx * 12.5, Y0 + 14.2, -0.4], [7.5, 14.2, 2.4], 0.3, ID, Tc);
  const slab = (c, hh, face) => cbox(g, c, hh, 0.12, ID, stone(tone(r, T0, 0.03)), { jit: 0.05, seed: c[0] * 3 + c[2], panels: { [face]: gp(r, 0.55, 0.3) } });
  for (const sx of [-1, 1]) for (const z of [-14.6, -10.4, -6.2]) slab([sx * 20.25, Y0 + 13.6, z], [0.3, 12.6, 1.7], sx > 0 ? '+x' : '-x');
  for (const x of [-12, -4, 4, 12]) slab([x, Y0 + 13.6, -18.25], [2.4, 12.6, 0.3], '-z');
  for (const sx of [-1, 1]) for (const x of [9.5, 15.5]) slab([sx * x, Y0 + 12.4, 2.25], [1.9, 11.0, 0.3], '+z');
  cbox(g, [0, Y0 + 23.8, -0.4], [5, 4.6, 2.4], 0.2, ID, Tc, { panels: { '+z': gp(r, 0.8, 0.3, { aspect: 1.4 }) } });
  for (const sx of [-1, 1]) for (const sz of [-1, 1]) cbox(g, [sx * 20.3, Y0 + 14.2, sz > 0 ? 1.9 : -17.9], [1.3, 14.2, 1.3], 0.22, ID, Tc);   // corner pilasters
  // the door frame and its glowing gold jambs
  for (const sx of [-1, 1]) cbox(g, [sx * 5.7, Y0 + 9.6, 2.3], [0.75, 9.6, 0.5], 0.15, ID, Tc, { panels: { '+z': gp(r, 0.18, 0.1, { aspect: 0.8 }) } });
  cbox(g, [0, Y0 + 19.9, 2.3], [6.45, 0.7, 0.5], 0.15, ID, Tc, { panels: { '+z': gp(r, 0.15, 0.08) } });
  // inside: shelves of crystal tablets glowing in the dark
  for (let row = 0; row < 3; row++) {
    const y = Y0 + 2.6 + row * 5.2;
    cbox(g, [0, y - 0.35, -2.45], [4.5, 0.18, 0.5], 0.05, ID, stone(IVORY_D));
    for (let k = 0; k < 5; k++) if (r() < 0.85) cbox(g, [-3.6 + k * 1.8 + r.range(-0.2, 0.2), y + 1.0, -2.55], [0.62, 1.05, 0.1], 0.04, Q(0, r.range(-0.12, 0.05), r.range(-0.06, 0.06)), flat(TABLET, r.range(1.8, 2.8)));
  }
  lights.push([0, Y0 + 8, -1, 0.4, 1.0, 0.9]);
  // the colonnade: six fluted columns (one snapped on broken archives), architrave, a glyph frieze, the cornice, the stepped roof
  const kb = broken ? 1 + r.int(4) : -1;
  for (let k = 0; k < 6; k++) {
    const x = -17.5 + k * 7;
    if (k === kb) {
      column(g, x, Y0, 9.5, 24.4, 1.45, Tc, { broken: r.range(0.18, 0.4) });
      for (let j = 0; j < 3; j++) drum(g, [x + r.range(-4, 4), r.range(1.1, 1.4) + (j ? 0 : Y0), 15 + j * 4 + r.range(-1, 1)], [r.range(-1, 1), 0, r.range(-0.3, 0.3)], 1.35, r.range(2, 3.2), stone(tone(r, T0, 0.04)));
    } else column(g, x, Y0, 9.5, 24.4, 1.45, Tc);
  }
  const YT = Y0 + 24.4;
  cbox(g, [0, YT + 1.0, 6], [21.5, 1.0, 4.6], 0.2, ID, Tc);
  cbox(g, [0, YT + 3.0, 6.2], [21.2, 1.0, 4.4], 0.15, ID, Tc, { panels: { '+z': gp(r, 0.3, 0.2, { aspect: 1.2 }), '+x': gp(r, 0.3, 0.2), '-x': gp(r, 0.3, 0.2) } });
  cbox(g, [0, YT + 4.6, -3.75], [22.6, 0.6, 15.6], 0.25, ID, Tc);
  cbox(g, [0, YT + 5.8, -3.75], [21, 0.6, 14], 0.25, ID, Tc);
  cbox(g, [0, YT + 6.9, -3.75], [18.5, 0.5, 11.5], 0.2, ID, Tc, { panels: { '+y': gp(r, 1.6, 0.3) } });
  if (broken) cbox(g, [r.sign() * r.range(14, 22), 1.6, r.range(24, 30)], [3.6, 0.6, 2.2], 0.25, Q(r() * 6.28, r.range(-0.4, 0.4), r.range(-0.3, 0.3)), stone(tone(r, T0, 0.04)), { jit: 0.3, seed: 3 });
  // two obelisks flank the stair, their gold pyramidions lit
  for (const sx of [-1, 1]) {
    const x = sx * 14, z = 26, T = stone(tone(r, T0, 0.03));
    cbox(g, [x, 0.9, z], [1.9, 0.9, 1.9], 0.15, ID, T);
    lathe(g, [x, 1.8, z], [0, 1, 0], [[1.3, 0], [1.0, 12.6]], 4, (t, s, n) => T(n, [x, 6, z]), { flat: true, keep: true, phase: Math.PI / 4 });
    lathe(g, [x, 14.4, z], [0, 1, 0], [[1.0, 0], [0, 1.6]], 4, () => SC(GOLD, 2.2), { flat: true, keep: true, phase: Math.PI / 4 });
    for (const [nx, nz] of [[0, 1], [1, 0], [0, -1], [-1, 0]]) {   // a glyph band down each face
      const n = norm([nx, 0.024, nz]), u = [-nz, 0, nx], o0 = [x + nx * 1.24 + u[0] * -0.42, 3.2, z + nz * 1.24 + u[2] * -0.42], top = [x + nx * 1.03 + u[0] * -0.42, 12.6, z + nz * 1.03 + u[2] * -0.42];
      glyphs(g, o0, scl(u, 0.84), sub(top, o0), n, r, SC(GOLD, 2.2), { lift: 0.06 });
    }
    lights.push([x, 15.4, z, 1.0, 0.75, 0.35]);
  }
  debris(g, r, 30, 24, 36, { keep: (x, z) => !(Math.abs(x) < 28 && z > -25 && z < 30) });
  return Math.round(YT + 7.4);
}

// ---- the Last Observatory: stepped terraces, a drum with a glyph ring, a cracked verdigris dome on stone ribs, the lens
function observatory(g, r, lights) {
  const T0 = tone(r, IVORY, 0.04), Tc = stone(T0), [, A, B] = basis([0, 1, 0]);
  const rp = (a, R, y) => [A[0] * Math.cos(a) * R + B[0] * Math.sin(a) * R, y, A[2] * Math.cos(a) * R + B[2] * Math.sin(a) * R];
  for (const [R, y0, y1] of [[17.5, 0, 1.2], [16, 1.2, 2.3], [14.5, 2.3, 3.4]]) lathe(g, [0, 0, 0], [0, 1, 0], [[R, y0 - 0.6], [R, y1 - 0.15], [R - 0.2, y1]], 32, (t, s, n) => Tc(n, [R, y1, 0]), { flat: true, keep: true, cap1: true });
  for (let k = 0; k < 3; k++) cbox(g, [0, 1.15 * (k + 1) / 2, 18.4 - k * 0.5 + 1.4], [3.2, 1.15 * (k + 1) / 2, 1.4 + (2 - k) * 0.5], 0.08, ID, Tc);   // the stair on +z
  // the drum: 24 flat bays, pilasters every second bay, a glyph band all round, a cornice
  const RD = 12.6, n = 24;
  lathe(g, [0, 0, 0], [0, 1, 0], [[RD, 3.3], [RD, 12.6]], n, (t, s, nn) => Tc(nn, [0, 3.3 + t * 9.3, 0]), { flat: true, keep: true });
  for (let k = 0; k < n; k++) {
    const a0 = k / n * 6.2832, a1 = (k + 1) / n * 6.2832, p0 = rp(a0, RD, 10.4), p1 = rp(a1, RD, 10.4), nn = norm(cross(sub(p1, p0), [0, 1, 0]));
    const nOut = dot(nn, p0) < 0 ? scl(nn, -1) : nn;
    glyphs(g, mad(lerp3(p0, p1, 0.04), nOut, 0.01), scl(sub(p1, p0), 0.92), [0, 1.1, 0], nOut, r, SC(GOLD, 2.4), { lift: 0.09, rules: true, aspect: 1.2 });
    if (k % 2 === 0) { const pc = rp(a0, RD + 0.25, 7.9), yaw = Math.atan2(pc[0], pc[2]); cbox(g, pc, [0.65, 4.6, 0.5], 0.12, Q(yaw), Tc); }
  }
  lathe(g, [0, 0, 0], [0, 1, 0], [[RD, 12.5], [RD + 0.9, 12.8], [RD + 0.9, 13.4], [RD - 0.6, 13.6]], n, (t, s, nn) => Tc(nn, [0, 13, 0]), { flat: true, keep: true });
  // the telescope: aimed through the slit (its azimuth picks the open sector)
  const az = r() * 6.2832, ld = norm([Math.cos(az) * 0.62, 1, Math.sin(az) * 0.62]), P0 = [0, 11.2, 0], sideV = norm(cross(ld, [0, 1, 0]));
  const YC = 13.4, RDm = 12.4, NS = 16, slitA = Math.atan2(ld[2], ld[0]);
  const sectorOf = (a) => ((Math.floor(((a % 6.2832) + 6.2832) % 6.2832 / (6.2832 / NS))) + NS) % NS, ks = sectorOf(slitA), fr = (((slitA % 6.2832) + 6.2832) % 6.2832) / (6.2832 / NS) % 1, ks2 = fr < 0.5 ? (ks + NS - 1) % NS : (ks + 1) % NS;
  const kh = (ks + 5 + r.int(6)) % NS;   // the broken side
  const missing = (k, i, j) => k === ks || k === ks2 || (k === kh && j >= 2 && j <= 4) || (k === (kh + 1) % NS && i === 0 && j >= 3 && j <= 4);
  const LAT = [0, 0.24, 0.48, 0.7, 0.9, 1.08, 1.24], sph = (a, f, R) => [Math.cos(a) * Math.cos(f) * R, YC + Math.sin(f) * R, Math.sin(a) * Math.cos(f) * R];
  for (let k = 0; k < NS; k++) for (let i = 0; i < 2; i++) for (let j = 0; j < LAT.length - 1; j++) {
    if (missing(k, i, j)) continue;
    const a0 = (k + i / 2) / NS * 6.2832, a1 = (k + (i + 1) / 2) / NS * 6.2832, f0 = LAT[j], f1 = LAT[j + 1], cc = SC(tone(r, VERD, 0.12), -0.6);
    const O = [sph(a0, f0, RDm), sph(a1, f0, RDm), sph(a1, f1, RDm), sph(a0, f1, RDm)], In = O.map((p) => add(scl(sub(p, [0, YC, 0]), (RDm - 0.45) / RDm), [0, YC, 0]));
    poly(g, O, cc, [0, YC, 0]); poly(g, In, SC(VERD.map((x) => x * 0.6), -0.6), mad(centroid(In), norm(sub(centroid(In), [0, YC, 0])), 6));
    // close the shell's edge where a neighbour is gone
    const nb = [[k, i - 1, j], [k, i + 1, j], [k, i, j - 1], [k, i, j + 1]];
    nb.forEach(([k2, i2, j2], e) => {
      if (j2 < 0 || j2 >= LAT.length - 1) return;
      let kq = k2, iq = i2; if (iq < 0) { kq = (k2 + NS - 1) % NS; iq = 1; } else if (iq > 1) { kq = (k2 + 1) % NS; iq = 0; }
      if (!missing(kq, iq, j2)) return;
      const E = [[0, 3], [1, 2], [0, 1], [3, 2]][e], q = [O[E[0]], O[E[1]], In[E[1]], In[E[0]]];
      poly(g, q, SC(tone(r, IVORY_D, 0.05)), centroid(O.concat(In)));
    });
  }
  for (let k = 0; k < NS; k++) {   // the stone ribs
    const a = k / NS * 6.2832, P = []; for (let j = 0; j <= 8; j++) P.push(sph(a, LAT[6] * j / 8, RDm + 0.3));
    sweep(g, P, P.map(() => 0.42), 4, at(Tc, 20), { keep: true, phase: Math.PI / 4 });
  }
  const yTop = YC + Math.sin(LAT[6]) * (RDm + 0.3), rTop = Math.cos(LAT[6]) * (RDm + 0.3);
  ring(g, [0, yTop, 0], [1, 0, 0], [0, 0, 1], rTop, 0.55, at(Tc, 25), 24, 6);
  ring(g, [0, yTop + 0.35, 0], [1, 0, 0], [0, 0, 1], rTop, 0.14, SC(GOLD, 2.4), 24, 4);
  // pedestal, yoke, the bronze tube with brass bands, the lens still lit
  lathe(g, [0, 3.4, 0], [0, 1, 0], [[1.9, 0], [1.7, 0.6], [1.25, 1.0], [1.15, 5.6], [1.6, 6.1]], 12, (t, s, nn) => Tc(nn, [0, 6, 0]), { keep: true, cap1: true });
  for (const s of [-1, 1]) cbox(g, add(P0, add(scl(sideV, s * 2.7), [0, -0.8, 0])), [0.35, 1.7, 0.7], 0.1, Q(Math.atan2(sideV[0], sideV[2]) + Math.PI / 2), flat(BRASS, -1.5));
  const prof = [[1.5, -3.2], [1.9, -2.9], [1.9, 4], [2.2, 4.15], [2.2, 5.2], [2.0, 5.35], [2.0, 13.5], [2.45, 13.7], [2.5, 17.6], [2.3, 17.8]];
  lathe(g, P0, ld, prof, 16, (t, s, nn, k) => ((k >= 3 && k <= 4) || k >= 7 ? SC(BRASS, -1.5) : SC(tone(r, VERD, 0.04), -0.5)), { keep: true, cap0: true });
  lathe(g, mad(P0, ld, 17.6), ld, [[2.3, 0], [0, 0.16]], 16, () => SC(LENS, 1.6), { keep: true });
  const lp = mad(P0, ld, 17.8); lights.push([lp[0], lp[1], lp[2], 0.5, 0.85, 1.0]);
  // instrument masts on the middle terrace: a pole, crossbars, an armillary sphere round a glowing core
  const nm = 3, broken = r() < 0.5;
  for (let m = 0; m < nm; m++) {
    const a = slitA + Math.PI * (0.55 + m * 0.45) + r.range(-0.1, 0.1), x = Math.cos(a) * 15.3, z = Math.sin(a) * 15.3, H = r.range(12, 16);
    if (broken && m === 1) {   // toppled across the terrace
      const d = norm([Math.cos(a + 1.3), 0.02, Math.sin(a + 1.3)]);
      lathe(g, [x, 2.7, z], d, [[0.3, 0], [0.18, H]], 6, () => SC(BRASS, -1.5), { keep: true, cap1: true });
      const e = mad([x, 2.7, z], d, H); ring(g, e, [0, 1, 0], d, 1.2, 0.08, SC(BRASS, -1.5), 20, 3); continue;
    }
    cbox(g, [x, 2.75, z], [0.7, 0.45, 0.7], 0.1, ID, Tc);
    lathe(g, [x, 3.2, z], [0, 1, 0], [[0.3, 0], [0.18, H]], 6, () => SC(BRASS, -1.5), { keep: true, cap1: true });
    for (const yy of [H * 0.6, H * 0.8]) cbox(g, [x, 3.2 + yy, z], [1.4, 0.08, 0.08], 0.02, Q(a), flat(BRASS, -1.5));
    const c = [x, 3.2 + H + 1.3, z];
    ring(g, c, [1, 0, 0], [0, 0, 1], 1.2, 0.07, SC(BRASS, -1.5), 24, 3);
    ring(g, c, [0, 1, 0], [Math.cos(a), 0, Math.sin(a)], 1.2, 0.07, SC(BRASS, -1.5), 24, 3);
    ring(g, c, norm([Math.sin(a), 1, -Math.cos(a)]), [Math.cos(a), 0, Math.sin(a)], 1.05, 0.06, SC(GOLD, 1.6), 24, 3);
    lathe(g, add(c, [0, -0.4, 0]), [0, 1, 0], [[0, 0], [0.32, 0.4], [0, 0.8]], 6, () => SC(CANDLE, 2.6), { keep: true });
    lights.push([c[0], c[1], c[2], 1.0, 0.75, 0.4]);
  }
  // shards of the dome under its broken side, then the debris field
  const ah = (kh + 1) / NS * 6.2832;
  for (let k = 0; k < 5; k++) { const d = r.range(14.8, 19), a = ah + r.range(-0.3, 0.3); cbox(g, [Math.cos(a) * d, d < 14.5 ? 3.6 : d < 16 ? 2.5 : d < 17.5 ? 1.4 : 0.3, Math.sin(a) * d], [r.range(1, 2), 0.22, r.range(0.8, 1.6)], 0.05, Q(r() * 6.28, r.range(-0.5, 0.5), r.range(-0.4, 0.4)), flat(tone(r, VERD, 0.1), -0.6)); }
  debris(g, r, 24, 19, 28);
  return Math.round(yTop + 1);
}

// ---- the shrine of a relic: a twisted stepped plinth, a gold cradle, four lantern pillars, a ring of votive steles
function relic(g, r, lights) {
  court(g, r, 17, 4.25, { miss: 0.03 });
  const T0 = tone(r, IVORY, 0.03), Tc = stone(T0);
  for (let k = 0; k < 3; k++) {
    const hs = 7 - k * 2, y = 0.55 + k * 1.0 + 0.5, band = k < 2 ? gp(r, 0.18, 0.12, { aspect: 2.2, rules: false }) : null;
    cbox(g, [0, y, 0], [hs, 0.5, hs], 0.14, Q(k * 0.4), stone(k === 2 ? T0 : tone(r, IVORY, 0.04)), { panels: band ? { '+x': band, '-x': band, '+z': band, '-z': band } : null });
  }
  const yT = 3.55;
  lathe(g, [0, yT, 0], [0, 1, 0], [[2.2, -0.02], [2.4, 0.18], [2.0, 0.42], [1.5, 0.42], [1.3, 0.3]], 16, (t, s, n) => Tc(n, [0, 4, 0]), { keep: true, cap1: true });
  for (let k = 0; k < 3; k++) {   // the cradle: three gold prongs curling toward the crystal
    const a = k / 3 * 6.2832 + 0.4, ca = Math.cos(a), sa = Math.sin(a), P = []; for (let j = 0; j <= 5; j++) { const t = j / 5; P.push([ca * (1.7 - 0.5 * t * t), yT + 0.35 + t * 1.6, sa * (1.7 - 0.5 * t * t)]); }
    sweep(g, P, P.map((_, j) => 0.16 - j * 0.018), 5, () => SC(GOLD, 2.0), { keep: true, cap1: 0.6 });
  }
  // four lantern pillars at the corners of the court's heart
  for (let k = 0; k < 4; k++) {
    const a = k / 4 * 6.2832 + 0.785, x = Math.cos(a) * 10.5, z = Math.sin(a) * 10.5, top = column(g, x, 0.55, z, 9.0, 0.5, stone(tone(r, T0, 0.03)));
    for (const [dx, dz] of [[-1, -1], [1, -1], [1, 1], [-1, 1]]) cbox(g, [x + dx * 0.42, top + 0.55, z + dz * 0.42], [0.07, 0.55, 0.07], 0.02, ID, flat(VERD, -0.5));
    lathe(g, [x, top + 1.1, z], [0, 1, 0], [[0.78, 0], [0.7, 0.12], [0, 0.85]], 4, () => SC(VERD, -0.5), { flat: true, keep: true, phase: Math.PI / 4 });
    lathe(g, [x, top + 1.9, z], [0, 1, 0], [[0.1, 0], [0, 0.35]], 4, () => SC(GOLD, 1.8), { keep: true });
    lathe(g, [x, top + 0.2, z], [0, 1, 0], [[0, 0], [0.26, 0.45], [0, 0.9]], 6, () => SC(CANDLE, 2.6), { keep: true });
    lights.push([x, top + 0.65, z, 1.0, 0.72, 0.38]);
  }
  // a henge of six trilithons, carved on their inner faces; one has fallen
  const fall = r.int(6);
  for (let k = 0; k < 6; k++) {
    const a = k / 6 * 6.2832 + 0.5236, q = Q(Math.atan2(-Math.cos(a), -Math.sin(a))), c = [Math.cos(a) * 14, 0, Math.sin(a) * 14], T = tone(r, T0, 0.05), P = (x, y, z) => add(c, q([x, y, z]));
    if (k === fall) {
      cbox(g, P(-1.7, 0.55 + 3.4, 0), [0.75, 3.4, 0.5], 0.12, q, stone(T), { jit: 0.12, seed: k * 3, panels: { '+z': gp(r, 0.22, 0.12) } });
      const q2 = Q(Math.atan2(-Math.cos(a), -Math.sin(a)) + 0.5, -1.45, 0.1); cbox(g, add(P(2.4, 0, 2.6), [0, 1.1, 0]), [0.75, 3.4, 0.5], 0.12, q2, stone(T), { jit: 0.15, seed: k * 3 + 1 });
      cbox(g, add(P(0.5, 0, -2.4), [0, 0.9, 0]), [2.7, 0.45, 0.6], 0.12, Q(Math.atan2(-Math.cos(a), -Math.sin(a)) - 0.3, 0.1, 0.25), stone(T), { jit: 0.15, seed: k * 3 + 2 });
      continue;
    }
    for (const sx of [-1, 1]) cbox(g, P(sx * 1.7, 0.55 + 3.4, 0), [0.75, 3.4, 0.5], 0.12, q, stone(T), { jit: 0.1, seed: k * 3 + (sx > 0 ? 1 : 0), panels: { '+z': gp(r, 0.22, 0.12) } });
    cbox(g, P(0, 0.55 + 6.8 + 0.45, 0), [2.75, 0.45, 0.62], 0.12, q, stone(T), { jit: 0.1, seed: k * 3 + 2, panels: { '+z': gp(r, 0.12, 0.08, { aspect: 1.5 }) } });
  }
  for (let k = 0; k < 12; k++) { const a = r() * 6.2832, d = r.range(7.6, 9); cbox(g, [Math.cos(a) * d, 0.68, Math.sin(a) * d], [0.08, 0.13 + r() * 0.1, 0.08], 0.02, ID, flat(CANDLE, 2.2)); }   // candles
  debris(g, r, 10, 18, 26, { k: 0.7 });
  return 12;
}

// ---- the Guild outpost: an octagonal landing pad, a brass hab dome, a lattice mast with its blue light, tanks and crates
function guildPost(g, r, lights) {
  const BLUE = [0.4, 0.62, 1.4], pc = [5, 0, 5];
  lathe(g, pc, [0, 1, 0], [[12.4, -0.6], [12.4, 0.35], [12.0, 0.6]], 8, (t, s, n) => (n[1] > 0.7 ? SC(tone(r, IVORY, 0.03), -0.3) : SC([0.3, 0.31, 0.34], -1.5)), { flat: true, keep: true, cap1: true, phase: Math.PI / 8 });
  lathe(g, add(pc, [0, 0.66, 0]), [0, 1, 0], [[9.4, 0], [8.4, 0]], 8, () => SC(ROYAL, -0.3), { flat: true, keep: true, phase: Math.PI / 8 });
  const st = []; for (let k = 0; k < 8; k++) { const a = k / 8 * 6.2832, rr = k % 2 ? 1.2 : 3.6; st.push(add(pc, [Math.cos(a) * rr, 0.68, Math.sin(a) * rr])); }
  for (let k = 0; k < 8; k++) poly(g, [add(pc, [0, 0.68, 0]), st[(k + 1) % 8], st[k]], SC(GOLD, -0.3), add(pc, [0, -5, 0]));
  for (let k = 0; k < 8; k++) { const a = (k + 0.5) / 8 * 6.2832, rr = 12.6; cbox(g, add(pc, [Math.cos(a) * rr, 0.45, Math.sin(a) * rr]), [0.35, 0.22, 0.35], 0.06, ID, flat(BLUE, 2.0)); }
  lights.push([pc[0], 1.5, pc[2], 0.4, 0.6, 1.3]);
  // the hab dome: polished brass on an ivory ring, ivory ribs, a ring of lit portholes, the airlock
  const dc = [-9, 0, -7];
  lathe(g, dc, [0, 1, 0], [[6.6, -0.4], [6.6, 1.2], [6.4, 1.3]], 20, () => SC(IVORY, -0.4), { keep: true });
  lathe(g, add(dc, [0, 1.2, 0]), [0, 1, 0], [[6.2, 0], [6.0, 1.5], [5.3, 3.1], [4.1, 4.4], [2.3, 5.3], [0, 5.6]], 20, () => SC(BRASS, -1.5), { keep: true });
  for (let k = 0; k < 8; k++) { const a = k / 8 * 6.2832, P = []; for (let j = 0; j <= 6; j++) { const f = j / 6 * 1.5; P.push(add(dc, [Math.cos(a) * Math.cos(f) * 6.25, 1.2 + Math.sin(f) * 5.65, Math.sin(a) * Math.cos(f) * 6.25])); } sweep(g, P, P.map(() => 0.16), 4, () => SC(IVORY, -0.3), { keep: true }); }
  for (let k = 0; k < 10; k++) { const a = (k + 0.5) / 10 * 6.2832, c = add(dc, [Math.cos(a) * 5.75, 3.0, Math.sin(a) * 5.75]), nn = norm([Math.cos(a), 0.35, Math.sin(a)]), u = norm(cross(nn, [0, 1, 0])), v = cross(u, nn), P = []; for (let j = 0; j < 6; j++) { const b = j / 6 * 6.2832; P.push(mad(mad(mad(c, nn, 0.12), u, Math.cos(b) * 0.55), v, Math.sin(b) * 0.55)); } poly(g, P, SC([1.0, 0.86, 0.55], 1.6), dc); }
  lathe(g, add(dc, [0, 1.9, 4.6]), [0, 0, 1], [[1.6, 0], [1.6, 3.4], [1.75, 3.5], [1.75, 4.0]], 12, (t, s, n, k) => (k >= 2 ? SC(ROYAL, -0.4) : SC(IVORY, -0.4)), { keep: true });
  lathe(g, add(dc, [0, 1.9, 8.6]), [0, 0, 1], [[1.75, 0], [1.2, 0.05], [0, 0.08]], 12, (t, s, n, k) => (k === 0 ? SC(BLUE, 2.0) : SC([0.05, 0.06, 0.1], 0.2)), { keep: true });
  lights.push([dc[0], 2.2, dc[2] + 9, 0.4, 0.6, 1.3]);
  // the lattice mast: three legs, rings, a dish, the blue light on top
  const mc = [9, 0, -9], MH = 21;
  for (let k = 0; k < 3; k++) { const a = k / 3 * 6.2832; rod(g, add(mc, [Math.cos(a) * 1.6, 0, Math.sin(a) * 1.6]), add(mc, [Math.cos(a) * 0.3, MH, Math.sin(a) * 0.3]), 0.13, SC([0.62, 0.63, 0.66], -1.5), 4); }
  for (let j = 1; j <= 6; j++) { const y = j / 7 * MH, rr = lerp(1.6, 0.3, y / MH); ring(g, add(mc, [0, y, 0]), [1, 0, 0], [0, 0, 1], rr, 0.06, SC([0.62, 0.63, 0.66], -1.5), 9, 3); }
  const dd = norm([0.5, 0.45, 0.6]); lathe(g, add(mc, [0.8, 14, 0.8]), dd, [[0.2, -0.2], [1.2, 0.25], [2.2, 0.85]], 12, () => SC(IVORY, -0.3), { keep: true });
  lathe(g, add(mc, [0, MH, 0]), [0, 1, 0], [[0, 0], [0.42, 0.4], [0, 0.85]], 8, () => SC(BLUE, 2.4), { keep: true });
  lights.push([mc[0], MH + 0.4, mc[2], 0.4, 0.7, 1.4]);
  // fuel tanks on cradles, stacked crates
  for (let k = 0; k < 2; k++) {
    const c = [-12 + k * 4.2, 2.1, 9];
    for (const dz of [-2.2, 2.2]) cbox(g, add(c, [0, -1.25, dz]), [1.5, 0.85, 0.35], 0.08, ID, flat([0.3, 0.31, 0.34], -1.5));
    lathe(g, add(c, [0, 0, -4]), [0, 0, 1], [[0, 0], [1.2, 0.5], [1.6, 1.4], [1.6, 6.6], [1.2, 7.5], [0, 8]], 12, (t, s, n, k2) => (k2 === 3 ? SC(ROYAL, -0.4) : SC(IVORY, -0.4)), { keep: true });
  }
  for (let k = 0; k < 7; k++) { const x = 13 + (k % 3) * 2.3 - (k > 4 ? 1.1 : 0), y = k < 3 ? 1.1 : k < 5 ? 3.3 : 5.5, z = 12 + (k % 2) * 0.3; cbox(g, [x, y, z], [1.1, 1.1, 1.1], 0.1, Q(r.range(-0.1, 0.1)), flat(k % 2 ? ROYAL : IVORY, -0.5)); }
  for (let k = 0; k < 10; k++) { const a = k / 10 * 6.2832 + 0.3, rr = 19; cbox(g, [Math.cos(a) * rr, 0.6, Math.sin(a) * rr], [0.25, 0.6, 0.25], 0.05, ID, flat(IVORY, -0.4)); cbox(g, [Math.cos(a) * rr, 1.32, Math.sin(a) * rr], [0.18, 0.12, 0.18], 0.03, ID, flat(BLUE, 2.0)); }
  return MH + 1;
}

// ---- the Wreckers' yard: scrap shacks, stacked containers, a lattice crane, sodium lamps, scrap heaps and a plate fence
function wreckerPost(g, r, lights) {
  const MET = [0.3, 0.31, 0.32], rust = () => tone(r, RUST, 0.3);
  for (let k = 0; k < 9; k++) cbox(g, [r.range(-14, 14), 0.12, r.range(-14, 14)], [r.range(2, 4), 0.12, r.range(1.5, 3)], 0.04, Q(r() * 6.28, r.range(-0.03, 0.03), r.range(-0.03, 0.03)), flat(k % 3 ? OIL : rust(), -0.8));
  // shacks: rusty walls, slanted roofs, a dark door, a lit window, a stovepipe
  const ns = 3 + r.int(2);
  for (let k = 0; k < ns; k++) {
    const a = k / ns * 6.2832 + r.range(-0.3, 0.3), d = r.range(8, 12), x = Math.cos(a) * d, z = Math.sin(a) * d, yaw = Math.atan2(-x, -z) + r.range(-0.4, 0.4), q = Q(yaw), w = r.range(2.2, 3.4), hh = r.range(1.5, 2.1), dp = r.range(2, 3);
    const P = (lx, ly, lz) => add([x, 0, z], q([lx, ly, lz]));
    cbox(g, P(0, hh, 0), [w, hh, dp], 0.06, q, flat(rust(), -0.7));
    cbox(g, P(0, hh * 2 + 0.25, 0.2), [w + 0.5, 0.1, dp + 0.6], 0.03, Q(yaw, 0.18, 0), flat(k % 2 ? OIL : [0.16, 0.15, 0.14], k % 2 ? -0.9 : -1.5));
    for (let m = 0; m < 3; m++) { const pw = r.range(0.5, 1.1), ph = r.range(0.4, 0.9), side = m === 2 ? 1 : -1; cbox(g, m < 2 ? P(r.range(-w + pw, w - pw), r.range(ph, hh * 2 - ph), side * (dp + 0.04)) : P(w + 0.04, r.range(ph, hh * 2 - ph), r.range(-dp + pw, dp - pw)), m < 2 ? [pw, ph, 0.03] : [0.03, ph, pw], 0.01, Q(yaw, 0, r.range(-0.1, 0.1)), flat(m === 1 ? MET : r() < 0.5 ? OIL : rust(), m === 1 ? -1.5 : -0.9)); }
    cbox(g, P(w * 0.3, 1.1, dp + 0.02), [0.55, 1.05, 0.05], 0.02, q, flat(DARK, 0));
    cbox(g, P(-w * 0.45, hh * 1.2, dp + 0.03), [0.45, 0.35, 0.04], 0.02, q, flat(SODIUM, 2.0));
    lathe(g, P(-w * 0.6, hh * 2, -dp * 0.4), [0, 1, 0], [[0.18, 0], [0.18, 1.8], [0.26, 1.9], [0.26, 2.2]], 6, () => SC(OIL, -1.5), { keep: true });
  }
  // stacked containers (one striped)
  const cc = [-6, 0, 12], cy = r() * 0.6;
  for (let k = 0; k < 3; k++) { const y = k < 2 ? 1.3 : 3.9, x = k < 2 ? k * 2.6 : 1.3; cbox(g, add(cc, [x, y, 0]), [1.2, 1.3, 3.05], 0.06, Q(cy + (k > 1 ? 0.15 : 0)), flat([[0.5, 0.12, 0.06], OIL, [0.12, 0.2, 0.26]][k], -0.8)); }
  cbox(g, add(cc, [0, 1.3, 0]), [1.23, 0.18, 3.08], 0.02, Q(cy), flat(SODIUM, -0.6));
  // the crane: a lattice mast, a raised boom, a cable and a dangling plate, a sodium lamp at the tip
  const kc = [10, 0, -8], MH = 17, legs = [[-0.8, -0.8], [0.8, -0.8], [0.8, 0.8], [-0.8, 0.8]];
  for (const [lx, lz] of legs) rod(g, add(kc, [lx, 0, lz]), add(kc, [lx * 0.7, MH, lz * 0.7]), 0.11, SC([0.62, 0.42, 0.1], -0.8), 4);
  for (let j = 0; j < 5; j++) { const y0 = j * MH / 5, y1 = (j + 1) * MH / 5, s0 = lerp(0.8, 0.56, y0 / MH), s1 = lerp(0.8, 0.56, y1 / MH); for (let f = 0; f < 4; f++) { const A0 = legs[f], A1 = legs[(f + 1) % 4]; rod(g, add(kc, [A0[0] * s0 / 0.8, y0, A0[1] * s0 / 0.8]), add(kc, [A1[0] * s1 / 0.8, y1, A1[1] * s1 / 0.8]), 0.06, SC([0.62, 0.42, 0.1], -0.8), 3); } }
  const ba = r() * 6.2832, bd = norm([Math.cos(ba), 0.42, Math.sin(ba)]), b0 = add(kc, [0, MH + 0.4, 0]), b1 = mad(b0, bd, 15);
  cbox(g, add(kc, [0, MH + 0.4, 0]), [1.0, 0.5, 1.0], 0.08, ID, flat(OIL, -0.8));
  sweep(g, [mad(b0, bd, -4), b1], [0.45, 0.3], 4, () => SC([0.62, 0.42, 0.1], -0.8), { keep: true, phase: Math.PI / 4 });
  cbox(g, mad(b0, bd, -4.4), [0.9, 0.9, 0.9], 0.1, Q(ba), flat([0.25, 0.25, 0.26], -1.5));
  const hk = [b1[0], 3.5, b1[2]]; rod(g, b1, hk, 0.04, SC(OIL, -1.5));
  cbox(g, add(hk, [0, -0.9, 0]), [1.4, 0.9, 0.06], 0.02, Q(ba + 0.6, 0.1, 0.15), flat(rust(), -0.9));
  lathe(g, add(b1, [0, -0.5, 0]), [0, 1, 0], [[0.1, 0], [0.45, 0.3], [0, 0.5]], 6, () => SC(SODIUM, 2.6), { keep: true });
  lights.push([b1[0], b1[1] - 0.4, b1[2], 1.4, 0.62, 0.2]);
  // sodium lamp posts
  for (let k = 0; k < 2; k++) {
    const a = r() * 6.2832, x = Math.cos(a) * 15, z = Math.sin(a) * 15;
    lathe(g, [x, 0, z], [0, 1, 0], [[0.2, 0], [0.12, 6.5]], 6, () => SC(OIL, -1.5), { keep: true, cap1: true });
    cbox(g, [x, 6.6, z], [0.7, 0.1, 0.1], 0.02, Q(a), flat(OIL, -1.5));
    cbox(g, [x + Math.sin(a) * 0.6, 6.45, z + Math.cos(a) * 0.6], [0.3, 0.12, 0.2], 0.03, Q(a), flat(SODIUM, 2.6));
    lights.push([x, 6.3, z, 1.4, 0.62, 0.2]);
  }
  // scrap heaps and oil drums
  for (let h2 = 0; h2 < 2; h2++) {
    const a = r() * 6.2832, d = r.range(4, 9), hx = Math.cos(a) * d, hz = Math.sin(a) * d;
    for (let k = 0; k < 10; k++) { const s = r.range(0.3, 1.0); cbox(g, [hx + r.range(-2.5, 2.5), s * r.range(0.4, 1.6), hz + r.range(-2.5, 2.5)], [s * r.range(0.6, 1.6), s * 0.4, s], 0.04, Q(r() * 6.28, r.range(-0.8, 0.8), r.range(-0.8, 0.8)), flat(r() < 0.5 ? MET : rust(), r() < 0.5 ? -1.5 : -0.9)); }
    for (let k = 0; k < 4; k++) { const dx = hx + r.range(-3.5, 3.5), dz = hz + r.range(-3.5, 3.5), tip = r() < 0.3; lathe(g, [dx, tip ? 0.35 : 0, dz], tip ? [r.range(-1, 1), 0.05, r.range(-1, 1)] : [0, 1, 0], [[0.36, 0], [0.36, 0.42], [0.38, 0.45], [0.36, 0.48], [0.36, 0.95]], 8, (t, s, n, k2) => (k2 === 2 ? SC(SODIUM, -0.6) : SC(OIL, -0.8)), { keep: true, cap1: true }); }
  }
  // a salvaged engine nacelle lying in the yard, half stripped; a pole flying a tattered sodium-orange banner
  const na = r() * 6.2832, nd = norm([Math.cos(na), 0.06, Math.sin(na)]), nb = [Math.cos(na + 1.9) * 9, 2.3, Math.sin(na + 1.9) * 9];
  lathe(g, nb, nd, [[0, -4.6], [1.6, -4.4], [2.3, -3.2], [2.4, 1.8], [2.0, 2.4], [2.6, 4.2], [2.2, 4.4], [1.2, 2.6]], 14, (t, s2, n, k) => (k >= 5 ? SC([0.07, 0.065, 0.06], -1.5) : SC(k % 2 ? MET : rust(), k % 2 ? -1.5 : -0.9)), { keep: true });
  for (let k = 0; k < 4; k++) ring(g, mad(nb, nd, -2.4 + k * 1.3), [0, 1, 0], norm(cross(nd, [0, 1, 0])), 2.55, 0.12, SC(OIL, -1.5), 14, 3);
  for (const dz of [-2.6, 2.6]) { const p = mad(nb, nd, dz); cbox(g, [p[0], 0.5, p[2]], [0.5, 0.5, 1.8], 0.05, Q(Math.atan2(nd[0], nd[2]) + Math.PI / 2), flat(OIL, -0.9)); }
  const fp = [Math.cos(na - 1.2) * 13, 0, Math.sin(na - 1.2) * 13];
  lathe(g, fp, [0, 1, 0], [[0.14, 0], [0.09, 11]], 5, () => SC(OIL, -1.5), { keep: true, cap1: true });
  { const P = [], S = [], W = [], fa = r() * 6.2832; for (let k = 0; k <= 6; k++) { const t = k / 6; P.push([fp[0] + Math.cos(fa) * 2.6 * t, 9.6 - t * 0.6 + Math.sin(t * 7) * 0.12, fp[2] + Math.sin(fa) * 2.6 * t]); S.push([0, 1, 0]); W.push(0.75 * (1 - t * 0.35)); }
    ribbon(g, P, S, W, (t, k) => SC(k % 2 ? SODIUM : [0.8, 0.36, 0.08], -0.8), { ruffle: 0.15 }); }
  // a fence of scrap plates on part of the perimeter
  const fa = r() * 6.2832;
  for (let k = 0; k < 12; k++) { const a = fa + k * 0.2, x = Math.cos(a) * 18, z = Math.sin(a) * 18; cbox(g, [x, 1.0, z], [1.5, 1.05, 0.06], 0.02, Q(Math.atan2(-x, -z) + Math.PI / 2 + Math.PI / 2, r.range(-0.15, 0.15), r.range(-0.08, 0.08)), flat(k % 3 ? rust() : MET, k % 3 ? -0.9 : -1.5)); }
  return MH + 3;
}

// ---- the Keepers' house: a white lighthouse tower with a verdigris lantern, a chapel, a walled court lit by candles
function keeperPost(g, r, lights) {
  court(g, r, 12, 4, { c: WHITE, base: [0.55, 0.55, 0.52], miss: 0.02 });
  const W = stone(WHITE), tc = [-4, 0, -4];
  lathe(g, tc, [0, 1, 0], [[3.9, 0.3], [3.8, 1.9], [3.35, 2.1], [3.15, 15.2], [3.7, 15.5], [3.7, 16.0]], 14, (t, s, n) => W(n, [0, 3 + t * 13, 0]), { keep: true });
  for (const y of [6.2, 11.0]) lathe(g, tc, [0, 1, 0], [[3.36 - (y - 2.1) * 0.0153, y], [3.4 - (y - 2.1) * 0.0153, y + 0.1], [3.4 - (y - 2.1) * 0.0153, y + 0.5], [3.3 - (y - 2.1) * 0.0153, y + 0.6]], 14, () => SC(VERD, -0.5), { keep: true });
  for (let k = 0; k < 4; k++) { const a = k * 1.9, y = 3.6 + k * 2.9, rr = 3.32 - (y - 2.1) * 0.0153, c = add(tc, [Math.cos(a) * rr, y, Math.sin(a) * rr]); cbox(g, c, [0.28, 0.6, 0.12], 0.03, Q(Math.atan2(Math.cos(a), Math.sin(a))), flat(DARK)); }
  lathe(g, tc, [0, 1, 0], [[3.6, 15.7], [4.4, 16.0], [4.4, 16.4], [2.3, 16.4]], 14, (t, s, n) => W(n, [0, 16, 0]), { keep: true });
  for (let k = 0; k < 14; k++) { const a = k / 14 * 6.2832; cbox(g, add(tc, [Math.cos(a) * 4.2, 16.95, Math.sin(a) * 4.2]), [0.06, 0.55, 0.06], 0.01, ID, flat(VERD, -0.5)); }
  ring(g, add(tc, [0, 17.5, 0]), [1, 0, 0], [0, 0, 1], 4.2, 0.07, SC(VERD, -0.5), 28, 3);
  lathe(g, tc, [0, 1, 0], [[2.2, 16.4], [2.2, 19.0]], 12, () => SC(CANDLE, 2.0), { keep: true });
  for (let k = 0; k < 6; k++) { const a = k / 6 * 6.2832; cbox(g, add(tc, [Math.cos(a) * 2.25, 17.7, Math.sin(a) * 2.25]), [0.09, 1.3, 0.09], 0.02, ID, flat(VERD, -0.5)); }
  lathe(g, tc, [0, 1, 0], [[2.7, 19.0], [2.6, 19.4], [1.8, 20.4], [0.6, 21.3], [0.12, 22.4]], 12, () => SC(VERD, -0.5), { keep: true, cap1: true });
  lathe(g, add(tc, [0, 22.3, 0]), [0, 1, 0], [[0, 0], [0.3, 0.3], [0, 0.6]], 6, () => SC(GOLD, 1.4), { keep: true });
  lights.push([tc[0], 17.7, tc[2], 1.0, 0.75, 0.35]);
  // the chapel: white walls, a verdigris roof, a dark door under a gold glyph, candle-lit windows
  const cc = [5.5, 0.55, 3.5];
  cbox(g, add(cc, [0, 2.6, 0]), [3.2, 2.6, 4.4], 0.12, ID, W, { panels: { '+x': gp(r, 0.6, 0.15, { glyph: null }), '-x': gp(r, 0.6, 0.15, { glyph: null }) } });
  const R0 = add(cc, [0, 5.2, 0]), RP = [[-3.6, 0, -4.8], [3.6, 0, -4.8], [0, 2.4, -4.8], [0, 2.4, -4.8], [-3.6, 0, 4.8], [3.6, 0, 4.8], [0, 2.4, 4.8], [0, 2.4, 4.8]].map((p) => add(R0, p));
  hexa(g, [RP[0], RP[1], RP[2], RP[3], RP[4], RP[5], RP[6], RP[7]], flat(VERD, -0.5));
  cbox(g, add(cc, [0, 1.5, 4.42]), [0.85, 1.5, 0.05], 0.02, ID, flat(DARK));
  glyphs(g, add(cc, [-0.8, 3.3, 4.42]), [1.6, 0, 0], [0, 0.9, 0], [0, 0, 1], r, SC(GOLD, 2.4), { lift: 0.06 });
  for (const sz of [-2, 1]) for (const sx of [-1, 1]) cbox(g, add(cc, [sx * 3.22, 2.9, sz]), [0.04, 0.7, 0.3], 0.01, ID, flat(CANDLE, 1.8));
  lights.push([cc[0], 2.0, cc[2] + 5, 1.0, 0.72, 0.38]);
  // the court's low walls with verdigris coping and votive candles
  for (let s = 0; s < 4; s++) for (let k = -2; k <= 2; k++) {
    if (s === 0 && k === 0) continue;   // the gate
    const yaw = s * Math.PI / 2, q = Q(yaw), c = add([0, 0, 0], q([k * 4.6, 1.25, 11.6])), qq = Q(yaw);
    cbox(g, c, [2.2, 0.7, 0.35], 0.08, qq, W, { jit: 0.04, seed: s * 9 + k });
    cbox(g, add(c, [0, 0.78, 0]), [2.3, 0.08, 0.45], 0.03, qq, flat(VERD, -0.5));
    if ((k + s) % 2 === 0) cbox(g, add(c, [0, 1.0, 0]), [0.07, 0.14, 0.07], 0.02, ID, flat(CANDLE, 2.2));
  }
  debris(g, r, 8, 15, 22, { k: 0.6, c: WHITE });
  return 23;
}

// ---- a crash site: the scorched crater berm, a ploughed trench, torn hull plates, engine parts, beams, small fires
function wreckField(g, r, lights) {
  const MET = [0.34, 0.36, 0.4], scorch = (n, p) => { const k = 0.8 + hash(p[0] * 0.3, p[2] * 0.3) * 0.4; return [EARTH[0] * k, EARTH[1] * k, EARTH[2] * k, 0, 0]; };
  lathe(g, [0, 0, 0], [0, 1, 0], [[27, -0.8], [22, 0.5], [17.5, 2.3], [15, 2.0], [12.5, 0.6], [10, -0.6]], 22, (t, s, n) => scorch(n, [s, 0, t]), { flat: true, keep: true,
    rmod: (k, s) => 1 + (hash(k * 1.3, s) - 0.5) * 0.18, hmod: (k, s) => (k > 0 && k < 5 ? (hash(s * 2.1, k) - 0.4) * 1.4 : 0) });
  // the trench the hull ploughed coming in from +z: upturned earth and rock along both lips
  for (let k = 0; k < 14; k++) {
    const z = 18 + k * 3.2 + r.range(-1, 1), w = lerp(7.5, 3.5, k / 13), s = lerp(2.2, 0.8, k / 13);
    for (const sx of [-1, 1]) rock(g, [sx * (w + r.range(-0.8, 0.8)), s * 0.3, z], [s * 1.6, s, s * 1.3], r, 0.5, [[[sx * -0.6, 1, 0], s * 0.3]], (p, n) => scorch(n, p), { lo: true, floor: -s * 0.5, rot: rotY(r.range(-0.4, 0.4)) });
  }
  // torn hull plates: bent in two, half buried, around the crater and down the trench
  for (let k = 0; k < 18; k++) {
    const inTrench = k > 11, a = r() * 6.2832, d = r.range(11, 30), x = inTrench ? r.range(-9, 9) : Math.cos(a) * d, z = inTrench ? r.range(20, 55) : Math.sin(a) * d;
    const L = r.range(2, 5.5), W2 = r.range(1.2, 3), bend = r.range(0.3, 1.1), yaw = r() * 6.28, tx = r.range(-0.5, 0.5), c = [x, r.range(0.3, 1.2), z], bare = r() < 0.55, col = bare ? flat(MET, -1.5) : flat(r() < 0.5 ? [0.32, 0.12, 0.08] : [0.12, 0.16, 0.24], -0.7);
    cbox(g, c, [L / 2, 0.07, W2 / 2], 0.02, Q(yaw, tx, 0), col);
    const q2 = Q(yaw, tx, bend), e = add(c, Q(yaw, tx, 0)([L / 2, 0, 0])); cbox(g, add(e, q2([L * 0.3, 0, 0])), [L * 0.3, 0.07, W2 / 2 * 0.9], 0.02, q2, col);
  }
  // engine nozzles, pipes, beams and crates
  for (let k = 0; k < 2; k++) { const a = r() * 6.2832, d = r.range(12, 20), dir = norm([Math.cos(a + 1.6), r.range(0.1, 0.5), Math.sin(a + 1.6)]); lathe(g, [Math.cos(a) * d, 1.0, Math.sin(a) * d], dir, [[1.1, 0], [0.9, 0.9], [1.6, 2.6], [2.1, 3.4], [1.9, 3.5], [0.8, 0.95]], 14, (t, s, n, k2) => (k2 === 5 ? SC(SODIUM, 1.4) : SC([0.07, 0.065, 0.06], -1.5)), { keep: true }); }
  for (let k = 0; k < 5; k++) { const a = r() * 6.2832, d = r.range(10, 26), p0 = [Math.cos(a) * d, 0.3, Math.sin(a) * d], b2 = r() * 6.28, L = r.range(3, 7); sweep(g, [p0, add(p0, [Math.cos(b2) * L * 0.5, r.range(0.5, 1.6), Math.sin(b2) * L * 0.5]), add(p0, [Math.cos(b2 + 0.5) * L, 0.2, Math.sin(b2 + 0.5) * L])], [0.28, 0.28, 0.28], 6, () => SC(MET, -1.5), { keep: true, cap0: true, cap1: true }); }
  for (let k = 0; k < 7; k++) { const a = r() * 6.2832, d = r.range(9, 34), L = r.range(3, 8); cbox(g, [Math.cos(a) * d, 0.4, Math.sin(a) * d], [L / 2, 0.3, 0.3], 0.04, Q(r() * 6.28, r.range(-0.3, 0.3), r.range(-0.2, 0.2)), flat(k % 2 ? MET : OIL, -1.5)); }
  for (let k = 0; k < 4; k++) { const a = r() * 6.2832, d = r.range(14, 28); cbox(g, [Math.cos(a) * d, 0.7, Math.sin(a) * d], [0.9, 0.7, 0.7], 0.08, Q(r() * 6.28, r.range(-0.4, 0.4), r.range(-0.3, 0.3)), flat(k % 2 ? [0.3, 0.3, 0.2] : [0.25, 0.12, 0.06], -0.8)); }
  // small fires: glowing embers among the scorched rocks
  for (let k = 0; k < 4; k++) {
    const a = r() * 6.2832, d = r.range(9, 22), x = Math.cos(a) * d, z = Math.sin(a) * d;
    for (let m = 0; m < 5; m++) { const s = r.range(0.25, 0.6); rock(g, [x + r.range(-1.2, 1.2), s * 0.3, z + r.range(-1.2, 1.2)], [s * 1.2, s, s], r, 0.4, [], (p, n) => (n[1] > 0.4 && hash(p[0], p[2]) < 0.6 ? SC([1.0, 0.4, 0.1], 2.2) : SC(EARTH, 0)), { lo: true, floor: -s * 0.4 }); }
    lights.push([x, 1.0, z, 1.4, 0.55, 0.18]);
  }
  return 20;
}

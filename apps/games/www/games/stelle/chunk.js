// stelle/chunk.js — one terrain chunk as typed arrays (pure; the terrain worker runs it, the tests too).
//
// A chunk is one quadtree node of the cube-sphere (face f, level L, x, y): 33 x 33 vertices plus a skirt
// along each edge (the skirt hangs below the edge and hides any crack against a coarser neighbour). Per vertex:
//   position  (vec3)  relative to the node centre on the sea-level sphere; liquids are flat at sea level
//   normal    (vec3)  from the true ground (one ring of border samples, so seams shade the same on both sides)
//   aMor      (vec4)  xyz = coarse - fine (the position this vertex takes in the parent's mesh: CDLOD geomorph),
//                     w = level + rockiness (fract)
//   aSrf      (vec4)  elevation (m, true, negative under liquids), macro height h01, moisture, type mask
// The main thread lends its buffers with every request (transferred, not copied) and gets them back filled:
// in the steady state nothing is allocated on either side.
import { elevation, cubeDir, nodeSize, GRID } from './planet.js';

const N = GRID, V = N + 1, B = V + 2;            // vertices per side, with one border ring
const NV = V * V + 4 * V;                        // grid + skirts
const gx = new Float64Array(B * B), gy = new Float64Array(B * B), gz = new Float64Array(B * B);   // true ground positions (planet frame)
const ge = new Float64Array(B * B), gh = new Float32Array(B * B), gm = new Float32Array(B * B), g1 = new Float32Array(B * B), g2 = new Float32Array(B * B);
const fx = new Float64Array(V * V), fy = new Float64Array(V * V), fz = new Float64Array(V * V);   // displayed positions
const D = [0, 0, 0], C = [0, 0, 0], EX = { h: 0, mo: 0, m1: 0, m2: 0 };

export const VERTS = NV, SIDE = V;
export const newChunkBufs = () => ({ pos: new Float32Array(NV * 3), nrm: new Float32Array(NV * 3), mor: new Float32Array(NV * 4), srf: new Float32Array(NV * 4) });
// index buffer: grid triangles (diagonal i,j -> i+1,j+1) + the skirt strips, one per chunk shape
export function chunkIndex() {
  const idx = [];
  for (let j = 0; j < N; j++) for (let i = 0; i < N; i++) { const a = j * V + i, b = a + 1, c = a + V, d = c + 1; idx.push(a, b, d, a, d, c); }   // counter-clockwise seen from above (U x V = outward on every face)
  const S0 = V * V;
  const strip = (edgeIdx, base, flip) => { for (let i = 0; i < N; i++) { const a = edgeIdx(i), b = edgeIdx(i + 1), sa = base + i, sb = base + i + 1; if (flip) idx.push(a, b, sa, b, sb, sa); else idx.push(a, sa, b, b, sa, sb); } };
  strip((i) => i, S0, false); strip((i) => (V - 1) * V + i, S0 + V, true); strip((j) => j * V, S0 + 2 * V, true); strip((j) => j * V + V - 1, S0 + 3 * V, false);
  return new Uint16Array(idx);
}
export function buildChunk(S, f, L, ix, iy, out) {
  const n = 1 << L, size = nodeSize(S, L), R = S.R, liquid = S.sea > 0;
  const u0 = -1 + 2 * ix / n, du = 2 / n / N;
  cubeDir(f, -1 + 2 * (ix + 0.5) / n, -1 + 2 * (iy + 0.5) / n, C);
  const cx = C[0] * R, cy = C[1] * R, cz = C[2] * R;
  const v0 = -1 + 2 * iy / n;
  let emin = 1e9, emax = -1e9;
  // sample the ground on the grid plus a border ring (indices -1 .. V)
  for (let j = 0; j < B; j++) for (let i = 0; i < B; i++) {
    const k = j * B + i;
    // exact lattice coordinates (binary fractions) so neighbours share bit-identical edge vertices
    const u = u0 + (i - 1) * du, v = v0 + (j - 1) * du;
    cubeDir(f, u, v, D);
    const e = elevation(S, D[0], D[1], D[2], EX), r = R + e;
    gx[k] = D[0] * r; gy[k] = D[1] * r; gz[k] = D[2] * r; ge[k] = e; gh[k] = EX.h; gm[k] = EX.mo; g1[k] = EX.m1; g2[k] = EX.m2;
  }
  const P = out.pos, Nn = out.nrm, Mo = out.mor, Sr = out.srf;
  for (let j = 0; j < V; j++) for (let i = 0; i < V; i++) {
    const k = (j + 1) * B + (i + 1), o = j * V + i;
    let x = gx[k], y = gy[k], z = gz[k];
    const e = ge[k];
    if (liquid && e < 0) { const l = Math.hypot(x, y, z); x = x / l * R; y = y / l * R; z = z / l * R; }
    fx[o] = x; fy[o] = y; fz[o] = z;
    if (e < emin) emin = e; if (e > emax) emax = e;
    // normal: central differences of the true ground (border ring included)
    const kl = k - 1, kr = k + 1, kd = k - B, ku = k + B;
    let ax = gx[kr] - gx[kl], ay = gy[kr] - gy[kl], az = gz[kr] - gz[kl];
    let bx = gx[ku] - gx[kd], by = gy[ku] - gy[kd], bz = gz[ku] - gz[kd];
    let nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    // orient outward
    if (nx * x + ny * y + nz * z < 0) { nx = -nx; ny = -ny; nz = -nz; }
    let nl = Math.hypot(nx, ny, nz) || 1;
    if (liquid && e < 0) { nx = x; ny = y; nz = z; nl = Math.hypot(x, y, z); }
    Nn[o * 3] = nx / nl; Nn[o * 3 + 1] = ny / nl; Nn[o * 3 + 2] = nz / nl;
    Sr[o * 4] = e; Sr[o * 4 + 1] = gh[k]; Sr[o * 4 + 2] = gm[k]; Sr[o * 4 + 3] = g1[k];
    Mo[o * 4 + 3] = L + Math.min(0.999, Math.max(0, g2[k]));
  }
  // positions relative to the centre + the geomorph target (the parent mesh: odd vertices sit on the line / diagonal
  // between their even neighbours, matching the triangulation's diagonal (i, j) -> (i + 1, j + 1))
  for (let j = 0; j < V; j++) for (let i = 0; i < V; i++) {
    const o = j * V + i;
    P[o * 3] = fx[o] - cx; P[o * 3 + 1] = fy[o] - cy; P[o * 3 + 2] = fz[o] - cz;
    let tx = fx[o], ty = fy[o], tz = fz[o];
    const oi = i & 1, oj = j & 1;
    if (oi && !oj) { const a = o - 1, b = o + 1; tx = (fx[a] + fx[b]) / 2; ty = (fy[a] + fy[b]) / 2; tz = (fz[a] + fz[b]) / 2; }
    else if (!oi && oj) { const a = o - V, b = o + V; tx = (fx[a] + fx[b]) / 2; ty = (fy[a] + fy[b]) / 2; tz = (fz[a] + fz[b]) / 2; }
    else if (oi && oj) { const a = o - V - 1, b = o + V + 1; tx = (fx[a] + fx[b]) / 2; ty = (fy[a] + fy[b]) / 2; tz = (fz[a] + fz[b]) / 2; }
    Mo[o * 4] = tx - fx[o]; Mo[o * 4 + 1] = ty - fy[o]; Mo[o * 4 + 2] = tz - fz[o];
  }
  // skirts: the four edges again, hung down toward the centre
  const drop = size / N * 2.5 + 3 + S.A * 0.01;
  let s = V * V;
  const edge = (o) => {
    const l = Math.hypot(fx[o], fy[o], fz[o]), k = (l - drop) / l;
    P[s * 3] = fx[o] * k - cx; P[s * 3 + 1] = fy[o] * k - cy; P[s * 3 + 2] = fz[o] * k - cz;
    for (let c = 0; c < 3; c++) Nn[s * 3 + c] = Nn[o * 3 + c];
    for (let c = 0; c < 4; c++) { Mo[s * 4 + c] = Mo[o * 4 + c]; Sr[s * 4 + c] = Sr[o * 4 + c]; }
    s++;
  };
  for (let i = 0; i < V; i++) edge(i);                       // v = 0 row
  for (let i = 0; i < V; i++) edge((V - 1) * V + i);         // v = max row
  for (let j = 0; j < V; j++) edge(j * V);                   // u = 0 column
  for (let j = 0; j < V; j++) edge(j * V + V - 1);           // u = max column
  let rad = 0;
  for (let o = 0; o < NV; o++) { const r2 = P[o * 3] * P[o * 3] + P[o * 3 + 1] * P[o * 3 + 1] + P[o * 3 + 2] * P[o * 3 + 2]; if (r2 > rad) rad = r2; }
  return { cx, cy, cz, rad: Math.sqrt(rad), emin, emax };
}

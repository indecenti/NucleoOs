// stelle/noise.js — the one gradient noise of the web worlds, in JavaScript AND in GLSL, made to agree.
//
// The surface of a world is computed twice: in a Web Worker (JS, for the terrain chunks, the flight sim's
// ground and the placement of ruins) and on the GPU (GLSL, for the orbital bake of the planet seen from
// space). They must describe the same world, so the lattice hash is INTEGER arithmetic (32-bit multiply,
// xor, shift: Math.imul / >>> in JS, uint in GLSL ES 3.00) and only the smooth interpolation runs in
// floating point — the two sides differ by float rounding, never by a different gradient.
//
// Gradient noise (improved-Perlin gradients, quintic fade), roughly in [-1, 1]; octaves are turned by a
// fixed rotation so the lattice never shows. No allocation, no state: safe in a hot loop.

const fade = (t) => t * t * t * (t * (t * 6 - 15) + 10);
const OFF = 65536;   // lattice coordinates are shifted positive before the hash (same in GLSL)

export function hash3i(ix, iy, iz, s) {
  let h = (Math.imul((ix + OFF) | 0, 0x8da6b343) ^ Math.imul((iy + OFF) | 0, 0xd8163841) ^ Math.imul((iz + OFF) | 0, 0xcb1ab31f) ^ s) | 0;
  h ^= h >>> 16; h = Math.imul(h, 0x7feb352d); h ^= h >>> 15; h = Math.imul(h, 0x846ca68b); h ^= h >>> 16;
  return h >>> 0;
}
function grad(h, x, y, z) {
  h &= 15;
  const u = h < 8 ? x : y, v = h < 4 ? y : (h === 12 || h === 14 ? x : z);
  return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}
// 3D gradient noise, seed s (uint32)
export function noise3(x, y, z, s) {
  const ix = Math.floor(x), iy = Math.floor(y), iz = Math.floor(z);
  const fx = x - ix, fy = y - iy, fz = z - iz;
  const u = fade(fx), v = fade(fy), w = fade(fz);
  const a = grad(hash3i(ix, iy, iz, s), fx, fy, fz), b = grad(hash3i(ix + 1, iy, iz, s), fx - 1, fy, fz);
  const c = grad(hash3i(ix, iy + 1, iz, s), fx, fy - 1, fz), d = grad(hash3i(ix + 1, iy + 1, iz, s), fx - 1, fy - 1, fz);
  const e = grad(hash3i(ix, iy, iz + 1, s), fx, fy, fz - 1), f = grad(hash3i(ix + 1, iy, iz + 1, s), fx - 1, fy, fz - 1);
  const g = grad(hash3i(ix, iy + 1, iz + 1, s), fx, fy - 1, fz - 1), h = grad(hash3i(ix + 1, iy + 1, iz + 1, s), fx - 1, fy - 1, fz - 1);
  const ab = a + (b - a) * u, cd = c + (d - c) * u, ef = e + (f - e) * u, gh = g + (h - g) * u;
  const abcd = ab + (cd - ab) * v, efgh = ef + (gh - ef) * v;
  return abcd + (efgh - abcd) * w;
}

// the octave rotation (orthonormal, iq's): p' = M p * 2.03 + shift — the same constants in GLSL below
const M = [0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64];
const P = { x: 0, y: 0, z: 0 };
function rot(x, y, z, k) {
  P.x = (M[0] * x + M[3] * y + M[6] * z) * k + 1.7;
  P.y = (M[1] * x + M[4] * y + M[7] * z) * k + 9.2;
  P.z = (M[2] * x + M[5] * y + M[8] * z) * k + 3.1;
}
// fractal sum of n octaves, gain g, normalised to about [-1, 1]
export function fbm(x, y, z, n, s, g = 0.5) {
  let a = 1, sum = 0, norm = 0;
  for (let i = 0; i < n; i++) {
    sum += a * noise3(x, y, z, (s + i * 0x9E3779B9) >>> 0); norm += a; a *= g;
    rot(x, y, z, 2.03); x = P.x; y = P.y; z = P.z;
  }
  return sum / norm;
}
// ridged multifractal (sharp crests), about [0, 1]
export function ridged(x, y, z, n, s, g = 0.5) {
  let a = 1, sum = 0, norm = 0, prev = 1;
  for (let i = 0; i < n; i++) {
    let r = 1 - Math.abs(noise3(x, y, z, (s + i * 0x9E3779B9) >>> 0)); r *= r;
    sum += a * r * prev; norm += a; prev = Math.min(1, r * 1.6); a *= g;
    rot(x, y, z, 2.03); x = P.x; y = P.y; z = P.z;
  }
  return sum / norm;
}

// ---- GLSL twin (ESSL 3.00: three.js compiles ShaderMaterial as #version 300 es on WebGL2) -----------------------
export const NOISE_GLSL = /* glsl */`
uint czHash(ivec3 i, uint s) {
  uvec3 u = uvec3(i + 65536);
  uint h = (u.x * 0x8da6b343u) ^ (u.y * 0xd8163841u) ^ (u.z * 0xcb1ab31fu) ^ s;
  h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
  return h;
}
float czGrad(uint h, vec3 p) {
  h &= 15u;
  float u = h < 8u ? p.x : p.y, v = h < 4u ? p.y : ((h == 12u || h == 14u) ? p.x : p.z);
  return ((h & 1u) != 0u ? -u : u) + ((h & 2u) != 0u ? -v : v);
}
float czNoise(vec3 x, uint s) {
  vec3 fl = floor(x); ivec3 i = ivec3(fl); vec3 f = x - fl;
  vec3 w = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  float a = czGrad(czHash(i, s), f), b = czGrad(czHash(i + ivec3(1, 0, 0), s), f - vec3(1, 0, 0));
  float c = czGrad(czHash(i + ivec3(0, 1, 0), s), f - vec3(0, 1, 0)), d = czGrad(czHash(i + ivec3(1, 1, 0), s), f - vec3(1, 1, 0));
  float e = czGrad(czHash(i + ivec3(0, 0, 1), s), f - vec3(0, 0, 1)), g = czGrad(czHash(i + ivec3(1, 0, 1), s), f - vec3(1, 0, 1));
  float k = czGrad(czHash(i + ivec3(0, 1, 1), s), f - vec3(0, 1, 1)), l = czGrad(czHash(i + ivec3(1, 1, 1), s), f - vec3(1, 1, 1));
  return mix(mix(mix(a, b, w.x), mix(c, d, w.x), w.y), mix(mix(e, g, w.x), mix(k, l, w.x), w.y), w.z);
}
const mat3 czM = mat3(0.00, 0.80, 0.60, -0.80, 0.36, -0.48, -0.60, -0.48, 0.64);
float czFbm(vec3 p, int n, uint s, float g) {
  float a = 1.0, sum = 0.0, norm = 0.0;
  for (int i = 0; i < 12; i++) {
    if (i >= n) break;
    sum += a * czNoise(p, s + uint(i) * 0x9E3779B9u); norm += a; a *= g;
    p = czM * p * 2.03 + vec3(1.7, 9.2, 3.1);
  }
  return sum / norm;
}
float czRidged(vec3 p, int n, uint s, float g) {
  float a = 1.0, sum = 0.0, norm = 0.0, prev = 1.0;
  for (int i = 0; i < 12; i++) {
    if (i >= n) break;
    float r = 1.0 - abs(czNoise(p, s + uint(i) * 0x9E3779B9u)); r *= r;
    sum += a * r * prev; norm += a; prev = min(1.0, r * 1.6); a *= g;
    p = czM * p * 2.03 + vec3(1.7, 9.2, 3.1);
  }
  return sum / norm;
}
`;

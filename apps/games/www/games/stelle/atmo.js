// stelle/atmo.js — the atmosphere of a world: single scattering (Rayleigh + Mie) through an exponential shell,
// the sun's optical depth from Schüler's Chapman-function approximation (no inner loop), soft planet shadow.
//
// One model, three users that must agree: the sky (a shell around the world, drawn from inside or outside), the
// aerial perspective of the ground and everything on it (per vertex), and the world seen from orbit (the far
// pass) — so a sky turns into space, a horizon into a limb, without a seam. The JS twin below computes the
// same numbers on the CPU for the sun light, the ambient and the fog colour of the ships, and for tests.
// Uniforms (GLSL): uAtC planet centre (world), uAtR = (R, Rtop, HR, HM), uBR / uBM scattering (1/m), uAtG =
// (g, inscatter gain, dust absorption, -), uSunDir (unit, toward the sun), uSunI (sun irradiance colour).
// Dust absorbs the colours it does not scatter (a desert's haze eats the blue): Mie extinction = 1.11 b_M + k (max b_M - b_M).

export const ATMO_GLSL = /* glsl */`
uniform vec3 uAtC; uniform vec4 uAtR; uniform vec3 uBR; uniform vec3 uBM; uniform vec4 uAtG; uniform vec3 uSunDir; uniform vec3 uSunI;
// density integral (times metres) from radius r to space along a ray with zenith cosine mu, scale height H
float czOD(float r, float mu, float H) {
  float h = r - uAtR.x, x = r / H, c = sqrt(1.5707963 * x), e = exp(-h / H);
  if (mu >= 0.0) return H * e * c / ((c - 1.0) * mu + 1.0);
  float s = sqrt(max(0.0, 1.0 - mu * mu));
  return H * max(0.0, 2.0 * c * sqrt(s) * exp(min(30.0, (r * (1.0 - s) - h) / H)) - e * c / (1.0 - (c - 1.0) * mu));
}
// the planet's shadow on a point at radius r whose sun zenith cosine is muS (soft over a few hundred metres)
float czShadow(float r, float muS) {
  if (muS >= 0.0) return 1.0;
  float d = r * sqrt(max(0.0, 1.0 - muS * muS));
  return smoothstep(uAtR.x * 0.996, uAtR.x * 1.004, d);
}
vec3 czMieExt() { float m = max(uBM.r, max(uBM.g, uBM.b)); return uBM * 1.11 + (vec3(m) - uBM) * uAtG.z; }
// sun transmittance at a point p (relative to the centre)
vec3 czSunT(vec3 p) {
  float r = length(p), mu = dot(p, uSunDir) / r;
  return exp(-(uBR * czOD(r, mu, uAtR.z) + czMieExt() * czOD(r, mu, uAtR.w))) * czShadow(r, mu);
}
// in-scattered light along ro + rd * t, t in [0, tMax] (ro relative to the centre); T = transmittance of the path
vec3 czAtmo(vec3 ro, vec3 rd, float tMax, int N, out vec3 T) {
  T = vec3(1.0);
  float b = dot(ro, rd), c = dot(ro, ro) - uAtR.y * uAtR.y, disc = b * b - c;
  if (disc <= 0.0) return vec3(0.0);
  float sq = sqrt(disc), t0 = max(-b - sq, 0.0), t1 = min(-b + sq, tMax);
  if (t1 <= t0) return vec3(0.0);
  float dt = (t1 - t0) / float(N), mu = dot(rd, uSunDir), g = uAtG.x, g2 = g * g;
  float pr = 0.0596831 * (1.0 + mu * mu);
  float pm = 0.1193662 * (1.0 - g2) * (1.0 + mu * mu) / ((2.0 + g2) * pow(max(1e-4, 1.0 + g2 - 2.0 * g * mu), 1.5));
  vec3 sR = vec3(0.0), sM = vec3(0.0), BMe = czMieExt(); float oR = 0.0, oM = 0.0;
  // a loop the shader compiler cannot unroll (its bound comes from a uniform, which is 0): one copy of the body, so
  // the D3D compiler behind ANGLE builds every program that samples the air in a fraction of the time
  int n = min(N, 24) + int(uAtG.w);
  for (int i = 0; i < n; i++) {
    vec3 p = ro + rd * (t0 + (float(i) + 0.5) * dt); float r = length(p), h = r - uAtR.x;
    float dR = exp(-h / uAtR.z) * dt, dM = exp(-h / uAtR.w) * dt;
    oR += dR * 0.5; oM += dM * 0.5;
    float muS = dot(p, uSunDir) / r, sh = czShadow(r, muS);
    if (sh > 0.0) {
      vec3 a = exp(-(uBR * (oR + czOD(r, muS, uAtR.z)) + BMe * (oM + czOD(r, muS, uAtR.w)))) * sh;
      sR += a * dR; sM += a * dM;
    }
    oR += dR * 0.5; oM += dM * 0.5;
  }
  T = exp(-(uBR * oR + BMe * oM));
  return uSunI * (sR * uBR * pr + sM * uBM * pm) * uAtG.y;
}
`;

// The near scene's shared atmosphere (the world you are in or over): every material that draws inside it — terrain,
// water, flora, sites, ship hulls — references these very objects, so one write per frame updates them all.
// Rtop = 0 means "no atmosphere here" (czAtmo returns at once). Plain arrays: three.js uploads them as vec3/vec4.
export const AU = {
  uAtC: { value: [0, 0, 0] }, uAtR: { value: [1, 0, 1, 1] }, uBR: { value: [0, 0, 0] }, uBM: { value: [0, 0, 0] },
  uAtG: { value: [0.76, 3.2, 0, 0] }, uSunDir: { value: [0, 1, 0] }, uSunI: { value: [1, 1, 1] },
};
export function setAU(A, cx, cy, cz, sun, I) {
  const u = AU;
  u.uAtC.value[0] = cx; u.uAtC.value[1] = cy; u.uAtC.value[2] = cz;
  if (!A) { u.uAtR.value[1] = 0; return; }
  u.uAtR.value[0] = A.R; u.uAtR.value[1] = A.Rt; u.uAtR.value[2] = A.HR; u.uAtR.value[3] = A.HM;
  for (let k = 0; k < 3; k++) { u.uBR.value[k] = A.bR[k]; u.uBM.value[k] = A.bM[k]; u.uSunDir.value[k] = sun[k]; u.uSunI.value[k] = I[k]; }
  u.uAtG.value[0] = A.g; u.uAtG.value[1] = A.gain; u.uAtG.value[2] = A.abs;
}

// ---- the sun's shadow map of the near scene (one cascade that follows the camera; terrain.js renders it) --------------
// Every receiver (terrain, grass, flora, sites, ship hulls) samples the same map through these shared uniforms:
// uShMat world -> shadow texture space ([0,1]^3), uShP = (strength 0..1, texel, normal-offset bias in metres, -).
// uShP.x = 0 means no shadow map (low tier, space): czSunShadow returns 1 at once.
export const SH = { uShMap: { value: null }, uShMat: { value: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1] }, uShP: { value: [0, 1 / 1024, 0.3, 0] } };
export const SHADOW_GLSL = /* glsl */`
uniform highp sampler2DShadow uShMap; uniform mat4 uShMat; uniform vec4 uShP;
float czSunShadow(vec3 wp, vec3 n, float ndl) {
  if (uShP.x <= 0.0) return 1.0;
  vec3 s = (uShMat * vec4(wp + n * uShP.z * (1.0 + 2.0 * (1.0 - clamp(ndl, 0.0, 1.0))), 1.0)).xyz;
  float edge = min(min(s.x, 1.0 - s.x), min(s.y, 1.0 - s.y));
  if (edge <= 0.0 || s.z >= 1.0) return 1.0;
  float t = uShP.y * 0.75; s.z -= uShP.y * 0.6;
  float v = (texture(uShMap, s + vec3(-t, -t, 0.0)) + texture(uShMap, s + vec3(t, -t, 0.0)) + texture(uShMap, s + vec3(-t, t, 0.0)) + texture(uShMap, s + vec3(t, t, 0.0))) * 0.25;
  return mix(1.0, v, uShP.x * smoothstep(0.0, 0.06, edge));
}
`;

// ---- JS twin -------------------------------------------------------------------------------------------------------
// A = { R, Rt, HR, HM, bR: [3], bM: [3], g, gain }, sun = unit [3], I = sun irradiance [3]
export function atmoParams(S, gain = 5) {
  const a = S.atmo;
  const m = Math.max(...a.betaM), bMe = a.betaM.map((b) => b * 1.11 + (m - b) * (a.abs || 0));
  return { R: S.R, Rt: S.R + a.top, HR: a.HR, HM: a.HM, bR: a.betaR.slice(), bM: a.betaM.slice(), bMe, abs: a.abs || 0, g: a.g, gain: gain * (a.gainK || 1) };
}
export function od(A, r, mu, H) {
  const h = r - A.R, x = r / H, c = Math.sqrt(1.5707963 * x), e = Math.exp(-h / H);
  if (mu >= 0) return H * e * c / ((c - 1) * mu + 1);
  const s = Math.sqrt(Math.max(0, 1 - mu * mu));
  return H * Math.max(0, 2 * c * Math.sqrt(s) * Math.exp(Math.min(30, (r * (1 - s) - h) / H)) - e * c / (1 - (c - 1) * mu));
}
export function shadow(A, r, muS) {
  if (muS >= 0) return 1;
  const d = r * Math.sqrt(Math.max(0, 1 - muS * muS)), a = A.R * 0.996, b = A.R * 1.004, t = Math.min(1, Math.max(0, (d - a) / (b - a)));
  return t * t * (3 - 2 * t);
}
// sun transmittance at p (relative to the centre) -> out[3]
export function sunT(A, px, py, pz, sun, out) {
  const r = Math.hypot(px, py, pz) || 1, mu = (px * sun[0] + py * sun[1] + pz * sun[2]) / r;
  const a = od(A, r, mu, A.HR), m = od(A, r, mu, A.HM), sh = shadow(A, r, mu);
  for (let k = 0; k < 3; k++) out[k] = Math.exp(-(A.bR[k] * a + A.bMe[k] * m)) * sh;
  return out;
}
// in-scatter along a ray (same as czAtmo); returns radiance into out[3], transmittance into T[3]
export function scatter(A, ox, oy, oz, dx, dy, dz, tMax, N, sun, I, out, T) {
  out[0] = out[1] = out[2] = 0; T[0] = T[1] = T[2] = 1;
  const b = ox * dx + oy * dy + oz * dz, c = ox * ox + oy * oy + oz * oz - A.Rt * A.Rt, disc = b * b - c;
  if (disc <= 0) return out;
  const sq = Math.sqrt(disc), t0 = Math.max(-b - sq, 0), t1 = Math.min(-b + sq, tMax);
  if (t1 <= t0) return out;
  const dt = (t1 - t0) / N, mu = dx * sun[0] + dy * sun[1] + dz * sun[2], g = A.g, g2 = g * g;
  const pr = 0.0596831 * (1 + mu * mu), pm = 0.1193662 * (1 - g2) * (1 + mu * mu) / ((2 + g2) * Math.pow(Math.max(1e-4, 1 + g2 - 2 * g * mu), 1.5));
  let sR0 = 0, sR1 = 0, sR2 = 0, sM0 = 0, sM1 = 0, sM2 = 0, oR = 0, oM = 0;
  for (let i = 0; i < N; i++) {
    const t = t0 + (i + 0.5) * dt, px = ox + dx * t, py = oy + dy * t, pz = oz + dz * t, r = Math.hypot(px, py, pz), h = r - A.R;
    const dR = Math.exp(-h / A.HR) * dt, dM = Math.exp(-h / A.HM) * dt;
    oR += dR * 0.5; oM += dM * 0.5;
    const muS = (px * sun[0] + py * sun[1] + pz * sun[2]) / r, sh = shadow(A, r, muS);
    if (sh > 0) {
      const a = oR + od(A, r, muS, A.HR), m = oM + od(A, r, muS, A.HM);
      const e0 = Math.exp(-(A.bR[0] * a + A.bMe[0] * m)) * sh, e1 = Math.exp(-(A.bR[1] * a + A.bMe[1] * m)) * sh, e2 = Math.exp(-(A.bR[2] * a + A.bMe[2] * m)) * sh;
      sR0 += e0 * dR; sR1 += e1 * dR; sR2 += e2 * dR; sM0 += e0 * dM; sM1 += e1 * dM; sM2 += e2 * dM;
    }
    oR += dR * 0.5; oM += dM * 0.5;
  }
  for (let k = 0; k < 3; k++) T[k] = Math.exp(-(A.bR[k] * oR + A.bMe[k] * oM));
  out[0] = I[0] * (sR0 * A.bR[0] * pr + sM0 * A.bM[0] * pm) * A.gain;
  out[1] = I[1] * (sR1 * A.bR[1] * pr + sM1 * A.bM[1] * pm) * A.gain;
  out[2] = I[2] * (sR2 * A.bR[2] * pr + sM2 * A.bM[2] * pm) * A.gain;
  return out;
}
// what the camera needs each frame: sun colour at p, the sky's zenith / horizon radiance (ambient, fog, water)
const _o = [0, 0, 0], _t = [0, 0, 0];
export function ambientAt(A, px, py, pz, sun, I, res) {
  const r = Math.hypot(px, py, pz) || 1, ux = px / r, uy = py / r, uz = pz / r;
  sunT(A, px, py, pz, sun, res.sun);
  for (let k = 0; k < 3; k++) res.sun[k] *= I[k];
  scatter(A, px, py, pz, ux, uy, uz, 1e9, 10, sun, I, _o, _t); res.zen[0] = _o[0]; res.zen[1] = _o[1]; res.zen[2] = _o[2];
  // horizon: toward the sun and away from it (a tangent direction in the sun's vertical plane)
  let sx = sun[0] - ux * (sun[0] * ux + sun[1] * uy + sun[2] * uz), sy = sun[1] - uy * (sun[0] * ux + sun[1] * uy + sun[2] * uz), sz = sun[2] - uz * (sun[0] * ux + sun[1] * uy + sun[2] * uz);
  let l = Math.hypot(sx, sy, sz); if (l < 1e-4) { sx = -uz; sy = 0; sz = ux; l = Math.hypot(sx, sy, sz) || 1; }
  sx /= l; sy /= l; sz /= l;
  const hx = sx * 0.995 + ux * 0.1, hy = sy * 0.995 + uy * 0.1, hz = sz * 0.995 + uz * 0.1, hl = Math.hypot(hx, hy, hz);
  scatter(A, px, py, pz, hx / hl, hy / hl, hz / hl, 1e9, 12, sun, I, _o, _t); res.hzSun[0] = _o[0]; res.hzSun[1] = _o[1]; res.hzSun[2] = _o[2];
  const ax = -sx * 0.995 + ux * 0.1, ay = -sy * 0.995 + uy * 0.1, az = -sz * 0.995 + uz * 0.1, al = Math.hypot(ax, ay, az);
  scatter(A, px, py, pz, ax / al, ay / al, az / al, 1e9, 12, sun, I, _o, _t); res.hzAnti[0] = _o[0]; res.hzAnti[1] = _o[1]; res.hzAnti[2] = _o[2];
  res.alt = r - A.R; res.dens = Math.exp(-Math.max(0, res.alt) / A.HR);
  return res;
}
export const ambientRes = () => ({ sun: [0, 0, 0], zen: [0, 0, 0], hzSun: [0, 0, 0], hzAnti: [0, 0, 0], alt: 0, dens: 0 });

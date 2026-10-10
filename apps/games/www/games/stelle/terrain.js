// stelle/terrain.js — the ground of the world you are descending onto, drawn in the near pass with the ships.
//
//   · a cube-sphere quadtree of chunks (stelle/chunk.js, built by stelle/terrain-worker.js off the main thread):
//     split by distance (tier-scaled), horizon- and frustum-culled, CDLOD geomorphing (no popping, no cracks;
//     skirts as a safety net), a fixed pool of geometries whose typed arrays ping-pong with the worker;
//   · one terrain shader: the orbital colour ramp far away (the same czAlbedo the bake paints, so the world from
//     orbit is the world you land on), biome materials up close: ten tileable layers generated once on the GPU
//     (stelle/matgen.js: rock, sand ripples, grass, gravel, snow, ash crust, ice, crystal, dirt, bedding planes),
//     biplanar-projected at two scales, height-blended by slope / height / moisture / the worker's masks, painted
//     with the world's palette, strata bands on the cliffs, snow on the peaks, wet ground darkened by the water,
//     emissive lava and crystal veins; per-pixel detail normals, cavity and valley occlusion;
//   · water: Gerstner waves (displaced near the camera, analytic normals), Fresnel sky and cloud reflection, sun
//     glitter, depth colour over the shelf, shoreline foam bands and whitecaps; a lava sea, a frozen sea, crystal lakes;
//   · light: the sun through the air, cloud shadows, ONE shadow map cascade that follows the camera (terrain, flora,
//     grass, sites and ships cast and receive it: medium / high), a sky-coloured hemisphere with ground bounce,
//     aerial perspective per vertex and a low height fog (mist, dust, ash) tied to the same air;
//   · dense instanced ground cover on the near chunks (blades, tufts, frost, ash, crystal needles: built on the GPU
//     from the chunk's own vertices, wind in the vertex shader), rain splashes;
//   · the sky (a full-screen single-scattering pass that dims the far pass behind it; aurora on ice worlds; lightning),
//     the cloud deck and the puffs you fly through, weather (rain, snow, dust, ash, spores, motes), instanced flora with
//     distance fade and a far LOD, and the sites (ruins, shrines, wrecks, outposts).
// Everything sits in one group placed at the planet centre with the planet's orientation each frame.
import { cubeDir, nodeSize, GRID, maxLevel, MACRO_GLSL, FLORA_KINDS, FLORA_STRIDE } from './planet.js';
import { ATMO_GLSL, AU, SH, SHADOW_GLSL } from './atmo.js';
import { VERTS, SIDE, chunkIndex, newChunkBufs } from './chunk.js';
import { floraGeometry, FLORA_BIG, siteGeometry } from './props.js';
import { makeMaterials, MAT } from './matgen.js';
import { hullMaterial, shipGeometry } from './kit.js';
const SITE_SCALE = { gate: 1.6, archive: 1.4, observatory: 1.6 };

// world settings per quality tier: LOD distance factor K, finest vertex spacing, chunk pool, flora density and range,
// workers, cloud puffs, weather particles, requests in flight; shadow map size and half-extent (m), ground cover range
// (m) and blades per terrain quad, the material layers' size, a second (upper) cloud layer
const WT = {
  low: { K: 2.1, spacing: 4.2, pool: 380, flora: 0.32, floraR: 380, workers: 1, puffs: 48, weather: 700, inflight: 3, floraLod: 150, smallK: 0.6, imp: 0, impCap: 0, shadow: 0, shBox: 0, grass: 0, blades: 0, mat: 256, cloud2: false, near: false },
  medium: { K: 2.5, spacing: 2.6, pool: 640, flora: 0.62, floraR: 650, workers: 2, puffs: 90, weather: 1300, inflight: 4, floraLod: 220, smallK: 0.6, imp: 3400, impCap: 16000, shadow: 1024, shBox: 150, grass: 70, blades: 6, mat: 512, cloud2: true, near: true },
  high: { K: 2.9, spacing: 1.7, pool: 860, flora: 1.0, floraR: 950, workers: 2, puffs: 140, weather: 2200, inflight: 5, floraLod: 300, smallK: 0.7, imp: 5000, impCap: 30000, shadow: 2048, shBox: 230, grass: 115, blades: 9, mat: 512, cloud2: true, near: true },
};
const lin3 = (c) => [Math.pow(c[0], 2.2), Math.pow(c[1], 2.2), Math.pow(c[2], 2.2)];

// ---- procedural detail texture: R fbm, G ridges / cracks, B cells, A fine grain (tileable, mipmapped) -------------------
function detailTexture(THREE, size = 256) {
  const P = 256, perm = new Uint8Array(512);
  for (let i = 0; i < 256; i++) perm[i] = i;
  let s = 1234567; const rnd = () => ((s = (s * 1103515245 + 12345) >>> 0) / 4294967296);
  for (let i = 255; i > 0; i--) { const j = Math.floor(rnd() * (i + 1)); const t = perm[i]; perm[i] = perm[j]; perm[j] = t; }
  for (let i = 0; i < 256; i++) perm[256 + i] = perm[i];
  const g2 = (h, x, y) => { const a = (h & 7) * Math.PI / 4; return Math.cos(a) * x + Math.sin(a) * y; };
  const fade = (t) => t * t * t * (t * (t * 6 - 15) + 10);
  function pn(x, y, per) {   // periodic 2D gradient noise
    const xi = Math.floor(x), yi = Math.floor(y), xf = x - xi, yf = y - yi;
    const X0 = ((xi % per) + per) % per, Y0 = ((yi % per) + per) % per, X1 = (X0 + 1) % per, Y1 = (Y0 + 1) % per;
    const h = (a, b) => perm[(perm[a & 255] + b) & 255];
    const u = fade(xf), v = fade(yf);
    const a = g2(h(X0, Y0), xf, yf), b = g2(h(X1, Y0), xf - 1, yf), c = g2(h(X0, Y1), xf, yf - 1), d = g2(h(X1, Y1), xf - 1, yf - 1);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
  }
  const cells = [], CP = 16; for (let j = 0; j < CP; j++) for (let i = 0; i < CP; i++) cells.push([i + rnd(), j + rnd()]);
  const data = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
    const u = x / size, v = y / size;
    let f = 0, a = 0.5, fr = 0, ar = 0.5, fine = 0, af = 0.5;
    for (let o = 0; o < 5; o++) { const p = 4 << o; f += a * pn(u * p, v * p, p); a *= 0.5; }
    for (let o = 0; o < 4; o++) { const p = 6 << o; const n = 1 - Math.abs(pn(u * p + 17, v * p + 5, p)); fr += ar * n * n; ar *= 0.5; }
    for (let o = 0; o < 3; o++) { const p = 64 << o; fine += af * pn(u * p, v * p, p); af *= 0.5; }
    // Worley F1 on a 16-cell torus
    const cx = u * CP, cy = v * CP; let best = 9;
    for (let dj = -1; dj <= 1; dj++) for (let di = -1; di <= 1; di++) {
      const ci = Math.floor(cx) + di, cj = Math.floor(cy) + dj, k = (((cj % CP) + CP) % CP) * CP + (((ci % CP) + CP) % CP), c = cells[k];
      const px = Math.floor(cx) + di + (c[0] - Math.floor(c[0])), py = Math.floor(cy) + dj + (c[1] - Math.floor(c[1]));
      best = Math.min(best, Math.hypot(px - cx, py - cy));
    }
    const o = (y * size + x) * 4;
    data[o] = Math.max(0, Math.min(255, (f * 0.9 + 0.5) * 255)); data[o + 1] = Math.max(0, Math.min(255, fr * 1.15 * 255));
    data[o + 2] = Math.max(0, Math.min(255, best * 1.3 * 255)); data[o + 3] = Math.max(0, Math.min(255, (fine * 1.1 + 0.5) * 255));
  }
  const t = new THREE.DataTexture(data, size, size, THREE.RGBAFormat);
  t.wrapS = t.wrapT = THREE.RepeatWrapping; t.magFilter = THREE.LinearFilter; t.minFilter = THREE.LinearMipmapLinearFilter; t.generateMipmaps = true; t.anisotropy = 4;
  t.needsUpdate = true;
  return t;
}

// ---- shaders -----------------------------------------------------------------------------------------------------------
const MACRO_UNIFORMS = /* glsl */`uniform uvec4 uSeedA; uniform float uF, uMntK, uSea, uType; uniform vec3 r0, r1, r2, r3, r4, uOcean;`;
const COMMON = /* glsl */`
uniform sampler2D uDetail; uniform sampler2D uCloud; uniform mat3 uPInv; uniform vec3 uAxis;
uniform float uCloudOff, uCloudAlt, uCloudK, uTime, uR;
uniform vec3 uAmbZ, uAmbH, uNight, uGround;
uniform vec4 uFog; uniform vec3 uFogC; uniform vec4 uBolt;
vec2 czEq(vec3 n) { return vec2(atan(n.z, n.x + (abs(n.x) + abs(n.z) < 1e-6 ? 1e-6 : 0.0)) / 6.2831853 + 0.5, asin(clamp(n.y, -1.0, 1.0)) / 3.14159265 + 0.5); }
float czH12(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
// cloud shadow at a planet-local point (elevation e): march to the deck along the sun, read the coverage there
float czCloudShadow(vec3 pl, float e, vec3 Ll) {
  if (uCloudK < 0.01) return 1.0;
  vec3 up = normalize(pl); float c = dot(Ll, up); if (c < 0.02) return 1.0;
  vec3 pc = pl + Ll * max(0.0, uCloudAlt - e) / max(c, 0.12);
  vec2 uv = czEq(normalize(pc)); uv.x += uCloudOff;
  float cov = texture2D(uCloud, uv).r * uCloudK;
  return 1.0 - 0.72 * smoothstep(0.3, 0.75, cov);
}
// low height fog (mist in the valleys, dust and ash near the ground): exponential in elevation, integrated along the
// view ray; uFog = (density at sea level 1/m, 1/scale height, camera elevation, max amount); uFogC its lit colour
vec3 czFog(vec3 col, float e, float dist) {
  if (uFog.x <= 0.0) return col;
  float b = uFog.y, hc = uFog.z, dh = e - hc;
  float k = abs(dh * b) > 1e-3 ? (1.0 - exp(-b * dh)) / (b * dh) : 1.0;
  float F = uFog.x * dist * exp(-b * max(hc, -20.0)) * k;
  return mix(col, uFogC, min(1.0 - exp(-F), uFog.w));
}
`;
// the sky seen from the camera, one small table per frame (the sky pass, the water and the clouds read it): u = azimuth
// from the sun, v = the zenith angle split at the true horizon (a small world's horizon dips far below level), both
// stretched where the sky changes fastest
const SKYVIEW_GLSL = /* glsl */`
uniform sampler2D uSkyLut; uniform vec3 uSkyUp, uSkyS1, uSkyS2; uniform vec2 uSkyHz;
vec2 czSkyUV(vec3 d) {
  float th = acos(clamp(dot(d, uSkyUp), -1.0, 1.0)), v;
  if (th < uSkyHz.x) v = (1.0 - sqrt(max(0.0, 1.0 - th / uSkyHz.x))) * 0.5;
  else v = sqrt(clamp((th - uSkyHz.x) / max(uSkyHz.y, 1e-4), 0.0, 1.0)) * 0.5 + 0.5;
  vec2 hz = vec2(dot(d, uSkyS1), dot(d, uSkyS2)); float hl = length(hz), cl = hl > 1e-5 ? hz.x / hl : 1.0;
  return vec2(sqrt(clamp(0.5 - 0.5 * cl, 0.0, 1.0)), v);
}
vec4 czSkyView(vec3 d) { return texture2D(uSkyLut, czSkyUV(d)); }
`;
const SKYLUT_FRAG = /* glsl */`varying vec2 vUv;
uniform vec3 uSkyUp, uSkyS1, uSkyS2; uniform vec2 uSkyHz;
${ATMO_GLSL}
void main() {
  float th;
  if (vUv.y < 0.5) { float c = 1.0 - vUv.y * 2.0; th = (1.0 - c * c) * uSkyHz.x; }
  else { float c = vUv.y * 2.0 - 1.0; th = uSkyHz.x + c * c * uSkyHz.y; }
  float cl = 1.0 - 2.0 * vUv.x * vUv.x, sl = sqrt(max(0.0, 1.0 - cl * cl));
  vec3 rd = normalize(uSkyUp * cos(th) + (uSkyS1 * cl + uSkyS2 * sl) * sin(th)), ro = cameraPosition - uAtC;
  float b = dot(ro, rd), c = dot(ro, ro) - uAtR.x * uAtR.x, disc = b * b - c, tMax = 1e9;
  if (disc > 0.0) { float t = -b - sqrt(disc); if (t > 0.0) tMax = t; }
  vec3 T, L = czAtmo(ro, rd, tMax, 20, T);
  gl_FragColor = vec4(L, dot(T, vec3(0.2126, 0.7152, 0.0722)));
}`;
// Gerstner swell in a planet-fixed tangent frame (re-anchored near the camera with a cross-fade): returns the height,
// the slope (d/dx, d/dz) and, in disp, the horizontal travel of the water
const WAVES_GLSL = /* glsl */`
uniform vec4 uWD[4]; uniform vec4 uWave; uniform vec3 uWT1, uWT2, uWT1b, uWT2b;
float czWaveD = 0.0;   // the viewing distance (set by the fragment shader; 0 in the vertex shader)
vec3 czWave1(vec3 p, vec3 T1, vec3 T2, inout vec2 disp) {
  vec2 xz = vec2(dot(p, T1), dot(p, T2)); vec3 r = vec3(0.0);
  for (int i = 0; i < 4; i++) {
    vec4 w = uWD[i]; float k = 6.2831853 / w.z, om = sqrt(9.81 * k), ph = k * dot(w.xy, xz) - om * uWave.z + float(i) * 1.7;
    float A = w.w * uWave.x * (1.0 - smoothstep(w.z * 14.0, w.z * 45.0, czWaveD)), c = cos(ph), s = sin(ph);
    r.x += A * c; r.yz -= A * k * w.xy * s; disp += uWave.y * A * w.xy * c;
  }
  return r;
}
vec3 czWaves(vec3 p, out vec2 disp, out vec3 T1, out vec3 T2) {
  disp = vec2(0.0); T1 = uWT1; T2 = uWT2;
  vec3 a = czWave1(p, uWT1, uWT2, disp);
  if (uWave.w > 0.001) { vec2 d2 = vec2(0.0); vec3 b = czWave1(p, uWT1b, uWT2b, d2); a = mix(a, b, uWave.w); disp = mix(disp, d2, uWave.w); if (uWave.w > 0.5) { T1 = uWT1b; T2 = uWT2b; } }
  return a;
}
`;
const TERRAIN_VERT = /* glsl */`
attribute vec4 aMor; attribute vec4 aSrf; attribute vec4 aEx;
uniform vec2 uLod; uniform mat3 uPInv;
varying vec3 vW; varying vec3 vN; varying vec3 vL; varying vec4 vSrf; varying float vRock; varying vec3 vIns; varying vec3 vTr; varying vec3 vSunT; varying float vDist; varying vec4 vEx;
${ATMO_GLSL}
${WAVES_GLSL}
void main() {
  float lev = floor(aMor.w), dl = uLod.x * uLod.y * exp2(-lev);
  float d0 = length((modelViewMatrix * vec4(position, 1.0)).xyz);
  vec3 pos = position + aMor.xyz * smoothstep(1.35 * dl, 1.85 * dl, d0);
  // the sea near the camera rises and falls (object space = the planet's own frame, shifted to the chunk)
  if (uWave.x > 0.0 && aSrf.x < 0.0 && d0 < 1100.0) {
    vec3 pl = uPInv * ((modelMatrix * vec4(pos, 1.0)).xyz - uAtC), T1, T2; vec2 disp;
    vec3 wv = czWaves(pl, disp, T1, T2);
    float f = 1.0 - smoothstep(500.0, 1100.0, d0);
    pos += (normalize(pl) * wv.x + T1 * disp.x + T2 * disp.y) * f * smoothstep(0.0, 1.5, -aSrf.x);
  }
  vec4 mv = modelViewMatrix * vec4(pos, 1.0), w = modelMatrix * vec4(pos, 1.0);
  vW = w.xyz; vN = normalize(mat3(modelMatrix) * normal); vL = uPInv * (w.xyz - uAtC);
  vSrf = aSrf; vRock = fract(aMor.w); vDist = length(mv.xyz); vEx = aEx;
  vIns = czAtmo(cameraPosition - uAtC, (w.xyz - cameraPosition) / max(vDist, 1e-3), vDist, 8, vTr);
  vSunT = czSunT(w.xyz - uAtC);
  gl_Position = projectionMatrix * mv;
}`;
const TERRAIN_FRAG = /* glsl */`
precision highp sampler2DArray;
${MACRO_UNIFORMS}
uniform vec3 uGlow, uWater, uDeep, uHeadDir; uniform vec4 uShadow, uHead; uniform float uFade, uSnow, uA, uNear;
uniform sampler2DArray uMat; uniform vec4 uMS[4]; uniform vec3 uMA[4]; uniform vec3 uMB[4]; uniform vec4 uStrata;
varying vec3 vW; varying vec3 vN; varying vec3 vL; varying vec4 vSrf; varying float vRock; varying vec3 vIns; varying vec3 vTr; varying vec3 vSunT; varying float vDist; varying vec4 vEx;
${ATMO_GLSL}
${MACRO_GLSL}
${COMMON}
${SHADOW_GLSL}
${WAVES_GLSL}
${SKYVIEW_GLSL}
// ---- biplanar projection of the ground layers (iq): the two dominant planes of the normal, at two scales -------------
// (axis vectors instead of indices: no dynamic indexing — the D3D shader compiler behind ANGLE chokes on it)
vec3 gA1, gA2, gB1, gB2; vec2 gW;
void czBiplanar(vec3 n) {
  vec3 an = abs(n);
  vec3 ma = an.x > an.y ? (an.x > an.z ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0)) : (an.y > an.z ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
  vec3 mi = an.x < an.y ? (an.x < an.z ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 0.0, 1.0)) : (an.y < an.z ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0));
  vec3 me = vec3(1.0) - ma - mi;
  gA1 = ma.zxy; gA2 = ma.yzx; gB1 = me.zxy; gB2 = me.yzx;   // the plane across X reads (y, z), across Y (z, x), across Z (x, y)
  vec2 w = clamp((vec2(dot(an, ma), dot(an, me)) - 0.5773) / 0.4227, 0.0, 1.0); w *= w; w *= w;
  gW = w / max(w.x + w.y, 1e-4);
}
// one layer at one scale: (height, tone); its normal tilt added to d (planet-local), scaled by k
vec2 czTex(float L, vec3 p, inout vec3 d, float k) {
  vec4 a = texture(uMat, vec3(dot(p, gA1), dot(p, gA2), L));
  vec4 b = gW.y > 0.03 ? texture(uMat, vec3(dot(p, gB1), dot(p, gB2), L)) : a;
  vec2 na = (a.xy * 2.0 - 1.0) * (gW.x * k), nb = (b.xy * 2.0 - 1.0) * (gW.y * k);
  d += gA1 * na.x + gA2 * na.y + gB1 * nb.x + gB2 * nb.y;
  return vec2(a.z * gW.x + b.z * gW.y, a.w * gW.x + b.w * gW.y);
}
// a material slot: a wide scale (anti-tiling, always) and the near one (fades out with distance)
vec2 czSlot(vec4 ms, vec3 P, float kN, out vec3 d) {
  d = vec3(0.0);
  vec2 r = czTex(ms.x, P / (ms.y * 6.7) + vec3(0.37, 0.71, 0.13), d, 0.45 * ms.z);
  if (kN > 0.01) { vec2 n = czTex(ms.x, P / ms.y, d, kN * ms.z); r = mix(r, vec2((r.x + n.x) * 0.5, n.y), kN); }
  return r;
}
void main() {
  if (uFade < 0.999 && czH12(gl_FragCoord.xy) > uFade) discard;
  vec3 Ng = normalize(vN), N = Ng, up = normalize(vW - uAtC), V = normalize(cameraPosition - vW), L = uSunDir;
  vec3 Nl = uPInv * N, upL = normalize(vL);
  float e = vSrf.x, h = vSrf.y, mo = vSrf.z, m1 = vSrf.w, lat = dot(up, uAxis), wet;
  float ao = vEx.x, wetM = vEx.y, m3 = vEx.z, cv = vEx.w;
  vec3 macroA = czAlbedo(vec4(h, mo, 0.0, 0.0), lat, wet);
  float nearK = 1.0 - smoothstep(2500.0, 9000.0, vDist);
  vec3 alb = macroA, emit = vec3(0.0); float spec = 0.0, rough = 0.8;
  vec3 Ll = uPInv * L;
  float cs = czCloudShadow(vL, e, Ll);
  vec3 col;
  if (uSea > 0.0 && e < 0.0) {
    // ---- the liquid: water, a frozen sea, lava, a crystal lake
    float depth = -e, t = uTime;
    czBiplanar(upL);   // the layers lie flat on the liquid
    vec2 disp; vec3 T1, T2; czWaveD = vDist; vec3 wv = czWaves(vL, disp, T1, T2);
    float wk = (1.0 - smoothstep(400.0, 2500.0, vDist)) * uWave.x;
    vec3 nL = normalize(upL - (T1 * wv.y + T2 * wv.z) * (1.0 - smoothstep(600.0, 3000.0, vDist)));
    vec2 wq = vec2(dot(vL, T1), dot(vL, T2));
    vec2 g = vec2(texture2D(uDetail, wq / 31.0 + vec2(t * 0.011, t * 0.004)).r + texture2D(uDetail, wq.yx / 23.0 - vec2(t * 0.007, t * 0.012)).a - 1.0,
                  texture2D(uDetail, wq / 27.0 + vec2(-t * 0.009, t * 0.006) + 0.5).r + texture2D(uDetail, wq.yx / 19.0 + vec2(t * 0.013, -t * 0.005) + 0.25).a - 1.0);
    float sunUp = max(dot(up, L), 0.0);
    float shw = czSunShadow(vW, up, sunUp);
    if (uType > 4.5 && uType < 5.5) {   // lava: plates of dark crust drifting on open channels of glowing rock
      vec3 dd;
      // the crust at two scales: its cracks (the layer's dark edges) glow as thin lines; wide molten pools open and close
      vec3 dd2;
      vec2 cr = czTex(5.0, vL / 34.0 + vec3(t * 0.02, 0.0, t * 0.013), dd, 1.0), cr2 = czTex(5.0, vL / 113.0 + vec3(0.31, t * 0.006, 0.17), dd2, 0.5);
      float lines = 1.0 - smoothstep(0.02, 0.13, min(cr.y, cr2.y + 0.04));
      float pool = smoothstep(0.64, 0.86, texture2D(uDetail, wq / 520.0 + vec2(t * 0.002, 0.0)).r * 0.75 + texture2D(uDetail, wq / 190.0 - vec2(0.0, t * 0.003)).g * 0.35 + 0.06 * sin(t * 0.21));
      float heat = max(lines * (0.5 + 0.5 * pool), pool * 0.85), crust = 1.0 - heat;
      alb = vec3(0.05, 0.035, 0.03) * (0.7 + 0.6 * cr.x);
      float flow = 0.75 + 0.5 * texture2D(uDetail, wq / 60.0 + vec2(t * 0.03, -t * 0.02)).r + 0.35 * sin(t * 0.8 + cr.x * 12.0);
      emit = uGlow * heat * heat * flow * 0.62 + uGlow * heat * 0.12 + uGlow * 0.03 * crust;   // dull red under the crust, orange-gold only in the open cracks
      vec3 nl = normalize(upL + dd * 0.6 * crust);
      N = normalize(transpose(uPInv) * nl);
      col = alb * (uSunI * vSunT * max(dot(N, L), 0.0) * 0.3183 * cs * shw + uAmbZ * (0.6 + 0.4 * dot(N, up)) + uAmbH * 0.3 + uNight) + emit;
    } else if (uType > 2.5 && uType < 3.5) {   // the frozen sea: pale blue ice, pressure ridges, fractures, a gloss
      vec3 dd;
      vec2 ic = czTex(6.0, vL / 22.0, dd, 1.0);
      vec3 nw = normalize(transpose(uPInv) * normalize(upL + dd * 0.35));
      alb = mix(vec3(0.5, 0.68, 0.86), vec3(0.88, 0.93, 0.98), smoothstep(0.25, 0.85, ic.y)) * (0.85 + 0.25 * texture2D(uDetail, vL.xz / 300.0).r);
      vec3 R = reflect(-V, nw), sky = mix(uAmbH, uAmbZ, clamp(dot(R, up) * 2.0, 0.0, 1.0)) * 1.4;
      float fr = 0.03 + 0.97 * pow(1.0 - max(dot(nw, V), 0.0), 5.0);
      col = alb * (uSunI * vSunT * max(dot(nw, L), 0.0) * 0.3183 * cs * shw + uAmbZ * 1.1 + uAmbH * 0.3 + uNight);
      col = mix(col, sky, fr * 0.6) + uSunI * vSunT * cs * shw * pow(max(dot(R, L), 0.0), 250.0) * 1.5;
      N = nw;
    } else {   // water (and the violet glass of the crystal lakes)
      vec3 nl = normalize(nL + (T1 * g.x + T2 * g.y) * (0.14 * (0.2 + 0.8 * (1.0 - smoothstep(40.0, 900.0, vDist)))));
      vec3 nw = normalize(transpose(uPInv) * nl);
      N = nw;
      // depth colour: the shelf's sand shows through the shallows, turquoise, then the deep
      float sh = exp(-depth * 0.32), mid = exp(-depth / 16.0);
      vec3 bed = uMA[3] * (0.55 + 0.3 * texture2D(uDetail, vL.xz / 17.0).r);
      vec3 wc = mix(uDeep, mix(uWater, bed * mix(vec3(1.0), uWater * 3.0, 0.55), sh * 0.85), mid);
      vec3 R = reflect(-V, nw); R = normalize(R + up * max(0.0, -dot(R, up)) * 1.05);
      vec3 sky = czSkyView(R).rgb * 1.15 + uAmbZ * 0.25;
      if (uCloudK > 0.01) {   // the cloud deck in the water
        vec3 Rl = uPInv * R; float ru = max(dot(Rl, upL), 0.06);
        vec2 cuv = czEq(normalize(vL + Rl * (uCloudAlt - e) / ru)); cuv.x += uCloudOff;
        float cc = smoothstep(0.32, 0.75, texture2D(uCloud, cuv).r * uCloudK);
        sky = mix(sky, uSunI * vSunT * 0.32 * cs + uAmbZ * 1.6 + uAmbH * 0.4, cc * 0.85);
      }
      float fr = 0.02 + 0.98 * pow(1.0 - max(dot(nw, V), 0.0), 5.0);
      vec3 sunC = uSunI * vSunT * cs * shw;
      col = wc * (sunC * sunUp * 0.3183 * 0.75 + (uAmbZ + uAmbH * 0.3) * 0.95 + uNight);
      col = mix(col, sky, fr);
      float crest = clamp(wv.x / max(uWave.x * 0.55, 0.05), 0.0, 1.0) * (1.0 - smoothstep(80.0, 900.0, vDist));
      col += uWater * uSunI * vSunT * cs * crest * crest * (0.25 + 0.75 * pow(max(dot(-V, L), 0.0), 3.0)) * 0.22 * (1.0 - fr);
      float rl = max(dot(R, L), 0.0);
      // the sun's glitter: a tight core, a broad sheen, and sparkles where the ripples catch it
      float sp = pow(rl, 900.0) * 9.0 + pow(rl, 90.0) * 0.25 + pow(rl, 18.0) * 0.02;
      float glit = step(0.93, czH12(floor(vL.xz * 2.3 + vL.y * 0.7) + floor(t * 6.0))) * pow(rl, 30.0) * 3.0 * (1.0 - smoothstep(30.0, 400.0, vDist));
      col += sunC * (sp + glit);
      // foam: bands that run up the shore, the swash on the sand, whitecaps on the crests in a blow
      float fn = texture2D(uDetail, wq / 7.0 + vec2(t * 0.02, 0.0)).a + texture2D(uDetail, wq.yx / 4.0 - vec2(0.0, t * 0.03)).r * 0.6;
      float bands = smoothstep(0.62, 0.95, 0.5 + 0.5 * sin(depth * 4.5 - t * 1.4 + fn * 3.0));
      float foam = (1.0 - smoothstep(0.0, 2.4, depth)) * smoothstep(0.55, 1.1, fn + bands * 0.6 + (1.0 - smoothstep(0.0, 0.5, depth)) * 0.6);
      foam = max(foam, smoothstep(0.65, 1.0, wv.x / max(uWave.x * 0.9, 0.05)) * smoothstep(0.6, 1.0, fn) * uWave.x * 0.8 * (1.0 - smoothstep(200.0, 1200.0, vDist)));
      col = mix(col, (sunC * sunUp * 0.3183 + uAmbZ * 1.2 + uAmbH * 0.4) * 0.95, clamp(foam, 0.0, 1.0) * 0.9);
      if (uType > 6.5) col += uGlow * 0.12 * smoothstep(0.8, 0.95, texture2D(uDetail, vL.xz / 9.0).g);
      alb = wc;
    }
  } else {
    // ---- the ground: ten material layers in four slots per world (stelle/matgen.js), masks from the worker
    czBiplanar(Nl);
    vec3 P = vL;
    float kN = uNear * (1.0 - smoothstep(110.0, 240.0, vDist));
    float kD = 1.0 - smoothstep(600.0, 4000.0, vDist);
    vec4 mv4 = texture2D(uDetail, vec2(dot(P, gA1), dot(P, gA2)) / 410.0);   // big soft patches (and the strata warp)
    float mv = mv4.r * 0.65 + mv4.a * 0.35;
    float slope = 1.0 - clamp(dot(Ng, up), 0.0, 1.0);
    float cliff = smoothstep(0.3, 0.5, slope + (mv - 0.5) * 0.25);
    // slot weights by world type: 0 flat ground, 1 second ground, 2 cliff rock, 3 the special one
    float w0 = 1.0, w1 = 0.0, w3 = 0.0;
    if (uType < 0.5) {            // rocky: red dust and sand, gravel and scree, banded cliffs, pale canyon sand
      w3 = clamp(m1 * 1.3 - 0.15, 0.0, 1.0); w1 = clamp(m3 * 1.4 + smoothstep(0.62, 0.82, mv) * 0.6, 0.0, 1.0) * (1.0 - w3);
      cliff = max(cliff, smoothstep(0.17, 0.32, slope) * 0.75);
    } else if (uType < 1.5) {     // desert: rippled dune sand, wind-cut gravel flats, layered rock, salt pans
      w3 = m3; w1 = (1.0 - m1) * (1.0 - w3); w0 = m1;
      cliff = smoothstep(0.32, 0.55, slope + (1.0 - m1) * 0.12);
    } else if (uType < 2.5) {     // ocean islands: grass, dirt in patches, dark cliffs, beaches
      w3 = 1.0 - smoothstep(1.6, 3.6 + mv * 3.0, e); w1 = smoothstep(0.55, 0.8, mv) * 0.7 * (1.0 - w3);
      cliff = max(cliff, m3 * smoothstep(0.15, 0.3, slope));
    } else if (uType < 3.5) {     // ice: wind-packed snow, scoured blue ice, rock ridges, blue ice walls
      w1 = clamp(smoothstep(0.58, 0.85, mv) * 0.8 + m1, 0.0, 1.0); w3 = m3 * cliff;
    } else if (uType < 4.5) {     // jungle: moss and undergrowth, mud banks, mossy rock, pale karst
      w1 = clamp(m1 * 0.9 + wetM * 0.5 * smoothstep(0.4, 0.7, mv), 0.0, 1.0); w3 = m3 * cliff;
    } else if (uType < 5.5) {     // volcanic: ash drifts, basalt crust, black rock, lava crust (glows below)
      w1 = 1.0 - m3; w0 = m3; w3 = clamp(m1 * 1.4, 0.0, 1.0);
    } else {                      // crystal: violet facets, gravel, crystal walls, bright facet mesas
      w1 = smoothstep(0.6, 0.85, mv) * 0.7; w3 = m3;
    }
    float w2 = cliff; w0 *= 1.0 - w2; w1 *= 1.0 - w2; w3 *= (uType > 2.5 && uType < 4.5) ? 1.0 : 1.0 - w2;
    if (uType > 2.5 && uType < 4.5) w2 *= 1.0 - w3;
    float ws = w0 + w1 + w2 + w3; w0 /= ws; w1 /= ws; w2 /= ws; w3 /= ws;
    vec3 d0 = vec3(0.0), d1 = vec3(0.0), d2 = vec3(0.0), d3 = vec3(0.0);
    vec2 t0 = vec2(0.5), t1 = vec2(0.5), t2 = vec2(0.5), t3 = vec2(0.5);
    if (nearK > 0.0) {
      if (w0 > 0.01) t0 = czSlot(uMS[0], P, kN, d0);
      if (w1 > 0.01) t1 = czSlot(uMS[1], P, kN, d1);
      if (w2 > 0.01) t2 = czSlot(uMS[2], P, kN, d2);
      if (w3 > 0.01) t3 = czSlot(uMS[3], P, kN, d3);
    }
    // height blend: the higher grain wins at the edges (sand fills the cracks, stones poke through)
    vec4 hb = vec4(t0.x, t1.x, t2.x, t3.x) + vec4(w0, w1, w2, w3) * 1.6;
    float hm = max(max(hb.x, hb.y), max(hb.z, hb.w)) - 0.35;
    vec4 ww = max(hb - hm, 0.0) * step(0.01, vec4(w0, w1, w2, w3)); ww /= max(ww.x + ww.y + ww.z + ww.w, 1e-4);
    vec3 c0 = mix(uMA[0], uMB[0], t0.y), c1 = mix(uMA[1], uMB[1], t1.y), c3 = mix(uMA[3], uMB[3], t3.y);
    // the cliffs: bedding bands by elevation (warped), each band its own shade between the cliff's two colours
    float bz = e / uStrata.x + (mv4.g - 0.5) * uStrata.y + dot(P, vec3(0.013, 0.007, 0.011));
    float bh = fract(sin(floor(bz) * 91.7 + 3.1) * 4375.85), bf = fract(bz);
    vec3 c2 = mix(uMA[2], uMB[2], clamp(t2.y * 0.55 + bh * 0.6 * uStrata.z, 0.0, 1.0)) * (1.0 - uStrata.w * 0.22 * smoothstep(0.0, 0.08, bf) * smoothstep(0.2, 0.08, bf));
    alb = c0 * ww.x + c1 * ww.y + c2 * ww.z + c3 * ww.w;
    float th = t0.x * ww.x + t1.x * ww.y + t2.x * ww.z + t3.x * ww.w;
    vec3 dN = (d0 * ww.x + d1 * ww.y + d2 * ww.z * 1.3 + d3 * ww.w) * kD;
    spec = uMS[0].w * ww.x + uMS[1].w * ww.y + uMS[2].w * ww.z + uMS[3].w * ww.w;
    alb *= 0.84 + 0.32 * mv;                                      // big soft patches: no tile repeats from the air
    if (uType > 0.5 && uType < 1.5) alb *= mix(1.0, 0.72 + 0.5 * cv, ww.x);   // dune crests catch the light, troughs and slip faces darken
    alb = mix(alb, macroA * (dot(alb, vec3(0.333)) / max(dot(macroA, vec3(0.333)), 0.02)), 0.12);   // a touch of the orbital colour
    // emissive: lava in the cracks of the crust, crystal veins
    if (uType > 4.5 && uType < 5.5) {
      float crack = 1.0 - smoothstep(0.05, 0.35, t3.y);
      emit = uGlow * ww.w * (0.12 + 0.75 * crack * crack) * (1.0 + 0.3 * sin(uTime * 1.7 + mv * 11.0)) * 0.7;
      emit += uGlow * 0.3 * smoothstep(0.94, 0.99, mv4.b) * (1.0 - cliff) * ww.y;   // embers in the crust
    } else if (uType > 6.5) {
      // veins: the crystal's own facet edges light up along the vein lines (thin, pulsing), a faint glow round them
      float edge = smoothstep(0.8, 0.97, max(t0.y * ww.x, max(t2.y * ww.z, t3.y * ww.w)));
      emit = uGlow * smoothstep(0.25, 0.85, m1) * (0.26 + 2.6 * edge) * (0.75 + 0.25 * sin(uTime * 2.1 + mv * 9.0 + e * 0.1));
      emit += uGlow * 0.25 * edge * (0.6 + 0.4 * sin(uTime * 1.3 + bz)) * (1.0 - m1);
    }
    // water nearby darkens the ground and makes it shine; snow on the high ground
    bool lavaW = uType > 4.5 && uType < 5.5;
    float wk = lavaW ? 0.0 : clamp(wetM * (uSea > 0.0 && uType > 1.5 ? 1.0 : 0.7), 0.0, 1.0) * (1.0 - ww.z * 0.5);
    if (lavaW) emit += (alb * 2.0 + 0.03) * uGlow * wetM * wetM * (0.16 + 0.05 * sin(uTime * 1.3 + mv * 7.0));   // the lava's glow on the rock round it
    alb *= 1.0 - 0.42 * wk; spec = max(spec, wk * 0.55);
    if (uSnow > 0.0) {
      float sn = smoothstep(uSnow, uSnow + 45.0, e + (mv - 0.5) * 70.0 + cv * 20.0) * (1.0 - smoothstep(0.38, 0.62, slope));
      alb = mix(alb, vec3(0.86, 0.9, 0.96) * (0.92 + 0.12 * th), sn); spec = max(spec, sn * 0.3); dN *= 1.0 - sn * 0.6;
    }
    alb = mix(macroA, alb, nearK);
    ao = mix(1.0, clamp(ao * (0.6 + 0.55 * th), 0.0, 1.0), 0.65 * nearK) * (0.88 + 0.12 * cv);
    vec3 nl = normalize(Nl + dN);
    N = normalize(transpose(uPInv) * nl);
    float ndl = dot(N, L), shw = czSunShadow(vW, Ng, dot(Ng, L));
    if (uShadow.w > 0.0) shw *= 1.0 - 0.72 * (1.0 - smoothstep(uShadow.w * 0.3, uShadow.w, distance(vW, uShadow.xyz)));
    vec3 sunC = uSunI * vSunT * cs * shw;
    float hup = dot(N, up) * 0.5 + 0.5;
    vec3 amb = (uAmbZ * (0.35 + 0.65 * hup) + uAmbH * 0.3 + uGround * (1.0 - hup)) * ao + uNight;
    col = alb * (sunC * max(ndl, 0.0) * 0.3183 * (0.7 + 0.3 * ao) + amb);
    if (spec > 0.01) {
      vec3 H = normalize(L + V); float sh = mix(18.0, 160.0, spec);
      col += sunC * spec * pow(max(dot(N, H), 0.0), sh) * (sh + 8.0) * 0.004 * max(ndl, 0.0);
    }
    if (uType > 2.5 && uType < 3.5 || uSnow > 0.0) {   // snow and ice glitter in the sun
      float gl = step(0.985, czH12(floor(P.xz * 7.0 + P.y * 3.1) + floor(V.xy * 5.0))) * pow(max(dot(reflect(-L, N), V), 0.0), 6.0) * (1.0 - smoothstep(20.0, 160.0, vDist));
      col += sunC * gl * (uType > 2.5 && uType < 3.5 ? 1.0 : 0.3) * 1.6;
    }
    col += emit * (0.4 + 0.6 * nearK);
  }
  if (uHead.w > 0.0) { vec3 hv = vW - uHead.xyz; float hd = length(hv); vec3 hn = hv / hd;
    col += alb * uHead.w * smoothstep(0.8, 0.95, dot(hn, uHeadDir)) * max(dot(N, -hn), 0.0) / (1.0 + hd * hd * 0.0003); }
  if (uBolt.w > 0.0) col += alb * uBolt.xyz * uBolt.w * (0.5 + 0.5 * max(dot(N, up), 0.0));
  col = col * vTr + vIns;
  col = czFog(col, e, vDist);
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// the shadow pass: one depth material for every caster (terrain chunks keep their geomorph, instanced flora its
// instance matrix; the main camera's position drives the morph so the shadow matches what is drawn)
const DEPTH_VERT = /* glsl */`
attribute vec4 aMor; uniform vec2 uLod; uniform vec3 uMainCam;
void main() {
  vec4 p = vec4(position, 1.0);
  #ifdef USE_INSTANCING
    p = instanceMatrix * p;
  #endif
  vec4 w = modelMatrix * p;
  float lev = floor(aMor.w), dl = uLod.x * uLod.y * exp2(-lev);
  w.xyz += mat3(modelMatrix) * aMor.xyz * smoothstep(1.35 * dl, 1.85 * dl, distance(w.xyz, uMainCam));
  gl_Position = projectionMatrix * viewMatrix * w;
}`;
const DEPTH_FRAG = /* glsl */`void main() { gl_FragColor = vec4(1.0); }`;
// the sky: a full-screen pass in the near scene (the far pass's stars, sun and other worlds show through by T);
// aurora curtains on ice worlds at night, the flash of lightning in the cloud
const SKY_VERT = /* glsl */`varying vec3 vRay;
void main() { vec4 v = inverse(projectionMatrix) * vec4(position.xy, 1.0, 1.0); v /= v.w; vRay = (inverse(viewMatrix) * vec4(v.xyz, 0.0)).xyz; gl_Position = vec4(position.xy, 1.0, 1.0); }`;
const SKY_FRAG = /* glsl */`varying vec3 vRay;
uniform vec4 uAur; uniform vec4 uBolt; uniform sampler2D uDetail; uniform mat3 uPInv;
${ATMO_GLSL}
${SKYVIEW_GLSL}
void main() {
  vec3 rd = normalize(vRay), ro = cameraPosition - uAtC;
  float b = dot(ro, rd), c = dot(ro, ro) - uAtR.x * uAtR.x, disc = b * b - c, tMax = 1e9;
  if (disc > 0.0) { float t = -b - sqrt(disc); if (t > 0.0) tMax = t; }
  vec4 sv = czSkyView(rd); vec3 L = sv.rgb;
  if (uAur.x > 0.0 && tMax > 1e8) {   // aurora: curtains of light between 2.6 and 6 km, folding in the solar wind
    vec3 up = normalize(ro); float mu = dot(rd, up);
    if (mu > 0.02) {
      vec3 acc = vec3(0.0); float r0 = length(ro);
      for (int i = 0; i < 10; i++) {
        float hgt = uAtR.x + 2600.0 + float(i) * 380.0, bb = dot(ro, rd), cc = dot(ro, ro) - hgt * hgt, dd = bb * bb - cc;
        if (dd < 0.0) continue;
        vec3 q = uPInv * (ro + rd * (-bb + sqrt(dd)));
        float n1 = texture2D(uDetail, q.xz / 5200.0 + vec2(uAur.y * 0.004, 0.0)).r, n2 = texture2D(uDetail, q.zx / 1900.0 - vec2(0.0, uAur.y * 0.006)).g;
        // folded curtains (a few, not a sky full), combed into vertical rays, breathing slowly
        float cur = fract(q.x / 3400.0 + n1 * 1.3 + q.z / 9100.0);
        float band = exp(-abs(cur - 0.5) * 26.0) * smoothstep(0.35, 0.7, texture2D(uDetail, q.xz / 14000.0 + 0.37).r + 0.15 * sin(uAur.y * 0.05));
        band *= 0.35 + 0.9 * texture2D(uDetail, vec2(q.x / 140.0 + q.z / 260.0, 0.13 + uAur.y * 0.01)).g * (0.6 + 0.4 * n2);
        float k = float(i) / 9.0;
        acc += mix(vec3(0.15, 1.0, 0.45), vec3(0.75, 0.25, 1.0), k * k) * band * (1.0 - k * 0.6);
      }
      L += acc * uAur.x * 0.05 * smoothstep(0.02, 0.25, mu);
    }
  }
  if (uBolt.w > 0.0) L += uBolt.xyz * uBolt.w * 0.18 * (0.4 + 0.6 * max(dot(rd, normalize(ro)), 0.0));
  gl_FragColor = vec4(L, sv.a);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// the cloud decks: coverage from the orbital bake (the same clouds you saw from space), detail up close, lit as a
// volume would be (darker where more cloud stands toward the sun, a silver lining, darker bases), in the air
const CLOUD_VERT = /* glsl */`uniform mat3 uPInv; varying vec3 vW; varying vec3 vL;
${'uniform vec3 uAtC;'}
void main() { vec4 w = modelMatrix * vec4(position, 1.0); vW = w.xyz; vL = uPInv * (w.xyz - uAtC); gl_Position = projectionMatrix * viewMatrix * w; }`;
const CLOUD_FRAG = /* glsl */`
uniform float uLayer;
varying vec3 vW; varying vec3 vL;
${ATMO_GLSL}
${COMMON}
void main() {
  vec3 n = normalize(vL); vec2 uv = czEq(n); uv.x += uCloudOff * (1.0 + uLayer * 2.0);
  vec3 up = normalize(vW - uAtC), V = normalize(cameraPosition - vW), L = uSunDir, Ll = uPInv * L;
  float a, lit;
  if (uLayer < 0.5) {
    float cov = texture2D(uCloud, uv).r * uCloudK;
    vec4 d = texture2D(uDetail, vL.xz / 1400.0 + vec2(uTime * 0.002, 0.0)) * 0.6 + texture2D(uDetail, vL.zy / 520.0 - vec2(0.0, uTime * 0.003)) * 0.4;
    float dens = cov + (d.r - 0.5) * 0.45 + (d.a - 0.5) * 0.2;
    a = smoothstep(0.32, 0.72, dens);
    if (a < 0.004) discard;
    // light through the volume: how much cloud stands between this point and the sun
    vec2 uvS = czEq(normalize(vL + Ll * 900.0)); uvS.x += uCloudOff;
    float covS = texture2D(uCloud, uvS).r * uCloudK + (texture2D(uDetail, (vL.xz + Ll.xz * 900.0) / 1400.0 + vec2(uTime * 0.002, 0.0)).r - 0.5) * 0.3;
    float through = exp(-max(covS - 0.25, 0.0) * 2.6) * (0.55 + 0.45 * smoothstep(0.85, 0.4, dens));
    lit = through;
  } else {   // the high veil: thin streaks of ice cloud, combed by the wind
    // long streaks: the detail stretched along one axis, thinned by a slow mask so whole regions of sky stay clear
    float st = texture2D(uDetail, vec2(vL.x / 24000.0 + uTime * 0.0004, vL.z / 1500.0)).g * 0.65 + texture2D(uDetail, vec2(vL.z / 30000.0, vL.y / 1100.0 + uTime * 0.0006)).r * 0.35;
    float mask = smoothstep(0.45, 0.75, texture2D(uDetail, n.xz * 1.7 + 0.31).r);
    a = smoothstep(0.6, 0.84, st) * mask * 0.14;
    if (a < 0.004) discard;
    lit = 1.0;
  }
  vec3 sunT = czSunT(vW - uAtC);
  float below = step(dot(V, up), 0.0);   // seen from underneath: a darker base, light filtering through the thin parts
  float base = mix(0.55 + 0.45 * max(dot(up, L), 0.0), 0.32 * (1.0 - a) + 0.18, below);
  float silver = pow(max(dot(-V, L), 0.0), 8.0) * (1.0 - a) * 1.6 + pow(max(dot(-V, L), 0.0), 2.0) * 0.12;
  vec3 col = uSunI * sunT * (base * lit * 0.34 + silver) + uAmbZ * (1.4 + 0.4 * (1.0 - below)) + uAmbH * 0.4;
  col += uBolt.xyz * uBolt.w * 0.6 * (1.0 - uLayer);
  float dist = length(vW - cameraPosition); vec3 T, ins = czAtmo(cameraPosition - uAtC, (vW - cameraPosition) / dist, dist, 6, T);
  gl_FragColor = vec4(col * T + ins, a * 0.94 * smoothstep(20.0, 160.0, dist));
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// gas giants: three cloud decks coloured by the world's own bands (the orbital bake), the lowest one opaque — the floor
const DECK_FRAG = /* glsl */`
uniform sampler2D uSurf; uniform float uDeckI, uDeckK;
varying vec3 vW; varying vec3 vL;
${ATMO_GLSL}
${COMMON}
void main() {
  vec3 n = normalize(vL); vec2 uv = czEq(n);
  float drift = uCloudOff * (6.0 + uDeckI * 5.0);
  vec3 band = texture2D(uSurf, vec2(uv.x + drift, uv.y)).rgb;
  vec4 d = texture2D(uDetail, vL.xz / (2600.0 - uDeckI * 700.0) + vec2(drift * 40.0, uDeckI * 0.37)) * 0.6 + texture2D(uDetail, vL.zy / 760.0 - vec2(0.0, drift * 30.0)) * 0.4;
  float a = uDeckI < 0.5 ? 1.0 : smoothstep(0.46 + uDeckI * 0.04, 0.74, d.r + (d.a - 0.5) * 0.35) * uDeckK;
  if (a < 0.004) discard;
  vec3 up = normalize(vW - uAtC), V = normalize(cameraPosition - vW), L = uSunDir;
  vec3 sunT = czSunT(vW - uAtC);
  float below = step(dot(V, up), 0.0);
  float lit = mix(0.55 + 0.45 * max(dot(up, L), 0.0), 0.3 + 0.2 * a, below) * (0.8 + 0.35 * d.g);
  vec3 col = band * (uSunI * sunT * lit * 0.42 + uAmbZ * 1.4) * (uDeckI < 0.5 ? 0.85 : 1.1);
  col += uBolt.xyz * uBolt.w * 0.5 * (1.0 + d.r);
  float dist = length(vW - cameraPosition); vec3 T, ins = czAtmo(cameraPosition - uAtC, (vW - cameraPosition) / dist, dist, 6, T);
  gl_FragColor = vec4(col * T + ins, a);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// cloud puffs: soft billboards in a ring of cells round the camera on the deck — the clouds that rush past
const PUFF_VERT = /* glsl */`
attribute vec4 aP;   // cell offset x, z (0..1), height jitter (-1..1), size jitter
uniform vec3 uCamL, uE, uNn, uUp; uniform float uCell, uSpan, uDeckR, uThick; uniform mat3 uPRot; uniform vec3 uAtC;
uniform sampler2D uCloud; uniform float uCloudOff, uCloudK;
varying vec2 vUv; varying float vA; varying vec3 vW; varying float vH;
vec2 czEq2(vec3 n) { return vec2(atan(n.z, n.x + (abs(n.x) + abs(n.z) < 1e-6 ? 1e-6 : 0.0)) / 6.2831853 + 0.5, asin(clamp(n.y, -1.0, 1.0)) / 3.14159265 + 0.5); }
void main() {
  // a lattice fixed to the planet on the two axes of the cube face below (uE, uNn), wrapped round the camera, then
  // lifted onto the deck sphere along the third axis (uUp, signed): the puffs stay put while you fly through them
  float cx = dot(uCamL, uE), cz = dot(uCamL, uNn);
  float x = mod(aP.x * uSpan - cx, uSpan) - uSpan * 0.5 + cx, z = mod(aP.y * uSpan - cz, uSpan) - uSpan * 0.5 + cz;
  float rr = uDeckR + aP.z * uThick * 0.5;
  vec3 pl = uE * x + uNn * z + uUp * sqrt(max(0.0, rr * rr - x * x - z * z));
  vec3 dir = normalize(pl);
  vec2 uv = czEq2(dir); uv.x += uCloudOff;
  float cov = texture(uCloud, uv).r * uCloudK;
  float size = uCell * (0.9 + aP.w * 0.8) * smoothstep(0.3, 0.6, cov);
  vec3 w = uPRot * pl + uAtC; vW = w;
  vec4 mv = viewMatrix * vec4(w, 1.0);
  mv.xy += position.xy * size;
  float d = length(mv.xyz);
  vA = smoothstep(0.3, 0.65, cov) * smoothstep(uSpan * 0.5, uSpan * 0.3, length(vec2(x - cx, z - cz))) * smoothstep(8.0, 60.0, d);
  vUv = position.xy; vH = aP.z;
  gl_Position = projectionMatrix * mv;
}`;
const PUFF_FRAG = /* glsl */`
varying vec2 vUv; varying float vA; varying vec3 vW; varying float vH;
${ATMO_GLSL}
uniform vec3 uAmbZ; uniform vec4 uBolt;
void main() {
  float r = length(vUv), a = pow(max(0.0, 1.0 - r), 1.6) * vA;
  if (a < 0.003) discard;
  vec3 up = normalize(vW - uAtC), L = uSunDir, sunT = czSunT(vW - uAtC);
  // a lit top and a shaded belly: the side of the billboard toward the sun is brighter
  float side = clamp(0.5 + 0.5 * dot(vec3(vUv, 0.0), vec3(0.0, 1.0, 0.0)) + vH * 0.25, 0.0, 1.0);
  vec3 col = uSunI * sunT * (0.16 + 0.22 * side) * (0.6 + 0.4 * max(dot(up, L), 0.0)) + uAmbZ * (1.2 + 0.5 * side) + uBolt.xyz * uBolt.w * 0.4;
  gl_FragColor = vec4(col, a * 0.75);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// weather: streaks / flakes in a box that wraps round the camera, falling along -up with the wind
const WX_VERT = /* glsl */`
attribute float aEnd; attribute float aR;
uniform vec3 uCam, uFall, uUp; uniform float uBox, uLen, uT, uSize;
varying float vA; varying float vR;
void main() {
  vec3 p = position + uFall * uT * (0.7 + aR * 0.6);
  p = mod(p - uCam + uBox, 2.0 * uBox) - uBox;
  float d = length(p);
  p += uCam;
  if (aEnd > 0.5) p -= normalize(uFall) * uLen * (0.6 + aR * 0.8);
  vA = smoothstep(uBox, uBox * 0.5, d) * smoothstep(1.5, 6.0, d); vR = aR;
  vec4 mv = viewMatrix * vec4(p, 1.0);
  gl_Position = projectionMatrix * mv;
  gl_PointSize = uSize * (0.6 + aR) * 220.0 / max(-mv.z, 1.0);
}`;
const WX_FRAG = /* glsl */`uniform vec3 uCol; uniform float uK, uPts; varying float vA; varying float vR;
void main() { float a = vA * uK; if (uPts > 0.5) { vec2 c = gl_PointCoord - 0.5; a *= smoothstep(0.5, 0.15, length(c)); } if (a < 0.003) discard;
  gl_FragColor = vec4(uCol * (0.7 + vR * 0.6), a);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// flora: instanced, wind sway, glow, distance fade (shrink + dither), aerial perspective per vertex; the sun's shadow
// map, a world-tinted cap on what faces the sky (snow, moss, ash, dust), occlusion toward the root, light through leaves
const FLORA_VERT = /* glsl */`
attribute vec3 aMat;
uniform float uTime, uWind, uRange; uniform vec3 uWindDir;
varying vec3 vN; varying vec3 vC; varying vec3 vW; varying float vGlow; varying vec3 vIns; varying vec3 vTr; varying vec3 vSunT; varying float vFade; varying vec3 vUp; varying float vHt; varying float vE;
${ATMO_GLSL}
void main() {
  vec3 p = position;
  vec4 base = modelMatrix * instanceMatrix * vec4(0.0, 0.0, 0.0, 1.0);
  float gust = 0.75 + 0.5 * sin(uTime * 0.7 + base.x * 0.013 + base.z * 0.011) * sin(uTime * 0.23 + base.y * 0.017);
  float sw = aMat.z * uWind * gust * (0.06 + 0.05 * sin(uTime * 1.9 + instanceMatrix[3].x * 0.37 + instanceMatrix[3].z * 0.21));
  p.x += sw * p.y * uWindDir.x; p.z += sw * p.y * uWindDir.z;
  vec4 ip = instanceMatrix * vec4(p, 1.0);
  vec4 w = modelMatrix * ip;
  vec4 mv = viewMatrix * w; float d = length(mv.xyz);
  vFade = smoothstep(uRange, uRange * 0.82, d);
  w.xyz = base.xyz + (w.xyz - base.xyz) * (0.15 + 0.85 * vFade);
  mv = viewMatrix * w;
  vW = w.xyz; vN = normalize(mat3(modelMatrix) * mat3(instanceMatrix) * normal);
  vec3 tint = vec3(1.0);
  #ifdef USE_INSTANCING_COLOR
    tint = instanceColor;
  #endif
  vC = color * mix(vec3(1.0), tint, aMat.x); vGlow = aMat.y;
  vUp = normalize(base.xyz - uAtC); vHt = clamp(position.y, 0.0, 1.2);
  vE = length(base.xyz - uAtC) - uAtR.x;
  vIns = czAtmo(cameraPosition - uAtC, (w.xyz - cameraPosition) / max(d, 1e-3), d, 4, vTr);
  vSunT = czSunT(base.xyz - uAtC);
  gl_Position = projectionMatrix * mv;
}`;
const FLORA_FRAG = /* glsl */`
uniform vec3 uAmbZ, uAmbH, uNight, uGlow, uGround; uniform vec4 uHead; uniform vec3 uHeadDir; uniform vec4 uCap; uniform vec3 uCapC; uniform vec4 uFog; uniform vec3 uFogC; uniform vec4 uBolt;
varying vec3 vN; varying vec3 vC; varying vec3 vW; varying float vGlow; varying vec3 vIns; varying vec3 vTr; varying vec3 vSunT; varying float vFade; varying vec3 vUp; varying float vHt; varying float vE;
${ATMO_GLSL}
${SHADOW_GLSL}
vec3 czFog(vec3 col, float e, float dist) {
  if (uFog.x <= 0.0) return col;
  float b = uFog.y, hc = uFog.z, dh = e - hc, k = abs(dh * b) > 1e-3 ? (1.0 - exp(-b * dh)) / (b * dh) : 1.0;
  return mix(col, uFogC, min(1.0 - exp(-uFog.x * dist * exp(-b * max(hc, -20.0)) * k), uFog.w));
}
void main() {
  if (vFade < 0.999 && fract(sin(dot(floor(gl_FragCoord.xy), vec2(12.9898, 78.233))) * 43758.5453) > vFade) discard;
  vec3 N = normalize(vN); if (!gl_FrontFacing) N = -N;
  vec3 L = uSunDir, V = normalize(cameraPosition - vW);
  float ndl = dot(N, L);
  float sh = czSunShadow(vW, N, ndl);
  vec3 alb = vC;
  // what faces the sky takes the world's cover (snow, moss, ash, dust)
  float cap = uCap.x * smoothstep(uCap.y, uCap.y + 0.25, dot(N, vUp)) * smoothstep(0.05, 0.4, vHt);
  alb = mix(alb, uCapC, cap);
  float occ = mix(0.45, 1.0, smoothstep(0.0, 0.55, vHt));            // dark at the root, open at the crown
  float wrap = max(ndl * 0.7 + 0.3, 0.0);
  float trans = pow(max(dot(-V, L), 0.0), 4.0) * 0.6 * step(0.15, vHt) * (1.0 - cap);   // light through the leaves
  vec3 sun = uSunI * vSunT * sh;
  float hup = dot(N, vUp) * 0.5 + 0.5;
  vec3 col = alb * (sun * (wrap * 0.3183 + trans * 0.25) + (uAmbZ * (0.45 + 0.55 * hup) + uAmbH * 0.3 + uGround * (1.0 - hup)) * occ + uNight);
  col += vC * uGlow * vGlow;
  if (uHead.w > 0.0) { vec3 hv = vW - uHead.xyz; float hd = length(hv); col += alb * uHead.w * smoothstep(0.8, 0.95, dot(hv / hd, uHeadDir)) / (1.0 + hd * hd * 0.0003); }
  if (uBolt.w > 0.0) col += alb * uBolt.xyz * uBolt.w * 0.4;
  col = col * vTr + vIns;
  col = czFog(col, vE, length(vW - cameraPosition));
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// ground cover: one instance per terrain quad of a near chunk, its four corners read straight from the chunk's own
// vertex buffer (no CPU work, nothing streamed); each instance a tuft of blades placed, sized and coloured by hash
// and by the worker's masks, bent by the wind (gusts travel across the field), shrunk to nothing at the range
const GRASS_VERT = /* glsl */`
attribute vec3 iP0; attribute vec3 iP1; attribute vec3 iP2; attribute vec3 iP3; attribute vec3 iN; attribute vec4 iS; attribute vec4 iX;
// position = the blade vertex: x across (-1..1), y up (0..1), z the blade's index in the tuft
uniform float uTime, uWind, uRange, uGH, uGW, uDens, uSide, uKind, uSea; uniform vec3 uWindDir, uAtC; uniform sampler2D uDetail; uniform mat3 uPInv;
uniform vec3 uGA, uGB, uGT;
varying vec3 vC; varying vec3 vW; varying float vY; varying vec3 vN; varying float vD; varying float vE;
float hh(float n) { return fract(sin(n) * 43758.5453123); }
void main() {
  vec3 aB = position;
  int inst = gl_InstanceID, side = int(uSide), col = inst - (inst / side) * side;
  float sd = float(inst) * 0.618034 + aB.z * 0.371 + iP0.x * 0.0131 + iP0.z * 0.0071;
  float u = hh(sd * 12.9898 + 1.3), v = hh(sd * 78.233 + 4.1), r = hh(sd * 37.719 + 9.7), r2 = hh(sd * 11.3 + 2.9);
  vec3 p = mix(mix(iP0, iP1, u), mix(iP2, iP3, u), v);   // inside the quad, on the mesh as drawn
  vec3 wp = (modelMatrix * vec4(p, 1.0)).xyz, upW = normalize(wp - uAtC), n = normalize(mat3(modelMatrix) * iN);
  // where there is cover, and how much: the worker's masks of this quad
  float e = iS.x, mo = iS.z, m1 = iS.w, m3 = iX.z, slope = 1.0 - clamp(dot(n, upW), 0.0, 1.0), dens = uDens;
  if (uKind < 0.5) dens *= smoothstep(0.3, 0.52, mo) * (1.0 - smoothstep(0.25, 0.45, slope)) * (1.0 - m1 * 0.85);
  else if (uKind < 1.5) dens *= (1.0 - m1) * (0.35 + 0.65 * smoothstep(0.4, 0.6, mo)) * (1.0 - smoothstep(0.2, 0.4, slope)) * (1.0 - m3);
  else if (uKind < 2.5) dens *= (1.0 - smoothstep(0.3, 0.5, slope)) * (0.4 + 0.6 * (1.0 - m1));
  else if (uKind < 3.5) dens *= (1.0 - smoothstep(0.3, 0.5, slope)) * (0.3 + 0.7 * m3);
  else dens *= 1.0 - smoothstep(0.35, 0.55, slope);
  if (uSea > 0.0 && e < 0.6) dens = 0.0;
  float dc = distance(wp, cameraPosition), fade = 1.0 - smoothstep(uRange * 0.55, uRange, dc);
  float H = uGH * (0.55 + 0.9 * r2) * (0.65 + 0.35 * smoothstep(0.3, 0.7, mo)) * fade, Wd = uGW * (0.7 + 0.6 * hh(sd * 3.3));
  if (r >= dens || col == side - 1 || H < 0.02) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); vC = vec3(0.0); vW = wp; vY = 0.0; vN = n; vD = dc; vE = e; return; }
  float a = hh(sd * 5.1) * 6.2831853;
  vec3 t0 = normalize(cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), t1 = cross(n, t0);
  vec3 sdv = t0 * cos(a) + t1 * sin(a), fwd = cross(sdv, n);
  // wind: a gust field scrolling across the ground plus each blade's own flutter
  vec2 gq = (uPInv * (wp - uAtC)).xz / 38.0 - uWindDir.xz * uTime * 0.09;
  float gust = texture2D(uDetail, gq).r * 1.6 - 0.5;
  float bend = (0.22 + uWind * 0.35 * (0.6 + gust) + 0.08 * sin(uTime * 3.1 + sd * 7.0)) * aB.y * aB.y;
  vec3 wdir = normalize(uWindDir - n * dot(uWindDir, n) + fwd * 0.35 * (r2 - 0.5) + 1e-4);
  vec3 pos = wp + sdv * aB.x * Wd * (1.0 - aB.y * 0.85) + n * aB.y * H * (1.0 - bend * 0.35) + wdir * bend * H + fwd * (hh(sd * 2.1) - 0.5) * 0.3 * H * aB.y;
  vW = pos; vY = aB.y; vE = e; vD = dc;
  vC = mix(mix(uGA, uGB, hh(sd * 9.7)), uGT, aB.y * aB.y * 0.8) * (0.8 + 0.4 * hh(sd * 4.4));
  vN = normalize(n + wdir * 0.4 + sdv * aB.x * 0.4);
  gl_Position = projectionMatrix * viewMatrix * vec4(pos, 1.0);
}`;
const GRASS_FRAG = /* glsl */`
uniform vec3 uAmbZ, uAmbH, uNight, uGround; uniform vec4 uFog; uniform vec3 uFogC; uniform vec4 uNearAir; uniform vec3 uNearIns; uniform float uKind; uniform vec3 uGlow;
varying vec3 vC; varying vec3 vW; varying float vY; varying vec3 vN; varying float vD; varying float vE;
${ATMO_GLSL}
${SHADOW_GLSL}
void main() {
  vec3 N = normalize(vN), L = uSunDir, V = normalize(cameraPosition - vW), up = normalize(vW - uAtC);
  if (!gl_FrontFacing) N = -N;
  float ndl = dot(N, L), sh = czSunShadow(vW, up, max(dot(up, L), 0.0));
  float occ = mix(0.35, 1.0, vY);
  float trans = pow(max(dot(-V, L), 0.0), 3.0) * 0.55 * vY;
  vec3 sun = uSunI * czSunT(vW - uAtC) * sh;
  vec3 col = vC * (sun * (max(ndl * 0.6 + 0.4, 0.0) * 0.3183 + trans * 0.3) + (uAmbZ * (0.5 + 0.5 * dot(N, up)) + uAmbH * 0.3 + uGround * 0.3) * occ + uNight);
  if (uKind > 3.5) col += vC * uGlow * 0.35 * vY * vY;          // crystal needles glow at the tips
  if (uKind > 2.5 && uKind < 3.5) col += vec3(2.4, 0.7, 0.12) * step(0.985, fract(sin(dot(floor(vW.xz * 3.0), vec2(12.9898, 78.233))) * 43758.5453)) * vY;   // cinders
  // the air over the few tens of metres to the camera (from the near-air constants the CPU computes each frame)
  col = col * exp(-uNearAir.xyz * vD) + uNearIns * (1.0 - exp(-uNearAir.w * vD));
  if (uFog.x > 0.0) { float b = uFog.y, dh = vE - uFog.z, k = abs(dh * b) > 1e-3 ? (1.0 - exp(-b * dh)) / (b * dh) : 1.0; col = mix(col, uFogC, min(1.0 - exp(-uFog.x * vD * exp(-b * max(uFog.z, -20.0)) * k), uFog.w)); }
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// rain on the ground: rings that open and fade where the drops land, on the near chunks (the same vertex buffers as
// the ground cover), two per terrain quad, each on its own beat
const RAIN_VERT = /* glsl */`
attribute vec3 iP0; attribute vec3 iP1; attribute vec3 iP2; attribute vec3 iP3; attribute vec4 iS;
uniform float uTime, uSide, uRange, uRain; uniform vec3 uAtC;
varying vec2 vUv; varying float vA;
float hh(float n) { return fract(sin(n) * 43758.5453123); }
void main() {
  int inst = gl_InstanceID, side = int(uSide), col = inst - (inst / side) * side;
  float sd = float(inst) * 0.618034 + iP0.x * 0.0131 + iP0.z * 0.0071 + position.z * 0.37;
  float u = hh(sd * 12.9898), v = hh(sd * 78.233), rate = 1.2 + hh(sd * 3.1);
  float ph = fract(uTime * rate + hh(sd * 9.7));
  vec3 wp = (modelMatrix * vec4(mix(mix(iP0, iP1, u), mix(iP2, iP3, u), v), 1.0)).xyz, up = normalize(wp - uAtC);
  float dc = distance(wp, cameraPosition);
  vA = (1.0 - ph) * (1.0 - smoothstep(uRange * 0.5, uRange, dc)) * step(hh(sd * 5.5), uRain);
  vec3 t0 = normalize(cross(up, abs(up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0))), t1 = cross(up, t0);
  float sz = 0.05 + ph * (0.3 + 0.2 * hh(sd));
  vec3 pos = wp + up * (iS.x < 0.0 ? 0.25 : 0.05) + (t0 * position.x + t1 * position.y) * sz;
  vUv = position.xy;
  gl_Position = (vA < 0.01 || col == side - 1) ? vec4(2.0, 2.0, 2.0, 1.0) : projectionMatrix * viewMatrix * vec4(pos, 1.0);
}`;
const RAIN_FRAG = /* glsl */`uniform vec3 uRainC; varying vec2 vUv; varying float vA;
void main() { float r = length(vUv), ring = smoothstep(0.7, 0.9, r) * smoothstep(1.0, 0.9, r) + smoothstep(0.25, 0.0, r) * 0.25; if (ring * vA < 0.01) discard; gl_FragColor = vec4(uRainC * ring * vA, 1.0); }`;
// lightning: a jagged bolt from the cloud base to the ground (rebuilt per strike), HDR white-violet, additive
const BOLT_FRAG = /* glsl */`uniform vec4 uBolt; void main() { gl_FragColor = vec4(vec3(1.4, 1.3, 2.4) * uBolt.w * 6.0, 1.0); }`;
// impostors: the big kinds beyond the flora range as camera-facing cards (turning about the local up), painted once
// per world from their far mesh into an atlas, so the forests and spires carry on to the horizon
const IMP_VERT = /* glsl */`
attribute vec4 aI; attribute vec4 aJ;   // planet-local base + height (m); atlas tile, width / height, shade jitter
uniform float uTiles, uNear, uFar;
varying vec2 vUv; varying vec3 vIns; varying vec3 vTr; varying float vA; varying vec3 vSunT; varying vec3 vUp; varying float vJ;
${ATMO_GLSL}
void main() {
  vec3 base = (modelMatrix * vec4(aI.xyz, 1.0)).xyz, up = normalize(base - uAtC);
  vec3 toC = cameraPosition - base, side = normalize(cross(up, toC) + 1e-5);
  vec3 p = base + side * position.x * aI.w * aJ.y + up * position.y * aI.w;
  float d = length(cameraPosition - p);
  vA = smoothstep(uNear * 0.86, uNear, d) * smoothstep(uFar, uFar * 0.8, d);
  vUv = vec2((aJ.x + position.x + 0.5) / uTiles, position.y);
  vIns = czAtmo(cameraPosition - uAtC, (p - cameraPosition) / max(d, 1e-3), d, 4, vTr);
  vSunT = czSunT(base - uAtC); vUp = up; vJ = aJ.z;
  gl_Position = vA > 0.001 ? projectionMatrix * viewMatrix * vec4(p, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
}`;
const IMP_FRAG = /* glsl */`
uniform sampler2D uImp; uniform vec3 uAmbZ, uAmbH, uNight;
varying vec2 vUv; varying vec3 vIns; varying vec3 vTr; varying float vA; varying vec3 vSunT; varying vec3 vUp; varying float vJ;
${ATMO_GLSL}
void main() {
  vec4 c = texture2D(uImp, vUv); if (c.a < 0.45) discard;
  if (vA < 0.999 && fract(sin(dot(floor(gl_FragCoord.xy), vec2(12.9898, 78.233))) * 43758.5453) > vA) discard;
  float sunUp = max(dot(vUp, uSunDir), 0.0);
  vec3 col = c.rgb * (0.85 + vJ * 0.3) * (uSunI * vSunT * (0.3 + 0.55 * sunUp) * 0.3183 + uAmbZ * 0.9 + uAmbH * 0.3 + uNight);
  gl_FragColor = vec4(col * vTr + vIns, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// painting an impostor: the far mesh seen from the side, albedo with the world's tint and a soft top light
const IMPBAKE_VERT = /* glsl */`attribute vec3 aMat; varying vec3 vC; varying vec3 vN; varying float vG; uniform vec3 uTint;
void main() { vC = color * mix(vec3(1.0), uTint, aMat.x); vG = aMat.y; vN = normal; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;
const IMPBAKE_FRAG = /* glsl */`varying vec3 vC; varying vec3 vN; varying float vG; uniform vec3 uGlowC;
void main() { vec3 n = normalize(vN); float l = 0.55 + 0.45 * max(dot(n, normalize(vec3(0.35, 0.85, 0.4))), 0.0);
  gl_FragColor = vec4(vC * l + vC * uGlowC * vG * 0.15, 1.0); }`;

// ---- palettes per world type (linear), from the planet's own ramp so the ground matches the orbit view ----------------
// The material table: four slots, each a ground layer (stelle/matgen.js), its tile size (m), normal strength, shine, and
// two colours mixed by the layer's tone; strata = (band height m, warp, band contrast, bedding lines)
function palette(S) {
  const R = S.ramp.length ? S.ramp : [[0.1, 0.1, 0.1], [0.2, 0.2, 0.2], [0.3, 0.3, 0.3], [0.4, 0.4, 0.4], [0.5, 0.5, 0.5]];
  const mul = (c, k) => [c[0] * k, c[1] * k, c[2] * k], mix = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
  const L = (r, g, b) => lin3([r, g, b]);
  const slot = (layer, tile, nk, spec, a, b) => ({ layer, tile, nk, spec, a, b });
  const P = { glow: [0, 0, 0], water: S.ocean.map((x) => x * 1.6), deep: S.ocean.map((x) => x * 0.45), snow: -1, strata: [9, 1.4, 1, 1], slots: null,
    grass: null, cap: [0, 0.6, 0, 0], capC: [1, 1, 1], fog: [0, 1 / 80, 0.35] };
  switch (S.type) {
    case 'rocky': P.slots = [
      slot(MAT.dirt, 7, 1.0, 0.02, mix(L(0.5, 0.27, 0.16), R[2], 0.25), mix(L(0.74, 0.46, 0.3), R[3], 0.25)),
      slot(MAT.gravel, 4.5, 1.2, 0.05, L(0.42, 0.25, 0.17), L(0.7, 0.52, 0.4)),
      slot(MAT.strata, 9, 1.3, 0.04, mix(L(0.55, 0.3, 0.19), R[1], 0.3), mix(L(0.86, 0.66, 0.5), R[3], 0.2)),
      slot(MAT.sand, 6, 0.9, 0.02, L(0.78, 0.52, 0.34), L(0.92, 0.7, 0.5))];
      P.snow = S.A * 0.85; P.strata = [7, 1.6, 1, 1]; P.grass = { kind: 1, h: 0.5, w: 0.06, dens: 0.45, a: L(0.56, 0.42, 0.24), b: L(0.66, 0.52, 0.3), t: L(0.86, 0.74, 0.48) };
      P.cap = [0.35, 0.55, 0, 0]; P.capC = L(0.62, 0.42, 0.3); P.fog = [0.00004, 1 / 120, 0.25]; break;
    case 'desert': P.slots = [
      slot(MAT.sand, 5, 1.1, 0.03, mix(L(0.8, 0.5, 0.26), R[3], 0.25), mix(L(0.96, 0.72, 0.46), R[4] || R[3], 0.15)),
      slot(MAT.gravel, 4, 1.1, 0.04, mix(L(0.5, 0.34, 0.22), R[1], 0.3), L(0.72, 0.56, 0.4)),
      slot(MAT.strata, 10, 1.3, 0.04, mix(L(0.46, 0.27, 0.16), R[1], 0.25), mix(L(0.8, 0.58, 0.4), R[2], 0.25)),
      slot(MAT.snow, 12, 0.5, 0.15, L(0.82, 0.78, 0.7), L(0.94, 0.92, 0.86))];
      P.strata = [6, 1.2, 1, 1]; P.grass = { kind: 1, h: 0.35, w: 0.05, dens: 0.18, a: L(0.5, 0.42, 0.25), b: L(0.58, 0.5, 0.3), t: L(0.8, 0.72, 0.5) };
      P.cap = [0.5, 0.5, 0, 0]; P.capC = L(0.88, 0.7, 0.5); P.fog = [0.00008, 1 / 90, 0.4]; break;
    case 'ocean': P.slots = [
      slot(MAT.grass, 4, 1.0, 0.03, mix(L(0.16, 0.36, 0.1), R[3], 0.2), mix(L(0.42, 0.62, 0.22), R[3], 0.2)),
      slot(MAT.dirt, 5, 1.0, 0.03, L(0.3, 0.24, 0.16), L(0.48, 0.4, 0.28)),
      slot(MAT.rock, 8, 1.4, 0.06, L(0.16, 0.15, 0.15), L(0.36, 0.33, 0.3)),
      slot(MAT.sand, 4, 0.8, 0.03, L(0.82, 0.74, 0.55), L(0.96, 0.9, 0.74))];
      P.water = mix(L(0.15, 0.75, 0.72), S.ocean, 0.3); P.deep = mix(L(0.02, 0.12, 0.3), S.ocean, 0.3);
      P.grass = { kind: 0, h: 0.6, w: 0.07, dens: 0.8, a: L(0.3, 0.5, 0.16), b: L(0.42, 0.6, 0.2), t: L(0.7, 0.84, 0.42) };
      P.cap = [0.3, 0.55, 0, 0]; P.capC = L(0.3, 0.5, 0.18); P.fog = [0.00006, 1 / 70, 0.3]; break;
    case 'ice': P.slots = [
      slot(MAT.snow, 7, 0.9, 0.18, L(0.82, 0.87, 0.95), L(0.97, 0.98, 1.0)),
      slot(MAT.ice, 9, 0.9, 0.55, L(0.45, 0.66, 0.86), L(0.74, 0.88, 0.97)),
      slot(MAT.rock, 8, 1.3, 0.08, L(0.3, 0.32, 0.36), L(0.55, 0.58, 0.62)),
      slot(MAT.ice, 6, 1.1, 0.6, L(0.25, 0.5, 0.8), L(0.55, 0.78, 0.95))];
      P.snow = S.A * 0.3; P.strata = [12, 1, 0.5, 0.4]; P.grass = { kind: 2, h: 0.28, w: 0.05, dens: 0.35, a: L(0.75, 0.86, 1.0), b: L(0.85, 0.92, 1.0), t: L(0.95, 0.98, 1.0) };
      P.cap = [0.85, 0.35, 0, 0]; P.capC = L(0.93, 0.95, 1.0); P.fog = [0.00005, 1 / 100, 0.3]; break;
    case 'jungle': P.slots = [
      slot(MAT.grass, 4, 1.1, 0.04, mix(L(0.08, 0.26, 0.06), R[2], 0.25), mix(L(0.3, 0.52, 0.16), R[2], 0.25)),
      slot(MAT.dirt, 5, 1.0, 0.08, L(0.22, 0.16, 0.1), L(0.36, 0.27, 0.17)),
      slot(MAT.rock, 8, 1.4, 0.05, L(0.2, 0.22, 0.18), L(0.42, 0.42, 0.36)),
      slot(MAT.strata, 9, 1.2, 0.04, L(0.5, 0.5, 0.44), L(0.78, 0.76, 0.68))];
      P.water = mix(L(0.18, 0.42, 0.36), S.ocean, 0.4); P.deep = mix(L(0.03, 0.1, 0.12), S.ocean, 0.3); P.snow = S.A * 0.95; P.strata = [8, 1.2, 0.6, 0.6];
      P.grass = { kind: 0, h: 0.85, w: 0.09, dens: 1.0, a: L(0.2, 0.42, 0.12), b: L(0.3, 0.52, 0.16), t: L(0.6, 0.8, 0.32) };
      P.cap = [0.55, 0.5, 0, 0]; P.capC = L(0.18, 0.38, 0.1); P.fog = [0.00016, 1 / 55, 0.45]; break;
    case 'volcanic': P.slots = [
      slot(MAT.ash, 6, 0.9, 0.02, L(0.38, 0.36, 0.35), L(0.6, 0.57, 0.55)),
      slot(MAT.ash, 9, 1.2, 0.06, L(0.13, 0.12, 0.12), L(0.32, 0.29, 0.28)),
      slot(MAT.rock, 8, 1.4, 0.08, L(0.17, 0.155, 0.15), L(0.44, 0.4, 0.37)),
      slot(MAT.ash, 5, 1.3, 0.05, L(0.14, 0.09, 0.07), L(0.36, 0.25, 0.2))];
      P.glow = [3.2, 0.9, 0.18]; P.strata = [5, 1, 0.6, 0.5]; P.grass = { kind: 3, h: 0.2, w: 0.06, dens: 0.35, a: L(0.12, 0.11, 0.11), b: L(0.22, 0.2, 0.19), t: L(0.3, 0.28, 0.27) };
      P.cap = [0.6, 0.45, 0, 0]; P.capC = L(0.36, 0.34, 0.33); P.fog = [0.00012, 1 / 110, 0.45]; break;
    case 'crystal': P.slots = [
      slot(MAT.crystal, 6, 1.1, 0.35, L(0.12, 0.09, 0.22), L(0.34, 0.26, 0.56)),
      slot(MAT.gravel, 4.5, 1.1, 0.2, L(0.16, 0.12, 0.28), L(0.38, 0.3, 0.6)),
      slot(MAT.crystal, 9, 1.4, 0.6, L(0.3, 0.26, 0.62), L(0.68, 0.62, 1.0)),
      slot(MAT.crystal, 5, 1.3, 0.7, L(0.4, 0.5, 0.9), L(0.7, 0.85, 1.0))];
      P.glow = [0.4, 1.6, 2.4]; P.water = L(0.45, 0.3, 0.8); P.deep = L(0.1, 0.05, 0.25); P.snow = S.A * 0.9; P.strata = [10, 1, 0.5, 0.3];
      P.grass = { kind: 4, h: 0.35, w: 0.04, dens: 0.4, a: L(0.4, 0.4, 0.9), b: L(0.5, 0.6, 1.0), t: L(0.75, 0.9, 1.0) };
      P.cap = [0, 0.5, 0, 0]; P.fog = [0.00005, 1 / 80, 0.3]; break;
    default: P.slots = [0, 1, 2, 3].map(() => slot(MAT.rock, 8, 1, 0.05, R[1], R[3]));
  }
  return P;
}
// the colour each flora kind takes on this world (instances vary around it)
function floraTint(S, kind) {
  const R = S.ramp.length ? S.ramp : [[0.3, 0.3, 0.3], [0.3, 0.3, 0.3], [0.3, 0.3, 0.3], [0.3, 0.3, 0.3], [0.3, 0.3, 0.3]];
  switch (kind) {
    case 'rock': case 'hoodoo': case 'spire': case 'boulder': return S.type === 'rocky' || S.type === 'desert' ? lin3([0.78, 0.55, 0.42]) : S.type === 'ice' ? lin3([0.5, 0.52, 0.56]) : S.type === 'volcanic' ? lin3([0.25, 0.23, 0.23]) : S.type === 'crystal' ? lin3([0.42, 0.36, 0.6]) : lin3([0.42, 0.4, 0.38]);
    case 'shrub': return lin3([0.52, 0.5, 0.3]);
    case 'palm': return lin3([0.42, 0.68, 0.28]);
    case 'coral': return lin3([0.75, 0.85, 1.0]);
    case 'icespire': case 'frost': case 'icecrystal': return lin3([0.8, 0.9, 1.0]);
    case 'spiral': return [R[2][0] * 0.6 + 0.04, R[2][1] * 0.9 + 0.08, R[2][2] * 0.5 + 0.03];
    case 'fern': return lin3([0.42, 0.62, 0.3]);
    case 'treefern': return lin3([0.44, 0.66, 0.3]);
    case 'canopy': { const c = lin3([0.42, 0.62, 0.34]); return [c[0] * 0.7 + R[2][0] * 0.3, c[1] * 0.7 + R[2][1] * 0.3, c[2] * 0.7 + R[2][2] * 0.3]; }
    case 'bush': return lin3([0.4, 0.62, 0.28]);
    case 'glowpod': return lin3([0.5, 0.7, 0.45]);
    case 'mushroom': return lin3([0.86, 0.82, 0.92]);
    case 'kelp': return lin3([0.72, 0.62, 0.36]);
    case 'mangrove': return lin3([0.36, 0.6, 0.3]);
    case 'coralspire': return lin3([0.62, 0.66, 1.0]);
    case 'cactus': return lin3([0.5, 0.66, 0.5]);
    case 'juniper': return lin3([0.46, 0.52, 0.32]);
    case 'basalt': case 'ember': return lin3([0.3, 0.29, 0.3]);
    case 'obsidian': return lin3([0.3, 0.28, 0.34]);
    case 'deadtree': return lin3([0.25, 0.22, 0.2]);
    case 'ashtree': return lin3([0.62, 0.6, 0.58]);
    case 'crystree': return lin3([0.62, 0.8, 1.0]);
    case 'geode': return lin3([0.5, 0.46, 0.44]);
    case 'shard': case 'lattice': return lin3([0.75, 0.72, 1.0]);
  }
  return [0.5, 0.5, 0.5];
}

// ========================================================================================================================
export function createWorld(THREE, renderer, scene, tierName) {
  const T = WT[tierName] || WT.medium;
  const group = new THREE.Group(); group.name = 'world'; group.visible = false; scene.add(group);
  const detail = detailTexture(THREE);
  let mats = null;   // the ground layers: generated on the GPU the first time a world is set
  const blank = new THREE.DataTexture(new Uint8Array([0, 0, 0, 255]), 1, 1, THREE.RGBAFormat); blank.needsUpdate = true;
  const pInv = new THREE.Matrix3(), pRot = new THREE.Matrix3(), _q = new THREE.Quaternion(), _m4 = new THREE.Matrix4(), _v = new THREE.Vector3(), _v2 = new THREE.Vector3(), _v3 = new THREE.Vector3();
  // uniforms shared by every world material
  const SU = {
    uDetail: { value: detail }, uCloud: { value: blank }, uPInv: { value: pInv }, uAxis: { value: new THREE.Vector3(0, 1, 0) },
    uCloudOff: { value: 0 }, uCloudAlt: { value: 1000 }, uCloudK: { value: 0 }, uTime: { value: 0 }, uR: { value: 1 },
    uAmbZ: { value: new THREE.Vector3() }, uAmbH: { value: new THREE.Vector3() }, uNight: { value: new THREE.Vector3(0.012, 0.014, 0.022) }, uGround: { value: new THREE.Vector3() },
    uHead: { value: new THREE.Vector4() }, uHeadDir: { value: new THREE.Vector3(0, 0, -1) }, uShadow: { value: new THREE.Vector4() },
    uFog: { value: new THREE.Vector4() }, uFogC: { value: new THREE.Vector3() }, uBolt: { value: new THREE.Vector4() },
  };
  const MU = {   // the macro colour ramp (czAlbedo)
    uSeedA: { value: new THREE.Vector4() }, uF: { value: 2 }, uMntK: { value: 1 }, uSea: { value: 0 }, uType: { value: 0 },
    r0: { value: new THREE.Vector3() }, r1: { value: new THREE.Vector3() }, r2: { value: new THREE.Vector3() }, r3: { value: new THREE.Vector3() }, r4: { value: new THREE.Vector3() }, uOcean: { value: new THREE.Vector3() },
  };
  const WU = {   // the sea's swell: four Gerstner waves (dir xy, wavelength, amplitude), (height k, steepness, time, frame blend), frames
    uWD: { value: [0, 1, 2, 3].map(() => new THREE.Vector4(1, 0, 10, 0)) }, uWave: { value: new THREE.Vector4() },
    uWT1: { value: new THREE.Vector3(1, 0, 0) }, uWT2: { value: new THREE.Vector3(0, 0, 1) }, uWT1b: { value: new THREE.Vector3(1, 0, 0) }, uWT2b: { value: new THREE.Vector3(0, 0, 1) },
  };
  const TU = { uLod: { value: new THREE.Vector2(2.5, 1) }, uFade: { value: 1 }, uSnow: { value: -1 }, uA: { value: 100 }, uNear: { value: T.near ? 1 : 0 },
    uGlow: { value: new THREE.Vector3() }, uWater: { value: new THREE.Vector3() }, uDeep: { value: new THREE.Vector3() },
    uMat: { value: null }, uMS: { value: [0, 1, 2, 3].map(() => new THREE.Vector4()) }, uMA: { value: [0, 1, 2, 3].map(() => new THREE.Vector3()) }, uMB: { value: [0, 1, 2, 3].map(() => new THREE.Vector3()) },
    uStrata: { value: new THREE.Vector4(8, 1, 1, 1) } };
  const terrainMat = new THREE.ShaderMaterial({ vertexShader: TERRAIN_VERT, fragmentShader: TERRAIN_FRAG, uniforms: { ...AU, ...SU, ...MU, ...TU, ...WU, ...SH } });
  // drawn FIRST, in the opaque list (renderOrder), so the ground and every hull cover it with their own aerial perspective
  const AUR = { uAur: { value: new THREE.Vector4() } };
  const SKYV = { uSkyLut: { value: null }, uSkyUp: { value: new THREE.Vector3(0, 1, 0) }, uSkyS1: { value: new THREE.Vector3(1, 0, 0) }, uSkyS2: { value: new THREE.Vector3(0, 0, 1) }, uSkyHz: { value: new THREE.Vector2(Math.PI / 2, Math.PI / 2) } };
  const skyRT = new THREE.WebGLRenderTarget(192, 108, { type: THREE.HalfFloatType, depthBuffer: false, minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter, generateMipmaps: false });
  skyRT.texture.wrapS = THREE.ClampToEdgeWrapping; skyRT.texture.wrapT = THREE.ClampToEdgeWrapping; SKYV.uSkyLut.value = skyRT.texture;
  const lutMat = new THREE.ShaderMaterial({ vertexShader: `varying vec2 vUv; void main() { vUv = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }`, fragmentShader: SKYLUT_FRAG, uniforms: { ...AU, ...SKYV }, depthTest: false, depthWrite: false, toneMapped: false });
  const lutScene = new THREE.Scene(), lutCam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1), lutQuad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), lutMat); lutQuad.frustumCulled = false; lutScene.add(lutQuad);
  Object.assign(terrainMat.uniforms, SKYV);   // the water reflects the sky table
  const skyMat = new THREE.ShaderMaterial({ vertexShader: SKY_VERT, fragmentShader: SKY_FRAG, uniforms: { ...AU, ...AUR, ...SKYV, uBolt: SU.uBolt, uDetail: SU.uDetail, uPInv: SU.uPInv }, depthTest: false, depthWrite: false, transparent: false,
    blending: THREE.CustomBlending, blendEquation: THREE.AddEquation, blendSrc: THREE.OneFactor, blendDst: THREE.SrcAlphaFactor, blendSrcAlpha: THREE.ZeroFactor, blendDstAlpha: THREE.OneFactor });
  const sky = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), skyMat); sky.frustumCulled = false; sky.renderOrder = -1000; sky.visible = false; scene.add(sky);
  const cloudMat = new THREE.ShaderMaterial({ vertexShader: CLOUD_VERT, fragmentShader: CLOUD_FRAG, uniforms: { ...AU, ...SU, uLayer: { value: 0 } }, transparent: true, depthWrite: false, side: THREE.DoubleSide });
  const cloudGeo = new THREE.SphereGeometry(1, 160, 80);
  const clouds = new THREE.Mesh(cloudGeo, cloudMat); clouds.frustumCulled = false; clouds.renderOrder = 5; clouds.visible = false;
  const veilMat = new THREE.ShaderMaterial({ vertexShader: CLOUD_VERT, fragmentShader: CLOUD_FRAG, uniforms: { ...AU, ...SU, uLayer: { value: 1 } }, transparent: true, depthWrite: false, side: THREE.DoubleSide });
  const veil = new THREE.Mesh(cloudGeo, veilMat); veil.frustumCulled = false; veil.renderOrder = 4; veil.visible = false;
  const white = new THREE.DataTexture(new Uint8Array([255, 255, 255, 255]), 1, 1, THREE.RGBAFormat); white.needsUpdate = true;
  const decks = [0, 1, 2].map((i) => {
    const m = new THREE.ShaderMaterial({ vertexShader: CLOUD_VERT, fragmentShader: DECK_FRAG, uniforms: { ...AU, ...SU, uSurf: { value: blank }, uDeckI: { value: i }, uDeckK: { value: [1, 0.95, 0.7][i] } }, transparent: true, depthWrite: false, side: THREE.DoubleSide });
    const d = new THREE.Mesh(cloudGeo, m); d.frustumCulled = false; d.renderOrder = 3 + i; d.visible = false; return d;
  });
  const GAS_DECK = [60, 700, 1650];
  // puffs
  const NP = T.puffs, pGeo = new THREE.InstancedBufferGeometry();
  pGeo.setAttribute('position', new THREE.Float32BufferAttribute([-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 3)); pGeo.setIndex([0, 1, 2, 0, 2, 3]);
  const pAttr = new Float32Array(NP * 4); { let s = 99; const r = () => ((s = (s * 16807) % 2147483647) / 2147483647); for (let i = 0; i < NP; i++) { pAttr[i * 4] = r(); pAttr[i * 4 + 1] = r(); pAttr[i * 4 + 2] = r() * 2 - 1; pAttr[i * 4 + 3] = r(); } }
  pGeo.setAttribute('aP', new THREE.InstancedBufferAttribute(pAttr, 4)); pGeo.instanceCount = NP;
  const puffU = { uCamL: { value: new THREE.Vector3() }, uE: { value: new THREE.Vector3() }, uNn: { value: new THREE.Vector3() }, uUp: { value: new THREE.Vector3() }, uCell: { value: 160 }, uSpan: { value: 2400 },
    uDeckR: { value: 1 }, uThick: { value: 200 }, uPRot: { value: pRot } };
  const puffMat = new THREE.ShaderMaterial({ vertexShader: PUFF_VERT, fragmentShader: PUFF_FRAG, uniforms: { ...AU, ...SU, ...puffU }, transparent: true, depthWrite: false });
  const puffs = new THREE.Mesh(pGeo, puffMat); puffs.frustumCulled = false; puffs.renderOrder = 6; puffs.visible = false; scene.add(puffs);
  // weather
  const NW = T.weather, wPos = new Float32Array(NW * 6), wEnd = new Float32Array(NW * 2), wR = new Float32Array(NW * 2);
  { let s = 7; const r = () => ((s = (s * 16807) % 2147483647) / 2147483647); for (let i = 0; i < NW; i++) { const x = (r() - 0.5) * 120, y = (r() - 0.5) * 120, z = (r() - 0.5) * 120, k = r(); wPos.set([x, y, z, x, y, z], i * 6); wEnd[i * 2 + 1] = 1; wR[i * 2] = wR[i * 2 + 1] = k; } }
  const wGeo = new THREE.BufferGeometry(); wGeo.setAttribute('position', new THREE.BufferAttribute(wPos, 3)); wGeo.setAttribute('aEnd', new THREE.BufferAttribute(wEnd, 1)); wGeo.setAttribute('aR', new THREE.BufferAttribute(wR, 1));
  const wU = { uCam: { value: new THREE.Vector3() }, uFall: { value: new THREE.Vector3() }, uUp: { value: new THREE.Vector3() }, uBox: { value: 60 }, uLen: { value: 1 }, uT: { value: 0 }, uSize: { value: 1 }, uCol: { value: new THREE.Vector3(1, 1, 1) }, uK: { value: 0 }, uPts: { value: 0 } };
  const wLineMat = new THREE.ShaderMaterial({ vertexShader: WX_VERT, fragmentShader: WX_FRAG, uniforms: wU, transparent: true, depthWrite: false });
  const wPtsMat = new THREE.ShaderMaterial({ vertexShader: WX_VERT, fragmentShader: WX_FRAG, uniforms: wU, transparent: true, depthWrite: false });
  const wLines = new THREE.LineSegments(wGeo, wLineMat), wPts = new THREE.Points(wGeo, wPtsMat);
  for (const m of [wLines, wPts]) { m.frustumCulled = false; m.visible = false; m.renderOrder = 7; scene.add(m); }
  // lightning: a jagged bolt rebuilt per strike (a fixed pool of segments), the flash in the sky, clouds and ground
  const BOLT_N = 48, bPos = new Float32Array(BOLT_N * 2 * 3);
  const bGeo = new THREE.BufferGeometry(); bGeo.setAttribute('position', new THREE.BufferAttribute(bPos, 3).setUsage(THREE.DynamicDrawUsage));
  const boltMat = new THREE.ShaderMaterial({ vertexShader: `void main() { gl_Position = projectionMatrix * viewMatrix * vec4(position, 1.0); }`, fragmentShader: BOLT_FRAG, uniforms: { uBolt: SU.uBolt }, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending });
  const bolt = new THREE.LineSegments(bGeo, boltMat); bolt.frustumCulled = false; bolt.visible = false; bolt.renderOrder = 8; scene.add(bolt);
  const BOLT = { t: 0, next: 6, k: 0, dist: 0, events: 0 };
  // flora
  const floraU = { uTime: SU.uTime, uWind: { value: 1 }, uRange: { value: T.floraR * T.smallK }, uWindDir: { value: new THREE.Vector3(1, 0, 0) }, uGlow: { value: new THREE.Vector3(1, 1, 1) },
    uCap: { value: new THREE.Vector4(0, 0.6, 0, 0) }, uCapC: { value: new THREE.Vector3(1, 1, 1) } };
  const floraUni = (range) => ({ ...AU, ...SU, ...SH, ...floraU, uRange: range ? { value: range } : floraU.uRange });
  const floraMat = new THREE.ShaderMaterial({ vertexShader: FLORA_VERT, fragmentShader: FLORA_FRAG, uniforms: floraUni(0), vertexColors: true, side: THREE.DoubleSide });
  // the big ones (trees, spires, ribs) shape the skyline: they are drawn (and fade) twice as far out
  const floraMatBig = new THREE.ShaderMaterial({ vertexShader: FLORA_VERT, fragmentShader: FLORA_FRAG, uniforms: floraUni(T.floraR * 2.2), vertexColors: true, side: THREE.DoubleSide });
  const floraGeo = {}, floraGeoLo = {}; for (const k of FLORA_KINDS) floraGeo[k] = floraGeoLo[k] = null;
  const FLORA_LOD_D = T.floraLod;   // big kinds beyond this (cell distance) use their far LOD mesh
  const siteMat = hullMaterial(THREE, { metal: 0.08, rough: 0.82, panelK: 0.25, panel: 0.12, env: 0.35 });
  const wreckMat = hullMaterial(THREE, { metal: 0.5, rough: 0.6, panel: 0.4, env: 0.5 }); wreckMat.userData.U.uScorch.value = 0.75;

  // ---- the sun's shadow map: one cascade that follows the camera (medium / high) --------------------------------------
  const SHADOW_LAYER = 3;
  let shRT = null, shCam = null, depthMat = null;
  // a 1x1 depth map stands in when there is no shadow map (low tier, before a world): a shadow sampler must always
  // have a depth texture with a compare mode behind it, or the draw that declares it is dropped
  const shDummy = new THREE.WebGLRenderTarget(1, 1, { depthBuffer: true }); shDummy.depthTexture = new THREE.DepthTexture(1, 1, THREE.UnsignedIntType); shDummy.depthTexture.compareFunction = THREE.LessEqualCompare;
  { const prev = renderer.getRenderTarget(); renderer.setRenderTarget(shDummy); renderer.clear(true, true, false); renderer.setRenderTarget(prev); }
  if (!SH.uShMap.value) SH.uShMap.value = shDummy.depthTexture;
  const shU = { uLod: TU.uLod, uMainCam: { value: new THREE.Vector3() } };
  if (T.shadow) {
    shRT = new THREE.WebGLRenderTarget(T.shadow, T.shadow, { depthBuffer: true });
    shRT.depthTexture = new THREE.DepthTexture(T.shadow, T.shadow, THREE.UnsignedIntType);
    shRT.depthTexture.compareFunction = THREE.LessEqualCompare; shRT.depthTexture.minFilter = shRT.depthTexture.magFilter = THREE.LinearFilter;
    shCam = new THREE.OrthographicCamera(-T.shBox, T.shBox, T.shBox, -T.shBox, 1, 6000); shCam.layers.set(SHADOW_LAYER);
    depthMat = new THREE.ShaderMaterial({ vertexShader: DEPTH_VERT, fragmentShader: DEPTH_FRAG, uniforms: shU, side: THREE.DoubleSide });
    depthMat.defaultAttributeValues = { ...depthMat.defaultAttributeValues, aMor: [0, 0, 0, 0] };
    SH.uShMap.value = shRT.depthTexture;
  }
  const shMat = new THREE.Matrix4(), shBias = new THREE.Matrix4().set(0.5, 0, 0, 0.5, 0, 0.5, 0, 0.5, 0, 0, 0.5, 0.5, 0, 0, 0, 1);
  const shFocus = new THREE.Vector3(), shUp = new THREE.Vector3();
  let shadowOn = false;
  function renderShadow(cam, sunDir, active) {
    if (!shRT) { SH.uShP.value[0] = 0; return; }
    if (!active || api.shadowOff) { SH.uShP.value[0] = 0; return; }
    // the box: centred a little ahead of the camera, its texels snapped in light space (no shimmer as you move)
    _v.set(0, 0, -1).applyQuaternion(cam.quaternion);
    shFocus.copy(cam.position).addScaledVector(_v, T.shBox * 0.35);
    const L = _v2.set(sunDir[0], sunDir[1], sunDir[2]).normalize();
    shUp.copy(Math.abs(L.y) < 0.95 ? _v3.set(0, 1, 0) : _v3.set(1, 0, 0));
    shCam.position.copy(shFocus).addScaledVector(L, 3000); shCam.up.copy(shUp); shCam.lookAt(shFocus); shCam.updateMatrixWorld(true);
    const texel = (2 * T.shBox) / T.shadow;
    _v.copy(shFocus).applyMatrix4(shCam.matrixWorldInverse);
    const sx = Math.round(_v.x / texel) * texel - _v.x, sy = Math.round(_v.y / texel) * texel - _v.y;
    shCam.left = -T.shBox + sx; shCam.right = T.shBox + sx; shCam.top = T.shBox + sy; shCam.bottom = -T.shBox + sy; shCam.updateProjectionMatrix();
    shU.uMainCam.value.copy(cam.position);
    const prevRT = renderer.getRenderTarget(), prevAuto = renderer.autoClear, prevOv = scene.overrideMaterial, prevBg = scene.background;
    scene.overrideMaterial = depthMat; scene.background = null; renderer.autoClear = false;
    renderer.setRenderTarget(shRT); renderer.clear(true, true, false);
    for (const k in floraMeshes) { const fm = floraMeshes[k]; fm.full = fm.im.count; fm.im.count = Math.min(fm.im.count, fm.shN || 0); }
    renderer.render(scene, shCam);
    for (const k in floraMeshes) { const fm = floraMeshes[k]; fm.im.count = fm.full; }
    scene.overrideMaterial = prevOv; scene.background = prevBg; renderer.setRenderTarget(prevRT); renderer.autoClear = prevAuto;
    shMat.multiplyMatrices(shCam.projectionMatrix, shCam.matrixWorldInverse).premultiply(shBias);
    SH.uShMat.value = shMat.elements; SH.uShP.value[0] = 0.92; SH.uShP.value[1] = 1 / T.shadow; SH.uShP.value[2] = texel * 1.4;
    shadowOn = true;
  }

  // ---- ground cover: one instanced tuft per terrain quad of the finest near chunks -------------------------------------
  let grassGeo = null;
  const GU_ = { uGH: { value: 0.6 }, uGW: { value: 0.07 }, uDens: { value: 0.8 }, uSide: { value: SIDE }, uKind: { value: 0 }, uRange: { value: T.grass },
    uGA: { value: new THREE.Vector3() }, uGB: { value: new THREE.Vector3() }, uGT: { value: new THREE.Vector3() },
    uNearAir: { value: new THREE.Vector4() }, uNearIns: { value: new THREE.Vector3() } };
  const grassMat = T.grass ? new THREE.ShaderMaterial({ vertexShader: GRASS_VERT, fragmentShader: GRASS_FRAG, uniforms: { ...AU, ...SU, ...SH, ...GU_, uTime: SU.uTime, uWind: floraU.uWind, uWindDir: floraU.uWindDir, uGlow: floraU.uGlow, uSea: MU.uSea }, side: THREE.DoubleSide }) : null;
  function bladeGeometry(n) {
    // a tuft: n blades of 5 vertices (two tapering sections and a tip), position = (across, up, blade index)
    const p = [], idx = [];
    for (let b = 0; b < n; b++) {
      const o = b * 5; p.push(-1, 0, b, 1, 0, b, -0.6, 0.5, b, 0.6, 0.5, b, 0, 1, b);
      idx.push(o, o + 1, o + 2, o + 1, o + 3, o + 2, o + 2, o + 3, o + 4);
    }
    const g = new THREE.InstancedBufferGeometry(); g.setAttribute('position', new THREE.Float32BufferAttribute(p, 3)); g.setIndex(idx); return g;
  }
  // the slot's vertex arrays as per-instance data (one set of GL buffers shared by the ground cover and the rain rings)
  const fieldOf = (s) => {
    if (!s.field) {
      const a = s.geo.attributes;
      s.field = { ib: new THREE.InstancedInterleavedBuffer(a.position.array, 3, 1), nb: new THREE.InstancedInterleavedBuffer(a.normal.array, 3, 1),
        sb: new THREE.InstancedInterleavedBuffer(a.aSrf.array, 4, 1), xb: new THREE.InstancedInterleavedBuffer(a.aEx.array, 4, 1), stamp: -1 };
    }
    const f = s.field;
    if (f.stamp !== s.stamp) {   // the slot holds a new chunk: point the instances at its arrays
      const a = s.geo.attributes;
      f.ib.array = a.position.array; f.nb.array = a.normal.array; f.sb.array = a.aSrf.array; f.xb.array = a.aEx.array;
      f.ib.needsUpdate = f.nb.needsUpdate = f.sb.needsUpdate = f.xb.needsUpdate = true; f.stamp = s.stamp;
    }
    return f;
  };
  const fieldMesh = (s, geo0, mat, order) => {
    const f = fieldOf(s), g = new THREE.InstancedBufferGeometry(), V = SIDE;
    g.setAttribute('position', geo0.attributes.position); g.setIndex(geo0.index);
    g.setAttribute('iP0', new THREE.InterleavedBufferAttribute(f.ib, 3, 0)); g.setAttribute('iP1', new THREE.InterleavedBufferAttribute(f.ib, 3, 3));
    g.setAttribute('iP2', new THREE.InterleavedBufferAttribute(f.ib, 3, 3 * V)); g.setAttribute('iP3', new THREE.InterleavedBufferAttribute(f.ib, 3, 3 * V + 3));
    g.setAttribute('iN', new THREE.InterleavedBufferAttribute(f.nb, 3, 0)); g.setAttribute('iS', new THREE.InterleavedBufferAttribute(f.sb, 4, 0));
    g.setAttribute('iX', new THREE.InterleavedBufferAttribute(f.xb, 4, 0, true));
    g.instanceCount = V * (V - 1); g.boundingSphere = new THREE.Sphere(new THREE.Vector3(), 1e5);
    const m = new THREE.Mesh(g, mat); m.frustumCulled = false; m.visible = false; m.renderOrder = order; s.mesh.add(m);
    return { m };
  };
  const grassOf = (s) => { if (!s.grass) s.grass = fieldMesh(s, grassGeo, grassMat, 1); fieldOf(s); return s.grass; };
  const rainOf = (s) => { if (!s.rain) s.rain = fieldMesh(s, rainGeo, rainMat, 7); fieldOf(s); return s.rain; };
  const RU = { uRain: { value: 0 }, uRange: { value: 38 }, uRainC: { value: new THREE.Vector3(1, 1, 1) }, uSide: GU_.uSide };
  const rainMat = new THREE.ShaderMaterial({ vertexShader: RAIN_VERT, fragmentShader: RAIN_FRAG, uniforms: { ...AU, uTime: SU.uTime, ...RU }, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending });
  const rainGeo = (() => { const p = [], idx = []; for (let k = 0; k < 2; k++) { const o = k * 4; p.push(-1, -1, k, 1, -1, k, 1, 1, k, -1, 1, k); idx.push(o, o + 1, o + 2, o, o + 2, o + 3); } const g = new THREE.InstancedBufferGeometry(); g.setAttribute('position', new THREE.Float32BufferAttribute(p, 3)); g.setIndex(idx); return g; })();
  let rainOn = false;
  const grassObj = { visible: true };

  // ---- the chunk pool ------------------------------------------------------------------------------------------------
  const index = new THREE.BufferAttribute(chunkIndex(), 1);
  const slots = [], spare = [];
  function newSlot() {
    const b = newChunkBufs(), geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(b.pos, 3)); geo.setAttribute('normal', new THREE.BufferAttribute(b.nrm, 3));
    geo.setAttribute('aMor', new THREE.BufferAttribute(b.mor, 4)); geo.setAttribute('aSrf', new THREE.BufferAttribute(b.srf, 4)); geo.setAttribute('aEx', new THREE.BufferAttribute(b.ex, 4, true)); geo.setIndex(index);
    geo.boundingSphere = new THREE.Sphere(new THREE.Vector3(), 1);
    const mesh = new THREE.Mesh(geo, terrainMat); mesh.visible = false; mesh.matrixAutoUpdate = true; mesh.layers.enable(SHADOW_LAYER); group.add(mesh);
    const s = { geo, mesh, node: null, seen: 0, grass: null, stamp: 0 }; slots.push(s); return s;
  }
  for (let i = 0; i < 6; i++) spare.push(newChunkBufs());

  // ---- workers --------------------------------------------------------------------------------------------------------
  const workers = [];
  let reqId = 1;
  const pending = new Map();   // id -> { node, w } | { cell, w }
  function startWorkers() {
    if (workers.length) return;
    for (let i = 0; i < T.workers; i++) {
      let w; try { w = new Worker('/apps/games/games/stelle/terrain-worker.js', { type: 'module' }); } catch (e) { console.warn('[world] no module worker', e); break; }
      const W0 = { w, busy: 0 };
      w.onmessage = (ev) => onResult(W0, ev.data);
      w.onerror = (e) => console.warn('[world] worker error', e.message || e);
      workers.push(W0);
    }
  }

  // ---- state for the current world -----------------------------------------------------------------------------------
  let S = null, P = null, lmax = 8, frameNo = 0, faceSize = 1, gas = false;
  const nodes = new Map(), visible = [], wantReq = [];
  const camL = new THREE.Vector3();
  let floraCells = new Map(), floraDirty = false, floraKinds = [], floraMeshes = {}, sites = [];
  const stats = { chunks: 0, visible: 0, inflight: 0, built: 0, flora: 0, grass: 0, imp: 0, ms: 0 };
  const key = (f, L, x, y) => ((L * 8 + f) * 16384 + x) * 16384 + y;

  function set(S_, body) {
    clear();
    if (!S_) return;
    S = S_; P = palette(S); startWorkers();
    if (!mats) { mats = makeMaterials(THREE, renderer, T.mat, (tex) => { TU.uMat.value = tex; }); TU.uMat.value = mats.tex; }
    lmax = maxLevel(S, T.spacing); faceSize = nodeSize(S, 0);
    for (const w of workers) w.w.postMessage({ op: 'init', S });
    // material uniforms
    const sd = S.seeds; MU.uSeedA.value.set(sd[0], sd[1], sd[2], sd[3]); MU.uF.value = S.F; MU.uMntK.value = S.mntK; MU.uSea.value = S.sea; MU.uType.value = S.ti;
    S.ramp.forEach((c, i) => MU['r' + i].value.set(c[0], c[1], c[2])); MU.uOcean.value.set(S.ocean[0], S.ocean[1], S.ocean[2]);
    TU.uLod.value.set(T.K, faceSize); TU.uSnow.value = P.snow; TU.uA.value = S.A;
    P.slots.forEach((s, i) => { TU.uMS.value[i].set(s.layer, s.tile, s.nk, s.spec); TU.uMA.value[i].set(...s.a); TU.uMB.value[i].set(...s.b); });
    TU.uStrata.value.set(...P.strata);
    TU.uGlow.value.set(...P.glow); TU.uWater.value.set(...P.water); TU.uDeep.value.set(...P.deep);
    floraU.uGlow.value.set(...(S.type === 'jungle' ? [0.5, 2.2, 1.8] : S.type === 'crystal' ? [0.6, 1.2, 2.4] : S.type === 'ocean' ? [0.5, 1.4, 2.4] : S.type === 'volcanic' ? [2.6, 0.9, 0.2] : [1.2, 1.2, 1.2]));
    floraU.uCap.value.set(P.cap[0], P.cap[1], 0, 0); floraU.uCapC.value.set(...P.capC);
    // the sea: wind-driven swell (water worlds); calm lakes on the crystal worlds, none on lava or ice
    const water = S.sea > 0 && (S.liquid === 1 || S.liquid === 4), wa = Math.atan2(S.weather.wind[1], S.weather.wind[0]);
    const sea = water ? (S.liquid === 4 ? 0.25 : 0.55 + S.weather.k * 0.9) : 0;
    [[0, 34, 0.32], [0.55, 21, 0.2], [-0.7, 12.5, 0.12], [1.3, 6.8, 0.06]].forEach(([da, l, a], i) => WU.uWD.value[i].set(Math.cos(wa + da), Math.sin(wa + da), l, a));
    WU.uWave.value.set(sea, 0.9, 0, 0); waveFrame = null;
    if (P.grass && grassMat) {
      GU_.uGH.value = P.grass.h; GU_.uGW.value = P.grass.w; GU_.uDens.value = P.grass.dens; GU_.uKind.value = P.grass.kind;
      GU_.uGA.value.set(...P.grass.a); GU_.uGB.value.set(...P.grass.b); GU_.uGT.value.set(...P.grass.t);
      if (!grassGeo) grassGeo = bladeGeometry(T.blades);
    }
    SU.uR.value = S.R; SU.uCloud.value = body && body.aux ? body.aux : blank; SU.uCloudAlt.value = S.cloud.alt; SU.uCloudK.value = S.cloud.cover;
    gas = S.type === 'gas';
    clouds.scale.setScalar(S.R + S.cloud.alt); group.add(clouds); clouds.visible = !gas && S.cloud.cover > 0.05;
    veil.scale.setScalar(S.R + Math.min(S.atmo.top * 0.85, S.cloud.alt * 2.3)); group.add(veil); veil.visible = T.cloud2 && !gas;
    puffU.uDeckR.value = S.R + S.cloud.alt; puffU.uThick.value = S.cloud.thick;
    if (gas) {   // the decks of a gas giant; its puffs live on the middle deck, everywhere
      decks.forEach((d, i) => { d.scale.setScalar(S.R + GAS_DECK[i]); d.material.uniforms.uSurf.value = body && body.surf ? body.surf : blank; group.add(d); d.visible = true; });
      SU.uCloud.value = white; SU.uCloudK.value = 0.62; SU.uCloudAlt.value = GAS_DECK[1]; puffU.uDeckR.value = S.R + GAS_DECK[1]; puffU.uThick.value = 420;
    }
    BOLT.next = 4 + Math.random() * 8; BOLT.k = 0; SU.uBolt.value.w = 0;
    // flora kinds of this world
    floraKinds = S.flora.map((f) => f.kind);
    for (const k of floraKinds) {
      if (!floraGeo[k]) floraGeo[k] = floraGeometry(THREE, k);
      const cap = Math.round(FLORA_BIG.has(k) ? 1400 : 5200 * T.flora + 400);
      const im = new THREE.InstancedMesh(floraGeo[k], FLORA_BIG.has(k) ? floraMatBig : floraMat, cap); im.count = 0; im.frustumCulled = false;
      im.instanceColor = new THREE.InstancedBufferAttribute(new Float32Array(cap * 3), 3);
      im.instanceMatrix.setUsage(THREE.DynamicDrawUsage); im.instanceColor.setUsage(THREE.DynamicDrawUsage);
      im.layers.enable(SHADOW_LAYER);
      group.add(im); floraMeshes[k] = { im, cap, tint: floraTint(S, k), big: FLORA_BIG.has(k), lo: null, capLo: 0 };
      if (FLORA_BIG.has(k)) {   // the far LOD: a second instanced mesh with the light version of the kind
        if (!floraGeoLo[k]) floraGeoLo[k] = floraGeometry(THREE, k, true);
        const capLo = Math.round(3200 * T.flora + 600), lo = new THREE.InstancedMesh(floraGeoLo[k], floraMatBig, capLo); lo.count = 0; lo.frustumCulled = false;
        lo.instanceColor = new THREE.InstancedBufferAttribute(new Float32Array(capLo * 3), 3);
        lo.instanceMatrix.setUsage(THREE.DynamicDrawUsage); lo.instanceColor.setUsage(THREE.DynamicDrawUsage);
        group.add(lo); floraMeshes[k].lo = lo; floraMeshes[k].capLo = capLo;
      }
    }
    if (impMat) { impInit(); bakeImpostors(); if (impKinds.length) group.add(impMesh); }
    // sites
    for (const st of S.sites) {
      const sg = siteGeometry(THREE, st.kind, st.seed, st.fac);
      const g = new THREE.Group();
      const d = st.dir, r = S.R + st.e;
      g.position.set(d[0] * r, d[1] * r, d[2] * r);
      _q.setFromUnitVectors(_v.set(0, 1, 0), _v2.set(d[0], d[1], d[2])); g.quaternion.copy(_q); g.rotateY(st.yaw);
      g.scale.setScalar(SITE_SCALE[st.kind] || 1);   // the Costellatori built big: their ruins stand over the ridges
      const m = new THREE.Mesh(sg.geo, siteMat); m.layers.enable(SHADOW_LAYER); g.add(m);
      let crystal = null;
      if (sg.crystal) { crystal = new THREE.Mesh(sg.crystal, siteMat); crystal.position.y = 6.5; crystal.layers.enable(SHADOW_LAYER); g.add(crystal); }
      if (st.kind === 'wreck') {
        const hull = new THREE.Mesh(shipGeometry(THREE, ['hauler', 'warden', 'hulk'][st.seed % 3], st.seed % 3 === 2 ? 2 : 0, false), wreckMat); hull.userData.shared = true; hull.layers.enable(SHADOW_LAYER);
        const sc = st.seed % 3 === 0 ? 0.6 : 0.32; hull.scale.setScalar(sc); hull.rotation.set(0.25 + (st.seed % 7) * 0.05, 0, 0.35 - (st.seed % 5) * 0.12); hull.position.y = 3; g.add(hull);
      }
      group.add(g);
      sites.push({ st, g, crystal, lights: sg.lights, h: sg.h, found: false });
    }
    // the world's programs are compiled before it shows (in parallel off the main thread where the browser can): the
    // group stays hidden — the orbital body stands in — until they are ready, so entering a world never freezes a frame
    group.visible = false; progReady = false; const myS = S;
    if (grassMat && grassGeo && !grassProxy) { grassProxy = new THREE.Mesh(grassGeo, grassMat); grassProxy.frustumCulled = false; rainProxy = new THREE.Mesh(rainGeo, rainMat); rainProxy.frustumCulled = false; }
    if (grassProxy) { grassProxy.visible = rainProxy.visible = false; group.add(grassProxy); group.add(rainProxy); }
    // stand-ins for what is not built yet (the chunks arrive from the workers; the shadow pass's two variants)
    if (!terrainProxy) {
      terrainProxy = new THREE.Mesh(slotGeoProxy(), terrainMat); terrainProxy.frustumCulled = false;
      if (depthMat) { depthProxy = new THREE.Mesh(terrainProxy.geometry, depthMat); depthProxy.frustumCulled = false; depthIProxy = new THREE.InstancedMesh(terrainProxy.geometry, depthMat, 1); depthIProxy.frustumCulled = false; }
    }
    for (const o of [terrainProxy, depthProxy, depthIProxy]) if (o) { o.visible = false; group.add(o); }
    const done = () => { if (S !== myS) return; progReady = true; group.visible = true; for (const o of [grassProxy, rainProxy, terrainProxy, depthProxy, depthIProxy]) if (o) o.removeFromParent(); };
    // compiled for the target the near pass draws into (the post chain's: no tone mapping in the shader), the sky table too
    try {
      const prev = renderer.getRenderTarget();
      renderer.setRenderTarget(api.compileTarget ? api.compileTarget() : null);
      const p1 = renderer.compileAsync(group, compileCam, scene);
      renderer.setRenderTarget(skyRT); const p2 = renderer.compileAsync(lutScene, compileCam);
      renderer.setRenderTarget(prev);
      Promise.all([p1, p2]).then(done, done);
    } catch (e) { done(); }
    if (window.__cz) window.__cz.world = api;
  }
  function clear() {
    for (const s of slots) { s.mesh.visible = false; s.node = null; if (s.grass) s.grass.m.visible = false; if (s.rain) s.rain.m.visible = false; }
    nodes.clear(); visible.length = 0; wantReq.length = 0;
    for (const [id, p] of pending) { if (p.cell) p.dead = true; else p.dead = true; }
    for (const k in floraMeshes) { const fm = floraMeshes[k]; fm.im.removeFromParent(); fm.im.dispose(); if (fm.lo) { fm.lo.removeFromParent(); fm.lo.dispose(); } }
    floraMeshes = {}; floraCells = new Map(); floraKinds = [];
    for (const c of farCells.values()) if (c.buf) floraBufs.push(c.buf); farCells = new Map(); impKinds = []; if (impMesh) { impMesh.removeFromParent(); impGeo.instanceCount = 0; }
    for (const s of sites) { s.g.removeFromParent(); s.g.traverse((o) => { if (o.isMesh && o.geometry && !o.userData.shared) o.geometry.dispose(); }); }
    sites = [];
    if (S) for (const w of workers) w.w.postMessage({ op: 'drop', key: S.key });
    clouds.removeFromParent(); clouds.visible = false; veil.removeFromParent(); veil.visible = false; sky.visible = false; puffs.visible = false; wLines.visible = wPts.visible = false; bolt.visible = false;
    for (const d of decks) { d.removeFromParent(); d.visible = false; } gas = false;
    SH.uShP.value[0] = 0; SU.uBolt.value.w = 0; AUR.uAur.value.x = 0; SU.uFog.value.x = 0;
    group.visible = false; S = null;
  }

  // ---- quadtree ------------------------------------------------------------------------------------------------------
  const D = [0, 0, 0];
  function getNode(f, L, x, y) {
    const k = key(f, L, x, y); let n = nodes.get(k);
    if (!n) {
      const nn = 1 << L; cubeDir(f, -1 + 2 * (x + 0.5) / nn, -1 + 2 * (y + 0.5) / nn, D);
      const size = faceSize / nn;
      n = { k, f, L, x, y, cx: D[0] * S.R, cy: D[1] * S.R, cz: D[2] * S.R, rad: size * 0.78 + S.A + S.depth * (S.sea > 0 ? 0 : 0) + 20, emax: S.A, emin: -S.depth, size, slot: null, req: false, seen: 0, ready: false };
      nodes.set(k, n);
    }
    return n;
  }
  let horizonR = 0, camD = 0;
  function culled(n) {
    // horizon: hidden behind the curve of the world (a sphere of the lowest ground)
    const dx = n.cx - camL.x, dy = n.cy - camL.y, dz = n.cz - camL.z, d = Math.sqrt(dx * dx + dy * dy + dz * dz) - n.rad;
    if (camD > horizonR) {
      const Rm = horizonR, Rx = S.R + Math.max(0, n.emax);
      const lim = Math.sqrt(camD * camD - Rm * Rm) + Math.sqrt(Math.max(0, Rx * Rx - Rm * Rm));
      if (d > lim) return true;
    }
    return false;
  }
  function distOf(n) { const dx = n.cx - camL.x, dy = n.cy - camL.y, dz = n.cz - camL.z; return Math.sqrt(dx * dx + dy * dy + dz * dz) - n.rad; }
  // the split distance: what lies outside the view refines as if it were twice as far (coarser, never missing: a turn
  // of the head is met by the workers in a few frames)
  function lodDist(n) {
    const dx = n.cx - camL.x, dy = n.cy - camL.y, dz = n.cz - camL.z, l = Math.sqrt(dx * dx + dy * dy + dz * dz), d = l - n.rad;
    if (d < n.size * 0.5) return d;
    const c = (dx * camF.x + dy * camF.y + dz * camF.z) / l, lim = Math.cos(Math.min(1.45, camHalf + Math.asin(Math.min(1, n.rad / l))));
    return c >= lim ? d : d * (2.2 - 0.6 * Math.max(0, c + 0.2));
  }
  const camF = new THREE.Vector3(0, 0, -1); let camHalf = 1;
  function visit(n) {
    n.seen = frameNo; if (n.slot) n.slot.seen = frameNo;   // ancestors stay cached: a merge back must not rebuild them
    if (culled(n)) return true;   // nothing to draw: counts as ready
    const d = distOf(n);
    const split = n.L < lmax && lodDist(n) < T.K * n.size;
    if (split) {
      const L1 = n.L + 1, x2 = n.x * 2, y2 = n.y * 2;
      const c0 = getNode(n.f, L1, x2, y2), c1 = getNode(n.f, L1, x2 + 1, y2), c2 = getNode(n.f, L1, x2, y2 + 1), c3 = getNode(n.f, L1, x2 + 1, y2 + 1);
      const ok = (c) => c.ready || culled(c);
      // drawn by its children: the slot stays cached for a merge back, but it may be recycled before anything shown
      if (ok(c0) && ok(c1) && ok(c2) && ok(c3)) { if (n.slot) n.slot.seen = frameNo - 1; visit(c0); visit(c1); visit(c2); visit(c3); return true; }
      for (const c of [c0, c1, c2, c3]) { c.seen = frameNo; if (c.slot) c.slot.seen = frameNo; if (!c.ready && !c.req && !culled(c)) wantReq.push(c); }
    }
    if (n.ready) { show(n, d); return true; }
    if (!n.req) wantReq.push(n);
    return false;
  }
  function show(n, d) {
    const s = n.slot; s.seen = frameNo; s.mesh.visible = true; visible.push(s.mesh);
    // the finest chunks near the camera grow their ground cover
    if (grassMat && grassGeo && grassObj.visible && n.L >= lmax - 1 && d < T.grass && P && P.grass) { const g = grassOf(s); g.m.visible = true; grassShown.push(g.m); }
    if (rainOn && n.L >= lmax - 1 && d < RU.uRange.value) { const r = rainOf(s); r.m.visible = true; grassShown.push(r.m); }
  }
  const grassShown = [];
  function request() {
    if (!workers.length) return;
    wantReq.sort((a, b) => (a.L - b.L) * 1e6 + (distOf(a) / a.size - distOf(b) / b.size));
    let inflight = 0; for (const w of workers) inflight += w.busy;
    for (const n of wantReq) {
      if (inflight >= T.inflight * workers.length) break;
      if (n.req || n.ready) continue;
      const bufs = spare.pop() || newChunkBufs();
      const w = workers.reduce((a, b) => (b.busy < a.busy ? b : a));
      const id = reqId++; pending.set(id, { node: n, w });
      n.req = true; w.busy++; inflight++;
      w.w.postMessage({ op: 'chunk', id, key: S.key, f: n.f, L: n.L, x: n.x, y: n.y, bufs }, [bufs.pos.buffer, bufs.nrm.buffer, bufs.mor.buffer, bufs.srf.buffer, bufs.ex.buffer]);
    }
    wantReq.length = 0;
    stats.inflight = inflight;
  }
  function freeSlot() {
    let best = null;
    for (const s of slots) { if (!s.node) return s; if (s.seen !== frameNo && (!best || s.seen < best.seen)) best = s; }
    if (slots.length < T.pool) return newSlot();
    if (best) { best.node.ready = false; best.node.req = false; best.node.slot = null; best.node = null; best.mesh.visible = false; }
    return best;
  }
  function onResult(W0, m) {
    W0.busy = Math.max(0, W0.busy - 1);
    const p = pending.get(m.id); pending.delete(m.id);
    if (m.op === 'flora') return onFlora(m, p);
    if (!p || p.dead || !m.ok || !S || nodes.get(p.node.k) !== p.node) { spare.push(m.bufs); return; }
    const n = p.node, s = freeSlot();
    if (!s) { spare.push(m.bufs); n.req = false; return; }
    const a = s.geo.attributes;
    spare.push({ pos: a.position.array, nrm: a.normal.array, mor: a.aMor.array, srf: a.aSrf.array, ex: a.aEx.array });
    a.position.array = m.bufs.pos; a.normal.array = m.bufs.nrm; a.aMor.array = m.bufs.mor; a.aSrf.array = m.bufs.srf; a.aEx.array = m.bufs.ex;
    a.position.needsUpdate = a.normal.needsUpdate = a.aMor.needsUpdate = a.aSrf.needsUpdate = a.aEx.needsUpdate = true;
    const I = m.info; s.mesh.position.set(I.cx, I.cy, I.cz); s.geo.boundingSphere.radius = I.rad;
    n.cx = I.cx; n.cy = I.cy; n.cz = I.cz; n.rad = I.rad; n.emax = I.emax; n.emin = I.emin;
    s.node = n; n.slot = s; n.ready = true; n.req = false; s.seen = frameNo; s.stamp++;
    stats.built++;
  }

  // ---- flora cells: a fixed fine level of the quadtree near the camera ------------------------------------------------
  let floraL = 10;
  const floraWant = [], floraBufs = [];
  function floraPass() {
    if (!floraKinds.length) return;
    floraL = Math.max(0, Math.min(lmax, Math.round(Math.log2(faceSize / 300))));
    const range = T.floraR * 2.2;
    // walk the tree down to floraL within range
    const walk = (f, L, x, y) => {
      const nn = 1 << L; cubeDir(f, -1 + 2 * (x + 0.5) / nn, -1 + 2 * (y + 0.5) / nn, D);
      const size = faceSize / nn, cx = D[0] * S.R, cy = D[1] * S.R, cz = D[2] * S.R;
      const d = Math.hypot(cx - camL.x, cy - camL.y, cz - camL.z) - size * 0.75 - S.A;
      if (d > range) return;
      if (L < floraL) { walk(f, L + 1, x * 2, y * 2); walk(f, L + 1, x * 2 + 1, y * 2); walk(f, L + 1, x * 2, y * 2 + 1); walk(f, L + 1, x * 2 + 1, y * 2 + 1); return; }
      const k = key(f, L, x, y); let c = floraCells.get(k);
      if (!c) { c = { k, f, L, x, y, buf: null, n: 0, cx: 0, cy: 0, cz: 0, req: false, ready: false, seen: 0, d }; floraCells.set(k, c); }
      c.seen = frameNo; c.d = d;
      if (!c.ready && !c.req) floraWant.push(c);
    };
    for (let f = 0; f < 6; f++) walk(f, 0, 0, 0);
    // drop cells that left the range; request new ones (closest first)
    for (const [k, c] of floraCells) if (c.seen !== frameNo) { if (c.buf) floraBufs.push(c.buf); floraCells.delete(k); floraDirty = true; }
    floraWant.sort((a, b) => a.d - b.d);
    let budget = 2;
    for (const c of floraWant) {
      if (budget-- <= 0 || !workers.length) break;
      const buf = floraBufs.pop() || new Float32Array(1600 * FLORA_STRIDE);
      const w = workers.reduce((a, b) => (b.busy < a.busy ? b : a)); w.busy++;
      const id = reqId++; pending.set(id, { cell: c, w }); c.req = true;
      w.w.postMessage({ op: 'flora', id, key: S.key, f: c.f, L: c.L, x: c.x, y: c.y, density: T.flora, buf }, [buf.buffer]);
    }
    floraWant.length = 0;
  }
  function onFlora(m, p) {
    if (p && p.far) {
      if (!S || !m.ok || farCells.get(p.cell.k) !== p.cell) { floraBufs.push(m.buf); return; }
      const c = p.cell; c.buf = m.buf; c.n = m.n; c.cx = m.cx; c.cy = m.cy; c.cz = m.cz; c.ready = true; c.req = false; impDirty = true; return;
    }
    if (!p || !S || !m.ok || floraCells.get(p.cell.k) !== p.cell) { floraBufs.push(m.buf); return; }
    const c = p.cell; c.buf = m.buf; c.n = m.n; c.cx = m.cx; c.cy = m.cy; c.cz = m.cz; c.ready = true; c.req = false; floraDirty = true;
  }
  const _mtx = new THREE.Matrix4(), _pos = new THREE.Vector3(), _sc = new THREE.Vector3(), _up = new THREE.Vector3(), _qq = new THREE.Quaternion(), _qy = new THREE.Quaternion(), _col = new THREE.Color(), _Y = new THREE.Vector3(0, 1, 0);
  let floraT = 0, floraBand = 0, rebuildT = 0;
  const floraSorted = [];
  function rebuildFlora() {
    floraDirty = false;
    const counts = {}, countsLo = {}; for (const k of floraKinds) { counts[k] = countsLo[k] = 0; floraMeshes[k].shN = 0; }
    const shR = T.shBox * 1.9;
    let total = 0;
    // nearest cells first (the instance caps fill from the camera out); beyond the near range only the big kinds,
    // thinned with distance — the skyline keeps its silhouettes, the caps hold
    floraSorted.length = 0; for (const c of floraCells.values()) if (c.ready) floraSorted.push(c);
    floraSorted.sort((a, b) => a.d - b.d);
    for (const c of floraSorted) {
      const near = c.d < T.floraR, thin = near ? 1 : c.d < T.floraR * 1.6 ? 2 : 4, far = c.d > FLORA_LOD_D;
      for (let i = 0; i < c.n; i++) {
        const o = i * FLORA_STRIDE, kind = FLORA_KINDS[c.buf[o + 5]], fm = floraMeshes[kind]; if (!fm) continue;
        if (!near && (!fm.big || i % thin)) continue;
        if (!fm.big && c.d > T.floraR * T.smallK) continue;
        const useLo = far && fm.lo, ci = useLo ? countsLo[kind] : counts[kind]; if (ci >= (useLo ? fm.capLo : fm.cap)) continue;
        const im = useLo ? fm.lo : fm.im;
        const x = c.buf[o] + c.cx, y = c.buf[o + 1] + c.cy, z = c.buf[o + 2] + c.cz, s = c.buf[o + 3];
        _pos.set(x, y, z); _up.copy(_pos).normalize();
        _qq.setFromUnitVectors(_Y, _up); _qy.setFromAxisAngle(_Y, c.buf[o + 4]); _qq.multiply(_qy);
        if (c.buf[o + 7]) { _qy.setFromAxisAngle(_v.set(1, 0, 0), c.buf[o + 7]); _qq.multiply(_qy); }
        _sc.setScalar(s); _mtx.compose(_pos, _qq, _sc); im.setMatrixAt(ci, _mtx);
        // size and hue vary per plant: a warmer or cooler shade of the kind's colour
        const tr = c.buf[o + 6], t = 0.8 + tr * 0.4, hs = (tr * 7.31) % 1 - 0.5;
        _col.setRGB(fm.tint[0] * t * (1 + hs * 0.18), fm.tint[1] * t, fm.tint[2] * t * (1 - hs * 0.18)); im.setColorAt(ci, _col);
        if (useLo) countsLo[kind] = ci + 1; else { counts[kind] = ci + 1; if (c.d < shR) fm.shN = ci + 1; } total++;
      }
    }
    for (const k of floraKinds) {
      const fm = floraMeshes[k]; fm.im.count = counts[k]; if (counts[k]) { fm.im.instanceMatrix.needsUpdate = true; fm.im.instanceColor.needsUpdate = true; }
      if (fm.lo) { fm.lo.count = countsLo[k]; if (countsLo[k]) { fm.lo.instanceMatrix.needsUpdate = true; fm.lo.instanceColor.needsUpdate = true; } }
    }
    stats.flora = total;
  }

  // ---- the far ring: impostor cards from a coarser grid of cells, big kinds only ---------------------------------------
  const IMP_TILE = 128, IMP_MAX = 8;
  let impRT = null, impMesh = null, impGeo = null, impA = null, impB = null, impKinds = [], impDirty = false;
  const impMat = T.imp ? new THREE.ShaderMaterial({ vertexShader: IMP_VERT, fragmentShader: IMP_FRAG, uniforms: { ...AU, ...SU, uImp: { value: null }, uTiles: { value: IMP_MAX }, uNear: { value: T.floraR * 2.05 }, uFar: { value: T.imp } } }) : null;
  const bakeMat = new THREE.ShaderMaterial({ vertexShader: IMPBAKE_VERT, fragmentShader: IMPBAKE_FRAG, vertexColors: true, side: THREE.DoubleSide, uniforms: { uTint: { value: new THREE.Vector3(1, 1, 1) }, uGlowC: { value: new THREE.Vector3(1, 1, 1) } } });
  function bakeImpostors() {
    if (!impMat) return;
    impKinds = floraKinds.filter((k) => FLORA_BIG.has(k)).slice(0, IMP_MAX);
    if (!impKinds.length) return;
    if (!impRT) { impRT = new THREE.WebGLRenderTarget(IMP_TILE * IMP_MAX, IMP_TILE, { depthBuffer: true, generateMipmaps: true, minFilter: THREE.LinearMipmapLinearFilter, magFilter: THREE.LinearFilter }); impMat.uniforms.uImp.value = impRT.texture; }
    const scene0 = new THREE.Scene(), cam0 = new THREE.OrthographicCamera(-1, 1, 1, -1, 0.1, 20);
    const prev = renderer.getRenderTarget(), auto = renderer.autoClear, cc = renderer.getClearColor(new THREE.Color()), ca = renderer.getClearAlpha();
    renderer.setRenderTarget(impRT); renderer.setClearColor(0x000000, 0); renderer.clear(true, true, false); renderer.autoClear = false;
    impKinds.forEach((k, i) => {
      const g = floraGeoLo[k] || floraGeo[k]; if (!g.boundingBox) g.computeBoundingBox();
      const b = g.boundingBox, hw = Math.max(Math.abs(b.min.x), Math.abs(b.max.x), Math.abs(b.min.z), Math.abs(b.max.z), 0.05), h = Math.max(0.05, b.max.y);
      const tint = floraTint(S, k); bakeMat.uniforms.uTint.value.set(tint[0], tint[1], tint[2]); bakeMat.uniforms.uGlowC.value.copy(floraU.uGlow.value);
      const m = new THREE.Mesh(g, bakeMat); scene0.add(m);
      cam0.left = -hw; cam0.right = hw; cam0.top = h; cam0.bottom = 0; cam0.position.set(0, 0, 10); cam0.lookAt(0, 0, 0); cam0.updateProjectionMatrix();
      impRT.viewport.set(i * IMP_TILE, 0, IMP_TILE, IMP_TILE); impRT.scissor.set(i * IMP_TILE, 0, IMP_TILE, IMP_TILE); impRT.scissorTest = true;
      renderer.setRenderTarget(impRT); renderer.render(scene0, cam0); scene0.remove(m);
      floraMeshes[k].impW = (2 * hw) / h; floraMeshes[k].impH = h; floraMeshes[k].impTile = i;
    });
    impRT.scissorTest = false; impRT.viewport.set(0, 0, IMP_TILE * IMP_MAX, IMP_TILE);
    renderer.setRenderTarget(prev); renderer.setClearColor(cc, ca); renderer.autoClear = auto;
    renderer.initTexture && renderer.initTexture(impRT.texture);
  }
  function impInit() {
    if (!impMat || impMesh) return;
    impGeo = new THREE.InstancedBufferGeometry();
    impGeo.setAttribute('position', new THREE.Float32BufferAttribute([-0.5, 0, 0, 0.5, 0, 0, 0.5, 1, 0, -0.5, 1, 0], 3)); impGeo.setIndex([0, 1, 2, 0, 2, 3]);
    impA = new THREE.InstancedBufferAttribute(new Float32Array(T.impCap * 4), 4).setUsage(THREE.DynamicDrawUsage); impB = new THREE.InstancedBufferAttribute(new Float32Array(T.impCap * 4), 4).setUsage(THREE.DynamicDrawUsage);
    impGeo.setAttribute('aI', impA); impGeo.setAttribute('aJ', impB); impGeo.instanceCount = 0;
    impMesh = new THREE.Mesh(impGeo, impMat); impMesh.frustumCulled = false; impMesh.renderOrder = 2;
  }
  let farCells = new Map();
  const farWant = [];
  function farPass() {
    if (!impMat || !impKinds.length) return;
    const farL = Math.max(0, floraL - 2), range = T.imp, inner = T.floraR * 1.9;
    const walk = (f, L, x, y) => {
      const nn = 1 << L; cubeDir(f, -1 + 2 * (x + 0.5) / nn, -1 + 2 * (y + 0.5) / nn, D);
      const size = faceSize / nn, cx = D[0] * S.R, cy = D[1] * S.R, cz = D[2] * S.R;
      const dc = Math.hypot(cx - camL.x, cy - camL.y, cz - camL.z), d = dc - size * 0.75 - S.A;
      if (d > range) return;
      if (L < farL) { walk(f, L + 1, x * 2, y * 2); walk(f, L + 1, x * 2 + 1, y * 2); walk(f, L + 1, x * 2, y * 2 + 1); walk(f, L + 1, x * 2 + 1, y * 2 + 1); return; }
      if (dc + size * 0.75 < inner) return;   // wholly inside the real flora's range
      const k = key(f, L, x, y); let c = farCells.get(k);
      if (!c) { c = { k, f, L, x, y, buf: null, n: 0, cx: 0, cy: 0, cz: 0, req: false, ready: false, seen: 0, d }; farCells.set(k, c); }
      c.seen = frameNo; c.d = d;
      if (!c.ready && !c.req) farWant.push(c);
    };
    for (let f = 0; f < 6; f++) walk(f, 0, 0, 0);
    for (const [k, c] of farCells) if (c.seen !== frameNo) { if (c.buf) floraBufs.push(c.buf); farCells.delete(k); impDirty = true; }
    farWant.sort((a, b) => a.d - b.d);
    const only = impKinds.map((k) => FLORA_KINDS.indexOf(k));
    let budget = 1;
    for (const c of farWant) {
      if (budget-- <= 0 || !workers.length) break;
      const buf = floraBufs.pop() || new Float32Array(1600 * FLORA_STRIDE);
      const w = workers.reduce((a, b) => (b.busy < a.busy ? b : a)); w.busy++;
      const id = reqId++; pending.set(id, { cell: c, far: true, w }); c.req = true;
      w.w.postMessage({ op: 'flora', id, key: S.key, f: c.f, L: c.L, x: c.x, y: c.y, density: T.flora * 0.3, only, buf }, [buf.buffer]);
    }
    farWant.length = 0;
  }
  function rebuildImp() {
    impDirty = false; if (!impMesh) return;
    let n = 0; const A = impA.array, B = impB.array;
    for (const c of farCells.values()) {
      if (!c.ready) continue;
      for (let i = 0; i < c.n && n < T.impCap; i++) {
        const o = i * FLORA_STRIDE, kind = FLORA_KINDS[c.buf[o + 5]], fm = floraMeshes[kind]; if (!fm || fm.impTile == null) continue;
        const s = c.buf[o + 3];
        A[n * 4] = c.buf[o] + c.cx; A[n * 4 + 1] = c.buf[o + 1] + c.cy; A[n * 4 + 2] = c.buf[o + 2] + c.cz; A[n * 4 + 3] = s * fm.impH;
        B[n * 4] = fm.impTile; B[n * 4 + 1] = fm.impW; B[n * 4 + 2] = c.buf[o + 6]; B[n * 4 + 3] = 0; n++;
      }
    }
    impGeo.instanceCount = n; impA.needsUpdate = impB.needsUpdate = true; stats.imp = n;
  }

  // ---- lightning: a strike rebuilt in the segment pool, the flash decays with flicker -----------------------------------
  function strike(cam, up) {
    if (!S) return;
    // somewhere ahead of the camera, 0.4 .. 6 km away, from the cloud base down to the ground
    _v.set((Math.random() - 0.5) * 2, 0, -1).applyQuaternion(cam.quaternion); _v.addScaledVector(up, -_v.dot(up)).normalize();
    const dist = 400 + Math.random() * Math.random() * 5600, top = gas ? GAS_DECK[2] : S.cloud.alt, alt = camD - S.R;
    const bx = cam.position.x + _v.x * dist, by = cam.position.y + _v.y * dist, bz = cam.position.z + _v.z * dist;
    let px = bx + up.x * (top - alt), py = by + up.y * (top - alt), pz = bz + up.z * (top - alt);
    const lowAlt = gas ? GAS_DECK[0] : 0, len = top - lowAlt, seg = BOLT_N, side = _v2.crossVectors(up, _v).normalize();
    for (let i = 0; i < seg; i++) {
      const k = len / seg, jx = (Math.random() - 0.5) * k * 1.3, jz = (Math.random() - 0.5) * k * 1.3;
      const nx = px - up.x * k + side.x * jx + _v.x * jz, ny = py - up.y * k + side.y * jx + _v.y * jz, nz = pz - up.z * k + side.z * jx + _v.z * jz;
      bPos.set([px, py, pz, nx, ny, nz], i * 6); px = nx; py = ny; pz = nz;
    }
    bGeo.attributes.position.needsUpdate = true;
    BOLT.k = 1; BOLT.t = 0; BOLT.dist = dist; BOLT.events++;
    api.lastStrike = dist;
  }

  // ---- the sky table: the local frame (up, toward the sun, across), the horizon's zenith angle, 192 x 108 texels -------
  function renderSkyLut(cam, sd) {
    const up = SKYV.uSkyUp.value.copy(cam.position).sub(group.position).normalize();
    const s1 = SKYV.uSkyS1.value.set(sd[0], sd[1], sd[2]); s1.addScaledVector(up, -s1.dot(up));
    if (s1.lengthSq() < 1e-8) s1.set(1, 0, 0).addScaledVector(up, -up.x); s1.normalize();
    SKYV.uSkyS2.value.crossVectors(up, s1);
    const r = Math.max(camD, S.R + 0.5), beta = Math.asin(Math.min(1, S.R / r));
    SKYV.uSkyHz.value.set(Math.PI - beta, beta);
    const prev = renderer.getRenderTarget(); renderer.setRenderTarget(skyRT); renderer.render(lutScene, cam); renderer.setRenderTarget(prev);
  }
  // ---- per frame -----------------------------------------------------------------------------------------------------
  // o: { center[3], quat[4], camPos (Vector3), sunDir[3], sunI[3], amb (atmo.ambientRes), T (time), fade (0..1), shadow: [x,y,z,r] | null,
  //      head: { on, pos, dir, k }, weather: k, wind }
  const _cam = new THREE.Vector3(), _tN = new THREE.Vector3();
  let waveFrame = null, waveBlend = 0;
  const wfA = { c: new THREE.Vector3(), t1: new THREE.Vector3(), t2: new THREE.Vector3() }, wfB = { c: new THREE.Vector3(), t1: new THREE.Vector3(), t2: new THREE.Vector3() };
  const anchor = (f, cl) => { f.c.copy(cl).normalize(); f.t1.set(0, 1, 0).cross(f.c); if (f.t1.lengthSq() < 1e-6) f.t1.set(1, 0, 0); f.t1.normalize(); f.t2.crossVectors(f.c, f.t1); };
  function update(dt, cam, o) {
    if (!S) return;
    const t0 = performance.now();
    frameNo++;
    group.position.set(o.center[0], o.center[1], o.center[2]); group.quaternion.set(o.quat[0], o.quat[1], o.quat[2], o.quat[3]);
    group.updateMatrixWorld(true);
    _q.copy(group.quaternion); pRot.setFromMatrix4(_m4.makeRotationFromQuaternion(_q)); _q.invert(); pInv.setFromMatrix4(_m4.makeRotationFromQuaternion(_q));
    SU.uAxis.value.set(0, 1, 0).applyQuaternion(group.quaternion);
    _cam.copy(cam.position);
    camL.copy(_cam).sub(group.position).applyQuaternion(_q);
    camD = camL.length(); horizonR = S.R + Math.min(0, S.sea > 0 ? 0 : -S.A * 0.1);
    camF.set(0, 0, -1).applyQuaternion(cam.quaternion).applyQuaternion(_q); camHalf = (cam.fov || 70) * Math.PI / 360 * Math.max(1, cam.aspect || 1.6) * 1.15;
    SU.uTime.value = o.T % 10000; SU.uCloudOff.value = (o.T * 0.000004) % 1;
    // single scattering misses the light the sky bounces round itself: the ambient takes a share more
    const am = o.amb; SU.uAmbZ.value.set(am.zen[0] * 1.6, am.zen[1] * 1.6, am.zen[2] * 1.6); SU.uAmbH.value.set((am.hzSun[0] + am.hzAnti[0]) * 0.8, (am.hzSun[1] + am.hzAnti[1]) * 0.8, (am.hzSun[2] + am.hzAnti[2]) * 0.8);
    // the ground's own bounce (its colour lit by the sun) for whatever faces down
    const up = _tN.copy(_cam).sub(group.position).normalize(), sd = o.sunDir || [0, 1, 0];
    const sunUp = Math.max(0, up.x * sd[0] + up.y * sd[1] + up.z * sd[2]), gc = P.slots[0].b;
    SU.uGround.value.set(am.sun[0] * gc[0] * sunUp * 0.09 + SU.uAmbZ.value.x * gc[0] * 0.5, am.sun[1] * gc[1] * sunUp * 0.09 + SU.uAmbZ.value.y * gc[1] * 0.5, am.sun[2] * gc[2] * sunUp * 0.09 + SU.uAmbZ.value.z * gc[2] * 0.5);
    // low fog: its density by world (mist thickest at dawn and dusk and in rain), lit by the horizon sky and the sun
    const alt = camD - S.R, wkx = o.weather != null ? o.weather : S.weather.k, dusk = 1 - Math.min(1, sunUp * 3);
    SU.uFog.value.set(P.fog[0] * (0.6 + 0.8 * dusk + (S.weather.kind === 'rain' ? wkx * 1.2 : 0) + (S.weather.storm ? 0.5 : 0)), P.fog[1], alt, P.fog[2]);
    SU.uFogC.value.set(SU.uAmbH.value.x * 0.9 + am.sun[0] * 0.03, SU.uAmbH.value.y * 0.9 + am.sun[1] * 0.03, SU.uAmbH.value.z * 0.9 + am.sun[2] * 0.03);
    if (gas || S.sea > 0 && alt < -2) SU.uFog.value.x = 0;
    TU.uFade.value = o.fade;
    if (o.shadow && !shRT) SU.uShadow.value.set(o.shadow[0], o.shadow[1], o.shadow[2], o.shadow[3]); else SU.uShadow.value.w = 0;
    if (o.head && o.head.k > 0) { SU.uHead.value.set(o.head.pos.x, o.head.pos.y, o.head.pos.z, o.head.k); SU.uHeadDir.value.copy(o.head.dir); } else SU.uHead.value.w = 0;
    // the sea's frame: anchored near the camera; re-anchored (with a cross-fade) once it is 2 km behind
    if (WU.uWave.value.x > 0) {
      if (!waveFrame) { anchor(wfA, camL); waveFrame = wfA; waveBlend = 0; }
      if (waveBlend > 0) { waveBlend = Math.min(1, waveBlend + dt * 0.7); if (waveBlend >= 1) { wfA.c.copy(wfB.c); wfA.t1.copy(wfB.t1); wfA.t2.copy(wfB.t2); waveBlend = 0; } }
      else if (_v.copy(camL).normalize().distanceTo(wfA.c) * S.R > 2000) { anchor(wfB, camL); waveBlend = 0.001; }
      WU.uWT1.value.copy(wfA.t1); WU.uWT2.value.copy(wfA.t2); WU.uWT1b.value.copy(wfB.t1); WU.uWT2b.value.copy(wfB.t2);
      WU.uWave.value.z = o.T % 100000; WU.uWave.value.w = waveBlend;
    }
    // rain rings on the ground (near it, in rain)
    { const wk0 = o.weather != null ? o.weather : S.weather.k; rainOn = S.weather.kind === 'rain' && wk0 > 0.05 && alt < 90 && T.grass > 0; RU.uRain.value = Math.min(1, wk0 * (S.weather.storm ? 1.5 : 1)); RU.uRainC.value.set(SU.uAmbZ.value.x * 2.2 + 0.05, SU.uAmbZ.value.y * 2.2 + 0.05, SU.uAmbZ.value.z * 2.2 + 0.06); }
    // terrain selection
    for (const m of visible) m.visible = false; visible.length = 0;
    for (const m of grassShown) m.visible = false; grassShown.length = 0;
    if (!gas) { let cov = 0; for (let f = 0; f < 6; f++) if (visit(getNode(f, 0, 0, 0))) cov++; covK = cov / 6; } else covK = 1;
    stats.grass = grassShown.length;
    request();
    if ((frameNo & 255) === 0) for (const [k, n] of nodes) if (n.seen < frameNo - 600 && !n.slot && !n.req) nodes.delete(k);
    // the air near the ground for the ground cover: extinction and in-scatter over the last tens of metres
    if (grassMat) {
      const A = o.atmo; if (A) {
        const dR = Math.exp(-Math.max(0, alt) / A.HR), dM = Math.exp(-Math.max(0, alt) / A.HM);
        GU_.uNearAir.value.set(A.bR[0] * dR + A.bMe[0] * dM, A.bR[1] * dR + A.bMe[1] * dM, A.bR[2] * dR + A.bMe[2] * dM, (A.bR[1] * dR + A.bMe[1] * dM));
        GU_.uNearIns.value.copy(SU.uAmbH.value).multiplyScalar(0.7);
      }
    }
    sky.visible = true;
    renderSkyLut(cam, sd);
    // flora
    floraT -= dt;
    if (floraT <= 0) {
      floraT = 0.25; floraPass(); farPass();
      // refill by distance when the camera has moved a band's worth (cells change near/far, the caps re-sort)
      const band = Math.floor(camL.x / 150) * 73856093 ^ Math.floor(camL.y / 150) * 19349663 ^ Math.floor(camL.z / 150) * 83492791;
      if (band !== floraBand) { floraBand = band; floraDirty = true; }
    }
    // the instance buffers are refilled at most ~3 times a second (cells arrive in bursts while you fly)
    rebuildT -= dt;
    if (rebuildT <= 0 && (floraDirty || impDirty)) { if (floraDirty) rebuildFlora(); else if (impDirty) rebuildImp(); rebuildT = 0.3; }
    floraU.uWind.value = 0.6 + S.weather.k * 1.2 + (S.weather.storm ? 0.6 : 0); floraU.uWindDir.value.set(S.weather.wind[0], 0, S.weather.wind[1]).normalize();
    // cloud puffs round the camera when near the deck
    const deck = gas ? GAS_DECK[1] : S.cloud.alt, near = Math.abs(alt - deck) < (gas ? 420 : S.cloud.thick) * 4 + 600;
    puffs.visible = near && (gas || S.cloud.cover > 0.1);
    if (puffs.visible) {
      puffU.uCamL.value.copy(camL);
      const ax = Math.abs(camL.x), ay = Math.abs(camL.y), az = Math.abs(camL.z);
      if (ax >= ay && ax >= az) { puffU.uUp.value.set(Math.sign(camL.x), 0, 0); puffU.uE.value.set(0, 1, 0); puffU.uNn.value.set(0, 0, 1); }
      else if (ay >= az) { puffU.uUp.value.set(0, Math.sign(camL.y), 0); puffU.uE.value.set(1, 0, 0); puffU.uNn.value.set(0, 0, 1); }
      else { puffU.uUp.value.set(0, 0, Math.sign(camL.z)); puffU.uE.value.set(1, 0, 0); puffU.uNn.value.set(0, 1, 0); }
    }
    // weather
    const wk = o.weather != null ? o.weather : S.weather.k, kind = S.weather.kind;
    const wAlt = kind === 'rain' || kind === 'snow' ? 1 - Math.max(0, Math.min(1, (alt - deck * 0.8) / 300)) : 1 - Math.max(0, Math.min(1, (alt - 300) / 500));
    const wVis = wk * wAlt;
    const pts = kind === 'snow' || kind === 'ash' || kind === 'motes' || kind === 'spores';
    wLines.visible = wVis > 0.02 && !pts; wPts.visible = wVis > 0.02 && pts;
    if (wVis > 0.02) {
      const upv = _v.copy(_cam).sub(group.position).normalize();
      const wx = S.weather.wind[0], wz = S.weather.wind[1];
      const tE = _v2.set(0, 1, 0).applyQuaternion(group.quaternion).cross(upv); if (tE.lengthSq() < 1e-6) tE.set(1, 0, 0); tE.normalize();
      const tN = _v3.copy(upv).cross(tE);
      const fall = wU.uFall.value, storm = S.weather.storm ? 1.6 : 1;
      if (kind === 'rain') { fall.copy(upv).multiplyScalar(-26).addScaledVector(tE, wx * 6 * storm).addScaledVector(tN, wz * 6 * storm); wU.uLen.value = 2.6; wU.uCol.value.set(0.4 + am.zen[0] * 2, 0.45 + am.zen[1] * 2, 0.5 + am.zen[2] * 2); wU.uK.value = 0.32 * wVis * (S.weather.storm ? 1.4 : 1); }
      else if (kind === 'dust') { fall.copy(upv).multiplyScalar(-0.5).addScaledVector(tE, wx * 16 * storm).addScaledVector(tN, wz * 16 * storm); wU.uLen.value = 1.6 * storm; const c = S.atmo.betaM, l = Math.max(c[0], c[1], c[2]) || 1; wU.uCol.value.set(c[0] / l * 0.45 * am.zen[0] * 6, c[1] / l * 0.45 * am.zen[1] * 6, c[2] / l * 0.45 * am.zen[2] * 6); wU.uK.value = 0.14 * wVis * storm; }
      else if (kind === 'wind') { fall.copy(tE).multiplyScalar(30); wU.uLen.value = 6; wU.uCol.value.set(0.7, 0.65, 0.55); wU.uK.value = 0.25 * wVis; }
      else if (kind === 'snow') { fall.copy(upv).multiplyScalar(-2.2).addScaledVector(tE, wx * 3 * storm).addScaledVector(tN, wz * 3 * storm); wU.uSize.value = 0.35; wU.uCol.value.set(0.9, 0.93, 1.0); wU.uK.value = 0.85 * wVis; }
      else if (kind === 'ash') { fall.copy(upv).multiplyScalar(-1.2).addScaledVector(tE, wx * 4).addScaledVector(tN, wz * 4); wU.uSize.value = 0.3; wU.uCol.value.set(0.2, 0.18, 0.17); wU.uK.value = 0.9 * wVis; }
      else { fall.copy(upv).multiplyScalar(0.6).addScaledVector(tE, wx * 1.5); wU.uSize.value = 0.22; wU.uCol.value.set(kind === 'motes' ? 0.6 : 0.7, kind === 'motes' ? 0.9 : 1.2, kind === 'motes' ? 2.2 : 0.8); wU.uK.value = 0.9 * wVis; }
      wU.uCam.value.copy(_cam); wU.uUp.value.copy(upv); wU.uT.value = (o.T % 600); wU.uPts.value = pts ? 1 : 0;
    }
    // storms (and gas giants) throw lightning: a strike, the flash in the sky and on the ground, thunder for the sound
    const stormy = gas || (S.weather.storm && (kind === 'rain' || kind === 'snow' || kind === 'dust' || kind === 'ash') && wk > 0.35);
    if (stormy && alt < S.atmo.top * 1.1) { BOLT.next -= dt; if (BOLT.next <= 0) { strike(cam, _v.copy(_cam).sub(group.position).normalize()); BOLT.next = 3 + Math.random() * (gas ? 6 : 12); } }
    if (BOLT.k > 0) {
      BOLT.t += dt; BOLT.k = Math.max(0, 1 - BOLT.t / 0.55);
      const fl = BOLT.k * (0.55 + 0.45 * Math.sin(BOLT.t * 61)) * (BOLT.t < 0.08 || (BOLT.t > 0.16 && BOLT.t < 0.24) ? 1 : 0.35), near_ = Math.max(0.15, 1 - BOLT.dist / 6000);
      SU.uBolt.value.set(0.75, 0.75, 1.0, fl * near_ * 1.6); bolt.visible = fl > 0.05;
    } else { SU.uBolt.value.w = 0; bolt.visible = false; }
    // aurora: ice worlds (and some crystal ones) at night
    const night = 1 - Math.min(1, Math.max(0, (up.x * sd[0] + up.y * sd[1] + up.z * sd[2] + 0.08) / 0.2));
    AUR.uAur.value.set((S.type === 'ice' ? 1 : S.type === 'crystal' && (S.seeds[3] & 1) ? 0.6 : 0) * night, o.T % 10000, 0, 0);
    // the shadow map of the sun (after everything that casts has moved)
    renderShadow(cam, sd, !gas && alt < 3500 && sunUp > 0.02 && o.fade > 0.5);
    // the relic crystals turn
    for (const s of sites) if (s.crystal) { s.crystal.rotation.y += dt * 0.6; s.crystal.position.y = 6.5 + Math.sin(o.T * 1.3) * 0.4; s.crystal.visible = !s.looted; }
    if ((frameNo & 15) === 0) { let c = 0; for (const sl of slots) if (sl.node) c++; stats.chunks = c; }
    stats.visible = visible.length;
    const msNow = performance.now() - t0; stats.ms = stats.ms * 0.9 + msNow * 0.1; stats.msMax = Math.max(msNow, (stats.msMax || 0) * 0.995);
  }
  function setLooted(i, v) { const s = sites[i]; if (s) s.looted = !!v; }
  // how much of the ground is built (0..1, from the last frame's walk): the terrain fades in over the orbital body by it
  let covK = 0, progReady = false, grassProxy = null, rainProxy = null, terrainProxy = null, depthProxy = null, depthIProxy = null;
  const slotGeoProxy = () => { const b = newChunkBufs(), g = new THREE.BufferGeometry(); g.setAttribute('position', new THREE.BufferAttribute(b.pos, 3)); g.setAttribute('normal', new THREE.BufferAttribute(b.nrm, 3)); g.setAttribute('aMor', new THREE.BufferAttribute(b.mor, 4)); g.setAttribute('aSrf', new THREE.BufferAttribute(b.srf, 4)); g.setAttribute('aEx', new THREE.BufferAttribute(b.ex, 4, true)); g.setIndex(index); return g; };
  const compileCam = new THREE.PerspectiveCamera();
  function coverage() { return S && progReady ? covK : 0; }
  function dispose() {
    clear();
    for (const w of workers) w.w.terminate();
    for (const s of slots) { s.geo.dispose(); if (s.grass) s.grass.m.geometry.dispose(); if (s.rain) s.rain.m.geometry.dispose(); }
    rainMat.dispose(); rainGeo.dispose();
    terrainMat.dispose(); skyMat.dispose(); lutMat.dispose(); lutQuad.geometry.dispose(); skyRT.dispose(); cloudMat.dispose(); veilMat.dispose(); cloudGeo.dispose(); for (const d of decks) d.material.dispose(); white.dispose(); puffMat.dispose(); pGeo.dispose(); wLineMat.dispose(); wPtsMat.dispose(); wGeo.dispose(); floraMat.dispose(); floraMatBig.dispose(); siteMat.dispose(); wreckMat.dispose();
    bGeo.dispose(); boltMat.dispose(); bolt.removeFromParent();
    if (impMat) impMat.dispose(); bakeMat.dispose(); if (impRT) impRT.dispose(); if (impGeo) impGeo.dispose();
    if (grassMat) grassMat.dispose(); if (grassGeo) grassGeo.dispose();
    if (shRT) { shRT.depthTexture.dispose(); shRT.dispose(); depthMat.dispose(); }
    SH.uShMap.value = null; SH.uShP.value[0] = 0; shDummy.depthTexture.dispose(); shDummy.dispose();
    if (mats) mats.dispose();
    for (const k in floraGeo) { if (floraGeo[k]) floraGeo[k].dispose(); if (floraGeoLo[k]) floraGeoLo[k].dispose(); }
    detail.dispose(); blank.dispose(); sky.geometry.dispose(); sky.removeFromParent(); puffs.removeFromParent(); wLines.removeFromParent(); wPts.removeFromParent(); group.removeFromParent();
  }
  for (const [m, n] of [[terrainMat, 'cz-terrain'], [lutMat, 'cz-skylut'], [skyMat, 'cz-sky'], [cloudMat, 'cz-cloud'], [veilMat, 'cz-veil'], [puffMat, 'cz-puff'], [floraMat, 'cz-flora'], [floraMatBig, 'cz-flora-big'], [grassMat, 'cz-grass'], [rainMat, 'cz-rain'], [impMat, 'cz-imp'], [depthMat, 'cz-depth']]) if (m) m.name = n;   // (names show in the GPU debuggers)
  const api = { group, set, clear, update, dispose, coverage, setLooted, stats, sky, get S() { return S; }, get lmax() { return lmax; }, get sites() { return sites; }, detail, SU, TU, grass: grassObj, shadowOff: false, SHADOW_LAYER, lastStrike: 0, bolt: BOLT };
  return api;
}
export { WT as WORLD_TIERS };

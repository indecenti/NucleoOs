// stelle/terrain.js — the ground of the world you are descending onto, drawn in the near pass with the ships.
//
//   · a cube-sphere quadtree of chunks (stelle/chunk.js, built by stelle/terrain-worker.js off the main thread):
//     split by distance (tier-scaled), horizon- and frustum-culled, CDLOD geomorphing (no popping, no cracks;
//     skirts as a safety net), a fixed pool of geometries whose typed arrays ping-pong with the worker;
//   · one terrain shader: the orbital colour ramp far away (the same czAlbedo the bake paints, so the world from
//     orbit is the world you land on), biome materials up close (triplanar procedural detail, slope / height
//     blending, strata, snow, sand ripples, lava and crystal veins), water with waves, sky reflection, sun glint
//     and shore foam, cloud shadows, the ship's shadow, a headlight at night, aerial perspective per vertex;
//   · the sky (a full-screen single-scattering pass that dims the far pass behind it), the cloud deck and the
//     puffs you fly through, weather (rain, snow, dust, ash, spores, motes), instanced flora with distance fade,
//     and the sites (ruins, shrines, wrecks, outposts).
// Everything sits in one group placed at the planet centre with the planet's orientation each frame.
import { cubeDir, nodeSize, GRID, maxLevel, MACRO_GLSL, FLORA_KINDS, FLORA_STRIDE } from './planet.js';
import { ATMO_GLSL, AU, atmoParams } from './atmo.js';
import { VERTS, chunkIndex, newChunkBufs } from './chunk.js';
import { floraGeometry, FLORA_BIG, siteGeometry } from './props.js';
const SITE_SCALE = { gate: 1.6, archive: 1.4, observatory: 1.6 };
import { hullMaterial, shipGeometry } from './kit.js';

const WT = {   // world settings per quality tier
  low: { K: 2.1, spacing: 4.2, pool: 300, flora: 0.32, floraR: 380, workers: 1, puffs: 48, weather: 700, inflight: 3 },
  medium: { K: 2.5, spacing: 2.6, pool: 420, flora: 0.62, floraR: 650, workers: 2, puffs: 90, weather: 1300, inflight: 4 },
  high: { K: 2.9, spacing: 1.7, pool: 540, flora: 1.0, floraR: 950, workers: 2, puffs: 140, weather: 2200, inflight: 5 },
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
uniform vec3 uAmbZ, uAmbH, uNight;
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
`;
const TERRAIN_VERT = /* glsl */`
attribute vec4 aMor; attribute vec4 aSrf;
uniform vec2 uLod; uniform mat3 uPInv;
varying vec3 vW; varying vec3 vN; varying vec3 vL; varying vec4 vSrf; varying float vRock; varying vec3 vIns; varying vec3 vTr; varying vec3 vSunT; varying float vDist;
${ATMO_GLSL}
void main() {
  float lev = floor(aMor.w), dl = uLod.x * uLod.y * exp2(-lev);
  float d0 = length((modelViewMatrix * vec4(position, 1.0)).xyz);
  vec3 pos = position + aMor.xyz * smoothstep(1.35 * dl, 1.85 * dl, d0);
  vec4 mv = modelViewMatrix * vec4(pos, 1.0), w = modelMatrix * vec4(pos, 1.0);
  vW = w.xyz; vN = normalize(mat3(modelMatrix) * normal); vL = uPInv * (w.xyz - uAtC);
  vSrf = aSrf; vRock = fract(aMor.w); vDist = length(mv.xyz);
  vIns = czAtmo(cameraPosition - uAtC, (w.xyz - cameraPosition) / max(vDist, 1e-3), vDist, 8, vTr);
  vSunT = czSunT(w.xyz - uAtC);
  gl_Position = projectionMatrix * mv;
}`;
const TERRAIN_FRAG = /* glsl */`
${MACRO_UNIFORMS}
uniform vec3 uC0, uC1, uC2, uC3, uGlow, uWater, uDeep, uHeadDir; uniform vec4 uShadow, uHead; uniform float uFade, uSnow, uA;
varying vec3 vW; varying vec3 vN; varying vec3 vL; varying vec4 vSrf; varying float vRock; varying vec3 vIns; varying vec3 vTr; varying vec3 vSunT; varying float vDist;
${ATMO_GLSL}
${MACRO_GLSL}
${COMMON}
vec4 tri(vec3 p, vec3 w) { return texture2D(uDetail, p.yz) * w.x + texture2D(uDetail, p.xz) * w.y + texture2D(uDetail, p.xy) * w.z; }
void main() {
  if (uFade < 0.999 && czH12(gl_FragCoord.xy) > uFade) discard;
  vec3 N = normalize(vN), up = normalize(vW - uAtC), V = normalize(cameraPosition - vW), L = uSunDir;
  vec3 Nl = uPInv * N, bw = pow(abs(Nl), vec3(4.0)); bw /= bw.x + bw.y + bw.z;
  float e = vSrf.x, h = vSrf.y, mo = vSrf.z, m1 = vSrf.w, lat = dot(up, uAxis), wet;
  vec3 macroA = czAlbedo(vec4(h, mo, 0.0, 0.0), lat, wet);
  float nearK = 1.0 - smoothstep(1800.0, 8000.0, vDist), closeK = 1.0 - smoothstep(60.0, 260.0, vDist);
  vec4 d1 = tri(vL / 9.0, bw), d2 = tri(vL / 73.0, bw), d0 = closeK > 0.0 ? tri(vL / 1.6, bw) : vec4(0.5);
  float slope = 1.0 - clamp(dot(N, up), 0.0, 1.0);
  float cliff = smoothstep(0.26, 0.5, slope + (d2.r - 0.5) * 0.22);
  vec3 alb, emit = vec3(0.0); float spec = 0.0, shin = 60.0;
  vec3 Ll = uPInv * L;
  float cs = czCloudShadow(vL, e, Ll);
  float ss = uShadow.w > 0.0 ? 1.0 - 0.72 * (1.0 - smoothstep(uShadow.w * 0.3, uShadow.w, distance(vW, uShadow.xyz))) : 1.0;
  vec3 amb = uAmbZ * (0.55 + 0.45 * dot(N, up)) + uAmbH * 0.3 + uNight;
  vec3 col;
  if (uSea > 0.0 && e < 0.0) {
    // ---- the liquid: water, a frozen sea, lava, a crystal lake
    float depth = -e, t = uTime;
    vec3 T1 = normalize(cross(up, uAxis + vec3(1e-4))), T2 = cross(up, T1);
    vec2 g = vec2(texture2D(uDetail, vL.xz / 31.0 + vec2(t * 0.011, t * 0.004)).r + texture2D(uDetail, vL.zy / 23.0 - vec2(t * 0.007, t * 0.012)).a - 1.0,
                  texture2D(uDetail, vL.xy / 27.0 + vec2(-t * 0.009, t * 0.006)).r + texture2D(uDetail, vL.yz / 19.0 + vec2(t * 0.013, -t * 0.005)).a - 1.0);
    if (uType > 4.5 && uType < 5.5) {   // lava: a glowing sea under a cracked crust that drifts
      // plates of dark crust drifting on open channels of glowing rock (about a third of the sea glows)
      float crust = smoothstep(0.44, 0.6, tri(vL / 41.0 + vec3(t * 0.02, 0.0, t * 0.013), bw).g + (d1.r - 0.5) * 0.3 + (d2.g - 0.5) * 0.2);
      alb = vec3(0.05, 0.035, 0.03);
      emit = uGlow * (1.0 - crust) * (0.75 + 0.5 * d1.a + 0.35 * sin(t * 0.8 + d2.r * 12.0)) * 1.7 + uGlow * 0.06 * crust;
      col = alb * (uSunI * vSunT * max(dot(up, L), 0.0) * 0.3183 * cs + amb) + emit;
    } else if (uType > 2.5 && uType < 3.5) {   // the frozen sea: pale blue ice, pressure ridges, a gloss
      vec3 nw = normalize(up + (T1 * g.x + T2 * g.y) * 0.04);
      alb = mix(vec3(0.62, 0.76, 0.9), vec3(0.88, 0.93, 0.98), smoothstep(0.3, 0.8, d2.r)) * (1.0 - 0.35 * smoothstep(0.75, 0.95, d1.g));
      vec3 R = reflect(-V, nw), sky = mix(uAmbH, uAmbZ, clamp(dot(R, up) * 2.0, 0.0, 1.0)) * 1.4;
      float fr = 0.03 + 0.97 * pow(1.0 - max(dot(nw, V), 0.0), 5.0);
      col = alb * (uSunI * vSunT * max(dot(nw, L), 0.0) * 0.3183 * cs * ss + amb);
      col = mix(col, sky, fr * 0.6) + uSunI * vSunT * cs * pow(max(dot(R, L), 0.0), 250.0) * 1.5;
    } else {   // water (and the violet glass of crystal lakes): waves, sky reflection, glint, shore foam, depth colour
      vec3 nw = normalize(up + (T1 * g.x + T2 * g.y) * (0.22 * (0.4 + 0.6 * nearK)));
      vec3 wc = mix(uWater, uDeep, smoothstep(0.0, 32.0, depth));
      vec3 R = reflect(-V, nw); R = normalize(R + up * max(0.0, -dot(R, up)) * 1.05);
      vec3 sky = mix(uAmbH * 1.3, uAmbZ * 1.6, clamp(dot(R, up) * 2.5, 0.0, 1.0));
      float fr = 0.02 + 0.98 * pow(1.0 - max(dot(nw, V), 0.0), 5.0);
      col = wc * (uSunI * vSunT * max(dot(up, L), 0.0) * 0.3183 * cs * ss * 0.7 + amb * 0.9);
      col = mix(col, sky, fr);
      float rl = max(dot(R, L), 0.0);
      col += uSunI * vSunT * cs * (pow(rl, 600.0) * 7.0 + pow(rl, 70.0) * 0.18) * ss;
      float foam = (1.0 - smoothstep(0.0, 1.6, depth)) * smoothstep(0.35, 0.75, d0.a + d1.r * 0.6 + 0.25 * sin(t * 1.3 + depth * 4.0 + d2.r * 9.0));
      col = mix(col, (uSunI * vSunT * max(dot(up, L), 0.0) * 0.3183 * cs + amb) * 0.9, foam * 0.85);
      if (uType > 6.5) col += uGlow * 0.12 * smoothstep(0.8, 0.95, d1.g);
    }
  } else {
    // ---- the ground, by world type: four scales of procedural detail (1.6 m, 9 m, 73 m, 420 m), slope and height
    vec4 d3 = tri(vL / 420.0, bw);
    float patchy = d3.r * 0.6 + d2.r * 0.4;                         // big soft patches of different ground
    float band = 0.5 + 0.5 * sin(e * 0.19 + d2.r * 5.0 + d1.r * 1.5);
    vec3 flatC = macroA, rockC = mix(uC1, uC3, band) * (0.7 + 0.6 * d1.r) * (0.85 + 0.3 * d0.g * closeK);
    float bumpH = d1.r * 0.7 + d0.a * 0.5 * closeK + d1.g * 0.35, bumpK = 1.0;
    if (uType < 0.5) {            // rocky: dusty mesa tops, red canyon sand, banded sandstone walls
      vec3 dust = mix(macroA, uC2, 0.35 + 0.3 * patchy);
      flatC = mix(dust, uC2 * 0.85, m1 * 0.8) * (0.8 + 0.4 * d1.a) * (0.85 + 0.3 * d0.b * closeK);
      flatC = mix(flatC, rockC * 0.9, smoothstep(0.55, 0.8, d2.g) * 0.5);  // bare rock showing through the dust
      cliff = max(cliff, smoothstep(0.16, 0.3, slope) * 0.7);
      bumpK = 1.4;
    } else if (uType < 1.5) {     // desert: rippled sand seas, wind-cut rock, pale salt flats
      float rip = (d0.g * closeK + d1.g * 0.6);
      vec3 sand = mix(uC2, uC2 * vec3(1.06, 0.95, 0.85), patchy) * (0.86 + 0.18 * rip);
      flatC = mix(mix(uC0, macroA, 0.5) * (0.8 + 0.3 * d1.a), sand, m1);
      flatC = mix(flatC, vec3(0.78, 0.74, 0.66), smoothstep(0.7, 0.9, d3.g) * (1.0 - m1) * 0.6);
      cliff = smoothstep(0.3, 0.52, slope + (1.0 - m1) * 0.1);
      bumpH = mix(bumpH, rip * 0.6, m1); bumpK = 0.9;
    } else if (uType < 2.5) {     // ocean islands: beaches, lush grass in patches, dark volcanic cliffs
      float beach = 1.0 - smoothstep(1.5, 4.5 + d1.r * 3.0, e);
      vec3 grass = mix(uC0, uC0 * vec3(1.25, 1.1, 0.6), patchy) * (0.7 + 0.5 * d1.r) * (0.85 + 0.3 * d0.r * closeK);
      flatC = mix(mix(grass, macroA, 0.25), uC2 * (0.9 + 0.2 * d0.a), beach);
      rockC = mix(rockC, grass * 0.6, 0.25 * (1.0 - smoothstep(0.5, 0.75, slope)));
    } else if (uType < 3.5) {     // ice: wind-packed snow, blue ice walls, crevasses
      flatC = mix(vec3(0.9, 0.94, 1.0), macroA, 0.2) * (0.9 + 0.12 * d1.a + 0.06 * d0.g * closeK);
      flatC = mix(flatC, vec3(0.62, 0.78, 0.92), smoothstep(0.6, 0.85, patchy) * 0.5);   // wind-scoured blue ice
      rockC = mix(uC1, vec3(0.55, 0.75, 0.95), 0.5) * (0.8 + 0.4 * d1.g);
      flatC = mix(flatC, vec3(0.1, 0.22, 0.4), m1 * 0.85);
      spec = 0.35; shin = 120.0; bumpK = 0.6;
    } else if (uType < 4.5) {     // jungle: moss and undergrowth in patches, muddy banks, mossy rock
      vec3 moss = mix(uC0 * (0.6 + 0.6 * mo), uC0 * vec3(0.7, 1.15, 0.6), patchy) * (0.7 + 0.5 * d1.r) * (0.8 + 0.35 * d0.r * closeK);
      flatC = mix(mix(moss, macroA, 0.2), uC2 * (0.8 + 0.3 * d1.a), m1 * 0.75);
      rockC = mix(rockC, moss * 0.75, 0.45 * (1.0 - smoothstep(0.55, 0.85, slope)));
      bumpK = 1.2;
    } else if (uType < 5.5) {     // volcanic: basalt, ash drifts, glowing fissures
      flatC = mix(uC0 * (0.8 + 0.4 * d1.a), uC2 * (0.85 + 0.3 * d0.b * closeK), smoothstep(0.42, 0.72, patchy));
      rockC = uC1 * (0.7 + 0.6 * d1.g);
      emit = uGlow * m1 * (0.45 + 0.55 * d1.g) * (2.6 + 0.8 * sin(uTime * 1.7 + d2.r * 11.0));
      emit += uGlow * 0.6 * smoothstep(0.86, 0.97, d0.g * closeK) * (1.0 - cliff) * smoothstep(0.6, 0.85, d3.r);   // embers in the ash
      bumpK = 1.5;
    } else {                      // crystal: dark violet ground, glassy facets, cyan veins
      flatC = mix(uC0, macroA, 0.35) * (0.75 + 0.5 * d1.r) * (0.85 + 0.3 * d0.b * closeK);
      rockC = uC1 * (0.65 + 0.7 * d1.g); spec = 0.6; shin = 90.0;
      emit = uGlow * m1 * (1.6 + 0.9 * sin(uTime * 2.1 + d2.r * 9.0 + e * 0.1));
      emit += uGlow * 0.5 * smoothstep(0.9, 0.98, d1.g) * (1.0 - cliff);
    }
    alb = mix(flatC, rockC, cliff);
    // snow on the high ground (not on dry worlds)
    if (uSnow > 0.0) { float sn = smoothstep(uSnow, uSnow + 40.0, e + (d2.r - 0.5) * 60.0) * (1.0 - smoothstep(0.35, 0.6, slope)); alb = mix(alb, vec3(0.9, 0.93, 0.97), sn); spec = max(spec, sn * 0.25); }
    alb = mix(macroA, alb, nearK);
    // per-pixel relief: the detail heights bend the normal (planet-local derivatives: millimetre precision)
    {
      float hb = bumpH * nearK;
      vec3 dpx = dFdx(vL), dpy = dFdy(vL); float hx = dFdx(hb), hy = dFdy(hb);
      vec3 r1 = cross(dpy, Nl), r2 = cross(Nl, dpx); float det = dot(dpx, r1);
      vec3 nb = abs(det) * Nl - sign(det) * (hx * r1 + hy * r2) * (0.9 + 1.6 * cliff) * bumpK;
      float nl = length(nb); if (nl > 1e-12) N = normalize(transpose(uPInv) * (nb / nl));
    }
    // light: sun, sky, a little cavity darkening in the rough ground
    float cav = 0.82 + 0.18 * smoothstep(0.2, 0.7, d1.r);
    vec3 sun = uSunI * vSunT * max(dot(N, L), 0.0) * cs * ss;
    col = alb * (sun * 0.3183 + amb * cav) + emit * nearK + emit * 0.4 * (1.0 - nearK);
    if (spec > 0.0) { vec3 H = normalize(L + V); col += sun * spec * pow(max(dot(N, H), 0.0), shin) * 0.5; }
  }
  if (uHead.w > 0.0) { vec3 hv = vW - uHead.xyz; float hd = length(hv); vec3 hn = hv / hd;
    col += (uSea > 0.0 && e < 0.0 ? uWater : alb) * uHead.w * smoothstep(0.8, 0.95, dot(hn, uHeadDir)) * max(dot(N, -hn), 0.0) / (1.0 + hd * hd * 0.0003); }
  col = col * vTr + vIns;
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// the sky: a full-screen pass in the near scene (the far pass's stars, sun and other worlds show through by T)
const SKY_VERT = /* glsl */`varying vec3 vRay;
void main() { vec4 v = inverse(projectionMatrix) * vec4(position.xy, 1.0, 1.0); v /= v.w; vRay = (inverse(viewMatrix) * vec4(v.xyz, 0.0)).xyz; gl_Position = vec4(position.xy, 1.0, 1.0); }`;
const SKY_FRAG = /* glsl */`varying vec3 vRay;
${ATMO_GLSL}
void main() {
  vec3 rd = normalize(vRay), ro = cameraPosition - uAtC;
  float b = dot(ro, rd), c = dot(ro, ro) - uAtR.x * uAtR.x, disc = b * b - c, tMax = 1e9;
  if (disc > 0.0) { float t = -b - sqrt(disc); if (t > 0.0) tMax = t; }
  vec3 T, L = czAtmo(ro, rd, tMax, 16, T);
  gl_FragColor = vec4(L, dot(T, vec3(0.2126, 0.7152, 0.0722)));
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;
// the cloud deck: coverage from the orbital bake (the same clouds you saw from space), detail up close, lit, in the air
const CLOUD_VERT = /* glsl */`uniform mat3 uPInv; varying vec3 vW; varying vec3 vL;
${'uniform vec3 uAtC;'}
void main() { vec4 w = modelMatrix * vec4(position, 1.0); vW = w.xyz; vL = uPInv * (w.xyz - uAtC); gl_Position = projectionMatrix * viewMatrix * w; }`;
const CLOUD_FRAG = /* glsl */`
varying vec3 vW; varying vec3 vL;
${ATMO_GLSL}
${COMMON}
void main() {
  vec3 n = normalize(vL); vec2 uv = czEq(n); uv.x += uCloudOff;
  float cov = texture2D(uCloud, uv).r * uCloudK;
  vec4 d = texture2D(uDetail, vL.xz / 1400.0 + vec2(uTime * 0.002, 0.0)) * 0.6 + texture2D(uDetail, vL.zy / 520.0 - vec2(0.0, uTime * 0.003)) * 0.4;
  float a = smoothstep(0.32, 0.72, cov + (d.r - 0.5) * 0.45 + (d.a - 0.5) * 0.2);
  if (a < 0.004) discard;
  vec3 up = normalize(vW - uAtC), V = normalize(cameraPosition - vW), L = uSunDir;
  vec3 sunT = czSunT(vW - uAtC);
  float below = step(dot(V, up), 0.0);   // seen from underneath: the base is darker, light comes through
  float lit = mix(0.55 + 0.45 * max(dot(up, L), 0.0), 0.35 * a + 0.25, below);
  float silver = pow(max(dot(-V, L), 0.0), 8.0) * (1.0 - a) * 1.5;
  vec3 col = uSunI * sunT * (lit * 0.32 + silver) + uAmbZ * 1.6 + uAmbH * 0.4;
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
varying vec2 vUv; varying float vA; varying vec3 vW;
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
  vUv = position.xy;
  gl_Position = projectionMatrix * mv;
}`;
const PUFF_FRAG = /* glsl */`
varying vec2 vUv; varying float vA; varying vec3 vW;
${ATMO_GLSL}
uniform vec3 uAmbZ;
void main() {
  float r = length(vUv), a = pow(max(0.0, 1.0 - r), 1.6) * vA;
  if (a < 0.003) discard;
  vec3 up = normalize(vW - uAtC), L = uSunDir, sunT = czSunT(vW - uAtC);
  vec3 col = uSunI * sunT * (0.25 + 0.15 * max(dot(up, L), 0.0)) + uAmbZ * 1.5;
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
// flora: instanced, wind sway, glow, distance fade (shrink + dither) and aerial perspective per vertex
const FLORA_VERT = /* glsl */`
attribute vec3 aMat;
uniform float uTime, uWind, uRange; uniform vec3 uWindDir;
varying vec3 vN; varying vec3 vC; varying vec3 vW; varying float vGlow; varying vec3 vIns; varying vec3 vTr; varying vec3 vSunT; varying float vFade; varying vec3 vUp;
${ATMO_GLSL}
void main() {
  vec3 p = position;
  float sw = aMat.z * uWind * (0.06 + 0.05 * sin(uTime * 1.9 + instanceMatrix[3].x * 0.37 + instanceMatrix[3].z * 0.21));
  p.x += sw * p.y * uWindDir.x; p.z += sw * p.y * uWindDir.z;
  vec4 ip = instanceMatrix * vec4(p, 1.0);
  vec4 w = modelMatrix * ip;
  vec4 mv = viewMatrix * w; float d = length(mv.xyz);
  vFade = smoothstep(uRange, uRange * 0.82, d);
  vec4 base = modelMatrix * instanceMatrix * vec4(0.0, 0.0, 0.0, 1.0);
  w.xyz = base.xyz + (w.xyz - base.xyz) * (0.15 + 0.85 * vFade);
  mv = viewMatrix * w;
  vW = w.xyz; vN = normalize(mat3(modelMatrix) * mat3(instanceMatrix) * normal);
  vec3 tint = vec3(1.0);
  #ifdef USE_INSTANCING_COLOR
    tint = instanceColor;
  #endif
  vC = color * mix(vec3(1.0), tint, aMat.x); vGlow = aMat.y;
  vUp = normalize(base.xyz - uAtC);
  vIns = czAtmo(cameraPosition - uAtC, (w.xyz - cameraPosition) / max(d, 1e-3), d, 6, vTr);
  vSunT = czSunT(base.xyz - uAtC);
  gl_Position = projectionMatrix * mv;
}`;
const FLORA_FRAG = /* glsl */`
uniform vec3 uAmbZ, uAmbH, uNight, uGlow; uniform vec4 uHead; uniform vec3 uHeadDir;
varying vec3 vN; varying vec3 vC; varying vec3 vW; varying float vGlow; varying vec3 vIns; varying vec3 vTr; varying vec3 vSunT; varying float vFade; varying vec3 vUp;
${ATMO_GLSL}
void main() {
  if (vFade < 0.999 && fract(sin(dot(floor(gl_FragCoord.xy), vec2(12.9898, 78.233))) * 43758.5453) > vFade) discard;
  vec3 N = normalize(vN); if (!gl_FrontFacing) N = -N;
  vec3 L = uSunDir;
  float wrap = max(dot(N, L) * 0.7 + 0.3, 0.0);
  vec3 col = vC * (uSunI * vSunT * wrap * 0.3183 + uAmbZ * (0.6 + 0.4 * dot(N, vUp)) + uAmbH * 0.3 + uNight);
  col += vC * uGlow * vGlow;
  if (uHead.w > 0.0) { vec3 hv = vW - uHead.xyz; float hd = length(hv); col += vC * uHead.w * smoothstep(0.8, 0.95, dot(hv / hd, uHeadDir)) / (1.0 + hd * hd * 0.0003); }
  gl_FragColor = vec4(col * vTr + vIns, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;

// ---- palettes per world type (linear), from the planet's own ramp so the ground matches the orbit view ----------------
function palette(S) {
  const R = S.ramp.length ? S.ramp : [[0.1, 0.1, 0.1], [0.2, 0.2, 0.2], [0.3, 0.3, 0.3], [0.4, 0.4, 0.4], [0.5, 0.5, 0.5]];
  const mul = (c, k) => [c[0] * k, c[1] * k, c[2] * k], mix = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];
  const o = lin3([1, 1, 1]);
  const P = { c0: R[2], c1: mul(R[1], 0.9), c2: R[3], c3: R[0], glow: [0, 0, 0], water: S.ocean.map((x) => x * 1.6), deep: S.ocean.map((x) => x * 0.45), snow: -1 };
  switch (S.type) {
    case 'rocky': P.c1 = mix(lin3([0.62, 0.38, 0.26]), R[1], 0.35); P.c3 = mix(lin3([0.75, 0.55, 0.42]), R[2], 0.3); P.c2 = mix(lin3([0.7, 0.36, 0.22]), R[3], 0.3); P.snow = S.A * 0.82; break;
    case 'desert': P.c0 = mix(lin3([0.62, 0.45, 0.32]), R[1], 0.4); P.c1 = mix(lin3([0.55, 0.36, 0.24]), R[1], 0.4); P.c3 = mix(lin3([0.7, 0.5, 0.34]), R[2], 0.3); P.c2 = mix(lin3([0.86, 0.66, 0.44]), R[3], 0.45); break;
    case 'ocean': P.c0 = mix(lin3([0.24, 0.48, 0.2]), R[3], 0.3); P.c1 = lin3([0.22, 0.2, 0.2]); P.c3 = lin3([0.32, 0.29, 0.27]); P.c2 = lin3([0.9, 0.84, 0.66]); P.water = mix(lin3([0.15, 0.75, 0.72]), S.ocean, 0.3); P.deep = mix(lin3([0.02, 0.12, 0.3]), S.ocean, 0.3); break;
    case 'ice': P.c1 = lin3([0.42, 0.62, 0.82]); P.c3 = lin3([0.55, 0.7, 0.85]); P.snow = S.A * 0.3; break;
    case 'jungle': P.c0 = mix(lin3([0.18, 0.4, 0.14]), R[2], 0.35); P.c1 = lin3([0.3, 0.28, 0.24]); P.c3 = lin3([0.38, 0.36, 0.3]); P.c2 = lin3([0.36, 0.27, 0.18]); P.water = mix(lin3([0.18, 0.42, 0.36]), S.ocean, 0.4); P.deep = mix(lin3([0.03, 0.1, 0.12]), S.ocean, 0.3); P.snow = S.A * 0.95; break;
    case 'volcanic': P.c0 = lin3([0.12, 0.11, 0.115]); P.c1 = lin3([0.18, 0.16, 0.16]); P.c3 = lin3([0.09, 0.08, 0.08]); P.c2 = lin3([0.38, 0.36, 0.36]); P.glow = [3.2, 0.9, 0.18]; break;
    case 'crystal': P.c0 = lin3([0.2, 0.16, 0.34]); P.c1 = lin3([0.5, 0.45, 0.85]); P.c3 = lin3([0.32, 0.28, 0.6]); P.c2 = lin3([0.25, 0.2, 0.45]); P.glow = [0.4, 1.6, 2.4]; P.water = lin3([0.45, 0.3, 0.8]); P.deep = lin3([0.1, 0.05, 0.25]); P.snow = S.A * 0.9; break;
  }
  void o;
  return P;
}
// the colour each flora kind takes on this world (instances vary around it)
function floraTint(S, kind) {
  const R = S.ramp.length ? S.ramp : [[0.3, 0.3, 0.3], [0.3, 0.3, 0.3], [0.3, 0.3, 0.3], [0.3, 0.3, 0.3], [0.3, 0.3, 0.3]];
  switch (kind) {
    case 'rock': case 'hoodoo': case 'spire': return S.type === 'rocky' || S.type === 'desert' ? lin3([0.78, 0.55, 0.42]) : S.type === 'ice' ? lin3([0.5, 0.52, 0.56]) : lin3([0.42, 0.4, 0.38]);
    case 'shrub': return lin3([0.52, 0.5, 0.3]);
    case 'palm': return lin3([0.42, 0.68, 0.28]);
    case 'coral': return lin3([0.75, 0.85, 1.0]);
    case 'icespire': case 'frost': return lin3([0.8, 0.9, 1.0]);
    case 'spiral': return [R[2][0] * 0.6 + 0.04, R[2][1] * 0.9 + 0.08, R[2][2] * 0.5 + 0.03];
    case 'fern': return lin3([0.42, 0.62, 0.3]);
    case 'glowpod': return lin3([0.5, 0.7, 0.45]);
    case 'basalt': case 'ember': return lin3([0.3, 0.29, 0.3]);
    case 'deadtree': return lin3([0.25, 0.22, 0.2]);
    case 'shard': case 'lattice': return lin3([0.75, 0.72, 1.0]);
  }
  return [0.5, 0.5, 0.5];
}

// ========================================================================================================================
export function createWorld(THREE, renderer, scene, tierName) {
  const T = WT[tierName] || WT.medium;
  const group = new THREE.Group(); group.name = 'world'; group.visible = false; scene.add(group);
  const detail = detailTexture(THREE);
  const blank = new THREE.DataTexture(new Uint8Array([0, 0, 0, 255]), 1, 1, THREE.RGBAFormat); blank.needsUpdate = true;
  const pInv = new THREE.Matrix3(), pRot = new THREE.Matrix3(), _q = new THREE.Quaternion(), _m4 = new THREE.Matrix4(), _v = new THREE.Vector3(), _v2 = new THREE.Vector3();
  // uniforms shared by every world material
  const SU = {
    uDetail: { value: detail }, uCloud: { value: blank }, uPInv: { value: pInv }, uAxis: { value: new THREE.Vector3(0, 1, 0) },
    uCloudOff: { value: 0 }, uCloudAlt: { value: 1000 }, uCloudK: { value: 0 }, uTime: { value: 0 }, uR: { value: 1 },
    uAmbZ: { value: new THREE.Vector3() }, uAmbH: { value: new THREE.Vector3() }, uNight: { value: new THREE.Vector3(0.012, 0.014, 0.022) },
    uHead: { value: new THREE.Vector4() }, uHeadDir: { value: new THREE.Vector3(0, 0, -1) }, uShadow: { value: new THREE.Vector4() },
  };
  const MU = {   // the macro colour ramp (czAlbedo)
    uSeedA: { value: new THREE.Vector4() }, uF: { value: 2 }, uMntK: { value: 1 }, uSea: { value: 0 }, uType: { value: 0 },
    r0: { value: new THREE.Vector3() }, r1: { value: new THREE.Vector3() }, r2: { value: new THREE.Vector3() }, r3: { value: new THREE.Vector3() }, r4: { value: new THREE.Vector3() }, uOcean: { value: new THREE.Vector3() },
  };
  const TU = { uLod: { value: new THREE.Vector2(2.5, 1) }, uFade: { value: 1 }, uSnow: { value: -1 }, uA: { value: 100 },
    uC0: { value: new THREE.Vector3() }, uC1: { value: new THREE.Vector3() }, uC2: { value: new THREE.Vector3() }, uC3: { value: new THREE.Vector3() },
    uGlow: { value: new THREE.Vector3() }, uWater: { value: new THREE.Vector3() }, uDeep: { value: new THREE.Vector3() } };
  const terrainMat = new THREE.ShaderMaterial({ vertexShader: TERRAIN_VERT, fragmentShader: TERRAIN_FRAG, uniforms: { ...AU, ...SU, ...MU, ...TU } });
  // drawn FIRST, in the opaque list (renderOrder), so the ground and every hull cover it with their own aerial perspective
  const skyMat = new THREE.ShaderMaterial({ vertexShader: SKY_VERT, fragmentShader: SKY_FRAG, uniforms: { ...AU }, depthTest: false, depthWrite: false, transparent: false,
    blending: THREE.CustomBlending, blendEquation: THREE.AddEquation, blendSrc: THREE.OneFactor, blendDst: THREE.SrcAlphaFactor, blendSrcAlpha: THREE.ZeroFactor, blendDstAlpha: THREE.OneFactor });
  const sky = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), skyMat); sky.frustumCulled = false; sky.renderOrder = -1000; sky.visible = false; scene.add(sky);
  const cloudMat = new THREE.ShaderMaterial({ vertexShader: CLOUD_VERT, fragmentShader: CLOUD_FRAG, uniforms: { ...AU, ...SU }, transparent: true, depthWrite: false, side: THREE.DoubleSide });
  const cloudGeo = new THREE.SphereGeometry(1, 160, 80);
  const clouds = new THREE.Mesh(cloudGeo, cloudMat); clouds.frustumCulled = false; clouds.renderOrder = 5; clouds.visible = false;
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
  // flora
  const floraU = { uTime: SU.uTime, uWind: { value: 1 }, uRange: { value: T.floraR }, uWindDir: { value: new THREE.Vector3(1, 0, 0) }, uGlow: { value: new THREE.Vector3(1, 1, 1) } };
  const floraMat = new THREE.ShaderMaterial({ vertexShader: FLORA_VERT, fragmentShader: FLORA_FRAG, uniforms: { ...AU, ...SU, ...floraU }, vertexColors: true, side: THREE.DoubleSide });
  // the big ones (trees, spires, ribs) shape the skyline: they are drawn (and fade) twice as far out
  const floraMatBig = new THREE.ShaderMaterial({ vertexShader: FLORA_VERT, fragmentShader: FLORA_FRAG, uniforms: { ...AU, ...SU, ...floraU, uRange: { value: T.floraR * 2.2 } }, vertexColors: true, side: THREE.DoubleSide });
  const floraGeo = {}, floraGeoLo = {}; for (const k of FLORA_KINDS) floraGeo[k] = floraGeoLo[k] = null;
  const FLORA_LOD_D = 320;   // big kinds beyond this (cell distance) use their far LOD mesh
  const siteMat = hullMaterial(THREE, { metal: 0.08, rough: 0.82, panelK: 0.25, panel: 0.12, env: 0.35 });
  const wreckMat = hullMaterial(THREE, { metal: 0.5, rough: 0.6, panel: 0.4, env: 0.5 }); wreckMat.userData.U.uScorch.value = 0.75;

  // ---- the chunk pool ------------------------------------------------------------------------------------------------
  const index = new THREE.BufferAttribute(chunkIndex(), 1);
  const slots = [], spare = [];
  function newSlot() {
    const b = newChunkBufs(), geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(b.pos, 3)); geo.setAttribute('normal', new THREE.BufferAttribute(b.nrm, 3));
    geo.setAttribute('aMor', new THREE.BufferAttribute(b.mor, 4)); geo.setAttribute('aSrf', new THREE.BufferAttribute(b.srf, 4)); geo.setIndex(index);
    geo.boundingSphere = new THREE.Sphere(new THREE.Vector3(), 1);
    const mesh = new THREE.Mesh(geo, terrainMat); mesh.visible = false; mesh.matrixAutoUpdate = true; group.add(mesh);
    const s = { geo, mesh, node: null, seen: 0 }; slots.push(s); return s;
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
  const stats = { chunks: 0, visible: 0, inflight: 0, built: 0, flora: 0, ms: 0 };
  const key = (f, L, x, y) => ((L * 8 + f) * 16384 + x) * 16384 + y;

  function set(S_, body) {
    clear();
    if (!S_) return;
    S = S_; P = palette(S); startWorkers();
    lmax = maxLevel(S, T.spacing); faceSize = nodeSize(S, 0);
    for (const w of workers) w.w.postMessage({ op: 'init', S });
    // material uniforms
    const sd = S.seeds; MU.uSeedA.value.set(sd[0], sd[1], sd[2], sd[3]); MU.uF.value = S.F; MU.uMntK.value = S.mntK; MU.uSea.value = S.sea; MU.uType.value = S.ti;
    S.ramp.forEach((c, i) => MU['r' + i].value.set(c[0], c[1], c[2])); MU.uOcean.value.set(S.ocean[0], S.ocean[1], S.ocean[2]);
    TU.uLod.value.set(T.K, faceSize); TU.uSnow.value = P.snow; TU.uA.value = S.A;
    TU.uC0.value.set(...P.c0); TU.uC1.value.set(...P.c1); TU.uC2.value.set(...P.c2); TU.uC3.value.set(...P.c3);
    TU.uGlow.value.set(...P.glow); TU.uWater.value.set(...P.water); TU.uDeep.value.set(...P.deep);
    floraU.uGlow.value.set(...(S.type === 'jungle' ? [0.5, 2.2, 1.8] : S.type === 'crystal' ? [0.6, 1.2, 2.4] : S.type === 'ocean' ? [0.5, 1.4, 2.4] : S.type === 'volcanic' ? [2.6, 0.9, 0.2] : [1.2, 1.2, 1.2]));
    SU.uR.value = S.R; SU.uCloud.value = body && body.aux ? body.aux : blank; SU.uCloudAlt.value = S.cloud.alt; SU.uCloudK.value = S.cloud.cover;
    gas = S.type === 'gas';
    clouds.scale.setScalar(S.R + S.cloud.alt); group.add(clouds); clouds.visible = !gas && S.cloud.cover > 0.05;
    puffU.uDeckR.value = S.R + S.cloud.alt; puffU.uThick.value = S.cloud.thick;
    if (gas) {   // the decks of a gas giant; its puffs live on the middle deck, everywhere
      decks.forEach((d, i) => { d.scale.setScalar(S.R + GAS_DECK[i]); d.material.uniforms.uSurf.value = body && body.surf ? body.surf : blank; group.add(d); d.visible = true; });
      SU.uCloud.value = white; SU.uCloudK.value = 0.62; SU.uCloudAlt.value = GAS_DECK[1]; puffU.uDeckR.value = S.R + GAS_DECK[1]; puffU.uThick.value = 420;
    }
    // flora kinds of this world
    floraKinds = S.flora.map((f) => f.kind);
    for (const k of floraKinds) {
      if (!floraGeo[k]) floraGeo[k] = floraGeometry(THREE, k);
      const cap = Math.round(FLORA_BIG.has(k) ? 1400 : 5200 * T.flora + 400);
      const im = new THREE.InstancedMesh(floraGeo[k], FLORA_BIG.has(k) ? floraMatBig : floraMat, cap); im.count = 0; im.frustumCulled = false;
      im.instanceColor = new THREE.InstancedBufferAttribute(new Float32Array(cap * 3), 3);
      im.instanceMatrix.setUsage(THREE.DynamicDrawUsage); im.instanceColor.setUsage(THREE.DynamicDrawUsage);
      group.add(im); floraMeshes[k] = { im, cap, tint: floraTint(S, k), big: FLORA_BIG.has(k), lo: null, capLo: 0 };
      if (FLORA_BIG.has(k)) {   // the far LOD: a second instanced mesh with the light version of the kind
        if (!floraGeoLo[k]) floraGeoLo[k] = floraGeometry(THREE, k, true);
        const capLo = Math.round(3200 * T.flora + 600), lo = new THREE.InstancedMesh(floraGeoLo[k], floraMatBig, capLo); lo.count = 0; lo.frustumCulled = false;
        lo.instanceColor = new THREE.InstancedBufferAttribute(new Float32Array(capLo * 3), 3);
        lo.instanceMatrix.setUsage(THREE.DynamicDrawUsage); lo.instanceColor.setUsage(THREE.DynamicDrawUsage);
        group.add(lo); floraMeshes[k].lo = lo; floraMeshes[k].capLo = capLo;
      }
    }
    // sites
    for (const st of S.sites) {
      const sg = siteGeometry(THREE, st.kind, st.seed, st.fac);
      const g = new THREE.Group();
      const d = st.dir, r = S.R + st.e;
      g.position.set(d[0] * r, d[1] * r, d[2] * r);
      _q.setFromUnitVectors(_v.set(0, 1, 0), _v2.set(d[0], d[1], d[2])); g.quaternion.copy(_q); g.rotateY(st.yaw);
      g.scale.setScalar(SITE_SCALE[st.kind] || 1);   // the Costellatori built big: their ruins stand over the ridges
      const m = new THREE.Mesh(sg.geo, siteMat); g.add(m);
      let crystal = null;
      if (sg.crystal) { crystal = new THREE.Mesh(sg.crystal, siteMat); crystal.position.y = 6.5; g.add(crystal); }
      if (st.kind === 'wreck') {
        const hull = new THREE.Mesh(shipGeometry(THREE, ['hauler', 'warden', 'hulk'][st.seed % 3], st.seed % 3 === 2 ? 2 : 0, false), wreckMat); hull.userData.shared = true;
        const sc = st.seed % 3 === 0 ? 0.6 : 0.32; hull.scale.setScalar(sc); hull.rotation.set(0.25 + (st.seed % 7) * 0.05, 0, 0.35 - (st.seed % 5) * 0.12); hull.position.y = 3; g.add(hull);
      }
      group.add(g);
      sites.push({ st, g, crystal, lights: sg.lights, h: sg.h, found: false });
    }
    group.visible = true;
    if (window.__cz) window.__cz.world = api;
  }
  function clear() {
    for (const s of slots) { s.mesh.visible = false; s.node = null; }
    nodes.clear(); visible.length = 0; wantReq.length = 0;
    for (const [id, p] of pending) { if (p.cell) p.dead = true; else p.dead = true; }
    for (const k in floraMeshes) { const fm = floraMeshes[k]; fm.im.removeFromParent(); fm.im.dispose(); if (fm.lo) { fm.lo.removeFromParent(); fm.lo.dispose(); } }
    floraMeshes = {}; floraCells = new Map(); floraKinds = [];
    for (const s of sites) { s.g.removeFromParent(); s.g.traverse((o) => { if (o.isMesh && o.geometry && !o.userData.shared) o.geometry.dispose(); }); }
    sites = [];
    if (S) for (const w of workers) w.w.postMessage({ op: 'drop', key: S.key });
    clouds.removeFromParent(); clouds.visible = false; sky.visible = false; puffs.visible = false; wLines.visible = wPts.visible = false;
    for (const d of decks) { d.removeFromParent(); d.visible = false; } gas = false;
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
  function visit(n) {
    n.seen = frameNo; if (n.slot) n.slot.seen = frameNo;   // ancestors stay cached: a merge back must not rebuild them
    if (culled(n)) return true;   // nothing to draw: counts as ready
    const d = distOf(n);
    const split = n.L < lmax && d < T.K * n.size;
    if (split) {
      const L1 = n.L + 1, x2 = n.x * 2, y2 = n.y * 2;
      const c0 = getNode(n.f, L1, x2, y2), c1 = getNode(n.f, L1, x2 + 1, y2), c2 = getNode(n.f, L1, x2, y2 + 1), c3 = getNode(n.f, L1, x2 + 1, y2 + 1);
      const ok = (c) => c.ready || culled(c);
      if (ok(c0) && ok(c1) && ok(c2) && ok(c3)) { visit(c0); visit(c1); visit(c2); visit(c3); return true; }
      for (const c of [c0, c1, c2, c3]) { c.seen = frameNo; if (c.slot) c.slot.seen = frameNo; if (!c.ready && !c.req && !culled(c)) wantReq.push(c); }
    }
    if (n.ready) { show(n); return true; }
    if (!n.req) wantReq.push(n);
    return false;
  }
  function show(n) { const s = n.slot; s.seen = frameNo; s.mesh.visible = true; visible.push(s.mesh); }
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
      w.w.postMessage({ op: 'chunk', id, key: S.key, f: n.f, L: n.L, x: n.x, y: n.y, bufs }, [bufs.pos.buffer, bufs.nrm.buffer, bufs.mor.buffer, bufs.srf.buffer]);
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
    spare.push({ pos: a.position.array, nrm: a.normal.array, mor: a.aMor.array, srf: a.aSrf.array });
    a.position.array = m.bufs.pos; a.normal.array = m.bufs.nrm; a.aMor.array = m.bufs.mor; a.aSrf.array = m.bufs.srf;
    a.position.needsUpdate = a.normal.needsUpdate = a.aMor.needsUpdate = a.aSrf.needsUpdate = true;
    const I = m.info; s.mesh.position.set(I.cx, I.cy, I.cz); s.geo.boundingSphere.radius = I.rad;
    n.cx = I.cx; n.cy = I.cy; n.cz = I.cz; n.rad = I.rad; n.emax = I.emax; n.emin = I.emin;
    s.node = n; n.slot = s; n.ready = true; n.req = false; s.seen = frameNo;
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
    if (!p || !S || !m.ok || floraCells.get(p.cell.k) !== p.cell) { floraBufs.push(m.buf); return; }
    const c = p.cell; c.buf = m.buf; c.n = m.n; c.cx = m.cx; c.cy = m.cy; c.cz = m.cz; c.ready = true; c.req = false; floraDirty = true;
  }
  const _mtx = new THREE.Matrix4(), _pos = new THREE.Vector3(), _sc = new THREE.Vector3(), _up = new THREE.Vector3(), _qq = new THREE.Quaternion(), _qy = new THREE.Quaternion(), _col = new THREE.Color(), _Y = new THREE.Vector3(0, 1, 0);
  let floraT = 0, floraBand = 0;
  const floraSorted = [];
  function rebuildFlora() {
    floraDirty = false;
    const counts = {}, countsLo = {}; for (const k of floraKinds) counts[k] = countsLo[k] = 0;
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
        const useLo = far && fm.lo, ci = useLo ? countsLo[kind] : counts[kind]; if (ci >= (useLo ? fm.capLo : fm.cap)) continue;
        const im = useLo ? fm.lo : fm.im;
        const x = c.buf[o] + c.cx, y = c.buf[o + 1] + c.cy, z = c.buf[o + 2] + c.cz, s = c.buf[o + 3];
        _pos.set(x, y, z); _up.copy(_pos).normalize();
        _qq.setFromUnitVectors(_Y, _up); _qy.setFromAxisAngle(_Y, c.buf[o + 4]); _qq.multiply(_qy);
        if (c.buf[o + 7]) { _qy.setFromAxisAngle(_v.set(1, 0, 0), c.buf[o + 7]); _qq.multiply(_qy); }
        _sc.setScalar(s); _mtx.compose(_pos, _qq, _sc); im.setMatrixAt(ci, _mtx);
        const t = 0.82 + c.buf[o + 6] * 0.36; _col.setRGB(fm.tint[0] * t, fm.tint[1] * t, fm.tint[2] * t); im.setColorAt(ci, _col);
        if (useLo) countsLo[kind] = ci + 1; else counts[kind] = ci + 1; total++;
      }
    }
    for (const k of floraKinds) {
      const fm = floraMeshes[k]; fm.im.count = counts[k]; if (counts[k]) { fm.im.instanceMatrix.needsUpdate = true; fm.im.instanceColor.needsUpdate = true; }
      if (fm.lo) { fm.lo.count = countsLo[k]; if (countsLo[k]) { fm.lo.instanceMatrix.needsUpdate = true; fm.lo.instanceColor.needsUpdate = true; } }
    }
    stats.flora = total;
  }

  // ---- per frame -----------------------------------------------------------------------------------------------------
  // o: { center[3], quat[4], camPos (Vector3), sunDir[3], sunI[3], amb (atmo.ambientRes), T (time), fade (0..1), shadow: [x,y,z,r] | null,
  //      head: { on, pos, dir, k }, weather: k, wind }
  const _cam = new THREE.Vector3(), _tN = new THREE.Vector3();
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
    SU.uTime.value = o.T % 10000; SU.uCloudOff.value = (o.T * 0.000004) % 1;
    // single scattering misses the light the sky bounces round itself: the ambient takes a share more
    const am = o.amb; SU.uAmbZ.value.set(am.zen[0] * 1.6, am.zen[1] * 1.6, am.zen[2] * 1.6); SU.uAmbH.value.set((am.hzSun[0] + am.hzAnti[0]) * 0.8, (am.hzSun[1] + am.hzAnti[1]) * 0.8, (am.hzSun[2] + am.hzAnti[2]) * 0.8);
    TU.uFade.value = o.fade;
    if (o.shadow) SU.uShadow.value.set(o.shadow[0], o.shadow[1], o.shadow[2], o.shadow[3]); else SU.uShadow.value.w = 0;
    if (o.head && o.head.k > 0) { SU.uHead.value.set(o.head.pos.x, o.head.pos.y, o.head.pos.z, o.head.k); SU.uHeadDir.value.copy(o.head.dir); } else SU.uHead.value.w = 0;
    // terrain selection
    for (const m of visible) m.visible = false; visible.length = 0;
    if (!gas) { let cov = 0; for (let f = 0; f < 6; f++) if (visit(getNode(f, 0, 0, 0))) cov++; covK = cov / 6; } else covK = 1;
    request();
    if ((frameNo & 255) === 0) for (const [k, n] of nodes) if (n.seen < frameNo - 600 && !n.slot && !n.req) nodes.delete(k);
    // sky, clouds, puffs
    const alt = camD - S.R;
    sky.visible = true;
    // flora
    floraT -= dt;
    if (floraT <= 0) {
      floraT = 0.25; floraPass();
      // refill by distance when the camera has moved a band's worth (cells change near/far, the caps re-sort)
      const band = Math.floor(camL.x / 150) * 73856093 ^ Math.floor(camL.y / 150) * 19349663 ^ Math.floor(camL.z / 150) * 83492791;
      if (band !== floraBand) { floraBand = band; floraDirty = true; }
    }
    if (floraDirty) rebuildFlora();
    floraU.uWind.value = 0.6 + S.weather.k * 1.2; floraU.uWindDir.value.set(S.weather.wind[0], 0, S.weather.wind[1]).normalize();
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
      const up = _v.copy(_cam).sub(group.position).normalize();
      const wx = S.weather.wind[0], wz = S.weather.wind[1];
      const tE = _v2.set(0, 1, 0).applyQuaternion(group.quaternion).cross(up); if (tE.lengthSq() < 1e-6) tE.set(1, 0, 0); tE.normalize();
      const tN = _tN.copy(up).cross(tE);
      const fall = wU.uFall.value;
      if (kind === 'rain') { fall.copy(up).multiplyScalar(-26).addScaledVector(tE, wx * 6).addScaledVector(tN, wz * 6); wU.uLen.value = 2.6; wU.uCol.value.set(0.4 + am.zen[0] * 2, 0.45 + am.zen[1] * 2, 0.5 + am.zen[2] * 2); wU.uK.value = 0.32 * wVis; }
      else if (kind === 'dust') { fall.copy(up).multiplyScalar(-0.5).addScaledVector(tE, wx * 16).addScaledVector(tN, wz * 16); wU.uLen.value = 1.6; const c = S.atmo.betaM, l = Math.max(c[0], c[1], c[2]) || 1; wU.uCol.value.set(c[0] / l * 0.45 * am.zen[0] * 6, c[1] / l * 0.45 * am.zen[1] * 6, c[2] / l * 0.45 * am.zen[2] * 6); wU.uK.value = 0.14 * wVis; }
      else if (kind === 'wind') { fall.copy(tE).multiplyScalar(30); wU.uLen.value = 6; wU.uCol.value.set(0.7, 0.65, 0.55); wU.uK.value = 0.25 * wVis; }
      else if (kind === 'snow') { fall.copy(up).multiplyScalar(-2.2).addScaledVector(tE, wx * 3).addScaledVector(tN, wz * 3); wU.uSize.value = 0.35; wU.uCol.value.set(0.9, 0.93, 1.0); wU.uK.value = 0.85 * wVis; }
      else if (kind === 'ash') { fall.copy(up).multiplyScalar(-1.2).addScaledVector(tE, wx * 4).addScaledVector(tN, wz * 4); wU.uSize.value = 0.3; wU.uCol.value.set(0.2, 0.18, 0.17); wU.uK.value = 0.9 * wVis; }
      else { fall.copy(up).multiplyScalar(0.6).addScaledVector(tE, wx * 1.5); wU.uSize.value = 0.22; wU.uCol.value.set(kind === 'motes' ? 0.6 : 0.7, kind === 'motes' ? 0.9 : 1.2, kind === 'motes' ? 2.2 : 0.8); wU.uK.value = 0.9 * wVis; }
      wU.uCam.value.copy(_cam); wU.uUp.value.copy(up); wU.uT.value = (o.T % 600); wU.uPts.value = pts ? 1 : 0;
    }
    // the relic crystals turn
    for (const s of sites) if (s.crystal) { s.crystal.rotation.y += dt * 0.6; s.crystal.position.y = 6.5 + Math.sin(o.T * 1.3) * 0.4; s.crystal.visible = !s.looted; }
    stats.chunks = slots.filter((s) => s.node).length; stats.visible = visible.length;
    stats.ms = stats.ms * 0.9 + (performance.now() - t0) * 0.1;
  }
  function setLooted(i, v) { const s = sites[i]; if (s) s.looted = !!v; }
  // how much of the ground is built (0..1, from the last frame's walk): the terrain fades in over the orbital body by it
  let covK = 0;
  function coverage() { return S ? covK : 0; }
  function dispose() {
    clear();
    for (const w of workers) w.w.terminate();
    for (const s of slots) s.geo.dispose();
    terrainMat.dispose(); skyMat.dispose(); cloudMat.dispose(); cloudGeo.dispose(); for (const d of decks) d.material.dispose(); white.dispose(); puffMat.dispose(); pGeo.dispose(); wLineMat.dispose(); wPtsMat.dispose(); wGeo.dispose(); floraMat.dispose(); floraMatBig.dispose(); siteMat.dispose(); wreckMat.dispose();
    for (const k in floraGeo) { if (floraGeo[k]) floraGeo[k].dispose(); if (floraGeoLo[k]) floraGeoLo[k].dispose(); }
    detail.dispose(); blank.dispose(); sky.geometry.dispose(); sky.removeFromParent(); puffs.removeFromParent(); wLines.removeFromParent(); wPts.removeFromParent(); group.removeFromParent();
  }
  const api = { group, set, clear, update, dispose, coverage, setLooted, stats, sky, get S() { return S; }, get lmax() { return lmax; }, get sites() { return sites; }, detail, SU, TU };
  return api;
}
export { WT as WORLD_TIERS };

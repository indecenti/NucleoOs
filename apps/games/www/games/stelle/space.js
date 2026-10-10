// stelle/space.js — the environment of a star system, generated on the GPU from a world.js blueprint:
//   · a nebula skybox baked once into a cube map (domain-warped fbm, galactic band, dust lanes, faint
//     stars) that is also the image-based lighting of every ship (PMREM);
//   · a crisp point starfield; the star (granulated core, animated corona, glare, lens ghosts);
//   · planets whose surface, clouds and night lights are baked into equirect textures by a noise shader
//     and lit live (terminator, ocean glint, city lights, atmosphere rim with sunset tint, rings with the
//     planet's shadow) — no downloaded textures;
//   · the instanced asteroid field, the station / beacon models, nav holograms, speed-line dust and the
//     hyperspace tunnel.
// Far things (sky, star, planets) live in `far` and are drawn with the camera's rotation only; the
// battle space (`near`) is drawn on top with a fresh depth buffer — no depth-precision fights at 1e5 m.
import { stationGeometry, beaconGeometry, rockGeometry, hullMaterial } from './kit.js';
import { rng } from './world.js';

const lin3 = (c) => [Math.pow(c[0], 2.2), Math.pow(c[1], 2.2), Math.pow(c[2], 2.2)];

// ---- GLSL: 3D simplex noise (Ashima Arts / Stefan Gustavson, MIT) + fbm --------------------------
export const NOISE = /* glsl */`
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 mod289(vec4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec4 permute(vec4 x) { return mod289(((x * 34.0) + 1.0) * x); }
vec4 taylorInvSqrt(vec4 r) { return 1.79284291400159 - 0.85373472095314 * r; }
float snoise(vec3 v) {
  const vec2 C = vec2(1.0 / 6.0, 1.0 / 3.0); const vec4 D = vec4(0.0, 0.5, 1.0, 2.0);
  vec3 i = floor(v + dot(v, C.yyy)); vec3 x0 = v - i + dot(i, C.xxx);
  vec3 g = step(x0.yzx, x0.xyz); vec3 l = 1.0 - g; vec3 i1 = min(g.xyz, l.zxy); vec3 i2 = max(g.xyz, l.zxy);
  vec3 x1 = x0 - i1 + C.xxx; vec3 x2 = x0 - i2 + C.yyy; vec3 x3 = x0 - D.yyy;
  i = mod289(i);
  vec4 p = permute(permute(permute(i.z + vec4(0.0, i1.z, i2.z, 1.0)) + i.y + vec4(0.0, i1.y, i2.y, 1.0)) + i.x + vec4(0.0, i1.x, i2.x, 1.0));
  float n_ = 0.142857142857; vec3 ns = n_ * D.wyz - D.xzx;
  vec4 j = p - 49.0 * floor(p * ns.z * ns.z); vec4 x_ = floor(j * ns.z); vec4 y_ = floor(j - 7.0 * x_);
  vec4 x = x_ * ns.x + ns.yyyy; vec4 y = y_ * ns.x + ns.yyyy; vec4 h = 1.0 - abs(x) - abs(y);
  vec4 b0 = vec4(x.xy, y.xy); vec4 b1 = vec4(x.zw, y.zw);
  vec4 s0 = floor(b0) * 2.0 + 1.0; vec4 s1 = floor(b1) * 2.0 + 1.0; vec4 sh = -step(h, vec4(0.0));
  vec4 a0 = b0.xzyw + s0.xzyw * sh.xxyy; vec4 a1 = b1.xzyw + s1.xzyw * sh.zzww;
  vec3 p0 = vec3(a0.xy, h.x); vec3 p1 = vec3(a0.zw, h.y); vec3 p2 = vec3(a1.xy, h.z); vec3 p3 = vec3(a1.zw, h.w);
  vec4 norm = taylorInvSqrt(vec4(dot(p0, p0), dot(p1, p1), dot(p2, p2), dot(p3, p3)));
  p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;
  vec4 m = max(0.6 - vec4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0); m = m * m;
  return 42.0 * dot(m * m, vec4(dot(p0, x0), dot(p1, x1), dot(p2, x2), dot(p3, x3)));
}
float fbm(vec3 p) { float a = 0.5, s = 0.0; for (int i = 0; i < 6; i++) { s += a * snoise(p); p = p * 2.03 + vec3(1.7, 9.2, 3.1); a *= 0.5; } return s * 0.5 + 0.5; }
float fbm4(vec3 p) { float a = 0.5, s = 0.0; for (int i = 0; i < 4; i++) { s += a * snoise(p); p = p * 2.07 + vec3(4.3, 1.1, 7.7); a *= 0.5; } return s * 0.5 + 0.5; }
float ridged(vec3 p) { float a = 0.5, s = 0.0; for (int i = 0; i < 5; i++) { s += a * (1.0 - abs(snoise(p))); p = p * 2.1 + 3.3; a *= 0.5; } return s; }
vec3 hash33(vec3 p) { p = fract(p * vec3(443.897, 441.423, 437.195)); p += dot(p, p.yxz + 19.19); return fract((p.xxy + p.yxx) * p.zyx); }
`;

// ---- sky -------------------------------------------------------------------------------------------
const SKY_FRAG = /* glsl */`
uniform vec3 uA, uB, uDust, uAxis, uSeed; uniform float uDensity, uWarp, uScale, uBand, uStars;
varying vec3 vDir;
${NOISE}
float starLayer(vec3 d, float S, float thr, float sz) {
  vec3 c = floor(d * S); vec3 h = hash33(c);
  if (h.x < thr) return 0.0;
  vec3 sp = normalize(c + 0.2 + h * 0.6); float dd = length(d - sp) * S;
  return smoothstep(sz, 0.0, dd) * (0.4 + h.y * 1.6);
}
void main() {
  vec3 d = normalize(vDir);
  vec3 p = d * uScale * 1.25 + uSeed;
  // big-scale structure decides where the nebula lives; domain-warped fbm shapes soft veils inside it
  float big = fbm4(d * 0.9 + uSeed * 0.31);
  vec3 q = vec3(fbm4(p + 1.7), fbm4(p + 9.2), fbm4(p + 4.4));
  float n = fbm(p * 1.1 + (q - 0.5) * uWarp * 2.4);
  float n2 = fbm(p * 1.7 - (q - 0.5) * uWarp * 1.8 + 3.0);
  float region = smoothstep(0.38, 0.72, big);
  float ax = dot(d, uAxis), band = exp(-ax * ax / (uBand * uBand * 0.25));
  float a = smoothstep(0.3, 0.9, n) * region, b = smoothstep(0.42, 0.95, n2) * (0.35 + 0.65 * region);
  a = a * a; b = b * b;
  vec3 col = uA * a * 1.05 * uDensity + uB * b * 0.85 * uDensity;
  // bright cores where both veils overlap, a faint glow halo around them
  col += mix(uA, uB, 0.5) * pow(a * b, 0.7) * 0.9 * uDensity;
  col += uA * smoothstep(0.2, 0.8, big) * 0.025;
  // galactic band: a milky stripe crossed by dark dust lanes
  float lane = smoothstep(0.48, 0.74, fbm4(p * 2.6 + 7.0));
  vec3 milk = mix(vec3(0.82, 0.78, 0.72), uB, 0.2) * band * (0.04 + 0.08 * fbm4(p * 5.0));
  col += milk * (1.0 - lane * 0.85);
  col *= 1.0 - lane * (0.25 + band * 0.45) * smoothstep(0.1, 0.6, a + b + band);
  // stars: mostly faint, a few bright; denser along the band
  float st = starLayer(d, 340.0, 0.82, 0.16) * 0.22 + starLayer(d, 170.0, 0.9, 0.17) * 0.5 * (0.5 + band) + starLayer(d, 90.0, 0.95, 0.16) * 0.9;
  col += vec3(st) * uStars * (1.0 - lane * 0.7);
  gl_FragColor = vec4(col * 0.75, 1.0);
}`;

// ---- planet bake (equirect: rgb albedo + a height ; aux: r clouds, g night lights / lava, b ocean) --
const BAKE_FRAG = /* glsl */`
uniform vec3 r0, r1, r2, r3, r4, uOcean; uniform float uSea, uNoise, uBands, uType, uSeed, uLights;
uniform int uAux; varying vec2 vUv;
${NOISE}
vec3 ramp(float t) {
  t = clamp(t, 0.0, 1.0) * 4.0;
  if (t < 1.0) return mix(r0, r1, t); if (t < 2.0) return mix(r1, r2, t - 1.0); if (t < 3.0) return mix(r2, r3, t - 2.0); return mix(r3, r4, t - 3.0);
}
void main() {
  float lon = (vUv.x - 0.5) * 6.2831853, lat = (vUv.y - 0.5) * 3.14159265;
  vec3 d = vec3(cos(lat) * cos(lon), sin(lat), cos(lat) * sin(lon));
  vec3 p = d * uNoise + uSeed;
  vec3 w = vec3(fbm4(p + 2.0), fbm4(p + 5.0), fbm4(p + 8.0)) - 0.5;
  float h = fbm(p + w * 0.9);
  float mnt = ridged(p * 1.7 + 4.0);
  h = clamp(h * 0.75 + mnt * 0.25 * smoothstep(0.45, 0.7, h), 0.0, 1.0);
  float moist = fbm4(p * 1.4 + 11.0);
  float polar = smoothstep(0.62, 0.92, abs(d.y) + (fbm4(p * 3.0) - 0.5) * 0.25);
  vec3 alb; float ocean = 0.0, glow = 0.0;
  if (uType > 5.5 && uType < 6.5) {             // gas giant: turbulent latitude bands
    float lt = d.y * uBands + (fbm(p * vec3(1.0, 3.0, 1.0)) - 0.5) * 2.2 + sin(d.x * 3.0 + d.z * 2.0) * 0.15;
    float bnd = 0.5 + 0.5 * sin(lt * 3.14159);
    float storm = smoothstep(0.78, 0.86, fbm4(p * 2.5 + 3.0)) * step(abs(d.y), 0.5);
    alb = ramp(bnd * 0.8 + fbm4(p * 6.0) * 0.2); alb = mix(alb, r4 * 1.1, storm * 0.6);
    h = 0.5;
  } else {
    float sea = uSea;
    if (sea > 0.0 && h < sea) {
      float dep = (sea - h) / max(sea, 0.01);
      alb = mix(uOcean * 1.6, uOcean * 0.55, smoothstep(0.0, 0.6, dep)); ocean = 1.0;
      if (uType > 2.5 && uType < 3.5) { alb = mix(alb, vec3(0.85, 0.92, 0.98), 0.7); ocean = 0.3; }   // frozen sea
    } else {
      float t = sea > 0.0 ? (h - sea) / (1.0 - sea) : h;
      alb = ramp(t * 0.85 + moist * 0.25 - 0.05);
      if (uType > 4.5 && uType < 5.5) { float cr = smoothstep(0.06, 0.0, abs(fbm4(p * 4.0) - 0.5)); glow = cr * (1.0 - t); alb = mix(alb, vec3(0.05), 0.4); }   // volcanic: lava cracks
      if (uType > 6.5) { float v = smoothstep(0.08, 0.0, abs(ridged(p * 2.4) - 1.25)); glow = v * 0.8; }   // crystal: lattice veins
    }
    if (uType < 4.5 || uType > 6.5) alb = mix(alb, vec3(0.92, 0.95, 1.0), polar * (uType > 0.5 && uType < 1.5 ? 0.25 : 0.9));
  }
  if (uAux == 1) {
    float cl = fbm(d * uNoise * 1.3 + vec3(uSeed * 0.7) + w * 1.5);
    cl = smoothstep(0.45, 0.78, cl) * (0.6 + 0.4 * smoothstep(0.0, 0.5, abs(d.y) + 0.1));
    float city = 0.0;
    if (uLights > 0.5 && ocean < 0.5) city = smoothstep(0.62, 0.8, fbm4(p * 9.0)) * smoothstep(0.3, 0.6, moist) * (1.0 - polar);
    gl_FragColor = vec4(cl, max(city, glow), ocean, 1.0);
  } else gl_FragColor = vec4(alb, h);
}`;

const PLANET_VERT = /* glsl */`
varying vec3 vN; varying vec3 vW; varying vec3 vV;
void main() { vN = normal; vec4 w = modelMatrix * vec4(position, 1.0); vW = normalize(mat3(modelMatrix) * normal); vV = normalize(cameraPosition - w.xyz); gl_Position = projectionMatrix * viewMatrix * w; }`;
const PLANET_FRAG = /* glsl */`
uniform sampler2D uSurf, uAux; uniform vec3 uSun, uSunCol, uAtmo, uLightCol; uniform float uAtmoK, uSea, uType, uBump;
uniform mat3 uRot;
varying vec3 vN; varying vec3 vW; varying vec3 vV;
vec2 eq(vec3 n) { return vec2(atan(n.z, n.x) / 6.2831853 + 0.5, asin(clamp(n.y, -1.0, 1.0)) / 3.14159265 + 0.5); }
void main() {
  vec3 n = normalize(vN);
  vec2 uv = eq(n);
  vec4 s = texture2D(uSurf, uv); vec4 x = texture2D(uAux, uv);
  // bump from the height channel
  vec3 T = normalize(cross(vec3(0.0, 1.0, 0.0), n) + 1e-4), B = cross(n, T);
  float e = 1.0 / 512.0;
  float hx = texture2D(uSurf, uv + vec2(e, 0.0)).a - s.a, hy = texture2D(uSurf, uv + vec2(0.0, e)).a - s.a;
  vec3 nb = normalize(n - (T * hx + B * hy) * uBump * (1.0 - x.b));
  vec3 N = normalize(uRot * nb), V = normalize(vV), L = normalize(uSun);
  float ndl = dot(N, L), ndl0 = dot(normalize(vW), L);
  float day = smoothstep(-0.06, 0.28, ndl);
  vec3 col = s.rgb * uSunCol * max(ndl, 0.0) * 1.15 + s.rgb * 0.012;
  vec3 H = normalize(L + V);
  col += uSunCol * x.b * pow(max(dot(normalize(vW), H), 0.0), 70.0) * 1.6 * day;
  float night = 1.0 - smoothstep(-0.18, 0.08, ndl0);
  col += uLightCol * x.g * (uType > 4.5 ? 2.2 : 2.6 * night);
  float fr = pow(1.0 - max(dot(normalize(vW), V), 0.0), 2.6);
  vec3 sunset = mix(uAtmo, vec3(1.0, 0.45, 0.2), smoothstep(0.35, 0.0, abs(ndl0)) * 0.7);
  col = mix(col, sunset * (0.25 + day), fr * uAtmoK * 0.75 * smoothstep(-0.25, 0.15, ndl0));
  gl_FragColor = vec4(col, 1.0);
}`;
const CLOUD_FRAG = /* glsl */`
uniform sampler2D uAux; uniform vec3 uSun, uSunCol, uAtmo; uniform float uK, uTime; uniform mat3 uRot;
varying vec3 vN; varying vec3 vW; varying vec3 vV;
void main() {
  vec3 n = normalize(vN);
  vec2 uv = vec2(atan(n.z, n.x) / 6.2831853 + 0.5 + uTime, asin(clamp(n.y, -1.0, 1.0)) / 3.14159265 + 0.5);
  float c = texture2D(uAux, uv).r * uK;
  float ndl = dot(normalize(vW), normalize(uSun));
  float lit = smoothstep(-0.12, 0.35, ndl);
  vec3 col = mix(vec3(0.05, 0.06, 0.09), uSunCol * 1.05, lit);
  col = mix(col, col * mix(vec3(1.0), vec3(1.0, 0.55, 0.35), smoothstep(0.3, 0.0, abs(ndl))), 0.6 * lit);
  gl_FragColor = vec4(col, clamp(c, 0.0, 1.0) * 0.92);
}`;
const ATMO_FRAG = /* glsl */`
uniform vec3 uSun, uAtmo; uniform float uK;
varying vec3 vN; varying vec3 vW; varying vec3 vV;
void main() {
  vec3 N = normalize(vW), V = normalize(vV), L = normalize(uSun);
  float rim = pow(1.0 - abs(dot(N, V)), 3.2);
  float ndl = dot(N, L);
  float day = smoothstep(-0.35, 0.5, ndl);
  vec3 c = mix(uAtmo, vec3(1.0, 0.5, 0.25), smoothstep(0.4, -0.05, ndl) * day * 0.8);
  float fwd = pow(max(dot(-V, L), 0.0), 8.0) * 2.0;          // forward scattering when back-lit
  gl_FragColor = vec4(c * rim * (day * 1.6 + fwd) * uK, 1.0);
}`;
const RING_FRAG = /* glsl */`
uniform vec3 uSun, uSunCol, uCol, uCenter; uniform float uIn, uOut, uR, uDens, uSeed;
varying vec3 vPos; varying vec3 vWP;
float hh(float x) { return fract(sin(x * 91.7 + uSeed) * 43758.5453); }
float n1(float x) { float i = floor(x), f = fract(x); f = f * f * (3.0 - 2.0 * f); return mix(hh(i), hh(i + 1.0), f); }
void main() {
  float r = length(vPos.xy), t = (r - uIn) / (uOut - uIn);
  if (t < 0.0 || t > 1.0) discard;
  float b = n1(t * 40.0) * 0.6 + n1(t * 130.0) * 0.4;
  float gap = smoothstep(0.05, 0.0, abs(t - 0.62)) + smoothstep(0.02, 0.0, abs(t - 0.31)) * 0.7;
  float a = b * uDens * smoothstep(0.0, 0.06, t) * smoothstep(1.0, 0.9, t) * (1.0 - gap);
  // planet shadow: does the ray toward the sun hit the planet sphere?
  vec3 L = normalize(uSun), o = vWP - uCenter;
  float bb = dot(o, L), cc = dot(o, o) - uR * uR;
  float sh = (bb < 0.0 && bb * bb - cc > 0.0) ? 0.12 : 1.0;
  vec3 col = uCol * uSunCol * (0.35 + 0.9 * b) * sh;
  gl_FragColor = vec4(col, a * 0.9);
}`;
const SUN_FRAG = /* glsl */`
uniform vec3 uCol; uniform float uK, uTime; varying vec3 vN; varying vec3 vV;
${NOISE}
void main() {
  float mu = max(dot(normalize(vN), normalize(vV)), 0.0);
  float limb = 0.55 + 0.45 * pow(mu, 0.45);
  float g = fbm4(normalize(vN) * 9.0 + vec3(uTime * 0.05));
  gl_FragColor = vec4(uCol * uK * limb * (0.85 + 0.3 * g), 1.0);
}`;
const CORONA_FRAG = /* glsl */`
uniform vec3 uCol; uniform float uK, uTime, uFlare; varying vec2 vUv;
${NOISE}
void main() {
  vec2 p = vUv * 2.0 - 1.0; float r = length(p), a = atan(p.y, p.x);
  float core = exp(-r * 9.0) * 1.6;
  float str = fbm4(vec3(cos(a) * 2.2, sin(a) * 2.2, r * 2.0 - uTime * 0.04)) ;
  float streamers = pow(str, 3.0) * exp(-r * 3.4) * (1.6 + uFlare * 2.0);
  float halo = exp(-r * 4.0) * 0.4;
  gl_FragColor = vec4(uCol * (core + streamers + halo) * uK * smoothstep(1.0, 0.7, r), 1.0);
}`;
const GLARE_FRAG = /* glsl */`
uniform vec3 uCol; uniform float uK; varying vec2 vUv;
void main() { vec2 p = vUv * 2.0 - 1.0; float r = length(p);
  float g = pow(max(1.0 - r, 0.0), 3.0) * 0.6 + exp(-r * 18.0) * 2.0;
  float spikes = (exp(-abs(p.y) * 160.0) + exp(-abs(p.x) * 240.0) * 0.6) * pow(max(1.0 - r * 1.8, 0.0), 3.0) * 1.1;
  gl_FragColor = vec4(uCol * (g + spikes) * uK, 1.0); }`;
const BILL_VERT = /* glsl */`varying vec2 vUv; void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;
const SKY_VERT = /* glsl */`varying vec3 vDir; void main() { vDir = position; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;
const STAR_VERT = /* glsl */`
attribute float aSize; attribute vec3 aCol; uniform float uScale; varying vec3 vCol;
void main() { vec4 mv = modelViewMatrix * vec4(position, 1.0); gl_Position = projectionMatrix * mv; gl_PointSize = aSize * uScale; vCol = aCol; }`;
const STAR_FRAG = /* glsl */`
uniform float uBright; varying vec3 vCol;
void main() { vec2 c = gl_PointCoord - 0.5; float d = length(c); float a = smoothstep(0.5, 0.0, d); a *= a; gl_FragColor = vec4(vCol * a * uBright, 1.0); }`;

// ---- dust / speed lines ----------------------------------------------------------------------------------
const DUST_VERT = /* glsl */`
attribute float aEnd; uniform vec3 uCam, uVel; uniform float uBox, uStretch; varying float vA;
void main() {
  vec3 p = mod(position - uCam + uBox, 2.0 * uBox) - uBox;
  float d = length(p);
  p += uCam;
  vec3 tail = uVel * uStretch; float tl = length(tail); if (tl > 70.0) tail *= 70.0 / tl;
  if (aEnd > 0.5) p -= tail;
  vA = smoothstep(uBox, uBox * 0.55, d) * smoothstep(10.0, 30.0, d);
  gl_Position = projectionMatrix * viewMatrix * vec4(p, 1.0);
}`;
const DUST_FRAG = /* glsl */`uniform vec3 uCol; varying float vA; void main() { gl_FragColor = vec4(uCol * vA, 1.0); }`;

const TUNNEL_VERT = /* glsl */`varying vec2 vUv; varying float vZ; void main() { vUv = uv; vZ = position.y; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`;
const TUNNEL_FRAG = /* glsl */`
uniform float uT, uK; uniform vec3 uA, uB; varying vec2 vUv; varying float vZ;
float h(vec2 p) { return fract(sin(dot(p, vec2(41.3, 289.1))) * 43758.5453); }
void main() {
  float ang = vUv.x * 64.0, z = vUv.y * 9.0 - uT * 4.5;
  float lane = floor(ang); float f = fract(ang);
  float r = h(vec2(lane, floor(z * 0.5)));
  float streak = smoothstep(0.5, 0.0, abs(f - 0.5) * (2.0 + r * 6.0)) * smoothstep(0.0, 0.25, fract(z * 0.5 + r)) * smoothstep(1.0, 0.6, fract(z * 0.5 + r));
  vec3 col = mix(uA, uB, r) * (streak * 2.6 + 0.12) + vec3(1.0) * pow(streak, 6.0) * 2.0;
  float fade = smoothstep(0.0, 0.25, vUv.y) * smoothstep(1.0, 0.6, vUv.y);
  gl_FragColor = vec4(col * fade * uK, 1.0);
}`;

// ---------------------------------------------------------------------------------------------------------
export function createSpace(THREE, renderer, Q) {
  const far = new THREE.Scene(), near = new THREE.Scene();
  const pmrem = new THREE.PMREMGenerator(renderer);
  const disposables = [];
  const keep = (x) => { disposables.push(x); return x; };
  const sys = { group: new THREE.Group(), farGroup: new THREE.Group(), planets: [], station: null, beacon: null, rocks: [], lights: null, key: '' };
  near.add(sys.group); far.add(sys.farGroup);
  const sun = new THREE.DirectionalLight(0xffffff, 3); near.add(sun); near.add(sun.target);
  const bounce = new THREE.DirectionalLight(0x334455, 0.35); near.add(bounce); near.add(bounce.target);
  const amb = new THREE.AmbientLight(0x0a0c14, 0.4); near.add(amb);
  let skyRT = null, envRT = null, time = 0;
  const U = { time: { value: 0 } };

  // shared geometries
  const sphere = keep(new THREE.SphereGeometry(1, 96, 64));
  const quad = keep(new THREE.PlaneGeometry(2, 2));
  const rockMat = hullMaterial(THREE, { metal: 0.04, rough: 0.92, panelK: 0, env: 0.6, rock: true, nearFade: 60 });

  // ---- dust (speed lines) — persistent across systems --------------------------------------------------
  const ND = Q.dust;
  const dPos = new Float32Array(ND * 6), dEnd = new Float32Array(ND * 2);
  const dr = rng(99);
  for (let i = 0; i < ND; i++) { const x = dr.range(-140, 140), y = dr.range(-140, 140), z = dr.range(-140, 140); dPos.set([x, y, z, x, y, z], i * 6); dEnd[i * 2 + 1] = 1; }
  const dGeo = keep(new THREE.BufferGeometry()); dGeo.setAttribute('position', new THREE.BufferAttribute(dPos, 3)); dGeo.setAttribute('aEnd', new THREE.BufferAttribute(dEnd, 1));
  const dMat = new THREE.ShaderMaterial({ vertexShader: DUST_VERT, fragmentShader: DUST_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending,
    uniforms: { uCam: { value: new THREE.Vector3() }, uVel: { value: new THREE.Vector3() }, uBox: { value: 140 }, uStretch: { value: 0.08 }, uCol: { value: new THREE.Color(0.35, 0.38, 0.45) } } });
  const dust = new THREE.LineSegments(dGeo, dMat); dust.frustumCulled = false; near.add(dust);

  // ---- hyperspace tunnel (camera-attached) --------------------------------------------------------------
  const tGeo = keep(new THREE.CylinderGeometry(9, 9, 400, 64, 1, true));
  const tMat = new THREE.ShaderMaterial({ vertexShader: TUNNEL_VERT, fragmentShader: TUNNEL_FRAG, side: THREE.BackSide, transparent: true, depthWrite: false, depthTest: false, blending: THREE.AdditiveBlending,
    uniforms: { uT: { value: 0 }, uK: { value: 0 }, uA: { value: new THREE.Color(0.35, 0.85, 1.6) }, uB: { value: new THREE.Color(1.3, 0.45, 1.8) } } });
  const tunnel = new THREE.Mesh(tGeo, tMat); tunnel.rotation.x = Math.PI / 2; tunnel.position.z = -150; tunnel.visible = false; tunnel.renderOrder = 999; tunnel.frustumCulled = false;

  // ---- lens flare ghosts (camera-attached, positioned in camera space) ---------------------------------------
  const flareMat = (k) => new THREE.ShaderMaterial({ vertexShader: BILL_VERT, transparent: true, depthWrite: false, depthTest: false, blending: THREE.AdditiveBlending,
    uniforms: { uCol: { value: new THREE.Color() }, uK: { value: 0 }, uRing: { value: k } },
    fragmentShader: `uniform vec3 uCol; uniform float uK, uRing; varying vec2 vUv; void main() { float r = length(vUv * 2.0 - 1.0);
      float a = uRing > 0.5 ? smoothstep(0.08, 0.0, abs(r - 0.82)) * 0.8 + smoothstep(1.0, 0.0, r) * 0.05 : pow(max(1.0 - r, 0.0), 2.2);
      gl_FragColor = vec4(uCol * a * uK, 1.0); }` });
  const flares = new THREE.Group(); flares.renderOrder = 998;
  const GHOST = [[0.62, 0.03, 0, [0.6, 0.8, 1.0]], [0.35, 0.045, 0, [0.5, 1.0, 0.7]], [0.05, 0.02, 0, [1.0, 0.6, 0.4]], [-0.25, 0.06, 0, [0.6, 0.5, 1.0]], [-0.55, 0.035, 0, [0.8, 0.9, 1.0]], [-0.95, 0.09, 0, [0.5, 0.9, 1.0]]];
  for (const [k, s, ring, c] of GHOST) { const m = new THREE.Mesh(quad, flareMat(ring)); m.userData = { k, s, base: c }; m.material.uniforms.uCol.value.setRGB(c[0], c[1], c[2]); m.frustumCulled = false; m.renderOrder = 998; flares.add(m); }

  // ---- star (far) ---------------------------------------------------------------------------------------------
  const sunCore = new THREE.Mesh(sphere, new THREE.ShaderMaterial({ vertexShader: PLANET_VERT.replace('vN = normal;', 'vN = normal;'), fragmentShader: SUN_FRAG, uniforms: { uCol: { value: new THREE.Color() }, uK: { value: 30 }, uTime: U.time } }));
  sunCore.material.vertexShader = `varying vec3 vN; varying vec3 vV; void main() { vN = normalize(mat3(modelMatrix) * normal); vec4 w = modelMatrix * vec4(position, 1.0); vV = normalize(cameraPosition - w.xyz); gl_Position = projectionMatrix * viewMatrix * w; }`;
  const corona = new THREE.Mesh(quad, new THREE.ShaderMaterial({ vertexShader: BILL_VERT, fragmentShader: CORONA_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, uniforms: { uCol: { value: new THREE.Color() }, uK: { value: 3 }, uTime: U.time, uFlare: { value: 0.4 } } }));
  const glare = new THREE.Mesh(quad, new THREE.ShaderMaterial({ vertexShader: BILL_VERT, fragmentShader: GLARE_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, uniforms: { uCol: { value: new THREE.Color() }, uK: { value: 0.6 } } }));
  corona.renderOrder = 2; glare.renderOrder = 3;
  far.add(sunCore); far.add(corona); far.add(glare);

  // ---- starfield points (rebuilt per system) ----------------------------------------------------------------------
  let starPts = null;
  const starMat = new THREE.ShaderMaterial({ vertexShader: STAR_VERT, fragmentShader: STAR_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, uniforms: { uScale: { value: 1 }, uBright: { value: 1.4 } } });

  // ---- planet bake -------------------------------------------------------------------------------------------------
  const bakeCam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1), bakeScene = new THREE.Scene();
  const bakeMat = new THREE.ShaderMaterial({ vertexShader: `varying vec2 vUv; void main() { vUv = uv; gl_Position = vec4(position.xy, 0.0, 1.0); }`, fragmentShader: BAKE_FRAG,
    uniforms: { r0: { value: new THREE.Vector3() }, r1: { value: new THREE.Vector3() }, r2: { value: new THREE.Vector3() }, r3: { value: new THREE.Vector3() }, r4: { value: new THREE.Vector3() }, uOcean: { value: new THREE.Vector3() },
      uSea: { value: 0 }, uNoise: { value: 2 }, uBands: { value: 0 }, uType: { value: 0 }, uSeed: { value: 0 }, uLights: { value: 0 }, uAux: { value: 0 } } });
  const bakeQuad = new THREE.Mesh(quad, bakeMat); bakeQuad.frustumCulled = false; bakeScene.add(bakeQuad);
  const TYPE = { rocky: 0, desert: 1, ocean: 2, ice: 3, jungle: 4, volcanic: 5, gas: 6, crystal: 7 };
  function bakePlanet(pl, W) {
    const H = W / 2;
    const mk = () => { const rt = new THREE.WebGLRenderTarget(W, H, { minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter, generateMipmaps: false, wrapS: THREE.RepeatWrapping }); rt.texture.wrapS = THREE.RepeatWrapping; return rt; };
    const surf = mk(), aux = mk();
    const u = bakeMat.uniforms;
    pl.ramp.forEach((c, i) => { const l = lin3(c); u['r' + i].value.set(l[0], l[1], l[2]); });
    const oc = lin3(pl.oceanColor || [0.1, 0.2, 0.4]); u.uOcean.value.set(oc[0], oc[1], oc[2]);
    u.uSea.value = pl.ocean || 0; u.uNoise.value = pl.noise || 2.2; u.uBands.value = pl.bands || 0; u.uType.value = TYPE[pl.type] || 0;
    u.uSeed.value = (pl.seed % 1000) / 37; u.uLights.value = pl.lights || 0;
    const prev = renderer.getRenderTarget();
    u.uAux.value = 0; renderer.setRenderTarget(surf); renderer.render(bakeScene, bakeCam);
    u.uAux.value = 1; renderer.setRenderTarget(aux); renderer.render(bakeScene, bakeCam);
    renderer.setRenderTarget(prev);
    return { surf, aux };
  }

  // ---- system build / teardown ----------------------------------------------------------------------------------------
  function clearSystem() {
    for (const p of sys.planets) { p.group.removeFromParent(); p.rt.surf.dispose(); p.rt.aux.dispose(); p.mats.forEach((m) => m.dispose()); if (p.ringGeo) p.ringGeo.dispose(); }
    sys.planets.length = 0;
    for (const r of sys.rocks) { r.removeFromParent(); r.dispose(); }
    sys.rocks.length = 0;
    if (sys.station) { sys.station.removeFromParent(); sys.station.geometry.dispose(); sys.station = null; }
    if (sys.stationLights) { sys.stationLights.removeFromParent(); sys.stationLights.geometry.dispose(); sys.stationLights = null; }
    if (sys.beacon) { sys.beacon.removeFromParent(); sys.beacon.geometry.dispose(); sys.beacon = null; }
    if (sys.beaconGlow) { sys.beaconGlow.removeFromParent(); sys.beaconGlow = null; }
    if (starPts) { starPts.removeFromParent(); starPts.geometry.dispose(); starPts = null; }
    for (const n of sys.navs || []) n.removeFromParent();
    sys.navs = [];
  }

  function build(bp) {
    clearSystem();
    sys.key = bp.key; sys.bp = bp;
    // sky
    const size = Q.sky;
    if (!skyRT || skyRT.width !== size) { if (skyRT) skyRT.dispose(); skyRT = new THREE.WebGLCubeRenderTarget(size, { type: THREE.HalfFloatType, generateMipmaps: true, minFilter: THREE.LinearMipmapLinearFilter }); }
    const N = bp.nebula, la = lin3(N.a), lb = lin3(N.b), ld = lin3(N.dust);
    const skyMat = new THREE.ShaderMaterial({ vertexShader: SKY_VERT, fragmentShader: SKY_FRAG, side: THREE.BackSide, depthWrite: false,
      uniforms: { uA: { value: new THREE.Vector3(...la) }, uB: { value: new THREE.Vector3(...lb) }, uDust: { value: new THREE.Vector3(...ld) }, uAxis: { value: new THREE.Vector3(...N.axis) },
        uSeed: { value: new THREE.Vector3((N.seed % 97) / 7, (N.seed % 89) / 5, (N.seed % 83) / 3) }, uDensity: { value: N.density }, uWarp: { value: N.warp }, uScale: { value: N.scale }, uBand: { value: N.band }, uStars: { value: N.stars } } });
    const skyScene = new THREE.Scene(); skyScene.add(new THREE.Mesh(new THREE.BoxGeometry(10, 10, 10), skyMat));
    const cc = new THREE.CubeCamera(0.1, 100, skyRT); cc.update(renderer, skyScene);
    skyScene.children[0].geometry.dispose(); skyMat.dispose();
    far.background = skyRT.texture;
    if (envRT) envRT.dispose();
    envRT = pmrem.fromCubemap(skyRT.texture);
    near.environment = envRT.texture;
    // star
    const S = bp.star, sd = new THREE.Vector3(...S.dir), sc = lin3(S.color);
    const D = 60000, R = Math.tan(S.size * Math.PI / 180) * D;
    sunCore.position.copy(sd).multiplyScalar(D); sunCore.scale.setScalar(R);
    sunCore.material.uniforms.uCol.value.setRGB(sc[0], sc[1], sc[2]);
    corona.position.copy(sunCore.position); corona.scale.setScalar(R * 4.2 * S.corona); corona.material.uniforms.uCol.value.setRGB(sc[0], sc[1], sc[2]); corona.material.uniforms.uFlare.value = S.flare;
    glare.position.copy(sunCore.position).multiplyScalar(0.98); glare.scale.setScalar(R * 22); glare.material.uniforms.uCol.value.setRGB(sc[0] * 0.9 + 0.1, sc[1] * 0.9 + 0.1, sc[2] * 0.9 + 0.1);
    sun.color.setRGB(Math.min(1, sc[0] * 1.1 + 0.05), Math.min(1, sc[1] * 1.1 + 0.05), Math.min(1, sc[2] * 1.1 + 0.05));
    sun.intensity = S.lux; sun.position.copy(sd).multiplyScalar(1000); sun.target.position.set(0, 0, 0);
    sys.sunDir = sd.clone(); sys.sunCol = new THREE.Color(sc[0], sc[1], sc[2]);
    // stars
    const r = rng(N.seed ^ 0x5151), NS = Q.stars;
    const sp = new Float32Array(NS * 3), ss = new Float32Array(NS), scl = new Float32Array(NS * 3);
    const ax = new THREE.Vector3(...N.axis), v = new THREE.Vector3();
    for (let i = 0; i < NS; i++) {
      v.set(r.range(-1, 1), r.range(-1, 1), r.range(-1, 1)).normalize();
      if (r() < 0.45) { const k = v.dot(ax); v.addScaledVector(ax, -k * 0.85).normalize(); }
      sp[i * 3] = v.x * 9000; sp[i * 3 + 1] = v.y * 9000; sp[i * 3 + 2] = v.z * 9000;
      const m = Math.pow(r(), 9); ss[i] = 1.1 + m * 3.6;
      const kc = kelvinRGB(2800 + Math.pow(r(), 1.6) * 11000), br = 0.12 + m * 2.6;
      scl[i * 3] = kc[0] * br; scl[i * 3 + 1] = kc[1] * br; scl[i * 3 + 2] = kc[2] * br;
    }
    const sg = new THREE.BufferGeometry(); sg.setAttribute('position', new THREE.BufferAttribute(sp, 3)); sg.setAttribute('aSize', new THREE.BufferAttribute(ss, 1)); sg.setAttribute('aCol', new THREE.BufferAttribute(scl, 3));
    starPts = new THREE.Points(sg, starMat); starPts.frustumCulled = false; starPts.renderOrder = 1; sys.farGroup.add(starPts);
    // planets
    const PD = 40000;
    let bcol = [0.1, 0.12, 0.16];
    bp.planets.forEach((pl, idx) => {
      const g = new THREE.Group(); const pr = Math.tan(pl.angR * Math.PI / 180) * PD;
      const dir = new THREE.Vector3(...pl.dir).normalize();
      g.position.copy(dir).multiplyScalar(PD * (1 + idx * 0.02)); g.scale.setScalar(pr);
      const rt = bakePlanet(pl, pl.angR > 8 ? Q.planet : Math.max(256, Q.planet / 2));
      const rot = new THREE.Matrix3();
      const atmo = lin3(pl.atmoColor);
      const mPl = new THREE.ShaderMaterial({ vertexShader: PLANET_VERT, fragmentShader: PLANET_FRAG,
        uniforms: { uSurf: { value: rt.surf.texture }, uAux: { value: rt.aux.texture }, uSun: { value: sd }, uSunCol: { value: sys.sunCol }, uAtmo: { value: new THREE.Vector3(...atmo) },
          uLightCol: { value: new THREE.Vector3(1.0, 0.72, 0.4) }, uAtmoK: { value: pl.atmo }, uSea: { value: pl.ocean }, uType: { value: TYPE[pl.type] }, uBump: { value: pl.type === 'gas' ? 0 : 3.5 }, uRot: { value: rot } } });
      const body = new THREE.Mesh(sphere, mPl); body.rotation.z = pl.tilt; g.add(body);
      const mats = [mPl];
      let clouds = null;
      if (pl.clouds > 0.05) {
        const mC = new THREE.ShaderMaterial({ vertexShader: PLANET_VERT, fragmentShader: CLOUD_FRAG, transparent: true, depthWrite: false,
          uniforms: { uAux: { value: rt.aux.texture }, uSun: { value: sd }, uSunCol: { value: sys.sunCol }, uAtmo: { value: new THREE.Vector3(...atmo) }, uK: { value: Math.min(1.2, pl.clouds * 1.4) }, uTime: { value: 0 }, uRot: { value: rot } } });
        clouds = new THREE.Mesh(sphere, mC); clouds.scale.setScalar(1.012); clouds.rotation.z = pl.tilt; g.add(clouds); mats.push(mC);
      }
      if (pl.atmo > 0.1) {
        const mA = new THREE.ShaderMaterial({ vertexShader: PLANET_VERT, fragmentShader: ATMO_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending,
          uniforms: { uSun: { value: sd }, uAtmo: { value: new THREE.Vector3(...atmo) }, uK: { value: Math.min(1.4, pl.atmo * 1.3) } } });
        const shell = new THREE.Mesh(sphere, mA); shell.scale.setScalar(1.045); g.add(shell); mats.push(mA);
      }
      let ringGeo = null;
      if (pl.rings) {
        ringGeo = new THREE.RingGeometry(pl.rings.inner, pl.rings.outer, 160, 1);
        const rc = lin3(pl.rings.color);
        const mR = new THREE.ShaderMaterial({ transparent: true, depthWrite: false, side: THREE.DoubleSide,
          vertexShader: `varying vec3 vPos; varying vec3 vWP; void main() { vPos = position; vec4 w = modelMatrix * vec4(position, 1.0); vWP = w.xyz; gl_Position = projectionMatrix * viewMatrix * w; }`,
          fragmentShader: RING_FRAG, uniforms: { uSun: { value: sd }, uSunCol: { value: sys.sunCol }, uCol: { value: new THREE.Vector3(...rc) }, uCenter: { value: g.position }, uIn: { value: pl.rings.inner }, uOut: { value: pl.rings.outer }, uR: { value: pr }, uDens: { value: pl.rings.dens }, uSeed: { value: (pl.seed % 100) } } });
        const ring = new THREE.Mesh(ringGeo, mR); ring.rotation.x = Math.PI / 2 + pl.rings.tilt; ring.rotation.z = pl.tilt * 0.5; g.add(ring); mats.push(mR);
      }
      sys.farGroup.add(g);
      sys.planets.push({ group: g, body, clouds, rt, mats, pl, ringGeo, rot });
      if (idx === 0) { const s0 = pl.ramp[2]; bcol = [s0[0] * 0.5, s0[1] * 0.5, s0[2] * 0.5]; bounce.position.copy(dir).multiplyScalar(1000); }
    });
    bounce.color.setRGB(bcol[0] + 0.05, bcol[1] + 0.05, bcol[2] + 0.07); bounce.intensity = 0.55;
    amb.color.setRGB(la[0] * 0.3 + 0.02, la[1] * 0.3 + 0.02, la[2] * 0.3 + 0.03);
    // asteroid field: one InstancedMesh per base shape, sized and turned by the blueprint
    const F = bp.field, m4 = new THREE.Matrix4(), q = new THREE.Quaternion(), e = new THREE.Euler(), sv = new THREE.Vector3(), pv = new THREE.Vector3();
    const counts = [0, 0, 0, 0]; for (let i = 0; i < F.n; i++) counts[F.shape[i]]++;
    const extra = Q.rocksExtra, xr = rng(F.seed ^ 0x77);
    const exShape = new Uint8Array(extra); for (let i = 0; i < extra; i++) { exShape[i] = xr.int(4); counts[exShape[i]]++; }
    const fill = [0, 0, 0, 0], meshes = [];
    for (let s = 0; s < 4; s++) { const im = new THREE.InstancedMesh(rockGeometry(THREE, s, (F.seed % 7) + 1), rockMat, Math.max(1, counts[s])); im.count = counts[s]; im.frustumCulled = false; meshes.push(im); sys.group.add(im); sys.rocks.push(im); }
    const rr = rng(F.seed);
    const put = (s, x, y, z, rad) => { e.set(rr() * 6.28, rr() * 6.28, rr() * 6.28); q.setFromEuler(e); sv.setScalar(rad); pv.set(x, y, z); m4.compose(pv, q, sv); meshes[s].setMatrixAt(fill[s]++, m4); };
    for (let i = 0; i < F.n; i++) put(F.shape[i], F.x[i], F.y[i], F.z[i], F.r[i]);
    for (let i = 0; i < extra; i++) { const a = xr() * 6.28, d = Math.sqrt(xr()) * F.radius * 1.25; put(exShape[i], F.center[0] + Math.cos(a) * d, F.center[1] + xr.range(-1, 1) * F.radius * 0.22, F.center[2] + Math.sin(a) * d, 1.5 + Math.pow(xr(), 3) * 9); }
    for (const im of meshes) { im.instanceMatrix.needsUpdate = true; im.computeBoundingSphere(); }
    // station
    if (bp.station) {
      const st = stationGeometry(THREE, bp.station);
      const mat = hullMaterial(THREE, { metal: 0.55, rough: 0.5, panel: 0.12, env: 1.0 });
      sys.station = new THREE.Mesh(st.geo, mat); sys.station.position.set(...bp.station.pos); sys.group.add(sys.station);
      const lp = new Float32Array(st.lights.length * 3), lc = new Float32Array(st.lights.length * 3), lph = new Float32Array(st.lights.length);
      st.lights.forEach((l, i) => { lp.set([l[0], l[1], l[2]], i * 3); lc.set([l[3] * 3, l[4] * 3, l[5] * 3], i * 3); lph[i] = l[6] * 6.28; });
      const lg = new THREE.BufferGeometry(); lg.setAttribute('position', new THREE.BufferAttribute(lp, 3)); lg.setAttribute('aCol', new THREE.BufferAttribute(lc, 3)); lg.setAttribute('aPh', new THREE.BufferAttribute(lph, 1));
      sys.stationLights = new THREE.Points(lg, blinkMat); sys.stationLights.position.copy(sys.station.position); sys.stationLights.frustumCulled = false; sys.group.add(sys.stationLights);
    }
    if (bp.beacon) {
      const bc = beaconGeometry(THREE, bp.beacon);
      const mat = hullMaterial(THREE, { metal: 0.7, rough: 0.35, panel: 0.2, env: 1.2 });
      sys.beacon = new THREE.Mesh(bc.geo, mat); sys.beacon.position.set(...bp.beacon.pos); sys.group.add(sys.beacon);
      const gm = new THREE.Mesh(quad, new THREE.ShaderMaterial({ vertexShader: BILL_VERT, fragmentShader: GLARE_FRAG, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending,
        uniforms: { uCol: { value: bp.beacon.lit ? new THREE.Color(1.0, 0.75, 0.35) : new THREE.Color(0.35, 0.75, 1.0) }, uK: { value: bp.beacon.lit ? 1.6 : 0.45 } } }));
      gm.position.set(bp.beacon.pos[0] + bc.core[0], bp.beacon.pos[1] + bc.core[1], bp.beacon.pos[2] + bc.core[2]); gm.scale.setScalar(bp.beacon.lit ? 240 : 120);
      sys.beaconGlow = gm; sys.group.add(gm);
    }
  }
  const blinkMat = new THREE.ShaderMaterial({ transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, uniforms: { uTime: U.time, uScale: { value: 1 } },
    vertexShader: `attribute vec3 aCol; attribute float aPh; uniform float uTime, uScale; varying vec3 vC;
      void main() { vec4 mv = modelViewMatrix * vec4(position, 1.0); gl_Position = projectionMatrix * mv; float b = 0.5 + 0.5 * sin(uTime * 2.2 + aPh); b = pow(b, 6.0);
        vC = aCol * (0.15 + b); gl_PointSize = clamp(2600.0 * uScale / -mv.z, 2.0, 40.0) * (0.6 + b * 0.6); }`,
    fragmentShader: `varying vec3 vC; void main() { float d = length(gl_PointCoord - 0.5); float a = pow(smoothstep(0.5, 0.0, d), 2.0); gl_FragColor = vec4(vC * a, 1.0); }` });

  // ---- nav markers (patrol waypoints / convoy jump point) ---------------------------------------------------------------
  const navGeo = keep(new THREE.TorusGeometry(60, 1.6, 6, 64));
  const navMat = new THREE.MeshBasicMaterial({ color: new THREE.Color(1.0, 0.75, 0.25), transparent: true, opacity: 0.55, blending: THREE.AdditiveBlending, depthWrite: false });
  function setNavs(list, gate) {
    for (const n of sys.navs || []) n.removeFromParent();
    sys.navs = [];
    (list || []).forEach((p, i) => { const m = new THREE.Mesh(navGeo, navMat); m.position.set(p[0], p[1], p[2]); m.userData.i = i; sys.group.add(m); sys.navs.push(m); const m2 = new THREE.Mesh(navGeo, navMat); m2.scale.setScalar(0.6); m.add(m2); });
    if (gate) { const g = new THREE.Mesh(navGeo, navMat); g.position.set(gate[0], gate[1], gate[2]); g.scale.setScalar(4); g.userData.gate = true; sys.group.add(g); sys.navs.push(g); }
  }

  // ---- per-frame --------------------------------------------------------------------------------------------------------------
  const _v = new THREE.Vector3(), _c = new THREE.Vector3(), _q = new THREE.Quaternion();
  function update(dt, cam, camVel, opts = {}) {
    time += dt; U.time.value = time;
    // billboards face the camera
    corona.quaternion.copy(cam.quaternion); glare.quaternion.copy(cam.quaternion);
    if (sys.beaconGlow) sys.beaconGlow.quaternion.copy(cam.quaternion);
    for (const p of sys.planets) {
      p.body.rotation.y += dt * p.pl.rot * 0.25;
      p.rot.setFromMatrix4(p.body.matrixWorld);   // object normal -> world for lighting
      if (p.clouds) { p.clouds.rotation.y = p.body.rotation.y; p.clouds.material.uniforms.uTime.value = time * p.pl.rot * 0.08; p.clouds.material.uniforms.uRot.value = p.rot; }
    }
    for (const n of sys.navs || []) { n.rotation.y += dt * (n.userData.gate ? 0.15 : 0.6); n.lookAt(cam.position); n.rotateZ(time * 0.4); }
    // dust
    const u = dMat.uniforms; u.uCam.value.copy(cam.position); u.uVel.value.copy(camVel); u.uStretch.value = opts.stretch != null ? opts.stretch : 0.06;
    dMat.uniforms.uCol.value.setScalar(opts.dust != null ? opts.dust : 0.32);
    starMat.uniforms.uScale.value = opts.pxScale || 1; blinkMat.uniforms.uScale.value = opts.pxScale || 1;
    // lens flares from the star's screen position
    if (!sys.sunDir) return;
    _v.copy(sys.sunDir).applyQuaternion(_q.copy(cam.quaternion).invert());
    const inFront = _v.z < 0;
    const fl = flares.children;
    if (inFront) {
      const tanH = Math.tan(cam.fov * Math.PI / 360), sx = (_v.x / -_v.z) / (tanH * cam.aspect), sy = (_v.y / -_v.z) / tanH;
      const edge = Math.max(Math.abs(sx), Math.abs(sy));
      let k = (1 - Math.min(1, Math.max(0, (edge - 0.6) / 0.6))) * (opts.flare != null ? opts.flare : 1);
      for (const pl of sys.planets) { _c.copy(pl.group.position).normalize(); const ang = Math.acos(Math.min(1, _c.dot(sys.sunDir))); if (ang < Math.atan(pl.group.scale.x / pl.group.position.length())) k = 0; }
      for (const m of fl) {
        const f = m.userData.k, x = sx * f, y = sy * f;
        m.position.set(x * tanH * cam.aspect, y * tanH, -1); m.scale.setScalar(m.userData.s * (1 + Math.abs(f) * 0.2));
        m.material.uniforms.uK.value = k * 0.22; m.visible = k > 0.01;
      }
    } else for (const m of fl) m.visible = false;
  }

  function dispose() {
    clearSystem();
    for (const d of disposables) d.dispose();
    if (skyRT) skyRT.dispose(); if (envRT) envRT.dispose(); pmrem.dispose();
    dMat.dispose(); tMat.dispose(); starMat.dispose(); blinkMat.dispose(); navMat.dispose(); rockMat.dispose();
    sunCore.material.dispose(); corona.material.dispose(); glare.material.dispose();
    for (const m of flares.children) m.material.dispose();
  }
  return { far, near, sys, sun, build, update, setNavs, tunnel, tunnelMat: tMat, flares, dust, dispose, U };
}

function kelvinRGB(k) {
  const t = k / 100; let r, g, b;
  if (t <= 66) { r = 255; g = 99.47 * Math.log(t) - 161.12; b = t <= 19 ? 0 : 138.52 * Math.log(t - 10) - 305.04; }
  else { r = 329.7 * Math.pow(t - 60, -0.1332); g = 288.12 * Math.pow(t - 60, -0.0755); b = 255; }
  const c = (v) => Math.pow(Math.max(0, Math.min(255, v)) / 255, 2.2);
  return [c(r), c(g), c(b)];
}

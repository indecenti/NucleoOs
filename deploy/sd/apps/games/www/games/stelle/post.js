// stelle/post.js — the post chain of the medium / high tiers, built for integrated GPUs.
//
//   scene (far pass, then near pass, into ONE multisampled half-float target, resolved once)
//     → bloom: a bright-pass downsample to half resolution, a dual-filter (Kawase) pyramid down to 1/32 and back up
//       with additive tent upsamples (each level a few cheap taps on a small target)
//     → the volume pass (a world's clouds, stelle/terrain.js): half resolution, reading the scene's resolved depth
//     → sun shafts (when the sun is on screen in an atmosphere): a radial blur of the bloom's bright level toward the sun
//     → one final full-screen pass: scene + clouds (a depth-aware upsample) + bloom + shafts, heat shimmer over hot
//       ground, the grade (vignette, aberration, flash, damage, heat, fade, grain), the camera's tone mapping and the
//       sRGB output — straight to the canvas.
// The old chain (EffectComposer + UnrealBloom + a grade ShaderPass + OutputPass) wrote every pass into multisampled
// targets and resolved each of them; on the Arc 140T this one costs about a third of it.
// The grade's uniforms keep the names the renderer already drives (grade.uniforms.uFlash, ...).

const QUAD_VERT = /* glsl */`varying vec2 vUv; void main() { vUv = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }`;
// bright pass + first downsample: four bilinear taps (a 4x4 texel average: no fireflies), soft knee above the
// threshold, then compress what feeds the blur (a 30x sun counts like a ~3x lamp) so highlights glow tight
const BRIGHT_FRAG = /* glsl */`uniform sampler2D tSrc, tVol; uniform vec2 uTexel; uniform float uThr, uVol; varying vec2 vUv;
vec3 tap(vec2 o) { return min(max(texture2D(tSrc, vUv + o * uTexel).rgb, vec3(0.0)), vec3(64.0)); }
void main() {
  vec3 c = (tap(vec2(-1.0, -1.0)) + tap(vec2(1.0, -1.0)) + tap(vec2(-1.0, 1.0)) + tap(vec2(1.0, 1.0))) * 0.25;
  if (uVol > 0.5) { vec4 v = texture2D(tVol, vUv); c = c * v.a + v.rgb; }
  float l = dot(c, vec3(0.2126, 0.7152, 0.0722)), knee = 0.45, x = clamp(l - uThr + knee, 0.0, 2.0 * knee);
  c *= max(x * x / (4.0 * knee + 1e-4), l - uThr) / max(l, 1e-4);
  float lc = dot(c, vec3(0.2126, 0.7152, 0.0722)); c *= 2.6 / (2.6 + lc);
  gl_FragColor = vec4(c, 1.0);
}`;
const DOWN_FRAG = /* glsl */`uniform sampler2D tSrc; uniform vec2 uTexel; varying vec2 vUv;
void main() {
  vec2 h = uTexel * 0.5;
  vec3 s = texture2D(tSrc, vUv).rgb * 4.0 + texture2D(tSrc, vUv - h).rgb + texture2D(tSrc, vUv + h).rgb
         + texture2D(tSrc, vUv + vec2(h.x, -h.y)).rgb + texture2D(tSrc, vUv - vec2(h.x, -h.y)).rgb;
  gl_FragColor = vec4(s * 0.125, 1.0);
}`;
const UP_FRAG = /* glsl */`uniform sampler2D tSrc; uniform vec2 uTexel; uniform float uW; varying vec2 vUv;
void main() {
  vec2 h = uTexel * 0.5;
  vec3 s = texture2D(tSrc, vUv + vec2(-h.x * 2.0, 0.0)).rgb + texture2D(tSrc, vUv + vec2(h.x * 2.0, 0.0)).rgb
         + texture2D(tSrc, vUv + vec2(0.0, -h.y * 2.0)).rgb + texture2D(tSrc, vUv + vec2(0.0, h.y * 2.0)).rgb
         + (texture2D(tSrc, vUv + vec2(-h.x, h.y)).rgb + texture2D(tSrc, vUv + vec2(h.x, h.y)).rgb
          + texture2D(tSrc, vUv + vec2(h.x, -h.y)).rgb + texture2D(tSrc, vUv + vec2(-h.x, -h.y)).rgb) * 2.0;
  gl_FragColor = vec4(s * (uW / 12.0), 1.0);
}`;
// sun shafts: march from the pixel toward the sun's screen position over the bright level (only the sky's brightest
// light gets through: clouds, ridges and trees cut it into rays)
const SHAFT_FRAG = /* glsl */`uniform sampler2D tSrc; uniform vec2 uSun; uniform float uK, uAspect; varying vec2 vUv;
void main() {
  vec2 d = (uSun - vUv); float L = length(d * vec2(uAspect, 1.0));
  vec2 st = d / 24.0 * min(1.0, 0.9 / max(L, 1e-3)); vec2 p = vUv; float w = 1.0; vec3 s = vec3(0.0);
  float j = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
  p += st * j;
  for (int i = 0; i < 24; i++) { s += texture2D(tSrc, p).rgb * w; w *= 0.94; p += st; }
  gl_FragColor = vec4(s * (uK / 9.0) * smoothstep(1.4, 0.2, L), 1.0);
}`;
// the clouds' half-resolution result smoothed once (a 3x3 tent that does not cross a depth edge): the march's
// per-pixel jitter becomes a soft grain instead of a pattern
const VBLUR_FRAG = /* glsl */`uniform sampler2D tVol, tDepth; uniform vec2 uTexel, uNF; varying vec2 vUv;
float czLin(float d) { return uNF.x * uNF.y / max(uNF.y - d * (uNF.y - uNF.x), 1e-3); }
void main() {
  float zc = czLin(texture2D(tDepth, vUv).r); vec4 acc = vec4(0.0); float ws = 0.0;
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 q = vUv + vec2(float(x), float(y)) * uTexel;
    float w = (x == 0 && y == 0 ? 4.0 : x == 0 || y == 0 ? 2.0 : 1.0) / (0.02 + abs(czLin(texture2D(tDepth, q).r) - zc) / max(zc, 1.0) * 40.0);
    acc += texture2D(tVol, q) * w; ws += w;
  }
  gl_FragColor = acc / ws;
}`;
const FINAL_FRAG = /* glsl */`uniform sampler2D tScene, tBloom, tShaft, tVol, tDepth, tNoise; uniform float uBloom, uShaft, uVol, uShim;
uniform float uVig, uAber, uFlash, uDmg, uSat, uTime, uFade, uHeat, uWater; uniform vec3 uFlashCol, uShaftCol; uniform vec2 uVolTexel, uNF; varying vec2 vUv;
float czLin(float d) { return uNF.x * uNF.y / max(uNF.y - d * (uNF.y - uNF.x), 1e-3); }   // perspective depth -> view distance
// the half-resolution clouds brought up to full: the four nearest texels, weighted by how close their depth is to this
// pixel's (no halo of sky round a ridge or a hull)
vec4 czVolUp(vec2 uv, float zc) {
  vec2 p = uv / uVolTexel - 0.5, f = fract(p), b = (floor(p) + 0.5) * uVolTexel;
  vec4 acc = vec4(0.0); float ws = 0.0;
  for (int i = 0; i < 4; i++) {
    vec2 o = vec2(float(i - (i / 2) * 2), float(i / 2)), q = b + o * uVolTexel;
    float zq = czLin(texture2D(tDepth, q).r), wb = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y);
    float w = wb / (0.02 + abs(zq - zc) / max(zc, 1.0) * 40.0);
    acc += texture2D(tVol, q) * w; ws += w;
  }
  return acc / max(ws, 1e-5);
}
void main() {
  vec2 c = vUv - 0.5; float r = length(c);
  vec2 uv = vUv; float zc = uNF.y;
  if (uVol > 0.5 || uShim > 0.0) zc = czLin(texture2D(tDepth, vUv).r);
  // heat shimmer: the air over hot ground wobbles the view of whatever lies a few hundred metres off (not the sky)
  if (uShim > 0.0) {
    float k = uShim * smoothstep(90.0, 700.0, zc) * (1.0 - smoothstep(5000.0, 15000.0, zc));
    vec2 n = vec2(texture2D(tNoise, vUv * vec2(2.3, 7.0) + vec2(0.0, uTime * 0.21)).r, texture2D(tNoise, vUv * vec2(3.1, 9.0) - vec2(uTime * 0.05, uTime * 0.33)).a) - 0.5;
    uv += n * k * vec2(0.0016, 0.0032);
  }
  vec2 off = c * uAber * r;
  vec3 col = uAber > 0.0025 ? vec3(texture2D(tScene, uv + off).r, texture2D(tScene, uv).g, texture2D(tScene, uv - off).b) : texture2D(tScene, uv).rgb;
  col = min(max(col, vec3(0.0)), vec3(64.0));
  if (uVol > 0.5) { vec4 v = czVolUp(vUv, zc); col = col * v.a + v.rgb; }
  col += texture2D(tBloom, vUv).rgb * uBloom;
  if (uShaft > 0.0) col += texture2D(tShaft, vUv).rgb * uShaftCol * uShaft;
  if (uWater > 0.0) { col = mix(col, col * vec3(0.25, 0.62, 0.7) + vec3(0.0, 0.02, 0.03), uWater); }   // under the sea surface
  float l = dot(col, vec3(0.2126, 0.7152, 0.0722)); col = mix(vec3(l), col, uSat);
  col += vec3(0.8, 0.04, 0.02) * uDmg * smoothstep(0.32, 0.78, r);
  col += vec3(1.0, 0.42, 0.12) * uHeat * smoothstep(0.22, 0.85, r) * (0.8 + 0.2 * sin(uTime * 37.0 + vUv.y * 40.0));
  col *= 1.0 - uVig * smoothstep(0.42, 0.98, r);
  col += uFlashCol * uFlash;
  col *= 1.0 - uFade;
  col += (fract(sin(dot(vUv * 913.0 + uTime, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) * 0.01;
  col = min(max(col, vec3(0.0)), vec3(64.0));
  gl_FragColor = vec4(col, 1.0);
  #include <tonemapping_fragment>
  #include <colorspace_fragment>
}`;

export function createPost(THREE, renderer, Q) {
  const LEVELS = 5;
  const HF = { type: THREE.HalfFloatType, depthBuffer: false, minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter, generateMipmaps: false };
  const scene = new THREE.Scene(), cam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
  const quad = new THREE.Mesh(new THREE.PlaneGeometry(2, 2)); quad.frustumCulled = false; scene.add(quad);
  const mat = (frag, u, add = false) => new THREE.ShaderMaterial({ vertexShader: QUAD_VERT, fragmentShader: frag, uniforms: u, depthTest: false, depthWrite: false,
    transparent: add, blending: add ? THREE.AdditiveBlending : THREE.NoBlending, toneMapped: false });
  const bright = mat(BRIGHT_FRAG, { tSrc: { value: null }, tVol: { value: null }, uVol: { value: 0 }, uTexel: { value: new THREE.Vector2() }, uThr: { value: 1.0 } });
  const down = mat(DOWN_FRAG, { tSrc: { value: null }, uTexel: { value: new THREE.Vector2() } });
  const up = mat(UP_FRAG, { tSrc: { value: null }, uTexel: { value: new THREE.Vector2() }, uW: { value: 1 } }, true);
  const vblur = mat(VBLUR_FRAG, { tVol: { value: null }, tDepth: { value: null }, uTexel: { value: new THREE.Vector2() }, uNF: { value: new THREE.Vector2(0.4, 4e5) } });
  const shaft = mat(SHAFT_FRAG, { tSrc: { value: null }, uSun: { value: new THREE.Vector2(0.5, 0.5) }, uK: { value: 1 }, uAspect: { value: 1.6 } });
  const GU = { tScene: { value: null }, tBloom: { value: null }, tShaft: { value: null }, uBloom: { value: 0.5 }, uShaft: { value: 0 }, uShaftCol: { value: new THREE.Color(1, 0.9, 0.75) }, uWater: { value: 0 },
    tVol: { value: null }, tDepth: { value: null }, tNoise: { value: null }, uVol: { value: 0 }, uShim: { value: 0 }, uVolTexel: { value: new THREE.Vector2(1, 1) }, uNF: { value: new THREE.Vector2(0.4, 400000) },
    uVig: { value: 0.55 }, uAber: { value: 0.0 }, uFlash: { value: 0 }, uFlashCol: { value: new THREE.Color(1, 1, 1) }, uDmg: { value: 0 }, uSat: { value: 1 }, uTime: { value: 0 }, uFade: { value: 0 }, uHeat: { value: 0 } };
  const final = new THREE.ShaderMaterial({ vertexShader: QUAD_VERT, fragmentShader: FINAL_FRAG, uniforms: GU, depthTest: false, depthWrite: false });
  let sceneRT = null, lv = [], shaftRT = null, volRT = null, volRT2 = null, W = 0, H = 0;
  // the volume pass: set by the renderer to { on, draw(depthTexture, target, camera) } (a world's clouds); shimmer 0..1
  const vol = { on: false, draw: null }, fx = { shimmer: 0, noise: null };
  const bloom = { enabled: true, strength: 0.5 };   // what the debug hooks see (r3d.bloom)
  const sun = { on: false, x: 0.5, y: 0.5, k: 0 };
  function setSize(w, h) {
    w = Math.max(2, w | 0); h = Math.max(2, h | 0);
    if (w === W && h === H && sceneRT) return;
    dispose(true); W = w; H = h;
    sceneRT = new THREE.WebGLRenderTarget(w, h, { type: THREE.HalfFloatType, samples: Q.msaa || 0, minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter, generateMipmaps: false });
    // its depth is resolved with the colour (once per frame): the clouds and the shimmer read it
    sceneRT.depthTexture = new THREE.DepthTexture(w, h, THREE.UnsignedIntType);
    volRT = new THREE.WebGLRenderTarget(Math.max(1, Math.ceil(w / 2)), Math.max(1, Math.ceil(h / 2)), HF); volRT2 = volRT.clone();
    lv = [];
    for (let i = 0, lw = Math.ceil(w / 2), lh = Math.ceil(h / 2); i < LEVELS; i++, lw = Math.max(1, Math.ceil(lw / 2)), lh = Math.max(1, Math.ceil(lh / 2))) lv.push(new THREE.WebGLRenderTarget(lw, lh, HF));
    shaftRT = new THREE.WebGLRenderTarget(Math.max(1, Math.ceil(w / 4)), Math.max(1, Math.ceil(h / 4)), HF);
  }
  function pass(m, target) { quad.material = m; renderer.setRenderTarget(target); renderer.render(scene, cam); }
  // a = far scene / camera (drawn first, cleared), b = near scene / camera (over a cleared depth; null to skip)
  function render(sA, cA, sB, cB) {
    const auto = renderer.autoClear;
    renderer.autoClear = false;
    renderer.setRenderTarget(sceneRT); renderer.clear(true, true, true);
    // one resolve per frame: three resolves a multisampled target after every render() call, so the far pass is drawn
    // with the target's sample count hidden (its framebuffer is already bound) and only the near pass resolves it
    const ns = sceneRT.samples; if (sB && ns) sceneRT.samples = 0;
    try { renderer.render(sA, cA); } finally { sceneRT.samples = ns; }
    if (sB) { renderer.clearDepth(); renderer.render(sB, cB); }
    // the world's clouds, half resolution, over the resolved depth
    const vOn = !!(sB && vol.on && vol.draw && vol.draw(sceneRT.depthTexture, volRT, cB));
    if (cB) GU.uNF.value.set(cB.near, cB.far);
    if (vOn) { const u = vblur.uniforms; u.tVol.value = volRT.texture; u.tDepth.value = sceneRT.depthTexture; u.uTexel.value.set(1 / volRT.width, 1 / volRT.height); u.uNF.value.copy(GU.uNF.value); pass(vblur, volRT2); }
    GU.uVol.value = bright.uniforms.uVol.value = vOn ? 1 : 0; GU.tVol.value = bright.uniforms.tVol.value = volRT2.texture;
    GU.tDepth.value = sceneRT.depthTexture; GU.uVolTexel.value.set(1 / volRT.width, 1 / volRT.height);
    GU.uShim.value = sB && fx.noise ? fx.shimmer : 0; GU.tNoise.value = fx.noise;
    // bloom pyramid
    let src = sceneRT.texture;
    if (bloom.enabled) {
      bright.uniforms.tSrc.value = src; bright.uniforms.uTexel.value.set(1 / W, 1 / H); pass(bright, lv[0]);
      for (let i = 1; i < LEVELS; i++) { down.uniforms.tSrc.value = lv[i - 1].texture; down.uniforms.uTexel.value.set(1 / lv[i - 1].width, 1 / lv[i - 1].height); pass(down, lv[i]); }
      for (let i = LEVELS - 2; i >= 0; i--) { up.uniforms.tSrc.value = lv[i + 1].texture; up.uniforms.uTexel.value.set(1 / lv[i + 1].width, 1 / lv[i + 1].height); up.uniforms.uW.value = i === 0 ? 0.9 : 0.8; pass(up, lv[i]); }
    }
    GU.uShaft.value = 0;
    if (sun.on && sun.k > 0.01 && bloom.enabled) {
      shaft.uniforms.tSrc.value = lv[1].texture; shaft.uniforms.uSun.value.set(sun.x, sun.y); shaft.uniforms.uK.value = 1; shaft.uniforms.uAspect.value = W / H;
      pass(shaft, shaftRT); GU.uShaft.value = sun.k;
    }
    GU.tScene.value = sceneRT.texture; GU.tBloom.value = lv[0].texture; GU.tShaft.value = shaftRT.texture; GU.uBloom.value = bloom.enabled ? bloom.strength : 0;
    pass(final, null);
    renderer.autoClear = auto;
  }
  function dispose(keepMats) {
    if (sceneRT) { if (sceneRT.depthTexture) sceneRT.depthTexture.dispose(); sceneRT.dispose(); } for (const t of lv) t.dispose(); if (shaftRT) shaftRT.dispose(); if (volRT) { volRT.dispose(); volRT2.dispose(); }
    sceneRT = null; lv = []; shaftRT = null; volRT = volRT2 = null; W = H = 0;
    if (!keepMats) { for (const m of [bright, down, up, vblur, shaft, final]) m.dispose(); quad.geometry.dispose(); }
  }
  return { setSize, render, dispose: () => dispose(false), grade: { uniforms: GU }, bloom, sun, vol, fx, get target() { return sceneRT; } };
}

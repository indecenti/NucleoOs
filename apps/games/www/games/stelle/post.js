// stelle/post.js — the post chain of the medium / high tiers, built for integrated GPUs.
//
//   scene (far pass, then near pass, into ONE multisampled half-float target, resolved once)
//     → bloom: a bright-pass downsample to half resolution, a dual-filter (Kawase) pyramid down to 1/32 and back up
//       with additive tent upsamples (each level a few cheap taps on a small target)
//     → sun shafts (when the sun is on screen in an atmosphere): a radial blur of the bloom's bright level toward the sun
//     → one final full-screen pass: scene + bloom + shafts, the grade (vignette, aberration, flash, damage, heat, fade,
//       grain), the camera's tone mapping and the sRGB output — straight to the canvas.
// The old chain (EffectComposer + UnrealBloom + a grade ShaderPass + OutputPass) wrote every pass into multisampled
// targets and resolved each of them; on the Arc 140T this one costs about a third of it.
// The grade's uniforms keep the names the renderer already drives (grade.uniforms.uFlash, ...).

const QUAD_VERT = /* glsl */`varying vec2 vUv; void main() { vUv = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }`;
// bright pass + first downsample: four bilinear taps (a 4x4 texel average: no fireflies), soft knee above the
// threshold, then compress what feeds the blur (a 30x sun counts like a ~3x lamp) so highlights glow tight
const BRIGHT_FRAG = /* glsl */`uniform sampler2D tSrc; uniform vec2 uTexel; uniform float uThr; varying vec2 vUv;
vec3 tap(vec2 o) { return min(max(texture2D(tSrc, vUv + o * uTexel).rgb, vec3(0.0)), vec3(64.0)); }
void main() {
  vec3 c = (tap(vec2(-1.0, -1.0)) + tap(vec2(1.0, -1.0)) + tap(vec2(-1.0, 1.0)) + tap(vec2(1.0, 1.0))) * 0.25;
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
const FINAL_FRAG = /* glsl */`uniform sampler2D tScene, tBloom, tShaft; uniform float uBloom, uShaft;
uniform float uVig, uAber, uFlash, uDmg, uSat, uTime, uFade, uHeat, uWater; uniform vec3 uFlashCol, uShaftCol; varying vec2 vUv;
void main() {
  vec2 c = vUv - 0.5; float r = length(c); vec2 off = c * uAber * r;
  vec3 col = uAber > 0.0025 ? vec3(texture2D(tScene, vUv + off).r, texture2D(tScene, vUv).g, texture2D(tScene, vUv - off).b) : texture2D(tScene, vUv).rgb;
  col = min(max(col, vec3(0.0)), vec3(64.0));
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
  const bright = mat(BRIGHT_FRAG, { tSrc: { value: null }, uTexel: { value: new THREE.Vector2() }, uThr: { value: 1.0 } });
  const down = mat(DOWN_FRAG, { tSrc: { value: null }, uTexel: { value: new THREE.Vector2() } });
  const up = mat(UP_FRAG, { tSrc: { value: null }, uTexel: { value: new THREE.Vector2() }, uW: { value: 1 } }, true);
  const shaft = mat(SHAFT_FRAG, { tSrc: { value: null }, uSun: { value: new THREE.Vector2(0.5, 0.5) }, uK: { value: 1 }, uAspect: { value: 1.6 } });
  const GU = { tScene: { value: null }, tBloom: { value: null }, tShaft: { value: null }, uBloom: { value: 0.5 }, uShaft: { value: 0 }, uShaftCol: { value: new THREE.Color(1, 0.9, 0.75) }, uWater: { value: 0 },
    uVig: { value: 0.55 }, uAber: { value: 0.0 }, uFlash: { value: 0 }, uFlashCol: { value: new THREE.Color(1, 1, 1) }, uDmg: { value: 0 }, uSat: { value: 1 }, uTime: { value: 0 }, uFade: { value: 0 }, uHeat: { value: 0 } };
  const final = new THREE.ShaderMaterial({ vertexShader: QUAD_VERT, fragmentShader: FINAL_FRAG, uniforms: GU, depthTest: false, depthWrite: false });
  let sceneRT = null, lv = [], shaftRT = null, W = 0, H = 0;
  const bloom = { enabled: true, strength: 0.5 };   // what the debug hooks see (r3d.bloom)
  const sun = { on: false, x: 0.5, y: 0.5, k: 0 };
  function setSize(w, h) {
    w = Math.max(2, w | 0); h = Math.max(2, h | 0);
    if (w === W && h === H && sceneRT) return;
    dispose(true); W = w; H = h;
    sceneRT = new THREE.WebGLRenderTarget(w, h, { type: THREE.HalfFloatType, samples: Q.msaa || 0, minFilter: THREE.LinearFilter, magFilter: THREE.LinearFilter, generateMipmaps: false });
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
    if (sceneRT) sceneRT.dispose(); for (const t of lv) t.dispose(); if (shaftRT) shaftRT.dispose();
    sceneRT = null; lv = []; shaftRT = null; W = H = 0;
    if (!keepMats) { for (const m of [bright, down, up, shaft, final]) m.dispose(); quad.geometry.dispose(); }
  }
  return { setSize, render, dispose: () => dispose(false), grade: { uniforms: GU }, bloom, sun, get target() { return sceneRT; } };
}

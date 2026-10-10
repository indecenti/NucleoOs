// stelle/matgen.js — the ground materials of the worlds, generated once on the GPU into a texture array.
//
// Ten tileable layers (512² on medium / high, 256² on low), each texel = (normal x, normal y, height, tone):
//   0 rock (cracked, chipped)   1 sand (wind ripples)   2 grass / moss (clumps)   3 gravel (pebbles and stones)
//   4 snow (sastrugi)           5 ash crust (plates)    6 ice (fractures)         7 crystal (facets)
//   8 dirt / mud                9 sandstone (blocky joints)
// Colour is not stored: the terrain shader paints each layer with two colours of the world's own palette mixed by the
// tone, so one set of layers serves every world. Normals come from the height (central differences) and are
// mipmapped with it, so far ground flattens instead of sparkling. Generation is a handful of full-screen passes (no
// CPU noise, no worker round-trip; compiled off the main thread); the result is shared by every world of the session.
export const MAT = { rock: 0, sand: 1, grass: 2, gravel: 3, snow: 4, ash: 5, ice: 6, crystal: 7, dirt: 8, strata: 9 };
export const MAT_LAYERS = 10;

const GEN_FRAG = /* glsl */`
varying vec2 vUv;
float h21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
// periodic gradient noise (period P cells on each axis)
float pn(vec2 p, vec2 P) {
  vec2 i = floor(p), f = fract(p), u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  vec2 a = mod(i, P), b = mod(i + 1.0, P);
  float ga = h21(a + 0.31) * 6.2831853, gb = h21(vec2(b.x, a.y) + 0.31) * 6.2831853, gc = h21(vec2(a.x, b.y) + 0.31) * 6.2831853, gd = h21(b + 0.31) * 6.2831853;
  float va = dot(vec2(cos(ga), sin(ga)), f), vb = dot(vec2(cos(gb), sin(gb)), f - vec2(1.0, 0.0));
  float vc = dot(vec2(cos(gc), sin(gc)), f - vec2(0.0, 1.0)), vd = dot(vec2(cos(gd), sin(gd)), f - vec2(1.0, 1.0));
  return mix(mix(va, vb, u.x), mix(vc, vd, u.x), u.y) * 1.414;
}
float pf(vec2 uv, float P, int n, float g) {
  float s = 0.0, a = 1.0, nn = 0.0;
  for (int i = 0; i < 7; i++) { if (i >= n) break; s += a * pn(uv * P + float(i) * 7.31, vec2(P)); nn += a; a *= g; P *= 2.0; }
  return s / nn;
}
// periodic Worley: x = F1, y = F2, z = cell hash; w2 = offset to the nearest point
vec2 W2;
vec3 pw(vec2 uv, float P) {
  vec2 p = uv * P, i = floor(p), f = fract(p); float F1 = 9.0, F2 = 9.0, id = 0.0;
  for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {
    vec2 o = vec2(float(x), float(y)), c = mod(i + o, P);
    vec2 j = vec2(h21(c + 1.7), h21(c + 9.3));
    vec2 d = o + 0.08 + j * 0.84 - f; float dd = dot(d, d);
    if (dd < F1) { F2 = F1; F1 = dd; id = h21(c + 3.7); W2 = d; } else if (dd < F2) F2 = dd;
  }
  return vec3(sqrt(F1), sqrt(F2), id);
}
// one layer: height h (0..1) and tone t (0..1)
vec2 layer(vec2 uv) {
  float h = 0.5, t = 0.5;
#if LAYER == 0
  {            // rock: ridged mass, a crack network, chips
    float r = 1.0 - abs(pf(uv, 4.0, 5, 0.55)); r *= r;
    vec3 w = pw(uv, 7.0); float crack = smoothstep(0.0, 0.07, w.y - w.x);
    vec3 w2 = pw(uv + 0.37, 19.0); float chip = smoothstep(0.0, 0.05, w2.y - w2.x);
    h = (0.42 * r + 0.3 * (pf(uv, 16.0, 4, 0.5) * 0.5 + 0.5) + 0.2 * w.z) * (0.55 + 0.45 * crack) * (0.8 + 0.2 * chip);
    t = clamp(0.5 + 0.7 * pf(uv, 3.0, 3, 0.5) + 0.3 * (w.z - 0.5), 0.0, 1.0) * (0.65 + 0.35 * crack);
  }
#elif LAYER == 1
  {     // sand: wind ripples (22 per tile, meandering) and grain
    float warp = pf(uv, 3.0, 3, 0.5);
    float f = fract((uv.y + warp * 0.07 + pf(uv, 6.0, 2, 0.5) * 0.02) * 22.0);
    float rip = f < 0.72 ? smoothstep(0.0, 0.72, f) : 1.0 - smoothstep(0.72, 1.0, f);
    h = 0.3 + 0.45 * rip * (0.65 + 0.35 * (pf(uv, 5.0, 2, 0.5) * 0.5 + 0.5)) + 0.06 * pf(uv, 128.0, 2, 0.5);
    t = clamp(0.5 + 0.4 * pf(uv, 4.0, 3, 0.5) + 0.12 * rip + 0.3 * (h21(floor(uv * 512.0)) - 0.5), 0.0, 1.0);
  }
#elif LAYER == 2
  {     // grass / moss: clumps, tufts, blades
    float clump = pf(uv, 6.0, 4, 0.5) * 0.5 + 0.5;
    float blades = pf(uv, 96.0, 2, 0.5) * 0.5 + 0.5;
    vec3 w = pw(uv, 24.0); float tuft = smoothstep(0.45, 0.1, w.x);
    h = 0.25 + 0.3 * clump + 0.3 * blades * smoothstep(0.25, 0.7, clump) + 0.15 * tuft;
    t = clamp(clump * 0.75 + 0.35 * (w.z - 0.5) + 0.2 * blades - 0.1, 0.0, 1.0);
  }
#elif LAYER == 3
  {     // gravel: stones and pebbles over grit
    vec3 w = pw(uv, 12.0); float a = w.z;
    vec3 v = pw(uv + 0.5, 31.0);
    float st = smoothstep(0.55, 0.06, w.x) * (0.65 + 0.35 * a), st2 = smoothstep(0.5, 0.05, v.x) * 0.55;
    h = max(st, st2) + 0.08 * (pf(uv, 64.0, 2, 0.5) * 0.5 + 0.5);
    t = st > st2 ? a : v.z * 0.8;
  }
#elif LAYER == 4
  {     // snow: soft drifts, sastrugi carved by the wind
    float s1 = pf(uv, 3.0, 4, 0.5);
    float sas = 1.0 - abs(pn(vec2(uv.x * 5.0, uv.y * 18.0 + s1 * 3.0), vec2(5.0, 18.0)));
    h = 0.45 + 0.22 * s1 + 0.25 * sas * sas + 0.04 * pf(uv, 64.0, 2, 0.5);
    t = clamp(0.6 + 0.3 * s1 + 0.1 * sas, 0.0, 1.0);
  }
#elif LAYER == 5
  {     // ash crust: plates split by cracks, fine ash on top
    vec3 w = pw(uv, 6.0); float edge = smoothstep(0.0, 0.09, w.y - w.x);
    vec3 w2 = pw(uv + 0.21, 17.0); float e2 = smoothstep(0.0, 0.06, w2.y - w2.x);
    h = (0.3 + 0.45 * edge * (0.75 + 0.25 * w.z) + 0.15 * (pf(uv, 32.0, 3, 0.5) * 0.5 + 0.5)) * (0.85 + 0.15 * e2);
    t = clamp(0.25 + 0.5 * w.z + 0.25 * (pf(uv, 12.0, 3, 0.5) * 0.5 + 0.5), 0.0, 1.0) * edge;
  }
#elif LAYER == 6
  {     // ice: smooth, fractures, trapped bubbles
    vec3 w = pw(uv, 5.0); float fr = 1.0 - smoothstep(0.0, 0.03, w.y - w.x);
    vec3 b = pw(uv + 0.6, 40.0); float bub = smoothstep(0.12, 0.02, b.x) * step(0.8, b.z);
    h = 0.5 + 0.18 * pf(uv, 4.0, 3, 0.5) + 0.12 * fr - 0.08 * bub;
    t = clamp(0.35 + 0.55 * fr + 0.2 * pf(uv, 9.0, 2, 0.5) + 0.4 * bub, 0.0, 1.0);
  }
#elif LAYER == 7
  {     // crystal: planar facets per cell, bright edges
    vec3 w = pw(uv, 7.0); vec2 dd = W2; float a = w.z * 6.2831853;
    float edge = smoothstep(0.0, 0.05, w.y - w.x);
    h = clamp(0.62 - w.x * 0.7 + 0.35 * dot(vec2(cos(a), sin(a)), dd), 0.0, 1.0) * (0.7 + 0.3 * edge);
    t = mix(1.0, 0.15 + 0.6 * w.z, edge);
  }
#elif LAYER == 8
  {     // dirt / mud: soft lumps, a few pebbles, dark damp patches
    h = 0.45 + 0.3 * pf(uv, 5.0, 5, 0.55);
    vec3 w = pw(uv, 28.0); h += 0.2 * smoothstep(0.4, 0.05, w.x) * step(0.65, w.z);
    t = clamp(0.5 + 0.5 * pf(uv, 3.0, 3, 0.5), 0.0, 1.0);
  }
#else
  {                              // weathered sandstone: blocky joints, spalled faces, soft grain (the bedding itself comes
                                 // from the elevation bands of the terrain shader, so it always lies level)
    vec3 w = pw(uv, 5.0); float joint = smoothstep(0.0, 0.05, w.y - w.x);
    vec3 w2 = pw(uv + 0.23, 13.0); float spall = smoothstep(0.0, 0.04, w2.y - w2.x);
    float g = pf(uv, 8.0, 4, 0.55);
    h = (0.45 + 0.25 * w.z + 0.2 * g) * (0.62 + 0.38 * joint) * (0.86 + 0.14 * spall);
    t = clamp(0.45 + 0.35 * (w.z - 0.5) + 0.3 * g + 0.15 * (w2.z - 0.5), 0.0, 1.0) * (0.7 + 0.3 * joint);
  }
#endif
  return vec2(clamp(h, 0.0, 1.0), clamp(t, 0.0, 1.0));
}
void main() { vec2 c = layer(vUv); gl_FragColor = vec4(c.x, c.y, 0.0, 1.0); }
`;

// the normals: central differences of the height pass (half float, wrapped), strength per layer
const NRM_FRAG = /* glsl */`precision highp sampler2DArray;
uniform sampler2DArray tH; uniform float uL, uTexel, uK; varying vec2 vUv;
void main() {
  vec4 c = texture(tH, vec3(vUv, uL));
  float hx = texture(tH, vec3(vUv + vec2(uTexel, 0.0), uL)).r, hy = texture(tH, vec3(vUv + vec2(0.0, uTexel), uL)).r;
  vec3 n = normalize(vec3(-(hx - c.r) * uK / (uTexel * 64.0), -(hy - c.r) * uK / (uTexel * 64.0), 1.0));
  gl_FragColor = vec4(n.xy * 0.5 + 0.5, c.r, c.g);
}`;
const QUAD_VERT = 'varying vec2 vUv; void main() { vUv = position.xy * 0.5 + 0.5; gl_Position = vec4(position.xy, 0.0, 1.0); }';

// Returns { tex, ready }: tex is a neutral 1x1 placeholder until the layers exist, then the sampler2DArray of the layers
// (onReady(tex) is called then). The eleven small programs (one per layer, one for the normals) are compiled with
// renderer.compileAsync — in parallel off the main thread where the browser offers KHR_parallel_shader_compile — so
// entering the first world never waits on the shader compiler; then the passes take a few milliseconds of GPU.
export function makeMaterials(THREE, renderer, size = 512, onReady = null) {
  const ph = new THREE.DataArrayTexture(new Uint8Array(4 * MAT_LAYERS).fill(128), 1, 1, MAT_LAYERS); ph.needsUpdate = true;
  const res = { tex: ph, ready: false, dead: false, rt: null, dispose() { res.dead = true; ph.dispose(); if (res.rt) res.rt.dispose(); } };
  const geo = new THREE.PlaneGeometry(2, 2), scene = new THREE.Scene(), cam = new THREE.OrthographicCamera(-1, 1, 1, -1, 0, 1);
  const flat = { depthTest: false, depthWrite: false, toneMapped: false };
  const gens = [];
  for (let L = 0; L < MAT_LAYERS; L++) gens.push(new THREE.ShaderMaterial({ defines: { LAYER: L }, vertexShader: QUAD_VERT, fragmentShader: GEN_FRAG, ...flat }));
  const hRT = new THREE.WebGLArrayRenderTarget(size, size, MAT_LAYERS, { depthBuffer: false, type: THREE.HalfFloatType, format: THREE.RGBAFormat });
  hRT.texture.wrapS = hRT.texture.wrapT = THREE.RepeatWrapping; hRT.texture.minFilter = hRT.texture.magFilter = THREE.NearestFilter; hRT.texture.generateMipmaps = false;
  const nrm = new THREE.ShaderMaterial({ vertexShader: QUAD_VERT, fragmentShader: NRM_FRAG, uniforms: { tH: { value: hRT.texture }, uL: { value: 0 }, uTexel: { value: 1 / size }, uK: { value: 5 } }, ...flat });
  const quads = [...gens, nrm].map((m) => { const q = new THREE.Mesh(geo, m); q.frustumCulled = false; scene.add(q); return q; });
  const cleanup = () => { for (const m of gens) m.dispose(); nrm.dispose(); geo.dispose(); hRT.dispose(); };
  // compiled for the targets they draw into (linear output: an off-screen target)
  const prev0 = renderer.getRenderTarget(); renderer.setRenderTarget(hRT, 0);
  const ready = renderer.compileAsync(scene, cam); renderer.setRenderTarget(prev0);
  ready.then(() => {
    if (res.dead) { cleanup(); return; }
    const rt = new THREE.WebGLArrayRenderTarget(size, size, MAT_LAYERS, { depthBuffer: false, type: THREE.UnsignedByteType, format: THREE.RGBAFormat });
    const tex = rt.texture;
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping; tex.magFilter = THREE.LinearFilter; tex.minFilter = THREE.LinearMipmapLinearFilter; tex.generateMipmaps = false; tex.anisotropy = 8;
    // one layer per animation frame (height, then its normals): no frame waits on more than one pass of the GPU
    let L = 0;
    const step = () => {
      if (res.dead) { cleanup(); rt.dispose(); return; }
      const prev = renderer.getRenderTarget(), auto2 = renderer.autoClear; renderer.autoClear = false;
      for (const q of quads) q.visible = false;
      quads[L].visible = true; renderer.setRenderTarget(hRT, L); renderer.render(scene, cam); quads[L].visible = false;
      quads[MAT_LAYERS].visible = true; nrm.uniforms.uL.value = L; nrm.uniforms.uK.value = L === MAT.snow ? 3 : L === MAT.ice ? 2.5 : 5;   // snow and ice: gentler normals
      renderer.setRenderTarget(rt, L); renderer.render(scene, cam);
      renderer.setRenderTarget(prev); renderer.autoClear = auto2;
      if (++L < MAT_LAYERS) { requestAnimationFrame(step); return; }
      finish();
    };
    const finish = () => {
      // mipmaps: three r160 does not build them for array render targets — one raw call, then hand the state back
      try {
        const gl = renderer.getContext(), wt = renderer.properties.get(tex).__webglTexture;
        gl.bindTexture(gl.TEXTURE_2D_ARRAY, wt); gl.texParameteri(gl.TEXTURE_2D_ARRAY, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
        gl.generateMipmap(gl.TEXTURE_2D_ARRAY); gl.bindTexture(gl.TEXTURE_2D_ARRAY, null);
        renderer.resetState();
      } catch (e) { console.warn('[world] material mipmaps', e); }
      cleanup();
      res.rt = rt; res.tex = tex; res.ready = true;
      if (onReady) onReady(tex);
    };
    requestAnimationFrame(step);
  }).catch((e) => console.warn('[world] material layers', e));
  return res;
}

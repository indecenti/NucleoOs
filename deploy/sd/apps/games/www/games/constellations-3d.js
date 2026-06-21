// constellations-3d.js — Three.js renderer for "Costellazioni 3D".
//
//   const r = await createRenderer(canvas, api);
//   r.frame(state, ctx, api);   // ctx = { run, sector, missions, MT_NAME }
//   r.resize(w, h); r.dispose();
//
// Driven by state.phase: the hub family ('loading'/'new_run'/'conflict'/'hub') shows a slow 3D attract
// scene (drifting starfield + spinning planet) behind the DOM hub UI (makeUI in constellations-ui.js),
// and 'combat' is the first-person rail dogfight (warp starfield, enemy ships that grow as they near,
// twin converging lasers, a green lock box, shield/hull HUD); 'debrief' freezes the field for the
// after-action card. Combat geometry uses the SAME project() as the sim (constellations.js) so the lock
// always matches what you see. Falls back to a pretty 2D canvas if WebGL/Three is unavailable.
import { project, FOCAL } from '/apps/games/games/constellations.js';
import { makeUI } from '/apps/games/games/constellations-ui.js';
import { sfx } from '/apps/games/games/constellations-sfx.js';

const FAC_COL = [0x4d7bd0, 0x28aa96, 0xc06434, 0xa882e6];   // Guild / Keepers / Wrecks / Echo
const KIND_COL = [0xff5c50, 0x60cee8, 0xe6963c, 0xffbe40];  // fighter/scout/heavy/ace tint
// enemy threat ramp (advancing -> more menacing): cyan(harmless) -> gold/white-hot(lethal). Reads on additive halo/engine.
const THREAT_RAMP = [0x37e0ff, 0x3a9bff, 0x6a6dff, 0x9b5cff, 0xe24bd6, 0xff3b6b, 0xff7a2f, 0xffd25a];

export async function createRenderer(canvas, api) {
  let gl = null;
  try { gl = canvas.getContext('webgl2', { antialias: true, alpha: false }) || canvas.getContext('webgl', { antialias: true, alpha: false }); } catch {}
  let THREE = null;
  if (gl) { try { THREE = await import('/apps/games/vendor/three.module.min.js'); } catch (e) { console.warn('three.js unavailable -> 2D', e); } }
  const hud = makeUI(canvas);
  return THREE ? build3D(THREE, canvas, gl, hud) : build2D(canvas, hud);
}

// ============================ WebGL renderer ====================================================
function build3D(THREE, canvas, gl, hud) {
  const renderer = new THREE.WebGLRenderer({ canvas, context: gl, antialias: true });
  renderer.setPixelRatio(Math.min(2, window.devicePixelRatio || 1));
  renderer.outputColorSpace = THREE.SRGBColorSpace; renderer.toneMapping = THREE.ACESFilmicToneMapping; renderer.toneMappingExposure = 1.15;
  const scene = new THREE.Scene(); scene.background = new THREE.Color(0x05060f); scene.fog = new THREE.Fog(0x070a18, 14, 44);   // far, atmospheric: veils only the starfield, never the ships
  let asp = 1.78;
  const cam = new THREE.OrthographicCamera(-asp, asp, 1, -1, -10, 10); cam.position.z = 5;
  const ambient = new THREE.AmbientLight(0x3a4a68, 0.8); scene.add(ambient);                                  // lift the dark sides so models read
  const key = new THREE.DirectionalLight(0xeaf2ff, 1.9); key.position.set(3, 4, 5); scene.add(key);           // strong key sculpts the 3D hull
  const fill = new THREE.DirectionalLight(0xff9a70, 0.4); fill.position.set(-4, -1, 2); scene.add(fill);      // gentle warm fill
  const rim = new THREE.DirectionalLight(0x8fe0ff, 0.95); rim.position.set(0, -2, -5); scene.add(rim);        // cool rim lifts the silhouette off the black

  const glowTex = makeGlow(THREE);
  const sprite = (color, s, op = 1) => { const m = new THREE.Sprite(new THREE.SpriteMaterial({ map: glowTex, color, blending: THREE.AdditiveBlending, transparent: true, depthWrite: false, opacity: op })); m.scale.setScalar(s); return m; };

  // warp starfield — vector STREAK lines (Star Wars arcade): each star is a segment from its current
  // screen point back toward the vanishing point; the length grows with speed + proximity, so they fan
  // out as diagonal hyperspace lines that sell the 3D depth. project() is the shared perspective.
  const NS = 130, stars = []; for (let i = 0; i < NS; i++) stars.push(rndStar());
  function rndStar() { return { ex: (Math.random() * 2 - 1) * 1.7, ey: (Math.random() * 2 - 1) * 1.15, ez: 6 + Math.random() * 254 }; }
  const starGeo = new THREE.BufferGeometry(); const starPos = new Float32Array(NS * 6); starGeo.setAttribute('position', new THREE.BufferAttribute(starPos, 3));   // 2 verts (head+tail) per star
  const starLines = new THREE.LineSegments(starGeo, new THREE.LineBasicMaterial({ color: 0xbcd6ff, transparent: true, opacity: 0.9, blending: THREE.AdditiveBlending, depthWrite: false })); scene.add(starLines);

  // evolving nebula — large soft additive clouds that drift, breathe and shift hue over time (a living sky)
  const nebula = []; const tmpc = new THREE.Color();
  for (let i = 0; i < 4; i++) {
    const s = sprite(0x4466aa, 1.4, 0.1); s.material.blending = THREE.AdditiveBlending; s.renderOrder = -2;
    const ox = (Math.random() * 2 - 1) * 1.7, oy = (Math.random() * 2 - 1) * 1.1;
    s.position.set(ox, oy, -0.6); scene.add(s);
    nebula.push({ s, ox, oy, ph: Math.random() * 6.28, hue: Math.random(), sz: 1.0 + Math.random() * 1.1 });
  }
  let tt = 0;   // renderer wall-clock for ambient animation

  // ===== BIOME COLOR SYSTEM — the background changes per SECTOR (not per wave) with a smooth cross-fade,
  // and warms base->hot toward the boss wave. 8 arcade biomes; all bg below L~8% so ships/lasers pop.
  const BIOMES = [
    { name: 'Nebulosa Cremisi', base: { bg: 0x140509, fog: 0x2a0810, fogN: 14, fogF: 44, nebHue: 0.98, nebSat: 0.62, nebLight: 0.40, nebOp: 0.16, star: 0xffc9b0, starOp: 0.90, amb: 0x4a2030, key: 0xffe6e0, fill: 0xff7a55, rim: 0xff5fa0 }, hot: { bg: 0x1c060c, fog: 0x3a0a16, fogN: 11, fogF: 38, nebHue: 0.96, nebSat: 0.80, nebLight: 0.52, nebOp: 0.28, star: 0xffe0d0, starOp: 1.00, amb: 0x661e34, key: 0xfff0ea, fill: 0xff5a3a, rim: 0xff4a8a } },
    { name: 'Vuoto Glaciale', base: { bg: 0x040a12, fog: 0x0a1c2e, fogN: 15, fogF: 46, nebHue: 0.55, nebSat: 0.42, nebLight: 0.48, nebOp: 0.14, star: 0xeaf6ff, starOp: 0.92, amb: 0x2a4258, key: 0xf2fbff, fill: 0x6fb0d8, rim: 0x9fe8ff }, hot: { bg: 0x080612, fog: 0x140a26, fogN: 11, fogF: 38, nebHue: 0.74, nebSat: 0.62, nebLight: 0.52, nebOp: 0.24, star: 0xe0d6ff, starOp: 1.00, amb: 0x3a3060, key: 0xf0eaff, fill: 0x9a8aff, rim: 0xb0a0ff } },
    { name: 'Tempesta Ionica', base: { bg: 0x0d0518, fog: 0x1c0a32, fogN: 14, fogF: 44, nebHue: 0.74, nebSat: 0.62, nebLight: 0.44, nebOp: 0.17, star: 0xd9b6ff, starOp: 0.90, amb: 0x3a2858, key: 0xf0e6ff, fill: 0xb070ff, rim: 0x7a6cff }, hot: { bg: 0x14041e, fog: 0x26083a, fogN: 10, fogF: 36, nebHue: 0.82, nebSat: 0.82, nebLight: 0.52, nebOp: 0.28, star: 0xf0c0ff, starOp: 1.00, amb: 0x52286a, key: 0xf8e0ff, fill: 0xd060ff, rim: 0x9a6cff } },
    { name: 'Alba Dorata', base: { bg: 0x120a03, fog: 0x2c1606, fogN: 13, fogF: 42, nebHue: 0.09, nebSat: 0.60, nebLight: 0.46, nebOp: 0.18, star: 0xffe2a8, starOp: 0.90, amb: 0x4a3318, key: 0xfff2d6, fill: 0xffb24d, rim: 0xffd166 }, hot: { bg: 0x1a0c02, fog: 0x381606, fogN: 10, fogF: 35, nebHue: 0.05, nebSat: 0.84, nebLight: 0.52, nebOp: 0.28, star: 0xfff0c8, starOp: 1.00, amb: 0x6a4418, key: 0xfff6e0, fill: 0xff9020, rim: 0xffb840 } },
    { name: 'Abisso Smeraldo', base: { bg: 0x03120c, fog: 0x07281a, fogN: 14, fogF: 44, nebHue: 0.41, nebSat: 0.55, nebLight: 0.40, nebOp: 0.16, star: 0xc6ffe0, starOp: 0.88, amb: 0x204838, key: 0xeafff4, fill: 0x4fd99a, rim: 0x6cffc0 }, hot: { bg: 0x0c1604, fog: 0x16280a, fogN: 11, fogF: 38, nebHue: 0.28, nebSat: 0.78, nebLight: 0.52, nebOp: 0.27, star: 0xeaffc0, starOp: 1.00, amb: 0x3a5a26, key: 0xf2ffe0, fill: 0xb0ff50, rim: 0x8ce080 } },
    { name: 'Campo di Brace', base: { bg: 0x150700, fog: 0x301000, fogN: 13, fogF: 40, nebHue: 0.045, nebSat: 0.70, nebLight: 0.42, nebOp: 0.20, star: 0xffd0a0, starOp: 0.88, amb: 0x4a2810, key: 0xfff0e0, fill: 0xff8a3c, rim: 0xff5a2a }, hot: { bg: 0x1c0600, fog: 0x3a0e00, fogN: 9, fogF: 33, nebHue: 0.00, nebSat: 0.88, nebLight: 0.52, nebOp: 0.32, star: 0xfff0d0, starOp: 1.00, amb: 0x6a2a10, key: 0xfff4e0, fill: 0xff5a20, rim: 0xff3a18 } },
    { name: 'Distesa Zaffiro', base: { bg: 0x05060f, fog: 0x070a18, fogN: 14, fogF: 44, nebHue: 0.62, nebSat: 0.56, nebLight: 0.45, nebOp: 0.16, star: 0xbcd6ff, starOp: 0.90, amb: 0x3a4a68, key: 0xeaf2ff, fill: 0xff9a70, rim: 0x8fe0ff }, hot: { bg: 0x0a0814, fog: 0x0e0a20, fogN: 11, fogF: 38, nebHue: 0.72, nebSat: 0.74, nebLight: 0.50, nebOp: 0.26, star: 0xd6e0ff, starOp: 1.00, amb: 0x4a3a62, key: 0xf0eeff, fill: 0xff7048, rim: 0xa0c0ff } },
    { name: 'Risacca Aurora', base: { bg: 0x06101a, fog: 0x0c1e2c, fogN: 14, fogF: 44, nebHue: 0.49, nebSat: 0.52, nebLight: 0.45, nebOp: 0.17, star: 0xb8f0ff, starOp: 0.90, amb: 0x244a54, key: 0xeafcff, fill: 0xff6fb0, rim: 0x52ffd0 }, hot: { bg: 0x0a0820, fog: 0x121a30, fogN: 10, fogF: 37, nebHue: 0.58, nebSat: 0.74, nebLight: 0.52, nebOp: 0.27, star: 0xd0eaff, starOp: 1.00, amb: 0x343a64, key: 0xf0eeff, fill: 0xff50a0, rim: 0x60ffc0 } },
  ];
  const _bt = new THREE.Color(), _ba = new THREE.Color(), _bb = new THREE.Color();
  const lerpHexC = (c, hex, k) => { _bt.setHex(hex); c.lerp(_bt, k); };
  const mixHex = (a, b, t) => { _ba.setHex(a); _bb.setHex(b); return _ba.lerp(_bb, t).getHex(); };
  const bhash32 = (x) => { x = Math.imul((x >>> 16) ^ x, 0x45d9f3b); x = Math.imul((x >>> 16) ^ x, 0x45d9f3b); x = (x >>> 16) ^ x; return x >>> 0; };
  const biomeFor = (sector, epoch) => BIOMES[bhash32((Math.imul(sector | 0, 2654435761) + Math.imul(epoch | 0, 40503)) >>> 0) % BIOMES.length];
  const cur = { bg: new THREE.Color(0x05060f), fog: new THREE.Color(0x070a18), fogN: 14, fogF: 44, nebHue: 0.62, nebSat: 0.56, nebLight: 0.45, nebOp: 0.16, star: new THREE.Color(0xbcd6ff), starOp: 0.90, amb: new THREE.Color(0x3a4a68), key: new THREE.Color(0xeaf2ff), fill: new THREE.Color(0xff9a70), rim: new THREE.Color(0x8fe0ff) };
  let curSector = -999, whoosh = 0;
  function updateBiome(state, ctx, dt) {
    const sector = ctx ? (ctx.sector | 0) : 0, epoch = (ctx && ctx.run) ? (ctx.run.epoch | 0) : 0;
    if (sector !== curSector) { if (curSector !== -999) whoosh = 1; curSector = sector; }
    const B = biomeFor(sector, epoch);
    let t = 0;
    if (state && state.cc && state.phase === 'combat') { t = Math.min(1, Math.max(0, (state.wave - 1) / Math.max(1, state.cc.waves - 1))); if (state.wave >= state.cc.waves) t = Math.min(1, t + 0.25); }
    const a = B.base, h = B.hot, lin = (x, y) => x + (y - x) * t;
    const tBg = mixHex(a.bg, h.bg, t), tFog = mixHex(a.fog, h.fog, t), tStar = mixHex(a.star, h.star, t), tAmb = mixHex(a.amb, h.amb, t), tKey = mixHex(a.key, h.key, t), tFill = mixHex(a.fill, h.fill, t), tRim = mixHex(a.rim, h.rim, t);
    const tFogN = lin(a.fogN, h.fogN), tFogF = lin(a.fogF, h.fogF), tNebSat = lin(a.nebSat, h.nebSat), tNebLight = lin(a.nebLight, h.nebLight), tNebOp = lin(a.nebOp, h.nebOp), tStarOp = lin(a.starOp, h.starOp);
    let tNebHue; { let dh = h.nebHue - a.nebHue; if (dh > 0.5) dh -= 1; if (dh < -0.5) dh += 1; tNebHue = (a.nebHue + dh * t + 1) % 1; }
    const k = 1 - Math.exp(-(0.9 + whoosh * 1.6) * dt);
    lerpHexC(cur.bg, tBg, k); lerpHexC(cur.fog, tFog, k); lerpHexC(cur.star, tStar, k); lerpHexC(cur.amb, tAmb, k); lerpHexC(cur.key, tKey, k); lerpHexC(cur.fill, tFill, k); lerpHexC(cur.rim, tRim, k);
    cur.fogN += (tFogN - cur.fogN) * k; cur.fogF += (tFogF - cur.fogF) * k;
    let dh = tNebHue - cur.nebHue; if (dh > 0.5) dh -= 1; if (dh < -0.5) dh += 1; cur.nebHue = (cur.nebHue + dh * k + 1) % 1;
    cur.nebSat += (tNebSat - cur.nebSat) * k; cur.nebLight += (tNebLight - cur.nebLight) * k; cur.nebOp += (tNebOp - cur.nebOp) * k; cur.starOp += (tStarOp - cur.starOp) * k;
    if (whoosh > 0) whoosh = Math.max(0, whoosh - dt * 0.6);
    scene.background.copy(cur.bg);
    scene.fog.color.copy(cur.fog); scene.fog.near = cur.fogN; scene.fog.far = cur.fogF;
    starLines.material.color.copy(cur.star); starLines.material.opacity = cur.starOp + whoosh * 0.9;
    ambient.color.copy(cur.amb); key.color.copy(cur.key); fill.color.copy(cur.fill); rim.color.copy(cur.rim);
  }
  // enemy threat color: sector dominates, wave refines; lerped over ~0.4s in drawCombat so it never snaps.
  const _trA = new THREE.Color(), _trB = new THREE.Color(), _trR = new THREE.Color();
  function threatColor(sector, wave, waves) {
    sector = sector | 0; wave = wave || 1; waves = waves || 6;
    const wf = waves > 1 ? (wave - 1) / (waves - 1) : 1, bias = ((bhash32(Math.imul(sector, 2654435761) >>> 0) & 255) / 255 - 0.5) * 0.12;
    const tv = Math.max(0, Math.min(1, sector * 0.18 + wf * 0.34 + bias)), n = THREAT_RAMP.length - 1, x = tv * n, i = Math.min(n - 1, Math.floor(x)), fr = x - i;
    _trA.setHex(THREAT_RAMP[i]); _trB.setHex(THREAT_RAMP[i + 1]); return _trA.lerp(_trB, fr).getHex();
  }

  // foe pool — real low-poly SPACESHIPS built from primitives (fuselage + swept wings + fins), one
  // distinct hull per kind. They fly nose-toward-you and BANK into their turns (no drunk Z-spin).
  // Shared part geometries (one set, reused across all foes; per-foe material gives the colour).
  const SG = {
    fus: new THREE.ConeGeometry(0.05, 0.34, 6), fusA: new THREE.ConeGeometry(0.058, 0.42, 6), fusS: new THREE.ConeGeometry(0.042, 0.24, 4),
    wing: new THREE.BoxGeometry(1, 0.014, 0.11), hull: new THREE.BoxGeometry(0.12, 0.10, 0.30), pod: new THREE.BoxGeometry(0.055, 0.06, 0.18),
    fin: new THREE.BoxGeometry(0.018, 0.11, 0.12), nose: new THREE.ConeGeometry(0.06, 0.18, 4), cockpit: new THREE.SphereGeometry(0.03, 8, 6),
  };
  function primitiveShip(kind, mat) {
    const s = new THREE.Group();
    const add = (geo, p, r, sc) => { const m = new THREE.Mesh(geo, mat); if (p) m.position.set(p[0], p[1], p[2]); if (r) m.rotation.set(r[0], r[1], r[2]); if (sc) m.scale.set(sc[0], sc[1], sc[2]); s.add(m); return m; };
    if (kind === 2) { add(SG.hull); add(SG.pod, [-0.11, 0, -0.02]); add(SG.pod, [0.11, 0, -0.02]); add(SG.fin, [0, 0.07, -0.05], [0, 0, 0], [1.4, 1, 1]); add(SG.nose, [0, 0, 0.20], [Math.PI / 2, 0, 0]); }
    else { const fus = kind === 3 ? SG.fusA : kind === 1 ? SG.fusS : SG.fus; add(fus, [0, 0, 0.02], [Math.PI / 2, 0, 0]); const span = kind === 3 ? 0.30 : kind === 1 ? 0.18 : 0.24; add(SG.wing, [0, 0, -0.06], [0, 0, 0], [span, 1, 1]); if (kind === 3) { add(SG.wing, [0, 0.01, -0.12], [0, 0, 0], [span * 0.6, 1, 0.8]); add(SG.fin, [0, 0.06, -0.12]); } if (kind !== 1) add(SG.cockpit, [0, 0.02, 0.06]); }
    s.userData.isModel = false; return s;
  }

  // Real CC0 glTF hulls (Quaternius + Kenney) loaded async; clone one per foe. Primitive fallback until ready.
  const SHIPTPL = {};   // kind -> normalized + oriented template Group
  const ORIENT = { 0: [-0.34, Math.PI, 0], 1: [-0.34, Math.PI, 0], 2: [-0.34, Math.PI, 0], 3: [-0.34, Math.PI, 0] };   // nose -> camera, gentle 3/4 tilt (tuned visually)
  function makeTemplate(o, kind) {
    o.traverse(m => { if (m.isMesh && m.material) { const mm = Array.isArray(m.material) ? m.material : [m.material]; mm.forEach(x => { x.fog = false; if (x.emissive && x.color) { x.emissive.copy(x.color); x.emissiveIntensity = 0.2; } if (x.metalness != null) x.metalness = 0.35; if (x.roughness != null) x.roughness = 0.4; }); } });
    const inner = new THREE.Group(); inner.add(o);                       // wrap so we scale the GROUP, never the model's own root transform
    const box = new THREE.Box3().setFromObject(inner), sz = box.getSize(new THREE.Vector3()), ctr = box.getCenter(new THREE.Vector3());
    o.position.sub(ctr);                                                 // centre the hull within the wrapper
    inner.scale.setScalar(0.64 / Math.max(sz.x, sz.y, sz.z, 0.001));     // normalise size on the wrapper
    const r = ORIENT[kind] || ORIENT[0]; inner.rotation.set(r[0], r[1], r[2]);
    const w = new THREE.Group(); w.add(inner); w.userData.isModel = true; return w;
  }
  (async () => {
    try {
      const { GLTFLoader } = await import('/apps/games/vendor/GLTFLoader.js');
      const loader = new GLTFLoader(), base = '/apps/games/games/models/';
      const load = (u) => new Promise((res) => loader.load(u, g => res(g.scene), undefined, () => res(null)));
      const [f, sc, hv] = await Promise.all([load(base + 'fighter.glb'), load(base + 'scout.glb'), load(base + 'heavy.glb')]);
      if (f) { SHIPTPL[0] = makeTemplate(f, 0); SHIPTPL[3] = makeTemplate(f.clone(true), 3); }
      if (sc) SHIPTPL[1] = makeTemplate(sc, 1);
      if (hv) SHIPTPL[2] = makeTemplate(hv, 2);
    } catch (e) { console.warn('[costellazioni] ship models -> primitives', e); }
  })();
  function buildShip(kind, mat) {
    if (SHIPTPL[kind]) { const w = SHIPTPL[kind].clone(true); w.userData.isModel = true; return w; }
    return primitiveShip(kind, mat);
  }
  const NF = 8, foeMeshes = [];
  for (let i = 0; i < NF; i++) {
    const g = new THREE.Group();
    const mat = new THREE.MeshStandardMaterial({ color: 0xffffff, emissive: 0x000000, emissiveIntensity: 0.35, metalness: 0.55, roughness: 0.42, flatShading: true });
    mat.fog = false;
    const ship = buildShip(0, mat); g.add(ship);
    const halo = sprite(0xffffff, 0.2, 0.3);                                             // subtle aura only — the model carries the look
    const engine = sprite(0xff9060, 0.18, 0.9); engine.position.set(0, -0.05, -0.18);    // exhaust behind the hull
    g.add(engine); g.add(halo); g.visible = false; g.userData = { ship, mat, halo, engine, kind: -1, isModel: ship.userData.isModel, lastx: 0, bank: 0 }; scene.add(g); foeMeshes.push(g);
  }
  // enemy tracers + player lasers + reticle + lock box
  const NB = 18, tracers = []; for (let i = 0; i < NB; i++) { const s = sprite(0xff5c50, 0.12, 0); scene.add(s); tracers.push(s); }
  const NM = 8, missiles = []; for (let i = 0; i < NM; i++) { const s = sprite(0x9fe8ff, 0.14, 0); scene.add(s); missiles.push(s); }   // player homing missiles
  const NP = 6, pickupSpr = []; for (let i = 0; i < NP; i++) { const s = sprite(0xffffff, 0.16, 0); scene.add(s); pickupSpr.push(s); }   // power-up drops
  const laserMat = new THREE.LineBasicMaterial({ color: 0xff7050, transparent: true, opacity: 0 });
  const laserGeo = new THREE.BufferGeometry(); laserGeo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(12), 3));
  const lasers = new THREE.LineSegments(laserGeo, laserMat); scene.add(lasers);
  const retic = new THREE.Group(); { const m = new THREE.LineBasicMaterial({ color: 0x60cee8 }); const mk = (pts) => new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts.map(p => new THREE.Vector3(p[0], p[1], 0))), m); const ring = []; for (let a = 0; a <= 24; a++) ring.push([Math.cos(a / 24 * 6.28) * 0.05, Math.sin(a / 24 * 6.28) * 0.05]); retic.add(mk(ring)); const tick = [[0.03, 0], [0.07, 0]]; for (let i = 0; i < 4; i++) { const L = mk(tick); L.rotation.z = i * Math.PI / 2; retic.add(L); } scene.add(retic); }
  const lockBox = new THREE.Group(); { const gm = new THREE.LineBasicMaterial({ color: 0x76e68c }); for (let i = 0; i < 4; i++) { const L = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(-0.06, 0.03, 0), new THREE.Vector3(-0.06, 0.06, 0), new THREE.Vector3(-0.03, 0.06, 0)]), gm); L.rotation.z = i * Math.PI / 2; lockBox.add(L); } scene.add(lockBox); }
  // bridge attract planet
  const planet = new THREE.Mesh(new THREE.IcosahedronGeometry(0.5, 1), new THREE.MeshStandardMaterial({ color: 0x2a4f80, emissive: 0x102038, metalness: 0.3, roughness: 0.7, flatShading: true })); planet.position.set(1.0, -0.2, 0); scene.add(planet);

  // particle pool (explosions)
  const PMAX = 90, pool = []; for (let i = 0; i < PMAX; i++) { const s = sprite(0xffffff, 0.1, 0); s.visible = false; scene.add(s); pool.push({ s, vx: 0, vy: 0, ez: 0, life: 0, max: 1 }); } let ph = 0;
  function boom(sx, sy, big) { const n = big ? 22 : 14; for (let k = 0; k < n; k++) { const p = pool[ph++ % PMAX]; const a = Math.random() * 6.28, sp = (big ? 1.4 : 1) * (0.3 + Math.random()); p.vx = Math.cos(a) * sp; p.vy = Math.sin(a) * sp; p.life = p.max = 0.4 + Math.random() * 0.4; p.s.material.color.setHex((k & 1) ? 0xffbe40 : 0xffffff); p.s.position.set(sx, sy, 0.2); p.s.visible = true; p.s.scale.setScalar(0.08 + Math.random() * 0.08); } }

  let last = performance.now(), shake = 0, lastEvClock = -1, rax = 0, ray = 0, lockPulse = 0, threatC = null;   // rax/ray = smoothed reticle; threatC = smoothed enemy colour
  let aPhase = '', aWave = -1, aScreen = '', aFlash = null;   // audio edge-trackers
  function fit(w, h) { w = Math.max(2, w | 0); h = Math.max(2, h | 0); renderer.setSize(w, h, false); asp = w / h; cam.left = -asp; cam.right = asp; cam.top = 1; cam.bottom = -1; cam.updateProjectionMatrix(); }
  fit(canvas.clientWidth || 960, canvas.clientHeight || 540);

  function frame(state, ctx) {
    const now = performance.now(), dt = Math.min(0.05, (now - last) / 1000); last = now;
    tt += dt;                                                          // drive the living nebula sky
    updateBiome(state, ctx, dt);                                       // biome cross-fade (per-sector) -> refresh cur.* before the nebula reads it
    for (const n of nebula) {
      tmpc.setHSL((cur.nebHue + n.hue * 0.18 + tt * 0.012) % 1, Math.min(0.95, Math.max(0.2, cur.nebSat)), Math.min(0.62, Math.max(0.32, cur.nebLight)));
      n.s.material.color.copy(tmpc);
      n.s.material.opacity = cur.nebOp + 0.06 * Math.sin(tt * 0.18 + n.ph) + whoosh * 0.18;
      n.s.position.x = n.ox + Math.sin(tt * 0.05 + n.ph) * 0.22;
      n.s.position.y = n.oy + Math.cos(tt * 0.04 + n.ph) * 0.16;
      n.s.material.rotation += dt * (0.03 + whoosh * 0.12);
      n.s.scale.setScalar(n.sz * (1.4 + 0.2 * Math.sin(tt * 0.1 + n.ph)) * (1 + whoosh * 0.1));
    }
    const ph = state ? state.phase : 'loading';
    // audio edges — the renderer sees full state every frame, so drive all sound from here
    if (ph === 'combat' && aPhase !== 'combat') sfx.startDrone();
    if (ph !== 'combat' && aPhase === 'combat') sfx.stopDrone();
    if (ph === 'debrief' && aPhase !== 'debrief') { if (state && state.result === 1) sfx.victory(); else sfx.defeat(); }
    if (state) {
      if (ph === 'combat' && state.wave !== aWave) { if (state.wave > 0) sfx.wave(state.wave); if (state.cc) sfx.setIntensity(Math.min(1, (state.wave - 1) / Math.max(1, state.cc.waves - 1))); aWave = state.wave; }   // music heats up with the waves
      if (state.screen && state.screen !== aScreen) { if (aScreen && ph === 'hub') sfx.blip(); aScreen = state.screen; }
      if (state.flash && state.flash !== aFlash) { const k = state.flash.kind; if (k === 'warp' || k === 'sector') sfx.jump(); else if (k === 'pop') sfx.cash(); else if (k === 'ignite') sfx.confirm(); else if (k === 'shake') sfx.deny(); aFlash = state.flash; }
    }
    aPhase = ph;
    hud.show(ph, state ? state.screen : 'bridge');
    if (ph === 'combat') {
      planet.visible = false; retic.visible = true; hud.combat(state);
      state.sector = ctx ? (ctx.sector | 0) : 0;                        // expose sector for the enemy threat-colour ramp
      drawCombat(state, dt);
    } else if (ph === 'debrief') {
      // the killing blow that ends the fight ships its boom event on a snapshot already in 'debrief' —
      // drain it here (same gating as combat) so the final explosion is actually drawn, not swallowed.
      if (state && state.clock !== lastEvClock) { lastEvClock = state.clock; for (const e of (state.ev || [])) if (e.t === 'boom') { const p = project(e.ex, e.ey, e.ez); boom(p.sx * asp, -p.sy, e.big); sfx.boom(e.big); } }
      planet.visible = false; retic.visible = false; lockBox.visible = false; lasers.material.opacity = 0; hideCombat(); hud.debrief(state);
      for (const s of stars) { s.ez -= 8 * dt; if (s.ez < 6) Object.assign(s, rndStar()); } writeStars(2);
    } else { // loading / new_run / conflict / hub -> attract scene behind the menu
      hud.paint(ctx, state || { phase: 'loading', screen: 'bridge', focus: {}, marketCol: 0, marketQty: 1, target: -1, clock: 0 });
      for (const s of stars) { s.ez -= 18 * dt; if (s.ez < 6) Object.assign(s, rndStar()); }
      writeStars(3); hideCombat();
      planet.visible = true; planet.rotation.y += dt * 0.18; planet.rotation.x += dt * 0.05;
      lasers.material.opacity = 0; retic.visible = false; lockBox.visible = false;
    }
    // particles
    for (const p of pool) { if (p.life <= 0) { if (p.s.visible) p.s.visible = false; continue; } p.life -= dt; const k = p.life / p.max; p.s.position.x += p.vx * dt; p.s.position.y += p.vy * dt; p.s.material.opacity = k; p.s.scale.setScalar(0.05 + k * 0.1); }
    // camera shake
    shake = state && state.shake ? Math.max(shake, state.shake) : shake * 0.9;
    cam.position.x = (Math.random() - 0.5) * shake * 0.12; cam.position.y = (Math.random() - 0.5) * shake * 0.1; shake *= 0.86;
    renderer.render(scene, cam);
  }
  // streak = how far back (in z) the tail trails: 0 = dots, large = long hyperspace lines fanning out.
  function writeStars(streak) {
    const tz = streak || 1.2;
    for (let i = 0; i < NS; i++) {
      const s = stars[i], h = project(s.ex, s.ey, s.ez), t = project(s.ex, s.ey, s.ez + tz * (1 + (260 - s.ez) * 0.004)), o = i * 6;
      starPos[o] = h.sx * asp; starPos[o + 1] = -h.sy; starPos[o + 2] = 0;
      starPos[o + 3] = t.sx * asp; starPos[o + 4] = -t.sy; starPos[o + 5] = 0;
    }
    starGeo.attributes.position.needsUpdate = true;
  }
  function hideCombat() { for (const g of foeMeshes) g.visible = false; for (const t of tracers) t.material.opacity = 0; for (const m of missiles) m.material.opacity = 0; for (const s of pickupSpr) s.material.opacity = 0; }
  function drawCombat(st, dt) {
    // stars warp faster in combat
    for (const s of stars) { s.ez -= 60 * dt; if (s.ez < 6) Object.assign(s, rndStar()); } writeStars(11);   // long hyperspace streaks
    // enemy THREAT colour (sector+wave), lerped ~0.4s in the renderer closure so it never snaps at a wave change
    const tHex = threatColor(st.sector | 0, st.wave, (st.cc && st.cc.waves) || 6);
    if (!threatC) threatC = new THREE.Color(tHex);
    threatC.lerp(_trR.setHex(tHex), 1 - Math.exp(-2.5 * dt));
    const threatHex = threatC.getHex();
    // foes
    const fk = 1 - Math.exp(-26 * dt);                                  // foe smoothing factor (frame-rate independent)
    for (let i = 0; i < NF; i++) {
      const g = foeMeshes[i], f = st.foes[i], ud = g.userData;
      if (!f) { g.visible = false; ud.lastId = -1; continue; }
      const p = project(f.ex, f.ey, f.ez); g.visible = true;
      const mul = f.kind === 2 ? 1.45 : f.kind === 1 ? 0.9 : f.kind === 3 ? 1.3 : 1;
      const sc = Math.min(6, Math.max(0.8, p.scale * mul));            // higher floor: distant ships stay readable
      // INTERPOLATE toward the 30Hz tick position/scale every frame so movement is silky, never stepped.
      const tx = p.sx * asp, ty = -p.sy, tsc = sc * 0.6;
      if (ud.lastId !== f.id) { ud.sx = tx; ud.sy = ty; ud.ssc = tsc; ud.lastId = f.id; }   // new occupant of this slot -> snap (don't lerp from a dead foe)
      else { ud.sx += (tx - ud.sx) * fk; ud.sy += (ty - ud.sy) * fk; ud.ssc += (tsc - ud.ssc) * fk; }
      g.position.set(ud.sx, ud.sy, 0);
      g.scale.setScalar(ud.ssc);
      // threat-aware colour: ace stays fixed gold (priority target); others = threat warmed toward their kind tint
      const isAce = f.kind === 3;
      const col = isAce ? 0xffbe40 : _trR.setHex(threatHex).lerp(_trB.setHex(KIND_COL[f.kind] || KIND_COL[0]), 0.30).getHex();
      if (ud.kind !== f.kind) { g.remove(ud.ship); ud.ship = buildShip(f.kind, ud.mat); ud.isModel = ud.ship.userData.isModel; g.add(ud.ship); ud.kind = f.kind; }   // swap hull (real glTF or primitive)
      if (!ud.isModel) {                                                                          // primitive fallback: full tint + flash via the shared material
        ud.mat.color.setHex(col); ud.mat.emissive.setHex(col);
        const hitFlash = (st.clock - (f.hitAt || -999) < 90) ? 1.5 : 0;
        ud.mat.emissiveIntensity = (isAce ? 0.5 + 0.25 * Math.sin(st.clock / 110) : 0.32) + hitFlash;
      } else {                                                                                    // glTF: tint EMISSIVE only — keep the model's own materials/colour
        const eInt = (isAce ? 0.30 + 0.12 * Math.sin(st.clock / 110) : 0.22) + ((st.clock - (f.hitAt || -999) < 90) ? 0.9 : 0);
        ud.ship.traverse(m => { if (m.isMesh && m.material) { const mm = Array.isArray(m.material) ? m.material : [m.material]; mm.forEach(x => { if (x.emissive) { x.emissive.setHex(col); x.emissiveIntensity = eInt; } }); } });
      }
      const halo = ud.halo; halo.material.color.setHex(col); halo.material.opacity = ud.isModel ? 0.26 : 0.3; halo.scale.setScalar(0.18 + sc * 0.1);   // threat colour conveyed by the aura + engine
      const engHex = isAce ? 0xffd070 : _trA.setHex(threatHex).lerp(_trB.setHex(0xff9060), 0.45).getHex();
      ud.engine.material.color.setHex(engHex); ud.engine.material.opacity = 0.5 + 0.28 * Math.sin(st.clock / 70 + i);
      // fly like a ship: BANK into lateral motion + gentle pitch bob — never spin on itself
      const vx = f.ex - ud.lastx; ud.lastx = f.ex;
      ud.bank += (Math.max(-0.9, Math.min(0.9, -vx * 26)) - ud.bank) * Math.min(1, dt * 8);
      ud.ship.rotation.z = ud.bank;
      ud.ship.rotation.y = ud.bank * 0.4;                                                          // slight yaw with the bank
      ud.ship.rotation.x = (ud.isModel ? 0 : -0.55) + Math.sin((f.wphase || 0) * 0.8) * 0.05;      // models bake their own tilt
    }
    // tracers
    for (let i = 0; i < NB; i++) { const b = st.bolts[i], t = tracers[i]; if (!b) { t.material.opacity = 0; continue; } const p = project(b.ex, b.ey, b.ez); t.position.set(p.sx * asp, -p.sy, 0.1); t.scale.setScalar(0.06 + p.scale * 0.05); t.material.opacity = 0.95; }
    // player missiles (bright cyan darts streaking outward toward their lock)
    const pm = st.pmiss || []; for (let i = 0; i < NM; i++) { const m = pm[i], spr = missiles[i]; if (!m) { spr.material.opacity = 0; continue; } const p = project(m.ex, m.ey, m.ez); spr.position.set(p.sx * asp, -p.sy, 0.14); spr.scale.setScalar(0.1 + p.scale * 0.08); spr.material.opacity = 1; }
    // power-up drops — pulsing coloured orbs flying toward you (cyan missile / blue shield / green repair)
    const PK = st.pickups || []; for (let i = 0; i < NP; i++) { const pk = PK[i], spr = pickupSpr[i]; if (!pk) { spr.material.opacity = 0; continue; } const p = project(pk.ex, pk.ey, pk.ez); spr.position.set(p.sx * asp, -p.sy, 0.13); spr.scale.setScalar(0.14 + p.scale * 0.1); spr.material.color.setHex(pk.kind === 'missile' ? 0x60ffe0 : pk.kind === 'shield' ? 0x60a0ff : 0x76e68c); spr.material.opacity = 0.85 + 0.15 * Math.sin(st.clock / 70 + i); }
    // reticle: frame-rate-independent smoothing toward the 30Hz tick aim — kills the stutter
    const sk = 1 - Math.exp(-24 * dt);
    rax += (st.aimx - rax) * sk; ray += (st.aimy - ray) * sk;
    retic.position.set(rax * asp, -ray, 0);
    const locked = st.lock >= 0 && st.foes[st.lock];
    if (lockPulse > 0) lockPulse = Math.max(0, lockPulse - dt * 4);
    retic.scale.setScalar((locked ? 1 : 1 + Math.sin(st.clock / 380) * 0.06) + lockPulse * 0.5);   // breathe idle, snap on lock
    for (const c of retic.children) c.material.color.setHex(locked ? 0xff5c50 : 0x60cee8);
    if (locked) { const lg = foeMeshes[st.lock], f = st.foes[st.lock], p = project(f.ex, f.ey, f.ez); lockBox.visible = true; lockBox.position.set(lg.userData.sx, lg.userData.sy, 0.05); const r = Math.min(0.45, 0.07 + p.scale * 0.06); lockBox.scale.setScalar((r / 0.06) * (1 + lockPulse * 0.6)); lockBox.rotation.z += dt * 0.6; }   // track the SMOOTHED ship position
    else lockBox.visible = false;
    // twin lasers spring from the SMOOTHED reticle so bolt and crosshair never disagree
    if (st.muz && st.clock < st.muz) { const ax = rax * asp, ay = -ray; const pos = lasers.geometry.attributes.position.array; pos.set([-asp, -1, 0, ax, ay, 0, asp, -1, 0, ax, ay, 0]); lasers.geometry.attributes.position.needsUpdate = true; lasers.material.opacity = 0.95; } else lasers.material.opacity *= 0.6;
    // events -> booms + lock pulse (once per tick snapshot, not per RAF — the same state renders ~2 frames)
    if (st.clock !== lastEvClock) {
      lastEvClock = st.clock;
      for (const e of (st.ev || [])) {
        if (e.t === 'boom') { const p = project(e.ex, e.ey, e.ez); boom(p.sx * asp, -p.sy, e.big); sfx.boom(e.big); }
        else if (e.t === 'lock') { lockPulse = 1; sfx.lock(); }
        else if (e.t === 'laser') sfx.laser();
        else if (e.t === 'hit') sfx.hit();
        else if (e.t === 'pass') sfx.strafe();
        else if (e.t === 'hurt') sfx.hurt();
        else if (e.t === 'mfire') sfx.missile();
        else if (e.t === 'pickup') sfx.powerup(e.kind);
      }
    }
  }
  return { frame, resize: fit, dispose() { hud.dispose(); scene.traverse(o => { if (o.geometry) o.geometry.dispose(); if (o.material) (Array.isArray(o.material) ? o.material : [o.material]).forEach(m => { if (m.map) m.map.dispose(); m.dispose(); }); }); Object.values(SG).forEach(g => g.dispose()); glowTex.dispose(); renderer.dispose(); }, mode: 'webgl' };
}
function makeGlow(THREE) { const c = document.createElement('canvas'); c.width = c.height = 64; const g = c.getContext('2d'); const gr = g.createRadialGradient(32, 32, 0, 32, 32, 32); gr.addColorStop(0, 'rgba(255,255,255,1)'); gr.addColorStop(0.4, 'rgba(255,255,255,.5)'); gr.addColorStop(1, 'rgba(255,255,255,0)'); g.fillStyle = gr; g.fillRect(0, 0, 64, 64); const t = new THREE.CanvasTexture(c); t.colorSpace = THREE.SRGBColorSpace; return t; }

// ============================ 2D fallback =======================================================
function build2D(canvas, hud) {
  const ctx = canvas.getContext('2d'); let last = performance.now(), rax = 0, ray = 0;
  const stars = []; for (let i = 0; i < 80; i++) stars.push({ ex: (Math.random() * 2 - 1) * 1.6, ey: (Math.random() * 2 - 1) * 1.1, ez: 6 + Math.random() * 254 });
  function frame(state, c) {
    const now = performance.now(), dt = Math.min(0.05, (now - last) / 1000); last = now;
    const W = canvas.width, H = canvas.height, cx = W / 2, cy = H / 2, sc = Math.min(W, H) / 2;
    const ph = state ? state.phase : 'loading';
    hud.show(ph, state ? state.screen : 'bridge'); ctx.fillStyle = '#05060f'; ctx.fillRect(0, 0, W, H);
    const warp = ph === 'combat' ? 60 : 16;
    for (const s of stars) { s.ez -= warp * dt; if (s.ez < 6) { s.ex = (Math.random() * 2 - 1) * 1.6; s.ey = (Math.random() * 2 - 1) * 1.1; s.ez = 260; } const p = project(s.ex, s.ey, s.ez); ctx.fillStyle = 'rgba(159,196,255,.8)'; ctx.fillRect(cx + p.sx * sc, cy - p.sy * sc, 2, 2); }
    if (!state) { hud.paint(c, { phase: 'loading', screen: 'bridge', focus: {}, marketCol: 0, marketQty: 1, target: -1, clock: 0 }); return; }
    if (ph === 'combat') {
      hud.combat(state);
      for (const f of state.foes) { const p = project(f.ex, f.ey, f.ez); const r = Math.max(3, p.scale * sc * 0.05); ctx.fillStyle = f.kind === 3 ? '#ffbe40' : '#c06434'; ctx.beginPath(); ctx.arc(cx + p.sx * sc, cy - p.sy * sc, r, 0, 7); ctx.fill(); }
      for (const b of state.bolts) { const p = project(b.ex, b.ey, b.ez); ctx.fillStyle = '#ff5c50'; ctx.fillRect(cx + p.sx * sc - 2, cy - p.sy * sc - 2, 4, 4); }
      const k = 1 - Math.exp(-24 * dt); rax += (state.aimx - rax) * k; ray += (state.aimy - ray) * k;   // same smoothing as WebGL
      const ax = cx + rax * sc, ay = cy - ray * sc;
      ctx.strokeStyle = state.lock >= 0 ? '#ff5c50' : '#60cee8'; ctx.lineWidth = 2; ctx.beginPath(); ctx.arc(ax, ay, 10, 0, 7); ctx.stroke();
      if (state.muz && state.clock < state.muz) { ctx.strokeStyle = '#ff7050'; ctx.beginPath(); ctx.moveTo(2, H); ctx.lineTo(ax, ay); ctx.moveTo(W - 2, H); ctx.lineTo(ax, ay); ctx.stroke(); }
    } else if (ph === 'debrief') hud.debrief(state);
    else hud.paint(c, state);   // loading / new_run / conflict / hub
  }
  return { frame, resize() {}, dispose() { hud.dispose(); }, mode: '2d' };
}

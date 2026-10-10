// constellations-3d.js — the renderer of Costellazioni (web). It owns WebGL and draws, from one
// system blueprint (stelle/world.js) and one flight (stelle/sim.js):
//   · the hub backdrop — a slow cinematic orbit of the system's station, star, planets and nebula,
//     with the DOM hub (constellations-ui.js) on top; a hyperspace tunnel plays on every jump;
//   · the flight — 6DOF space combat with a chase or cockpit camera, the procedural ship kit
//     (stelle/kit.js), the environment (stelle/space.js), pooled effects (stelle/fx.js) and the HUD +
//     input layer (stelle/hud.js);
//   · the after-action card over a slowly orbiting freeze-frame.
// Post chain (medium/high tiers): far pass → near pass → UnrealBloom → grade (aberration, damage
// vignette, flash, grain) → ACES + sRGB output. The fixed 60 Hz sim is advanced here, decoupled from
// the frame rate; ships and bolts are interpolated between sim steps. Quality: auto by GPU with a
// manual override and dynamic resolution that holds the frame time. Falls back to 2D without WebGL.
import { makeUI } from '/apps/games/games/constellations-ui.js';
import { sfx } from '/apps/games/games/constellations-sfx.js';
import { CLS, EV, F_PLAYER, F_GILDA, F_RELITTI, F_CUSTODI, TEAM_P, TEAM_E, frame as simFrame, localToWorld } from '/apps/games/games/stelle/sim.js';
import { createHud } from '/apps/games/games/stelle/hud.js';

export const TIERS = {
  low: { scale: 0.72, bloom: false, msaa: 0, particles: 1800, debris: 90, trails: 24, dust: 360, sky: 512, planet: 512, stars: 1800, rocksExtra: 150 },
  medium: { scale: 1.0, bloom: true, msaa: 4, particles: 3600, debris: 160, trails: 40, dust: 620, sky: 768, planet: 1024, stars: 3200, rocksExtra: 500 },
  high: { scale: 1.35, bloom: true, msaa: 4, particles: 5200, debris: 220, trails: 48, dust: 900, sky: 1024, planet: 2048, stars: 4600, rocksExtra: 1100 },
};
const QKEY = 'stelle.quality';
export function pickTier(gl) {
  let forced = null; try { forced = localStorage.getItem(QKEY); } catch {}
  if (forced && TIERS[forced]) return { name: forced, auto: false };
  let name = 'medium';
  try {
    const ext = gl && gl.getExtension('WEBGL_debug_renderer_info');
    const r = ext ? String(gl.getParameter(ext.UNMASKED_RENDERER_WEBGL)) : '';
    if (/SwiftShader|llvmpipe|Software|Basic Render/i.test(r)) name = 'low';
    else if (/NVIDIA|GeForce|RTX|Radeon RX|Radeon Pro|Quadro/i.test(r)) name = 'high';
    if (/Android|iPhone|iPad|Mobile/i.test(navigator.userAgent)) name = 'low';
    pickTier.renderer = r;
  } catch {}
  return { name, auto: true };
}
export function setQuality(name) { try { if (name === 'auto') localStorage.removeItem(QKEY); else localStorage.setItem(QKEY, name); } catch {} }

const FAC_BOLT = { [F_PLAYER]: [3.2, 2.3, 0.75], [F_GILDA]: [0.55, 1.1, 3.4], [F_RELITTI]: [3.4, 0.75, 0.22], [F_CUSTODI]: [1.7, 2.8, 0.6], 3: [1.2, 0.6, 3.2] };
const FAC_DEBRIS = { [F_PLAYER]: [0.6, 0.58, 0.52], [F_GILDA]: [0.72, 0.7, 0.62], [F_RELITTI]: [0.32, 0.18, 0.1], [F_CUSTODI]: [0.7, 0.7, 0.66], 3: [0.1, 0.1, 0.14] };
const FAC_SHIELD = { [F_PLAYER]: [0.4, 1.4, 0.8], [F_GILDA]: [0.45, 0.8, 1.8], [F_RELITTI]: [1.6, 0.7, 0.25], [F_CUSTODI]: [1.6, 1.2, 0.5], 3: [0.7, 0.9, 1.8] };

export async function createRenderer(canvas, api) {
  let gl = null;
  try { gl = canvas.getContext('webgl2', { antialias: false, alpha: false, powerPreference: window.__czPowerPref || 'high-performance', stencil: false, preserveDrawingBuffer: !!window.__czPreserve }); } catch {}
  let THREE = null, POST = null, KIT = null, SPACE = null, FX = null;
  if (gl) {
    try {
      [THREE, POST, KIT, SPACE, FX] = await Promise.all([import('/apps/games/vendor/three.module.min.js'), import('/apps/games/vendor/three-postfx.js'),
        import('/apps/games/games/stelle/kit.js'), import('/apps/games/games/stelle/space.js'), import('/apps/games/games/stelle/fx.js')]);
    } catch (e) { console.warn('[costellazioni] 3D modules unavailable -> 2D', e); THREE = null; }
  }
  const ui = makeUI(canvas);
  const hud = createHud(canvas);
  return THREE ? build3D({ THREE, POST, KIT, SPACE, FX }, canvas, gl, ui, hud) : build2D(canvas, ui, hud);
}

// ================================= WebGL ===========================================================================
function build3D(M, canvas, gl, ui, hud) {
  const { THREE, POST, KIT, SPACE, FX } = M;
  const tier = pickTier(gl), Q = TIERS[tier.name];
  const renderer = new THREE.WebGLRenderer({ canvas, context: gl, antialias: false, powerPreference: 'high-performance' });
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  let scale = Math.min(Q.scale, tier.name === 'high' ? 1.35 : 1) * Math.min(1, dpr);
  renderer.setPixelRatio(scale * (tier.name === 'high' ? dpr : 1) || 1);
  renderer.outputColorSpace = THREE.SRGBColorSpace; renderer.toneMapping = THREE.ACESFilmicToneMapping; renderer.toneMappingExposure = 1.05;
  renderer.setClearColor(0x000000, 1);
  const cam = new THREE.PerspectiveCamera(70, 1.6, 0.4, 30000), farCam = new THREE.PerspectiveCamera(70, 1.6, 10, 200000);
  const space = SPACE.createSpace(THREE, renderer, Q);
  space.near.add(cam); cam.add(space.flares); cam.add(space.tunnel);
  const camFill = new THREE.DirectionalLight(0x8fa6c8, 0.55); camFill.position.set(0.3, 0.6, 1); cam.add(camFill); cam.add(camFill.target); camFill.target.position.set(0, 0, -1);   // keeps hulls readable against the dark
  const fx = FX.createFx(THREE, space.near, Q);

  // ---- post chain ------------------------------------------------------------------------------------------
  let composer = null, bloom = null, grade = null, farPass = null;
  const GRADE = {
    uniforms: { tDiffuse: { value: null }, uVig: { value: 0.55 }, uAber: { value: 0.0 }, uFlash: { value: 0 }, uFlashCol: { value: new THREE.Color(1, 1, 1) }, uDmg: { value: 0 }, uSat: { value: 1 }, uTime: { value: 0 } },
    vertexShader: `varying vec2 vUv; void main() { vUv = uv; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`,
    fragmentShader: `uniform sampler2D tDiffuse; uniform float uVig, uAber, uFlash, uDmg, uSat, uTime; uniform vec3 uFlashCol; varying vec2 vUv;
      void main() { vec2 c = vUv - 0.5; float r = length(c); vec2 off = c * uAber * r;
        vec3 col = vec3(texture2D(tDiffuse, vUv + off).r, texture2D(tDiffuse, vUv).g, texture2D(tDiffuse, vUv - off).b);
        float l = dot(col, vec3(0.2126, 0.7152, 0.0722)); col = mix(vec3(l), col, uSat);
        col += vec3(0.8, 0.04, 0.02) * uDmg * smoothstep(0.32, 0.78, r);
        col *= 1.0 - uVig * smoothstep(0.42, 0.98, r);
        col += uFlashCol * uFlash;
        col += (fract(sin(dot(vUv * 913.0 + uTime, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) * 0.01;
        col = min(max(col, vec3(0.0)), vec3(64.0));
        gl_FragColor = vec4(col, 1.0); }`,
  };
  function setupPost() {
    if (composer) { composer.dispose(); composer = null; }
    if (!Q.bloom) return;
    const sz = renderer.getDrawingBufferSize(new THREE.Vector2());
    const rt = new THREE.WebGLRenderTarget(Math.max(2, sz.x), Math.max(2, sz.y), { type: THREE.HalfFloatType, samples: Q.msaa });
    composer = new POST.EffectComposer(renderer, rt);
    composer.setPixelRatio(1); composer.setSize(Math.max(2, sz.x), Math.max(2, sz.y));
    farPass = new POST.RenderPass(space.far, farCam);
    const nearPass = new POST.RenderPass(space.near, cam); nearPass.clear = false; nearPass.clearDepth = true;
    bloom = new POST.UnrealBloomPass(new THREE.Vector2(sz.x, sz.y), 0.6, 0.45, 0.92);
    grade = new POST.ShaderPass(GRADE);
    composer.addPass(farPass); composer.addPass(nearPass); composer.addPass(bloom); composer.addPass(grade); composer.addPass(new POST.OutputPass());
  }

  // ---- ship visuals bound to sim slots ----------------------------------------------------------------------------
  const V = [];               // per sim ship slot
  const turretGeo = {};
  const _v = new THREE.Vector3(), _v2 = new THREE.Vector3(), _q = new THREE.Quaternion(), _q2 = new THREE.Quaternion(), _m = new THREE.Matrix4(), _c = new THREE.Color();
  const W = { x: 0, y: 0, z: 0 };
  function shipVisual(s) {
    const g = new THREE.Group();
    const ace = s.ace ? (s.aceId >= 0 ? s.aceId : true) : false;
    const geo = s.ck === 'station' || s.ck === 'beacon' ? null : KIT.shipGeometry(THREE, s.ck, s.isPlayer ? F_PLAYER : s.fac, ace);
    const mat = KIT.hullMaterial(THREE, { metal: s.cap ? 0.55 : 0.5, rough: 0.44, panel: s.cap ? 0.18 : 0.6, env: 1.0 });
    let body = null;
    if (geo) { body = new THREE.Mesh(geo, mat); g.add(body); if (ace !== false) body.scale.setScalar(1.15); }
    const plumes = [];
    const ec = KIT.PALETTES[s.isPlayer ? F_PLAYER : s.fac] ? KIT.PALETTES[s.isPlayer ? F_PLAYER : s.fac].engine : [1, 1, 1];
    for (const e of s.cls.eng || []) { const p = fx.plume(ec[0], ec[1], ec[2]); p.position.set(e[0], e[1], e[2] - 0.2); p.userData.r = e[3]; g.add(p); plumes.push(p); }
    const turrets = [];
    if (s.cls.sub) {
      const tg = turretGeo[s.fac] || (turretGeo[s.fac] = KIT.turretGeometry(THREE, s.fac));
      s.cls.sub.forEach((sb, k) => {
        if (sb.k !== 'turret') return;
        const base = new THREE.Mesh(tg.base, mat); base.position.set(...sb.p); if (sb.down) base.rotation.z = Math.PI;
        const head = new THREE.Mesh(tg.head, mat); head.position.y = 1.4; base.add(head);
        const gun = new THREE.Mesh(tg.gun, mat); gun.position.set(0, 1.3, -0.6); head.add(gun);
        g.add(base); turrets.push({ k, base, head, gun });
      });
    }
    space.near.add(g);
    return { gen: s.gen, g, body, mat, plumes, turrets, trail: null, smokeT: 0, flash: 0, dead: false, chain: 0, chainT: 0, warpShown: false, rad: s.cls.rad };
  }
  function dropVisual(i) { const v = V[i]; if (!v) return; v.g.removeFromParent(); v.mat.dispose(); V[i] = null; }
  function clearVisuals() { for (let i = 0; i < V.length; i++) dropVisual(i); fx.clear(); }

  // missiles / pickups pools
  const mslGeo = new THREE.CylinderGeometry(0.22, 0.32, 2.6, 6); mslGeo.rotateX(Math.PI / 2);
  const mslMat = new THREE.MeshStandardMaterial({ color: 0xb8bcc4, metalness: 0.6, roughness: 0.4, emissive: 0x331100 });
  const msls = []; for (let i = 0; i < 40; i++) { const m = new THREE.Mesh(mslGeo, mslMat); m.visible = false; space.near.add(m); msls.push(m); }
  const pkGeo = new THREE.OctahedronGeometry(3, 0);
  const pkMats = [new THREE.MeshBasicMaterial({ color: new THREE.Color(0.4, 2.2, 1.8) }), new THREE.MeshBasicMaterial({ color: new THREE.Color(0.5, 0.9, 2.6) }), new THREE.MeshBasicMaterial({ color: new THREE.Color(0.6, 2.4, 0.6) })];
  const picks = []; for (let i = 0; i < 12; i++) { const m = new THREE.Mesh(pkGeo, pkMats[0]); m.visible = false; space.near.add(m); picks.push(m); }
  const cockpit = new THREE.Mesh(KIT.cockpitGeometry(THREE), KIT.hullMaterial(THREE, { metal: 0.3, rough: 0.6, panelK: 0, env: 0.5 }));
  cockpit.visible = false; cam.add(cockpit);
  // hub: the player's parked ship
  let hubShip = null;

  // ---- state ----------------------------------------------------------------------------------------------------
  let F = null, lastF = null, mode = 'hub', last = performance.now(), evCur = 0, trauma = 0, time = 0, camMode = 'chase';
  let lastPh = '', cruiseK = 0;
  let bpKey = '', pendingBp = null, tunnelT = -1, flashK = 0, hitAber = 0, sysReady = false, hubAng = 0, deathCam = null, intro = 0;
  const camPos = new THREE.Vector3(), camQ = new THREE.Quaternion(), camVel = new THREE.Vector3(), lastCamPos = new THREE.Vector3();
  const perf = { frames: 0, ms: 0, ema: 16.7, worst: 0, hist: new Float32Array(240), hi: 0, simSteps: 0, scale, tier: tier.name, auto: tier.auto, renderer: pickTier.renderer || '' };
  window.__cz = window.__cz || {}; window.__cz.perf = perf;
  window.__cz.r3d = { get composer() { return composer; }, get bloom() { return bloom; }, get grade() { return grade; }, renderer, cam, space, fx };
  hud.setOptions({ camera: () => camMode, setCamera: (m) => { camMode = m; }, quality: () => (tier.auto ? 'auto' : tier.name), setQuality: (n) => { setQuality(n); } });

  function fit(w, h) {
    w = Math.max(2, w | 0); h = Math.max(2, h | 0);
    renderer.setSize(w, h, false);
    cam.aspect = farCam.aspect = w / h; cam.updateProjectionMatrix(); farCam.updateProjectionMatrix();
    setupPost();
    hud.resize(w, h);
  }
  fit(canvas.clientWidth || 960, canvas.clientHeight || 540);

  function ensureSystem(bp, animate) {
    if (!bp || bp.key === bpKey || (pendingBp && pendingBp.key === bp.key)) return;
    if (animate && sysReady && tunnelT < 0) { pendingBp = bp; tunnelT = 0; if (sfx.warp) sfx.warp(1); return; }
    doBuild(bp);
  }
  function doBuild(bp) {
    space.build(bp); bpKey = bp.key; sysReady = true;
    if (hubShip) { hubShip.removeFromParent(); hubShip.material.dispose(); hubShip = null; }
    hubShip = new THREE.Mesh(KIT.shipGeometry(THREE, 'lucciola', F_PLAYER, false), KIT.hullMaterial(THREE, {})); space.near.add(hubShip);
    const st = bp.station ? bp.station.pos : [0, 0, -2000];
    hubShip.position.set(st[0] + 520, st[1] + 60, st[2] + 900); hubShip.rotation.set(0.05, 0.6, -0.12);
    hubShip.visible = !F;
    if (window.__cz) window.__cz.sysBuilds = (window.__cz.sysBuilds || 0) + 1;
  }

  // ---- events -> fx / audio / hud ------------------------------------------------------------------------------
  function camDist(x, y, z) { return Math.hypot(x - cam.position.x, y - cam.position.y, z - cam.position.z); }
  function pan(x, y, z) { _v.set(x, y, z).sub(cam.position).applyQuaternion(_q.copy(cam.quaternion).invert()); return Math.max(-1, Math.min(1, _v.x / (Math.abs(_v.z) + 40))); }
  function shakeAt(x, y, z, k) { const d = camDist(x, y, z); trauma = Math.min(1, trauma + k * Math.max(0, 1 - d / 900)); }
  function events() {
    if (F.evHead - evCur > 500) evCur = F.evHead - 500;
    const p = F.player;
    while (evCur < F.evHead) {
      const e = F.ev[evCur % 512]; evCur++;
      const s = e.s;
      switch (e.n) {
        case EV.SHOT: {
          if (!s) break;
          const c = FAC_BOLT[s.isPlayer ? F_PLAYER : s.fac] || FAC_BOLT[0];
          fx.muzzle(e.x, e.y, e.z, s.vel ? s.vel.x : 0, s.vel ? s.vel.y : 0, s.vel ? s.vel.z : 0, c[0] * 0.4, c[1] * 0.4, c[2] * 0.4, e.a === 2 ? 4 : 1.8);
          if (s.isPlayer) { sfx.laserP && sfx.laserP(e.b ? 0.25 : -0.25); trauma = Math.min(1, trauma + 0.012); }
          else { const d = camDist(e.x, e.y, e.z); if (d < 1400) sfx.laserFar && sfx.laserFar(Math.max(0, 1 - d / 1400), pan(e.x, e.y, e.z), e.a === 2); }
          break;
        }
        case EV.HIT: {
          if (!s) break;
          fx.hitSparks(e.x, e.y, e.z, (e.x - s.pos.x) / (s.cls.rad || 1), (e.y - s.pos.y) / (s.cls.rad || 1), (e.z - s.pos.z) / (s.cls.rad || 1), 1.0, 0.55, 0.2, e.a > 20 ? 16 : 9);
          const v = V[s.i]; if (v) v.flash = 1;
          if (e.s2 && e.s2.isPlayer) { hud.hitMarker(s.hull <= 0); if (s.hull > 0) sfx.hit(); }
          break;
        }
        case EV.SHIELD: {
          if (!s) break;
          const v = V[s.i];
          if (v && e.v) { const r = s.cap ? s.cls.rad : s.cls.rad * 1.15; const sc = s.cap ? [r * 0.42, r * 0.36, r * 1.0] : [r * 0.9, r * 0.6, r * 1.1]; fx.shieldHit(v.g, sc[0], sc[1], sc[2], e.v[0], e.v[1], e.v[2], FAC_SHIELD[s.isPlayer ? F_PLAYER : s.fac] || FAC_SHIELD[0], e.a); }
          if (s.isPlayer) sfx.shieldHit && sfx.shieldHit(); else if (e.s2 && e.s2.isPlayer) hud.hitMarker(false);
          break;
        }
        case EV.SPARK: { fx.hitSparks(e.x, e.y, e.z, e.a, e.b, e.c, 1.2, 0.8, 0.5, 6); fx.smoke.add(e.x, e.y, e.z, e.a * 4, e.b * 4, e.c * 4, 1.4, 1.5, 6, 0.2, 0.17, 0.14, 0.5, 0.05, 0.05, 0.05, 0, 2); break; }
        case EV.KILL: {
          if (!s) break;
          const v = V[s.i];
          if (e.c) { fx.warp(e.x, e.y, e.z, 0, 1, 0, false); if (v) v.g.visible = false; break; }   // decoy pops
          fx.explosion(e.x, e.y, e.z, s.vel.x, s.vel.y, s.vel.z, s.cls.rad * (s.ace ? 1.4 : 1), FAC_DEBRIS[s.isPlayer ? F_PLAYER : s.fac] || FAC_DEBRIS[0], !!s.ace);
          if (v) { v.g.visible = false; v.dead = true; }
          const d = camDist(e.x, e.y, e.z);
          sfx.boomAt ? sfx.boomAt(s.ace ? 1.5 : 1, Math.max(0.15, 1 - d / 2500), pan(e.x, e.y, e.z)) : sfx.boom(!!s.ace);
          shakeAt(e.x, e.y, e.z, s.isPlayer ? 1 : 0.35);
          if (e.s2 && e.s2.isPlayer && s.team === TEAM_E) { hud.kill(s, e.b); flashK = Math.max(flashK, 0.06); }
          if (s.isPlayer) { deathCam = { x: e.x, y: e.y, z: e.z, t: 0 }; flashK = 0.6; }
          break;
        }
        case EV.CAPKILL: {
          if (!s) break;
          const v = V[s.i]; if (v) { v.chain = 9; v.chainT = 0; v.dead = true; }
          shakeAt(e.x, e.y, e.z, 0.6);
          if (e.s2 && e.s2.isPlayer) hud.kill(s, e.b);
          break;
        }
        case EV.MFIRE: { fx.muzzle(e.x, e.y, e.z, 0, 0, 0, 1.4, 0.9, 0.5, 4); if (s && s.isPlayer) { sfx.missile(); trauma = Math.min(1, trauma + 0.15); } else if (e.s2 && e.s2.isPlayer) sfx.missileWarn && sfx.missileWarn(); break; }
        case EV.MHIT: { if (e.a) { fx.explosion(e.x, e.y, e.z, 0, 0, 0, 4, [0.3, 0.3, 0.3], false); shakeAt(e.x, e.y, e.z, 0.3); sfx.boomAt && sfx.boomAt(0.7, Math.max(0.1, 1 - camDist(e.x, e.y, e.z) / 2000), pan(e.x, e.y, e.z)); } else fx.muzzle(e.x, e.y, e.z, 0, 0, 0, 1, 0.6, 0.3, 6); break; }
        case EV.FLARE: { if (s && s.isPlayer) sfx.flare && sfx.flare(); break; }
        case EV.LOCK: { sfx.lock(); hud.flashLock(); break; }
        case EV.LOCKING: { sfx.lockTone && sfx.lockTone(e.a); break; }
        case EV.WARPIN: { if (!s) break; fx.warp(e.x, e.y, e.z, e.a, e.b, e.c, s.cap); if (camDist(e.x, e.y, e.z) < 4000) sfx.warp && sfx.warp(0.5); break; }
        case EV.WARPOUT: { fx.warp(e.x, e.y, e.z, 0, 0, -1, s && s.cap); const v = s && V[s.i]; if (v) v.g.visible = false; if (s && s.isPlayer) { sfx.warp && sfx.warp(1); tunnelT = 0; pendingBp = null; } break; }
        case EV.TETHER: { if (e.s2 && e.s2.isPlayer) { sfx.tether && sfx.tether(); trauma = Math.min(1, trauma + 0.3); } break; }
        case EV.PICKUP: { sfx.powerup(['missile', 'shield', 'repair'][e.a] || 'repair'); hud.pickup(e.a); break; }
        case EV.COMMS: { hud.comms(e); sfx.comms && sfx.comms(); break; }
        case EV.SUBSYS: { fx.explosion(e.x, e.y, e.z, s ? s.vel.x : 0, s ? s.vel.y : 0, s ? s.vel.z : 0, 6, [0.3, 0.3, 0.3], false); hud.subsys(s, e.k); shakeAt(e.x, e.y, e.z, 0.3); break; }
        case EV.PHURT: {
          trauma = Math.min(1, trauma + Math.min(0.5, 0.08 + e.a * 3));
          hitAber = Math.min(1, hitAber + (e.b ? 0.6 : 0.25));
          hud.hurt(e.x, e.y, e.z, e.b);
          if (e.b) sfx.hurt();
          break;
        }
        case EV.BOOST: { sfx.boost && sfx.boost(); trauma = Math.min(1, trauma + 0.12); break; }
        case EV.OVERHEAT: { sfx.overheat && sfx.overheat(); hud.note('cz_hud_overheat', 'bad'); break; }
        case EV.WAVE: { hud.wave(e.a, e.b); if (e.a > 1) sfx.wave(e.a); sfx.setIntensity(Math.min(1, e.a / Math.max(1, e.b))); break; }
        case EV.END: { hud.end(e.a); if (e.a === 1) sfx.victory(); else if (e.a === 2) sfx.defeat(); break; }
        case EV.BUMP: { if (s && s.isPlayer) { trauma = Math.min(1, trauma + Math.min(0.6, e.a / 160)); sfx.clang && sfx.clang(); } break; }
        case EV.TARGET: { sfx.blip(); break; }
        case EV.PIPS: { sfx.pip && sfx.pip(e.a); break; }
        case EV.NOAMMO: { sfx.deny(); break; }
        case EV.CRUISE: {
          if (e.a === -1) { hud.note(e.k, 'bad'); sfx.deny(); }
          else if (e.a === 1) { sfx.cruiseSpool && sfx.cruiseSpool(); }
          else if (e.a === 2) { trauma = Math.min(1, trauma + 0.25); flashK = Math.max(flashK, 0.12); sfx.boost && sfx.boost(); }
          else if (e.a === 0) { if (e.b === 2) { trauma = Math.min(1, trauma + 0.3); sfx.warp && sfx.warp(0.4); } if (e.k && e.k !== 'cz_cr_manual') hud.note(e.k, e.k === 'cz_cr_arrived' ? 'good' : 'bad'); }
          break;
        }
        case EV.BOARD: { if (e.s2 && e.s2.objective) hud.note('cz_hud_boarded', 'bad'); break; }
      }
    }
  }

  // ---- per-frame flight sync ---------------------------------------------------------------------------------------
  const lerp = (a, b, t) => a + (b - a) * t;
  function syncFlight(dt, alpha) {
    const ships = F.ships;
    for (let i = 0; i < ships.length; i++) {
      const s = ships[i];
      let v = V[i];
      const show = s.alive || (s.cap && s.dieT > 0);
      if (!show) { if (v && !(v.chain > 0)) { if (v.trail) v.trail = null; if (s.dieT <= 0) dropVisual(i); else v.g.visible = false; } continue; }
      if (!v || v.gen !== s.gen) { dropVisual(i); v = V[i] = shipVisual(s); }
      const g = v.g;
      g.position.set(lerp(s.ppos.x, s.pos.x, alpha), lerp(s.ppos.y, s.pos.y, alpha), lerp(s.ppos.z, s.pos.z, alpha));
      _q.set(s.pq.x, s.pq.y, s.pq.z, s.pq.w); _q2.set(s.q.x, s.q.y, s.q.z, s.q.w); g.quaternion.slerpQuaternions(_q, _q2, alpha);
      const warping = s.st === 11 && s.warpT > 0.25;
      g.visible = !v.dead && !warping && !(s.isPlayer && camMode === 'cockpit' && !deathCam && intro <= 0);
      // hit flash + damage scorch
      v.flash = Math.max(0, v.flash - dt * 7);
      const U = v.mat.userData.U; U.uFlash.value = v.flash * 1.6; U.uScorch.value = s.hullMax ? Math.max(0, 1 - s.hull / s.hullMax) * 0.9 : 0;
      if (s.ace) { U.uFlashCol.value.setRGB(1, 0.5, 0.3); }
      // engines
      const boost = s.boosting ? 1 : 0, thr = s.alive ? Math.max(0.12, s.thr) : 0;
      const pk = s.isPlayer && camMode === 'chase' ? 0.45 : 1;   // the chase camera looks straight down the player's exhaust
      for (const p of v.plumes) { const r = p.userData.r; const len = r * (1.6 + thr * 4 + boost * 7) * (s.engOk ? 1 : 0.3) * pk; p.scale.set(r * 0.75, r * 0.75, len); p.visible = g.visible && s.st !== 8; }
      // trails for small ships
      if (!s.cap && !s.isPlayer && s.alive && g.visible && s.spd > 40) {
        if (!v.trail) { const ec = KIT.PALETTES[s.isPlayer ? F_PLAYER : s.fac].engine; v.trail = fx.trailFor(i * 1000 + s.gen, [ec[0] * 0.5, ec[1] * 0.5, ec[2] * 0.5], s.isPlayer ? 0.7 : 0.9); }
        if (v.trail) {
          const e = s.cls.eng[0]; _v.set(e[0], e[1], e[2] + 0.5).applyQuaternion(g.quaternion).add(g.position);
          fx.trailPush(v.trail, _v.x, _v.y, _v.z, dt); v.trail.seen = true;
          v.trail.col[0] = v.trail.col[0]; v.trail.w = (s.isPlayer ? 0.55 : 0.8) * (1 + boost);
        }
      }
      // damage states: smoke < 50 %, fire < 25 %
      if (s.alive && s.hull < s.hullMax * 0.5 && !s.board) {
        v.smokeT -= dt;
        if (v.smokeT <= 0) {
          v.smokeT = s.hull < s.hullMax * 0.25 ? 0.03 : 0.08;
          const off = s.cap ? s.cls.rad * 0.5 : 1;
          _v.set((Math.random() - 0.5) * off, (Math.random() - 0.3) * off, (Math.random() - 0.2) * off).applyQuaternion(g.quaternion).add(g.position);
          fx.smoke.add(_v.x, _v.y, _v.z, s.vel.x * 0.5, s.vel.y * 0.5, s.vel.z * 0.5, 1.6, s.cls.rad * 0.25, s.cls.rad * 0.9, 0.07, 0.065, 0.07, 0.5, 0.02, 0.02, 0.02, 0, 2);
          if (s.hull < s.hullMax * 0.25) fx.add.add(_v.x, _v.y, _v.z, s.vel.x * 0.7, s.vel.y * 0.7, s.vel.z * 0.7, 0.35, s.cls.rad * 0.18, s.cls.rad * 0.4, 5, 2.2, 0.6, 1, 1, 0.2, 0.05, 0, 1);
        }
      }
      // capital turrets aim (world aim dir -> turret base space)
      for (const t of v.turrets) {
        const alive = s.subHp && s.subHp[t.k] > 0;
        if (alive) {
          const a = s.turAim[t.k]; t.base.getWorldQuaternion(_q); _v.set(a.x, a.y, a.z).applyQuaternion(_q.invert());
          t.head.rotation.y = Math.atan2(-_v.x, -_v.z); t.gun.rotation.x = Math.max(-0.1, Math.min(1.3, Math.atan2(_v.y, Math.hypot(_v.x, _v.z))));
        } else { t.gun.rotation.x = -0.35; t.head.rotation.z = 0.3; if (Math.random() < dt * 8) { t.base.getWorldPosition(_v); fx.smoke.add(_v.x, _v.y, _v.z, 0, 4, 0, 2, 3, 9, 0.06, 0.06, 0.06, 0.5, 0.02, 0.02, 0.02, 0, 2); } }
      }
      // capital death: chain explosions, then the big one
      if (v.chain > 0) {
        v.chainT -= dt;
        if (v.chainT <= 0) {
          v.chain--; v.chainT = 0.22 + Math.random() * 0.2;
          const hs = s.hs || [[0, 0, 0, s.cls.rad * 0.3]], h = hs[(Math.random() * hs.length) | 0];
          localToWorld(s, h[0] + (Math.random() - 0.5) * h[3], h[1] + (Math.random() - 0.5) * h[3], h[2], W);
          fx.explosion(W.x, W.y, W.z, s.vel.x, s.vel.y, s.vel.z, 7 + Math.random() * 5, FAC_DEBRIS[s.fac] || FAC_DEBRIS[0], false);
          sfx.boomAt && sfx.boomAt(1, Math.max(0.2, 1 - camDist(W.x, W.y, W.z) / 3000), pan(W.x, W.y, W.z));
          shakeAt(W.x, W.y, W.z, 0.2);
          if (v.chain === 0) {
            fx.explosion(s.pos.x, s.pos.y, s.pos.z, s.vel.x, s.vel.y, s.vel.z, s.cls.rad * 0.45, FAC_DEBRIS[s.fac] || FAC_DEBRIS[0], true);
            fx.debris(s.pos.x, s.pos.y, s.pos.z, s.vel.x, s.vel.y, s.vel.z, 40, s.cls.rad * 0.5, FAC_DEBRIS[s.fac] || FAC_DEBRIS[0], 30, 0.7);
            fx.shock(s.pos.x, s.pos.y, s.pos.z, s.cls.rad * 9, 1.6, 1.0, 0.6, 1.3);
            sfx.boomAt ? sfx.boomAt(2.5, 1, pan(s.pos.x, s.pos.y, s.pos.z)) : sfx.boom(true);
            shakeAt(s.pos.x, s.pos.y, s.pos.z, 1); flashK = Math.max(flashK, 0.5);
            v.g.visible = false; v.chain = -1;
          }
        }
      }
    }
    // bolts
    fx.boltsBegin();
    for (const b of F.bolts) {
      if (!b.alive) continue;
      const hx = lerp(b.px, b.x, alpha), hy = lerp(b.py, b.y, alpha), hz = lerp(b.pz, b.z, alpha);
      const vl = Math.hypot(b.vx, b.vy, b.vz) || 1, L = b.len;
      const c = FAC_BOLT[b.fac] || FAC_BOLT[0];
      fx.bolt(hx, hy, hz, hx - b.vx / vl * L, hy - b.vy / vl * L, hz - b.vz / vl * L, c[0], c[1], c[2], b.kind === 2 ? 1.6 : b.kind === 1 ? 1.0 : 0.62);
    }
    fx.boltsEnd();
    // missiles
    let mi = 0;
    for (const m of F.msls) {
      if (!m.alive) continue;
      const mm = msls[mi++]; if (!mm) break;
      mm.visible = true; mm.position.set(lerp(m.ppos.x, m.pos.x, alpha), lerp(m.ppos.y, m.pos.y, alpha), lerp(m.ppos.z, m.pos.z, alpha));
      _v.set(m.vel.x, m.vel.y, m.vel.z).normalize(); _q.setFromUnitVectors(_v2.set(0, 0, -1), _v); mm.quaternion.copy(_q);
      fx.add.add(mm.position.x - _v.x * 1.6, mm.position.y - _v.y * 1.6, mm.position.z - _v.z * 1.6, 0, 0, 0, 0.08, 1.4, 0.6, 4, 2.6, 1.2, 1, 1, 0.3, 0.1, 0, 0);
      if (Math.random() < 0.6) fx.smoke.add(mm.position.x, mm.position.y, mm.position.z, (Math.random() - 0.5) * 3, (Math.random() - 0.5) * 3, (Math.random() - 0.5) * 3, 1.6, 0.8, 4, 0.5, 0.5, 0.52, 0.35, 0.3, 0.3, 0.32, 0, 2);
    }
    for (; mi < msls.length; mi++) msls[mi].visible = false;
    for (const f of F.flares) if (f.alive) fx.add.add(f.pos.x, f.pos.y, f.pos.z, f.vel.x, f.vel.y, f.vel.z, 0.12, 2.6, 1.2, 4, 2.2, 3.2, 1, 2, 0.5, 1, 0, 3);
    let pi = 0;
    for (const k of F.picks) { if (!k.alive) continue; const m = picks[pi++]; if (!m) break; m.visible = true; m.material = pkMats[k.kind] || pkMats[0]; m.position.set(k.pos.x, k.pos.y, k.pos.z); m.rotation.set(k.spin, k.spin * 1.3, 0); m.scale.setScalar(1 + Math.sin(k.spin * 6) * 0.15); }
    for (; pi < picks.length; pi++) picks[pi].visible = false;
    // tethers
    fx.beamsBegin(); let bi = 0;
    for (const s of ships) if (s.alive && s.tether && s.tether.alive) { const tp = s.cls.tether || [0, 0, -8]; localToWorld(s, tp[0], tp[1], tp[2], W); const t = s.tether; fx.beam(bi++, W.x, W.y, W.z, t.pos.x, t.pos.y, t.pos.z, cam, 2.6); }
  }

  // ---- camera rig ------------------------------------------------------------------------------------------------------------
  const off = new THREE.Vector3(), look = new THREE.Vector3(), up = new THREE.Vector3();
  function flightCamera(dt) {
    const p = F.player, pv = V[p.i];
    const fovT = (p.boosting ? 80 : 70) + (F.input.drift ? 4 : 0) + (F.retreat > 0 ? 25 : 0) + (F.cruise === 2 ? 14 * Math.min(1, p.spd / 1500) : 0);
    cam.fov += (fovT - cam.fov) * (1 - Math.exp(-4 * dt));
    if (deathCam) {
      deathCam.t += dt;
      const a = deathCam.t * 0.25, R = 90 + deathCam.t * 25;
      camPos.set(deathCam.x + Math.cos(a) * R, deathCam.y + 30 + deathCam.t * 6, deathCam.z + Math.sin(a) * R);
      cam.position.copy(camPos); cam.lookAt(deathCam.x, deathCam.y, deathCam.z); cockpit.visible = false;
      return;
    }
    if (!pv) return;
    const g = pv.g;
    if (intro > 0) {   // establishing shot: swing in from the side to the chase position
      intro -= dt;
      const k = 1 - Math.max(0, intro) / 3.2, e = k * k * (3 - 2 * k);
      off.set(lerp(38, 0, e), lerp(9, 3.4, e), lerp(-26, 15.5, e)).applyQuaternion(g.quaternion);
      cam.position.copy(g.position).add(off);
      up.set(0, 1, 0).applyQuaternion(g.quaternion);
      _v.set(0, 0, -30 * e).applyQuaternion(g.quaternion).add(g.position);
      cam.up.copy(up); cam.lookAt(_v); cam.up.set(0, 1, 0);
      camQ.copy(cam.quaternion); camPos.copy(cam.position);
      cockpit.visible = false;
      return;
    }
    if (camMode === 'cockpit') {
      off.set(0, 2.3, -2.9).applyQuaternion(g.quaternion);
      cam.position.copy(g.position).add(off); cam.quaternion.copy(g.quaternion);
      cockpit.visible = true;
      cockpit.position.set(-p.w.y * 0.02, -p.w.x * 0.03, 0); cockpit.rotation.z = p.w.z * 0.02;
      camPos.copy(cam.position); camQ.copy(cam.quaternion);
    } else {
      cockpit.visible = false;
      const dist = 16 + (p.boosting ? 4 : 0) + p.spd * 0.004;
      off.set(0, 4.3, dist).applyQuaternion(g.quaternion);
      const target = _v.copy(g.position).add(off);
      // tight follow (critically damped) so the ship stays framed under the reticle
      camPos.lerp(target, 1 - Math.exp(-14 * dt));
      if (camPos.distanceTo(target) > 60) camPos.copy(target);
      camQ.slerp(g.quaternion, 1 - Math.exp(-9 * dt));
      cam.position.copy(camPos); cam.quaternion.copy(camQ);
      // keep the camera level with the ship's heading: the hull rides in the lower third, the reticle above it
      cam.rotateX(0.015);
    }
  }
  function applyShake(dt) {
    trauma = Math.max(0, trauma - dt * 1.4);
    const k = trauma * trauma;
    if (k <= 0.0001) return;
    const t = time * 32;
    const nx = Math.sin(t * 1.3) * 0.6 + Math.sin(t * 2.7 + 1) * 0.4, ny = Math.sin(t * 1.7 + 2) * 0.6 + Math.sin(t * 3.1) * 0.4, nz = Math.sin(t * 1.1 + 4);
    if (camMode !== 'cockpit') cam.position.add(off.set(nx * k * 0.9, ny * k * 0.7, 0).applyQuaternion(cam.quaternion));
    cam.rotateX(ny * k * 0.02); cam.rotateY(nx * k * 0.02); cam.rotateZ(nz * k * 0.03);
    if (camMode === 'cockpit') { cockpit.position.x += nx * k * 0.03; cockpit.position.y += ny * k * 0.03; }
  }
  // dev / codex view: a lineup of the procedural ship kit in system light (window.__cz.showcase([...]))
  let show = null;
  window.__cz.showcase = (list, o = {}) => {
    if (show) { show.g.removeFromParent(); show.mats.forEach((m) => m.dispose()); show = null; }
    if (!list) return;
    const g = new THREE.Group(), mats = []; let x = 0; const items = [];
    for (const it of list) {
      const [ck, fac, ace] = Array.isArray(it) ? it : [it, CLS[it].fac != null ? CLS[it].fac : F_PLAYER, false];
      const m = KIT.hullMaterial(THREE, { panel: CLS[ck].cap ? 0.18 : 0.6 }); mats.push(m);
      const mesh = new THREE.Mesh(KIT.shipGeometry(THREE, ck, fac, ace), m);
      const w = CLS[ck].rad * 2.2; x += w / 2; mesh.position.set(x, 0, 0); x += w / 2 + 4; mesh.rotation.y = o.yaw != null ? o.yaw : 2.45;
      for (const e of CLS[ck].eng || []) { const ec = KIT.PALETTES[fac].engine, pl = fx.plume(ec[0], ec[1], ec[2]); pl.position.set(e[0], e[1], e[2] - 0.2); pl.scale.set(e[3] * 0.75, e[3] * 0.75, e[3] * 3.5); mesh.add(pl); }
      g.add(mesh); items.push(mesh);
    }
    items.forEach((m) => { m.position.x -= x / 2; });
    const base = hubShip ? hubShip.position : new THREE.Vector3();
    g.position.copy(base).add(new THREE.Vector3(0, 0, -120)); space.near.add(g);
    show = { g, mats, span: x, dist: o.dist || Math.max(30, x * 0.75), elev: o.elev != null ? o.elev : 0.25 };
    if (hubShip) hubShip.visible = false;
  };
  function hubCamera(dt) {
    if (show) {
      hubAng += dt * 0.08;
      const c = show.g.position, a = Math.sin(hubAng) * 0.35;
      cam.position.set(c.x + Math.sin(a) * show.dist, c.y + show.dist * show.elev, c.z + Math.cos(a) * show.dist);
      cam.lookAt(c); cam.fov += (40 - cam.fov) * (1 - Math.exp(-4 * dt)); cockpit.visible = false; return;
    }
    hubAng += dt * 0.025;
    const bp = space.sys.bp;
    const tgt = hubShip ? hubShip.position : _v.set(0, 0, -2000);
    const st = bp && bp.station ? bp.station.pos : [tgt.x, tgt.y, tgt.z];
    const R = 260;
    cam.position.set(tgt.x + Math.cos(hubAng) * R, tgt.y + 70 + Math.sin(hubAng * 0.7) * 30, tgt.z + Math.sin(hubAng) * R);
    look.set(lerp(tgt.x, st[0], 0.55), lerp(tgt.y, st[1], 0.55), lerp(tgt.z, st[2], 0.55));
    cam.lookAt(look);
    cam.fov += (62 - cam.fov) * (1 - Math.exp(-2 * dt));
    if (hubShip) { hubShip.rotation.z = -0.12 + Math.sin(time * 0.4) * 0.03; hubShip.position.y += Math.sin(time * 0.5) * 0.02; }
    cockpit.visible = false;
  }

  // ---- render ------------------------------------------------------------------------------------------------------------------
  function render() {
    farCam.quaternion.copy(cam.quaternion); farCam.fov = cam.fov; farCam.position.set(0, 0, 0); farCam.updateProjectionMatrix(); cam.updateProjectionMatrix();
    if (composer) { grade.uniforms.uTime.value = time % 100; composer.render(); }
    else { renderer.autoClear = false; renderer.setRenderTarget(null); renderer.clear(); renderer.render(space.far, farCam); renderer.clearDepth(); renderer.render(space.near, cam); }
  }

  function frame(state, model) {
    const now = performance.now(), rawDt = (now - last) / 1000, dt = Math.min(0.05, rawDt); last = now;
    time += dt;
    const ph = state ? state.phase : 'loading';
    // perf bookkeeping (frame-to-frame time)
    perf.frames++; perf.hist[perf.hi++ % perf.hist.length] = rawDt * 1000; perf.ema += (rawDt * 1000 - perf.ema) * 0.05;
    const bp = model && model.bp;
    // jump tunnel: fade in, swap the system at the peak, fade out
    if (tunnelT >= 0) {
      tunnelT += dt;
      const k = tunnelT < 0.6 ? tunnelT / 0.6 : tunnelT < 1.4 ? 1 : Math.max(0, 1 - (tunnelT - 1.4) / 0.9);
      space.tunnel.visible = k > 0.01; space.tunnelMat.uniforms.uK.value = k; space.tunnelMat.uniforms.uT.value = tunnelT;
      if (tunnelT >= 0.6 && pendingBp) { const b = pendingBp; pendingBp = null; doBuild(b); flashK = 0.8; }
      if (tunnelT > 2.3) { tunnelT = -1; space.tunnel.visible = false; }
    }
    if (tunnelT < 0) {   // cruise: a faint streak tunnel that grows with speed
      const ck = F && ph === 'combat' && F.cruise === 2 ? Math.min(1, (F.player.spd - 400) / 1800) : 0;
      cruiseK += (Math.max(0, ck) - cruiseK) * (1 - Math.exp(-3 * dt));
      space.tunnel.visible = cruiseK > 0.01; space.tunnelMat.uniforms.uK.value = cruiseK * 0.22; space.tunnelMat.uniforms.uT.value += dt * (0.3 + cruiseK);
    }
    if (bp) ensureSystem(bp, ph === 'hub' || ph === 'debrief');
    ui.show(ph, state ? state.screen : 'bridge');
    // ---- mode switch
    const flying = ph === 'combat' && model && model.flight;
    if (flying && model.flight !== F) {
      F = model.flight; evCur = F.evHead; clearVisuals(); deathCam = null; intro = F.briefT > 0 ? 3.2 : 0; trauma = 0;
      space.setNavs(F.mission.nav, F.mission.jump);
      camPos.set(0, 0, 0); camQ.identity();
      hud.begin(F, model);
      // prewarm: build every hull this sortie can field and compile the programs now, not on first contact
      const fset = [F_PLAYER, F.mission.ef, F.mission.offer];
      for (const ck of Object.keys(CLS)) { const C = CLS[ck]; if (ck === 'station' || ck === 'beacon') continue; for (const f of fset) if (C.fac === f || (ck === 'lucciola' && f === F_PLAYER) || ck === 'hauler') KIT.shipGeometry(THREE, ck, f, false); }
      try { renderer.compile(space.near, cam); } catch {}
      sfx.startDrone(); sfx.engineStart && sfx.engineStart();
      if (hubShip) hubShip.visible = false;
    }
    if (ph !== 'combat' && ph !== 'debrief' && F) {   // back at the hub
      F = null; clearVisuals(); space.setNavs(null, null); hud.stop(); sfx.stopDrone(); sfx.engineStop && sfx.engineStop(); if (hubShip) hubShip.visible = true; deathCam = null;
    }
    if (ph === 'combat' && F) {
      const paused = !!(model.paused || hud.paused());
      F.paused = paused;
      const steps = paused ? 0 : simFrame(F, rawDt);
      perf.simSteps += steps;
      events();
      syncFlight(dt, F.alpha);
      flightCamera(dt);
      applyShake(dt);
      cam.updateMatrixWorld(); cam.updateProjectionMatrix();
      hud.frame(F, cam, dt, paused);
      // audio: engine follows throttle/boost
      if (sfx.engineSet) sfx.engineSet(F.player.alive ? F.player.thr : 0, F.player.boosting, paused);
      ui.combat(state);
    } else if (ph === 'debrief' && F) {
      syncFlight(dt, 1);
      const p = F.player;
      hubAng += dt * 0.1;
      if (!deathCam) { cam.position.set(p.pos.x + Math.cos(hubAng) * 40, p.pos.y + 12, p.pos.z + Math.sin(hubAng) * 40); cam.lookAt(p.pos.x, p.pos.y, p.pos.z); }
      else { deathCam.t += dt; cam.position.set(deathCam.x + Math.cos(hubAng) * 160, deathCam.y + 50, deathCam.z + Math.sin(hubAng) * 160); cam.lookAt(deathCam.x, deathCam.y, deathCam.z); }
      hud.hide(); ui.debrief(state, F);
      if (sfx.engineSet) sfx.engineSet(0, false, true);
    } else {
      if (ph === 'hub' && lastPh !== 'hub') sfx.hubMusic && sfx.hubMusic(!!(model && model.run && model.run.sector > 5));
      if ((ph === 'loading' || ph === 'new_run' || ph === 'conflict') && ph !== lastPh && sfx.playTrack) sfx.playTrack('theme', 0.14);
      hubCamera(dt); hud.hide();
      ui.paint(model, state || { phase: 'loading', screen: 'bridge', focus: {}, marketCol: 0, marketQty: 1, target: -1, clock: 0 });
    }
    // camera velocity (dust streaks)
    lastPh = ph;
    camVel.copy(cam.position).sub(lastCamPos).divideScalar(Math.max(dt, 1e-3)); lastCamPos.copy(cam.position);
    if (camVel.lengthSq() > 4e6) camVel.setScalar(0);
    fx.update(ph === 'combat' && F && F.hitStop > 0 ? dt * 0.08 : dt, cam);
    if (F && ph === 'combat') fx.trailsBuild(1); else fx.trailsBuild(0.6);
    space.update(dt, cam, camVel, { stretch: F && F.cruise === 2 ? 0.03 : 0.05, dust: F ? 0.32 : 0.2, pxScale: canvas.height / 900, flare: 1 });
    // grade
    flashK = Math.max(0, flashK - dt * 1.8); hitAber = Math.max(0, hitAber - dt * 2.5);
    if (grade) {
      const p = F && F.player, hullF = p ? Math.max(0, p.hull / p.hullMax) : 1;
      grade.uniforms.uFlash.value = flashK * 0.6 + (tunnelT >= 0 && tunnelT > 0.5 && tunnelT < 0.75 ? 0.6 : 0);
      grade.uniforms.uAber.value = 0.004 + hitAber * 0.03 + (p && p.boosting ? 0.006 : 0) + (tunnelT >= 0 ? 0.02 : 0) + cruiseK * 0.012;
      grade.uniforms.uDmg.value = p && ph === 'combat' ? Math.max(0, 0.35 - hullF) * 2.2 + (p.alive ? 0 : 0.3) : 0;
      grade.uniforms.uSat.value = p && ph === 'combat' ? 0.75 + 0.25 * Math.min(1, hullF * 2.5) : 1;
    }
    render();
    adaptResolution(ph);
  }

  // dynamic resolution: hold ~60 fps by trading pixels, never below 60 % of the tier's scale
  let adaptT = 0;
  function adaptResolution(ph) {
    adaptT += 1;
    if (adaptT < 90 || ph !== 'combat') return;
    adaptT = 0;
    const base = renderer.getPixelRatio(), maxS = Math.min(Q.scale, 1.35) * (tier.name === 'high' ? dpr : 1), minS = maxS * 0.6;
    let ns = base;
    if (perf.ema > 21 && base > minS + 0.01) ns = Math.max(minS, base * 0.85);
    else if (perf.ema < 13 && base < maxS - 0.01) ns = Math.min(maxS, base * 1.1);
    if (Math.abs(ns - base) > 0.01) { renderer.setPixelRatio(ns); perf.scale = ns; fit(canvas.clientWidth || 960, canvas.clientHeight || 540); }
  }

  function dispose() {
    clearVisuals(); fx.dispose(); space.dispose(); if (composer) composer.dispose(); hud.dispose(); ui.dispose();
    mslGeo.dispose(); mslMat.dispose(); pkGeo.dispose(); pkMats.forEach((m) => m.dispose()); cockpit.geometry.dispose(); cockpit.material.dispose();
    if (hubShip) hubShip.material.dispose();
    sfx.stopDrone(); sfx.engineStop && sfx.engineStop();
    renderer.dispose();
  }
  return { frame, resize: fit, dispose, mode: 'webgl', tier: tier.name };
}

// ================================= 2D fallback (no WebGL): tactical top-down view ===================================
function build2D(canvas, ui, hud) {
  const ctx = canvas.getContext('2d'); let last = performance.now(), F = null;
  function frame(state, model) {
    const now = performance.now(), dt = Math.min(0.05, (now - last) / 1000); last = now;
    const ph = state ? state.phase : 'loading', W = canvas.width, H = canvas.height;
    ui.show(ph, state ? state.screen : 'bridge');
    ctx.fillStyle = '#04060c'; ctx.fillRect(0, 0, W, H);
    if (ph === 'combat' && model && model.flight) {
      if (F !== model.flight) { F = model.flight; hud.begin(F, model); }
      F.paused = !!(model.paused || hud.paused());
      simFrame(F, dt);
      const p = F.player, cx = W / 2, cy = H / 2, k = Math.min(W, H) / 5000;
      for (const s of F.ships) {
        if (!s.alive) continue;
        const x = cx + (s.pos.x - p.pos.x) * k, y = cy + (s.pos.z - p.pos.z) * k;
        ctx.fillStyle = s.isPlayer ? '#ffd866' : s.team === TEAM_P ? '#7cf29a' : '#ff5a4a';
        ctx.beginPath(); ctx.arc(x, y, s.cap ? 6 : 3, 0, 7); ctx.fill();
      }
      ctx.fillStyle = '#ffcc66'; for (const b of F.bolts) if (b.alive) ctx.fillRect(cx + (b.x - p.pos.x) * k, cy + (b.z - p.pos.z) * k, 2, 2);
      hud.frame(F, null, dt, F.paused);
      ui.combat(state);
    } else if (ph === 'debrief') { hud.hide(); ui.debrief(state, F); }
    else { hud.hide(); ui.paint(model, state || { phase: 'loading', screen: 'bridge', focus: {}, marketCol: 0, marketQty: 1, target: -1, clock: 0 }); }
  }
  return { frame, resize() {}, dispose() { hud.dispose(); ui.dispose(); }, mode: '2d' };
}

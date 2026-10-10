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
import { stationBlueprint, planetAt, planetQuat, universeTime } from '/apps/games/games/stelle/world.js';
import { surfaceFor } from '/apps/games/games/stelle/planet.js';
import { AU, setAU, atmoParams, ambientAt, ambientRes } from '/apps/games/games/stelle/atmo.js';
import { SURF, groundAt, REST } from '/apps/games/games/stelle/surface.js';
import { createGalaxy } from '/apps/games/games/stelle/galaxy.js';
import { entriesOf, CATS } from '/apps/games/games/constellations-codex.js';
import I18N from '/nucleo-i18n.js';
const tr = (() => { const t = I18N.scope('games'); return (k, v) => { const s = t(k, v); return s === k ? '' : s; }; })();

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
// engine exhaust colour per faction (linear HDR-ish; the nozzle core itself burns white)
const FAC_ENGINE = { [F_PLAYER]: [1.0, 0.62, 0.3], [F_GILDA]: [0.42, 0.66, 1.3], [F_RELITTI]: [1.25, 0.46, 0.14], [F_CUSTODI]: [1.15, 0.78, 0.32], 3: [0.66, 0.42, 1.35] };
const FAC_SHIELD = { [F_PLAYER]: [0.4, 1.4, 0.8], [F_GILDA]: [0.45, 0.8, 1.8], [F_RELITTI]: [1.6, 0.7, 0.25], [F_CUSTODI]: [1.6, 1.2, 0.5], 3: [0.7, 0.9, 1.8] };

export async function createRenderer(canvas, api) {
  let gl = null;
  try { gl = canvas.getContext('webgl2', { antialias: false, alpha: false, powerPreference: window.__czPowerPref || 'high-performance', stencil: false, preserveDrawingBuffer: !!window.__czPreserve }); } catch {}
  let THREE = null, POST = null, KIT = null, SPACE = null, FX = null, TERRAIN = null;
  if (gl) {
    try {
      [THREE, POST, KIT, SPACE, FX, TERRAIN] = await Promise.all([import('/apps/games/vendor/three.module.min.js'), import('/apps/games/games/stelle/post.js'),
        import('/apps/games/games/stelle/kit.js'), import('/apps/games/games/stelle/space.js'), import('/apps/games/games/stelle/fx.js'), import('/apps/games/games/stelle/terrain.js')]);
    } catch (e) { console.warn('[costellazioni] 3D modules unavailable -> 2D', e); THREE = null; }
  }
  const ui = makeUI(canvas);
  const hud = createHud(canvas);
  return THREE ? build3D({ THREE, POST, KIT, SPACE, FX, TERRAIN }, canvas, gl, ui, hud) : build2D(canvas, ui, hud);
}

// ================================= WebGL ===========================================================================
function build3D(M, canvas, gl, ui, hud) {
  const { THREE, POST, KIT, SPACE, FX, TERRAIN } = M;
  const tier = pickTier(gl), Q = TIERS[tier.name];
  const renderer = new THREE.WebGLRenderer({ canvas, context: gl, antialias: false, powerPreference: 'high-performance' });
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  let scale = Math.min(Q.scale, tier.name === 'high' ? 1.35 : 1) * Math.min(1, dpr);
  renderer.setPixelRatio(scale * (tier.name === 'high' ? dpr : 1) || 1);
  renderer.outputColorSpace = THREE.SRGBColorSpace; renderer.toneMapping = THREE.ACESFilmicToneMapping; renderer.toneMappingExposure = 1.05;
  renderer.setClearColor(0x000000, 1);
  // the near camera reaches past a whole world (terrain to the horizon from orbit); depth precision is set by the
  // near plane, not the far one, so this costs nothing up close
  const cam = new THREE.PerspectiveCamera(70, 1.6, 0.4, 400000), farCam = new THREE.PerspectiveCamera(70, 1.6, 20, 600000);
  const space = SPACE.createSpace(THREE, renderer, Q);
  space.near.add(cam); cam.add(space.flares); cam.add(space.tunnel);
  const camFill = new THREE.DirectionalLight(0x8fa6c8, 0.55); camFill.position.set(0.3, 0.6, 1); cam.add(camFill); cam.add(camFill.target); camFill.target.position.set(0, 0, -1);   // keeps hulls readable against the dark
  const fx = FX.createFx(THREE, space.near, Q);
  const WORLD = TERRAIN.createWorld(THREE, renderer, space.near, tier.name);
  WORLD.compileTarget = () => (post ? post.target : null);   // a world's programs are compiled for the target they draw into
  space.near.add(WORLD.group);
  const GAL = createGalaxy(THREE, canvas, tr), emptyScene = new THREE.Scene();
  let mapMode = false;

  // ---- post chain (stelle/post.js): one multisampled scene target, a dual-filter bloom, sun shafts, one final pass -----------
  let post = null, bloom = null, grade = null;
  function setupPost() {
    if (!Q.bloom) { if (post) { post.dispose(); post = null; } bloom = grade = null; return; }
    if (!post) post = POST.createPost(THREE, renderer, Q);
    const sz = renderer.getDrawingBufferSize(new THREE.Vector2());
    post.setSize(sz.x, sz.y); bloom = post.bloom; grade = post.grade; bloom.strength = 0.34;
  }

  // ---- ship visuals bound to sim slots ----------------------------------------------------------------------------
  const V = [];               // per sim ship slot
  const turretGeo = {};
  const _v = new THREE.Vector3(), _v2 = new THREE.Vector3(), _q = new THREE.Quaternion(), _q2 = new THREE.Quaternion(), _m = new THREE.Matrix4(), _c = new THREE.Color();
  const W = { x: 0, y: 0, z: 0 };
  function shipVisual(s) {
    const g = new THREE.Group();
    const ace = s.ace ? (s.aceId >= 0 ? s.aceId : true) : false;
    const mat = KIT.hullMaterial(THREE, { metal: s.cap ? 0.55 : 0.5, rough: 0.44, panel: s.cap ? 0.18 : 0.6, env: 1.0 });
    let body = null, pods = null;
    if (s.isPlayer && KIT.shipParts) {   // the courier's nacelles swivel on their pylons
      const P = KIT.shipParts(THREE, 'lucciola', F_PLAYER, false);
      body = new THREE.Mesh(P.body, mat); g.add(body); pods = [];
      for (const part of P.parts) {
        const m = new THREE.Mesh(part.geo, mat); m.position.set(part.pivot[0], part.pivot[1], part.pivot[2]); g.add(m);
        const e = (s.cls.eng || []).find((x) => Math.sign(x[0]) === Math.sign(part.pivot[0])) || s.cls.eng[0];
        pods.push({ m, pivot: part.pivot, eng: e });
      }
    } else {
      const geo = s.ck === 'station' || s.ck === 'beacon' ? null : KIT.shipGeometry(THREE, s.ck, s.isPlayer ? F_PLAYER : s.fac, ace);
      if (geo) { body = new THREE.Mesh(geo, mat); g.add(body); if (ace !== false) body.scale.setScalar(1.15); }
    }
    const plumes = s.cls.eng || [];
    const ec = FAC_ENGINE[s.isPlayer ? F_PLAYER : s.fac] || FAC_ENGINE[F_GILDA];
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
    g.traverse((o) => { if (o.isMesh) o.layers.enable(WORLD.SHADOW_LAYER); });   // hulls cast into the world's shadow map
    space.near.add(g);
    return { gen: s.gen, g, body, mat, plumes, ec, pods, podA: 0.4, turrets, trail: null, smokeT: 0, flash: 0, dead: false, chain: 0, chainT: 0, warpShown: false, rad: s.cls.rad };
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
  let relightCam = 0, landAng = 0; const entOff = new THREE.Vector3();
  let lastPh = '', cruiseK = 0, expo = 1.05, baseFov = 70, departT = -1, arriveHub = -1, fadeK = 0, cineK = 0, lastModel = null, jumpK = 0, codexShown = '';
  const hubPark = new THREE.Vector3(), _fwd = new THREE.Vector3();
  const SPO = { stretch: 0.05, dust: 0.2, pxScale: 1, flare: 1, farPos: null };   // space.update options, reused every frame
  let bpKey = '', pendingBp = null, tunnelT = -1, flashK = 0, hitAber = 0, sysReady = false, hubAng = 0, deathCam = null, intro = 0;
  const camPos = new THREE.Vector3(), camQ = new THREE.Quaternion(), camVel = new THREE.Vector3(), lastCamPos = new THREE.Vector3();
  const perf = { frames: 0, ms: 0, ema: 16.7, worst: 0, hist: new Float32Array(240), hi: 0, simSteps: 0, scale, tier: tier.name, auto: tier.auto, renderer: pickTier.renderer || '',
    work: new Float32Array(240), wi: 0, cpu: 0 };   // work: frame cost with the GPU waited for (tests set __czSync; vsync hides it otherwise)
  const SYNC_PX = new Uint8Array(4);
  window.__cz = window.__cz || {}; window.__cz.perf = perf;
  window.__cz.r3d = { get post() { return post; }, get bloom() { return bloom; }, get grade() { return grade; }, renderer, cam, space, fx, get world() { return WORLD; }, get worldIdx() { return wIdx; }, setCamera: (m) => { camMode = m; } };
  hud.setOptions({ camera: () => camMode, setCamera: (m) => { camMode = m; }, quality: () => (tier.auto ? 'auto' : tier.name), setQuality: (n) => { setQuality(n); } });

  function fit(w, h) {
    w = Math.max(2, w | 0); h = Math.max(2, h | 0);
    renderer.setSize(w, h, false);
    cam.aspect = farCam.aspect = w / h; cam.updateProjectionMatrix(); farCam.updateProjectionMatrix();
    // portrait screens: widen the vertical field so the horizontal one stays near 50 degrees
    baseFov = w >= h ? 70 : Math.min(92, Math.max(70, 2 * Math.atan(Math.tan(25 * Math.PI / 180) / (w / h)) * 180 / Math.PI));
    setupPost();
    hud.resize(w, h); GAL.resize(w, h);
  }
  fit(canvas.clientWidth || 960, canvas.clientHeight || 540);

  function ensureSystem(bp, animate) {
    if (!bp || (pendingBp && pendingBp.key === bp.key)) return;
    if (bp.key === bpKey) { if (bp !== space.sys.bp && bp.beacon && space.sys.bp && space.sys.bp.beacon && !!bp.beacon.lit !== !!space.sys.bp.beacon.lit && !F) doBuild(bp); return; }
    if (animate && sysReady && tunnelT < 0 && departT < 0) { pendingBp = bp; departT = 0; if (sfx.boost) sfx.boost(); return; }
    doBuild(bp);
  }
  // golden threads from this system's beacon toward the other lit beacons of the sector (directions on the map plane)
  function updateThreads(model, extraLit = 0) {
    const r = model && model.run, S = model && model.sector; if (!r || !S || !space.sys.bp || !space.sys.bp.beacon) return;
    const i = r.sys, lit = (r.beacon_lit >>> 0) | extraLit, dirs = [];
    if (((lit >>> i) & 1) === 0) { space.beaconFx({ threads: [] }); return; }
    for (let j = 0; j < S.length; j++) if (j !== i && S[j].beacon && ((lit >>> j) & 1)) { const dx = S[j].x - S[i].x, dz = S[j].y - S[i].y, l = Math.hypot(dx, dz) || 1; dirs.push([dx / l, 0.12, dz / l]); }
    space.beaconFx({ threads: dirs });
  }
  function doBuild(bp) {
    switchWorld(-1);
    space.build(bp); bpKey = bp.key; sysReady = true;
    if (hubShip) { hubShip.removeFromParent(); hubShip.material.dispose(); hubShip = null; }
    hubShip = new THREE.Mesh(KIT.shipGeometry(THREE, 'lucciola', F_PLAYER, false), KIT.hullMaterial(THREE, {})); space.near.add(hubShip);
    // parked off the docking bay, nose out (or, in Echo space, hanging in the dark)
    const st = bp.station, d = st && st.dock;
    if (d) { hubShip.position.set(st.pos[0] + d.p[0] + d.n[0] * 220, st.pos[1] + d.p[1] + d.n[1] * 220 + 24, st.pos[2] + d.p[2] + d.n[2] * 220); hubShip.lookAt(hubShip.position.x - d.n[0], hubShip.position.y - d.n[1], hubShip.position.z - d.n[2]); hubShip.rotateZ(-0.1); }
    else if (st) { hubShip.position.set(st.pos[0] + 520, st.pos[1] + 60, st.pos[2] + 900); hubShip.rotation.set(0.05, 0.6, -0.12); }
    else { hubShip.position.set(-300, 180, 1400); hubShip.rotation.set(0.05, 2.6, -0.12); }   // Echo space: hanging in the open, the home world ahead
    hubPark.copy(hubShip.position);
    hubShip.visible = !F;
    updateThreads(lastModel);
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
        case EV.WARPOUT: { if (s && s.isPlayer) { _fwd.set(0, 0, -1).applyQuaternion(V[s.i] ? V[s.i].g.quaternion : cam.quaternion); fx.warp(e.x, e.y, e.z, _fwd.x, _fwd.y, _fwd.z, false); } else fx.warp(e.x, e.y, e.z, 0, 0, -1, s && s.cap); const v = s && V[s.i]; if (v) v.g.visible = false; if (s && s.isPlayer) { sfx.warp && sfx.warp(1); tunnelT = 0; pendingBp = null; flashK = Math.max(flashK, 0.5); } break; }
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
        case EV.DOCK: {
          if (e.a === 1) { sfx.confirm && sfx.confirm(); hud.note('cz_hud_dock_ok', 'good'); }
          else if (e.a === -1) { if (e.k) hud.note(e.k, 'bad'); sfx.deny(); }
          else if (e.a === -2) hud.note('cz_hud_dock_cancel', 'info');
          else if (e.a === 4) { sfx.clang && sfx.clang(); trauma = Math.min(1, trauma + 0.15); }
          else if (e.a === 5) { hud.note('cz_hud_undocked', 'good'); sfx.boost && sfx.boost(); }
          break;
        }
        case EV.JUMP: { if (e.a === 1) { sfx.cruiseSpool && sfx.cruiseSpool(); sfx.warp && sfx.warp(0.3); hud.note('cz_hud_jump_spool', 'info'); } else if (e.a === -1) { hud.note(e.k || 'cz_cr_blocked', 'bad'); sfx.deny(); } break; }
        case EV.CHARGE: { if (e.a >= 0) space.beaconFx({ charge: e.a }); else if (e.k) { hud.note(e.k, 'bad'); sfx.deny(); } break; }
        case EV.RELIGHT: {
          space.beaconFx({ charge: 1, burst: true }); updateThreads(lastModel, 1 << (lastModel && lastModel.run ? lastModel.run.sys : 0)); relightCam = 5;
          fx.shock(e.x, e.y, e.z, 2600, 0.9, 0.65, 0.3, 2.2); fx.shock(e.x, e.y, e.z, 900, 1.2, 1.0, 0.6, 1.1); fx.flash(e.x, e.y, e.z, 1, 0.8, 0.45, 300, 9000, 2.5);
          flashK = 0.55; if (grade) grade.uniforms.uFlashCol.value.setRGB(1.0, 0.82, 0.5); trauma = Math.min(1, trauma + 0.8);
          sfx.warp && sfx.warp(1.4); sfx.victory && sfx.victory();
          hud.banner && hud.banner(tr('cz_hud_relit'), '#ffd66b', 4.5);
          break;
        }
        case EV.SCAN: { if (e.v && (e.k === 'planet' || e.k === 'moon')) { hud.note('cz_hud_scanned', 'good', { name: e.v.name }); sfx.lock && sfx.lock(); } break; }
        case EV.SURF: surfEvent(e); break;
        case EV.DISCOVER: { hud.banner && hud.banner(tr('cz_site_' + e.k), '#ffd66b', 4); hud.discovered && hud.discovered(e); sfx.victory && sfx.victory(); flashK = Math.max(flashK, 0.12); break; }
        case EV.LOOT: { hud.banner && hud.banner(tr('cz_hud_relic'), '#ffe7a8', 3.5); sfx.powerup && sfx.powerup('shield'); fx.flash(e.x, e.y, e.z, 1, 0.8, 0.45, 3, 200, 1.2); break; }
        case EV.GROUND: groundEvent(e); break;
        case EV.SCANPING: { hud.ping && hud.ping(e.b, e.a); sfx.lock && sfx.lock(); break; }
        case EV.TORP: { sfx.missile && sfx.missile(); if (e.s2 && e.s2.isPlayer) sfx.missileWarn && sfx.missileWarn(); break; }
        case EV.PHASE: { fx.add.add(e.x, e.y, e.z, 0, 0, 0, 0.35, 4, 26, 0.6, 1.6, 2.6, 1, 0.3, 0.2, 1.2, 0, 0); fx.shock(e.x, e.y, e.z, 40, 0.5, 1.2, 2.2, 0.35); if (camDist(e.x, e.y, e.z) < 1500) sfx.lockTone && sfx.lockTone(e.a ? 0.9 : 0.4); break; }
        case EV.CHORD: { fx.muzzle(e.x, e.y, e.z, 0, 0, 0, 0.6, 1.4, 2.4, 14); if (camDist(e.x, e.y, e.z) < 2600) { sfx.lock && sfx.lock(); } break; }
      }
    }
  }

  // one ship's exhaust (nozzles from its class hardpoints, in the group's frame); also drives the hull's engine glow
  const _ax = new THREE.Vector3();
  function enginePlumes(g, engs, ec, thr, boost, ok, lenK, U, world = false) {
    if (world) { g.updateWorldMatrix(true, false); _ax.set(0, 0, 1).transformDirection(g.matrixWorld); } else _ax.set(0, 0, 1).applyQuaternion(g.quaternion);
    const k = (0.32 + thr * 0.7 + boost * 0.75) * ok, L = (1.3 + thr * 3.4 + boost * 6.5) * ok * lenK, sc = world ? g.matrixWorld.getMaxScaleOnAxis() : g.scale.x;
    for (let i = 0; i < engs.length; i++) {
      const e = engs[i];
      if (world) _v.set(e[0], e[1], e[2]).applyMatrix4(g.matrixWorld);
      else _v.set(e[0], e[1], e[2]).multiplyScalar(sc).applyQuaternion(g.quaternion).add(g.position);
      fx.plume(_v.x, _v.y, _v.z, _ax.x, _ax.y, _ax.z, e[3] * 0.62 * sc, e[3] * L * sc, ec[0], ec[1], ec[2], k, boost);
    }
    if (U && U.uEngine) U.uEngine.value = 0.3 + thr * 0.8 + boost * 0.9;
  }
  // the courier's nacelles: tilted down when slow (hover, docking), level at speed, a flick up under boost, leaning
  // into the pitch; the exhaust follows each pod (nozzle and axis turned about the pylon hinge)
  const _pq = new THREE.Quaternion(), _px = new THREE.Vector3(1, 0, 0);
  function podsStep(v, s, dt, thr, boost, vis, U) {
    const slow = s.spd < 45 || F.dock || F.undock;
    const goal = slow ? 0.62 : boost ? -0.08 : Math.max(-0.22, Math.min(0.3, 0.06 + s.w.x * 0.22));
    v.podA += (goal - v.podA) * (1 - Math.exp(-(slow ? 2 : 4) * dt));
    const g = v.g;
    for (const pd of v.pods) {
      pd.m.rotation.x = v.podA;
      if (!vis) continue;
      // nozzle in ship space: pivot + Rx(a) (eng - pivot); axis Rx(a) (0,0,1); then to world
      _pq.setFromAxisAngle(_px, v.podA);
      _v.set(pd.eng[0] - pd.pivot[0], pd.eng[1] - pd.pivot[1], pd.eng[2] - pd.pivot[2]).applyQuaternion(_pq).add(_v2.set(pd.pivot[0], pd.pivot[1], pd.pivot[2])).applyQuaternion(g.quaternion).add(g.position);
      _ax.set(0, 0, 1).applyQuaternion(_pq).applyQuaternion(g.quaternion);
      const ok = s.engOk ? 1 : 0.35, k = (0.32 + thr * 0.7 + boost * 0.75) * ok, L = (1.3 + thr * 3.4 + boost * 6.5) * ok * (camMode === 'chase' ? 0.8 : 1);
      fx.plume(_v.x, _v.y, _v.z, _ax.x, _ax.y, _ax.z, pd.eng[3] * 0.62, pd.eng[3] * L, v.ec[0], v.ec[1], v.ec[2], k, boost);
    }
    if (U && U.uEngine) U.uEngine.value = 0.3 + thr * 0.8 + boost * 0.9;
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
      g.visible = !v.dead && !warping && !(s.isPlayer && camMode === 'cockpit' && !deathCam && intro <= 0 && cineK < 0.5) && !(s.phaseT > 0 && Math.random() < 0.75);
      if (s.phaseT > 0) fx.add.add(g.position.x, g.position.y, g.position.z, s.vel.x * 0.3, s.vel.y * 0.3, s.vel.z * 0.3, 0.12, 5, 9, 0.4, 1.4, 2.4, 0.9, 0.2, 0.3, 1.2, 0, 0);
      // hit flash + damage scorch
      v.flash = Math.max(0, v.flash - dt * 7);
      const U = v.mat.userData.U; U.uFlash.value = v.flash * 1.6; U.uScorch.value = s.hullMax ? Math.max(0, 1 - s.hull / s.hullMax) * 0.9 : 0;
      if (s.ace) { U.uFlashCol.value.setRGB(1, 0.5, 0.3); }
      // engines: hot nozzle cores + exhaust that scales with throttle / boost / cruise
      const boost = s.boosting || (s.isPlayer && F.cruise === 2) ? 1 : 0, thr = s.alive ? Math.max(0.12, s.thr) : 0;
      if (v.pods) podsStep(v, s, dt, thr, boost, g.visible, U);
      else if (g.visible && s.st !== 8) enginePlumes(g, v.plumes, v.ec, thr, boost, s.engOk ? 1 : 0.35, s.isPlayer && camMode === 'chase' ? 0.8 : 1, U);
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
      mm.visible = !m.torp; mm.position.set(lerp(m.ppos.x, m.pos.x, alpha), lerp(m.ppos.y, m.pos.y, alpha), lerp(m.ppos.z, m.pos.z, alpha));
      if (m.torp) {
        fx.add.add(mm.position.x, mm.position.y, mm.position.z, 0, 0, 0, 0.05, 5.5, 5, 3.4, 2.4, 1.0, 1, 2, 1.2, 0.4, 0, 0);
        fx.add.add(mm.position.x, mm.position.y, mm.position.z, (Math.random() - 0.5) * 8, (Math.random() - 0.5) * 8, (Math.random() - 0.5) * 8, 0.7, 2.2, 0.4, 2.2, 1.4, 0.4, 0.8, 0.6, 0.2, 0.05, 0, 3, 0.03);
        continue;
      }
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
    for (const s of ships) if (s.alive && s.links && s.links.length && s.ck === 'choir') for (const o of s.links) if (o.alive) fx.beam(bi++, s.pos.x, s.pos.y, s.pos.z, o.pos.x, o.pos.y, o.pos.z, cam, 1.8, 0.35, 1.1, 1.9);
  }


  // ---- M3: the world below — terrain, sky, clouds, weather, flora and sites; the light of its air on everything ----------
  const amb = ambientRes(), SKYU = KIT.hullSky(THREE);
  let wIdx = -1, wA = null, wFade = 0, wExpo = 1, wAirK = 0;
  const _wc = [0, 0, 0], _wq = [0, 0, 0, 1], _wc0 = [0, 0, 0], _wqd = [0, 0, 0, 1], _wsun = [0, 0, 0], _wI = [0, 0, 0];
  const _qPrev = new THREE.Quaternion(), _qNow = new THREE.Quaternion(), _qD = new THREE.Quaternion();
  const sunBase = { col: new THREE.Color(), k: 3, set: false };
  const WO = { center: _wc, quat: _wq, amb, T: 0, fade: 0, shadow: [0, 0, 0, 0], head: { k: 0, pos: new THREE.Vector3(), dir: new THREE.Vector3() }, weather: null, sunDir: _wsun, atmo: null };
  function switchWorld(i) {
    if (i === wIdx) return;
    if (wIdx >= 0) { space.planetMode(wIdx, true); WORLD.clear(); }
    wIdx = i; wA = null; wFade = 0;
    if (sunBase.set) { space.sun.color.copy(sunBase.col); space.sun.intensity = sunBase.k; sunBase.set = false; }
    if (i < 0) { setAU(null, 0, 0, 0); SKYU.uEnvK.value = 1; SKYU.uSkyIrr.value.set(0, 0, 0); SKYU.uSkyRad.value.set(0, 0, 0); space.sys.group.visible = true; space.daylight(0); space.dispMat.uniforms.uCol.value.setScalar(0.32); wExpo = 1; return; }
    const bp = space.sys.bp, S = surfaceFor(bp, i), body = space.sys.planets.find((x) => x.idx === i && x.moonOf < 0);
    WORLD.set(S, { aux: body ? body.rt.aux.texture : null, surf: body ? body.rt.surf.texture : null });
    wA = atmoParams(S);
    space.planetMode(i, false);
    sunBase.col.copy(space.sun.color); sunBase.k = space.sun.intensity; sunBase.set = true;
    planetQuat(bp.planets[i], WO.T, _wq); _qPrev.set(_wq[0], _wq[1], _wq[2], _wq[3]);
    planetAt(bp.planets[i], i, WO.T, _wc0);
  }
  const sstep = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t); };
  function worldUpdate(dt) {
    const bp = space.sys.bp; if (!bp || !sysReady) return;
    const T = SPO.T; WO.T = T;
    // which world: the nearest one whose air (or ground, from orbit) the camera is close enough to draw
    let best = -1, bd = Infinity;
    if (!mapMode && (F || cxShowing() === false)) for (let i = 0; i < bp.planets.length; i++) {
      const pl = bp.planets[i], S = surfaceFor(bp, i); if (!S) continue;
      planetAt(pl, i, T, _wc);
      const alt = Math.hypot(cam.position.x - _wc[0], cam.position.y - _wc[1], cam.position.z - _wc[2]) - pl.radius;
      const act = Math.max(S.atmo.top * 2.6, pl.radius * 0.45) * (i === wIdx ? 1.08 : 1);
      if (alt < act && alt < bd) { bd = alt; best = i; }
    }
    if (best !== wIdx) switchWorld(best);
    if (wIdx < 0) { ambienceOff(); return; }
    const pl = bp.planets[wIdx], S = WORLD.S;
    planetAt(pl, wIdx, T, _wc); planetQuat(pl, T, _wq);
    // the world spun and moved since the last frame: carry the effects with it
    _qNow.set(_wq[0], _wq[1], _wq[2], _wq[3]); _qD.copy(_qNow).multiply(_qPrev.invert());
    _wqd[0] = _qD.x; _wqd[1] = _qD.y; _wqd[2] = _qD.z; _wqd[3] = _qD.w;
    const R2 = Math.pow(pl.radius + S.atmo.top * 2, 2);
    if (Math.abs(_qD.w) < 0.9999999999 || _wc0[0] !== _wc[0] || _wc0[1] !== _wc[1] || _wc0[2] !== _wc[2]) fx.frameDrag(_wc0, _wc, _wqd, R2);
    _qPrev.copy(_qNow); _wc0[0] = _wc[0]; _wc0[1] = _wc[1]; _wc0[2] = _wc[2];
    // the air: shared uniforms, the light it lets through, the sky's ambient
    const sd = space.sys.sunDir, I = space.sys.sunI; _wsun[0] = sd.x; _wsun[1] = sd.y; _wsun[2] = sd.z; _wI[0] = I.x; _wI[1] = I.y; _wI[2] = I.z;
    setAU(wA, _wc[0], _wc[1], _wc[2], _wsun, _wI);
    ambientAt(wA, cam.position.x - _wc[0], cam.position.y - _wc[1], cam.position.z - _wc[2], _wsun, _wI, amb);
    const alt = amb.alt, tp = S.atmo.top;
    wAirK = sstep(tp * 1.4, tp * 0.55, alt);
    const act = Math.max(tp * 2.6, pl.radius * 0.45);
    wFade = sstep(act * 0.95, act * 0.72, alt) * WORLD.coverage();
    // sun light through the air; the sky's light on every hull instead of the nebula's
    const sm = Math.max(1e-3, amb.sun[0], amb.sun[1], amb.sun[2]), k = sstep(tp * 2, tp * 0.8, alt);
    space.sun.color.setRGB(sunBase.col.r + (amb.sun[0] / sm - sunBase.col.r) * k, sunBase.col.g + (amb.sun[1] / sm - sunBase.col.g) * k, sunBase.col.b + (amb.sun[2] / sm - sunBase.col.b) * k);
    space.sun.intensity = sunBase.k + (sm - sunBase.k) * k;
    const zr = amb.zen, hr = amb.hzSun, ha = amb.hzAnti;
    SKYU.uEnvK.value = 1 - 0.8 * wAirK;
    SKYU.uSkyIrr.value.set((zr[0] * 2 + hr[0] + ha[0]) * 0.78 * wAirK, (zr[1] * 2 + hr[1] + ha[1]) * 0.78 * wAirK, (zr[2] * 2 + hr[2] + ha[2]) * 0.78 * wAirK);
    SKYU.uSkyRad.value.set((zr[0] + hr[0] * 0.5 + ha[0] * 0.5) * 0.6 * wAirK, (zr[1] + hr[1] * 0.5 + ha[1] * 0.5) * 0.6 * wAirK, (zr[2] + hr[2] * 0.5 + ha[2] * 0.5) * 0.6 * wAirK);
    space.amb.intensity = 0.4 * (1 - wAirK); space.bounce.intensity = 0.55 * (1 - wAirK); camFill.intensity = 0.55 * (1 - 0.7 * wAirK);
    // the station and the asteroid field are specks beyond the sky: drop them deep in the air
    space.sys.group.visible = alt > tp * 0.6;
    // eye: a bright sky closes the iris a little, a night opens it
    const skyL = (zr[0] * 0.2 + zr[1] * 0.7 + zr[2] * 0.1) + (hr[1] + ha[1]) * 0.3;
    wExpo = 1 + wAirK * (Math.min(1.4, Math.max(0.62, 0.32 / (skyL + 0.12))) - 1);
    // the ship's shadow on the ground, the headlight at night
    const p = F && F.player, U = F && F.surf;
    WO.shadow[3] = 0; WO.head.k = 0;
    if (p && U && U.w === wIdx && p.alive) {
      const up = U.up, su = sd.x * up.x + sd.y * up.y + sd.z * up.z;
      if (U.agl < 420 && su > 0.05) { const d = U.agl / Math.max(su, 0.22); WO.shadow[0] = p.pos.x - sd.x * d; WO.shadow[1] = p.pos.y - sd.y * d; WO.shadow[2] = p.pos.z - sd.z * d; WO.shadow[3] = 8 + U.agl * 0.05; }
      const night = 1 - sstep(-0.06, 0.12, su);
      if (night > 0.02 && U.agl < 900) {
        const pv = V[p.i]; if (pv) { _fwd.set(0, -0.18, -1).normalize().applyQuaternion(pv.g.quaternion); WO.head.pos.copy(pv.g.position).addScaledVector(_fwd, 6); WO.head.dir.copy(_fwd); WO.head.k = night * 2.2; }
      }
    }
    WO.fade = wFade;
    if (U && U.w === wIdx) for (let i = 0; i < U.sites.length; i++) WORLD.setLooted(i, U.sites[i].looted);
    WO.atmo = wA;
    WORLD.update(dt, cam, WO);
    ambience(S, U, p, alt, tp);
    // daylight hides the nebula; at night the stars come back over the ground
    const sunUp = (sd.x * (cam.position.x - _wc[0]) + sd.y * (cam.position.y - _wc[1]) + sd.z * (cam.position.z - _wc[2])) / Math.max(1, alt + pl.radius);
    space.daylight(wAirK * sstep(-0.08, 0.15, sunUp) * sstep(tp * 1.1, tp * 0.6, alt));   // a quarter of the air above you still hides the stars by day
    // night: starlight and the glow of the sky keep the ground readable (blue, faint)
    const nightK = 1 - sstep(-0.12, 0.08, sunUp);
    WORLD.SU.uNight.value.set(0.006 + 0.03 * nightK, 0.008 + 0.038 * nightK, 0.012 + 0.06 * nightK);
    // dust lines turn into faint air streaks
    space.dispMat.uniforms.uCol.value.setScalar(0.32 * (1 - wAirK * 0.75));
  }
  const cxShowing = () => !!show;
  // the sound of the world: wind with the speed in the air, the roar of an entry, rain, the biome's own bed, thunder
  const AMB = { k: 0, biome: '', spd: 0, dens: 0, agl: 1e4, heat: 0, rain: 0, snow: 0, dust: 0, ash: 0, storm: 0, water: 0, night: 0, landed: false, paused: false };
  let ambOn = false, ambBolts = 0, ambPaused = false;
  function ambience(S, U, p, alt, tp) {
    if (!sfx.ambSet) return;
    if (!ambOn) { sfx.ambStart(); ambOn = true; ambBolts = WORLD.bolt.events; }
    const wk = S.weather.k, kind = S.weather.kind, low = 1 - Math.min(1, Math.max(0, (alt - S.cloud.alt * 0.8) / 300));
    AMB.k = Math.min(1, Math.max(0, (tp * 1.5 - alt) / (tp * 0.9))); AMB.biome = S.type; AMB.dens = amb.dens || 0;
    AMB.spd = p ? p.spd : 0; AMB.agl = U && U.w === wIdx ? U.agl : alt; AMB.heat = U && U.w === wIdx ? U.heat : 0;
    AMB.rain = kind === 'rain' ? wk * low : 0; AMB.snow = kind === 'snow' ? wk * low : 0; AMB.dust = kind === 'dust' || kind === 'wind' ? wk * low : 0; AMB.ash = kind === 'ash' ? wk * low : 0;
    AMB.storm = S.weather.storm || S.type === 'gas' ? Math.max(0.4, wk) * low : 0;
    AMB.water = U && U.w === wIdx && U.water ? 1 : S.sea > 0 && S.liquid !== 3 && S.liquid !== 2 ? 0.25 * low : 0;
    AMB.night = 1 - Math.min(1, Math.max(0, ((space.sys.sunDir.x * (cam.position.x - _wc[0]) + space.sys.sunDir.y * (cam.position.y - _wc[1]) + space.sys.sunDir.z * (cam.position.z - _wc[2])) / Math.max(1, alt + S.R) + 0.1) / 0.25));
    AMB.landed = !!(U && U.st === SURF.LANDED); AMB.paused = ambPaused;
    sfx.ambSet(AMB);
    if (WORLD.bolt.events !== ambBolts) { ambBolts = WORLD.bolt.events; const d = WORLD.lastStrike || 2000; if (sfx.thunder) sfx.thunder(d); if (d < 700 && sfx.lightningCrackle) sfx.lightningCrackle(); }
  }
  function ambienceOff() { if (ambOn && sfx.ambStop) sfx.ambStop(); ambOn = false; }
  // keep a point (the camera) a margin above the ground of the world we are in
  function groundClamp(v, m) {
    if (!F || !F.surf || F.surf.w < 0 || F.surf.w !== wIdx) return;
    const a = groundAt(F, v.x, v.y, v.z);
    if (a < m) { const W = F.worlds[F.surf.w]; _v.set(v.x - W.c.x, v.y - W.c.y, v.z - W.c.z).normalize(); v.addScaledVector(_v, m - a); }
  }

  // the plasma sheath of an entry, the dust and spray of low flight, the landing gear
  const plasmaGeo = new THREE.SphereGeometry(1, 28, 18);
  const plasmaMat = new THREE.ShaderMaterial({ transparent: true, depthWrite: false, blending: THREE.AdditiveBlending, side: THREE.DoubleSide,
    uniforms: { uK: { value: 0 }, uT: { value: 0 }, uMap: { value: WORLD.detail } },
    vertexShader: `varying vec3 vP; varying vec3 vN; varying vec3 vV; void main() { vP = position; vN = normalize(normalMatrix * normal); vec4 mv = modelViewMatrix * vec4(position, 1.0); vV = normalize(-mv.xyz); gl_Position = projectionMatrix * mv; }`,
    fragmentShader: `uniform float uK, uT; uniform sampler2D uMap; varying vec3 vP; varying vec3 vN; varying vec3 vV;
      void main() {
        float rim = pow(1.0 - abs(dot(normalize(vN), normalize(vV))), 2.2);
        float nose = smoothstep(-0.2, -1.0, vP.z);   // the front (local -Z) burns white-hot, the sheath trails off in flames
        float a = atan(vP.y, vP.x);
        float st = texture2D(uMap, vec2(a * 0.6, vP.z * 0.35 - uT * 2.2)).g * 0.8 + texture2D(uMap, vec2(a * 1.3 + 0.3, vP.z * 0.9 - uT * 3.1)).r * 0.7;
        st = smoothstep(0.35, 1.2, st);
        vec3 c = mix(vec3(1.4, 0.36, 0.08), vec3(2.4, 1.5, 0.9), nose) * (rim * (0.6 + nose * 1.4) + nose * nose * 0.5) * (0.3 + st * 1.4);
        float tail = smoothstep(1.0, -0.2, vP.z);
        gl_FragColor = vec4(c * uK * tail, 1.0);
      }` });
  const plasma = new THREE.Mesh(plasmaGeo, plasmaMat); plasma.visible = false; plasma.frustumCulled = false; plasma.renderOrder = 8; space.near.add(plasma);
  const gearGeo = (() => {
    const parts = [];
    const leg = (x, z, h) => { const c = new THREE.CylinderGeometry(0.16, 0.2, h, 6); c.translate(x, -h / 2, z); parts.push(c); const f = new THREE.CylinderGeometry(0.55, 0.6, 0.18, 8); f.translate(x, -h, z); parts.push(f); };
    leg(0, -4.0, 2.2); leg(-2.4, 2.6, 2.3); leg(2.4, 2.6, 2.3);
    let n = 0; for (const g of parts) n += g.attributes.position.count;
    const pos = new Float32Array(n * 3), nrm = new Float32Array(n * 3), idx = []; let o = 0;
    for (const g of parts) { pos.set(g.attributes.position.array, o * 3); nrm.set(g.attributes.normal.array, o * 3); for (const i of g.index.array) idx.push(i + o); o += g.attributes.position.count; g.dispose(); }
    const geo = new THREE.BufferGeometry(); geo.setAttribute('position', new THREE.BufferAttribute(pos, 3)); geo.setAttribute('normal', new THREE.BufferAttribute(nrm, 3)); geo.setIndex(idx); return geo;
  })();
  const gearMat = new THREE.MeshStandardMaterial({ color: 0x8a8d92, metalness: 0.7, roughness: 0.45 });
  const gear = new THREE.Mesh(gearGeo, gearMat); gear.visible = false;
  let dustT = 0, lastHeat = 0;
  function surfaceFx(dt) {
    const p = F.player, U = F.surf, pv = V[p.i];
    if (!U || !pv) { plasma.visible = false; return; }
    // landing gear: drops out of the belly
    if (gear.parent !== pv.g) pv.g.add(gear);
    gear.visible = U.gear > 0.02 && pv.g.visible; gear.scale.set(1, Math.max(0.02, U.gear), 1);
    // the entry: plasma sheath, glowing hull, embers, the shake
    const h = U.w >= 0 ? U.heat : 0;
    plasma.visible = h > 0.02 && p.alive;
    if (plasma.visible) {
      _v.set(p.vel.x, p.vel.y, p.vel.z); const vl = _v.length() || 1; _v.divideScalar(vl);
      plasma.position.copy(pv.g.position).addScaledVector(_v, 2.5);
      plasma.quaternion.setFromUnitVectors(_v2.set(0, 0, -1), _v);
      const r = (p.cls.rad || 7) * 0.72; plasma.scale.set(r, r * 0.62, r * (1.2 + h * 1.4));
      plasma.position.copy(pv.g.position).addScaledVector(_v, r * 0.35);
      plasmaMat.uniforms.uK.value = h * 0.55; plasmaMat.uniforms.uT.value = time;
      const U0 = pv.mat.userData.U; U0.uFlash.value = Math.max(U0.uFlash.value, h * 0.22); U0.uFlashCol.value.setRGB(1.0, 0.42, 0.14);
      for (let k = 0; k < Math.round(h * 6); k++) { const s = Math.random(); fx.add.add(pv.g.position.x + (Math.random() - 0.5) * 8, pv.g.position.y + (Math.random() - 0.5) * 8, pv.g.position.z + (Math.random() - 0.5) * 8,
        -_v.x * vl * (0.05 + s * 0.1), -_v.y * vl * (0.05 + s * 0.1), -_v.z * vl * (0.05 + s * 0.1), 0.35 + s * 0.4, 1.5 + s * 2, 0.3, 4, 1.6, 0.4, 1, 1.5, 0.3, 0.05, 0, 1, 0.04); }
      trauma = Math.min(0.55, trauma + h * dt * 2.2);
    } else if (lastHeat > 0.02) { const U0 = pv.mat.userData.U; U0.uFlashCol.value.setRGB(1, 1, 1); }
    lastHeat = h;
    // low flight: dust, snow, ash or spray kicked up under the ship
    if (U.w >= 0 && U.agl < 32 && p.alive && U.st !== SURF.LANDED) {
      dustT -= dt;
      const k = (1 - U.agl / 32) * Math.min(1, (p.spd + (U.st === SURF.LANDING || U.st === SURF.TAKEOFF ? 60 : 0)) / 80);
      if (dustT <= 0 && k > 0.05) {
        dustT = 0.025;
        const up = U.up, gx = p.pos.x - up.x * U.agl, gy = p.pos.y - up.y * U.agl, gz = p.pos.z - up.z * U.agl;
        const c = groundDustCol();
        for (let i = 0; i < 3; i++) {
          const a = Math.random() * 6.283, sp = 6 + Math.random() * 14 * k;
          _v.set(Math.cos(a), 0, Math.sin(a)); _v2.set(up.x, up.y, up.z); _v.addScaledVector(_v2, -_v.dot(_v2)).normalize();
          fx.smoke.add(gx + _v.x * 3, gy + _v.y * 3, gz + _v.z * 3, _v.x * sp + p.vel.x * 0.25 + up.x * 3, _v.y * sp + p.vel.y * 0.25 + up.y * 3, _v.z * sp + p.vel.z * 0.25 + up.z * 3,
            1.1 + Math.random(), 2 + k * 3, 7 + k * 10, c[0], c[1], c[2], 0.5 * k, c[0], c[1], c[2], 0, 2, 0, 1.2);
        }
      }
    }
  }
  function groundDustCol() {
    const S = WORLD.S; if (!S) return [0.5, 0.5, 0.5];
    if (F.surf.water) return [0.85, 0.9, 0.95];
    switch (S.type) { case 'ice': return [0.85, 0.9, 0.95]; case 'volcanic': return [0.16, 0.14, 0.14]; case 'desert': return [0.72, 0.55, 0.38]; case 'rocky': return [0.62, 0.45, 0.35]; case 'crystal': return [0.5, 0.45, 0.75]; case 'jungle': return [0.38, 0.36, 0.28]; default: return [0.55, 0.52, 0.48]; }
  }
  function surfEvent(e) {
    if (e.a === -1) { const n = e.v && e.v.name; hud.note('cz_hud_soi', 'info', { name: n }); return; }
    if (e.a === -2) { if (e.k) hud.note(e.k, 'bad'); sfx.deny(); return; }
    if (e.a === -3) { hud.note(e.k, 'info'); sfx.blip(); return; }
    if (e.a === -4) { hud.note(e.k, 'bad'); return; }
    if (e.a === SURF.ENTRY) { hud.banner && hud.banner(tr(e.k), '#ffb070', 3); sfx.warp && sfx.warp(0.8); trauma = Math.min(1, trauma + 0.4); flashK = Math.max(flashK, 0.15); if (grade) grade.uniforms.uFlashCol.value.setRGB(1, 0.6, 0.35); return; }
    if (e.a === SURF.DESCENT || e.a === SURF.ASCENT) { hud.banner && hud.banner(tr(e.k), '#9fe8ff', 2.4); sfx.cruiseSpool && sfx.cruiseSpool(); return; }
    if (e.a === SURF.LANDED) { hud.note(e.k, 'good'); sfx.clang && sfx.clang(); return; }
    if (e.k) hud.note(e.k, e.a === SURF.SPACE || e.a === SURF.FLIGHT ? 'good' : 'info');
  }
  function groundEvent(e) {
    const c = groundDustCol();
    if (e.b) {   // touchdown: a ring of dust
      for (let i = 0; i < 26; i++) { const a = i / 26 * 6.283, U = F.surf; _v.set(Math.cos(a), 0.15, Math.sin(a)); _v2.set(U.up.x, U.up.y, U.up.z); _v.addScaledVector(_v2, -_v.dot(_v2)).normalize().multiplyScalar(8 + Math.random() * 6);
        fx.smoke.add(e.x, e.y, e.z, _v.x + U.up.x * 2, _v.y + U.up.y * 2, _v.z + U.up.z * 2, 1.8, 2, 9, c[0], c[1], c[2], 0.55, c[0], c[1], c[2], 0, 2, 0, 1.6); }
      shakeAt(e.x, e.y, e.z, 0.25); return;
    }
    if (e.a < 6) return;
    fx.hitSparks(e.x, e.y, e.z, F.surf.up.x, F.surf.up.y, F.surf.up.z, 1.2, 0.8, 0.5, Math.min(24, 6 + e.a));
    for (let i = 0; i < 8; i++) fx.smoke.add(e.x, e.y, e.z, (Math.random() - 0.5) * 12, (Math.random() - 0.5) * 12, (Math.random() - 0.5) * 12, 1.4, 2, 8, c[0], c[1], c[2], 0.5, c[0], c[1], c[2], 0, 2, 0, 1);
    if (e.s && e.s.isPlayer) { sfx.clang && sfx.clang(); shakeAt(e.x, e.y, e.z, Math.min(1, e.a / 40)); }
  }

  // ---- camera rig ------------------------------------------------------------------------------------------------------------
  const off = new THREE.Vector3(), look = new THREE.Vector3(), up = new THREE.Vector3();
  function flightCamera(dt) {
    const p = F.player, pv = V[p.i];
    const fovT = baseFov + (p.boosting ? 10 : 0) + (F.input.drift ? 4 : 0) + (F.retreat > 0 ? 25 : 0) + (F.cruise === 2 ? 14 * Math.min(1, p.spd / 1500) : 0);
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
    // jump drive spool: the field opens, the stars start to stream
    jumpK = F.jump ? Math.min(1, F.jump.t / 4.2) : Math.max(0, jumpK - dt * 2);
    cam.fov += jumpK * 22 * (1 - Math.exp(-4 * dt));
    if (cineCamera(dt, g)) { cineK = 1; return; }
    cineK = Math.max(0, cineK - dt * 0.8);
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
      camPos.lerp(target, 1 - Math.exp(-14 * (1 - cineK * 0.88) * dt));
      if (camPos.distanceTo(target) > 60) camPos.copy(target);
      camQ.slerp(g.quaternion, 1 - Math.exp(-9 * dt));
      // never inside a rock, a station module or a capital hull: pull in along the ship -> camera segment
      _v2.set(0, 1.6, 0).applyQuaternion(g.quaternion).add(g.position);
      const free = camFree(_v2, camPos);
      if (free < 1) camPos.sub(_v2).multiplyScalar(Math.max(0.34, free)).add(_v2);
      groundClamp(camPos, 2.2);
      cam.position.copy(camPos); cam.quaternion.copy(camQ);
      // keep the camera level with the ship's heading: the hull rides in the lower third, the reticle above it
      cam.rotateX(0.015);
    }
  }
  // fraction (0..1) of the segment a->b that is clear of obstacles: the sim's static grid (asteroids at their
  // VISUAL bound, larger than the collision sphere; station modules) and the hull spheres of nearby capitals
  const _seg = new THREE.Vector3(), _hp = { x: 0, y: 0, z: 0 };
  function rayHit(ax, ay, az, dx, dy, dz, L, cx, cy, cz, r) {
    const ox = ax - cx, oy = ay - cy, oz = az - cz, b = ox * dx + oy * dy + oz * dz, c = ox * ox + oy * oy + oz * oz - r * r;
    if (c < 0) return 0;                         // already inside
    const disc = b * b - c; if (disc < 0) return 1;
    const t = -b - Math.sqrt(disc); return t >= 0 && t < L ? t / L : 1;
  }
  function camFree(a, b) {
    if (!F || !F.obs) return 1;
    _seg.copy(b).sub(a); const L = _seg.length(); if (L < 1e-3) return 1; _seg.divideScalar(L);
    const O = F.obs, C = F.cell; let t = 1;
    for (let k = 0; k <= 2; k++) {
      const px = a.x + _seg.x * L * k / 2, py = a.y + _seg.y * L * k / 2, pz = a.z + _seg.z * L * k / 2;
      const list = F.grid.get(F.gkey(Math.floor(px / C), Math.floor(py / C), Math.floor(pz / C)));
      if (!list) continue;
      for (let j = 0; j < list.length; j++) { const i = list[j], r = O.kind[i] === 0 ? O.r[i] * 1.2 + 2 : O.r[i] + 4; t = Math.min(t, rayHit(a.x, a.y, a.z, _seg.x, _seg.y, _seg.z, L, O.x[i], O.y[i], O.z[i], r)); }
    }
    for (const s of F.ships) {
      if (!s.alive || !s.cap || !s.hs) continue;
      const dx = s.pos.x - a.x, dy = s.pos.y - a.y, dz = s.pos.z - a.z; if (dx * dx + dy * dy + dz * dz > (s.cls.rad + 60) * (s.cls.rad + 60)) continue;
      for (const h of s.hs) { localToWorld(s, h[0], h[1], h[2], _hp); t = Math.min(t, rayHit(a.x, a.y, a.z, _seg.x, _seg.y, _seg.z, L, _hp.x, _hp.y, _hp.z, h[3] + 3)); }
    }
    return t < 1 ? Math.max(0, t - 2.5 / L) : 1;
  }
  // the hands-off moments of a flight get a director: leaving the bay, the docking approach, the arrival from a jump
  const _cs = new THREE.Vector3(), _cu = new THREE.Vector3(), _v3 = new THREE.Vector3();
  function cineCamera(dt, g) {
    const st = F.pois && F.pois.find((x) => x.kind === 'station');
    let goal = null;
    const U = F.surf;
    if (U && U.w >= 0 && (U.auto && (U.st === SURF.DESCENT || U.st === SURF.ENTRY) || U.st === SURF.LANDED || U.st === SURF.LANDING || U.st === SURF.TAKEOFF)) {
      // entry: off the wing, low, the plasma on the nose and the world ahead; landed: a slow walk round the ship
      const up = U.up;
      _cu.set(up.x, up.y, up.z);
      _fwd.set(0, 0, -1).applyQuaternion(g.quaternion);
      _cs.crossVectors(_fwd, _cu); if (_cs.lengthSq() < 1e-4) _cs.set(1, 0, 0); _cs.normalize();
      if (U.st === SURF.LANDED || U.st === SURF.LANDING || U.st === SURF.TAKEOFF) {
        landAng += dt * (U.st === SURF.LANDED ? 0.12 : 0.05);
        // a site close by: the ship in front, the site behind it (the reveal), the camera drifting a little to the side
        let site = null; for (const s of U.sites) if (s.d < 700 && (!site || s.d < site.d)) site = s;
        if (site) {
          _v.set(site.pos.x - g.position.x, site.pos.y - g.position.y, site.pos.z - g.position.z); _v.addScaledVector(_cu, -_v.dot(_cu));
          const dd = Math.max(1, _v.length()); _v.multiplyScalar(1 / dd); _cs.crossVectors(_v, _cu).normalize();
          const sw = Math.sin(landAng * 0.7);
          goal = _v2.copy(g.position).addScaledVector(_v, -(30 + dd * 0.04)).addScaledVector(_cs, 12 + sw * 9).addScaledVector(_cu, 7 + dd * 0.03 + (U.st === SURF.TAKEOFF ? 4 : 0));
          look.copy(g.position).lerp(_v3.set(site.pos.x, site.pos.y, site.pos.z), Math.min(0.45, 60 / dd + 0.15)).addScaledVector(_cu, 3);
        } else {
          const R = U.st === SURF.LANDED ? 26 : 34, ca = Math.cos(landAng), sa = Math.sin(landAng);
          goal = _v2.copy(g.position).addScaledVector(_cs, ca * R).addScaledVector(_fwd, sa * R).addScaledVector(_cu, U.st === SURF.LANDED ? 5 : 9);
          look.copy(g.position).addScaledVector(_cu, 1.5);
        }
        groundClamp(goal, 2.5);
        if (cineK < 0.99) camPos.copy(goal); else camPos.lerp(goal, 1 - Math.exp(-2 * dt));
        cam.position.copy(camPos); cam.up.copy(_cu); cam.lookAt(look); cam.up.set(0, 1, 0);
        camQ.copy(cam.quaternion); cockpit.visible = false; cam.fov += (58 - cam.fov) * (1 - Math.exp(-2 * dt));
        return true;
      }
      // the offset from the ship eases (the ship itself moves at km/s: the camera rides with it, no lag)
      const k = U.heat;
      _v.set(0, 0, 0).addScaledVector(_cs, 9 + 4 * k).addScaledVector(_cu, 6.5 - 2 * k).addScaledVector(_fwd, -27 + 6 * k);
      if (cineK < 0.99) entOff.copy(_v); else entOff.lerp(_v, 1 - Math.exp(-3 * dt));
      camPos.copy(g.position).add(entOff);
      cam.position.copy(camPos); look.copy(g.position).addScaledVector(_fwd, 14).addScaledVector(_cu, 2.5); cam.up.copy(_cu); cam.lookAt(look); cam.up.set(0, 1, 0);
      camQ.copy(cam.quaternion); cockpit.visible = false; cam.fov += (64 + k * 6 - cam.fov) * (1 - Math.exp(-3 * dt));
      return true;
    }
    if (F.undock && st) {
      const n = st.n; _cu.set(0, 1, 0); _cs.set(n[0], n[1], n[2]).cross(_cu).normalize();
      goal = _v2.set(st.mouth[0] + n[0] * 110 + _cs.x * 46, st.mouth[1] + n[1] * 110 + 20, st.mouth[2] + n[2] * 110 + _cs.z * 46);
    } else if (F.dock && F.dock.ph >= 1) {
      const n = F.dock.P.n, m = F.dock.P.mouth; _cu.set(0, 1, 0); _cs.set(n[0], n[1], n[2]).cross(_cu); if (_cs.lengthSq() < 1e-4) _cs.set(1, 0, 0); _cs.normalize();
      const along = Math.max(80, (g.position.x - m[0]) * n[0] + (g.position.y - m[1]) * n[1] + (g.position.z - m[2]) * n[2] + 46);
      goal = _v2.set(m[0] + n[0] * along + _cs.x * 20, m[1] + n[1] * along + 13, m[2] + n[2] * along + _cs.z * 20);
      if (cineK < 0.99) camPos.copy(goal); else camPos.lerp(goal, 1 - Math.exp(-3 * dt));
      cam.position.copy(camPos); look.copy(g.position).lerp(_cu.set(m[0], m[1], m[2]), 0.4); cam.lookAt(look); camQ.copy(cam.quaternion); cockpit.visible = false;
      cam.fov += (58 - cam.fov) * (1 - Math.exp(-3 * dt));
      return true;
    } else if (relightCam > 0 && space.sys.beaconCore) {
      // the burst: hold on the spire over the pilot's shoulder while the light goes out across the sector
      relightCam -= dt;
      const c = space.sys.beaconCore; _cs.set(g.position.x - c[0], g.position.y - c[1], g.position.z - c[2]).normalize();
      goal = _v2.copy(g.position).addScaledVector(_cs, 70); goal.y += 26;
      if (cineK < 0.99) camPos.copy(goal); else camPos.lerp(goal, 1 - Math.exp(-2 * dt));
      cam.position.copy(camPos); look.set(c[0], c[1] - 60, c[2]).lerp(g.position, 0.12); cam.lookAt(look); camQ.copy(cam.quaternion); cockpit.visible = false;
      cam.fov += (62 - cam.fov) * (1 - Math.exp(-2 * dt));
      return true;
    } else if (F.arriveT > 0) {
      const k = 1 - Math.max(0, F.arriveT) / 2.6, e = k * k * (3 - 2 * k);
      off.set(lerp(18, 0, e), lerp(7, 4.3, e), lerp(-95, 16, e)).applyQuaternion(g.quaternion);
      goal = _v2.copy(g.position).add(off);
      cam.position.copy(goal); up.set(0, 1, 0).applyQuaternion(g.quaternion); cam.up.copy(up); cam.lookAt(g.position); cam.up.set(0, 1, 0);
      camPos.copy(cam.position); camQ.copy(cam.quaternion); cockpit.visible = false;
      return true;
    }
    if (!goal) return false;
    if (cineK < 0.99) camPos.copy(goal); else camPos.lerp(goal, 1 - Math.exp(-2.5 * dt));
    cam.position.copy(camPos); cam.lookAt(g.position); camQ.copy(cam.quaternion); cockpit.visible = false;
    cam.fov += (54 - cam.fov) * (1 - Math.exp(-3 * dt));
    return true;
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
    space.sys.group.visible = !list;
    if (!list) return;
    const g = new THREE.Group(), mats = []; let x = 0; const items = [];
    for (const it of list) {
      const [ck, fac, ace] = Array.isArray(it) ? it : [it, CLS[it].fac != null ? CLS[it].fac : F_PLAYER, false];
      const m = KIT.hullMaterial(THREE, { panel: CLS[ck].cap ? 0.18 : 0.6 }); mats.push(m);
      const mesh = new THREE.Mesh(KIT.shipGeometry(THREE, ck, fac, ace), m);
      const w = CLS[ck].rad * 2.2; x += w / 2; mesh.position.set(x, 0, 0); x += w / 2 + 4; mesh.rotation.y = o.yaw != null ? o.yaw : 2.45;
      mesh.userData.engs = CLS[ck].eng || []; mesh.userData.ec = FAC_ENGINE[fac] || FAC_ENGINE[F_GILDA];
      g.add(mesh); items.push(mesh);
    }
    items.forEach((m) => { m.position.x -= x / 2; });
    const base = hubShip ? hubShip.position : new THREE.Vector3();
    g.position.copy(base).add(new THREE.Vector3(0, 0, -120)); space.near.add(g);
    show = { g, mats, items, span: x, dist: o.dist || Math.max(30, x * 0.75), elev: o.elev != null ? o.elev : 0.25 };
    if (hubShip) hubShip.visible = false;
  };
  // debug: render every top-level object of the far and near scenes alone into a float target and count
  // non-finite pixels (NaN/Inf); window.__cz.nanScan() -> [{ scene, name, nan }] for the objects that produce any
  window.__cz.nanScan = (w = 480, h = 300) => {
    const rt = new THREE.WebGLRenderTarget(w, h, { type: THREE.FloatType }), buf = new Float32Array(w * h * 4), out = [];
    // non-finite, or above the half-float range the composer stores the frame in (65504 -> Inf there)
    const count = () => { renderer.readRenderTargetPixels(rt, 0, 0, w, h, buf); let n = 0; for (let i = 0; i < buf.length; i++) if (!(Math.abs(buf[i]) < 65000)) n++; return n; };
    const prevRT = renderer.getRenderTarget(), prevAuto = renderer.autoClear, prevBg = space.far.background;
    renderer.autoClear = false;   // the frame as the composer builds it: far, then near over a cleared depth
    renderer.setRenderTarget(rt); renderer.clear(); renderer.render(space.far, farCam); renderer.clearDepth(); renderer.render(space.near, cam);
    out.push({ scene: 'frame', name: '*composite*', nan: count() });
    renderer.autoClear = true;
    for (const [name, scene, camera] of [['far', space.far, farCam], ['near', space.near, cam]]) {
      renderer.setRenderTarget(rt); renderer.render(scene, camera); const all = count();
      out.push({ scene: name, name: '*all*', nan: all });
      if (!all) continue;
      const kids = scene.children.slice(), vis = kids.map((k) => k.visible), bg = scene.background; scene.background = null;
      for (let i = 0; i < kids.length; i++) {
        kids.forEach((k, j) => { k.visible = j === i && vis[j]; });
        if (!vis[i]) continue;
        renderer.render(scene, camera); const n = count();
        if (n) out.push({ scene: name, name: kids[i].name || kids[i].type + '#' + i, nan: n, mat: kids[i].material && (kids[i].material.type + ':' + (kids[i].material.fragmentShader || '').slice(0, 60)) });
      }
      kids.forEach((k, j) => { k.visible = vis[j]; }); scene.background = bg;
      if (bg) { kids.forEach((k) => { k.visible = false; }); renderer.render(scene, camera); out.push({ scene: name, name: 'background', nan: count() }); kids.forEach((k, j) => { k.visible = vis[j]; }); }
    }
    space.far.background = prevBg; renderer.setRenderTarget(prevRT); renderer.autoClear = prevAuto; rt.dispose();
    return out;
  };
  // dev / review view: one faction's station, alone, in this system's light (window.__cz.showStation(fac, { seed, yaw }))
  window.__cz.showStation = (fac, o = {}) => {
    if (show) { show.g.removeFromParent(); show.mats.forEach((m) => m.dispose()); show = null; }
    space.sys.group.visible = fac == null;
    if (fac == null) return;
    const bp = stationBlueprint(o.seed != null ? o.seed : 987654, o.sector || 0, o.sys || 0, fac);
    const st = KIT.stationGeometry(THREE, bp);
    const m = KIT.hullMaterial(THREE, { metal: 0.55, rough: 0.5, panel: 0.12, env: 1.0 });
    const g = new THREE.Group(), mesh = new THREE.Mesh(st.geo, m); g.add(mesh);
    mesh.rotation.y = o.yaw != null ? o.yaw : 0.6;
    const base = hubShip ? hubShip.position : new THREE.Vector3();
    g.position.copy(base).add(new THREE.Vector3(0, 0, -900)); space.near.add(g);
    show = { g, mats: [m], span: bp.radius * 2, dist: o.dist || bp.radius * 2.6, elev: o.elev != null ? o.elev : 0.3 };
    if (hubShip) hubShip.visible = false;
    return { mods: bp.mods.length, tris: st.geo.attributes.position.count / 3, dock: bp.dock || null };
  };
  function hubCamera(dt) {
    // the hub jump: the ship lights its drive and pulls away, the tunnel, then it drops out next to the new station
    let thr = 0.05, boost = 0;
    if (departT >= 0 && hubShip) {
      departT += dt; thr = 1; boost = 1;
      _fwd.set(0, 0, 1).applyQuaternion(hubShip.quaternion);
      hubShip.position.copy(hubPark).addScaledVector(_fwd, departT * departT * 260);
      if (departT > 1.05 && tunnelT < 0) { tunnelT = 0; sfx.warp && sfx.warp(1); }
      if (tunnelT >= 0.6 && departT > 0) { departT = -1; arriveHub = 0; }
    } else if (arriveHub >= 0 && hubShip) {
      arriveHub += dt; const k = Math.min(1, arriveHub / 2.2), e = 1 - Math.pow(1 - k, 3);
      _fwd.set(0, 0, 1).applyQuaternion(hubShip.quaternion);
      hubShip.position.copy(hubPark).addScaledVector(_fwd, -(1 - e) * 1400);
      thr = 1 - e * 0.9; boost = k < 0.4 ? 1 : 0;
      if (k >= 1) { arriveHub = -1; hubShip.position.copy(hubPark); }
    }
    if (show && show.items) for (const m of show.items) enginePlumes(m, m.userData.engs, m.userData.ec, 0.55, 0, 1, 1, null, true);
    else if (hubShip && hubShip.visible) enginePlumes(hubShip, CLS.lucciola.eng, FAC_ENGINE[F_PLAYER], thr, boost, 1, 1, hubShip.material.userData.U, true);
    if (departT >= 0 && hubShip) {   // hold the shot on the ship as it goes
      cam.lookAt(hubShip.position); cam.fov += (50 - cam.fov) * (1 - Math.exp(-3 * dt)); cockpit.visible = false; return;
    }
    if (show) {
      hubAng += dt * 0.08;
      const c = show.g.position, a = Math.sin(hubAng) * 0.35;
      cam.position.set(c.x + Math.sin(a) * show.dist, c.y + show.dist * show.elev, c.z + Math.cos(a) * show.dist);
      cam.lookAt(c); cam.fov += (40 - cam.fov) * (1 - Math.exp(-4 * dt)); cockpit.visible = false; return;
    }
    hubAng += dt * 0.025;
    const bp = space.sys.bp;
    const tgt = hubShip ? (arriveHub >= 0 ? hubPark : hubShip.position) : _v.set(0, 0, -2000);
    const st = bp && bp.station ? bp.station.pos : [tgt.x, tgt.y, tgt.z];
    const R = 260;
    cam.position.set(tgt.x + Math.cos(hubAng) * R, tgt.y + 70 + Math.sin(hubAng * 0.7) * 30, tgt.z + Math.sin(hubAng) * R);
    look.set(lerp(tgt.x, st[0], 0.55), lerp(tgt.y, st[1], 0.55), lerp(tgt.z, st[2], 0.55));
    cam.lookAt(look);
    cam.fov += (62 - cam.fov) * (1 - Math.exp(-2 * dt));
    if (hubShip && arriveHub < 0) hubShip.position.y = hubPark.y + Math.sin(time * 0.5) * 1.2;
    if (arriveHub >= 0 && hubShip) cam.lookAt(hubShip.position.x * 0.6 + look.x * 0.4, hubShip.position.y * 0.6 + look.y * 0.4, hubShip.position.z * 0.6 + look.z * 0.4);
    cockpit.visible = false;
  }

  // ---- render ------------------------------------------------------------------------------------------------------------------
  function render() {
    farCam.quaternion.copy(cam.quaternion); farCam.fov = cam.fov; farCam.position.copy(cam.position);
    if (cam.view && cam.view.enabled) farCam.setViewOffset(cam.view.fullWidth, cam.view.fullHeight, cam.view.offsetX, cam.view.offsetY, cam.view.width, cam.view.height); else farCam.clearViewOffset();
    farCam.updateProjectionMatrix(); cam.updateProjectionMatrix();
    if (post) {
      grade.uniforms.uTime.value = time % 100;
      if (mapMode) post.render(GAL.scene, GAL.cam, null, null); else post.render(space.far, farCam, space.near, cam);
    } else if (mapMode) { renderer.autoClear = true; renderer.setRenderTarget(null); renderer.render(GAL.scene, GAL.cam); }
    else { renderer.autoClear = false; renderer.setRenderTarget(null); renderer.clear(); renderer.render(space.far, farCam); renderer.clearDepth(); renderer.render(space.near, cam); }
  }

  function frame(state, model) {
    const now = performance.now(), rawDt = (now - last) / 1000, dt = Math.min(0.05, rawDt); last = now;
    time += dt;
    fx.plumesBegin();
    const ph = state ? state.phase : 'loading';
    // perf bookkeeping (frame-to-frame time)
    perf.frames++; perf.hist[perf.hi++ % perf.hist.length] = rawDt * 1000; perf.ema += (rawDt * 1000 - perf.ema) * 0.05;
    const bp = model && model.bp;
    lastModel = model;
    // the galaxy map: the hub's map screen, or the jump-plot overlay in flight (the flight is paused under it)
    const wantMap = !!state && ((ph === 'hub' && state.screen === 'map' && departT < 0 && tunnelT < 0) || (ph === 'combat' && hud.mapOpen && hud.mapOpen()));
    if (wantMap !== mapMode) { mapMode = wantMap; GAL.setActive(mapMode); }
    if (mapMode) GAL.update(dt, model, ph === 'hub' ? state : { target: model.plot });
    // jump tunnel: fade in, swap the system at the peak, fade out
    if (tunnelT >= 0) {
      tunnelT += dt;
      const k = tunnelT < 0.6 ? tunnelT / 0.6 : tunnelT < 1.4 ? 1 : Math.max(0, 1 - (tunnelT - 1.4) / 0.9);
      space.tunnel.visible = k > 0.01; space.tunnelMat.uniforms.uK.value = k; space.tunnelMat.uniforms.uT.value = tunnelT;
      if (tunnelT >= 0.6 && pendingBp) { const b = pendingBp; pendingBp = null; doBuild(b); flashK = 0.8; }
      if (tunnelT > 2.3) { tunnelT = -1; space.tunnel.visible = false; }
    }
    if (tunnelT < 0) {   // cruise (and the jump drive's spool): a faint streak tunnel that grows with speed
      const ck = F && ph === 'combat' && F.cruise === 2 ? Math.min(1, (F.player.spd - 400) / 1800) : jumpK * 1.6;
      cruiseK += (Math.max(0, ck) - cruiseK) * (1 - Math.exp(-3 * dt));
      space.tunnel.visible = cruiseK > 0.01; space.tunnelMat.uniforms.uK.value = cruiseK * 0.22; space.tunnelMat.uniforms.uT.value += dt * (0.3 + cruiseK);
    }
    if (bp) ensureSystem(bp, ph === 'hub' || ph === 'debrief');
    ui.show(ph, state ? state.screen : 'bridge');
    if (ui.cine) ui.cine(ph === 'hub' && (departT >= 0 || tunnelT >= 0 || arriveHub >= 0));
    // ---- mode switch
    const flying = ph === 'combat' && model && model.flight;
    if (flying && model.flight !== F) {
      F = model.flight; evCur = F.evHead; clearVisuals(); deathCam = null; intro = F.briefT > 0 ? 3.2 : 0; trauma = 0; relightCam = 0;
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
      if (F.outcome === 3) fadeK = 1;   // docked: the bay doors close on black, the hub fades in
      F = null; clearVisuals(); space.setNavs(null, null); hud.stop(); switchWorld(-1); sfx.stopDrone(); sfx.engineStop && sfx.engineStop(); if (hubShip) hubShip.visible = true; deathCam = null; cineK = 0; jumpK = 0;
      hubShip && hubShip.position.copy(hubPark);
    }
    // codex ship dossiers: the model turns in the hangar on the left of the screen
    const cxe = ph === 'hub' && state.screen === 'codex' && CATS[state.codexCat | 0] === 'ships' ? entriesOf(state.codexCat | 0)[state.focus.codex | 0] : null;
    const cxId = cxe && cxe.model && model.web && model.web.codex[cxe.id] != null ? cxe.id : '';
    if (cxId !== codexShown) {
      codexShown = cxId;
      if (cxId) { window.__cz.showcase([[cxe.model[0], cxe.model[1], false]], { yaw: 2.3, elev: 0.18, dist: Math.max(34, CLS[cxe.model[0]].rad * 3.2) }); cam.setViewOffset(canvas.width || 1, canvas.height || 1, (canvas.width || 1) * 0.22, 0, canvas.width || 1, canvas.height || 1); }
      else { window.__cz.showcase(null); cam.clearViewOffset(); }
    }
    if (model && model.notes && model.notes.length && ph === 'combat' && F) { const id = model.notes.shift(); hud.note('cz_t_codex', 'good', { name: tr('cz_cx_' + id + '_t') }); }
    if (ph === 'combat' && F) {
      const paused = !!(model.paused || hud.paused() || mapMode);
      ambPaused = paused;
      // the dock: fade to black while the bay takes the ship
      if (F.dock && F.dock.ph === 3) fadeK = Math.min(1, F.dock.t / 1.4); else if (F.outcome === 3) fadeK = 1;
      F.paused = paused;
      const steps = paused ? 0 : simFrame(F, rawDt);
      perf.simSteps += steps;
      events();
      syncFlight(dt, F.alpha);
      surfaceFx(dt);
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
    SPO.stretch = F && F.cruise === 2 ? 0.03 : 0.05; SPO.dust = F ? 0.32 : 0.2; SPO.pxScale = canvas.height / 900; SPO.farPos = cam.position;
    SPO.T = F ? F.T0 + F.t : universeTime();
    space.update(dt, cam, camVel, SPO);
    worldUpdate(dt);
    // eye adaptation: looking into the star closes the iris a little (the HUD stays readable, the star still burns)
    let sunK = 0;
    if (space.sys.sunDir) { _v.copy(space.sys.sunDir).applyQuaternion(_q.copy(cam.quaternion).invert()); const c = -_v.z / (_v.length() || 1); sunK = c > 0.6 ? Math.min(1, (c - 0.6) / 0.38) : 0; }
    expo += (1.05 * (1 - 0.24 * sunK * sunK) * wExpo - expo) * (1 - Math.exp(-2.5 * dt)); renderer.toneMappingExposure = expo;
    // grade
    flashK = Math.max(0, flashK - dt * 1.8); hitAber = Math.max(0, hitAber - dt * 2.5);
    if (grade) {
      const p = F && F.player, hullF = p ? Math.max(0, p.hull / p.hullMax) : 1;
      grade.uniforms.uFlash.value = flashK * 0.6 + (tunnelT >= 0 && tunnelT > 0.5 && tunnelT < 0.75 ? 0.6 : 0);
      grade.uniforms.uAber.value = 0.0015 + hitAber * 0.03 + (p && p.boosting ? 0.006 : 0) + (tunnelT >= 0 ? 0.02 : 0) + cruiseK * 0.012;
      grade.uniforms.uDmg.value = p && ph === 'combat' ? Math.max(0, 0.35 - hullF) * 2.2 + (p.alive ? 0 : 0.3) : 0;
      grade.uniforms.uSat.value = p && ph === 'combat' ? 0.75 + 0.25 * Math.min(1, hullF * 2.5) : 1;
      grade.uniforms.uHeat.value = F && ph === 'combat' && F.surf ? F.surf.heat * 0.55 : 0;
      if (grade.uniforms.uWater) { const Sw = wIdx >= 0 ? WORLD.S : null; grade.uniforms.uWater.value = Sw && Sw.sea > 0 && (Sw.liquid === 1 || Sw.liquid === 4) && amb.alt < -0.3 ? 1 : 0; }   // the camera under the sea surface
      if (!(F && (F.dock || F.outcome === 3))) fadeK = Math.max(0, fadeK - dt * 1.4);
      grade.uniforms.uFade.value = mapMode ? 0 : fadeK;
      if (flashK <= 0.01) grade.uniforms.uFlashCol.value.setRGB(1, 1, 1);
    }
    // sun shafts in the air: the sun's place on screen; the rays strongest with the sun low and through broken cloud
    if (post) {
      const ps = post.sun; ps.on = false;
      if (wIdx >= 0 && wAirK > 0.2 && space.sys.sunDir) {
        const sdv = space.sys.sunDir;
        _v.copy(sdv).multiplyScalar(5000).add(cam.position).project(cam);
        const facing = _v2.copy(sdv).applyQuaternion(_q.copy(cam.quaternion).invert()).z < 0;
        if (facing && Math.abs(_v.x) < 1.6 && Math.abs(_v.y) < 1.6) {
          const ux = cam.position.x - _wc[0], uy = cam.position.y - _wc[1], uz = cam.position.z - _wc[2], ul = Math.max(1, Math.hypot(ux, uy, uz));
          const lowSun = 1 - Math.min(1, Math.max(0, (sdv.x * ux + sdv.y * uy + sdv.z * uz) / ul) * 1.6);
          ps.on = true; ps.x = _v.x * 0.5 + 0.5; ps.y = _v.y * 0.5 + 0.5;
          ps.k = wAirK * (0.35 + 0.65 * lowSun) * (1 - Math.max(0, Math.max(Math.abs(_v.x), Math.abs(_v.y)) - 1) / 0.6) * 0.55;
          const s = amb.sun, m = Math.max(1e-3, s[0], s[1], s[2]); grade.uniforms.uShaftCol.value.setRGB(s[0] / m, s[1] / m, s[2] / m);
        }
      }
    }
    fx.plumesEnd();
    render();
    perf.cpu += (performance.now() - now - perf.cpu) * 0.05;   // the frame's main-thread cost (GL calls queued, not waited for)
    if (window.__czSync && gl) { gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, SYNC_PX); perf.work[perf.wi++ % perf.work.length] = performance.now() - now; }
    adaptResolution(ph);
  }

  // dynamic resolution: hold ~60 fps by trading pixels, never below 60 % of the tier's scale
  let adaptT = 0;
  function adaptResolution(ph) {
    adaptT += 1;
    if (adaptT < 90 || ph !== 'combat' || window.__czNoDynRes) return;   // the flag pins the scale (perf probes)
    adaptT = 0;
    const base = renderer.getPixelRatio(), maxS = Math.min(Q.scale, 1.35) * (tier.name === 'high' ? dpr : 1), minS = maxS * 0.6;
    let ns = base;
    if (perf.ema > 18 && base > minS + 0.01) ns = Math.max(minS, base * (perf.ema > 24 ? 0.85 : 0.92));
    else if (perf.ema < 12.5 && base < maxS - 0.01) ns = Math.min(maxS, base * 1.08);
    if (Math.abs(ns - base) > 0.01) { renderer.setPixelRatio(ns); perf.scale = ns; fit(canvas.clientWidth || 960, canvas.clientHeight || 540); }
  }

  function dispose() {
    clearVisuals(); fx.dispose(); WORLD.dispose(); space.dispose(); GAL.dispose(); plasmaMat.dispose(); plasmaGeo.dispose(); gearGeo.dispose(); gearMat.dispose(); if (post) post.dispose(); hud.dispose(); ui.dispose();
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

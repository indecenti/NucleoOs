// cz-preview-server.mjs — TEMPORARY local preview of the Costellazioni 3D web game so it can be SEEN
// in a real browser (HUD, ship rendering, cursor) instead of only headless-logic-tested. Serves the
// real apps/games/www tree under the /apps/games/ URL prefix the modules import from, mocks the device
// /api/* endpoints (save -> 404 = fresh run), and serves a minimal page that boots the REAL GameHarness
// + the real constellations game with single-player host wiring. Delete after debugging.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { join, extname, normalize, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');   // repo root (tools/..), works on any machine
const WWW = process.env.CZ_WWW || join(REPO, 'apps', 'games', 'www');
const MODELS_DIR = process.env.CZ_MODELS || join(REPO, 'apps', 'games', 'www', 'games', 'models');   // GLBs for the picker
const PORT = 7359;
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.json': 'application/json', '.png': 'image/png', '.svg': 'image/svg+xml', '.wasm': 'application/wasm', '.glb': 'model/gltf-binary', '.gltf': 'model/gltf+json' };

const VIEWER_HTML = [
  '<!doctype html><html><head><meta charset="utf-8"><style>html,body{margin:0;height:100%;background:#0a0e16;overflow:hidden;font-family:monospace}#labels div{position:absolute;color:#9fe6ff;font-size:14px;font-weight:bold;transform:translateX(-50%);text-shadow:0 0 4px #000}</style>',
  '<script type="importmap">{"imports":{"three":"https://unpkg.com/three@0.169.0/build/three.module.js"}}</' + 'script></head><body><div id="labels"></div><canvas id="c"></canvas>',
  '<script type="module">',
  'import * as THREE from "three";',
  'import { GLTFLoader } from "https://unpkg.com/three@0.169.0/examples/jsm/loaders/GLTFLoader.js";',
  'const canvas=document.getElementById("c"); const r=new THREE.WebGLRenderer({canvas,antialias:true}); r.setSize(innerWidth,innerHeight); r.setPixelRatio(Math.min(2,devicePixelRatio));',
  'const scene=new THREE.Scene(); scene.background=new THREE.Color(0x0a0e16);',
  'scene.add(new THREE.AmbientLight(0x8090b0,0.7)); const k=new THREE.DirectionalLight(0xffffff,1.5); k.position.set(3,5,4); scene.add(k); const rim=new THREE.DirectionalLight(0x88ddff,0.9); rim.position.set(-3,-1,-4); scene.add(rim);',
  'const cam=new THREE.PerspectiveCamera(42,innerWidth/innerHeight,0.1,100); cam.position.set(0,0,12);',
  'const files=["spaceship","s00","s01","s02","s03","s04","s05","s06","s08","s09"];',
  'const loader=new GLTFLoader(); const objs=[]; const labelsEl=document.getElementById("labels"); const cols=4, sp=2.5;',
  'files.forEach((f,i)=>{ loader.load("/mt/"+f+".glb", g=>{ const o=g.scene; const box=new THREE.Box3().setFromObject(o); const sz=box.getSize(new THREE.Vector3()); const ctr=box.getCenter(new THREE.Vector3()); const s=1.6/Math.max(sz.x,sz.y,sz.z,0.001); o.scale.setScalar(s); o.position.set(-ctr.x*s,-ctr.y*s,-ctr.z*s); const wrap=new THREE.Group(); wrap.add(o); const col=i%cols,row=Math.floor(i/cols); wrap.position.set((col-(cols-1)/2)*sp,(1-row)*2.6,0); scene.add(wrap); objs.push(wrap); const lab=document.createElement("div"); lab.textContent=f; labelsEl.appendChild(lab); wrap.userData.lab=lab; }, undefined, e=>{ const lab=document.createElement("div"); lab.textContent=f+" FAIL"; lab.style.color="#f55"; labelsEl.appendChild(lab); }); });',
  'function loop(){ requestAnimationFrame(loop); for(const o of objs){ o.rotation.y+=0.012; const p=o.position.clone().project(cam); const lab=o.userData.lab; if(lab){ lab.style.left=(p.x*0.5+0.5)*innerWidth+"px"; lab.style.top=((-p.y*0.5+0.5)*innerHeight+innerHeight*0.16)+"px"; } } r.render(scene,cam); }',
  'loop(); window.__loaded=()=>objs.length;',
  '</' + 'script></body></html>',
].join('\n');

const TEST_HTML = [
  '<!doctype html><html><head><meta charset="utf-8"><title>cz test</title>',
  '<style>html,body{margin:0;height:100%;background:#05070d;overflow:hidden}',
  '#boardWrap{position:absolute;inset:0}#board{width:100%;height:100%;display:block;background:#05070d}',
  '#reacts{position:absolute;inset:0;pointer-events:none}</style></head><body>',
  '<div id="boardWrap"><canvas id="board"></canvas><div id="reacts"></div></div>',
  '<script type="module">',
  'import { GameHarness } from "/apps/games/nucleo-game.js";',
  'import game from "/apps/games/games/constellations.js";',
  'const play = { seat:0, isHost:true, roomId:"cztest", roster:[{seat:0,name:"pilot",ai:false}], on(){}, send(){} };',
  'const canvas = document.getElementById("board");',
  'const harness = new GameHarness(play, game, canvas);',
  'window.harness = harness; window.def = game; window.log = [];',
  'function fit(){ const dpr = Math.min(2, window.devicePixelRatio||1); const r = canvas.getBoundingClientRect(); canvas.width = Math.max(2,Math.round(r.width*dpr)); canvas.height = Math.max(2,Math.round(r.height*dpr)); try{ harness.resize(canvas.width, canvas.height); }catch(e){} }',
  'harness.start(); harness.begin({}); fit(); window.addEventListener("resize", fit);',
  'let lockX=0, lockY=0;',
  'window.addEventListener("keydown", e=>{ if(e.key==="Tab")e.preventDefault(); harness.feedKey(e.key); });',
  'window.addEventListener("keyup", e=> harness.feedKeyUp(e.key));',
  'const toC = e=>{ const r=canvas.getBoundingClientRect(); return { x:(e.clientX-r.left)*canvas.width/r.width, y:(e.clientY-r.top)*canvas.height/r.height }; };',
  'canvas.oncontextmenu=e=>e.preventDefault();',
  'canvas.onpointerdown = e=>{ const alt=e.button===2; if(document.pointerLockElement===canvas){ if(game.pointerAxes==="xy"){ if(alt) harness.feedKey("m"); else if(e.button===0) harness.feedPointer(lockX,lockY);} return;} if(alt){ harness.feedKey("m"); return;} if(e.button!==0) return; const p=toC(e); harness.feedPointer(p.x,p.y); harness.feedPointerMove(p.x,p.y); };',
  'canvas.onpointermove = e=>{ if(document.pointerLockElement===canvas){ lockY=Math.max(0,Math.min(canvas.height,lockY+e.movementY)); if(game.pointerAxes==="xy"){ lockX=Math.max(0,Math.min(canvas.width,lockX+e.movementX)); harness.feedPointerMove(lockX,lockY);} else harness.feedPointerMove(canvas.width/2,lockY); return;} if(!e.buttons) return; const p=toC(e); harness.feedPointerMove(p.x,p.y); };',
  'document.addEventListener("pointerlockchange", ()=>{ if(document.pointerLockElement===canvas){ lockX=canvas.width/2; lockY=canvas.height/2; } });',
  'const origErr = console.error; console.error = (...a)=>{ window.log.push(a.map(String).join(" ")); origErr(...a); };',
  '// debug driver helpers',
  'window.cz = {',
  '  phase: ()=> harness.state && harness.state.phase,',
  '  screen: ()=> harness.state && harness.state.screen,',
  '  st: ()=> { const s=harness.state||{}; return { phase:s.phase, screen:s.screen, foes:(s.foes||[]).length, wave:s.wave, waves:s.cc&&s.cc.waves, lock:s.lock, hull:s.hull, shield:s.shield, kills:s.kills }; },',
  '  key: k=> harness.feedKey(k),',
  '  aim: (x,y)=> harness.feedPointerMove(x,y),',
  '  newRun(){ harness.feedKey(" "); },',
  '  launch(slot){ harness.feedKey("5"); for(let i=0;i<(slot||0);i++) harness.feedKey("s"); harness.feedKey(" "); },',
  '};',
  '</script></body></html>',
].join('\n');

createServer(async (req, res) => {
  const url = decodeURIComponent((req.url || '/').split('?')[0]);
  if (url === '/viewer') { res.writeHead(200, { 'content-type': 'text/html', 'cache-control': 'no-store' }); res.end(VIEWER_HTML); return; }
  if (url.startsWith('/mt/')) {
    try { const data = await readFile(normalize(join(MODELS_DIR, url.slice('/mt/'.length)))); res.writeHead(200, { 'content-type': 'model/gltf-binary', 'cache-control': 'no-store' }); res.end(data); }
    catch { res.writeHead(404); res.end(); } return;
  }
  if (url.startsWith('/api/game/costellazioni/save')) { res.writeHead(404); res.end(); return; }
  if (url.startsWith('/api/')) { res.writeHead(200, { 'content-type': 'application/json' }); res.end(JSON.stringify({ ok: true, storage: { mounted: true, fs: 'FAT', free_bytes: 4e9 } })); return; }
  if (url === '/' || url === '/index.html' || url === '/_cztest.html') { res.writeHead(200, { 'content-type': 'text/html', 'cache-control': 'no-store' }); res.end(TEST_HTML); return; }
  const rel = url.startsWith('/apps/games/') ? url.slice('/apps/games/'.length) : url.replace(/^\//, '');
  const file = normalize(join(WWW, rel));
  if (!file.startsWith(normalize(WWW))) { res.writeHead(403); res.end(); return; }
  try {
    const data = await readFile(file);
    res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream', 'cache-control': 'no-store' });
    res.end(data);
  } catch { res.writeHead(404); res.end('not found: ' + url); }
}).listen(PORT, () => console.log('cz preview on http://localhost:' + PORT));

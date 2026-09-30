// Host gate for the firmware app registry. Two layers:
//  1. (always, also in CI) every app the release ships fits the firmware's registry limits
//     (firmware/components/nucleo_registry/include/nucleo_registry.h), and the cap leaves room for the
//     user's own Agent-published apps — the old 48-app cap equalled the shipped count, so every user app
//     was silently dropped.
//  2. (when the ESP-IDF cJSON is present — the local gate) compile the REAL nucleo_registry.c and load
//     three fixture cards: the real registry (route/icon/name must come out byte-identical to the manifest
//     strings /api/apps has always served), real + one Agent app (it must load), and 90 apps (exactly the
//     cap loads).
// Run: node tools/anima-host/registry-check.mjs      (wired as npm run registry:test, ANIMA gate, CI)
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync, copyFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build', 'registry-host');
const HDR = readFileSync('firmware/components/nucleo_registry/include/nucleo_registry.h', 'utf8');
const num = (re) => { const m = re.exec(HDR); if (!m) throw new Error(`registry header: ${re}`); return +m[1]; };
const CAP = num(/#define NUCLEO_MAX_APPS\s+(\d+)/);
const ID_MAX = num(/#define NUCLEO_APP_ID_MAX\s+(\d+)/);
const fieldLen = (name) => num(new RegExp(`char\\s+${name}\\[(\\d+)\\]`));
const VER_MAX = fieldLen('version') - 1, NAME_MAX = fieldLen('name') - 1, ICON_MAX = fieldLen('icon_raw') - 1;
const USER_HEADROOM = 16;   // slots that must stay free for the user's own apps

let fail = 0;
const bad = (m) => { fail++; console.log(`FAIL ${m}`); };
const reg = JSON.parse(readFileSync('registry/apps.json', 'utf8')).installed;
const manifestOf = (id) => { try { return JSON.parse(readFileSync(`apps/${id}/manifest.json`, 'utf8')); } catch { return null; } };

// ---- layer 1: limits ----
if (reg.length + USER_HEADROOM > CAP) bad(`registry ships ${reg.length} apps; cap ${CAP} leaves < ${USER_HEADROOM} for user apps`);
for (const a of reg) {
  const m = manifestOf(a.id) || {};
  if (a.id.length > ID_MAX) bad(`${a.id}: id longer than ${ID_MAX}`);
  if ((a.version || '').length > VER_MAX) bad(`${a.id}: version longer than ${VER_MAX}`);
  if ((m.name || '').length > NAME_MAX) bad(`${a.id}: name longer than ${NAME_MAX} (would be truncated)`);
  if (m.web_route !== undefined && m.web_route !== `/apps/${a.id}/`) bad(`${a.id}: web_route '${m.web_route}' is not /apps/<id>/`);
  if (m.icon !== undefined && m.icon !== `/apps/${a.id}/icon.svg` && Buffer.byteLength(String(m.icon)) > ICON_MAX)
    bad(`${a.id}: icon '${m.icon}' longer than ${ICON_MAX} bytes`);
}
const agentIdRe = /isValidId\(id\)\s*\{\s*return\s*\/\^\[a-z\]\[a-z0-9-\]\{1,(\d+)\}\$\//.exec(readFileSync('apps/agent/www/app-publish.js', 'utf8'));
if (!agentIdRe) bad('could not read the Agent app id rule (apps/agent/www/app-publish.js isValidId)');
else if (1 + +agentIdRe[1] > ID_MAX) bad(`Agent allows ${1 + +agentIdRe[1]}-char ids, firmware keeps ${ID_MAX}`);
console.log(`registry limits: ${reg.length} shipped apps, cap ${CAP} (${CAP - reg.length} free for user apps), id<=${ID_MAX}`);

// ---- layer 2: the real C ----
const CJSON = process.env.IDF_PATH ? join(process.env.IDF_PATH, 'components/json/cJSON') : 'C:/esp/esp-idf/components/json/cJSON';
if (!existsSync(join(CJSON, 'cJSON.c'))) {
  console.log(`registry C gate: SKIP (no ESP-IDF cJSON at ${CJSON})`);
  console.log(`registry: ${fail ? 'FAIL' : 'ok'} (limits only)`);
  process.exit(fail ? 1 : 0);
}
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
rmSync(BUILD, { recursive: true, force: true });
mkdirSync(BUILD, { recursive: true });
const exe = join(BUILD, process.platform === 'win32' ? 'registryctest.exe' : 'registryctest');
const cc = spawnSync(GCC, ['-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation',
  '-I', 'firmware/components/nucleo_registry/include', '-I', 'tools/ui-host/shim', '-I', CJSON,
  'tools/anima-host/registry-ctest.c', 'firmware/components/nucleo_registry/nucleo_registry.c', join(CJSON, 'cJSON.c'),
  '-o', exe], { cwd: ROOT, env, encoding: 'utf8' });
if (cc.status !== 0) { console.log('registry: COMPILE FAILED'); process.stdout.write(cc.stdout || ''); process.stdout.write(cc.stderr || ''); process.exit(1); }

// A fixture card: sd/system/registry/apps.json + sd/apps/<id>/manifest.json, and the expected rows.
function fixture(name, entries, manifests) {
  const dir = join(BUILD, name), sd = join(dir, 'sd');
  mkdirSync(join(sd, 'system', 'registry'), { recursive: true });
  writeFileSync(join(sd, 'system', 'registry', 'apps.json'), JSON.stringify({ schema: 1, installed: entries }, null, 2));
  const rows = [];
  for (const e of entries) {
    const m = manifests[e.id];
    if (m) {
      mkdirSync(join(sd, 'apps', e.id), { recursive: true });
      if (m.__copy) copyFileSync(m.__copy, join(sd, 'apps', e.id, 'manifest.json'));
      else writeFileSync(join(sd, 'apps', e.id, 'manifest.json'), JSON.stringify(m));
    }
    const mm = m ? (m.__copy ? JSON.parse(readFileSync(m.__copy, 'utf8')) : m) : {};
    const stdIcon = `/apps/${e.id}/icon.svg`;
    const icon = mm.icon === undefined ? '' : (Buffer.byteLength(String(mm.icon)) > ICON_MAX && mm.icon !== stdIcon ? stdIcon : String(mm.icon));
    rows.push([e.id, (mm.name || e.id).slice(0, NAME_MAX), mm.web_route === undefined ? '' : `/apps/${e.id}/`,
      icon, e.enabled ? '1' : '0'].join('\t'));
  }
  writeFileSync(join(dir, 'expected.tsv'), rows.slice(0, CAP).join('\n') + '\n');
  return dir;
}
function run(label, dir, count) {
  const r = spawnSync(exe, [dir, join(dir, 'expected.tsv'), String(count)], { env, encoding: 'utf8' });
  const out = (r.stdout || '').trim();
  console.log(`  ${label}: ${out.split('\n').pop()}`);
  if (r.status !== 0) { bad(label); console.log(out); if (r.stderr) console.log(r.stderr); }
}

const realManifests = Object.fromEntries(reg.filter((a) => existsSync(`apps/${a.id}/manifest.json`))
  .map((a) => [a.id, { __copy: join(ROOT, 'apps', a.id, 'manifest.json') }]));
run('real registry, byte-identical fields', fixture('real', reg, realManifests), reg.length);

const agent = { id: 'my-agent-app-with-24chr', version: '0.1.0', path: '/apps/my-agent-app-with-24chr', enabled: true, created_by: 'agent' };
run('real + one Agent app (24-char id)', fixture('agent', [...reg, agent], {
  ...realManifests, [agent.id]: { id: agent.id, name: 'My agent app', web_route: `/apps/${agent.id}/`, icon: 'icon.svg', created_by: 'agent' },
}), reg.length + 1);

const many = Array.from({ length: CAP + 10 }, (_, i) => ({ id: `app-${i}`, version: '1.0.0', enabled: i % 2 === 0 }));
const manyM = Object.fromEntries(many.map((e, i) => [e.id, i % 3 === 0 ? { name: `App ${i}`, web_route: '/custom/route/', icon: 'x'.repeat(40) }
  : { name: `App ${i}`, web_route: `/apps/${e.id}/`, icon: i % 3 === 1 ? `/apps/${e.id}/icon.svg` : 'assets/icon.svg' }]));
run(`${CAP + 10} apps -> exactly the cap loads (custom route / long icon fall back)`, fixture('many', many, manyM), CAP);

console.log(`registry: ${fail ? 'FAIL' : 'ok'}`);
process.exit(fail ? 1 : 0);

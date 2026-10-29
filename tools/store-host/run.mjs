#!/usr/bin/env node
// Host gate for the app-store client (npm run store:test).
//
// Builds a throw-away store with the store repository's OWN tools (tools/store.py from
// indecenti/nucleoos-cardputer-store: NUCLEO_CARDPUTER_STORE_REPO, default ../nucleoos-cardputer-store) and
// throw-away keys, compiles the REAL firmware engine (nucleo_store/store_engine.c + store_verify.c, the
// SD-content SHA-256 and registry code) against mbedTLS built from ESP-IDF's sources, and runs every
// scenario the device must survive: install, update, remove, tampering, foreign keys, rollback, expiry,
// conflicts with system apps, network faults. Nothing here touches the real store or its keys.
import { execFileSync, spawnSync } from 'node:child_process';
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync, appendFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '..', '..');
const STORE_REPO = process.env.NUCLEO_CARDPUTER_STORE_REPO || join(root, '..', 'nucleoos-cardputer-store');
const STORE_TOOL = join(STORE_REPO, 'tools', 'store.py');
const MBED = 'C:/esp/esp-idf/components/mbedtls/mbedtls';
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(`${MINGW}/gcc.exe`) ? `${MINGW}/gcc.exe` : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH}` };
const OUT = join(root, 'build', 'store-host');
const PY = process.platform === 'win32' ? 'python' : 'python3';

if (!existsSync(STORE_TOOL)) { console.error(`store-host: ${STORE_TOOL} not found (set NUCLEO_CARDPUTER_STORE_REPO)`); process.exit(1); }
mkdirSync(join(OUT, 'mbed'), { recursive: true });

// ---- a throw-away store ---------------------------------------------------------------------------------
const work = mkdtempSync(join(tmpdir(), 'nstore-'));
const keys = mkdtempSync(join(tmpdir(), 'nstore-keys-'));
const store = join(work, 'store');
const sd = join(work, 'sd');
const senv = { ...process.env, NUCLEO_STORE_ROOT: store, NUCLEO_CARDPUTER_STORE_KEY: join(keys, 'k.pem'),
               NUCLEO_CARDPUTER_STORE_BACKUP_KEY: join(keys, 'kb.pem') };
const tool = (...a) => execFileSync(PY, [STORE_TOOL, ...a], { env: senv, encoding: 'utf8' });
const LANGS = ['it', 'en', 'es', 'fr', 'de'];
function app(id, code, files, extra = {}) {
  const d = join(store, 'apps', 'web', id);
  rmSync(join(d, 'files'), { recursive: true, force: true });
  mkdirSync(join(d, 'files'), { recursive: true });
  const m = { id, kind: 'web', version: `1.0.${code}`, version_code: code, min_os: '0.7.0', category: 'games',
    name: Object.fromEntries(LANGS.map((l) => [l, `${id} ${l}`])), description: Object.fromEntries(LANGS.map((l) => [l, `about ${id}`])),
    author: 'Test', license: 'MIT', source: 'https://example.org', permissions: [], ...extra };
  writeFileSync(join(d, 'manifest.json'), JSON.stringify(m));
  // A web app that passes every Cardputer rule (docs/rules.md in the store): its own icon, five catalogs,
  // a NucleoOS manifest; a given manifest.json is merged into it.
  const appManifest = { id, runtime: 'web', web_route: `/apps/${id}/`, icon: 'icon.svg', version: m.version,
    ...(files['manifest.json'] ? JSON.parse(files['manifest.json']) : {}) };
  files = {
    'www/index.html': '<h1>' + id + '</h1>',
    'www/icon.svg': `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect width="64" height="64" rx="12"/><circle cx="32" cy="32" r="${10 + code}"/><text y="60">${id}</text></svg>`,
    ...Object.fromEntries(LANGS.map((l) => [`www/i18n.${l}.json`, JSON.stringify({ _lang: l, t: id })])),
    'device-icon.txt': Array.from({ length: 32 }, (_, y) => Array.from({ length: 32 }, (_, x) =>
      (x === 0 || x === 31 || y === 0 || y === 31 || (x > 4 && x < 4 + id.length + code && y === 16)) ? '#' : '.').join('')).join('\n') + '\n',
    ...files,
    'manifest.json': JSON.stringify(appManifest),
  };
  for (const [rel, data] of Object.entries(files)) {
    const p = join(d, 'files', rel);
    mkdirSync(dirname(p), { recursive: true });
    writeFileSync(p, data);
  }
}
// A native app: the store checks the module's header and info record (the firmware loader gate, npm run
// native:test, checks real modules); the engine treats app.nap as one more signed file.
function nativeApp(id, code, abi = 1, nap = null) {
  const d = join(store, 'apps', 'native', id);
  rmSync(join(d, 'files'), { recursive: true, force: true });
  mkdirSync(join(d, 'files'), { recursive: true });
  writeFileSync(join(d, 'manifest.json'), JSON.stringify({ id, kind: 'native', version: `1.0.${code}`, version_code: code,
    min_os: '0.7.0', category: 'games', abi, name: Object.fromEntries(LANGS.map((l) => [l, `${id} ${l}`])),
    description: Object.fromEntries(LANGS.map((l) => [l, `about ${id}`])), author: 'Test', license: 'MIT',
    source: 'https://example.org', permissions: [],
    build: { repo: 'indecenti/NucleoOs', commit: '0123456789abcdef0123456789abcdef01234567', register: 'r', sources: ['a.cpp'] } }));
  const elf = Buffer.alloc(52 + 64); Buffer.from('\x7fELF\x01\x01\x01', 'latin1').copy(elf);
  elf.writeUInt16LE(3, 16); elf.writeUInt16LE(94, 18); elf.writeUInt32LE(1, 20);
  const info = Buffer.alloc(52); info.writeUInt32LE(0x3150414e, 0); info.writeUInt16LE(abi, 4); info.write(id, 16, 'ascii');
  writeFileSync(join(d, 'files', 'app.nap'), nap || Buffer.concat([elf, info, Buffer.alloc(9000, code)]));
  writeFileSync(join(d, 'files', 'device-icon.txt'), Array.from({ length: 32 }, (_, y) => Array.from({ length: 32 }, (_, x) =>
    (x === 0 || x === 31 || y === 0 || y === 31 || (y === 4 + id.charCodeAt(0) % 20 && x > 2 && x < 9 + code)) ? '#' : '.').join('')).join('\n') + '\n');
}
tool('keygen'); tool('keygen', '--backup');
const big = Buffer.alloc(6144 * 3, 7);                 // exactly three windows: the 416 end path
const odd = Buffer.from(Array.from({ length: 20011 }, (_, i) => (i * 31) & 255));
app('alpha', 1, { 'manifest.json': '{"id":"alpha","version":"1.0.1","permissions":["device.mic"]}', 'www/index.html': '<h1>alpha</h1>', 'www/big.bin': big, 'www/a/b/odd.bin': odd });
app('beta', 1, { 'manifest.json': '{"id":"beta"}', 'www/index.html': 'beta' });
nativeApp('gamma', 1);
nativeApp('delta', 1, 2);                                // built for ABI 2: newer than the test firmware
tool('publish');
tool('firmware-keys', '--out', join(OUT, 'test_keys.c'));

// ---- compile the real engine -----------------------------------------------------------------------------
const lib = join(OUT, 'mbed', 'libmbedcrypto-host.a');
if (!existsSync(lib)) {
  console.log('store-host: building mbedTLS for the host (once)...');
  const objs = [];
  for (const f of readdirSync(join(MBED, 'library')).filter((f) => f.endsWith('.c'))) {
    const o = join(OUT, 'mbed', f.replace(/\.c$/, '.o'));
    execFileSync(GCC, ['-std=gnu11', '-O1', '-w', '-I', join(MBED, 'include'), '-I', join(MBED, 'library'), '-c', join(MBED, 'library', f), '-o', o], { env });
    objs.push(o);
  }
  execFileSync(join(MINGW, 'ar.exe'), ['rcs', lib, ...objs], { env });
}
const C = join(root, 'firmware', 'components');
const exe = join(OUT, 'store_cli.exe');
const cc = spawnSync(GCC, ['-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-result',
  '-I', join(C, 'nucleo_store', 'include'), '-I', join(C, 'nucleo_sdcontent', 'include'), '-I', join(MBED, 'include'),
  join(here, 'store_cli.c'), join(C, 'nucleo_store', 'store_engine.c'), join(C, 'nucleo_store', 'store_verify.c'),
  join(C, 'nucleo_sdcontent', 'sdc_sha256.c'), join(C, 'nucleo_sdcontent', 'sdc_registry.c'), join(OUT, 'test_keys.c'),
  lib, ...(process.platform === 'win32' ? ['-lbcrypt'] : []), '-o', exe], { env, encoding: 'utf8' });
if (cc.status !== 0) { console.error('store-host: COMPILE FAILED\n' + cc.stdout + cc.stderr); process.exit(1); }

// ---- scenarios -------------------------------------------------------------------------------------------
let fails = 0;
const pubRoot = join(store, 'public');
const pub = join(pubRoot, 'web');
function cli(cmd, arg = '', fault = '', channel = 'web', abi = '1', arg2 = '') {
  const r = spawnSync(exe, [pubRoot, sd, cmd, ...(arg !== '' ? [String(arg)] : []), ...(arg2 ? [arg2] : [])],
    { env: { ...env, FAULT: fault, CHANNEL: channel, NATIVE_ABI: abi }, encoding: 'utf8' });
  const out = r.stdout || '';
  const rc = Number((out.match(/rc=(-?\d+)/) || [])[1]);
  return { out, rc };
}
function check(name, cond, info = '') {
  console.log(`${cond ? 'ok  ' : 'FAIL'} ${name}${cond ? '' : '  ' + info}`);
  if (!cond) fails++;
}
const ST = { OK: 0, NO_SD: 1, NET: 2, SIG: 3, ROLLBACK: 4, EXPIRED: 5, BAD: 6, VERIFY: 7, WRITE: 8, NOTFOUND: 10, CONFLICT: 11,
  UNSUPPORTED: 12, ABI: 14 };
const reg = join(sd, 'system', 'registry', 'apps.json');
const regHas = (id) => readFileSync(reg, 'utf8').includes(`"id":"${id}"`) || readFileSync(reg, 'utf8').includes(`"id": "${id}"`);

check('path and version rules', cli('paths').rc === ST.OK, cli('paths').out);
check('no SD card', cli('refresh').rc === ST.NO_SD);
mkdirSync(join(sd, 'system', 'registry'), { recursive: true });
cpSync(join(root, 'registry', 'apps.json'), reg);
const regBefore = readFileSync(reg, 'utf8');

let r = cli('refresh');
check('refresh: catalog verified', r.rc === ST.OK && /serial=1/.test(r.out), r.out);
r = cli('list');
check('catalog lists both apps', /count=2/.test(r.out) && /app alpha web 1 alpha en/.test(r.out), r.out);

r = cli('install', 'alpha');
check('install alpha (Range windows, 416 end, nested paths)', r.rc === ST.OK && /files=11/.test(r.out), r.out);
check('installed files are byte-exact', readFileSync(join(sd, 'apps', 'alpha', 'www', 'big.bin')).equals(big)
  && readFileSync(join(sd, 'apps', 'alpha', 'www', 'a', 'b', 'odd.bin')).equals(odd));
check('store owns it', /installed=1 system=0/.test(cli('installed', 'alpha').out));
check('registered in the web OS, existing entries kept', regHas('alpha') && readFileSync(reg, 'utf8').includes('"file-commander"'));
check('registry entry carries the app permissions', /"id":"alpha"[^}]*"permissions":\["device.mic"\]/.test(readFileSync(reg, 'utf8')));
check('no temp files left', !existsSync(join(sd, 'apps', '.tmp', 'alpha')) && !existsSync(join(sd, 'apps', '.old', 'alpha')));

// A system app (on the card, not installed by the store) is never touched.
mkdirSync(join(sd, 'apps', 'beta'), { recursive: true });
writeFileSync(join(sd, 'apps', 'beta', 'mine.txt'), 'user data');
r = cli('install', 'beta');
check('system app with the same id: refused, untouched', r.rc === ST.CONFLICT && readFileSync(join(sd, 'apps', 'beta', 'mine.txt'), 'utf8') === 'user data', r.out);
check('system app cannot be removed by the store', cli('remove', 'beta').rc === ST.CONFLICT && existsSync(join(sd, 'apps', 'beta', 'mine.txt')));
rmSync(join(sd, 'apps', 'beta'), { recursive: true });

// Network faults: dropped connections are retried; a dead network fails cleanly.
r = cli('install', 'beta', 'drop');
check('dropped connections are retried', r.rc === ST.OK, r.out);
r = cli('refresh', '', 'dead');
check('dead network: clean failure, old catalog kept', r.rc === ST.NET && /count=2/.test(cli('list').out), r.out);

// Tampering on the server after signing.
const victim = join(pub, 'apps', 'alpha', '1', 'www', 'index.html');
const orig = readFileSync(victim);
rmSync(join(sd, 'apps', 'alpha'), { recursive: true });      // reinstall from scratch over a tampered server
cpSync(join(sd, 'apps'), join(work, 'apps-snapshot'), { recursive: true });
writeFileSync(victim, '<h1>evil</h1>');
r = cli('install', 'alpha');
check('tampered file: refused, nothing installed', r.rc === ST.VERIFY && !existsSync(join(sd, 'apps', 'alpha')), r.out);
writeFileSync(victim, orig);
const idx = join(pub, 'index.txt');
const idxOrig = readFileSync(idx);
writeFileSync(idx, idxOrig.toString().replace('alpha en', 'alpha EN'));
r = cli('refresh');
check('tampered catalog: bad signature, old catalog kept', r.rc === ST.SIG && /count=2/.test(cli('list').out), r.out);
writeFileSync(idx, idxOrig);
const psig = join(pub, 'apps', 'alpha', '1', 'package.sig');
const psigOrig = readFileSync(psig);
writeFileSync(psig, psigOrig.toString().replace('www/index.html', 'www/index.htm_'));
r = cli('install', 'alpha');
check('swapped package.sig: refused (catalog names another)', r.rc === ST.VERIFY, r.out);
writeFileSync(psig, psigOrig);

// Expiry and rollback.
r = cli('refresh', String(Math.floor(Date.now() / 1000) + 400 * 86400));
check('expired catalog refused when the clock is set', r.rc === ST.EXPIRED, r.out);
check('unset clock: expiry not judged', cli('refresh', '0').rc === ST.OK);
const oldIdx = readFileSync(idx), oldSig = readFileSync(join(pub, 'index.sig'));
app('alpha', 2, { 'manifest.json': '{"id":"alpha","version":"1.0.2"}', 'www/index.html': '<h1>alpha 2</h1>' });
tool('publish');
r = cli('refresh');
check('new catalog serial 2', r.rc === ST.OK && /serial=2/.test(r.out), r.out);
r = cli('install', 'alpha');
check('update to version 2', r.rc === ST.OK && /installed=2/.test(cli('installed', 'alpha').out)
  && readFileSync(join(sd, 'apps', 'alpha', 'www', 'index.html'), 'utf8') === '<h1>alpha 2</h1>'
  && !existsSync(join(sd, 'apps', 'alpha', 'www', 'big.bin')), r.out);
check('update does not duplicate the registry entry', (readFileSync(reg, 'utf8').match(/"id":"alpha"/g) || []).length === 1);
// A corrupted UPDATE must leave the installed version exactly as it was.
app('alpha', 3, { 'manifest.json': '{"id":"alpha","version":"1.0.3"}', 'www/index.html': '<h1>alpha 3</h1>' });
tool('publish');
cli('refresh');
writeFileSync(join(pub, 'apps', 'alpha', '3', 'www', 'index.html'), '<h1>evil 3</h1>');
r = cli('install', 'alpha');
check('corrupted update: refused, version 2 untouched', r.rc === ST.VERIFY && /installed=2/.test(cli('installed', 'alpha').out)
  && readFileSync(join(sd, 'apps', 'alpha', 'www', 'index.html'), 'utf8') === '<h1>alpha 2</h1>', r.out);
writeFileSync(join(pub, 'apps', 'alpha', '3', 'www', 'index.html'), '<h1>alpha 3</h1>');
const newIdx = readFileSync(idx), newSig = readFileSync(join(pub, 'index.sig'));
writeFileSync(idx, oldIdx); writeFileSync(join(pub, 'index.sig'), oldSig);
r = cli('refresh');
check('replayed older catalog: rollback refused', r.rc === ST.ROLLBACK && /serial=3/.test(r.out), r.out);
writeFileSync(idx, newIdx); writeFileSync(join(pub, 'index.sig'), newSig);

// A catalog signed by a key the firmware does not trust.
const foreign = mkdtempSync(join(tmpdir(), 'nstore-foreign-'));
const fenv = { ...senv, NUCLEO_CARDPUTER_STORE_KEY: join(foreign, 'f.pem') };
execFileSync(PY, [STORE_TOOL, 'keygen'], { env: { ...fenv, NUCLEO_STORE_ROOT: join(foreign, 'root') } });
cpSync(join(foreign, 'root', 'keys', 'store.pem'), join(store, 'keys', 'foreign.pem'));   // the TOOL trusts it...
execFileSync(PY, [STORE_TOOL, 'sign'], { env: fenv });
r = cli('refresh');
check('foreign key: refused (the firmware does not trust it)', r.rc === ST.SIG, r.out);
rmSync(foreign, { recursive: true, force: true });

rmSync(join(store, 'keys', 'foreign.pem'));               // back to the trusted keys for what follows
tool('sign');
// ---- the native channel: its own catalog, never mixed with the web one --------------------------------------
const N = (cmd, arg = '', abi = '1', arg2 = '') => cli(cmd, arg, '', 'native', abi, arg2);
r = N('refresh');
check('native: own catalog verified (its own serial)', r.rc === ST.OK && /serial=[1-9]/.test(r.out), r.out);
r = N('list');
check('native: lists only native apps, with their ABI', /count=2/.test(r.out) && /app gamma native 1 gamma en abi 1/.test(r.out)
  && !/alpha/.test(r.out), r.out);
check('web catalog lists no native app', !/gamma|delta/.test(cli('list').out));
r = N('install', 'gamma');
check('native: install', r.rc === ST.OK && existsSync(join(sd, 'apps', 'gamma', 'app.nap')), r.out);
check('native: the signed package stays with the app', existsSync(join(sd, 'apps', 'gamma', '.package.sig'))
  && /channel native/.test(readFileSync(join(sd, 'apps', 'gamma', '.store-version'), 'utf8')));
check('native: not registered in the web OS', !regHas('gamma'));
check('native: installed file verifies against its signature', N('verify-installed', 'gamma', '1', 'app.nap').rc === ST.OK);
const nap = join(sd, 'apps', 'gamma', 'app.nap'), napOrig = readFileSync(nap);
const napBad = Buffer.from(napOrig); napBad[200] ^= 0xff; writeFileSync(nap, napBad);
check('native: a module edited on the card is refused before it runs', N('verify-installed', 'gamma', '1', 'app.nap').rc === ST.VERIFY);
writeFileSync(nap, napOrig);
const keep = join(sd, 'apps', 'gamma', '.package.sig'), keepOrig = readFileSync(keep);
writeFileSync(keep, keepOrig.toString().replace(/^sig .*$/m, 'sig ' + Buffer.alloc(70, 1).toString('base64')));
check('native: a forged package.sig on the card is refused', N('verify-installed', 'gamma', '1', 'app.nap').rc === ST.SIG);
writeFileSync(keep, keepOrig);
check('native: a file the package does not list is refused', N('verify-installed', 'gamma', '1', 'device-icon.tx_').rc !== ST.OK);
r = N('install', 'delta');
check('native: an app for a newer ABI is refused', r.rc === ST.ABI && !existsSync(join(sd, 'apps', 'delta')), r.out);
rmSync(join(sd, 'apps', 'gamma'), { recursive: true });
r = N('install', 'gamma', '0');
check('native: a firmware without native support installs none', r.rc === ST.UNSUPPORTED && !existsSync(join(sd, 'apps', 'gamma')), r.out);
r = cli('install', 'gamma');
check('a native app cannot be installed through the web channel', r.rc === ST.NOTFOUND || r.rc === ST.UNSUPPORTED, r.out);
// The web catalog (validly signed) served at the native URL: the signed channel line refuses it.
const nIdx = join(pubRoot, 'native', 'index.txt'), nSig = join(pubRoot, 'native', 'index.sig');
const nIdxOrig = readFileSync(nIdx), nSigOrig = readFileSync(nSig);
cpSync(join(pub, 'index.txt'), nIdx); cpSync(join(pub, 'index.sig'), nSig);
r = N('refresh');
check("native: another channel's signed catalog is refused", r.rc === ST.BAD, r.out);
writeFileSync(nIdx, nIdxOrig); writeFileSync(nSig, nSigOrig);

// Store art: the colour icons, a signed pack beside the catalog, fetched with it; never fails a refresh.
check('art: a store without icons for this app has none', /art=none/.test(cli('art', 'alpha').out));
execFileSync(PY, ['-c', "import sys;from PIL import Image;Image.new('RGB',(128,128),(255,0,0)).save(sys.argv[1])",
  join(store, 'apps', 'web', 'alpha', 'store-icon.png')]);
execFileSync(PY, [join(STORE_REPO, 'tools', 'art_pack.py')], { env: senv, encoding: 'utf8' });
r = cli('refresh');
let a = cli('art', 'alpha').out;
check('art: fetched with the catalog, verified, 48 px red', r.rc === ST.OK && /art=48 px0=f800/.test(a), r.out + a);
const artBin = join(pub, 'art.bin'), artSig = join(pub, 'art.sig');
const artOrig = readFileSync(artBin), sigOrig = readFileSync(artSig, 'utf8');
const forged = Buffer.from(artOrig); forged[forged.length - 1] ^= 0xff;
writeFileSync(artBin, forged);
writeFileSync(artSig, sigOrig.replace(/sig (.)/, (m, c) => 'sig ' + (c === 'A' ? 'B' : 'A')));
r = cli('refresh');
a = cli('art', 'alpha').out;
check('art: a forged pack is refused, the old art stays, the refresh still succeeds', r.rc === ST.OK && /art=48 px0=f800/.test(a), r.out + a);
rmSync(artBin); rmSync(artSig);
r = cli('refresh');
check('art: a store that stops publishing art (404) leaves the refresh ok', r.rc === ST.OK && /art=48/.test(cli('art', 'alpha').out), r.out);
writeFileSync(artBin, artOrig); writeFileSync(artSig, sigOrig);

// Remove.
r = cli('remove', 'alpha');
check('remove: folder and registry entry gone, others kept', r.rc === ST.OK && !existsSync(join(sd, 'apps', 'alpha'))
  && !regHas('alpha') && readFileSync(reg, 'utf8').includes('"file-commander"'), r.out);
check('registry stays valid JSON', (() => { try { JSON.parse(readFileSync(reg, 'utf8')); return true; } catch { return false; } })());
check('remove what is not installed', cli('remove', 'alpha').rc === ST.NOTFOUND);
cli('remove', 'beta');
check('after removing everything the registry is the original again',
  JSON.stringify(JSON.parse(readFileSync(reg, 'utf8'))) === JSON.stringify(JSON.parse(regBefore)));

rmSync(work, { recursive: true, force: true });
rmSync(keys, { recursive: true, force: true });
console.log(fails ? `store-host: ${fails} FAILED` : 'store-host: all scenarios passed');
process.exit(fails ? 1 : 0);

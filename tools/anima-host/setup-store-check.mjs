#!/usr/bin/env node
// SETUP-STORE-CHECK — nucleo_setup's three-tier config store (firmware/components/nucleo_setup/
// setup_store.c) + the hotspot credential core (ap_creds.c), host-compiled with MinGW against the REAL
// cJSON from ESP-IDF and an in-memory NVS (nvs_host.c): the SAME C the device runs. Two programs:
//   setup-store-ctest.c          the reset contract behind Settings ▸ Reset: fan-out, read order, erase
//                                of EVERY tier, honest failures, the seal against a racing save
//   setup-store-secrets-ctest.c  the removable SD mirror never holds a Wi-Fi or hotspot password while
//                                /cfg + NVS keep them; every recovery path (flash wipe, launcher install,
//                                no card, legacy card with plaintext passwords) still works, incl. an
//                                allocation failure at every malloc of a save; a deliberately OPEN
//                                hotspot survives AP restarts, reboots and flash-wipe recovery
// Plus a static drift guard on nucleo_setup.c + ap_creds.c: the persisted documents name the secrets the
// store strips ("ap_pass", each net's "pass"), no other password-like member is serialized, the hotspot
// goes through ap_creds, the factory reset erases both documents, and nothing touches the SD mirror
// paths except through the store.
// Wired as `npm run setupstore:test` + the ANIMA gate (gate.mjs).
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, rmSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const ROOT = process.cwd();
const BUILD = join(ROOT, 'build', 'setup-store');
const MINGW = 'C:/msys64/mingw64/bin';
const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
const EXE = process.platform === 'win32' ? '.exe' : '';

const cjsonDirs = [process.env.IDF_PATH && join(process.env.IDF_PATH, 'components/json/cJSON'),
  'C:/esp/esp-idf/components/json/cJSON'].filter(Boolean);
const CJSON = cjsonDirs.find((d) => existsSync(join(d, 'cJSON.c')));
if (!CJSON) { console.error(`setup-store: cJSON (ESP-IDF) not found — looked in ${cjsonDirs.join(', ')}; set IDF_PATH`); process.exit(1); }

// ---- static drift guard ----------------------------------------------------------------------------
let sFail = 0, sPass = 0;
const check = (cond, msg) => { if (cond) sPass++; else { sFail++; console.log(`  ✗ ${msg}`); } };
const stripComments = (t) => t.replace(/\/\/[^\n]*/g, '');
const src = stripComments(readFileSync(join(ROOT, 'firmware/components/nucleo_setup/nucleo_setup.c'), 'utf8'));
const apSrc = stripComments(readFileSync(join(ROOT, 'firmware/components/nucleo_setup/ap_creds.c'), 'utf8'));
const persisted = src + '\n' + apSrc;                               // every member setup.json / networks.json can hold
check(/SETUP_DOC\s*=\s*\{\s*SETUP_JSON\s*,\s*"setup"\s*,\s*SETUP_LEGACY\s*,\s*"ap_pass"\s*\}/.test(src),
  'nucleo_setup.c: SETUP_DOC strips "ap_pass" from the SD mirror');
check(/NETS_DOC\s*=\s*\{\s*NETS_JSON\s*,\s*"networks"\s*,\s*NETS_SD\s*,\s*"pass"\s*\}/.test(src),
  'nucleo_setup.c: NETS_DOC strips "pass" from the SD mirror');
check(/cJSON_AddStringToObject\(\s*doc\s*,\s*"ap_pass"/.test(apSrc) && /cJSON_AddStringToObject\(\s*o\s*,\s*"pass"/.test(src),
  'the stripped names are the members ap_creds_save()/save_networks() actually write');
check(/ap_creds_save\(\s*&s_ap\s*,\s*r\s*\)/.test(src) && /ap_creds_load\(\s*&s_ap\s*,\s*r\s*\)/.test(src),
  'nucleo_setup.c: save_config()/load_config() persist the hotspot through ap_creds (incl. the ap_open choice)');
check(/setup_store_erase\(\s*&SETUP_DOC\s*\)/.test(src) && /setup_store_erase\(\s*&NETS_DOC\s*\)/.test(src),
  'nucleo_setup.c: the factory reset erases both documents from every tier');
const secretish = [...persisted.matchAll(/cJSON_Add\w+ToObject\(\s*\w+\s*,\s*"([^"]+)"/g)].map((m) => m[1])
  .filter((k) => /pass|psk|key|secret|token|pin/i.test(k) && k !== 'ap_pass' && k !== 'pass');
check(secretish.length === 0, `nucleo_setup.c + ap_creds.c: no other secret-looking member is persisted (found: ${secretish.join(', ') || '-'}) — add it to the SD redaction`);
const sdUses = src.split('\n').filter((l) => /\b(SETUP_LEGACY|NETS_SD)\b/.test(l) && !/^\s*#define\b/.test(l) && !/_DOC\s*=/.test(l));
check(sdUses.length === 0, `nucleo_setup.c: the SD mirror paths are only reached through setup_store (${sdUses.map((l) => l.trim()).join(' | ') || '-'})`);
check(!/\b(write_file_atomic|persist_doc|load_doc|write_sd_backup)\s*\(/.test(src), 'nucleo_setup.c: no private tier writer bypasses setup_store');

// ---- build ---------------------------------------------------------------------------------------------
rmSync(BUILD, { recursive: true, force: true });
mkdirSync(BUILD, { recursive: true });
const cjsonObj = join(BUILD, 'cJSON.o');
const cc = spawnSync(GCC, ['-std=gnu11', '-O1', '-w', '-c', join(CJSON, 'cJSON.c'), '-o', cjsonObj], { cwd: ROOT, env, encoding: 'utf8' });
if (cc.status !== 0) { console.error(`setup-store: cJSON COMPILE FAILED\n${cc.stdout || ''}${cc.stderr || ''}`); process.exit(1); }

function build(name, srcs) {
  const exe = join(BUILD, name + EXE);
  const r = spawnSync(GCC, [
    '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation',   // as the component's CMakeLists
    '-include', 'tools/anima-host/setup-store-shim/host_compat.h',
    '-I', 'tools/anima-host/setup-store-shim',     // nvs.h (in-memory NVS)
    '-I', 'tools/anima-host/shim',                 // esp_err.h / esp_log.h
    '-I', 'firmware/components/nucleo_setup/include', '-I', CJSON,
    ...srcs, 'tools/anima-host/nvs_host.c', 'firmware/components/nucleo_setup/setup_store.c', cjsonObj,
    '-o', exe,
  ], { cwd: ROOT, env, encoding: 'utf8' });
  if (r.status !== 0) { console.error(`setup-store: COMPILE FAILED (${name})\n${r.stdout || ''}${r.stderr || ''}`); process.exit(1); }
  if (r.stderr) process.stderr.write(r.stderr);    // surface warnings even on success
  return exe;
}
const resetExe = build('setupstorectest', ['tools/anima-host/setup-store-ctest.c']);
const secretsExe = build('setupstoresecretsctest', ['tools/anima-host/setup-store-secrets-ctest.c', 'firmware/components/nucleo_setup/ap_creds.c']);

// ---- run -----------------------------------------------------------------------------------------------
let cPass = 0, cFail = 0;
function run(tag, exe, cwd) {
  const r = spawnSync(exe, [], { cwd, env, encoding: 'utf8' });
  const out = r.stdout || '';
  const m = out.match(/setup-store: (\d+) passed, (\d+) failed/);
  if (!m || r.status !== 0) process.stdout.write(out + (r.stderr || ''));   // ESP_LOG lines only on failure
  const p = m ? Number(m[1]) : 0, f = m ? Number(m[2]) : 1;
  cPass += p; cFail += f + (m && r.status !== 0 && f === 0 ? 1 : 0);
  return `${p} ${tag}`;
}
rmSync(join(ROOT, 'build', 'setupstore'), { recursive: true, force: true });   // the reset test's fake /cfg + /sd trees
const rReset = run('reset', resetExe, ROOT);
const RUN = join(BUILD, 'run');
mkdirSync(RUN, { recursive: true });
const rSecrets = run('secrets', secretsExe, RUN);
const pass = cPass + sPass, fail = cFail + sFail;
console.log(`setup-store: ${pass} passed, ${fail} failed (${rReset} + ${rSecrets} host C + ${sPass} static drift)`);
process.exit(fail ? 1 : 0);

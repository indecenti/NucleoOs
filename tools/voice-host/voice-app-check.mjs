// The Voice app + word->app bindings (issue #14). Usage: npm run voice:test (also in anima:gate). No device, no network.
//   1. bindings: the REAL bind_load / nucleo_voice_binding_get / nucleo_voice_binding_set, cut out of
//      firmware/components/nucleo_voice/nucleo_voice.c and run against a temp folder: set / get (case-insensitive) /
//      replace / remove / a second word / a corrupt, oversize or non-object file / an empty word / no stray temp file.
//   2. source invariants: the single "voice" app is registered as a Solo app and Voice Lab is gone everywhere; the Solo
//      boot brings the engine up for it; a bound word is resolved BEFORE ANIMA is asked; bound-only run mode ends the
//      dispatch for any other sentence; leaving the app restores normal dispatch.
//   3. languages: every TR5(it,en,es,fr,de) in the app has exactly five string arguments (a missing language falls
//      back to English silently and nobody notices), and the launcher name row has all five.
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const read = (p) => readFileSync(join(repo, p), 'utf8').replace(/\r\n/g, '\n');
const fails = [];
const ok = (cond, msg) => { if (!cond) fails.push(msg); };

const VOICE_C  = read('firmware/components/nucleo_voice/nucleo_voice.c');
const VOICE_H  = read('firmware/components/nucleo_voice/include/nucleo_voice.h');
const APP      = read('firmware/components/nucleo_app/app_voice.cpp');
const NUCLEO   = read('firmware/components/nucleo_app/nucleo_app.cpp');
const MAIN_C   = read('firmware/main/main.c');
const LAUNCHER = read('firmware/components/nucleo_app/launcher_render.cpp');
const CMAKE    = read('firmware/components/nucleo_app/CMakeLists.txt');

// ---- 2. source invariants --------------------------------------------------------------------------------------
ok(/"voice", "Voice", "System",[^]*?exit_app, NX_SOLO \| NX_WIFI,/.test(APP), 'the voice app is not registered as NX_SOLO | NX_WIFI');
ok(!existsSync(join(repo, 'firmware/components/nucleo_app/app_voicelab.cpp')), 'app_voicelab.cpp is back: Voice Lab was merged into the voice app');
for (const [name, text] of [['CMakeLists', CMAKE], ['nucleo_app.cpp', NUCLEO], ['launcher_render.cpp', LAUNCHER], ['main.c', MAIN_C]])
  ok(!/voicelab/i.test(text), `${name} still mentions voicelab`);
ok(/nucleo_app_solo_needs_voice\(void\)\s*\{\s*return s_solo_app == SOLO_APPID && !strcmp\(s_solo_id, "voice"\);/.test(NUCLEO),
  'nucleo_app_solo_needs_voice must be true for the voice app only');
ok(/nucleo_app_solo_needs_speech\(\) \|\| nucleo_app_solo_needs_voice\(\)\) && !setup_pending && nucleo_voice_init\(\)/.test(MAIN_C),
  'main.c does not start the voice engine in the voice app\'s Solo boot');
ok(!/needs_speech\(\)[^;]*nucleo_tts_init/.test(MAIN_C.replace(/needs_voice/g, '')) || /\(!solo \|\| nucleo_app_solo_needs_speech\(\)\) && !setup_pending\) nucleo_tts_init/.test(MAIN_C),
  'the Solo voice boot must not start TTS');
{   // a bound word is resolved before ANIMA is even asked for the lock; bound-only ends the dispatch right after
  const at = VOICE_C.indexOf('static void semantic_dispatch');
  const body = VOICE_C.slice(at, VOICE_C.indexOf('\n}\n', at));
  const iBind = body.indexOf('nucleo_voice_binding_get(sentence'), iOnly = body.indexOf('if (s_bound_only)'), iLock = body.indexOf('nucleo_anima_try_lock()');
  ok(iBind > 0 && iOnly > iBind && iLock > iOnly, 'semantic_dispatch order must be: bound word, bound-only stop, then the ANIMA lock');
  ok(/if \(!s_test_mode\) launched = nucleo_app_launch_id\(nucleo_app_native_id\(bapp\)\)/.test(body), 'a bound word must launch only outside test mode');
}
ok(/void nucleo_voice_set_bound_only\(bool on\)/.test(VOICE_C) && /void\s+nucleo_voice_set_bound_only\(bool on\);/.test(VOICE_H), 'nucleo_voice_set_bound_only is not declared + defined');
{
  const ex = APP.slice(APP.indexOf('static void exit_app(void)'), APP.indexOf('static void tick(void)'));
  ok(/nucleo_voice_set_test_mode\(false\)/.test(ex) && /nucleo_voice_set_bound_only\(false\)/.test(ex), 'leaving the app must restore normal dispatch (test mode + bound-only off)');
  ok(/nucleo_voice_set_test_mode\(!on\)/.test(APP) && /nucleo_voice_set_bound_only\(on\)/.test(APP), 'run mode must turn test mode off and bound-only on');
  ok(/nucleo_voice_set_test_mode\(true\);[^]*?nucleo_voice_set_bound_only\(false\)/.test(APP.slice(APP.indexOf('static void enter(void)'), APP.indexOf('static void exit_app(void)'))),
    'entering the app must start in test mode');
  ok(/if \(!nucleo_anima_solo_active\(\)\)\s*\n\s*nucleo_exclusive_enter/.test(APP), 'in the Solo boot the app must not enter exclusive mode');
  ok(/nucleo_voice_binding_set\(s_t\[s_sel\]\.name, ""\)/.test(APP), 'deleting a word must drop its binding');
}

// ---- 3. languages: TR5 has exactly five string arguments --------------------------------------------------------
function tr5Calls(src) {
  const out = [];
  for (let at = src.indexOf('TR5('); at >= 0; at = src.indexOf('TR5(', at + 4)) {
    let i = at + 4, depth = 1, arg = '', args = [], inStr = false;
    for (; i < src.length && depth > 0; i++) {
      const ch = src[i];
      if (inStr) { arg += ch; if (ch === '\\') { arg += src[++i]; } else if (ch === '"') inStr = false; continue; }
      if (ch === '"') { inStr = true; arg += ch; continue; }
      if (ch === '(') { depth++; arg += ch; continue; }
      if (ch === ')') { depth--; if (depth === 0) break; arg += ch; continue; }
      if (ch === ',' && depth === 1) { args.push(arg.trim()); arg = ''; continue; }
      arg += ch;
    }
    args.push(arg.trim());
    out.push({ line: src.slice(0, at).split('\n').length, args });
  }
  return out;
}
const calls = tr5Calls(APP);
ok(calls.length >= 60, `expected the app's text in TR5 (found ${calls.length} calls)`);
for (const c of calls) {
  if (c.args.length !== 5) { fails.push(`app_voice.cpp:${c.line}: TR5 has ${c.args.length} arguments, needs 5 (it, en, es, fr, de)`); continue; }
  c.args.forEach((a, k) => { if (!/^"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*$/.test(a) && !/^[A-Za-z_][\w:>.\[\]-]*(?:\([^)]*\))?$/.test(a) && !/\?/.test(a))
    fails.push(`app_voice.cpp:${c.line}: TR5 argument ${k + 1} is neither a string nor a plain value: ${a.slice(0, 50)}`); });
}
{ // a bare "TR(" would be it/en only: the app must not use it
  const bare = [...APP.matchAll(/[^A-Za-z_]TR\(/g)];
  ok(bare.length === 0, `app_voice.cpp uses ${bare.length} two-language TR( call(s): use TR5`);
}
{ const row = /\{ "voice",([^}]*)\}/.exec(LAUNCHER);
  ok(row && (row[1].match(/"[^"]+"/g) || []).length === 5, 'the launcher name row for "voice" must have five names (it, en, es, fr, de)'); }

// ---- 1. bindings: the real functions, compiled and run ---------------------------------------------------------------
const from = VOICE_C.indexOf('#define VOICE_BIND_PATH');
const fnAt = VOICE_C.indexOf('esp_err_t nucleo_voice_binding_set');
const to = fnAt < 0 ? -1 : VOICE_C.indexOf('\n}\n', fnAt) + 3;
ok(from >= 0 && to > from, 'the bindings block was not found in nucleo_voice.c');
let ran = 0;
if (from >= 0 && to > from) {
  const MINGW = 'C:/msys64/mingw64/bin';
  const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
  const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
  const cjsonDirs = [process.env.IDF_PATH && join(process.env.IDF_PATH, 'components/json/cJSON'),
    'C:/esp/esp-idf/components/json/cJSON', 'D:/esp/esp-idf/components/json/cJSON'].filter(Boolean);
  const CJSON = cjsonDirs.find((d) => existsSync(join(d, 'cJSON.c')));
  if (!CJSON) { console.error(`[voice-app] cJSON (ESP-IDF) not found in ${cjsonDirs.join(', ')}; set IDF_PATH`); process.exit(1); }
  const work = mkdtempSync(join(tmpdir(), 'voice-bind-'));
  try {
    const dir = join(work, 'voice').replace(/\\/g, '/');
    mkdirSync(dir, { recursive: true });
    const win = process.platform === 'win32';
    const c = [
      '#include <stdio.h>', '#include <stdlib.h>', '#include <string.h>', '#include <strings.h>', '#include <stdbool.h>', '#include <stdint.h>',
      win ? '#include <direct.h>' : '#include <sys/stat.h>',
      win ? '#define mkdir(p, m) _mkdir(p)' : '',
      '#include "cJSON.h"',
      'typedef int esp_err_t;', '#define ESP_OK 0', '#define ESP_FAIL -1', '#define ESP_ERR_NO_MEM 0x101', '#define ESP_ERR_INVALID_ARG 0x102',
      'static size_t strlcpy(char *d, const char *s, size_t n) { size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l; }',
      `#define TPL_PATH ${JSON.stringify(dir)}`,
      VOICE_C.slice(from, to),
      'static int bad = 0;',
      '#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s (line %d)\\n", m, __LINE__); bad++; } } while (0)',
      'static void put(const char *txt) { FILE *f = fopen(VOICE_BIND_PATH, "w"); fputs(txt, f); fclose(f); }',
      'static int has_tmp(void) { FILE *f = fopen(TPL_PATH "/bindings.tmp", "r"); if (f) { fclose(f); return 1; } return 0; }',
      'int main(void) {',
      '  char a[32];',
      '  remove(VOICE_BIND_PATH);',
      '  CHECK(!nucleo_voice_binding_get("anima", a, sizeof a) && !a[0], "get on a missing file is false and clears the output");',
      '  CHECK(nucleo_voice_binding_set("anima", "anima") == ESP_OK, "set a word");',
      '  CHECK(nucleo_voice_binding_get("anima", a, sizeof a) && !strcmp(a, "anima"), "get it back");',
      '  CHECK(nucleo_voice_binding_get("ANIMA", a, sizeof a) && !strcmp(a, "anima"), "get is case-insensitive");',
      '  CHECK(nucleo_voice_binding_set("Anima", "calc") == ESP_OK, "rebind with another case");',
      '  CHECK(nucleo_voice_binding_get("anima", a, sizeof a) && !strcmp(a, "calc"), "the rebind replaced the old app");',
      '  { cJSON *r = bind_load(); int n = 0; for (cJSON *it = r ? r->child : NULL; it; it = it->next) n++; CHECK(n == 1, "rebinding leaves ONE entry, not two"); cJSON_Delete(r); }',
      '  CHECK(nucleo_voice_binding_set("meteo", "weather") == ESP_OK, "a second word");',
      '  CHECK(nucleo_voice_binding_get("meteo", a, sizeof a) && !strcmp(a, "weather") && nucleo_voice_binding_get("anima", a, sizeof a) && !strcmp(a, "calc"), "both words kept");',
      '  CHECK(nucleo_voice_binding_set("anima", "") == ESP_OK, "unbind");',
      '  CHECK(!nucleo_voice_binding_get("anima", a, sizeof a) && nucleo_voice_binding_get("meteo", a, sizeof a), "unbinding one leaves the other");',
      '  CHECK(!has_tmp(), "no stray bindings.tmp after a save");',
      '  CHECK(nucleo_voice_binding_get("meteo", a, 4) && !strcmp(a, "wea"), "a short buffer is cut, never overrun");',
      '  put("{not json");',
      '  CHECK(!nucleo_voice_binding_get("anima", a, sizeof a), "a corrupt file reads as no binding");',
      '  CHECK(nucleo_voice_binding_set("anima", "anima") == ESP_OK && nucleo_voice_binding_get("anima", a, sizeof a), "a save recovers a corrupt file");',
      '  put("[\\"a\\",\\"b\\"]");',
      '  CHECK(!nucleo_voice_binding_get("a", a, sizeof a), "a non-object file reads as no binding");',
      '  { char big[VOICE_BIND_MAX + 64]; memset(big, \' \', sizeof big); big[sizeof big - 1] = 0; memcpy(big, "{\\"anima\\":\\"x\\"}", 13); put(big); }',
      '  CHECK(!nucleo_voice_binding_get("anima", a, sizeof a), "an oversize file is refused");',
      '  put("{\\"anima\\":5,\\"vuota\\":\\"\\"}");',
      '  CHECK(!nucleo_voice_binding_get("anima", a, sizeof a) && !nucleo_voice_binding_get("vuota", a, sizeof a), "a non-string or empty value is not a binding");',
      '  CHECK(nucleo_voice_binding_set("", "x") == ESP_ERR_INVALID_ARG && nucleo_voice_binding_set(NULL, "x") == ESP_ERR_INVALID_ARG, "an empty word is refused");',
      '  CHECK(!nucleo_voice_binding_get("", a, sizeof a) && !nucleo_voice_binding_get(NULL, a, sizeof a) && !nucleo_voice_binding_get("x", NULL, 0), "get with empty arguments is safe");',
      '  if (!bad) printf("ALL OK\\n");',
      '  return bad ? 1 : 0;', '}', ''].join('\n');
    const cfile = join(work, 'bind-test.c'), exe = join(work, 'bind-test' + (win ? '.exe' : ''));
    writeFileSync(cfile, c);
    const cc = spawnSync(GCC, ['-std=gnu11', '-O0', '-Wall', '-Wno-unused-function', '-Wno-unused-variable', '-I', CJSON, cfile, join(CJSON, 'cJSON.c'), '-o', exe],
      { encoding: 'utf8', env });
    if (cc.error || cc.status !== 0) fails.push('the bindings code does not compile on the host:\n' + (cc.stderr || cc.stdout || (cc.error && cc.error.message)));
    else {
      const run = spawnSync(exe, [], { encoding: 'utf8', env });
      for (const l of (run.stdout || '').split('\n')) if (l.startsWith('FAIL')) fails.push('bindings: ' + l.slice(5));
      if (run.status === 0) ran = 1; else if (!fails.some((f) => f.startsWith('bindings:'))) fails.push('bindings test exited ' + run.status + (run.stderr ? ': ' + run.stderr : ''));
    }
  } finally { rmSync(work, { recursive: true, force: true }); }
}

if (fails.length) { console.error('[voice-app] FAIL\n - ' + fails.join('\n - ')); process.exit(1); }
console.log(`[voice-app] OK: bindings ${ran ? '22 checks' : 'not run'}, ${calls.length} TR5 strings x 5 languages, Solo wiring + dispatch order`);

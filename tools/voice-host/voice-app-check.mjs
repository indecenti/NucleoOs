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

// ---- 2b. GO held in the launcher -> Voice app in Solo, listening by itself ------------------------------------------
{
  const gs = NUCLEO.indexOf('if (gpio_get_level(GPIO_NUM_0) == 0 && !s_g0_eat');
  const ge = NUCLEO.indexOf('// Drain pending reminders.', gs);
  ok(gs > 0 && ge > gs, 'the GO handler block was not found in nucleo_app.cpp');
  const go = NUCLEO.slice(gs, ge);
  const rel = go.indexOf('// released');
  ok(rel > 0, 'the GO release branch was not found');
  const hold = go.slice(0, rel), released = go.slice(rel);
  // GPIO0 is the boot-mode strapping pin: the reboot may only start once GO is RELEASED
  ok(!/solo_listen_launch\(\)/.test(hold), 'the reboot into the Voice app must never start while GO is still held (GPIO0 strapping pin)');
  ok(/solo_listen_launch\(\)/.test(released) && /else if \(g0_solo_listen\)/.test(released), 'releasing GO must start the Voice app reboot when a hold could not host the recognizer');
  ok(/nucleo_app_release_buffers\(\);\s*\n\s*if \(!s_solo_active && !nucleo_voice_ptt_possible\(\)\)/.test(hold),
    'the hold must ask nucleo_voice_ptt_possible() AFTER giving the canvas back, and never inside a Solo boot');
  ok(/g0_solo_listen = true;/.test(hold) && /nucleo_voice_ptt\(true\);/.test(hold), 'the hold keeps the in-place voice session when the heap can host it');
  ok(/nucleo_app_set_brightness\(s_brightness\);/.test(hold.slice(hold.indexOf('g0_solo_listen = true;') - 400, hold.indexOf('g0_solo_listen = true;'))), 'the blank must be undone before asking the user to release GO');
  const sl = NUCLEO.slice(NUCLEO.indexOf('static void solo_listen_launch(void)'), NUCLEO.indexOf('static void solo_listen_launch(void)') + 700);
  ok(/nucleo_app_wifi_skip_next_boot\(\);/.test(sl) && /s_solo_listen = SOLO_LISTEN_MAGIC;/.test(sl) && /nucleo_app_solo_request_id\("voice"\);/.test(sl),
    'solo_listen_launch must skip the radio, set the one-shot mark and request the voice app\'s Solo boot');
  ok(/if \(esp_reset_reason\(\) != ESP_RST_SW\) \{ s_solo_req = 0; s_solo_listen = 0; return false; \}/.test(NUCLEO), 'a cold or crash boot must clear the listen mark');
  ok(/nucleo_app_solo_listen_take\(void\)\s*\n\{\s*\n\s*if \(!s_solo_active \|\| s_solo_app != SOLO_APPID \|\| strcmp\(s_solo_id, "voice"\) \|\| s_solo_listen != SOLO_LISTEN_MAGIC\) return false;\s*\n\s*s_solo_listen = 0;/.test(NUCLEO),
    'the listen mark must be honoured only by the voice app in a Solo boot, and only once');
  ok(/bool nucleo_voice_ptt_possible\(void\)/.test(VOICE_C) && /VOICE_PTT_MIN_BLOCK \+ up/.test(VOICE_C) && /VOICE_PTT_MIN_FREE \+ up/.test(VOICE_C) && /if \(!nucleo_voice_ptt_possible\(\)\) \{/.test(VOICE_C),
    'the engine gate and the UI check must be the same function, raised by the task stack while the engine is down');
  const tk = APP.slice(APP.indexOf('static void tick(void)'), APP.indexOf('// ── Input'));
  ok(/AL_WAIT[^]*?nucleo_voice_ptt\(true\)[^]*?AL_LISTEN/.test(tk) && /nucleo_voice_ptt\(false\); s_al = AL_MISS/.test(tk) && /nucleo_voice_ptt\(false\); s_al = AL_OFF/.test(tk),
    'the auto-listen must start a capture, end it after the first word, and give up after the time limit');
  ok(/AL_LIMIT_US\s+9000000/.test(APP), 'the auto-listen needs a hard time limit');
  ok(/if \(nucleo_app_solo_listen_take\(\)\) \{[^]*?set_live\(true\);[^]*?s_al = AL_WAIT/.test(APP), 'entering with the listen mark must switch to run mode and arm the auto-listen');
  ok(/if \(s_al == AL_LISTEN \|\| s_al == AL_TAIL\) nucleo_voice_ptt\(false\);/.test(APP.slice(APP.indexOf('static void exit_app(void)'), APP.indexOf('static void tick(void)'))), 'leaving the app must not leave a capture armed');
}

// ---- 2c. a spoken word that is not the whole sentence, and ANIMA from another app's Solo boot ----------------------
{
  const at = VOICE_C.indexOf('static void semantic_dispatch');
  const body = VOICE_C.slice(at, VOICE_C.indexOf('\n}\n', at));
  ok(/for \(int i = 0; i < ntok && !bword; i\+\+\)\s*\n\s*if \(nucleo_voice_binding_get\(tokens\[i\], bapp, sizeof bapp\)\) bword = tokens\[i\];/.test(body),
    'a bound word must also be found among the recognized words of the sentence ("open anima")');
  const ms = NUCLEO.slice(NUCLEO.indexOf('static bool maybe_solo_launch(int idx)'), NUCLEO.indexOf('static bool maybe_solo_launch(int idx)') + 1400);
  ok(/s_solo_active && s_solo_app == SOLO_APPID && !strcmp\(s_apps\[idx\]\.id, "anima"\)\) \{\s*\n\s*launcher_capture_return\(\);\s*\n\s*nucleo_anima_solo_request\(\);/.test(ms)
     && ms.indexOf('nucleo_anima_solo_request();') < ms.indexOf('if (s_solo_active || idx < 0) return false;'),
    'ANIMA opened from another app\'s Solo boot must reboot into its own Solo profile (it needs a 26 KB task and the radio), before the "already Solo" early return');
}

// ---- 2d. the panel is black while the app listens by itself, and always comes back ------------------------------------
{
  const al = APP.slice(APP.indexOf('static void al_dark(bool on)'), APP.indexOf('static bool listening_now(void)'));
  ok(/nucleo_ui_set_brightness\(0\)/.test(al) && /nucleo_app_set_brightness\(nucleo_app_brightness\(\)\)/.test(al),
    'al_dark must blank with the raw backlight and restore the STORED percent through the app layer (never overwrite the user\'s level)');
  ok(/al_dark\(true\);/.test(APP.slice(APP.indexOf('static void enter(void)'), APP.indexOf('static void exit_app(void)'))), 'entering with the listen mark must blank the panel');
  ok(/al_dark\(false\);\s*\n\s*if \(s_al == AL_LISTEN/.test(APP.slice(APP.indexOf('static void exit_app(void)'), APP.indexOf('static void tick(void)'))),
    'leaving the app must give the backlight back FIRST (an app opened by the word replaces this one)');
  const tk = APP.slice(APP.indexOf('static void tick(void)'), APP.indexOf('// ── Input'));
  ok(/s_al_dark && \(s_al == AL_OFF \|\| s_al == AL_MISS\) && !nucleo_voice_ptt_engaged\(\)\) al_dark\(false\)/.test(tk), 'the panel must come back once the engine has finished (done beep played)');
  ok(/AL_DARK_MAX_US\) al_dark\(false\)/.test(tk) && /AL_DARK_MAX_US\s+20000000/.test(APP), 'the black panel needs a hard time limit');
  ok(/if \(s_al_dark\) al_dark\(false\);/.test(APP.slice(APP.indexOf('static void on_key(int key, char ch)'), APP.indexOf('static void on_key(int key, char ch)') + 500)), 'any key must bring the panel back');
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

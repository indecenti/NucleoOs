// The user's OpenAI-compatible teacher ("custom", issue #10) in the firmware (firmware/components/nucleo_anima/
// nucleo_anima_online.c). Usage: npm run customteacher:test (also in anima:gate). No network, no device.
//   1. model_label(), extracted from the real source and run: a "custom" teacher is shown by its own model name
//      ("deepseek/deepseek-chat" -> "deepseek-chat"), cut to the row, never as "OpenAI" or "Groq"; the other
//      providers keep their names (Claude family + version, Gemini, Grok, Groq, OpenAI).
//   2. source invariants: the provider string is copied as written (so "custom" survives the load), it fits the
//      field, the Whisper path never takes a "custom" top level (chat only is promised), and a custom teacher is
//      called on the OpenAI-compatible wire with its own base (no default host is ever substituted for it).
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, '..', '..');
const SRC = join(repo, 'firmware/components/nucleo_anima/nucleo_anima_online.c');
const src = readFileSync(SRC, 'utf8').replace(/\r\n/g, '\n');
const fails = [];
const ok = (cond, msg) => { if (!cond) fails.push(msg); };

// ---- 2. source invariants --------------------------------------------------------------------------------
const cfgT = /typedef struct \{\n\s*char provider\[(\d+)\];/.exec(src);
ok(cfgT && Number(cfgT[1]) > 'custom'.length, 'teacher_cfg_t.provider cannot hold "custom"');
ok(/if \(cJSON_IsString\(pr\) && pr->valuestring\[0\]\) snprintf\(c->provider, sizeof c->provider, "%s", pr->valuestring\);/.test(src),
  'teacher_obj_to_cfg no longer copies the provider as written');
const audio = /bool audio_capable = ([^;]+);/.exec(src);
ok(audio && /strcmp\(top\.provider, "custom"\)/.test(audio[1]), 'the Whisper path may take a "custom" top level (it promises chat only)');
// the OpenAI-compatible wire: anything that is not "anthropic" posts to <base>/chat/completions with its own base
ok(/snprintf\(url, sizeof url, "%s\/chat\/completions", c->base\)/.test(src), 'the OpenAI-compatible wire no longer uses the entry\'s own base');
ok(!/strcmp\(c->provider, "custom"\)\) snprintf\(c->base/.test(src), 'a default host is substituted for a custom teacher');

// ---- 1. model_label, extracted and run -------------------------------------------------------------------
const moptT = /typedef struct \{ char keyname\[\d+\]; char provider\[\d+\]; char model\[\d+\]; \} mopt_t;/.exec(src);
const fnAt = src.indexOf('static void model_label(const mopt_t *m, char *out, int cap)');
const fnEnd = fnAt < 0 ? -1 : src.indexOf('\n}\n', fnAt);
ok(!!moptT, 'mopt_t not found');
ok(fnAt >= 0 && fnEnd > fnAt, 'model_label() not found');
let ran = 0;
if (moptT && fnAt >= 0 && fnEnd > fnAt) {
  const MINGW = 'C:/msys64/mingw64/bin';
  const GCC = existsSync(join(MINGW, 'gcc.exe')) ? join(MINGW, 'gcc.exe') : 'gcc';
  const env = { ...process.env, PATH: `${MINGW};${process.env.PATH || ''}` };
  const cjsonDirs = [process.env.IDF_PATH && join(process.env.IDF_PATH, 'components/json/cJSON'),
    'C:/esp/esp-idf/components/json/cJSON', 'D:/esp/esp-idf/components/json/cJSON'].filter(Boolean);
  const CJSON = cjsonDirs.find((d) => existsSync(join(d, 'cJSON.c')));
  if (!CJSON) { console.error(`[custom-teacher] cJSON (ESP-IDF) not found in ${cjsonDirs.join(', ')}; set IDF_PATH`); process.exit(1); }
  const CASES = [
    // provider, keyname, model, cap, expected
    ['custom', 'custom', 'deepseek/deepseek-chat', 15, 'deepseek-chat'],
    ['custom', 'custom', 'mock-small', 15, 'mock-small'],
    ['custom', 'custom', 'meta-llama/llama-3.3-70b-instruct:free', 15, 'llama-3.3-70b-'],   // cut to the row, never "Groq"
    ['custom', 'custom', '', 15, 'OpenAI-compat'],
    ['custom', 'custom', 'gpt-4o', 4, 'gpt'],                                                  // the buffer is never overrun
    ['anthropic', 'anthropic', 'claude-sonnet-5-5', 15, 'Sonnet 5.5'],
    ['google', 'google', 'gemini-2.5-flash', 15, 'Gemini'],
    ['xai', 'xai', 'grok-2-latest', 15, 'Grok'],
    ['openai', 'openai', 'llama-3.1-8b-instant', 15, 'Groq'],
    ['openai', 'openai', 'gpt-4o-mini', 15, 'OpenAI'],
  ];
  const c = [
    // the prologue claude-stream-test.c gives anima_claude.h (anth_model_ver lives there)
    '#include <stdio.h>', '#include <stdlib.h>', '#include <string.h>', '#include <stdint.h>', '#include <stdbool.h>', '#include <ctype.h>',
    '#include "cJSON.h"', '#define TAG "t"', '#define ESP_LOGW(t, ...) (printf("W: " __VA_ARGS__), putchar(10))',
    '#define ESP_LOGI(t, ...) (printf("I: " __VA_ARGS__), putchar(10))', 'typedef struct { const char *q, *a; } anima_turn_t;', '#include "anima_claude.h"',
    moptT[0],
    src.slice(fnAt, fnEnd + 2),
    'int main(void) {', '  int bad = 0; char out[64];',
    ...CASES.map(([p, k, m, cap, want]) => `  { mopt_t o; memset(&o, 0, sizeof o); snprintf(o.keyname, sizeof o.keyname, "%s", ${JSON.stringify(k)}); snprintf(o.provider, sizeof o.provider, "%s", ${JSON.stringify(p)}); snprintf(o.model, sizeof o.model, "%s", ${JSON.stringify(m)});
    memset(out, 'X', sizeof out); model_label(&o, out, ${cap});
    if (strlen(out) >= ${cap} || strcmp(out, ${JSON.stringify(want)})) { printf("FAIL %s %s -> '%s' (want '%s')\\n", ${JSON.stringify(p)}, ${JSON.stringify(m)}, out, ${JSON.stringify(want)}); bad++; }
    if (out[${cap}] != 'X') { printf("FAIL %s %s wrote past its %d bytes\\n", ${JSON.stringify(p)}, ${JSON.stringify(m)}, ${cap}); bad++; } }`),
    `  if (!bad) printf("ALL OK %d\\n", ${CASES.length});`, '  return bad ? 1 : 0;', '}', ''].join('\n');
  const build = join(here, 'build'); mkdirSync(build, { recursive: true });
  const cfile = join(build, 'custom-teacher-test.c');
  const exe = join(build, 'custom-teacher' + (process.platform === 'win32' ? '.exe' : ''));
  writeFileSync(cfile, c);
  const cc = spawnSync(GCC, ['-std=gnu11', '-O0', '-Wall', '-Wno-unused-function', '-Werror=format-overflow',
    '-I', join(repo, 'firmware/components/nucleo_anima'), '-I', CJSON, cfile, join(CJSON, 'cJSON.c'), '-o', exe], { encoding: 'utf8', env });
  if (cc.error || cc.status !== 0) fails.push('model_label does not compile on the host:\n' + (cc.stderr || cc.stdout || (cc.error && cc.error.message)));
  else {
    const run = spawnSync(exe, [], { encoding: 'utf8', env });
    for (const l of (run.stdout || '').split('\n')) if (l.startsWith('FAIL')) fails.push('model_label: ' + l.slice(5));
    if (run.status === 0) ran = CASES.length; else if (!fails.length) fails.push('model_label test exited ' + run.status);
  }
}

if (fails.length) { console.log(`[custom-teacher] FAIL ${fails.length}:\n  - ` + fails.join('\n  - ')); process.exit(1); }
console.log(`[custom-teacher] OK: model_label ${ran}/${ran} cases, 5 source invariants`);

// Run the real firmware sampling policy with a minimal JSON spy; no network/device/ESP-IDF.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const source = readFileSync(new URL('../../firmware/components/nucleo_anima/nucleo_anima_online.c', import.meta.url), 'utf8');

test('custom teachers use endpoint defaults; built-in teachers retain caller temperatures', () => {
  const policy = source.match(/static void teacher_add_temperature\([^]*?\n\}/)?.[0];
  assert.ok(policy, 'firmware policy missing');
  const dir = mkdtempSync(join(tmpdir(), 'nucleo-temperature-'));
  try {
    const file = join(dir, 'temperature.c'), exe = join(dir, process.platform === 'win32' ? 'temperature.exe' : 'temperature');
    writeFileSync(file, `
#include <assert.h>
#include <string.h>
typedef struct { char provider[16]; } teacher_cfg_t;
typedef struct { int calls; double value; } cJSON;
static void cJSON_AddNumberToObject(cJSON *o, const char *name, double value) {
    assert(!strcmp(name, "temperature")); o->calls++; o->value = value;
}
${policy}
int main(void) {
    const char *providers[] = {"custom", "openai", "google", "xai"};
    const double temps[] = {0, 0.2, 0.3, 0.4, 0.6, 1};
    for (unsigned p = 0; p < sizeof providers / sizeof *providers; p++) {
        teacher_cfg_t cfg = {0}; strcpy(cfg.provider, providers[p]);
        for (unsigned t = 0; t < sizeof temps / sizeof *temps; t++) {
            cJSON req = {0}; teacher_add_temperature(&req, &cfg, temps[t]);
            assert(req.calls == (p != 0));
            if (p) assert(req.value == temps[t]);
        }
    }
}
`);
    const cc = spawnSync(process.env.CC || 'gcc', ['-std=c11', '-Wall', '-Wextra', '-Werror', file, '-o', exe], { encoding: 'utf8' });
    assert.equal(cc.status, 0, cc.stderr || cc.error?.message);
    const run = spawnSync(exe, [], { encoding: 'utf8' });
    assert.equal(run.status, 0, run.stderr || run.error?.message);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

test('all five OpenAI-compatible firmware request builders use the policy', () => {
  // No caller may bypass the policy with a directly-added temperature.
  assert.equal((source.match(/cJSON_AddNumberToObject\(req, "temperature"/g) || []).length, 1);
  for (const name of ['teacher_one_shot', 'grok_verify', 'nucleo_anima_online_teacher', 'chat_once', 'nucleo_anima_online_longform']) {
    const at = source.indexOf(name + '(');
    // Some public functions have forward declarations: find their definition instead.
    const def = new RegExp('(?:static )?(?:int|char \\*)\\s*' + name + '\\([^;{}]*\\)\\s*\\{').exec(source);
    assert.ok(at >= 0 && def, name + ' definition missing');
    const body = source.slice(def.index, source.indexOf('\n}', def.index) + 2);
    assert.match(body, /teacher_add_temperature\(/, name + ' bypasses policy');
  }
});

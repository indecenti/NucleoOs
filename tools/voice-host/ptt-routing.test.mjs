// Exercise the firmware's actual GO routing and voice-app canvas lifecycle on the host.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';

const root = new URL('../../', import.meta.url);
const read = path => readFileSync(new URL(path, root), 'utf8');
const framework = read('firmware/components/nucleo_app/nucleo_app.cpp');
const begin = framework.indexOf('        if (gpio_get_level(GPIO_NUM_0) == 0 && !s_g0_eat');
const end = framework.indexOf('// Drain pending reminders.', begin);
assert.ok(begin >= 0 && end > begin, 'GO routing block missing');
const routing = framework.slice(begin, end);

for (const app of ['app_voice.cpp', 'app_voicelab.cpp']) {
  test(`${app}: exclusive GO hold captures visibly and canvas returns after recognition`, () => {
    const src = read('firmware/components/nucleo_app/' + app);
    const helpers = src.match(/static bool s_ptt_canvas = false;[^]*?(?=static void enter\(void\))/)?.[0];
    assert.ok(helpers, 'voice PTT lifecycle missing');
    const enter = src.slice(src.indexOf('static void enter(void)'), src.indexOf('static void exit_app(void)'));
    assert.match(enter, /nucleo_app_set_ptt_handler\(voice_ptt\)/);
    const exit = src.slice(src.indexOf('static void exit_app(void)'), src.indexOf('static void tick(void)'));
    assert.match(exit, /nucleo_app_set_ptt_handler\(nullptr\)/);
    assert.match(exit, /nucleo_voice_ptt\(false\)/);
    assert.match(src.slice(src.indexOf('static void tick(void)')), /restore_ptt_canvas\(\)/);
    assert.match(enter, /NX_HTTPD \| NX_ANIMA_L1 \| NX_DISCOVERY/);

    const dir = mkdtempSync(join(tmpdir(), 'nucleo-ptt-'));
    try {
      const file = join(dir, 'ptt.cpp'), exe = join(dir, process.platform === 'win32' ? 'ptt.exe' : 'ptt');
      writeFileSync(file, `
#include <cassert>
#include <cstdint>
#define ESP_LOGW(...) ((void)0)
#define GPIO_NUM_0 0
#define PM_ON 0
#define G0_HOLD_MS 500
static int pin = 1, brightness = 100, torch_count = 0, voice_on = 0, voice_off = 0, restores = 0;
static bool engaged = false, exclusive = true, s_app_direct = false;
static bool s_voice_dark = false, s_wake_pending = false, s_g0_eat = false, s_torch = false;
static int s_pm_state = PM_ON;
static uint32_t now, last_act, s_pm_act, g0_down_ms, g0_dark_since, g0_cooldown_ms;
static bool g0_ptt = false;
static void (*s_app_ptt)(bool) = nullptr, (*s_g0_app_ptt)(bool) = nullptr;
static bool nucleo_exclusive_active() { return exclusive; }
static int gpio_get_level(int) { return pin; }
static void pm_poke() {}
static void torch_off() { s_torch = false; }
static void toggle_torch() { s_torch = !s_torch; torch_count++; }
static void nucleo_app_set_brightness(int b) { brightness = b; }
static void nucleo_ui_set_brightness(int b) { brightness = b; }
static void nucleo_app_release_buffers() {}
static void nucleo_app_request_draw() {}
static void nucleo_app_set_direct_draw(bool on) { s_app_direct = on; if (!on) restores++; }
static bool nucleo_voice_ptt_engaged() { return engaged; }
static void nucleo_voice_ptt(bool on) { if (on) { voice_on++; engaged = true; } else voice_off++; }
${helpers}
static void step(uint32_t time, int level) { now = time; pin = level;
${routing}
}
int main() {
    s_app_ptt = voice_ptt; // enter() registers this before the exclusive guard
    step(1000, 0); step(1500, 0);
    assert(voice_on == 1 && s_app_direct && s_ptt_canvas);
    assert(!s_voice_dark && brightness == 100 && torch_count == 0);
    step(2000, 1);
    assert(voice_off == 1 && !g0_ptt);
    restore_ptt_canvas(); // release != recognition finished: never steal its RAM
    assert(s_app_direct && restores == 0);
    engaged = false; restore_ptt_canvas();
    assert(!s_app_direct && !s_ptt_canvas && restores == 1);
    restore_ptt_canvas(); assert(restores == 1);
    // A second hold still works. A refused capture also returns the canvas.
    step(3000, 0); step(3500, 0); engaged = false;
    restore_ptt_canvas(); assert(restores == 2);
    step(4000, 1);
    // Other exclusive apps retain their protection; no recognizer starts.
    s_app_ptt = nullptr;
    step(5000, 0); step(5500, 0); step(6000, 1);
    assert(voice_on == 2 && !s_voice_dark);
    // Quick taps retain the documented torch behavior.
    step(7000, 0); step(7100, 1); assert(torch_count == 1);
}
`);
      const cc = spawnSync(process.env.CXX || 'g++', ['-std=c++17', '-Wall', '-Wextra', file, '-o', exe], { encoding: 'utf8' });
      assert.equal(cc.status, 0, cc.stderr || cc.error?.message);
      const run = spawnSync(exe, [], { encoding: 'utf8' });
      assert.equal(run.status, 0, run.stderr || run.error?.message);
    } finally { rmSync(dir, { recursive: true, force: true }); }
  });
}

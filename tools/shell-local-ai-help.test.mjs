// web/shell/local-ai-help.js — the "Ollama is running but refuses NucleoOS" wizard. Pins the exact,
// documented fix per OS (Ollama FAQ: Windows user env var + restart from Start, macOS launchctl setenv,
// Linux systemd drop-in) and per server (LM Studio --cors), the device origin carried into every command,
// and that every string it shows exists in all five core catalogs.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { detectOS, originsFix } from '../web/shell/local-ai-help.js';

const here = dirname(fileURLToPath(import.meta.url));
const core = (l) => JSON.parse(readFileSync(join(here, '..', 'web', 'shell', 'i18n', `core.${l}.json`), 'utf8'));
const OLLAMA = { id: 'ollama', kind: 'ollama', name: 'Ollama', base: 'http://localhost:11434' };
const ORIGIN = 'http://192.168.0.166';

test('detectOS reads the real user agents', () => {
  assert.equal(detectOS('Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/140 Safari/537.36'), 'windows');
  assert.equal(detectOS('Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 Safari/605.1.15'), 'mac');
  assert.equal(detectOS('Mozilla/5.0 (X11; Linux x86_64) Gecko/20100101 Firefox/143.0'), 'linux');
  assert.equal(detectOS('Mozilla/5.0 (Linux; Android 15; Pixel 9) Chrome/140 Mobile Safari/537.36'), 'android', 'Android says Linux too: it must win');
  assert.equal(detectOS('Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X)'), 'ios', 'iOS says Mac OS X too: it must win');
});

test('Ollama: the documented per-OS fix, with this page\'s origin in the command', () => {
  const w = originsFix({ os: 'windows', origin: ORIGIN, server: OLLAMA });
  assert.deepEqual(w.steps.map((s) => s.k), ['lai_win_quit', 'lai_win_run', 'lai_win_start'], 'quit → set → start from the Start menu');
  assert.equal(w.steps[1].code, 'setx OLLAMA_ORIGINS "http://192.168.0.166"');
  assert.ok(w.notes.some((n) => n.k === 'cap_tip_ip'), 'an IP origin warns that a new DHCP address needs it again');
  const m = originsFix({ os: 'mac', origin: ORIGIN, server: OLLAMA });
  assert.equal(m.steps[0].code, 'launchctl setenv OLLAMA_ORIGINS "http://192.168.0.166"');
  assert.ok(m.notes.some((n) => n.k === 'lai_mac_reboot'), 'launchctl does not survive a reboot: say so');
  const l = originsFix({ os: 'linux', origin: ORIGIN, server: OLLAMA });
  assert.equal(l.steps[0].code, 'sudo systemctl edit ollama.service');
  assert.equal(l.steps[1].code, '[Service]\nEnvironment="OLLAMA_ORIGINS=http://192.168.0.166"');
  assert.ok(l.steps[1].block, 'multi-line drop-in shown as a block');
  assert.equal(l.steps[2].code, 'sudo systemctl daemon-reload && sudo systemctl restart ollama');
  assert.equal(originsFix({ os: 'linux', origin: 'http://nucleo.local', server: OLLAMA }).notes.some((n) => n.k === 'cap_tip_ip'), false, 'a hostname does not change with DHCP');
  assert.deepEqual(originsFix({ os: 'android', origin: ORIGIN, server: OLLAMA }).steps.map((s) => s.k), ['lai_other']);
});

test('other servers: LM Studio --cors, anything else a generic CORS step', () => {
  const s = originsFix({ os: 'windows', origin: ORIGIN, server: { id: 'lmstudio', kind: 'openai', name: 'LM Studio' } });
  assert.equal(s.steps[0].code, 'lms server start --cors');
  assert.deepEqual(originsFix({ os: 'mac', origin: ORIGIN, server: { id: 'jan', kind: 'openai', name: 'Jan' } }).steps.map((x) => x.k), ['lai_generic']);
});

test('every string the wizard can show exists in all five languages, placeholders intact', () => {
  const keys = new Set(['lai_why', 'cap_verify', 'cap_copy', 'lai_ok', 'lai_still', 'lai_down', 'lai_hint', 'lai_how', 'lai_title']);
  for (const os of ['windows', 'mac', 'linux', 'android', 'other']) for (const server of [OLLAMA, { id: 'lmstudio', kind: 'openai' }, { id: 'jan', kind: 'openai' }]) {
    const f = originsFix({ os, origin: ORIGIN, server });
    for (const x of [...f.steps, ...f.notes]) keys.add(x.k);
  }
  const en = core('en');
  for (const l of ['it', 'en', 'es', 'fr', 'de']) {
    const c = core(l);
    for (const k of keys) {
      assert.ok(typeof c[k] === 'string' && c[k].trim(), `${l}: ${k}`);
      const ph = (s) => (String(s).match(/\{\w+\}/g) || []).sort().join();
      assert.equal(ph(c[k]), ph(en[k]), `${l}: ${k} keeps the placeholders of en`);
    }
  }
});

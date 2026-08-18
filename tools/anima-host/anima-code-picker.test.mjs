// anima-code-picker.test.mjs — the F4 host gate (docs/anima-code.md §10 F4): the engine picker's
// pure core. What is pinned, in the order the user asked for it:
//
//   1. HONEST HARDWARE VERDICTS — a PC without WebGPU gets 'unsupported' with the reason KEY (the
//      UI resolves it in the OS language), and the same for a browser without WebAssembly. No
//      install can start on an unsupported rung, because the row never reaches 'needs-model'.
//   2. FIVE LANGUAGES — every i18n key the picker emits exists in ALL FIVE agent catalogues. This
//      is the CI teeth behind "we support five languages, not just English and Italian".
//   3. NO LYING LABELS — a stored engine choice is honored only while that rung is READY and
//      RUNNABLE; the wasm rung installs for real but is excluded from the runnable ladder until
//      the F3 chat adapter exists, and says so via its noteKey.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { rungRows, pickEngine, localRungOrder, ENGINE_LS, WEBGPU_MODEL, WASM_MODEL } from '../../apps/agent/www/engine-picker.js';

const CACHED = { [WEBGPU_MODEL]: 'cached', [WASM_MODEL]: 'cached' };

// ---- 1. honest hardware verdicts ---------------------------------------------------------------
test('no WebGPU → unsupported with the reason key, and no install path', () => {
  const rows = rungRows({ webgpu: false, wasm: true, online: true }, {}, true);
  const g = rows.find((r) => r.id === 'webgpu');
  assert.equal(g.state, 'unsupported');
  assert.equal(g.reasonKey, 'eng_no_webgpu');
  assert.notEqual(g.state, 'needs-model', 'an unsupported rung must never offer the download');
});

test('no WebAssembly → the CPU rung is unsupported with its own reason', () => {
  const rows = rungRows({ webgpu: true, wasm: false, online: true }, {}, true);
  const w = rows.find((r) => r.id === 'wasm');
  assert.equal(w.state, 'unsupported');
  assert.equal(w.reasonKey, 'eng_no_wasm');
});

test('supported but uncached → needs-model with the size; cached → ready', () => {
  const caps = { webgpu: true, wasm: true, online: true };
  const before = rungRows(caps, {}, true).find((r) => r.id === 'webgpu');
  assert.equal(before.state, 'needs-model');
  assert.match(before.sizeText, /MB/);
  const after = rungRows(caps, CACHED, true).find((r) => r.id === 'webgpu');
  assert.equal(after.state, 'ready');
});

test('cloud row tells the truth about key and network', () => {
  assert.equal(rungRows({ online: true }, {}, false)[0].reasonKey, 'eng_cloud_nokey');
  assert.equal(rungRows({ online: false }, {}, true)[0].reasonKey, 'eng_cloud_offline');
  assert.equal(rungRows({ online: true }, {}, true)[0].state, 'ready');
});

// ---- 2. five languages, enforced ---------------------------------------------------------------
test('every picker i18n key exists in ALL FIVE agent catalogues', () => {
  const src = readFileSync(new URL('../../apps/agent/www/engine-picker.js', import.meta.url), 'utf8')
    + readFileSync(new URL('../../apps/agent/www/agent.js', import.meta.url), 'utf8');
  const used = new Set([...src.matchAll(/\b(?:t\(|reasonKey: '|noteKey: '|STATE_KEY = \{)/g)].length ? [] : []);
  // collect: every quoted eng_* key + the gate/local keys the wiring uses
  for (const m of src.matchAll(/'(eng_[a-z_]+|ready_local|gate_local)'/g)) used.add(m[1]);
  assert.ok(used.size >= 18, 'expected the picker key set, got ' + used.size);
  for (const lg of ['it', 'en', 'fr', 'es', 'de']) {
    const cat = JSON.parse(readFileSync(new URL(`../../apps/agent/www/i18n.${lg}.json`, import.meta.url), 'utf8'));
    for (const k of used) assert.ok(typeof cat[k] === 'string' && cat[k].length, `${lg} missing ${k}`);
  }
});

test('the hardware warnings are real prose in every language, not placeholders', () => {
  for (const lg of ['it', 'en', 'fr', 'es', 'de']) {
    const cat = JSON.parse(readFileSync(new URL(`../../apps/agent/www/i18n.${lg}.json`, import.meta.url), 'utf8'));
    assert.ok(cat.eng_no_webgpu.length > 40, lg + ' WebGPU warning too thin to help anyone');
    assert.match(cat.eng_no_webgpu, /WebGPU/);
    assert.ok(cat.eng_no_wasm.length > 30, lg + ' WASM warning too thin');
  }
});

// ---- 3. no lying labels ------------------------------------------------------------------------
test('a stored choice is honored only while its rung is ready and runnable', () => {
  const caps = { webgpu: true, wasm: true, online: true };
  assert.equal(pickEngine('webgpu', rungRows(caps, CACHED, true)), 'webgpu');
  assert.equal(pickEngine('webgpu', rungRows(caps, {}, true)), 'auto', 'model gone → back to auto');
  assert.equal(pickEngine('webgpu', rungRows({ webgpu: false, wasm: true }, CACHED, true)), 'auto', 'GPU gone → back to auto');
  assert.equal(pickEngine('wasm', rungRows(caps, CACHED, true)), 'auto', 'wasm is not runnable until F3 — an explicit pick must not stick');
});

test('the runnable ladder: webgpu only (wasm excluded until its adapter exists, and it says why)', () => {
  const rows = rungRows({ webgpu: true, wasm: true, online: true }, CACHED, true);
  assert.deepEqual(localRungOrder(rows).map((r) => r.id), ['webgpu']);
  const w = rows.find((r) => r.id === 'wasm');
  assert.equal(w.runnable, false);
  assert.equal(w.noteKey, 'eng_wasm_soon');
  assert.equal(w.state, 'ready', 'the INSTALL is genuinely done — only the run wiring is pending');
});

test('an explicit choice narrows the ladder to that rung alone', () => {
  const rows = rungRows({ webgpu: true, wasm: true, online: true }, CACHED, true);
  assert.deepEqual(localRungOrder(rows, 'webgpu').map((r) => r.id), ['webgpu']);
  assert.deepEqual(localRungOrder(rows, 'wasm').map((r) => r.id), [], 'a non-runnable pick yields an empty ladder, never a fake engine');
});

test('the persisted-choice key is stable', () => { assert.equal(ENGINE_LS, 'agent.engine'); });

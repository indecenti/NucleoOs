// Unit tests for one-tap scenes/profiles (apps/settings/www/scenes.js).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { SCENES, SCENE_ENDPOINTS, applyScene } from '../apps/settings/www/scenes.js';
import { DEFAULTS, merge, clone } from '../apps/settings/www/settings-merge.js';

// Collect the leaf dotted-paths a patch touches, to assert it only references real settings keys.
function leaves(obj, prefix = '') {
  const out = [];
  for (const k of Object.keys(obj)) {
    const p = prefix ? prefix + '.' + k : k;
    if (obj[k] && typeof obj[k] === 'object' && !Array.isArray(obj[k])) out.push(...leaves(obj[k], p));
    else out.push(p);
  }
  return out;
}
function hasPath(obj, dotted) {
  return dotted.split('.').reduce((o, k) => (o && typeof o === 'object' && k in o) ? o[k] : undefined, obj) !== undefined;
}

test('scene ids are unique', () => {
  const ids = SCENES.map((s) => s.id);
  assert.equal(new Set(ids).size, ids.length);
});

test('every scene patch references only keys that exist in DEFAULTS', () => {
  for (const s of SCENES) {
    for (const path of leaves(s.patch || {})) {
      assert.ok(hasPath(DEFAULTS, path), `scene "${s.id}" patches unknown settings key: ${path}`);
    }
  }
});

test('every scene apiCall targets an endpoint that exists', () => {
  for (const s of SCENES) {
    for (const c of (s.apiCalls || [])) {
      assert.ok(SCENE_ENDPOINTS.includes(c.path), `scene "${s.id}" calls unknown endpoint: ${c.path}`);
      assert.ok(c.method === 'POST', `scene "${s.id}" must POST, got ${c.method}`);
    }
  }
});

test('applyScene("battery") patches brightness 30 and swarm off', () => {
  const res = applyScene(clone(DEFAULTS), 'battery');
  assert.equal(res.model.power.display_brightness, 30);
  assert.equal(res.model.network.swarm.enabled, false);
  assert.equal(res.model.voice.alwaysOn, false);
});

test('applyScene returns the ordered live calls (battery frees RAM via voice + L1 off)', () => {
  const res = applyScene(clone(DEFAULTS), 'battery');
  const paths = res.apiCalls.map((c) => c.path);
  assert.deepEqual(paths, ['/api/voice/always', '/api/anima/l1']);
  assert.equal(res.apiCalls[0].body.on, false);
  assert.equal(res.apiCalls[1].body.mode, 'off');
});

test('the diagnostics scene runs the self-test, not a patch', () => {
  const res = applyScene(clone(DEFAULTS), 'diagnostics');
  assert.equal(res.selfTest, true);
  assert.equal(res.apiCalls.length, 0);
});

test('applyScene is pure (does not mutate the input model)', () => {
  const input = clone(DEFAULTS);
  const snapshot = clone(input);
  applyScene(input, 'focus');
  assert.deepEqual(input, snapshot);
});

test('applyScene returns null for an unknown scene', () => {
  assert.equal(applyScene(clone(DEFAULTS), 'nope'), null);
});

test('focus scene = quiet + low-RAM (screen off, voice off, L1 off)', () => {
  const res = applyScene(clone(DEFAULTS), 'focus');
  assert.equal(res.model.power.display_brightness, 40);
  const order = res.apiCalls.map((c) => c.path);
  assert.deepEqual(order, ['/api/voice/always', '/api/anima/l1', '/api/display']);
  assert.equal(res.apiCalls[2].query.on, 0);   // screen off
});

// Unit tests for the command-palette action registry + fuzzy filter (apps/settings/www/actions.js).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildActionRegistry, filterActions } from '../apps/settings/www/actions.js';

function mkCtx() {
  const calls = { nav: [], api: [], reboot: 0, selfTest: 0, applyScene: [], scanWifi: 0 };
  const ctx = {
    nav: (t) => calls.nav.push(t),
    api: (p, o) => { calls.api.push({ p, o }); return Promise.resolve(); },
    reboot: () => calls.reboot++,
    selfTest: () => calls.selfTest++,
    applyScene: (id) => calls.applyScene.push(id),
    scanWifi: () => calls.scanWifi++,
    toast: () => {},
  };
  return { ctx, calls };
}

test('every action has a unique id and non-empty bilingual titles', () => {
  const list = buildActionRegistry(mkCtx().ctx);
  const ids = new Set();
  for (const a of list) {
    assert.ok(a.id && !ids.has(a.id), 'unique id: ' + a.id);
    ids.add(a.id);
    assert.ok(a.it && a.it.length, 'IT title: ' + a.id);
    assert.ok(a.en && a.en.length, 'EN title: ' + a.id);
    assert.equal(typeof a.run, 'function');
  }
  assert.ok(list.length >= 15);
});

test('empty query returns the whole registry', () => {
  const list = buildActionRegistry(mkCtx().ctx);
  assert.equal(filterActions(list, '').length, list.length);
  assert.equal(filterActions(list, '   ').length, list.length);
});

test('keyword search finds the right actions (lingua → appearance, wifi → network)', () => {
  const list = buildActionRegistry(mkCtx().ctx);
  const lingua = filterActions(list, 'lingua', 'it').map((a) => a.id);
  assert.ok(lingua.includes('go-personalization'));
  const wifi = filterActions(list, 'wifi', 'it').map((a) => a.id);
  assert.ok(wifi.includes('go-network'));
  assert.ok(wifi.includes('scan-wifi'));
});

test('title-prefix ranks above keyword-only match', () => {
  const list = buildActionRegistry(mkCtx().ctx);
  const res = filterActions(list, 'riavvia', 'it');
  assert.equal(res[0].id, 'reboot', 'the action titled "Riavvia…" wins');
});

test('bilingual: IT and EN keywords both hit the same action', () => {
  const list = buildActionRegistry(mkCtx().ctx);
  assert.ok(filterActions(list, 'spegni', 'it').some((a) => a.id === 'screen-off'));
  assert.ok(filterActions(list, 'off', 'en').some((a) => a.id === 'screen-off'));
});

test('accent-insensitive matching (luminosità ↔ luminosita)', () => {
  const list = buildActionRegistry(mkCtx().ctx);
  assert.ok(filterActions(list, 'luminosità', 'it').some((a) => a.id === 'go-display'));
  assert.ok(filterActions(list, 'luminosita', 'it').some((a) => a.id === 'go-display'));
});

test('running a nav action calls ctx.nav with the right tab', () => {
  const { ctx, calls } = mkCtx();
  const list = buildActionRegistry(ctx);
  list.find((a) => a.id === 'go-network').run();
  assert.deepEqual(calls.nav, ['network']);
});

test('running the screen-off action drives /api/display on=0', () => {
  const { ctx, calls } = mkCtx();
  const list = buildActionRegistry(ctx);
  list.find((a) => a.id === 'screen-off').run();
  assert.equal(calls.api[0].p, '/api/display');
  assert.equal(calls.api[0].o.query.on, 0);
});

test('scene actions are generated from SCENES and call applyScene', () => {
  const { ctx, calls } = mkCtx();
  const list = buildActionRegistry(ctx);
  const focus = list.find((a) => a.id === 'scene-focus');
  assert.ok(focus, 'scene-focus exists');
  focus.run();
  assert.deepEqual(calls.applyScene, ['focus']);
});

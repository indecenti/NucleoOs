// Unit tests for the Control Center's clobber-safe settings merge (apps/settings/www/settings-merge.js).
// Regression guard for the old inline merge that replaced whole sub-trees and clobbered untouched keys.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { DEFAULTS, merge, clone, changedPaths } from '../apps/settings/www/settings-merge.js';

test('partial override keeps untouched DEFAULTS keys', () => {
  const out = merge(DEFAULTS, { device: { name: 'pico' } });
  assert.equal(out.device.name, 'pico');
  assert.equal(out.device.locale, DEFAULTS.device.locale);     // untouched sibling preserved
  assert.equal(out.ui.theme, DEFAULTS.ui.theme);               // untouched sub-tree preserved
});

test('nested objects merge, not replace', () => {
  const out = merge(DEFAULTS, { ui: { theme: 'light' } });
  assert.equal(out.ui.theme, 'light');
  assert.equal(out.ui.accent, DEFAULTS.ui.accent);             // the rest of ui survives
  assert.equal(out.ui.fontSize, DEFAULTS.ui.fontSize);
});

test('missing voice / ipv6 sub-trees are back-filled from DEFAULTS', () => {
  const fromDisk = { device: { name: 'x' } };                 // a settings.json that predates voice/ipv6
  const out = merge(DEFAULTS, fromDisk);
  assert.equal(out.voice.alwaysOn, false);
  assert.equal(out.network.ipv6.enabled, false);
  assert.equal(out.power.volume, 70);
});

test('arrays are replaced wholesale (not element-merged)', () => {
  const out = merge({ list: [1, 2, 3] }, { list: [9] });
  assert.deepEqual(out.list, [9]);
});

test('merge mutates neither argument', () => {
  const base = clone(DEFAULTS);
  const over = { ui: { theme: 'light' }, network: { wifi: { enabled: false } } };
  const overCopy = clone(over);
  const baseCopy = clone(base);
  merge(base, over);
  assert.deepEqual(base, baseCopy, 'base untouched');
  assert.deepEqual(over, overCopy, 'over untouched');
});

test('merge is deep-stable on a second pass (idempotent overlay)', () => {
  const once = merge(DEFAULTS, { ui: { accent: '#5ee0a0' } });
  const twice = merge(DEFAULTS, once);
  assert.deepEqual(twice, once);
});

test('preserves unknown keys present on disk (drift-aware write)', () => {
  const onDisk = merge(DEFAULTS, { _customSetting: 42, ui: { theme: 'light' } });
  const out = merge(onDisk, { ui: { theme: 'dark' } });        // model overlays disk
  assert.equal(out._customSetting, 42, 'unknown key not lost');
  assert.equal(out.ui.theme, 'dark');
});

test('changedPaths reports only what differs from DEFAULTS', () => {
  const m = merge(DEFAULTS, { ui: { theme: 'light' }, power: { display_brightness: 30 } });
  const cp = changedPaths(m);
  assert.ok(cp.includes('ui.theme'));
  assert.ok(cp.includes('power.display_brightness'));
  assert.ok(!cp.includes('ui.accent'));
});

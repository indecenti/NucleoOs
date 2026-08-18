// The copilot's inline i18n catalog (web/shell/copilot.js STR) — parity gate.
//
// The OS-wide i18n gate (tools/i18n-check.mjs) walks the JSON catalogs and cannot see this inline
// table, so nothing else notices when a key lands in Italian and English but not in the other
// three languages: the UI silently shows the raw key (or undefined) to a German user. This test
// is that missing gate. It also doubles as a headless-import smoke test: copilot.js must keep
// building its DOM lazily in initCopilot(), never at module top level.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { STR } from '../web/shell/copilot.js';   // the import ITSELF asserts: no DOM at load time

const LANGS = ['it', 'en', 'es', 'fr', 'de'];

test('all five OS languages are present', () => {
  assert.deepEqual(Object.keys(STR).sort(), [...LANGS].sort());
});

test('every language carries the SAME key set — no half-translated features', () => {
  const base = Object.keys(STR.en).sort();
  for (const l of LANGS) {
    assert.deepEqual(Object.keys(STR[l]).sort(), base, `key set of '${l}' must match 'en'`);
  }
});

test('no key is empty in any language', () => {
  for (const l of LANGS) for (const [k, v] of Object.entries(STR[l])) {
    if (Array.isArray(v)) { assert.ok(v.length > 0 && v.every((x) => String(x).trim()), `${l}.${k} has an empty entry`); }
    else assert.ok(String(v).trim(), `${l}.${k} is empty`);
  }
});

test('the voice strings exist everywhere — the mic button speaks every OS language', () => {
  for (const l of LANGS) for (const k of ['mic', 'micListening', 'micBusy', 'micErr']) {
    assert.ok(STR[l][k], `missing ${l}.${k}`);
  }
});

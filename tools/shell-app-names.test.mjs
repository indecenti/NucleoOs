// Host tests for the web shell's localised app names (`app_<id>` keys in web/shell/i18n/shell.<lang>.json).
//
// /api/apps carries one manifest `name` per app — a historical mix of Italian and English — and the
// shell used to show it verbatim, so an English or German desktop titled windows "Giochi" and
// "Contatti". shell.js appName() now resolves `app_<id>` from the shell catalog, falling back to the
// manifest name for apps without a key. The i18n gate enforces en/it parity but lets es/fr/de be
// partial (English fallback) — fine for chrome, wrong for a NAME: a German desktop must never show an
// English app name because a key was forgotten. So app names get a stricter contract here.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const LANGS = ['it', 'en', 'es', 'fr', 'de'];
const readJSON = (p) => JSON.parse(readFileSync(p, 'utf8').replace(/^﻿/, ''));
const cat = Object.fromEntries(LANGS.map((l) => [l, readJSON(join(ROOT, 'web', 'shell', 'i18n', `shell.${l}.json`))]));
const appKeys = (o) => Object.keys(o).filter((k) => k.startsWith('app_'));

test('every app_<id> key names a real app', () => {
  const keys = appKeys(cat.en);
  assert.ok(keys.length > 20, 'the shell catalog localises the app names');
  for (const k of keys) {
    const id = k.slice(4);
    assert.ok(existsSync(join(ROOT, 'apps', id, 'manifest.json')), `${k}: no apps/${id}/manifest.json — renamed or removed app?`);
  }
});

test('app names are complete in ALL five languages (no English fallback for a name)', () => {
  const base = appKeys(cat.en).sort();
  for (const l of LANGS) {
    assert.deepEqual(appKeys(cat[l]).sort(), base, `shell.${l}.json must carry exactly the same app_<id> keys as en`);
    for (const k of base) assert.ok(typeof cat[l][k] === 'string' && cat[l][k].trim(), `shell.${l}.json: ${k} is empty`);
  }
});

test('an app without a key keeps its manifest name (proper nouns, agent-installed apps)', () => {
  // Mirrors shell.js appName(): the engine returns the key itself for a missing key.
  const tr = (dict) => (k) => (k in dict ? dict[k] : k);
  const appName = (a, t) => { const k = 'app_' + a.id; const v = t(k); return v && v !== k ? v : (a.name || a.id); };
  const de = tr(cat.de);
  assert.equal(appName({ id: 'games', name: 'Giochi' }, de), 'Spiele');
  assert.equal(appName({ id: 'contacts', name: 'Contatti' }, de), 'Kontakte');
  assert.equal(appName({ id: 'anima', name: 'ANIMA' }, de), 'ANIMA');
  assert.equal(appName({ id: 'my-agent-app', name: 'Mia App' }, de), 'Mia App');
  assert.equal(appName({ id: 'nameless' }, de), 'nameless');
});

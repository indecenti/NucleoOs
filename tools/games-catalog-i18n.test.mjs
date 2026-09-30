// Host tests for the Game Center's localised game cards (apps/games/www/i18n.<lang>.json).
//
// The hub reads names + descriptions from registry/games.json — one Italian text per game — so every
// other language showed Italian cards ("Filetto a turni…" on a German desktop). The catalog now carries
// `game_<id>_name` / `game_<id>_desc`; the hub falls back to the registry text for a game without keys.
// The i18n gate lets es/fr/de be partial, which is wrong for a card: a missing key would show English.
// Italian stays the registry text, so a registry edit must be mirrored here — this test says so.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const LANGS = ['it', 'en', 'es', 'fr', 'de'];
const readJSON = (p) => JSON.parse(readFileSync(p, 'utf8').replace(/^﻿/, ''));
const GAMES = readJSON(join(ROOT, 'registry', 'games.json')).games;
const cat = Object.fromEntries(LANGS.map((l) => [l, readJSON(join(ROOT, 'apps', 'games', 'www', `i18n.${l}.json`))]));

test('every registry game has a name and a description in all five languages', () => {
  assert.ok(GAMES.length > 0);
  for (const g of GAMES) for (const l of LANGS) for (const f of ['name', 'desc']) {
    const v = cat[l][`game_${g.id}_${f}`];
    assert.ok(typeof v === 'string' && v.trim(), `i18n.${l}.json: game_${g.id}_${f} missing or empty`);
  }
});

test('Italian cards are the registry text verbatim (edit registry/games.json → mirror it here)', () => {
  for (const g of GAMES) {
    assert.equal(cat.it[`game_${g.id}_name`], g.name, `game_${g.id}_name drifted from registry/games.json`);
    assert.equal(cat.it[`game_${g.id}_desc`], g.desc, `game_${g.id}_desc drifted from registry/games.json`);
  }
});

test('no orphan game keys (every game_<id>_* names a registry game)', () => {
  const ids = new Set(GAMES.map((g) => g.id));
  for (const l of LANGS) for (const k of Object.keys(cat[l])) {
    const m = k.match(/^game_(.+)_(name|desc)$/);
    if (m) assert.ok(ids.has(m[1]), `i18n.${l}.json: ${k} has no game "${m[1]}" in registry/games.json`);
  }
});

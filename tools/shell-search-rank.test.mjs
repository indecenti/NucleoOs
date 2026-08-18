// Host tests for the OS search ranking (web/shell/search-rank.js) — pure, no DOM, no device.
//
// What this locks down: search used to be `name.includes(q)` with no ranking, no app-ID match, and
// no coverage of the settings/system commands the Start box has always advertised ("apps, files and
// settings"). Ranking is exactly the kind of logic that silently rots, so it gets a gate.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { normMatch, matchScore, kwScore, rankApps, rankActions, looksLikeNL, clipAnswer } from '../web/shell/search-rank.js';

const APPS = [
  { id: 'calculator', name: 'Calculator' },
  { id: 'calendar', name: 'Calendar' },
  { id: 'file-commander', name: 'File Commander' },
  { id: 'log-viewer', name: 'Log Viewer' },
  { id: 'media-player', name: 'Media Player' },
];
const names = (rows) => rows.map((r) => r.name);

test('normMatch folds case and treats -, _ and . as spaces', () => {
  assert.equal(normMatch('File-Commander'), 'file commander');
  assert.equal(normMatch('log_viewer.js'), 'log viewer js');
  assert.equal(normMatch(null), '');
});

test('matchScore ranks exact > prefix > word-start > anywhere', () => {
  assert.equal(matchScore('Clock', 'clock'), 100);
  assert.ok(matchScore('Calculator', 'calc') > matchScore('File Commander', 'command'));
  assert.ok(matchScore('File Commander', 'commander') >= 60, 'a later word start is a strong match');
  assert.ok(matchScore('Unit Converter', 'nit') > 0, 'a mid-word hit still matches, just weakly');
  assert.equal(matchScore('Calculator', 'zzz'), -1);
});

test('an earlier mid-word hit outranks a later one', () => {
  assert.ok(matchScore('abXYZ', 'xyz') > matchScore('abcdefghijXYZ', 'xyz'));
});

test('rankApps puts the prefix match first, not an incidental substring', () => {
  const r = names(rankApps(APPS, 'cal'));
  assert.deepEqual(r, ['Calculator', 'Calendar'], 'both match by prefix, tie broken alphabetically');
  assert.equal(names(rankApps(APPS, 'player'))[0], 'Media Player');
});

test('rankApps matches the app ID too, but the display name wins a tie', () => {
  assert.equal(names(rankApps(APPS, 'file-commander'))[0], 'File Commander');
  assert.equal(names(rankApps(APPS, 'log-viewer'))[0], 'Log Viewer');
  assert.deepEqual(rankApps(APPS, ''), [], 'an empty query matches nothing');
});

// The system-actions provider: labels are shown, `kw` is a search-only synonym list.
const ACTIONS = [
  { id: 'settings', name: 'Apri Impostazioni', kw: 'impostazioni preferenze opzioni configura sistema' },
  { id: 'theme', name: 'Cambia tema chiaro / scuro', kw: 'tema scuro chiaro aspetto colori notte' },
  { id: 'lock', name: 'Blocca la console', kw: 'blocca schermo privacy assente' },
];

test('rankActions finds a command by its label', () => {
  assert.equal(names(rankActions(ACTIONS, 'blocca'))[0], 'Blocca la console');
  assert.equal(names(rankActions(ACTIONS, 'tema'))[0], 'Cambia tema chiaro / scuro');
});

test('rankActions finds a command by a synonym it does not display', () => {
  assert.equal(names(rankActions(ACTIONS, 'opzioni'))[0], 'Apri Impostazioni');
  assert.equal(names(rankActions(ACTIONS, 'privacy'))[0], 'Blocca la console');
});

test('keywords only match on a word boundary — "tema" must not hit "sistema"', () => {
  assert.equal(kwScore('impostazioni preferenze opzioni configura sistema', 'tema'), -1);
  assert.deepEqual(names(rankActions(ACTIONS, 'tema')), ['Cambia tema chiaro / scuro'],
    'the settings row must not ride in on the "sis-tema" substring');
});

test('rankActions needs 2+ characters and stays capped', () => {
  assert.deepEqual(rankActions(ACTIONS, 'a'), [], 'one letter must not flood the list with OS commands');
  const many = Array.from({ length: 20 }, (_, i) => ({ id: 'x' + i, name: 'Test action ' + i, kw: 'test' }));
  assert.ok(rankActions(many, 'test').length <= 6);
});

test('looksLikeNL covers all five OS languages', () => {
  for (const q of ['che ore sono', 'apri la calcolatrice',           // it
    'what time is it', 'open the calculator',                        // en
    'abre la calculadora', 'cómo estás',                             // es
    'ouvre la calculatrice', 'comment ça va',                        // fr
    'öffne den rechner', 'wie spät ist es']) {                       // de
    assert.ok(looksLikeNL(q), 'should read as natural language: ' + q);
  }
});

test('looksLikeNL leaves short name lookups alone', () => {
  for (const q of ['calc', 'notepad', 'wifi.json', 'IMG_0021']) {
    assert.ok(!looksLikeNL(q), 'should read as a name lookup: ' + q);
  }
  assert.ok(looksLikeNL('foo?'), 'a question mark is always a question');
});

test('clipAnswer fits a search row without butchering words', () => {
  assert.equal(clipAnswer('short answer'), 'short answer', 'under the cap → untouched');
  assert.equal(clipAnswer('  spaced\n\nout   text '), 'spaced out text', 'whitespace collapses');
  const long = 'Mercurio è il pianeta più interno del sistema solare e anche il più piccolo, ' +
               'con un diametro di circa quattromilaottocento chilometri e nessuna atmosfera stabile.';
  const c = clipAnswer(long, 80);
  assert.ok(c.length <= 81, 'must respect the cap (+ellipsis): ' + c.length);
  assert.ok(c.endsWith('…'), 'a clipped answer announces itself: ' + c);
  assert.ok(!/[,;:.]…$/.test(c), 'no dangling punctuation before the ellipsis: ' + c);
  const inner = c.slice(0, -1).trimEnd();
  assert.ok(long.startsWith(inner), 'the clip is a prefix, words intact: ' + inner);
  assert.equal(clipAnswer('', 80), '', 'empty in, empty out');
  assert.equal(clipAnswer(null, 80), '');
});

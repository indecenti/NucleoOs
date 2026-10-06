// Every string a web app builds in JS through t('key') must exist in all 5 languages (IT/EN/ES/FR/DE),
// in the app's own catalog or the shared core one. tools/i18n-check.mjs only sees declarative
// data-i18n attributes, so a key used only in code (Browser's errDlBusy) could be missing everywhere and
// silently show the hard-coded fallback. Also: error messages the user sees must not be hard-coded English
// (Archive Manager threw "Failed to read file" etc. straight into the UI).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const LANGS = ['it', 'en', 'es', 'fr', 'de'];
const json = (p) => { let s = readFileSync(p, 'utf8'); if (s.charCodeAt(0) === 0xfeff) s = s.slice(1); return JSON.parse(s); };
const core = Object.fromEntries(LANGS.map((l) => [l, json(join(REPO, 'web', 'shell', 'i18n', `core.${l}.json`))]));

// t('key' …) call sites in the given app files (the call shape these apps use).
// A whole literal key only (t('cat_' + id) builds a dynamic key: not checkable here).
function usedKeys(app, files, re = /\bt\(\s*'([A-Za-z0-9_.]+)'\s*[,)]/g) {
  const keys = new Set();
  for (const f of files) {
    const p = join(REPO, 'apps', app, 'www', f);
    if (existsSync(p)) for (const m of readFileSync(p, 'utf8').matchAll(re)) keys.add(m[1]);
  }
  return keys;
}
function missing(app, keys) {
  const out = [];
  for (const l of LANGS) {
    const c = json(join(REPO, 'apps', app, 'www', `i18n.${l}.json`));
    const has = (o, k) => k in o || Object.keys(o).some((x) => x.startsWith(k + '.'));   // miei-fatti: x.chip.ok → "chip.ok"
    for (const k of keys) if (!has(c, k) && !has(core[l], k)) out.push(`${l}:${k}`);
  }
  return out;
}

const APPS = {
  browser: ['index.html'],
  'archive-manager': ['index.html'],
  ssh: ['app.js'],
  radio: ['index.html'],
  'ir-remote': ['index.html'],
  mail: ['index.html'],
  dj: ['index.html'],
  calendar: ['index.html'],
};
for (const [app, files] of Object.entries(APPS)) {
  test(`${app}: every t() key exists in all 5 languages`, () => {
    const keys = usedKeys(app, files);
    assert.ok(keys.size > 0, 'found the t() call sites');
    assert.deepEqual(missing(app, keys), []);
  });
}

test('miei-fatti: every t().key exists in all 5 languages', () => {
  const keys = usedKeys('miei-fatti', ['index.html'], /\bt\(\)\.([A-Za-z0-9_]+)/g);
  assert.ok(keys.size > 0);
  assert.deepEqual(missing('miei-fatti', keys), []);
});

test('archive-manager: no hard-coded English error text reaches the UI', () => {
  const src = readFileSync(join(REPO, 'apps', 'archive-manager', 'www', 'index.html'), 'utf8');
  const literal = [...src.matchAll(/new Error\(\s*(["'`])([^"'`]*)\1/g)].map((m) => m[2]);
  assert.deepEqual(literal, [], 'Error(...) built from a literal string');
  assert.doesNotMatch(src, /new Error\(\s*["'`][A-Z]/, 'Error(...) starting with an English literal');
});

test('settings: the teacher.json read-failure messages exist in all 5 languages', async () => {
  const { STR } = await import('../apps/settings/www/i18n.js');
  for (const l of LANGS) for (const k of ['teacherReadFailed', 'teacherReadCorrupt'])
    assert.ok(typeof STR[l][k] === 'string' && STR[l][k].length > 10, `${l}.${k}`);
});

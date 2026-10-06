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

// Settings ▸ AI profile panel: once rendered it-or-English only (`EN() ? 'english' : 'italiano'`),
// so ES/FR/DE owners read English there. Every string must now come from the 5-language STR table
// (index.html) or a 5-language label object (preset-engine.js).
const SETTINGS = join(REPO, 'apps', 'settings', 'www');
test('settings: no it/en-only EN() ternary is left in index.html', () => {
  const src = readFileSync(join(SETTINGS, 'index.html'), 'utf8');
  const hits = src.split('\n').map((l, i) => [i + 1, l]).filter(([, l]) => /\bEN\(\)/.test(l)).map(([n, l]) => `${n}: ${l.trim().slice(0, 90)}`);
  assert.deepEqual(hits, []);
});

test('settings: every T(\'key\') used by index.html exists in all 5 languages', async () => {
  const { STR } = await import('../apps/settings/www/i18n.js');
  const src = readFileSync(join(SETTINGS, 'index.html'), 'utf8');
  const keys = new Set([...src.matchAll(/\bT\(\s*'([A-Za-z0-9_]+)'\s*\)/g)].map((m) => m[1]));
  assert.ok(keys.size > 20, 'found the T() call sites');
  const out = [];
  for (const l of LANGS) for (const k of keys) if (!(typeof STR[l][k] === 'string' && STR[l][k].length)) out.push(`${l}:${k}`);
  assert.deepEqual(out, []);
});

test('settings: every preset-engine { it, en } label also has es, fr, de', async () => {
  const P = await import('../apps/settings/www/preset-engine.js');
  const reg = { providers: {
    anthropic: { label: 'Anthropic', def: 'm', models: [['m']], caps: {}, tiers: { max: 'm', mid: 'm' } },
    google: { label: 'Gemini', def: 'g', models: [['g']], caps: { image: true, whisper: true, ir: true }, tiers: { max: 'g', mid: 'g' } },
  } };
  const base = P.defaultSig();
  const sigs = [
    base,
    { ...base, online: true, hasKey: true },
    { ...base, online: true, hasKey: false, keys: { ...base.keys, xai: true } },
    { ...base, online: true, hasKey: true, provider: 'google', keys: { ...base.keys, google: true } },
    { ...base, webgpu: true, vramMB: 4096, localModelReady: true, localModelId: 'x' },
    { ...base, packCached: true, packUsable: true },
  ];
  const roots = [P.PRESETS, P.APP_MAP];
  for (const sig of sigs) {
    roots.push(P.gaps(sig, reg), P.APP_MAP.map((a) => P.appStatus(a, sig, reg)));
    for (const id of P.PRESET_IDS) roots.push(P.planPreset(id, sig, reg), P.feasibility(id, sig, reg), P.applyPreset({}, id, sig, reg));
  }
  const bad = new Set(), seen = new Set();
  const walk = (o, path) => {
    if (!o || typeof o !== 'object' || seen.has(o)) return; seen.add(o);
    if (typeof o.it === 'string' && typeof o.en === 'string')
      for (const l of ['es', 'fr', 'de'])
        if (typeof o[l] !== 'string' || (o.en && !o[l])) bad.add(`${path}.${l} (${o.en})`);
    for (const [k, v] of Object.entries(o)) if (k !== 'model') walk(v, `${path}.${k}`);
  };
  roots.forEach((r, i) => walk(r, `#${i}`));
  assert.deepEqual([...bad], []);
});

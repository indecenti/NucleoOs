// unit-converter.test.mjs — host gate for the Unit Converter app's PURE core: the conversion engine
// and the bilingual (EN/IT) natural-language command parser. Auto-picked by the gate's
// `node --test "tools/**/*.test.mjs"` and registered in tools/test-registry.json (nl:true).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { convert, parseCommand, runCommand, resolveUnit } from '../../apps/unit-converter/www/convert.js';
import { validateManifest, lintApp, planRegistryUpdate } from '../../apps/agent/www/app-publish.js';

const near = (a, b, eps = 1e-3) => Math.abs(a - b) <= eps * Math.max(1, Math.abs(b));

// ── the conversion engine ──────────────────────────────────────────────────────────────────
test('length', () => {
  assert.equal(convert(1, 'km', 'm').value, 1000);
  assert.ok(near(convert(1, 'mi', 'm').value, 1609.344));
  assert.ok(near(convert(100, 'cm', 'm').value, 1));
  assert.ok(near(convert(1, 'ft', 'in').value, 12));
});
test('temperature (non-linear)', () => {
  assert.ok(near(convert(0, 'C', 'F').value, 32));
  assert.ok(near(convert(100, 'C', 'F').value, 212));
  assert.ok(near(convert(32, 'F', 'C').value, 0));
  assert.ok(near(convert(0, 'C', 'K').value, 273.15));
});
test('mass / data / time / speed', () => {
  assert.ok(near(convert(1, 'kg', 'lb').value, 2.2046226));
  assert.equal(convert(1, 'GB', 'MB').value, 1024);
  assert.equal(convert(1, 'h', 'min').value, 60);
  assert.ok(near(convert(100, 'kmh', 'mph').value, 62.1371));
});
test('cross-category rejected', () => {
  const r = convert(1, 'km', 'kg');
  assert.equal(r.ok, false);
  assert.equal(r.reason, 'cross-category');
});

// ── natural language: ENGLISH ──────────────────────────────────────────────────────────────
const EN = [
  ['convert 10 km to miles', 10, 'km', 'mi'],
  ['10 km to mi', 10, 'km', 'mi'],
  ['how much is 5 kg in pounds', 5, 'kg', 'lb'],
  ['32 F in C', 32, 'F', 'C'],
  ['convert 2.5 liters to milliliters', 2.5, 'l', 'ml'],
  ['1 GB to MB', 1, 'GB', 'MB'],
  ['100 km/h to mph', 100, 'kmh', 'mph'],
  ['10km -> mi', 10, 'km', 'mi'],
  ['5 in to cm', 5, 'in', 'cm'],
];
for (const [q, v, f, t] of EN) test('NL EN: ' + q, () => {
  const p = parseCommand(q, 'en');
  assert.equal(p.ok, true, 'parse failed: ' + JSON.stringify(p));
  assert.equal(p.value, v); assert.equal(p.from, f); assert.equal(p.to, t);
});

// ── natural language: ITALIAN ──────────────────────────────────────────────────────────────
const IT = [
  ['converti 10 km in miglia', 10, 'km', 'mi'],
  ['quanto fa 5 kg in libbre', 5, 'kg', 'lb'],
  ['100 gradi celsius in fahrenheit', 100, 'C', 'F'],
  ['10 km in miglia', 10, 'km', 'mi'],
  ['converti 2,5 litri in millilitri', 2.5, 'l', 'ml'],
  ['5 chili in grammi', 5, 'kg', 'g'],
  ['3 ore in minuti', 3, 'h', 'min'],
  ['converti 1 gb in mb', 1, 'GB', 'MB'],
];
for (const [q, v, f, t] of IT) test('NL IT: ' + q, () => {
  const p = parseCommand(q, 'it');
  assert.equal(p.ok, true, 'parse failed: ' + JSON.stringify(p));
  assert.equal(p.value, v); assert.equal(p.from, f); assert.equal(p.to, t);
});

test('NL rejects cross-category / gibberish / empty', () => {
  assert.equal(parseCommand('convert 10 km to kg', 'en').ok, false);
  assert.equal(parseCommand('hello world', 'en').ok, false);
  assert.equal(parseCommand('', 'en').ok, false);
});

test('runCommand end-to-end (parse + convert)', () => {
  const r = runCommand('convert 0 C to F', 'en');
  assert.equal(r.ok, true);
  assert.ok(near(r.result, 32));
  const it = runCommand('converti 100 gradi in fahrenheit', 'it');
  assert.equal(it.ok, true);
  assert.ok(near(it.result, 212));
});

// ── the app INSTALLS through the agent's real publish pipeline (deterministic proof) ─────────
// Same pure functions publish_app uses: the manifest validates, the www assets lint clean, and
// the registry upsert is safe (won't clobber a system app). This is "ANIMA Code installs it",
// proven without a device or a model.
const APP = join(dirname(fileURLToPath(import.meta.url)), '..', '..', 'apps', 'unit-converter');
const readApp = (rel) => readFileSync(join(APP, rel), 'utf8');
const checkSyntax = (code) => { try { new Function(code); return { ok: true }; } catch (e) { return { ok: false, error: e.message }; } };

test('pipeline install: manifest validates', () => {
  const m = JSON.parse(readApp('manifest.json'));
  const v = validateManifest(m);
  assert.equal(v.ok, true, JSON.stringify(v.errors));
});
test('pipeline install: www assets lint clean', () => {
  const files = ['www/index.html', 'www/convert.js', 'www/i18n.en.json', 'www/i18n.it.json']
    .map((p) => ({ path: p, content: readApp(p) }));
  const r = lintApp(files, checkSyntax);
  assert.equal(r.ok, true, JSON.stringify(r.errors));
});
test('pipeline install: registry upsert is safe + enables the app', () => {
  const m = JSON.parse(readApp('manifest.json'));
  const reg = { schema: 1, installed: [{ id: 'notepad', created_by: 'system' }] };
  const r = planRegistryUpdate(reg, m);
  assert.equal(r.ok, true, r.reason);
  assert.ok(r.doc.installed.some((a) => a.id === 'unit-converter' && a.enabled === true));
  assert.ok(r.doc.installed.some((a) => a.id === 'notepad'));   // preserves the others
});

console.log(`— ${EN.length + IT.length} NL cases (EN ${EN.length} + IT ${IT.length})`);

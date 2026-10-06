// WiFi Scanner: the "Auto" button's tooltip must state the interval the code really uses, in every
// language. The interval was raised 8 s -> 25 s (each scan blocks the device's single httpd task ~1.5-2 s)
// but the tooltip kept promising 8 s — a user timing the refresh would think Auto is broken. Pure node --test:
// reads apps/wifi-scanner/www/index.html + its i18n catalogs (no browser, no device).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const WWW = join(dirname(fileURLToPath(import.meta.url)), '..', 'apps', 'wifi-scanner', 'www');
const html = readFileSync(join(WWW, 'index.html'), 'utf8');

test('the Auto tooltip states the real auto-scan interval in every language', () => {
  const m = html.match(/autoTimer\s*=\s*setInterval\(\s*scan\s*,\s*(\d+)\s*\)/);
  assert.ok(m, 'auto-scan interval found in index.html');
  const secs = Number(m[1]) / 1000;
  const catalogs = readdirSync(WWW).filter((f) => /^i18n\.[a-z]{2}\.json$/.test(f));
  assert.ok(catalogs.length >= 5, 'all five languages present');
  for (const f of catalogs) {
    const hint = JSON.parse(readFileSync(join(WWW, f), 'utf8').replace(/^﻿/, '')).auto_hint;
    const n = (hint.match(/\d+/g) || []).map(Number);
    assert.deepEqual(n, [secs], `${f}: auto_hint "${hint}" must say ${secs} s`);
  }
});

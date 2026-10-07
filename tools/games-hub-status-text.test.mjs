// The Game Center's "LLM unavailable" status carried the provider/network reason into innerHTML: text from
// outside the app became markup. It must be set as text.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const src = readFileSync(new URL('../apps/games/www/index.html', import.meta.url), 'utf8');

test('failGrok never puts the failure reason into innerHTML', () => {
  const m = /function failGrok\(reason\) \{([\s\S]*?)\n\}/.exec(src);
  assert.ok(m, 'failGrok found');
  assert.doesNotMatch(m[1], /innerHTML[^;]*reason/, 'the reason reaches innerHTML');
  assert.match(m[1], /textContent = t\('llm_unavailable', \{ reason \}\)/);
});

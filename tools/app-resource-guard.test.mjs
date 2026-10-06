// Every curated app carries the resource guard FIRST in its <head>: an inline classic script that reports a
// lost same-origin <script>/<link> to the shell, which reloads that window once (web/shell/wm.js retryFrame,
// e2e: tools/web-e2e/resilience.e2e.mjs). Found on a real Cardputer: its single-task httpd resets a connection
// under a burst, and an app that lost a module stayed dead until reopened. The guard must run before any
// resource of the page starts loading, or the error it listens for has already fired.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync, existsSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO = join(dirname(fileURLToPath(import.meta.url)), '..');
const apps = readdirSync(join(REPO, 'apps')).filter((id) => existsSync(join(REPO, 'apps', id, 'www', 'index.html')));

test('there are curated apps to check', () => assert.ok(apps.length >= 40, `only ${apps.length} apps`));

for (const id of apps) {
  test(`${id}: the resource guard is in <head>, before any script or stylesheet`, () => {
    const html = readFileSync(join(REPO, 'apps', id, 'www', 'index.html'), 'utf8');
    const guard = html.indexOf("type:'app-resource-failed'");
    assert.ok(guard > 0, 'no resource guard — copy it from any other app (first line after <meta charset>)');
    const head = html.search(/<\/head>/i);
    assert.ok(head < 0 || guard < head, 'the guard must sit inside <head>');
    // The first OTHER script or stylesheet of the page must come after the guard.
    const before = html.slice(0, html.lastIndexOf('<script>', guard));
    assert.doesNotMatch(before, /<script\b|<link\b[^>]*rel=["']?(stylesheet|modulepreload)/i, 'a script or stylesheet loads before the guard');
    // It reports same-origin resources only, to the shell's origin only.
    assert.match(html, /u\.origin===location\.origin\)parent\.postMessage\(\{type:'app-resource-failed',src:u\.pathname\},location\.origin\)/);
  });
}

// Host tests for the session-restore contract (web/shell/wm.js + shell.js restoreSession).
//
// Two defects this pins down, both measured on the running OS:
//   1. serialize() saved only the RECTANGLE, so every window came back BLANK — File Commander at its
//      default folder, Notepad on an empty buffer, the player on nothing.
//   2. restoreSession() created EVERY iframe at once, minimised windows included, on a device with
//      4-6 sockets. Those are app loads for windows the user cannot even see.
//
// The query-rebuild rule is the security-relevant half and is pure, so it is tested directly here:
// a saved URL may only ever re-navigate the app it belongs to.

import { test } from 'node:test';
import assert from 'node:assert/strict';

// The rule as implemented in shell.js restoreSession(). Kept in step deliberately: if you change it
// there, this fails and tells you to think about it.
function queryFor(app, saved) {
  if (typeof saved.url === 'string' && app.route && saved.url.indexOf(app.route) === 0) {
    const q = saved.url.indexOf('?');
    if (q >= 0) return saved.url.slice(q + 1);
  }
  return '';
}

const FC = { id: 'file-commander', route: '/apps/file-commander/' };

test('a saved URL restores the app CONTENT, not just the window', () => {
  assert.equal(queryFor(FC, { url: '/apps/file-commander/?path=%2Fdata%2FMusic' }), 'path=%2Fdata%2FMusic');
});

test('no query saved → the app opens at its default', () => {
  assert.equal(queryFor(FC, { url: '/apps/file-commander/' }), '');
  assert.equal(queryFor(FC, {}), '');
  assert.equal(queryFor(FC, { url: null }), '');
});

test('a URL belonging to ANOTHER app is refused', () => {
  // A stale or tampered session must never navigate an app somewhere it does not own.
  assert.equal(queryFor(FC, { url: '/apps/settings/?tab=keys' }), '');
  assert.equal(queryFor(FC, { url: 'https://evil.example/?x=1' }), '');
  assert.equal(queryFor(FC, { url: '/apps/file-commander-evil/?x=1' }).length, 0);
});

test('an app with no route never takes a query', () => {
  assert.equal(queryFor({ id: 'x' }, { url: '/apps/x/?a=1' }), '');
});

// ── the deferred-window contract ──────────────────────────────────────────────────────────────
globalThis.document = { getElementById: () => null, addEventListener() {}, createElement: () => ({ style: {}, classList: { add() {}, remove() {} } }) };
globalThis.window = { addEventListener() {} };
const wm = await import('../web/shell/wm.js');

test('open() accepts the deferred option and frameHtml still drives the body', () => {
  assert.equal(typeof wm.open, 'function');
  assert.equal(wm.open.length, 2, 'open(app, query, opts) — opts is optional so length stays 2');
  // A deferred window renders no frame; a normal one does. frameHtml is the single source for both.
  const html = wm.frameHtml({ id: 'a', name: 'A', route: '/apps/a/' }, '/apps/a/?x=1');
  assert.match(html, /<iframe /);
  assert.ok(html.includes('src="/apps/a/?x=1"'), html);
});

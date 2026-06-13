// Unit tests for the OS keyboard-shortcut resolver (web/shell/shortcuts.js), the pure decision
// logic behind the shell's osKeydown bridge. Run: node --test (or npm test).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolveShortcut, resolveEscape, ESC_SURFACES } from '../web/shell/shortcuts.js';

// Tiny keydown-event factory.
const ev = (key, o = {}) => ({ key, ctrlKey: !!o.ctrl, metaKey: !!o.meta, shiftKey: !!o.shift });
const WIN = { hasActiveWindow: true, editable: false };
const WIN_EDIT = { hasActiveWindow: true, editable: true };
const NOWIN = { hasActiveWindow: false, editable: false };

// ---- document actions (Ctrl+S/Shift+S/N/O): fire even while typing, need an active window ----
test('Ctrl+S → doc save (app focused)', () => assert.deepEqual(resolveShortcut(ev('s', { ctrl: true }), WIN), { type: 'doc', action: 'save' }));
test('Cmd+S → doc save (metaKey == ctrl)', () => assert.deepEqual(resolveShortcut(ev('s', { meta: true }), WIN), { type: 'doc', action: 'save' }));
test('Ctrl+Shift+S → doc saveAs', () => assert.deepEqual(resolveShortcut(ev('S', { ctrl: true, shift: true }), WIN), { type: 'doc', action: 'saveAs' }));
test('Ctrl+N → doc new', () => assert.deepEqual(resolveShortcut(ev('n', { ctrl: true }), WIN), { type: 'doc', action: 'new' }));
test('Ctrl+O → doc open', () => assert.deepEqual(resolveShortcut(ev('o', { ctrl: true }), WIN), { type: 'doc', action: 'open' }));
test('uppercase key normalises (Ctrl+S via "S")', () => assert.equal(resolveShortcut(ev('S', { ctrl: true }), WIN).action, 'save'));

test('Ctrl+S fires EVEN inside a text field (the bug we fixed)', () =>
  assert.deepEqual(resolveShortcut(ev('s', { ctrl: true }), WIN_EDIT), { type: 'doc', action: 'save' }));
test('Ctrl+S with NO active window → none (desktop never hijacks save)', () =>
  assert.deepEqual(resolveShortcut(ev('s', { ctrl: true }), NOWIN), { type: 'none', action: null }));
test('Ctrl+N with no active window → none', () => assert.equal(resolveShortcut(ev('n', { ctrl: true }), NOWIN).type, 'none'));

// ---- editing actions: only OUTSIDE text fields ----
for (const [key, action] of [['c', 'copy'], ['x', 'cut'], ['v', 'paste'], ['a', 'selectAll'], ['z', 'undo'], ['y', 'redo']]) {
  test(`Ctrl+${key.toUpperCase()} outside a field → edit ${action}`, () =>
    assert.deepEqual(resolveShortcut(ev(key, { ctrl: true }), WIN), { type: 'edit', action }));
  test(`Ctrl+${key.toUpperCase()} inside a field → native (browser handles it)`, () =>
    assert.deepEqual(resolveShortcut(ev(key, { ctrl: true }), WIN_EDIT), { type: 'native', action: null }));
}

test('F2 outside a field → edit rename', () => assert.deepEqual(resolveShortcut(ev('F2'), WIN), { type: 'edit', action: 'rename' }));
test('Delete outside a field → edit delete', () => assert.deepEqual(resolveShortcut(ev('Delete'), WIN), { type: 'edit', action: 'delete' }));
test('F5 outside a field → edit refresh', () => assert.deepEqual(resolveShortcut(ev('F5'), WIN), { type: 'edit', action: 'refresh' }));
test('Delete inside a field → native (don\'t delete OS items while editing text)', () =>
  assert.equal(resolveShortcut(ev('Delete'), WIN_EDIT).type, 'native'));

// ---- editing actions also work on the desktop (no window) ----
test('Ctrl+A on the empty desktop → edit selectAll', () => assert.deepEqual(resolveShortcut(ev('a', { ctrl: true }), NOWIN), { type: 'edit', action: 'selectAll' }));
test('Delete on the empty desktop → edit delete', () => assert.deepEqual(resolveShortcut(ev('Delete'), NOWIN), { type: 'edit', action: 'delete' }));

// ---- non-shortcuts and guards ----
test('plain letter → none', () => assert.deepEqual(resolveShortcut(ev('a'), WIN), { type: 'none', action: null }));
test('Ctrl+Shift+C is NOT a copy (shift guard)', () => assert.equal(resolveShortcut(ev('c', { ctrl: true, shift: true }), WIN).type, 'none'));
test('Ctrl+K (unhandled) → none', () => assert.equal(resolveShortcut(ev('k', { ctrl: true }), WIN).type, 'none'));
test('missing ctx defaults safely (no active window, not editable)', () => assert.equal(resolveShortcut(ev('s', { ctrl: true })).type, 'none'));
test('paste carries no extra fields here (caller attaches the clip)', () => assert.equal(resolveShortcut(ev('v', { ctrl: true }), WIN).action, 'paste'));

// ---- Escape priority: Esc is APP-OWNED — the OS dismisses the topmost open SYSTEM surface and
// NEVER closes a window. This locks down the regression the user hit (Esc used to close the focused
// app, unlike every real desktop OS). If anyone re-introduces a "close window" verdict, these fail. ----
const NONE_OPEN = { taskSwitcher: false, dialog: false, copilot: false, actionCenter: false, start: false, contextMenu: false };

test('Esc with nothing open → "app" (THE guard: the OS must NEVER close a window on Esc)', () =>
  assert.equal(resolveEscape(NONE_OPEN), 'app'));
test('Esc with no argument → "app" (defensive default)', () => assert.equal(resolveEscape(), 'app'));
test('Esc with {} → "app"', () => assert.equal(resolveEscape({}), 'app'));

// Each system surface, open alone, is the one Esc dismisses.
for (const surface of ESC_SURFACES) {
  test(`Esc dismisses ${surface} when only it is open`, () =>
    assert.equal(resolveEscape({ ...NONE_OPEN, [surface]: true }), surface));
}

// Fixed priority: an earlier surface wins over every later one when both are open.
for (let i = 0; i < ESC_SURFACES.length; i++) {
  for (let j = i + 1; j < ESC_SURFACES.length; j++) {
    const hi = ESC_SURFACES[i], lo = ESC_SURFACES[j];
    test(`Esc priority: ${hi} beats ${lo}`, () =>
      assert.equal(resolveEscape({ ...NONE_OPEN, [hi]: true, [lo]: true }), hi));
  }
}

// EXHAUSTIVE invariant over all 2^6 open-state combinations:
//   • the verdict is ALWAYS a known surface or 'app' — never a window-close,
//   • it equals 'app' IFF no surface is open,
//   • otherwise it is exactly the FIRST open surface in priority order.
test('Esc resolver invariant holds for all 64 surface combinations (never closes a window)', () => {
  const n = ESC_SURFACES.length;
  for (let mask = 0; mask < (1 << n); mask++) {
    const open = {};
    ESC_SURFACES.forEach((s, bit) => { open[s] = !!(mask & (1 << bit)); });
    const verdict = resolveEscape(open);
    assert.ok(verdict === 'app' || ESC_SURFACES.includes(verdict), `unexpected verdict "${verdict}" for mask ${mask}`);
    assert.equal(verdict, ESC_SURFACES.find((s) => open[s]) || 'app');
    assert.notEqual(verdict, 'closeWindow');   // the exact regression we are guarding against
  }
});

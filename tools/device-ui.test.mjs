// Unit tests for the on-device launcher navigation state machine (web/device/nav.js).
// Pure logic, zero dependencies — run with:  node --test  (npm test)
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Launcher, handleKey } from '../web/device/nav.js';
import { ROOT } from '../web/device/menu-data.js';

const fresh = () => new Launcher(ROOT);

test('starts at root showing the categories', () => {
  const l = fresh();
  const s = l.screen();
  assert.equal(s.title, 'Home');
  assert.equal(s.depth, 0);
  assert.deepEqual(s.items.map((i) => i.label), ['Media', 'Tools', 'System', 'Connect']);
  assert.equal(s.sel, 0);
});

test('up/down wrap around', () => {
  const l = fresh();
  l.up();                                   // wrap from first to last
  assert.equal(l.screen().sel, l.screen().items.length - 1);
  l.down();                                 // wrap back to first
  assert.equal(l.screen().sel, 0);
  l.down();
  assert.equal(l.screen().sel, 1);
});

test('entering a category pushes a submenu with its apps', () => {
  const l = fresh();
  const ev = l.enter();                     // Media
  assert.equal(ev, null);                   // navigation only, no launch
  assert.equal(l.depth, 1);
  const s = l.screen();
  assert.equal(s.title, 'Media');
  assert.deepEqual(s.breadcrumb, ['Home', 'Media']);
  assert.ok(s.items.some((i) => i.id === 'recorder'));
});

test('entering an app emits a launch event', () => {
  const l = fresh();
  l.enter();                                // into Media
  const ev = l.enter();                     // Voice Recorder (first item)
  assert.equal(ev.type, 'launch');
  assert.equal(ev.node.id, 'recorder');
  assert.equal(l.depth, 1, 'launching does not change the menu stack');
});

test('back pops one level and stops at root', () => {
  const l = fresh();
  l.enter();                                // Media (depth 1)
  assert.equal(l.back(), true);
  assert.equal(l.depth, 0);
  assert.equal(l.back(), false);            // already at root
  assert.equal(l.depth, 0);
});

test('type-to-filter narrows the current menu', () => {
  const l = fresh();
  l.enter();                                // into Media
  l.typeChar('m'); l.typeChar('u');         // "mu" -> Music
  const s = l.screen();
  assert.deepEqual(s.items.map((i) => i.id), ['music']);
  assert.equal(s.filter, 'mu');
  assert.match(s.instruction, /Play MP3/);
});

test('filter is case-insensitive and substring', () => {
  const l = fresh();
  l.enter();                                // Media
  l.typeChar('O'); l.typeChar('I'); l.typeChar('C'); // "OIC" appears in vOICe recorder
  assert.deepEqual(l.visibleItems().map((i) => i.id), ['recorder']);
});

test('no-match shows a clear instruction and empty list', () => {
  const l = fresh();
  l.enter();
  l.typeChar('z'); l.typeChar('z');
  const s = l.screen();
  assert.equal(s.items.length, 0);
  assert.match(s.instruction, /No match for "zz"/);
});

test('backspace shrinks the filter; back clears it before popping', () => {
  const l = fresh();
  l.enter();                                // Media (depth 1)
  l.typeChar('m'); l.typeChar('u');
  l.backspace();
  assert.equal(l.screen().filter, 'm');
  assert.equal(l.back(), true);             // clears remaining filter, does NOT pop
  assert.equal(l.depth, 1);
  assert.equal(l.screen().filter, '');
  assert.equal(l.back(), true);             // now pops to root
  assert.equal(l.depth, 0);
});

test('quick keys 1-9 select and activate the n-th visible item', () => {
  const l = fresh();
  const ev = l.quick(2);                    // Tools is 2nd category -> submenu
  assert.equal(ev, null);
  assert.equal(l.screen().title, 'Tools');
  const ev2 = l.quick(1);                   // Calculator -> launch
  assert.equal(ev2.type, 'launch');
  assert.equal(ev2.node.id, 'calculator');
});

test('quick key out of range is ignored', () => {
  const l = fresh();
  assert.equal(l.quick(9), null);           // only 4 categories
  assert.equal(l.depth, 0);
});

test('context menu opens an app action submenu and runs an action', () => {
  const l = fresh();
  l.enter();                                // Media
  assert.equal(l.openContext(), true);      // Voice Recorder actions
  const s = l.screen();
  assert.deepEqual(s.items.map((i) => i.id), ['open', 'pin', 'info']);
  assert.equal(s.items.every((i) => i.type === 'action'), true);
  l.down();                                 // Pin to Home
  const ev = l.enter();
  assert.equal(ev.type, 'action');
  assert.equal(ev.action.id, 'pin');
  assert.equal(ev.node.id, 'recorder');     // remembers which app owns the context menu
});

test('openContext does nothing on a category', () => {
  const l = fresh();
  assert.equal(l.openContext(), false);     // focused item is the Media menu
  assert.equal(l.depth, 0);
});

test('handleKey dispatches raw key names like the firmware', () => {
  const l = fresh();
  assert.equal(handleKey(l, 'down'), null);
  assert.equal(l.screen().sel, 1);
  handleKey(l, 'up');
  assert.equal(l.screen().sel, 0);
  assert.equal(handleKey(l, 'enter'), null);   // into Media
  assert.equal(l.screen().title, 'Media');
  assert.equal(handleKey(l, '2'), null);       // digits now type into the filter, not quick-launch
  assert.equal(l.screen().filter, '2');
});

test('hint text adapts to context', () => {
  const l = fresh();
  assert.match(l.screen().hint, /scroll/);
  assert.doesNotMatch(l.screen().hint, /quick/);      // numeric quick-select removed
  assert.doesNotMatch(l.screen().hint, /esc back/);   // at root, nothing to go back to
  l.enter();                                          // Media (depth 1)
  assert.match(l.screen().hint, /esc back/);
  assert.match(l.screen().hint, /\/ options/);        // focused app has a context menu
  l.typeChar('x');
  assert.match(l.screen().hint, /esc clear/);
});

test('reset returns to a clean root', () => {
  const l = fresh();
  l.enter(); l.typeChar('m');
  l.reset();
  assert.equal(l.depth, 0);
  assert.equal(l.screen().filter, '');
  assert.equal(l.screen().title, 'Home');
});

// Walk the whole menu tree and collect every app id.
function allAppIds(node, out = []) {
  for (const it of node.items || []) {
    if (it.type === 'app') out.push(it.id);
    else if (it.type === 'menu') allAppIds(it, out);
  }
  return out;
}

test('launcher covers every installed app (no app hidden from the on-device menu)', () => {
  const ids = allAppIds(ROOT);
  const expected = ['recorder', 'music', 'photos', 'video-player',          // Media
    'calculator', 'clock', 'files', 'notepad', 'ir', 'dosbox', 'automation-studio', // Tools
    'info', 'status', 'network', 'settings', 'log-viewer',                  // System
    'companion', 'swarm'];                                                  // Connect
  for (const id of expected) assert.ok(ids.includes(id), `app missing from launcher: ${id}`);
});

test('every app id in the tree is unique', () => {
  const ids = allAppIds(ROOT);
  assert.equal(new Set(ids).size, ids.length, 'duplicate app id in the menu tree');
});

test('newly added apps are reachable and launch', () => {
  // Media -> Video
  let l = fresh(); l.enter();
  assert.ok(l.visibleItems().some((i) => i.id === 'video-player'));
  // Tools -> Notes launches
  l = fresh(); l.quick(2);                                  // Tools
  l.typeChar('n'); l.typeChar('o');                         // filter -> Notes
  assert.deepEqual(l.visibleItems().map((i) => i.id), ['notepad']);
  const ev = l.enter();
  assert.equal(ev.type, 'launch');
  assert.equal(ev.node.id, 'notepad');
  // System -> Logs present
  l = fresh(); l.quick(3);                                  // System
  assert.ok(l.visibleItems().some((i) => i.id === 'log-viewer'));
});

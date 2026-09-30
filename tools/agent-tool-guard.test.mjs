// apps/agent/www/tool-guard.js — tool-name repair + doom-loop stop for the agent (OpenCode-style, MIT).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createToolGuard } from '../apps/agent/www/tool-guard.js';

const TOOLS = ['list_files', 'read_file', 'search_files', 'write_file', 'edit_file', 'append_file', 'open_in_os', 'device_status', 'update_plan'];

test('misspelled or mis-cased tool names are repaired when unambiguous', () => {
  const g = createToolGuard(TOOLS);
  assert.deepEqual(g.check('read_file', { path: 'a' }), { run: true, name: 'read_file' });
  assert.equal(g.check('Read_File', { path: 'b' }).name, 'read_file');
  assert.equal(g.check('readfile', { path: 'c' }).name, 'read_file');
  assert.equal(g.check('read-file', { path: 'd' }).name, 'read_file');
  assert.equal(g.check('write_fle', { path: 'e' }).name, 'write_file', 'one-letter typo');
  assert.equal(g.check('devicestatus', {}).name, 'device_status');
});

test('an unknown tool is a readable error that lists the real tools — the loop goes on', () => {
  const g = createToolGuard(TOOLS);
  const r = g.check('run_shell', { cmd: 'ls' });
  assert.equal(r.run, false);
  assert.match(r.content, /Unknown tool "run_shell"/);
  assert.match(r.content, /read_file/);
  assert.equal(g.check('xx', {}).run, false, 'too short to repair');
});

test('the third identical call in a row is not run (doom loop)', () => {
  const g = createToolGuard(TOOLS);
  assert.equal(g.check('read_file', { path: 'x.js' }).run, true);
  assert.equal(g.check('read_file', { path: 'x.js' }).run, true);
  const third = g.check('read_file', { path: 'x.js' });
  assert.equal(third.run, false);
  assert.match(third.content, /3 times in a row/);
  assert.equal(g.check('read_file', { path: 'y.js' }).run, true, 'different arguments: fine');
  assert.equal(g.check('read_file', { path: 'x.js' }).run, true, 'not consecutive any more: fine');
});

test('argument order does not hide a repeat; a real change of arguments resets it', () => {
  const g = createToolGuard(TOOLS);
  g.check('edit_file', { path: 'a', old: 'x', new: 'y' });
  g.check('edit_file', { new: 'y', old: 'x', path: 'a' });
  assert.equal(g.check('edit_file', { old: 'x', path: 'a', new: 'y' }).run, false);
  g.reset();
  assert.equal(g.check('edit_file', { path: 'a', old: 'x', new: 'y' }).run, true, 'reset per task');
});

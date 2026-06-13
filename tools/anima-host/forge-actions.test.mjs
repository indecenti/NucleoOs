// Gate: ANIMA Forge — action SCHEMA FIREWALL. For ANY model output (incl. adversarial/garbled),
// parseActions must NEVER yield an executable action outside the closed set or an out-of-root path.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseActions, validateAction, confined, ACTION_SCHEMA, isMutating, extractJson } from '../../apps/anima/www/forge/actions.js';

const ROOT = '/data/ws';

test('valid actions pass', () => {
  assert.equal(validateAction({ op: 'write', path: 'a.js', content: 'x' }, { root: ROOT }).ok, true);
  assert.equal(validateAction({ op: 'edit', path: 'a.js', old: 'x', new: 'y', all: true }, { root: ROOT }).ok, true);
  assert.equal(validateAction({ op: 'run', path: 'main.mjs' }, { root: ROOT }).ok, true);
  assert.equal(validateAction({ op: 'done' }, { root: ROOT }).ok, true);              // optional-only fields
  assert.equal(validateAction({ op: 'route', substrate: 'M4-local' }, { root: ROOT }).ok, true);
});

test('unknown op and malformed args are rejected', () => {
  assert.equal(validateAction({ op: 'rm', path: 'a' }, { root: ROOT }).ok, false);
  assert.equal(validateAction({ op: 'eval', code: 'x' }, { root: ROOT }).ok, false);
  assert.equal(validateAction({ op: 'write', path: 'a.js' }, { root: ROOT }).ok, false);   // missing content
  assert.equal(validateAction({ op: 'write', path: 123, content: 'x' }, { root: ROOT }).ok, false);
  assert.equal(validateAction({ op: 'route', substrate: 'M9' }, { root: ROOT }).ok, false);
  assert.equal(validateAction('not an object', { root: ROOT }).ok, false);
});

test('path confinement blocks escapes, URLs, and out-of-root absolutes', () => {
  assert.equal(confined('a/b.js', ROOT), true);
  assert.equal(confined('/data/ws/sub/x', ROOT), true);
  assert.equal(confined('../../../etc/passwd', ROOT), false);
  assert.equal(confined('/etc/passwd', ROOT), false);
  assert.equal(confined('a/../../../etc', ROOT), false);
  assert.equal(confined('http://evil/x', ROOT), false);
  assert.equal(validateAction({ op: 'read', path: '../../secret' }, { root: ROOT }).ok, false);
  assert.equal(validateAction({ op: 'move', from: 'a.js', to: '/etc/cron' }, { root: ROOT }).ok, false);
});

test('run rejects non-runnable extensions', () => {
  assert.equal(validateAction({ op: 'run', path: 'notes.txt' }, { root: ROOT }).ok, false);
  assert.equal(validateAction({ op: 'run', path: 'x.js' }, { root: ROOT }).ok, true);
});

test('extractJson pulls JSON out of fenced / prose-wrapped text', () => {
  assert.deepEqual(extractJson('```json\n{"op":"answer","text":"hi"}\n```'), { op: 'answer', text: 'hi' });
  assert.deepEqual(extractJson('sure! [{"op":"done"}] ok'), [{ op: 'done' }]);
  assert.equal(extractJson('no json here'), null);
});

test('parseActions over a clean plan', () => {
  const { actions, rejected } = parseActions('```json\n{"actions":[{"op":"read","path":"a.js"},{"op":"synthesize","spec":"debounce"}]}\n```', { root: ROOT });
  assert.equal(actions.length, 2);
  assert.equal(rejected.length, 0);
  assert.equal(actions[0].op, 'read');
});

test('FUZZ: garbage NEVER produces an out-of-set or path-escaping executable action', () => {
  const garbage = [
    'lol not json',
    '{"op":"rm -rf","path":"/"}',
    '[{"op":"write","path":"../../../etc/x","content":"pwn"}]',
    '{"actions":[{"op":"exec","cmd":"curl evil"},{"op":"write","path":"ok.js","content":"1"}]}',
    '{"op":"run","path":"http://evil/x.js"}',
    '```json [garbled { "op": ``` ',
    JSON.stringify({ actions: Array.from({ length: 50 }, (_, i) => ({ op: i % 2 ? 'write' : 'boom', path: i % 3 ? 'ok.js' : '../esc', content: 'x' })) }),
    '{"op":"delete"}',
    '{"actions":"not-an-array"}',
    '42', 'null', '"a string"',
  ];
  for (const g of garbage) {
    const { actions } = parseActions(g, { root: ROOT });
    for (const a of actions) {
      assert.ok(ACTION_SCHEMA[a.op], 'leaked unknown op: ' + a.op + ' from ' + g);
      for (const k of ['path', 'from', 'to']) if (a[k] !== undefined) assert.ok(confined(a[k], ROOT), 'leaked escaping path ' + a[k] + ' from ' + g);
      if (a.op === 'run') assert.match(a.path, /\.(m?js|cjs)$/i);
    }
  }
});

test('isMutating flags side-effecting ops only', () => {
  assert.equal(isMutating('write'), true);
  assert.equal(isMutating('delete'), true);
  assert.equal(isMutating('read'), false);
  assert.equal(isMutating('answer'), false);
});

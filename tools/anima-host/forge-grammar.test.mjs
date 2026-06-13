// Gate: ANIMA Forge — GBNF grammar compiled from the closed action registry. Single source of
// truth: the grammar (decode-time) admits EXACTLY what the firewall (parse-time) validates.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { toGBNF, grammarAccepts, grammarOps } from '../../apps/anima/www/forge/grammar.js';
import { ACTION_SCHEMA } from '../../apps/anima/www/forge/actions.js';

test('toGBNF is derived from the schema (every op present, no drift)', () => {
  const g = toGBNF();
  assert.deepEqual(grammarOps(), Object.keys(ACTION_SCHEMA));
  for (const op of Object.keys(ACTION_SCHEMA)) assert.match(g, new RegExp('op_' + op + ' ::='), 'missing rule for ' + op);
  assert.match(g, /^root ::=/m);
  assert.match(g, /string ::=/m);
});

test('grammarAccepts admits a pure JSON array of valid actions', () => {
  const r = grammarAccepts('[{"op":"read","path":"a.js"},{"op":"done"}]', { root: '/data/ws' });
  assert.equal(r.ok, true);
  assert.equal(r.actions.length, 2);
  assert.equal(grammarAccepts('{"actions":[{"op":"synthesize","spec":"x"}]}').ok, true);
});

test('grammarAccepts rejects prose, unknown ops, and path escapes (== firewall)', () => {
  assert.equal(grammarAccepts('sure: [{"op":"done"}]').ok, false);            // surrounding prose
  assert.equal(grammarAccepts('[{"op":"rm","path":"/"}]').ok, false);          // unknown op
  assert.equal(grammarAccepts('[{"op":"read","path":"../../etc"}]').ok, false); // escape
  assert.equal(grammarAccepts('not json').ok, false);
});

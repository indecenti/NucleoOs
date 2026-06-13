// Gate: ANIMA Forge — the BUILD-TIME learning gatekeeper (promote-learned.mjs). Conservative second
// gate: only certain+useful Forge recipes enter the shipped corpus; collisions/dups/unsafe rejected.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { promote, mergeLearned, parseJsonl } from '../anima/promote-learned.mjs';

const corpus = [
  { id: 'code-recipe.debounce', category: 'code-recipe', ask: { it: ['come faccio un debounce in javascript'], en: ['how do i debounce in javascript'] }, reply: { it: 'debounce', en: 'debounce' }, detail: { en: 'function debounce(){}' } },
  { id: 'animals.cat', category: 'knowledge', ask: { en: ['how do i feed a cat'] }, reply: { en: 'feed the cat' } },
];

const card = (o) => Object.assign({ category: 'code-recipe', reply: { en: 'x' }, ask: { en: ['how do i x in javascript'] }, detail: { en: 'function x(){}' }, provenance: 'prov-abc' }, o);

test('a CERTAIN + USEFUL novel recipe is promoted', () => {
  const s = card({ id: 'code-recipe.throttle', ask: { it: ['come faccio un throttle in javascript'], en: ['how do i throttle in javascript'] }, reply: { en: 'throttle' }, detail: { en: 'function throttle(fn,ms){let t;return (...a)=>{clearTimeout(t);t=setTimeout(()=>fn(...a),ms);};}' } });
  const { promoted, rejected } = promote([s], corpus);
  assert.equal(promoted.length, 1);
  assert.equal(rejected.length, 0);
  assert.equal(promoted[0].id, 'code-recipe.throttle');
});

test('no provenance → rejected (learned cards MUST be auditable)', () => {
  const s = card({ id: 'code-recipe.noprov', provenance: undefined });
  const { rejected } = promote([s], corpus);
  assert.equal(rejected[0].reason, 'no-provenance');
});

test('code that no longer parses → rejected (bad-syntax)', () => {
  const s = card({ id: 'code-recipe.broken', ask: { en: ['how do i parse json in javascript'] }, detail: { en: 'function(' } });
  const { rejected } = promote([s], corpus);
  assert.equal(rejected[0].reason, 'bad-syntax');
});

test('a same-topic duplicate of an existing recipe → rejected (duplicate)', () => {
  const s = card({ id: 'code-recipe.debounce-fn', ask: { it: ['come faccio un debounce in javascript'], en: ['how do i debounce in javascript'] }, reply: { en: 'debounce' }, detail: { en: 'function debounce(){}' } });
  const { promoted, rejected } = promote([s], corpus);
  assert.equal(promoted.length, 0);
  assert.equal(rejected[0].reason, 'duplicate');
  assert.equal(rejected[0].against, 'code-recipe.debounce');
});

test('a recipe whose trigger collides with a DIFFERENT-category card → rejected (collision)', () => {
  const s = card({ id: 'code-recipe.catfeeder', ask: { en: ['how do i feed a cat'] }, reply: { en: 'a cat feeder' }, detail: { en: 'function feedCat(){}' } });
  const { rejected } = promote([s], corpus);
  assert.equal(rejected[0].reason, 'collision');
  assert.equal(rejected[0].against, 'animals.cat');
});

test('same id as an existing card → duplicate-id (idempotent, never double-ships)', () => {
  const s = card({ id: 'code-recipe.debounce' });
  assert.equal(promote([s], corpus).rejected[0].reason, 'duplicate-id');
});

test('mergeLearned is idempotent by id and round-trips to valid JSONL', () => {
  const p = [card({ id: 'code-recipe.throttle' })];
  const once = mergeLearned('', p);
  const twice = mergeLearned(once, p);
  const a = parseJsonl(once).cards, b = parseJsonl(twice).cards;
  assert.equal(a.length, 1);
  assert.equal(b.length, 1);                          // merging the same card twice does not duplicate
  assert.equal(b[0].id, 'code-recipe.throttle');
});

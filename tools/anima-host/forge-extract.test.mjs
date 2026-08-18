// Gate: ANIMA Forge — claim EXTRACTION + COVERAGE. The must-build-first core of verification:
// any assertive span we cannot route to a checker MUST be reported uncovered (→ WARN, never PASS).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { extract, extractCodeClaims, extractProseClaims, routeClaim } from '../../apps/anima/www/forge/extract.js';

test('code literals (printed/returned) are extracted as checkable claims', () => {
  const claims = extractCodeClaims('function f(){ console.log(42); return 7; }\nconsole.log("hello");');
  const nums = claims.filter((c) => c.kind === 'numeric').map((c) => c.value).sort((a, b) => a - b);
  assert.deepEqual(nums, [7, 42]);
  assert.ok(claims.some((c) => c.kind === 'string' && c.text === 'hello'));
  assert.ok(claims.every((c) => c.checkable === true));
});

test('prose: numeric and known-pattern facts are checkable; bare assertions are UNCOVERED', () => {
  const claims = extractProseClaims('The capital of France is Paris. Bananas are yellow. It costs 5 euros.');
  const known = claims.find((c) => c.kind === 'fact');
  const numeric = claims.find((c) => c.kind === 'numeric');
  const assertion = claims.find((c) => c.kind === 'assertion');
  assert.ok(known && known.checkable === true, 'capital-of pattern should be checkable');
  assert.ok(numeric && numeric.checkable === true);
  assert.ok(assertion && assertion.checkable === false, 'bare factual assertion must be uncovered');
});

test('coverage counts found / checkable / uncovered honestly', () => {
  const { coverage } = extract({ code: 'console.log(1)', prose: 'Bananas are yellow.' });
  assert.equal(coverage.found, 2);
  assert.equal(coverage.checkable, 1);   // the printed 1
  assert.equal(coverage.uncovered, 1);   // the bare assertion
});

test('PLANTED claim must NOT be missed: an assertive sentence raises uncovered', () => {
  // an answer that asserts a fact we cannot ground must surface as uncovered (→ WARN downstream)
  const { coverage } = extract({ prose: 'Mount Everest is located in the Andes.' });
  assert.ok(coverage.uncovered >= 1, 'a confident unverifiable assertion must be counted uncovered');
});

test('pure code with only verifiable literals has zero uncovered', () => {
  const { coverage } = extract({ code: 'console.log(2+2===4 ? "ok":"no")' });
  assert.equal(coverage.uncovered, 0);
});

// ── routeClaim: prose claim → device-verifier spec (the truth-lamp router) ────────────────────

const claimOf = (text) => extractProseClaims(text).find((c) => c.checkable || c.kind === 'fact');

test('routeClaim: a DERIVABLE expression routes, in every phrasing the corpus showed', () => {
  for (const [text, key, asserted] of [
    ['Il risultato di 2+2 è 4.', '2+2', '4'],
    ['10 * 10 fa 100.', '10 * 10', '100'],
    ['La somma richiesta, 15+27, è uguale a 42.', '15+27', '42'],
    ['Sure — 12*12 equals 144.', '12*12', '144'],
    ['The division 100/4 is equal to 25.', '100/4', '25'],
    ['Applicando la formula: 3*(4+5) = 27.', '3*(4+5)', '27'],
  ]) {
    const spec = routeClaim(claimOf(text));
    assert.ok(spec, 'must route: ' + text);
    assert.equal(spec.kind, 'numeric');
    assert.equal(spec.key, key, text);
    assert.equal(spec.asserted, asserted, text);
  }
});

test('routeClaim: a bare measurement has nothing to re-derive → null, never a fake spec', () => {
  for (const text of [
    'Il Monte Bianco è alto 4808 metri.',
    'The Eiffel Tower is 330 metres tall and was completed in 1889.',
    'La Luna dista in media 384400 km dalla Terra.',
  ]) {
    assert.equal(routeClaim(claimOf(text)), null, 'must NOT route: ' + text);
  }
});

test('routeClaim: capital-of facts route with their OWN language attached', () => {
  const it = routeClaim(claimOf('La capitale della Francia è Parigi.'));
  assert.deepEqual(it, { kind: 'fact', key: 'capitale della Francia', asserted: 'Parigi', lang: 'it' });
  const en = routeClaim(claimOf('As you asked: the capital of Portugal is Lisbon.'));
  assert.deepEqual(en, { kind: 'fact', key: 'capital of Portugal', asserted: 'Lisbon', lang: 'en' });
});

test('routeClaim survives garbage without inventing a spec', () => {
  for (const c of [null, {}, { kind: 'numeric', text: '' }, { kind: 'fact', text: 'niente di utile qui' },
                   { kind: 'assertion', text: 'The capital of France is Paris.' }]) {
    assert.equal(routeClaim(c), null);
  }
});

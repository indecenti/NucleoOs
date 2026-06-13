// Gate: ANIMA Forge — claim EXTRACTION + COVERAGE. The must-build-first core of verification:
// any assertive span we cannot route to a checker MUST be reported uncovered (→ WARN, never PASS).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { extract, extractCodeClaims, extractProseClaims } from '../../apps/anima/www/forge/extract.js';

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

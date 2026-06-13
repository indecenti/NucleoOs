// Gate: ANIMA Forge — grounded best-of-N selection. Picks by EXECUTED signal, never self-report.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pickBest, rank, admissible } from '../../apps/anima/www/forge/selfcheck.js';

const C = (o) => Object.assign({ code: 'x', check: { ok: true }, verdict: 'pass', caps: 0, bytes: 100, tests: { passed: 1, total: 1 } }, o);

test('admissible requires a passing check and a non-veto verdict', () => {
  assert.equal(admissible(C({})), true);
  assert.equal(admissible(C({ check: { ok: false } })), false);
  assert.equal(admissible(C({ verdict: 'veto' })), false);
});

test('pickBest chooses the candidate that PASSES its tests over one that does not', () => {
  const good = C({ code: 'good', tests: { passed: 3, total: 3 } });
  const weak = C({ code: 'weak', tests: { passed: 1, total: 3 } });
  const broken = C({ code: 'broken', check: { ok: false }, verdict: 'veto' });
  assert.equal(pickBest([weak, broken, good]).code, 'good');
});

test('tie-breakers: pass>warn, then least capability, then smallest', () => {
  const warnBig = C({ code: 'w', verdict: 'warn', caps: 1, bytes: 50 });
  const passBig = C({ code: 'p', verdict: 'pass', caps: 3, bytes: 500 });
  assert.equal(pickBest([warnBig, passBig]).code, 'p');                 // pass beats warn
  const a = C({ code: 'a', caps: 2, bytes: 100 }), b = C({ code: 'b', caps: 1, bytes: 100 });
  assert.equal(pickBest([a, b]).code, 'b');                              // fewer caps
});

test('no admissible candidate → null (loop takes a FIX turn)', () => {
  assert.equal(pickBest([C({ check: { ok: false } }), C({ verdict: 'veto' })]), null);
  assert.equal(rank([]).length, 0);
});

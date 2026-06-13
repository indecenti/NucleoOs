// Gate: ANIMA Forge — verdict combiner. Honesty contract: absence of evidence → WARN, never PASS;
// a failed dry-run or a contradicted claim → VETO.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { combineVerdict, verdictChip } from '../../apps/anima/www/forge/verify.js';

test('clean: ran hermetically, all checks confirmed, full coverage → PASS', () => {
  const v = combineVerdict({ run: { ok: true }, checks: [{ status: 'confirmed' }], coverage: { found: 1, checkable: 1, uncovered: 0 } });
  assert.equal(v.verdict, 'pass');
});

test('dry-run failure → VETO', () => {
  const v = combineVerdict({ run: { ok: false, error: 'SyntaxError' }, coverage: { uncovered: 0 } });
  assert.equal(v.verdict, 'veto');
  assert.match(v.reasons.join(), /dry-run-failed/);
});

test('contradicted grounded claim → VETO (even if it ran)', () => {
  const v = combineVerdict({ run: { ok: true }, checks: [{ status: 'contradicted' }], coverage: { uncovered: 0 } });
  assert.equal(v.verdict, 'veto');
});

test('uncovered assertive span → WARN (never silent PASS)', () => {
  const v = combineVerdict({ run: { ok: true }, checks: [], coverage: { found: 1, checkable: 0, uncovered: 1 } });
  assert.equal(v.verdict, 'warn');
});

test('unknown check → WARN', () => {
  const v = combineVerdict({ run: { ok: true }, checks: [{ status: 'unknown' }], coverage: { uncovered: 0 } });
  assert.equal(v.verdict, 'warn');
});

test('capguard BLOCK (dangerous code) → VETO even if it parses', () => {
  const v = combineVerdict({ run: { ok: true }, coverage: { uncovered: 0 }, capguard: { severity: 'block', dangers: [{ kind: 'dynamic-eval' }] } });
  assert.equal(v.verdict, 'veto');
  assert.match(v.reasons.join(), /capability-block/);
});

test('capguard WARN (over-privilege) → WARN', () => {
  const v = combineVerdict({ run: { ok: true }, coverage: { uncovered: 0 }, capguard: { severity: 'warn', over: ['fs.write'], dangers: [] } });
  assert.equal(v.verdict, 'warn');
  assert.match(v.reasons.join(), /capability-warn/);
});

test('verdict chips are icon+text (never colour-only)', () => {
  assert.equal(verdictChip({ verdict: 'pass' }).icon, '✓');
  assert.equal(verdictChip({ verdict: 'warn' }).label, 'unverified');
  assert.equal(verdictChip({ verdict: 'veto' }).icon, '⛔');
});

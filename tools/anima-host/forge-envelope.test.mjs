// Gate: ANIMA Forge — Envelope v1. Every substrate must normalise into ONE renderer-safe shape.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { normalize, validate, provenance, deviceSubstrate, MODE_TO_SUBSTRATE } from '../../apps/anima/www/forge/envelope.js';

test('device JSON (mode off) normalises to a valid M1 envelope', () => {
  const raw = { query: 'chi è dante', tier: 'fact', action: 'answer', reply: 'Dante…', confidence: 92, domain: 'knowledge', intent: 'who', lang: 'it', trace: 'L1 · ok', awaiting: 0 };
  const env = normalize(raw, { mode: 'off' });
  assert.equal(env.substrate, 'M1');
  assert.equal(env.reply, 'Dante…');
  assert.equal(env.confidence, 92);
  assert.equal(env.awaiting, false);
  assert.deepEqual(env.actions, []);
  assert.equal(env.verdict, null);
  assert.ok(validate(env).ok, JSON.stringify(validate(env).errors));
});

test('M4 local result conforms to the identical envelope', () => {
  const env = normalize({ reply: 'function debounce(){}', domain: 'local-llm', actions: [{ op: 'write', path: 'a.js' }], verdict: { verdict: 'warn', reasons: [] }, usage: { tokens: 120 } }, { substrate: 'M4-local' });
  assert.equal(env.substrate, 'M4-local');
  assert.equal(env.verdict.verdict, 'warn');
  assert.equal(env.usage.tokens, 120);
  assert.ok(validate(env).ok);
});

test('mode→substrate mapping and provenance (M4 is NOT grounded)', () => {
  assert.equal(deviceSubstrate('on'), 'M2');
  assert.equal(MODE_TO_SUBSTRATE.only, 'M3');
  assert.equal(provenance({ substrate: 'M1' }).grounded, true);
  assert.equal(provenance({ substrate: 'M4-local' }).grounded, false);
  assert.equal(provenance({ substrate: 'M4-local' }).label, 'local-GPU');
});

test('validate rejects malformed envelopes', () => {
  assert.equal(validate(null).ok, false);
  assert.equal(validate({ v: 2 }).ok, false);
  const bad = normalize({}, { substrate: 'M9' });
  assert.equal(validate(bad).ok, false);
});

test('numeric/bool coercion is renderer-safe (no NaN, no undefined strings)', () => {
  const env = normalize({ confidence: '87', awaiting: 1, reply: null, trace: undefined });
  assert.equal(env.confidence, 87);
  assert.equal(env.awaiting, true);
  assert.equal(env.reply, '');
  assert.equal(env.trace, '');
  assert.ok(validate(env).ok);
});

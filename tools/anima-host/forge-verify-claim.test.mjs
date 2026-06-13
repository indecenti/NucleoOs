// Gate: ANIMA Forge — the DEVICE-SIDE grounded verifier (nucleo_anima_verify_claim) driven through
// the REAL firmware cascade exe. This is the "M1 judges M4" half of cross-substrate verification:
// numeric claims re-derived on the exact math engine; fact claims checked against KGE/L1 with
// abstention. Builds the real anima.exe first (host harness), then asserts the verdicts.
import { test, before } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { existsSync } from 'node:fs';

const here = dirname(fileURLToPath(import.meta.url));
const exe = join(here, 'build', 'anima.exe');

before(() => {
  const b = spawnSync('node', [join(here, 'anima.mjs'), '--ensure'], { encoding: 'utf8' });
  assert.ok(b.status === 0 && existsSync(exe), 'anima.exe must build for the verify gate');
});

// Run the device verifier; returns 'confirmed' | 'contradicted' | 'unknown'.
function verify(spec, { en = false } = {}) {
  const args = (en ? ['--en'] : []).concat(['--verify', spec]);
  const r = spawnSync(exe, args, { encoding: 'utf8' });
  const m = /VERDICT=(\w+)/.exec(r.stdout || '');
  assert.ok(m, 'no VERDICT in output: ' + (r.stdout || '') + (r.stderr || ''));
  return m[1];
}

test('NUMERIC: the exact math engine re-derives and judges', () => {
  assert.equal(verify('numeric|2+2|4'), 'confirmed');
  assert.equal(verify('numeric|2+2|5'), 'contradicted');
  assert.equal(verify('numeric|10*10|100'), 'confirmed');
  assert.equal(verify('numeric|not a calc|3'), 'unknown');     // not computable → abstain (caller WARNs)
});

test('FACT (IT): KGE/L1 confirms a true claim and CONTRADICTS a false one', () => {
  assert.equal(verify('fact|capitale della francia|Parigi'), 'confirmed');
  assert.equal(verify('fact|capitale della francia|Lione'), 'contradicted');
});

test('FACT (EN): same grounded judgement in English', () => {
  assert.equal(verify('fact|capital of france|Paris', { en: true }), 'confirmed');
  assert.equal(verify('fact|capital of france|Berlin', { en: true }), 'contradicted');
});

test('ABSTAIN: an unknown-domain claim is UNKNOWN, never fabricated as confirmed/contradicted', () => {
  assert.equal(verify('fact|qual e la capitale di Floonkistan|Boop'), 'unknown');
});

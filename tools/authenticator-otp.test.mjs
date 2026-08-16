// Unit tests for the pure-JS OTP core (apps/authenticator/www/otp.js) against the official RFC
// test vectors — RFC 4226 (HOTP, App. D) and RFC 6238 (TOTP, App. B). Pure JS + no Web Crypto is
// the whole point (the device serves over http LAN = no secure context), so it must be provably
// correct here.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { sha1, sha256, base32Decode, hotp, totp, parseOtpauth, secondsRemaining } from '../apps/authenticator/www/otp.js';

const hex = (bytes) => bytes.map((b) => b.toString(16).padStart(2, '0')).join('');

test('SHA-1 known-answer', () => {
  assert.equal(hex(sha1([...Buffer.from('abc')])), 'a9993e364706816aba3e25717850c26c9cd0d89d');
  assert.equal(hex(sha1([])), 'da39a3ee5e6b4b0d3255bfef95601890afd80709');
});
test('SHA-256 known-answer', () => {
  assert.equal(hex(sha256([...Buffer.from('abc')])), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
});

// RFC 4226 App. D — secret "12345678901234567890" (ASCII), counters 0..9, 6 digits, SHA-1.
test('HOTP RFC 4226 vectors', () => {
  const secret = [...Buffer.from('12345678901234567890')];
  const expected = ['755224', '287082', '359152', '969429', '338314', '254676', '287922', '162583', '399871', '520489'];
  for (let c = 0; c < expected.length; c++) assert.equal(hotp(secret, c, { digits: 6 }), expected[c], 'counter ' + c);
});

// RFC 6238 App. B — SHA-1 secret is the 20-byte "12345678901234567890"; T = floor(unixtime/30), 8 digits.
test('TOTP RFC 6238 vectors (SHA-1, 8 digits)', () => {
  // base32 of "12345678901234567890" = GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ
  const secret = 'GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ';
  const cases = [[59, '94287082'], [1111111109, '07081804'], [1111111111, '14050471'], [1234567890, '89005924'], [2000000000, '69279037']];
  for (const [t, code] of cases) assert.equal(totp(secret, { digits: 8, period: 30, algorithm: 'SHA1', now: t * 1000 }), code, 't=' + t);
});

test('TOTP RFC 6238 vectors (SHA-256, 8 digits)', () => {
  // 32-byte SHA-256 seed "12345678901234567890123456789012" -> base32
  const secret = base32(Buffer.from('12345678901234567890123456789012'));
  const cases = [[59, '46119246'], [1111111109, '68084774'], [2000000000, '90698825']];
  for (const [t, code] of cases) assert.equal(totp(secret, { digits: 8, period: 30, algorithm: 'SHA256', now: t * 1000 }), code, 't=' + t);
});

function base32(buf) {
  const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'; let bits = 0, val = 0, out = '';
  for (const b of buf) { val = (val << 8) | b; bits += 8; while (bits >= 5) { out += A[(val >>> (bits - 5)) & 31]; bits -= 5; } }
  if (bits) out += A[(val << (5 - bits)) & 31];
  return out;
}

test('base32Decode tolerates spaces, lowercase, missing padding', () => {
  assert.deepEqual(base32Decode('me'), base32Decode('ME======'));
  assert.deepEqual(base32Decode('jbsw y3dp'), [...Buffer.from('Hello')]);   // JBSWY3DP = "Hello"
});

test('parseOtpauth reads a standard Google/GitHub URI', () => {
  const p = parseOtpauth('otpauth://totp/GitHub:alice?secret=JBSWY3DPEHPK3PXP&issuer=GitHub&digits=6&period=30&algorithm=SHA1');
  assert.equal(p.type, 'totp'); assert.equal(p.issuer, 'GitHub'); assert.equal(p.account, 'alice');
  assert.equal(p.secret, 'JBSWY3DPEHPK3PXP'); assert.equal(p.digits, 6); assert.equal(p.period, 30);
});
test('parseOtpauth splits an "Issuer:account" label and rejects junk', () => {
  const p = parseOtpauth('otpauth://totp/ACME%20Co:john@acme.com?secret=JBSWY3DPEHPK3PXP');
  assert.equal(p.issuer, 'ACME Co'); assert.equal(p.account, 'john@acme.com');
  assert.equal(parseOtpauth('https://example.com'), null);
  assert.equal(parseOtpauth('otpauth://totp/x?secret=not-base32!!'), null);
});
test('secondsRemaining is within (0, period]', () => {
  const r = secondsRemaining(30, 1000 * 45);   // 45s -> 15 into the 2nd window -> 15 left
  assert.equal(r, 15);
});

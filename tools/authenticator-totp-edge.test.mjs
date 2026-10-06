// Edge-case regression tests for the Authenticator's pure-JS OTP core (apps/authenticator/www/otp.js).
// authenticator-otp.test.mjs pins the RFC 4226 / RFC 6238 8-digit vectors; this file covers what a user
// actually hits on top of them: 6-digit codes, window boundaries, non-default periods, every RFC 4648
// base32 vector in the shapes people paste (lowercase, no padding, grouped with spaces/dashes), the
// otpauth:// URIs real QR codes carry, and — the dangerous class — an algorithm the hashers cannot compute
// silently producing a SHA-1 code that never works. Cross-checked against node:crypto as an independent
// reference implementation. Pure node --test: no browser, no device, runs on Linux CI.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { base32Decode, isValidBase32, hotp, totp, parseOtpauth, secondsRemaining } from '../apps/authenticator/www/otp.js';

// RFC 4648 §6 encoder (the test's own, so a decoder bug cannot hide behind a matching encoder bug).
function b32(buf) {
  const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'; let bits = 0, val = 0, out = '';
  for (const b of buf) { val = ((val << 8) | b) & 0xffff; bits += 8; while (bits >= 5) { out += A[(val >>> (bits - 5)) & 31]; bits -= 5; } }
  if (bits) out += A[(val << (5 - bits)) & 31];
  return out;
}
// Independent RFC 6238 reference over node:crypto's HMAC.
function refTotp(secretBuf, tSec, { digits = 6, period = 30, algorithm = 'sha1' } = {}) {
  const c = Buffer.alloc(8); c.writeBigUInt64BE(BigInt(Math.floor(tSec / period)));
  const h = createHmac(algorithm, secretBuf).update(c).digest();
  const o = h[h.length - 1] & 0x0f;
  const bin = ((h[o] & 0x7f) << 24) | (h[o + 1] << 16) | (h[o + 2] << 8) | h[o + 3];
  return String(bin % 10 ** digits).padStart(digits, '0');
}

const SEED20 = Buffer.from('12345678901234567890');
const SEED32 = Buffer.from('12345678901234567890123456789012');

test('TOTP 6-digit codes (what nearly every site uses) are the RFC 6238 vectors truncated mod 10^6', () => {
  // RFC 6238 App. B SHA-1 column; 6-digit truncation = the low 6 digits of the 8-digit code.
  const cases = [[59, '94287082'], [1111111109, '07081804'], [1111111111, '14050471'], [1234567890, '89005924'],
    [2000000000, '69279037'], [20000000000, '65353130']];
  for (const [t, code8] of cases) {
    assert.equal(totp(b32(SEED20), { digits: 8, now: t * 1000 }), code8, 'SHA-1 8 digits t=' + t);
    assert.equal(totp(b32(SEED20), { digits: 6, now: t * 1000 }), code8.slice(2), 'SHA-1 6 digits t=' + t);
  }
  // The SHA-256 rows the existing suite does not pin (incl. a 64-bit counter beyond 2^32 s).
  for (const [t, code8] of [[1111111111, '67062674'], [1234567890, '91819424'], [20000000000, '77737706']])
    assert.equal(totp(b32(SEED32), { digits: 8, algorithm: 'SHA256', now: t * 1000 }), code8, 'SHA-256 t=' + t);
});

test('TOTP keeps leading zeros and changes exactly at the window boundary', () => {
  const s = b32(SEED20);
  // t=1111111109 -> 07081804: a leading zero must survive (a Number() round-trip would print 7081804).
  assert.equal(totp(s, { digits: 8, now: 1111111109 * 1000 }), '07081804');
  // Same 30 s window -> same code; one millisecond into the next window -> the next code.
  assert.equal(totp(s, { now: 30000 }), totp(s, { now: 59999 }));
  assert.equal(totp(s, { now: 29999 }), refTotp(SEED20, 0));
  assert.equal(totp(s, { now: 30000 }), refTotp(SEED20, 30));
  assert.notEqual(totp(s, { now: 29999 }), totp(s, { now: 30000 }));
  assert.equal(secondsRemaining(30, 30000), 30, 'a fresh window has the full period left');
  assert.equal(secondsRemaining(30, 59999), 1, 'the last second of a window');
  assert.equal(secondsRemaining(60, 61000), 59, 'non-default period');
});

test('TOTP matches an independent HMAC reference across periods, digits, algorithms and secret lengths', () => {
  let seed = 7; const rnd = () => (seed = (seed * 1103515245 + 12345) >>> 0) / 2 ** 32;
  for (let i = 0; i < 60; i++) {
    const len = [10, 16, 20, 32, 64, 80][i % 6];                     // 80 B > the HMAC block: the key is hashed first
    const secret = Buffer.from(Array.from({ length: len }, () => Math.floor(rnd() * 256)));
    const t = Math.floor(rnd() * 4e9);
    const o = { digits: i % 2 ? 8 : 6, period: [30, 60, 15][i % 3] };
    for (const [alg, ref] of [['SHA1', 'sha1'], ['SHA256', 'sha256']])
      assert.equal(totp(b32(secret), { ...o, algorithm: alg, now: t * 1000 }), refTotp(secret, t, { ...o, algorithm: ref }),
        `${alg} len=${len} t=${t} ${JSON.stringify(o)}`);
  }
});

test('base32 decodes every RFC 4648 vector in the shapes users paste', () => {
  const V = [['', ''], ['f', 'MY======'], ['fo', 'MZXQ===='], ['foo', 'MZXW6==='], ['foob', 'MZXW6YQ='],
    ['fooba', 'MZXW6YTB'], ['foobar', 'MZXW6YTBOI======']];
  for (const [plain, enc] of V) {
    const want = [...Buffer.from(plain)];
    assert.deepEqual(base32Decode(enc), want, enc);
    assert.deepEqual(base32Decode(enc.replace(/=+$/, '')), want, 'no padding: ' + enc);
    assert.deepEqual(base32Decode(enc.toLowerCase()), want, 'lowercase: ' + enc);
  }
  // Grouped the way sites display it ("jbsw y3dp ehpk 3pxp"), with dashes, tabs/newlines from a copy.
  const hello = [...Buffer.from('Hello!\xde\xad\xbe\xef', 'latin1')];
  assert.deepEqual(base32Decode('JBSWY3DPEHPK3PXP'), hello);
  for (const v of ['jbsw y3dp ehpk 3pxp', 'JBSW-Y3DP-EHPK-3PXP', ' JBSWY3DP\tEHPK3PXP\n', 'JBSWY3DPEHPK3PXP===='])
    assert.deepEqual(base32Decode(v), hello, JSON.stringify(v));
});

test('base32 rejects what is not base32 instead of decoding garbage', () => {
  for (const bad of ['JBSWY3DP0', 'JBSWY3DP1', 'JBSWY3DP8', 'JBSWY3DP9', 'JBSW!Y3DP', 'JBSWY3DPé'])
    assert.throws(() => base32Decode(bad), /bad base32/, bad);
  for (const bad of ['', '   ', '====', 'A', 'not-base32!!', null, undefined]) assert.equal(isValidBase32(bad), false, String(bad));
  assert.equal(isValidBase32('jbsw y3dp'), true);
});

test('otpauth:// URIs as real QR codes encode them', () => {
  // Issuer only in the label prefix, URL-encoded space and an encoded colon separator.
  let p = parseOtpauth('otpauth://totp/ACME%20Co%3Ajohn.doe%40acme.com?secret=JBSWY3DPEHPK3PXP');
  assert.equal(p.issuer, 'ACME Co'); assert.equal(p.account, 'john.doe@acme.com');
  // issuer= parameter wins over the label prefix; the account keeps any further colons.
  p = parseOtpauth('otpauth://totp/Old:me:work?secret=JBSWY3DPEHPK3PXP&issuer=New');
  assert.equal(p.issuer, 'New'); assert.equal(p.account, 'me:work');
  // No issuer anywhere: the whole label is the account.
  p = parseOtpauth('otpauth://totp/alice@example.com?secret=JBSWY3DPEHPK3PXP');
  assert.equal(p.issuer, ''); assert.equal(p.account, 'alice@example.com');
  // A grouped secret (spaces encoded) is accepted and stored without the spaces.
  p = parseOtpauth('otpauth://totp/X:y?secret=JBSW%20Y3DP%20EHPK%203PXP');
  assert.equal(p.secret, 'JBSWY3DPEHPK3PXP');
  // Upper-case type, 8 digits, 60 s, SHA256 — all carried through.
  p = parseOtpauth('  otpauth://TOTP/X:y?secret=JBSWY3DPEHPK3PXP&digits=8&period=60&algorithm=SHA256  ');
  assert.deepEqual([p.type, p.digits, p.period, p.algorithm], ['totp', 8, 60, 'SHA256']);
  // Missing / zero / absurd period: never a 0 s window (division by zero) or a sub-5 s one.
  assert.equal(parseOtpauth('otpauth://totp/X?secret=JBSWY3DPEHPK3PXP&period=0').period, 30);
  assert.equal(parseOtpauth('otpauth://totp/X?secret=JBSWY3DPEHPK3PXP&period=1').period, 5);
  assert.equal(parseOtpauth('otpauth://totp/X?secret=JBSWY3DPEHPK3PXP&period=abc').period, 30);
  // SHA512 is REPORTED as SHA512 (so the app can refuse it), never folded into SHA1.
  assert.equal(parseOtpauth('otpauth://totp/X?secret=JBSWY3DPEHPK3PXP&algorithm=SHA512').algorithm, 'SHA512');
  // HOTP is recognized as HOTP (the app refuses it) with its counter.
  p = parseOtpauth('otpauth://hotp/X?secret=JBSWY3DPEHPK3PXP&counter=42');
  assert.deepEqual([p.type, p.counter], ['hotp', 42]);
  // Junk: wrong scheme/type, no secret, invalid secret, not a URL.
  for (const bad of ['otpauth://totp/X', 'otpauth://totp/X?secret=', 'otpauth://totp/X?secret=0189',
    'otpauth://steam/X?secret=JBSWY3DPEHPK3PXP', 'http://totp/X?secret=JBSWY3DPEHPK3PXP', 'JBSWY3DPEHPK3PXP', '', null])
    assert.equal(parseOtpauth(bad), null, String(bad));
});

test('otpauth:// with a raw "%" in the label is parsed, never thrown (the add sheet would die mid-paste)', () => {
  // "100% Pure" typed into a QR generator that does not percent-encode: decodeURIComponent throws a
  // URIError on "% P". The parser is called from an input handler and from Save — a throw there leaves
  // the sheet silently unresponsive. It must return a usable account (or null), never throw.
  let p;
  assert.doesNotThrow(() => { p = parseOtpauth('otpauth://totp/100% Pure:bob?secret=JBSWY3DPEHPK3PXP'); });
  assert.ok(p, 'the account is still usable');
  assert.equal(p.secret, 'JBSWY3DPEHPK3PXP');
  assert.equal(p.account, 'bob');
  assert.match(p.issuer, /^100% ?Pure$/);
});

test('an algorithm the core cannot compute throws instead of silently producing a SHA-1 code', () => {
  // A vault entry with algorithm SHA512 (written by another tool, a future version, an import) used to
  // fall back to SHA-1: the app showed a perfectly plausible code that never logs in. Throwing lets the
  // UI show "——————" instead.
  const s = b32(SEED20);
  const sha1Code = totp(s, { now: 59000 });
  for (const alg of ['SHA512', 'MD5', 'sha3-256']) assert.throws(() => totp(s, { algorithm: alg, now: 59000 }), alg);
  assert.throws(() => hotp([...SEED20], 1, { algorithm: 'SHA512' }));
  // The supported spellings keep working, and an absent algorithm still means SHA1 (old vault entries).
  for (const alg of ['SHA1', 'sha1', 'SHA-1', undefined, null, '']) assert.equal(totp(s, { algorithm: alg, now: 59000 }), sha1Code, String(alg));
  assert.equal(totp(b32(SEED32), { algorithm: 'sha-256', digits: 8, now: 59000 }), '46119246');
});

// otp.js — RFC 4226 (HOTP) + RFC 6238 (TOTP) in PURE JavaScript, no Web Crypto.
//
// Why pure JS and not crypto.subtle: the Cardputer serves the whole OS over a plain http:// LAN IP,
// which is NOT a secure context, so `crypto.subtle` is unavailable there. A pure implementation runs
// everywhere — http LAN, offline, inside the WASM twin — with zero dependencies. It's also what makes
// this module unit-testable under `node --test` against the RFC test vectors.
//
// Everything here is browser/host side; the Cardputer is never involved in generating a code.

// ---- SHA-1 / SHA-256 (byte[] -> byte[]) ---------------------------------------------------------
function rotl(n, b) { return ((n << b) | (n >>> (32 - b))) >>> 0; }

export function sha1(bytes) {
  const ml = bytes.length * 8;
  const withOne = [...bytes, 0x80];
  while (withOne.length % 64 !== 56) withOne.push(0);
  // 64-bit length, big-endian (high word is 0 for our small messages)
  for (let i = 7; i >= 0; i--) withOne.push((i < 4) ? (ml >>> (i * 8)) & 0xff : 0);
  let h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;
  const w = new Array(80);
  for (let i = 0; i < withOne.length; i += 64) {
    for (let j = 0; j < 16; j++) w[j] = (withOne[i + j * 4] << 24) | (withOne[i + j * 4 + 1] << 16) | (withOne[i + j * 4 + 2] << 8) | (withOne[i + j * 4 + 3]);
    for (let j = 16; j < 80; j++) w[j] = rotl(w[j - 3] ^ w[j - 8] ^ w[j - 14] ^ w[j - 16], 1);
    let a = h0, b = h1, c = h2, d = h3, e = h4;
    for (let j = 0; j < 80; j++) {
      let f, k;
      if (j < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (j < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      const tmp = (rotl(a, 5) + f + e + k + w[j]) >>> 0;
      e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
    }
    h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0; h3 = (h3 + d) >>> 0; h4 = (h4 + e) >>> 0;
  }
  const out = [];
  for (const h of [h0, h1, h2, h3, h4]) out.push((h >>> 24) & 0xff, (h >>> 16) & 0xff, (h >>> 8) & 0xff, h & 0xff);
  return out;
}

const K256 = [
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2];
export function sha256(bytes) {
  const ror = (n, b) => ((n >>> b) | (n << (32 - b))) >>> 0;
  const ml = bytes.length * 8;
  const m = [...bytes, 0x80];
  while (m.length % 64 !== 56) m.push(0);
  for (let i = 7; i >= 0; i--) m.push((i < 4) ? (ml >>> (i * 8)) & 0xff : 0);
  let h = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
  const w = new Array(64);
  for (let i = 0; i < m.length; i += 64) {
    for (let j = 0; j < 16; j++) w[j] = (m[i + j * 4] << 24) | (m[i + j * 4 + 1] << 16) | (m[i + j * 4 + 2] << 8) | m[i + j * 4 + 3];
    for (let j = 16; j < 64; j++) {
      const s0 = ror(w[j - 15], 7) ^ ror(w[j - 15], 18) ^ (w[j - 15] >>> 3);
      const s1 = ror(w[j - 2], 17) ^ ror(w[j - 2], 19) ^ (w[j - 2] >>> 10);
      w[j] = (w[j - 16] + s0 + w[j - 7] + s1) >>> 0;
    }
    let [a, b, c, d, e, f, g, hh] = h;
    for (let j = 0; j < 64; j++) {
      const S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (hh + S1 + ch + K256[j] + w[j]) >>> 0;
      const S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      hh = g; g = f; f = e; e = (d + t1) >>> 0; d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    h = [h[0] + a, h[1] + b, h[2] + c, h[3] + d, h[4] + e, h[5] + f, h[6] + g, h[7] + hh].map((x) => x >>> 0);
  }
  const out = [];
  for (const x of h) out.push((x >>> 24) & 0xff, (x >>> 16) & 0xff, (x >>> 8) & 0xff, x & 0xff);
  return out;
}

// ---- HMAC (RFC 2104) over the pure hashes -------------------------------------------------------
export function hmac(hashFn, blockSize, keyBytes, msgBytes) {
  let key = keyBytes.slice();
  if (key.length > blockSize) key = hashFn(key);
  while (key.length < blockSize) key.push(0);
  const oKey = key.map((b) => b ^ 0x5c);
  const iKey = key.map((b) => b ^ 0x36);
  return hashFn(oKey.concat(hashFn(iKey.concat(msgBytes))));
}
const HASHERS = {
  SHA1: { fn: sha1, block: 64 },
  SHA256: { fn: sha256, block: 64 },
};

// ---- Base32 (RFC 4648) decode, tolerant of spaces / lowercase / missing padding ----------------
const B32 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567';
export function base32Decode(s) {
  const clean = String(s || '').toUpperCase().replace(/[\s-]/g, '').replace(/=+$/, '');
  let bits = 0, value = 0; const out = [];
  for (const ch of clean) {
    const idx = B32.indexOf(ch);
    if (idx < 0) throw new Error('bad base32 char: ' + ch);
    value = (value << 5) | idx; bits += 5;
    if (bits >= 8) { out.push((value >>> (bits - 8)) & 0xff); bits -= 8; }
  }
  return out;
}
export function isValidBase32(s) { try { const b = base32Decode(s); return b.length > 0; } catch { return false; } }

// ---- HOTP / TOTP --------------------------------------------------------------------------------
// counter -> 8-byte big-endian
function counterBytes(counter) {
  const b = new Array(8).fill(0);
  let c = counter;
  for (let i = 7; i >= 0; i--) { b[i] = c & 0xff; c = Math.floor(c / 256); }
  return b;
}
export function hotp(secretBytes, counter, { digits = 6, algorithm = 'SHA1' } = {}) {
  const h = HASHERS[String(algorithm).toUpperCase()] || HASHERS.SHA1;
  const hs = hmac(h.fn, h.block, secretBytes, counterBytes(counter));
  const offset = hs[hs.length - 1] & 0x0f;
  const bin = ((hs[offset] & 0x7f) << 24) | (hs[offset + 1] << 16) | (hs[offset + 2] << 8) | hs[offset + 3];
  return String(bin % (10 ** digits)).padStart(digits, '0');
}
// TOTP for a unix time in seconds (default: floor(now/period)).
export function totp(secretBase32, { digits = 6, period = 30, algorithm = 'SHA1', now = Date.now() } = {}) {
  const counter = Math.floor((now / 1000) / period);
  return hotp(base32Decode(secretBase32), counter, { digits, algorithm });
}
// Seconds until the current TOTP window rolls over.
export function secondsRemaining(period = 30, now = Date.now()) {
  return period - (Math.floor(now / 1000) % period);
}

// ---- otpauth:// URI parsing (what a QR from Google/GitHub/… encodes) ----------------------------
// otpauth://TYPE/LABEL?secret=…&issuer=…&digits=6&period=30&algorithm=SHA1&counter=0
export function parseOtpauth(uri) {
  let u;
  try { u = new URL(String(uri).trim()); } catch { return null; }
  if (u.protocol !== 'otpauth:') return null;
  const type = (u.host || '').toLowerCase();
  if (type !== 'totp' && type !== 'hotp') return null;
  const q = u.searchParams;
  const secret = (q.get('secret') || '').replace(/\s/g, '');
  if (!secret || !isValidBase32(secret)) return null;
  const rawLabel = decodeURIComponent((u.pathname || '').replace(/^\//, ''));
  let issuer = q.get('issuer') || '', account = rawLabel;
  if (rawLabel.includes(':')) { const [i, ...rest] = rawLabel.split(':'); if (!issuer) issuer = i.trim(); account = rest.join(':').trim(); }
  return {
    type, secret, issuer: issuer.trim(), account: account.trim(),
    digits: parseInt(q.get('digits'), 10) === 8 ? 8 : 6,
    period: Math.max(5, parseInt(q.get('period'), 10) || 30),
    algorithm: /256/.test(q.get('algorithm') || '') ? 'SHA256' : 'SHA1',
    counter: parseInt(q.get('counter'), 10) || 0,
  };
}

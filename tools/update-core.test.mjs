// update-core.test.mjs — unit gate for the release update-check stack (shared shell/Settings code).
//   node --test tools/update-core.test.mjs     (also picked up by `npm run test:unit`)
// Covers: PROJECT_VER/tag parsing, the semver-triplet compare, the check cadence, the notify
// decision, the conditional GitHub fetch (fake fetch — ETag/304/errors), SHA256SUMS parsing, and
// the pure-JS SHA-256 against the FIPS vectors plus node:crypto on random lengths.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';

import {
  parseSemver, cmpSemver, checkDue, decideNotify, fetchLatestRelease, parseSha256Sums,
  UPDATE_API_LATEST,
} from '../web/shell/update-core.js';
import { sha256Hex } from '../web/shell/sha256.js';

// ---- version parsing -------------------------------------------------------------------------

test('parseSemver: firmware PROJECT_VER and release tags', () => {
  assert.deepEqual(parseSemver('0.2.11+17.gabc1234*'), { maj: 0, min: 2, pat: 11 });  // dirty dev build
  assert.deepEqual(parseSemver('v0.2.11'), { maj: 0, min: 2, pat: 11 });              // plain tag
  assert.deepEqual(parseSemver('v0.3.0.5'), { maj: 0, min: 3, pat: 0 });              // 4-part tag → triplet
  assert.deepEqual(parseSemver('1.0.0+3.gnogit'), { maj: 1, min: 0, pat: 0 });        // no-git build
  assert.equal(parseSemver('v1.2'), null);       // incomplete triplet
  assert.equal(parseSemver(''), null);
  assert.equal(parseSemver('?'), null);          // firmware fallback when app desc is missing
  assert.equal(parseSemver(null), null);
});

test('cmpSemver: triplet ordering, build metadata ignored', () => {
  assert.equal(cmpSemver('0.2.11+17.gabc', 'v0.2.11'), 0);    // same release, rebuilt → never nag
  assert.equal(cmpSemver('0.2.11', 'v0.3.0'), -1);
  assert.equal(cmpSemver('0.3.0', 'v0.2.11'), 1);
  assert.equal(cmpSemver('0.2.9', 'v0.2.11'), -1);            // numeric, not lexicographic (9 < 11)
  assert.equal(cmpSemver('1.0.0', 'v0.9.9'), 1);
  assert.equal(cmpSemver('garbage', 'v0.3.0'), 0);            // unparsable → "equal" → no action
});

// ---- cadence ---------------------------------------------------------------------------------

test('checkDue: no cache, fresh cache, stale cache', () => {
  const now = 1_000_000_000_000;
  assert.equal(checkDue({ cache: null, nowMs: now }), true);
  assert.equal(checkDue({ cache: {}, nowMs: now }), true);
  assert.equal(checkDue({ cache: { checkedAt: now - 1000 }, nowMs: now, ttlMs: 3600_000 }), false);
  assert.equal(checkDue({ cache: { checkedAt: now - 3600_001 }, nowMs: now, ttlMs: 3600_000 }), true);
});

// ---- notify decision -------------------------------------------------------------------------

test('decideNotify: newer/seen/equal/dev-build/garbage', () => {
  const base = { currentVer: '0.2.11+17.gabc', latestTag: 'v0.3.0' };
  assert.deepEqual(decideNotify({ ...base, notifiedTags: [] }), { notify: true, newer: true });
  assert.deepEqual(decideNotify({ ...base, notifiedTags: ['v0.3.0'] }), { notify: false, newer: true });
  assert.equal(decideNotify({ currentVer: '0.3.0+1.gdef', latestTag: 'v0.3.0', notifiedTags: [] }).notify, false);
  assert.equal(decideNotify({ currentVer: '0.4.0+1.gdef', latestTag: 'v0.3.0', notifiedTags: [] }).notify, false);  // dev ahead
  assert.equal(decideNotify({ currentVer: '?', latestTag: 'v0.3.0', notifiedTags: [] }).notify, false);
  assert.equal(decideNotify({ currentVer: '0.2.11', latestTag: 'weird', notifiedTags: [] }).notify, false);
});

// ---- conditional GitHub fetch ----------------------------------------------------------------

function fakeResponse({ status = 200, json = null, etag = '' }) {
  return {
    ok: status >= 200 && status < 300, status,
    headers: { get: (k) => (k.toLowerCase() === 'etag' ? etag : null) },
    json: async () => { if (json === 'THROW') throw new Error('bad json'); return json; },
  };
}

test('fetchLatestRelease: 200 fills the cache and remembers the ETag', async () => {
  const calls = [];
  const fetchImpl = async (url, opts) => { calls.push({ url, opts }); return fakeResponse({ status: 200, etag: 'W/"e1"', json: { tag_name: 'v0.3.0', name: 'v0.3.0 — big one', html_url: 'https://x/rel', body: 'notes'.repeat(2000) } }); };
  const r = await fetchLatestRelease(fetchImpl, null, 123);
  assert.equal(r.ok, true);
  assert.equal(r.cache.tag, 'v0.3.0');
  assert.equal(r.cache.etag, 'W/"e1"');
  assert.equal(r.cache.checkedAt, 123);
  assert.ok(r.cache.notes.length <= 4000);                      // capped
  assert.equal(calls[0].url, UPDATE_API_LATEST);
  assert.equal(calls[0].opts.headers['If-None-Match'], undefined);

  // second call sends If-None-Match; 304 keeps the payload and only bumps checkedAt
  const fetch304 = async (url, opts) => { calls.push({ url, opts }); return fakeResponse({ status: 304 }); };
  const r2 = await fetchLatestRelease(fetch304, r.cache, 456);
  assert.equal(r2.ok, true);
  assert.equal(r2.cache.tag, 'v0.3.0');
  assert.equal(r2.cache.checkedAt, 456);
  assert.equal(calls[1].opts.headers['If-None-Match'], 'W/"e1"');
});

test('fetchLatestRelease: failures keep the old cache and report !ok', async () => {
  const cache = { tag: 'v0.2.11', etag: 'W/"old"', checkedAt: 1 };
  for (const impl of [
    async () => fakeResponse({ status: 500 }),
    async () => fakeResponse({ status: 403 }),                       // rate-limited
    async () => { throw new Error('offline'); },
    async () => fakeResponse({ status: 200, json: 'THROW' }),
    async () => fakeResponse({ status: 200, json: { nope: 1 } }),    // no tag_name
  ]) {
    const r = await fetchLatestRelease(impl, cache, 999);
    assert.equal(r.ok, false);
    assert.equal(r.cache, cache);                                    // untouched
  }
});

// ---- SHA256SUMS ------------------------------------------------------------------------------

test('parseSha256Sums: sha256sum output, CRLF, binary marker, junk lines', () => {
  const hex = 'a'.repeat(64);
  const m = parseSha256Sums(`${hex}  nucleoos-latest-ota.bin\r\n${'B'.repeat(64)} *nucleoos-latest.bin\nnot a line\n\n`);
  assert.equal(m.get('nucleoos-latest-ota.bin'), hex);
  assert.equal(m.get('nucleoos-latest.bin'), 'b'.repeat(64));        // lowercased
  assert.equal(m.size, 2);
});

// ---- SHA-256 ---------------------------------------------------------------------------------

test('sha256Hex: FIPS 180-4 vectors', () => {
  const enc = (s) => new TextEncoder().encode(s);
  assert.equal(sha256Hex(enc('')), 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
  assert.equal(sha256Hex(enc('abc')), 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  assert.equal(sha256Hex(enc('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq')),
    '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1');
  assert.equal(sha256Hex(new Uint8Array(1_000_000).fill(0x61)),      // one million 'a'
    'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0');
});

test('sha256Hex: matches node:crypto across padding boundaries and random buffers', () => {
  // Every tail-length regime: rem < 56 (one pad block) and rem >= 56 (two), plus exact blocks.
  for (const len of [0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 1000]) {
    const buf = new Uint8Array(len);
    for (let i = 0; i < len; i++) buf[i] = (i * 131 + 7) & 0xff;
    const ref = createHash('sha256').update(buf).digest('hex');
    assert.equal(sha256Hex(buf), ref, `len=${len}`);
  }
  // A firmware-image-shaped buffer (starts with the ESP magic, a few hundred KB).
  const img = new Uint8Array(300_000);
  img[0] = 0xe9;
  for (let i = 1; i < img.length; i++) img[i] = (i ^ (i >> 8)) & 0xff;
  assert.equal(sha256Hex(img), createHash('sha256').update(img).digest('hex'));
});

// Gate: ANIMA Forge — tamper-evident provenance ledger for applied artifacts (hash-chain + anchor).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { append, verify, canonical, head, sha256hex } from '../../apps/anima/www/forge/provenance.js';

const entry = (path, sha, ts) => ({ path, substrate: 'M4-local', model: 'qwen2.5-coder-1.5b', revision: 'deadbeef', verdict: 'pass', contentSha: sha, approver: 'human', ts });

async function chain3() {
  let c = [];
  c = await append(c, entry('a.js', 'aa', 1));
  c = await append(c, entry('b.js', 'bb', 2));
  c = await append(c, entry('c.js', 'cc', 3));
  return c;
}

test('append builds a linked chain that verifies', async () => {
  const c = await chain3();
  assert.equal(c.length, 3);
  assert.equal(c[0].prevHash, 'GENESIS');
  assert.equal(c[1].prevHash, c[0].hash);
  assert.equal((await verify(c)).ok, true);
});

test('mutating any record breaks verification at that index', async () => {
  const c = await chain3();
  c[1] = { ...c[1], verdict: 'veto' };                 // tamper without recomputing hash
  const v = await verify(c);
  assert.equal(v.ok, false);
  assert.equal(v.brokenAt, 1);
});

test('forge+reseal is internally consistent but DEFEATED by the off-chain anchor', async () => {
  const good = await chain3();
  const anchor = head(good);                            // the genuine head, stored off-chain
  // attacker rewrites record 1 and reseals the chain from there
  let forged = good.slice(0, 1);
  forged = await append(forged, entry('b.js', 'EVIL', 2));
  forged = await append(forged, entry('c.js', 'cc', 3));
  assert.equal((await verify(forged)).ok, true);         // self-consistent…
  assert.equal((await verify(forged, { anchor })).ok, false);  // …but the anchor catches the reseal
});

test('canonical hashing is key-order independent', async () => {
  assert.equal(canonical({ b: 1, a: 2 }), canonical({ a: 2, b: 1 }));
  assert.match(await sha256hex('x'), /^[0-9a-f]{64}$/);
});

// tools/lib/sd-provenance.mjs — the content record push-ota --sync writes after a clean full sync.
// Its effect on the firmware's verdict (COMPLETE / OUTDATED) is proven against the REAL sdc_engine.c by
// tools/anima-host/sdcontent-e2e.mjs (case S21); this pins the JS side.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { CONTENT_JSON, syncRecord, readRecord, recordCurrent, mayRecord } from './lib/sd-provenance.mjs';

test('the record uses the exact spelling the firmware substring-matches, well under its 192-byte read', () => {
  const r = syncRecord('0.7.2');
  assert.equal(CONTENT_JSON, 'system/content.json');
  assert.ok(r.includes('"tag":"0.7.2"') && r.includes('"complete":true') && r.includes('"declined":false'));
  assert.ok(Buffer.byteLength(r) < 120);
  assert.deepEqual(JSON.parse(r), { tag: '0.7.2', complete: true, declined: false, by: 'dev-sync' });
  assert.equal(syncRecord('v0.7.2'), r, 'a leading v is dropped');
});

test('only a release version can be recorded', () => {
  for (const bad of ['', '0.7', '0.7.2+1.gabc', 'dev', null]) assert.throws(() => syncRecord(bad));
});

test('the repository VERSION is recordable', () => {
  const v = readFileSync(new URL('../firmware/version/VERSION', import.meta.url), 'utf8').trim();
  assert.doesNotThrow(() => syncRecord(v));
});

test('readRecord reads like sdc_content_read', () => {
  assert.equal(readRecord(null), null);
  assert.deepEqual(readRecord('{"tag":"0.5.0","complete":true,"declined":false}\n'), { tag: '0.5.0', complete: true, declined: false });
  assert.deepEqual(readRecord('{"tag":"0.7.2","complete":false,"declined":true}'), { tag: '0.7.2', complete: false, declined: true });
  // the firmware matches "complete":true without spaces — a spaced record is NOT complete for it either
  assert.equal(readRecord('{"tag":"0.7.2", "complete": true}').complete, false);
});

test('recordCurrent: leave a matching complete record alone, rewrite anything else', () => {
  assert.equal(recordCurrent('{"tag":"0.7.2","complete":true,"declined":false}', '0.7.2'), true, 'engine-written record kept');
  assert.equal(recordCurrent(syncRecord('0.7.2'), '0.7.2'), true);
  assert.equal(recordCurrent('{"tag":"0.5.0","complete":true,"declined":false}', '0.7.2'), false, 'the stale ADV case');
  assert.equal(recordCurrent('{"tag":"0.7.2","complete":false,"declined":false}', '0.7.2'), false, 'partial run');
  assert.equal(recordCurrent('{"tag":"0.7.2","complete":true,"declined":true}', '0.7.2'), false);
  assert.equal(recordCurrent(null, '0.7.2'), false, 'no record');
});

test('mayRecord: only a full, clean, real run vouches for the card', () => {
  assert.equal(mayRecord({}), true);
  assert.equal(mayRecord({ dryRun: true }), false);
  assert.equal(mayRecord({ only: 'www' }), false);
  assert.equal(mayRecord({ exclude: ['data/anima'] }), false);
  assert.equal(mayRecord({ failed: 1 }), false);
  assert.equal(mayRecord({ unknown: 2 }), false);
});

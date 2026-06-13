// Gate: ANIMA Forge — MCU-aware coexistence scheduler (must-fix #3, principled). Never let an SD
// model pull starve the verifier or trip the device heap breaker.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { decide, windowFor, HEAP_FLOOR, HEAP_TIGHT, MIN_WINDOW, MAX_WINDOW } from '../../apps/anima/www/forge/scheduler.js';

const healthy = { freeHeap: 120 * 1024, largestBlock: 300 * 1024, verifyInFlight: false };

test('CDN pull always proceeds (device is bypassed for the heavy fetch)', () => {
  const d = decide({ freeHeap: 1, largestBlock: 1 }, { op: 'model-pull', source: 'cdn' });
  assert.equal(d.action, 'go');
});

test('verifier traffic is privileged and always proceeds', () => {
  assert.equal(decide({ largestBlock: 1 }, { op: 'verify' }).action, 'go');
});

test('SD pull pauses while a verifier request is in flight (never concurrent)', () => {
  assert.equal(decide({ ...healthy, verifyInFlight: true }, { op: 'model-pull', source: 'sd' }).reason, 'verify-in-flight');
});

test('SD pull pauses below the heap-floor breaker and throttles when heap is tight', () => {
  assert.equal(decide({ largestBlock: HEAP_FLOOR - 1, freeHeap: 200 * 1024 }, { op: 'model-pull', source: 'sd' }).action, 'pause');
  const tight = decide({ largestBlock: 200 * 1024, freeHeap: HEAP_TIGHT - 1 }, { op: 'model-pull', source: 'sd' });
  assert.equal(tight.action, 'throttle');
  assert.equal(tight.window, MIN_WINDOW);
});

test('healthy SD pull proceeds serially with a window sized by the free block', () => {
  const d = decide(healthy, { op: 'model-pull', source: 'sd' });
  assert.equal(d.action, 'go');
  assert.equal(d.concurrency, 1);
  assert.equal(d.window, MAX_WINDOW);
  assert.equal(windowFor({ largestBlock: 64 * 1024 }), MIN_WINDOW);
});

// Unit test for the shell device-busy indicator controller (web/shell/busy.js). Drives the debounce
// state machine with a MANUAL scheduler (no real timers, no DOM) so the anti-flicker behaviour is
// deterministic: show is immediate, hide is deferred and coalesces a burst of short heavy jobs into
// one steady "busy". This is the "logic tested" half; the end-to-end (a real system.busy event from
// the flashed device) is confirmed on-device.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createBusyController } from '../web/shell/busy.js';

// A manual scheduler: schedule() records the pending callback; tick() fires it. Lets the test
// "advance time" explicitly so there is zero timing flakiness.
function harness(debounceMs = 600) {
  const shown = [];                 // labels passed to show()
  let hides = 0;
  let pending = null;               // the single deferred-hide callback
  const ctl = createBusyController({
    show: (job) => shown.push(job),
    hide: () => { hides++; },
    debounceMs,
    schedule: (fn) => { pending = fn; return 1; },
    cancel: () => { pending = null; },
  });
  return {
    ctl, shown,
    hides: () => hides,
    tick: () => { if (pending) { const f = pending; pending = null; f(); } },
    hasPending: () => pending !== null,
  };
}

test('busy=true shows immediately with the job label', () => {
  const h = harness();
  h.ctl.onEvent(true, 'proxy');
  assert.equal(h.ctl.state().visible, true);
  assert.deepEqual(h.shown, ['proxy']);
  assert.equal(h.hides(), 0);
});

test('busy=false defers the hide (debounce), then hides on tick', () => {
  const h = harness();
  h.ctl.onEvent(true, 'llm');
  h.ctl.onEvent(false);
  assert.equal(h.ctl.state().visible, true, 'still visible while hide is pending');
  assert.equal(h.ctl.state().pendingHide, true);
  assert.equal(h.hides(), 0);
  h.tick();
  assert.equal(h.ctl.state().visible, false, 'hidden after the debounce fires');
  assert.equal(h.hides(), 1);
});

test('a burst of short jobs does NOT flicker (busy cancels the pending hide)', () => {
  const h = harness();
  h.ctl.onEvent(true, 'anima-get');
  h.ctl.onEvent(false);                 // job ends → hide scheduled
  assert.equal(h.hasPending(), true);
  h.ctl.onEvent(true, 'anima-post');    // next job starts before the debounce → cancel the hide
  assert.equal(h.hasPending(), false, 'pending hide was cancelled');
  assert.equal(h.ctl.state().visible, true);
  h.tick();                             // the stale timer (if any) must be a no-op now
  assert.equal(h.ctl.state().visible, true, 'still visible: no flicker');
  assert.equal(h.hides(), 0);
});

test('label refreshes to the latest job while staying visible', () => {
  const h = harness();
  h.ctl.onEvent(true, 'proxy');
  h.ctl.onEvent(true, 'transcribe');
  assert.deepEqual(h.shown, ['proxy', 'transcribe']);
  assert.equal(h.ctl.state().visible, true);
});

test('busy=false while already hidden is a no-op (no spurious hide, no timer)', () => {
  const h = harness();
  h.ctl.onEvent(false);
  assert.equal(h.ctl.state().visible, false);
  assert.equal(h.hasPending(), false);
  assert.equal(h.hides(), 0);
});

test('repeated busy=false does not schedule a second hide', () => {
  const h = harness();
  h.ctl.onEvent(true, 'proxy');
  h.ctl.onEvent(false);
  h.ctl.onEvent(false);                 // second false must not stack another timer
  assert.equal(h.hasPending(), true);
  h.tick();
  assert.equal(h.hides(), 1, 'exactly one hide');
  assert.equal(h.hasPending(), false);
});

test('missing/non-string job falls back to empty label (no undefined in UI)', () => {
  const h = harness();
  h.ctl.onEvent(true);
  assert.deepEqual(h.shown, ['']);
});

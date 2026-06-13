// Device-busy indicator controller — PURE logic (the DOM glue lives in shell.js::renderBusy).
//
// The firmware's heavy-work arbiter publishes a `system.busy` {busy, job} event every time it takes
// or releases its single token (one TLS/SD/heavy job at a time on the PSRAM-less device). For short
// TLS fetches that edge can flap on→off in tens of milliseconds, so naively toggling the tray
// indicator would make it FLICKER. This controller shows immediately on busy=true but DEFERS hiding
// by `debounceMs`, cancelling the pending hide if another busy arrives — so a burst of short jobs
// reads as one steady "busy", and only a genuine idle gap clears it.
//
// Pure + injectable scheduler ⇒ unit-testable with no DOM and no real timers (see the manual
// scheduler in tools/shell-busy.test.mjs). debounceMs is the knob to tune once the real device's
// busy cadence is observed (short proxy fetches vs long model pulls).
export function createBusyController({
  show, hide, debounceMs = 600,
  schedule = (fn, ms) => setTimeout(fn, ms),
  cancel = (h) => clearTimeout(h),
} = {}) {
  let visible = false;
  let timer = null;

  const clearTimer = () => { if (timer !== null) { cancel(timer); timer = null; } };

  function onEvent(busy, job) {
    if (busy) {
      clearTimer();                              // fresh heavy job: cancel any pending hide, stay shown
      visible = true;
      // refresh the label every time (the job may have changed); cheap and idempotent
      show(typeof job === 'string' ? job : '');
    } else {
      if (!visible || timer !== null) return;    // already hidden, or a hide is already pending
      timer = schedule(() => { timer = null; visible = false; hide(); }, debounceMs);
    }
  }

  return { onEvent, state: () => ({ visible, pendingHide: timer !== null }) };
}

// device-queue-check.mjs — DETERMINISTIC verification of the unified device queue (apps/agent/www/
// device-queue.js): light reads pool+space, heavy ops run EXCLUSIVE (alone), FIFO so a heavy op is never
// starved, values/errors propagate. This is the discipline that keeps the PSRAM-less Cardputer from being
// hit by concurrent requests, so it is locked by a host test.
//   node tools/anima-host/device-queue-check.mjs
import { createDeviceQueue } from '../../apps/agent/www/device-queue.js';

let pass = 0, fail = 0; const fails = [];
const ok = (name, cond, detail) => { if (cond) { pass++; } else { fail++; fails.push(name + (detail ? ' — ' + detail : '')); } };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* ---- A. exclusivity + pool cap under a concurrent burst ---- */
let curLight = 0, curHeavy = 0, maxLight = 0, heavyViol = 0;
function opStart(heavy) {
  if (heavy) { if (curLight + curHeavy > 0) heavyViol++; curHeavy++; }
  else { if (curHeavy > 0) heavyViol++; curLight++; if (curLight > maxLight) maxLight = curLight; }
}
function opEnd(heavy) { if (heavy) curHeavy--; else curLight--; }

await (async () => {
  const dq = createDeviceQueue({ maxConcurrent: 2, minGapMs: 0 });
  const mk = (heavy, ms) => dq.run(async () => { opStart(heavy); await sleep(ms); opEnd(heavy); return heavy ? 'H' : 'L'; }, { heavy });
  const res = await Promise.all([mk(false, 15), mk(false, 15), mk(false, 15), mk(true, 12), mk(false, 15), mk(true, 12), mk(false, 15)]);
  ok('heavy op never overlaps another op', heavyViol === 0, 'violations=' + heavyViol);
  ok('light pool capped at maxConcurrent (2)', maxLight <= 2, 'maxLight=' + maxLight);
  ok('all 7 ops completed', res.length === 7 && res.filter((x) => x === 'H').length === 2);
})();

/* ---- B. values and errors propagate ---- */
await (async () => {
  const dq = createDeviceQueue({ minGapMs: 0 });
  ok('read returns the value', (await dq.read(() => Promise.resolve(42))) === 42);
  let caught = false;
  try { await dq.write(() => { throw new Error('boom'); }); } catch (e) { caught = /boom/.test(e.message); }
  ok('error propagates to caller', caught);
  ok('queue recovers after an error', (await dq.read(() => Promise.resolve('ok'))) === 'ok');
})();

/* ---- C. FIFO: a heavy op holds the line (not starved by later light ops) ---- */
await (async () => {
  const dq = createDeviceQueue({ maxConcurrent: 2, minGapMs: 0 });
  const order = [];
  const a = dq.run(async () => { await sleep(30); order.push('L1'); }, { heavy: false }); // starts now
  const b = dq.run(async () => { order.push('H'); }, { heavy: true });                     // waits for idle, runs alone
  const c = dq.run(async () => { order.push('L2'); }, { heavy: false });                   // behind the heavy in FIFO
  await Promise.all([a, b, c]);
  ok('heavy waits for idle then precedes later light', order.join(',') === 'L1,H,L2', order.join(','));
})();

/* ---- D. spacing (minGapMs) staggers starts ---- */
await (async () => {
  const dq = createDeviceQueue({ maxConcurrent: 4, minGapMs: 30 });
  const t0 = Date.now();
  await Promise.all([dq.read(() => Promise.resolve()), dq.read(() => Promise.resolve()), dq.read(() => Promise.resolve())]);
  const dt = Date.now() - t0;
  ok('spacing staggers concurrent starts', dt >= 50, 'dt=' + dt + 'ms');
})();

/* ---- E. pending counter ---- */
await (async () => {
  const dq = createDeviceQueue({ maxConcurrent: 1, minGapMs: 0 });
  const p1 = dq.write(() => sleep(20));
  const p2 = dq.write(() => sleep(1));
  ok('pending reflects the backlog', dq.pending >= 1, 'pending=' + dq.pending);
  await Promise.all([p1, p2]);
  ok('pending drains to 0', dq.pending === 0);
})();

console.log(`\ndevice-queue-check: ${pass} passed, ${fail} failed`);
if (fail) { console.log('FAILED:\n - ' + fails.join('\n - ')); process.exit(1); }
console.log('all green ✓');

// device-queue.js — ONE intelligent queue for everything that touches the PSRAM-less Cardputer.
//
// The board has ~18 KB heap and 4 httpd sockets: a burst of concurrent requests starves it. So every
// call that hits the device (workspace fs ops, /api/status, /api/apps, the app-install writes, and the
// Gemini /api/llm proxy) funnels through here. Cloud calls that DON'T touch the device (Claude/Groq/Grok
// browser-direct, Open-Meteo) bypass it entirely — their parallelism is free and must not be throttled.
//
// Two classes, FIFO, no starvation:
//   • LIGHT (reads: list/read/search/tree/status/apps) — up to `maxConcurrent` at once, spaced by minGapMs.
//   • HEAVY (writes, system-registry writes, /api/llm proxy TLS) — EXCLUSIVE: a heavy op runs strictly
//     ALONE (it waits for the device to go idle, and while it runs nothing else starts). Once a heavy op
//     is at the head of the queue it holds the line, so a stream of reads can't starve it.
//
// This replaces the old split (a concurrency throttle for fs + a separate serial gate for the Gemini
// proxy) with a single coordinated discipline. The firmware arbiter (one-TLS-at-a-time) + the shell SW
// write-gate remain the cross-surface backstop; this is the in-agent half.
//
// Pure (no DOM/fetch): host-testable in tools/anima-host/device-queue-check.mjs.

export function createDeviceQueue({ maxConcurrent = 2, minGapMs = 70 } = {}) {
  const q = [];                 // pending: { fn, heavy, resolve, reject }
  let active = 0;               // ops currently running
  let heavyActive = false;      // a heavy (exclusive) op is running
  let lastStart = 0;            // reserved start time of the most recent admit (for spacing)
  const now = () => (typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now());

  function admit() {
    while (q.length) {
      if (heavyActive) return;                          // nothing runs alongside a heavy op
      const next = q[0];
      if (next.heavy) { if (active > 0) return; }        // heavy waits for full idle, then runs alone
      else if (active >= maxConcurrent) return;          // light respects the small pool
      q.shift();
      active++; if (next.heavy) heavyActive = true;
      const wait = Math.max(0, lastStart + minGapMs - now());
      lastStart = now() + wait;                          // reserve so concurrent admits stack their gaps
      const launch = () => Promise.resolve().then(next.fn).then(
        (v) => { release(next); next.resolve(v); },
        (e) => { release(next); next.reject(e); });
      if (wait > 0) setTimeout(launch, wait); else launch();
      if (next.heavy) return;                            // a heavy op blocks further admits until it ends
    }
  }
  function release(item) { active--; if (item.heavy) heavyActive = false; admit(); }

  function run(fn, { heavy = false } = {}) {
    return new Promise((resolve, reject) => { q.push({ fn, heavy, resolve, reject }); admit(); });
  }
  return {
    run,
    read: (fn) => run(fn, { heavy: false }),
    write: (fn) => run(fn, { heavy: true }),
    get pending() { return q.length; },
    stats: () => ({ active, heavyActive, queued: q.length }),
  };
}

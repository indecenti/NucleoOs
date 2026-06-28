// Resilient boot/data fetch for the desktop shell — EXTRACTED so it is unit-testable in node with a
// mock fetch (tools/shell-boot-fetch.test.mjs). The Cardputer serves the WHOLE OS from a SINGLE-TASK,
// 6-socket, PSRAM-less web server: when several requests land at once it answers 503 (busy / low-heap)
// or stalls. So each attempt gets a hard timeout (it must never hang the boot splash) and we RETRY
// 503/502/504 + timeouts + network errors with growing backoff — SPACING the load so the device
// reclaims heap between calls instead of being flooded. This is what lets the web OS attach + load
// reliably; a slower first boot is the accepted trade. A 4xx is deterministic (a retry can't fix it),
// so it fails fast. Dependencies (fetch / sleep / log / AbortSignal) are injected so the policy can be
// exercised deterministically in tests without real timers or a real network.

export function makeFetchJSON({ fetch, sleep, log = () => {}, AbortSignal: AS } = {}) {
  if (typeof fetch !== 'function') throw new Error('makeFetchJSON: fetch dependency required');
  if (typeof sleep !== 'function') throw new Error('makeFetchJSON: sleep dependency required');
  const Abort = AS || (typeof AbortSignal !== 'undefined' ? AbortSignal : null);

  return async function fetchJSON(path, { tries = 5, timeout = 6000 } = {}) {
    let lastErr;
    for (let i = 0; i < tries; i++) {
      const last = i === tries - 1;
      let r;
      try {
        const opts = { cache: 'no-store' };
        if (Abort && Abort.timeout) opts.signal = Abort.timeout(timeout);   // fail fast, never hang the splash
        r = await fetch(path, opts);
      } catch (e) {                                       // network error / timeout / abort → retryable
        lastErr = e;
        log('fail', path, '(' + ((e && (e.name || e.message)) || e) + ')', 'retry', (i + 1) + '/' + tries);
        if (!last) await sleep(300 * (i + 1));
        continue;
      }
      if (r.status === 503 || r.status === 502 || r.status === 504) {   // device busy / low-heap → retryable
        const ra = parseFloat(r.headers && r.headers.get && r.headers.get('Retry-After')) || 0;
        const wait = ra ? ra * 1000 : 300 * (i + 1);     // honour Retry-After when the device sends it
        log('busy', r.status, path, 'retry', (i + 1) + '/' + tries, 'in', wait + 'ms');
        if (!last) await sleep(wait);
        continue;
      }
      if (!r.ok) throw new Error('HTTP ' + r.status);     // 4xx → deterministic, fail fast (no retry)
      if (i > 0) log('ok', path, 'after', i, 'retr' + (i === 1 ? 'y' : 'ies'));
      return r.json();
    }
    log('GAVE UP', path, 'after', tries, 'tries');
    throw lastErr || new Error('exhausted retries: ' + path);
  };
}

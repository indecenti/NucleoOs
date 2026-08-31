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
      // A body we cannot parse is NOT a transport problem, and the difference matters upstream: the
      // device answers a 0-byte file with 200 and an EMPTY body, and Response.json() always rejects
      // on that. Tagging it lets loadState re-seed a corrupt file instead of concluding the device is
      // unreachable — which would put the store read-only and make the corrupt file unfixable.
      try { return await r.json(); }
      catch (e) { e.parseFailure = true; throw e; }
    }
    log('GAVE UP', path, 'after', tries, 'tries');
    throw lastErr || new Error('exhausted retries: ' + path);
  };
}

// Load a small piece of USER STATE (desktop layout, window session, clipboard history) with a TYPED
// outcome, because the caller must treat "the device has nothing saved yet" and "I could not reach the
// device" completely differently. Conflating them is a DATA-LOSS bug, and it was a real one: the shell
// read each store with a bare un-retried fetch, took ANY failure as "first run", seeded defaults — and
// then WROTE THOSE DEFAULTS BACK over the user's real state. One 503 from a 4-socket, PSRAM-less device
// during boot was enough to replace a desktop layout with the factory one, permanently.
//
// The distinction is clean because of how the firmware answers (nucleo_fsapi.c, read_get): a MISSING
// /system/config/*.json is deliberately served as 200 with "{}" (not 404) so a fresh SD doesn't spam the
// console. So:
//   { state:'found',       data }   real saved state came back
//   { state:'absent'  }             the device answered, and there is genuinely nothing yet → seed + save
//   { state:'unreachable', error }  network error / busy / 4xx after retries → the store goes READ-ONLY
// "Unreachable" must NEVER lead to a write: better a session that cannot save than one that destroys.
export function makeLoadState(fetchJSON) {
  if (typeof fetchJSON !== 'function') throw new Error('makeLoadState: fetchJSON dependency required');
  return async function loadState(path, { tries = 4, timeout = 5000 } = {}) {
    let data;
    try {
      data = await fetchJSON('/api/fs/read?path=' + encodeURIComponent(path), { tries, timeout });
    } catch (e) {
      // Corrupt/empty file → treat as "nothing usable saved", exactly as the pre-loadState code did
      // (it caught JSON.parse and re-seeded). Calling it unreachable froze the store FOREVER: writes
      // were blocked, so the unreadable file could never be replaced, on every boot.
      if (e && e.parseFailure) return { state: 'absent', corrupt: true };
      return { state: 'unreachable', error: String((e && e.message) || e) };
    }
    // A non-object (or a null) is not state we can merge — treat it as nothing saved rather than
    // spreading garbage over the defaults.
    if (!data || typeof data !== 'object' || Array.isArray(data)) return { state: 'absent' };
    if (Object.keys(data).length === 0) return { state: 'absent' };   // the firmware's "{}" for a fresh SD
    return { state: 'found', data };
  };
}

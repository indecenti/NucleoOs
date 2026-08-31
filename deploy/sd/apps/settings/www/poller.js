// poller.js — the ONE adaptive live-data poller for the Settings app.
//
// The Cardputer is a single-task, no-PSRAM HTTP server: concurrency is the enemy. So every live
// tile in the Control Center is fed by exactly ONE poller that:
//   • keeps at most one request in flight (overlapping ticks are dropped),
//   • round-robins its sources so two heavy reads never overlap,
//   • backs off geometrically when readings are calm and tightens when a value crosses a threshold,
//   • pauses entirely when the page is hidden,
//   • can be frozen by the user,
//   • backs off N ticks on a 503 / arbiter-busy instead of retry-storming.
//
// It is pure timing logic: `fetchJson`, `hidden`, `now` and the change/threshold predicates are all
// injected, so the whole control loop is unit-testable with a fake clock and fake fetch. `start()`
// /`stop()` add a real `setTimeout` driver for the browser.

export function createPoller(opts = {}) {
  const {
    fetchJson,                       // async (source) => parsed data | { __status:number }
    hidden = () => false,            // () => bool  (document.hidden)
    sources = ['status', 'heap', 'cpu'],
    minMs = 2000, idleMs = 3000, maxMs = 10000,
    backoff = 1.5,
    skipOnBusy = 3,                  // ticks to skip after a 503 / busy
    changed = () => true,            // (source, prev, next) => bool  (true ⇒ value moved)
    crossedThreshold = () => false,  // (source, prev, next) => bool  (true ⇒ entered a warn/bad band)
    onSample = () => {},             // (source, data) => void
    onError = () => {},              // (err) => void
  } = opts;

  let inFlight = false, frozen = false, interval = idleMs, skipTicks = 0, rr = 0;
  const last = {};
  let timer = null;

  function isBusy(d) {
    return !!d && (d.__status === 503 || (d.arbiter && d.arbiter.busy === true));
  }

  // Run one read. Returns a small record describing what happened (handy for tests).
  async function tick() {
    if (frozen)     return { skipped: 'frozen' };
    if (hidden())   return { skipped: 'hidden' };
    if (inFlight)   return { skipped: 'inflight' };
    if (skipTicks > 0) { skipTicks--; return { skipped: 'busy-backoff' }; }

    const source = sources[rr % sources.length];
    rr++;
    inFlight = true;
    try {
      const data = await fetchJson(source);
      const prev = last[source];
      last[source] = data;

      if (isBusy(data)) {
        skipTicks = skipOnBusy;
        interval = Math.min(maxMs, Math.round(interval * backoff));
      } else if (crossedThreshold(source, prev, data)) {
        interval = minMs;                                   // something entered a warn/bad band → watch closely
      } else if (!changed(source, prev, data)) {
        interval = Math.min(maxMs, Math.round(interval * backoff));  // calm → widen
      } else {
        interval = idleMs;                                  // moving but fine → baseline
      }

      onSample(source, data);
      return { source, data, interval, busy: isBusy(data) };
    } catch (e) {
      onError(e);
      interval = Math.min(maxMs, Math.round(interval * backoff));   // unreachable → ease off, don't hammer
      return { error: e, interval };
    } finally {
      inFlight = false;
    }
  }

  // `gen` guards the async gap in the loop: stop() during an in-flight tick would otherwise let the
  // trailing `timer = setTimeout(...)` re-arm a "stopped" poller (and a start() in that same gap
  // would spawn a second loop, since `timer` is briefly null).
  let gen = 0;
  function start() {
    if (timer) return;
    const g = ++gen;
    const loop = async () => { await tick(); if (g !== gen) return; timer = setTimeout(loop, interval); };
    loop();
  }
  function stop() { gen++; if (timer) { clearTimeout(timer); timer = null; } }

  // One sequential sweep over ALL sources (one at a time, awaited) so every tile paints within the
  // first moments of the app instead of one-source-per-interval (~3 s each). Still one request in
  // flight, still respects frozen/hidden; ticks that get skipped don't consume a source.
  async function prime() {
    for (let i = 0; i < sources.length; i++) {
      const r = await tick();
      if (r && r.skipped) break;               // frozen/hidden/inflight → stop sweeping, the loop resumes later
    }
  }

  return {
    tick, start, stop, prime,
    setFrozen(v) { frozen = !!v; if (frozen) stop(); else if (!timer) start(); },
    refreshNow() { if (!frozen && !hidden()) return tick(); },
    get frozen() { return frozen; },
    get interval() { return interval; },
    get skipTicks() { return skipTicks; },
    get inFlight() { return inFlight; },
    last,
  };
}

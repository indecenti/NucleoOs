// download-manager.js — the runnable controller that ties download.js (manifest + bounded-Range
// FSM, idempotent SHA resume) to scheduler.js (MCU-aware coexistence) with INJECTED I/O. It owns
// the loop the pure reducers describe: for each pending shard it asks the scheduler whether it may
// proceed (and at what window/concurrency) BEFORE every request, issues only bounded Range fetches,
// verifies the assembled shard's SHA, and folds every outcome through download.reduce. Three rules
// are load-bearing: (1) never a whole-file GET — only planRanges windows; (2) an SD pull must back
// off (pause) the instant the scheduler says so (heap breaker / verifier in flight) and never
// proceed; (3) a shard whose cached SHA already matches the manifest is skipped (idempotent resume).
// All I/O — fetchRange, sha256, telemetry, onProgress, cache — is injected, so the whole controller
// runs deterministically host-side with in-memory fakes. Pure & DOM-free → host-testable.

import {
  verifyManifest, planRanges, rangeHeader,
  pendingShards, initState, reduce, maxConcurrency, DL, MIN_WINDOW,
} from './download.js';
import { decide } from './scheduler.js';

const MAX_SHARD_RETRIES = 3;   // bounded re-fetch attempts before declaring verify-fail

// makeDownloadManager({ manifest, source, fetchRange, sha256, telemetry, onProgress, cache })
//   manifest    : { model, revision, shards:[{name,bytes,sha256}] }      (verified before any I/O)
//   source      : 'cdn' | 'sd'                                            (SD is serial; CDN parallel)
//   fetchRange  : async (shardName, {start,end}) -> { ok, status, bytes } (the ONLY transport)
//   sha256      : async (bytes) -> hex                                    (injected hasher)
//   telemetry   : () -> { freeHeap, largestBlock, verifyInFlight }        (read fresh each request)
//   onProgress  : ({ phase, done, pending, shard, pct }) -> void          (optional)
//   cache       : { name -> sha256 }                                      (idempotent-resume map; read/written)
export function makeDownloadManager({ manifest, source = 'cdn', fetchRange, sha256, telemetry, onProgress, cache = {} } = {}) {
  if (typeof fetchRange !== 'function') throw new Error('fetchRange must be injected');
  if (typeof sha256 !== 'function') throw new Error('sha256 must be injected');
  const tel = typeof telemetry === 'function' ? telemetry : () => ({});
  const emit = typeof onProgress === 'function' ? onProgress : () => {};

  const vm = verifyManifest(manifest);
  const total = manifest && Array.isArray(manifest.shards) ? manifest.shards.length : 0;

  let state = vm.ok ? initState(manifest, { source, cached: cache }) : null;
  let pauseRequested = false;     // external pause() request, honoured between/within shards
  let running = false;

  const byName = vm.ok ? Object.fromEntries(manifest.shards.map((s) => [s.name, s])) : {};

  function progress(extra = {}) {
    const done = state.done.length;
    const pct = total ? Math.round((done / total) * 100) : 100;
    emit({ phase: state.phase, done, pending: state.pending.length, shard: null, pct, ...extra });
  }

  // Fetch every bounded Range of one shard at the given window, assemble the byte length, verify SHA.
  // Returns { ok:true, bytes } | { ok:false, status } (status 503 → caller backs off the window).
  async function fetchShard(shard, window, concurrency) {
    const ranges = planRanges(shard.bytes, window);
    // Hard invariant: no single planned range may cover the whole shard when the shard exceeds one
    // window — every request is bounded. (planRanges guarantees this; we assemble within it.)
    const max = Math.min(concurrency || 1, maxConcurrency(source));
    let next = 0;
    let got = 0;
    let failure = null;

    async function worker() {
      while (next < ranges.length && !failure) {
        const r = ranges[next++];
        const res = await fetchRange(shard.name, { start: r.start, end: r.end, header: rangeHeader(r) });
        if (!res || !res.ok) { failure = { status: res && res.status }; return; }
        got += (res.bytes != null ? res.bytes : (r.end - r.start + 1));
      }
    }

    const workers = [];
    for (let i = 0; i < Math.max(1, max); i++) workers.push(worker());
    await Promise.all(workers);

    if (failure) return { ok: false, status: failure.status };
    return { ok: true, bytes: got };
  }

  // Download + verify a single shard with bounded re-fetch on SHA mismatch. Honours the scheduler
  // BEFORE every request: 'pause' → bubble up paused (never proceed); 'throttle' → MIN_WINDOW serial.
  // Returns 'ok' | 'paused' | { fail:'verify' } | { fail:'fetch', status }.
  async function runShard(shard) {
    for (let attempt = 0; attempt < MAX_SHARD_RETRIES; attempt++) {
      if (pauseRequested) return 'paused';

      const plan = decide(tel(), { op: 'model-pull', source });
      if (plan.action === 'pause') return 'paused';

      // 'throttle' → MIN_WINDOW, serial (task contract). 'go' → scheduler's window, but never wider
      // than what the FSM has adapted to after prior resets (state.window shrinks via adaptiveWindow).
      const schedWindow = plan.action === 'throttle' ? MIN_WINDOW : plan.window;
      const window = Math.min(state.window, schedWindow || state.window);
      const concurrency = plan.action === 'throttle' ? 1 : (plan.concurrency || 1);

      progress({ shard: shard.name });
      const res = await fetchShard(shard, window, concurrency);

      if (!res.ok) {
        // Transport failure (e.g. 503 circuit breaker): shrink the window + back off via the FSM.
        state = reduce(state, { type: 'shard-fail', name: shard.name, status: res.status, error: 'fetch' });
        if (state.phase === DL.PAUSED) { pauseRequested = true; return 'paused'; }
        continue;   // retry within bounds at the now-smaller window
      }

      const digest = await sha256(res.bytes);
      if (digest === shard.sha256) {
        cache[shard.name] = shard.sha256;     // record for idempotent resume
        return 'ok';
      }
      // SHA mismatch → re-fetch (the loop) until retries exhausted, then verify-fail.
    }
    return { fail: 'verify' };
  }

  async function loop() {
    running = true;
    try {
      if (!vm.ok) {
        return { ok: false, phase: DL.ERROR, error: 'bad-manifest', errors: vm.errors };
      }

      // Idempotent resume: everything already cached → initState already parked us at READY.
      if (state.phase === DL.READY) { progress(); return result(); }

      state = reduce(state, { type: 'start' });
      progress();

      // Snapshot the pending shard objects (initState tracks names; we need bytes + sha256).
      const queue = pendingShards(manifest, cache);

      for (const shard of queue) {
        // Skip if a concurrent cache write (or a prior attempt) already satisfied this shard.
        if (cache[shard.name] === shard.sha256) {
          if (state.pending.includes(shard.name)) state = reduce(state, { type: 'shard-ok', name: shard.name });
          progress({ shard: shard.name });
          continue;
        }

        const outcome = await runShard(byName[shard.name] || shard);

        if (outcome === 'paused') {
          if (state.phase !== DL.PAUSED) state = reduce(state, { type: 'pause' });
          progress({ shard: shard.name });
          return result();   // never proceed past a pause
        }
        if (outcome === 'ok') {
          state = reduce(state, { type: 'shard-ok', name: shard.name });
          progress({ shard: shard.name });
          continue;
        }
        if (outcome && outcome.fail === 'verify') {
          state = reduce(state, { type: 'verify-fail', reason: 'sha-mismatch:' + shard.name });
          progress({ shard: shard.name });
          return result();
        }
      }

      // All pending shards downloaded → the FSM is in VERIFYING (per-shard SHA already checked).
      if (state.phase === DL.VERIFYING) state = reduce(state, { type: 'verify-ok' });
      progress();
      return result();
    } finally {
      running = false;
    }
  }

  function result() {
    return {
      ok: state.phase === DL.READY,
      phase: state.phase,
      done: state.done.slice(),
      pending: state.pending.slice(),
      window: state.window,
      errors: state.errors.slice(),
    };
  }

  return {
    async run() { return loop(); },
    pause() { pauseRequested = true; if (state && state.phase !== DL.READY && state.phase !== DL.ERROR) state = reduce(state, { type: 'pause' }); },
    resume() { pauseRequested = false; if (state && state.phase === DL.PAUSED) state = reduce(state, { type: 'resume' }); },
    state() { return state ? result() : { ok: false, phase: DL.ERROR, error: 'bad-manifest', errors: vm.errors }; },
  };
}

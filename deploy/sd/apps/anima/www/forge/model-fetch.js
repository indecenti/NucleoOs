// model-fetch.js — the REAL resumable model downloader for the ANIMA Model Manager. It is the runnable
// sibling of download-manager.js: same hard invariants, but it carries ACTUAL bytes, persists them to the
// Cardputer SD, and resumes across page reloads. All I/O is INJECTED, so the whole controller runs
// deterministically host-side with in-memory fakes (see tools/anima-host/forge-model-fetch.test.mjs).
//
// Five load-bearing rules:
//   (1) ONE download at a time across the whole tab — a module-level lock (the user's "un solo download
//       alla volta"). A second run() while one is active is refused with {error:'busy', active}.
//   (2) Shards ARE the 7 MB pieces (the SD chunk size / httpd reliability ceiling). Resume granularity is
//       therefore one shard: an interrupted pull never re-fetches a verified shard, and loses at most the
//       in-flight ≤7 MB ("non perde i mezzi download, riprende").
//   (3) Every shard is verified by SHA-256 over the REAL assembled bytes BEFORE it is trusted/persisted.
//       A mismatch re-fetches (bounded retries) — corruption can never land.
//   (4) Persistence is two-tier: a browser cache {name->sha256} (fast, survives reload) AND a device-side
//       index file on SD (ground truth for what is actually on disk). On resume the device index wins.
//   (5) The MCU-coexistence scheduler is consulted before every request: an SD pull pauses the instant the
//       device says so (heap breaker / verifier in flight) and never proceeds.
// Pure & DOM-free → host-testable. The concrete adapters (fetch/crypto/SD) live in model-io.js.

import {
  verifyManifest, planRanges, rangeHeader, pendingShards,
  DL, MIN_WINDOW, MAX_WINDOW, adaptiveWindow, maxConcurrency,
} from './download.js';
import { decide } from './scheduler.js';

const MAX_SHARD_RETRIES = 3;

// One global lock for the whole tab. Holds { model, revision } while a download runs, else null.
let LOCK = null;
export function activeDownload() { return LOCK; }
export function isBusy() { return LOCK !== null; }

// makeModelDownload(opts) → controller { run, pause, resume, cancel, state, progress }
//   manifest   : { model, revision, shards:[{name,bytes,sha256}], ... }            (verified before any I/O)
//   source     : 'cdn' | 'sd'                                                       (SD serial; CDN parallel)
//   fetchRange : async (shardName,{start,end,header}) -> { ok, status, chunk:Uint8Array }  (REAL bytes)
//   sha256     : async (Uint8Array) -> hex
//   writeShard : async (shardName, bytes:Uint8Array) -> { ok, error? }              (persist to SD, idempotent)
//   telemetry  : async () -> { freeHeap, largestBlock, verifyInFlight }             (read fresh per request)
//   store      : { loadCache(): {name:sha}|Promise, saveCache(cache), removeCache?() }  (cross-reload resume)
//   onProgress : ({ phase, done, total, pending, shard, pct, shardPct, reason }) -> void
export function makeModelDownload({
  manifest, source = 'cdn', fetchRange, sha256, writeShard,
  telemetry, store, onProgress,
} = {}) {
  if (typeof fetchRange !== 'function') throw new Error('fetchRange must be injected');
  if (typeof sha256 !== 'function') throw new Error('sha256 must be injected');
  if (typeof writeShard !== 'function') throw new Error('writeShard must be injected');
  const tel = typeof telemetry === 'function' ? telemetry : async () => ({});
  const emit = typeof onProgress === 'function' ? onProgress : () => {};
  const st = store || { loadCache: () => ({}), saveCache: () => {} };

  const vm = verifyManifest(manifest);
  const total = vm.ok ? manifest.shards.length : 0;
  const byName = vm.ok ? Object.fromEntries(manifest.shards.map((s) => [s.name, s])) : {};

  let cache = {};                 // {name -> sha256} of shards proven present
  let phase = vm.ok ? DL.IDLE : DL.ERROR;
  let window = MAX_WINDOW;
  let done = [];
  let pending = [];
  let errors = vm.ok ? [] : vm.errors.map((e) => ({ manifest: e }));
  let pauseRequested = false;
  let cancelRequested = false;
  let running = false;

  function recompute() {
    pending = pendingShards(manifest, cache).map((s) => s.name);
    done = manifest.shards.map((s) => s.name).filter((n) => cache[n] === byName[n].sha256);
  }

  function progress(extra = {}) {
    const pct = total ? Math.round((done.length / total) * 100) : 100;
    emit({ phase, done: done.length, total, pending: pending.length, shard: null, pct, ...extra });
  }

  // Fetch + assemble ALL bounded ranges of one shard into a single Uint8Array. The scheduler decision is
  // made once per shard in runShard (window + concurrency); workers only watch the pause/cancel flags. Each
  // worker CLAIMS its range index synchronously (no await between the bounds check and `next++`) so parallel
  // CDN workers can't race past the end. Returns { ok:true, bytes } | { ok:false, status } | { paused:true }.
  async function fetchShard(shard, win, concurrency) {
    const ranges = planRanges(shard.bytes, win);
    const buf = new Uint8Array(shard.bytes);
    const max = Math.min(concurrency || 1, maxConcurrency(source));
    let next = 0, failure = null, paused = false;

    async function worker() {
      while (!failure && !paused) {
        if (pauseRequested || cancelRequested) { paused = true; return; }
        const i = next++;
        if (i >= ranges.length) return;
        const r = ranges[i];
        const res = await fetchRange(shard.name, { start: r.start, end: r.end, header: rangeHeader(r) });
        if (!res || !res.ok) { failure = { status: res && res.status }; return; }
        const chunk = res.chunk || new Uint8Array(0);
        buf.set(chunk.subarray(0, Math.min(chunk.length, shard.bytes - r.start)), r.start);
      }
    }
    const workers = [];
    for (let k = 0; k < Math.max(1, max); k++) workers.push(worker());
    await Promise.all(workers);

    if (cancelRequested) return { paused: true };
    if (paused) return { paused: true };
    if (failure) return { ok: false, status: failure.status };
    return { ok: true, bytes: buf };
  }

  // Download + verify + persist one shard, with bounded re-fetch on SHA mismatch.
  // Returns 'ok' | 'paused' | { fail:'verify' } | { fail:'fetch', status } | { fail:'write', error }.
  async function runShard(shard) {
    for (let attempt = 0; attempt < MAX_SHARD_RETRIES; attempt++) {
      if (pauseRequested || cancelRequested) return 'paused';
      const plan = decide(await tel(), { op: 'model-pull', source });
      if (plan.action === 'pause') return 'paused';
      const win = Math.min(window, plan.action === 'throttle' ? MIN_WINDOW : (plan.window || window));
      const concurrency = plan.action === 'throttle' ? 1 : (plan.concurrency || 1);

      progress({ shard: shard.name, reason: plan.reason });
      const res = await fetchShard(shard, win, concurrency);
      if (res.paused) return 'paused';
      if (!res.ok) { window = adaptiveWindow(window, true); continue; }     // transport fail → shrink + retry

      const digest = await sha256(res.bytes);
      if (digest !== shard.sha256) { window = adaptiveWindow(window, true); continue; }   // corruption → re-fetch

      const w = await writeShard(shard.name, res.bytes);
      if (!w || !w.ok) return { fail: 'write', error: (w && w.error) || 'write-failed' };

      cache[shard.name] = shard.sha256;
      try { await st.saveCache({ ...cache }); } catch { /* persistence best-effort */ }
      return 'ok';
    }
    return { fail: 'verify' };
  }

  async function loop() {
    running = true;
    try {
      if (!vm.ok) { phase = DL.ERROR; progress(); return state(); }
      cache = (await st.loadCache()) || {};
      recompute();
      if (!pending.length) { phase = DL.READY; progress(); return state(); }

      phase = DL.DOWNLOADING; progress();
      for (const name of [...pending]) {
        if (cache[name] === byName[name].sha256) { recompute(); progress({ shard: name }); continue; }
        const outcome = await runShard(byName[name]);
        if (outcome === 'paused') {
          phase = cancelRequested ? DL.IDLE : DL.PAUSED;
          recompute(); progress({ shard: name });
          return state();
        }
        if (outcome === 'ok') { recompute(); progress({ shard: name }); continue; }
        // hard failure (verify / write) → ERROR, never auto-proceed past corruption.
        errors.push({ name, ...(typeof outcome === 'object' ? outcome : {}) });
        phase = DL.ERROR; recompute(); progress({ shard: name });
        return state();
      }
      recompute();
      phase = pending.length ? DL.PAUSED : DL.READY;
      progress();
      return state();
    } finally { running = false; }
  }

  function state() {
    return { ok: phase === DL.READY, phase, done: done.slice(), pending: pending.slice(),
             total, window, errors: errors.slice(), model: manifest && manifest.model, revision: manifest && manifest.revision };
  }

  return {
    // Acquire the single global lock, run to completion/pause, release. Refuses if another model is active.
    async run() {
      if (!vm.ok) return { ...state(), error: 'bad-manifest' };
      const id = { model: manifest.model, revision: manifest.revision };
      if (LOCK && (LOCK.model !== id.model || LOCK.revision !== id.revision)) return { ...state(), error: 'busy', active: LOCK };
      LOCK = id;
      pauseRequested = false; cancelRequested = false;
      try { return await loop(); }
      finally { if (LOCK && LOCK.model === id.model && LOCK.revision === id.revision) LOCK = null; }
    },
    pause() { pauseRequested = true; if (phase === DL.DOWNLOADING) phase = DL.PAUSED; },
    resume() { pauseRequested = false; return this.run(); },
    cancel() { cancelRequested = true; pauseRequested = true; },
    state, running: () => running,
  };
}

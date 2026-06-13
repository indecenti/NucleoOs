// Gate: ANIMA Forge — install-flow (the resilient offline-model installer). Enforces: blocks when the
// engine can't run, auto-resumes across transient drops (never loses verified shards), stops on the user's
// cancel, surfaces a clear message for each fatal class, and runs under the OS-wide download lock.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  prereqFor, messageFor, backoffMs, etaSeconds, humanBytes, abortableSleep, installModel,
} from '../../apps/anima/www/forge/install-flow.js';

class DlError extends Error { constructor(kind, msg) { super(msg || kind); this.kind = kind; } }
const instantSleep = () => Promise.resolve();

// A recording fake of the modal surface the driver drives.
function mkUi() {
  const calls = { phase: [], progress: [], reconnect: [], error: null, cancelled: 0, done: null };
  let cancelCb = null;
  return {
    label: 'Test model',
    onCancel(cb) { cancelCb = cb; },
    fireCancel() { if (cancelCb) cancelCb(); },
    setPhase(n) { calls.phase.push(n); },
    onProgress(p) { calls.progress.push(p); },
    setReconnecting(x) { calls.reconnect.push(x); },
    setError(e) { calls.error = e; },
    setCancelled() { calls.cancelled++; },
    setDone(r) { calls.done = r; },
    calls,
  };
}

test('prereqFor: a GPU model is blocked without WebGPU, allowed with it', () => {
  assert.equal(prereqFor('webgpu', { webgpu: false }).reason, 'no-webgpu');
  assert.equal(prereqFor('webgpu', { webgpu: true }).ok, true);
  assert.equal(prereqFor('wasm', { wasm: false }).reason, 'no-wasm');
  assert.equal(prereqFor('wasm', { wasm: true }).ok, true);
});

test('messageFor: transient is non-fatal (auto-resume), corruption/notfound/cache/no-webgpu are fatal', () => {
  assert.equal(messageFor('transient').fatal, false);
  for (const k of ['integrity', 'notfound', 'cache', 'no-webgpu', 'busy']) {
    const m = messageFor(k, { sizeText: '290 MB' });
    assert.equal(m.fatal, true, k + ' is fatal');
    assert.ok(m.title && m.detail, k + ' has title+detail');
  }
  // bilingual
  assert.notEqual(messageFor('integrity', {}, 'it').title, messageFor('integrity', {}, 'en').title);
});

test('backoffMs grows then caps; etaSeconds & humanBytes are sane', () => {
  assert.equal(backoffMs(1), 1200);
  assert.equal(backoffMs(2), 2400);
  assert.ok(backoffMs(99) <= 15000);
  assert.ok(backoffMs(3) > backoffMs(2));
  assert.equal(etaSeconds(0, 0, 0), null);
  assert.equal(etaSeconds(50, 100, 50), 1);
  assert.equal(humanBytes(290e6), '290.0 MB');
});

test('abortableSleep: returns true when the signal aborts during the wait, false on clean timeout', async () => {
  const ac = new AbortController();
  assert.equal(await abortableSleep(0, ac.signal, instantSleep), false);
  ac.abort();
  assert.equal(await abortableSleep(0, ac.signal, instantSleep), true);   // already aborted
});

test('prereq block: a GPU model with no WebGPU never calls download, shows a clear error', async () => {
  let calls = 0;
  const store = { download: async () => { calls++; return { ok: true, source: 'cdn' }; } };
  const ui = mkUi();
  const r = await installModel({ store, modelId: 'M', kind: 'webgpu', caps: { webgpu: false }, ui, sleep: instantSleep });
  assert.equal(r.reason, 'no-webgpu');
  assert.equal(calls, 0);
  assert.equal(ui.calls.error.title.length > 0, true);
});

test('happy path: download succeeds first try → setDone, no reconnect', async () => {
  const store = { download: async () => ({ ok: true, source: 'cdn' }) };
  const ui = mkUi();
  const r = await installModel({ store, modelId: 'M', kind: 'wasm', caps: { wasm: true }, ui, sleep: instantSleep });
  assert.deepEqual(r, { ok: true, source: 'cdn' });
  assert.deepEqual(ui.calls.done, { ok: true, source: 'cdn' });
  assert.equal(ui.calls.reconnect.length, 0);
});

test('AUTO-RESUME: transient drops are retried until success — verified work is never thrown away', async () => {
  let n = 0;
  const seen = [];
  const store = { download: async (id, { signal }) => {
    seen.push(!!signal);                                   // the abort signal is threaded through
    if (++n < 3) throw new DlError('transient', 'cdn 0 on shard');
    return { ok: true, source: 'sd' };
  } };
  const ui = mkUi();
  const r = await installModel({ store, modelId: 'M', kind: 'wasm', caps: { wasm: true }, ui, sleep: instantSleep });
  assert.equal(r.ok, true);
  assert.equal(n, 3);                                      // 2 failures, then success — it resumed
  assert.equal(ui.calls.reconnect.length, 2);
  assert.ok(seen.every(Boolean));
});

test('CANCEL during the reconnect wait stops the loop (no further download attempts)', async () => {
  const ac = new AbortController();
  let n = 0;
  const store = { download: async () => { n++; throw new DlError('transient', 'drop'); } };
  const ui = mkUi();
  // sleep that aborts the run the moment the backoff wait begins → simulates the user hitting Cancel.
  const sleep = () => { ac.abort(); return Promise.resolve(); };
  const r = await installModel({ store, modelId: 'M', kind: 'wasm', caps: { wasm: true }, ui, sleep, controller: ac });
  assert.equal(r.reason, 'cancelled');
  assert.equal(n, 1);                                      // failed once, cancelled during backoff → not retried
  assert.equal(ui.calls.cancelled, 1);
});

test('FATAL integrity: a SHA mismatch is not retried — clear, fatal error', async () => {
  let n = 0;
  const store = { download: async () => { n++; throw new DlError('integrity', 'SHA mismatch'); } };
  const ui = mkUi();
  const r = await installModel({ store, modelId: 'M', kind: 'wasm', caps: { wasm: true }, ui, sleep: instantSleep });
  assert.equal(r.reason, 'integrity');
  assert.equal(n, 1);                                      // fatal → no auto-resume
  assert.equal(ui.calls.error.fatal, true);
});

test('cancelled thrown by download (mid-pull abort) → setCancelled', async () => {
  const store = { download: async () => { throw new DlError('cancelled', 'cancelled'); } };
  const ui = mkUi();
  const r = await installModel({ store, modelId: 'M', kind: 'wasm', caps: { wasm: true }, ui, sleep: instantSleep });
  assert.equal(r.reason, 'cancelled');
  assert.equal(ui.calls.cancelled, 1);
});

// Regression guard (adversarial-review finding, 2026-06-10): the transient/fatal taxonomy must apply ONLY
// to download I/O. A defect in a UI method (no .kind) must NOT be mistaken for a dropped connection and
// retried forever — it must surface. Before the fix, setPhase()/setDone() lived inside the download try, so
// a throw there was classified transient → infinite auto-resume loop. These two lock the narrowed try.
test('REGRESSION: a UI method that throws BEFORE download surfaces (not an infinite transient retry)', async () => {
  let n = 0;
  const store = { download: async () => { n++; return { ok: true, source: 'cdn' }; } };
  const ui = mkUi();
  ui.setPhase = () => { throw new Error('DOM boom'); };          // a UI defect, not a network drop
  await assert.rejects(() => installModel({ store, modelId: 'M', kind: 'wasm', caps: { wasm: true }, ui, sleep: instantSleep }), /DOM boom/);
  assert.equal(n, 0);                                            // surfaced the UI error; never spun the download loop
});

test('REGRESSION: a UI render throw AFTER a successful download surfaces (not retried as transient)', async () => {
  let n = 0;
  const store = { download: async () => { n++; return { ok: true, source: 'cdn' }; } };
  const ui = mkUi();
  ui.setDone = () => { throw new Error('render boom'); };
  await assert.rejects(() => installModel({ store, modelId: 'M', kind: 'wasm', caps: { wasm: true }, ui, sleep: instantSleep }), /render boom/);
  assert.equal(n, 1);                                            // downloaded once, then the UI error propagated — no re-download
});

test('OS-wide lock: the whole run is wrapped in the injected dlLock', async () => {
  const store = { download: async () => ({ ok: true, source: 'cdn' }) };
  const ui = mkUi();
  let wrapped = false;
  const dlLock = async (label, fn) => { wrapped = true; assert.equal(label, 'Test model'); return fn(); };
  const r = await installModel({ store, modelId: 'M', kind: 'wasm', caps: { wasm: true }, ui, dlLock, sleep: instantSleep });
  assert.equal(r.ok, true);
  assert.equal(wrapped, true);
});

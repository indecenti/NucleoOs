// scheduler.js — MCU-aware coexistence scheduler (the principled form of must-fix #3). The client
// pulls model weights through a single-task, no-PSRAM httpd that also serves /api/anima (the
// verifier) and /api/status. This pure controller reads the device telemetry (free heap, largest
// contiguous block, in-flight verifier work) and decides whether a ranged shard fetch may proceed,
// must throttle (shrink the Range window, go serial), or must pause (back off the device) — and
// never lets an SD model pull run concurrently with a verifier request. Mirrors the webfs
// 512KB/32KB-free circuit breaker so the client backs off BEFORE the device 503s.
// Pure & DOM-free → host-testable.

export const HEAP_FLOOR = 32 * 1024;     // largest contiguous block below this → the device 503s; pause
export const HEAP_TIGHT = 50 * 1024;     // total free below this → throttle (serial, min window)
export const MIN_WINDOW = 256 * 1024;
export const MAX_WINDOW = 1024 * 1024;

export function windowFor(telemetry = {}) {
  const block = telemetry.largestBlock || 0;
  if (block >= 256 * 1024) return MAX_WINDOW;
  if (block >= 128 * 1024) return 512 * 1024;
  return MIN_WINDOW;
}

// decide(telemetry, ctx) → { action:'go'|'throttle'|'pause', window, concurrency, reason }
//   telemetry: { freeHeap, largestBlock, verifyInFlight }
//   ctx:       { op:'model-pull'|'verify', source:'cdn'|'sd' }
export function decide(telemetry = {}, ctx = {}) {
  const { op = 'model-pull', source = 'cdn' } = ctx;
  const freeHeap = telemetry.freeHeap ?? Infinity;
  const block = telemetry.largestBlock ?? Infinity;

  // The CDN path never touches the device → always go (the device is bypassed for the heavy fetch).
  if (op === 'model-pull' && source === 'cdn') return { action: 'go', window: MAX_WINDOW, concurrency: 4, reason: 'cdn-bypasses-device' };

  // Verifier traffic is privileged and tiny; it always proceeds.
  if (op === 'verify') return { action: 'go', window: 0, concurrency: 1, reason: 'verify-privileged' };

  // SD model pull — coexist carefully with the verifier and the heap.
  if (telemetry.verifyInFlight) return { action: 'pause', window: windowFor(telemetry), concurrency: 1, reason: 'verify-in-flight' };
  if (block < HEAP_FLOOR) return { action: 'pause', window: MIN_WINDOW, concurrency: 1, reason: 'heap-floor-breaker' };
  if (freeHeap < HEAP_TIGHT) return { action: 'throttle', window: MIN_WINDOW, concurrency: 1, reason: 'heap-tight' };
  return { action: 'go', window: windowFor(telemetry), concurrency: 1, reason: 'sd-serial-ok' };
}

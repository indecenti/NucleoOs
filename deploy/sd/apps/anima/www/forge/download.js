// download.js — pure orchestration for fetching M4 model weights. NO I/O here: the reducer plans
// requests and folds results; the caller performs fetch. Two hard lessons baked in:
//   (1) every shard fetch is a BOUNDED Range request, never a whole-file GET — the single-task,
//       no-PSRAM httpd resets on sustained multi-MB reads (the voice.js read-storm). Windows shrink
//       adaptively toward MIN_WINDOW on repeated resets.
//   (2) resume is idempotent via SHA match — a shard whose cached SHA matches the manifest is
//       skipped, so an interrupted download (or a CDN copy already cached) needs no re-fetch.
// Pure & DOM-free → host-testable.

export const MIN_WINDOW = 256 * 1024;
export const MAX_WINDOW = 1024 * 1024;

// Manifest: { model, revision, libWasm?, minVramMB?, shards:[{name, bytes, sha256}] }
export function verifyManifest(m) {
  const errors = [];
  if (!m || typeof m !== 'object') return { ok: false, errors: ['not-object'] };
  for (const k of ['model', 'revision']) if (typeof m[k] !== 'string' || !m[k]) errors.push('missing:' + k);
  if (!Array.isArray(m.shards) || !m.shards.length) errors.push('no-shards');
  else m.shards.forEach((s, i) => {
    if (!s || typeof s.name !== 'string' || !s.name) errors.push('shard' + i + ':name');
    if (!Number.isInteger(s && s.bytes) || s.bytes <= 0) errors.push('shard' + i + ':bytes');
    if (typeof (s && s.sha256) !== 'string' || !/^[0-9a-f]{64}$/i.test(s.sha256)) errors.push('shard' + i + ':sha256');
  });
  return { ok: errors.length === 0, errors };
}

export function adaptiveWindow(prev = MAX_WINDOW, didReset = false) {
  if (didReset) return Math.max(MIN_WINDOW, Math.floor((prev || MAX_WINDOW) / 2));
  return Math.max(MIN_WINDOW, Math.min(MAX_WINDOW, prev || MAX_WINDOW));
}

// Bounded Range windows covering [0, bytes). Each {start,end} is inclusive (HTTP Range semantics).
export function planRanges(bytes, window = MAX_WINDOW) {
  const w = Math.max(MIN_WINDOW, Math.min(MAX_WINDOW, (window | 0) || MAX_WINDOW));
  const out = [];
  for (let start = 0; start < bytes; start += w) out.push({ start, end: Math.min(bytes, start + w) - 1 });
  return out;
}

// Build the next bounded Range request for a shard (header string included for assertion in tests).
export function rangeHeader(r) { return `bytes=${r.start}-${r.end}`; }

// Shards still needed given a cache map { name -> sha256 } (idempotent resume).
export function pendingShards(manifest, cached = {}) {
  return manifest.shards.filter((s) => cached[s.name] !== s.sha256);
}

export const DL = { IDLE: 'idle', DOWNLOADING: 'downloading', VERIFYING: 'verifying', READY: 'ready', ERROR: 'error', PAUSED: 'paused' };

export function initState(manifest, { source = 'cdn', cached = {}, window = MAX_WINDOW } = {}) {
  const pending = pendingShards(manifest, cached).map((s) => s.name);
  return { phase: pending.length ? DL.IDLE : DL.READY, source, window, pending, done: [], errors: [], manifest };
}

export function reduce(state, ev) {
  const s = { ...state, pending: [...state.pending], done: [...state.done], errors: [...state.errors] };
  switch (ev.type) {
    case 'start':
      if (s.phase === DL.IDLE || s.phase === DL.PAUSED) s.phase = s.pending.length ? DL.DOWNLOADING : DL.VERIFYING;
      return s;
    case 'shard-ok':
      s.done.push(ev.name); s.pending = s.pending.filter((n) => n !== ev.name);
      s.phase = s.pending.length ? DL.DOWNLOADING : DL.VERIFYING;
      return s;
    case 'shard-fail':
      s.window = adaptiveWindow(s.window, true); s.errors.push({ name: ev.name, error: ev.error, status: ev.status });
      if (s.source === 'sd' && ev.status === 503) s.phase = DL.PAUSED;   // back off the device per Retry-After
      return s;
    case 'pause': s.phase = DL.PAUSED; return s;
    case 'resume': s.phase = s.pending.length ? DL.DOWNLOADING : DL.VERIFYING; return s;
    case 'verify-ok': s.phase = DL.READY; return s;
    case 'verify-fail': s.phase = DL.ERROR; s.errors.push({ verify: ev.reason }); return s;
    case 'reset': return initState(s.manifest, { source: s.source });
    default: return s;
  }
}

// Concurrency policy for the SD source: serial (=1) to avoid the read-storm; CDN may parallelise.
export function maxConcurrency(source) { return source === 'sd' ? 1 : 4; }

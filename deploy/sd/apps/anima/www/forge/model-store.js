// model-store.js — EXPLICIT, user-initiated model downloads for ANIMA Forge. Encodes the product
// rules so they're enforced (and host-tested), not just intended:
//   • NEVER auto-download — nothing fetches on its own; download(id) runs ONLY from a Settings action.
//   • ONE AT A TIME — a hard lock; a second download() while one runs is REJECTED (never silent/parallel).
//   • SOURCE ORDER — online CDN first; if offline (or the CDN fails) → the device SD (Cardputer).
//   • SAFE on the SD — serial, BOUNDED Range windows, scheduler-gated (back off under device heap
//     pressure / while the verifier is busy), so a pull never tips the single-task no-PSRAM webserver.
//   • INTEGRITY — every shard is SHA-256 verified against the manifest before it counts as cached.
// All I/O is injected (fetchWhole / fetchRange / sha256 / cache / telemetry / isOnline), so it is pure
// and host-testable; the UI wires the real fetch + Cache API.

import { planRanges, rangeHeader, adaptiveWindow, MAX_WINDOW, verifyManifest } from './download.js';
import { decide } from './scheduler.js';

// Typed download error so callers (install-flow / the UI) can react by CLASS, not by parsing a string.
//   'transient'  network drop / device 503 / scheduler back-off → SAFE to auto-resume (cached shards skip).
//   'integrity'  SHA mismatch → the bytes are corrupt; re-fetching the SAME source is futile (fatal).
//   'notfound'   404/405 → the model/shard isn't at this source (fall through / fatal if forced).
//   'cache'      the browser Cache API write failed (quota) → fatal, tell the user to free space.
//   'cancelled'  the user aborted via the AbortSignal → stop, never fall through to another source.
//   'busy'       another download already holds the one-at-a-time lock.
export class DlError extends Error {
  constructor(kind, message) { super(message || kind); this.kind = kind; this.name = 'DlError'; }
}
const sumBytes = (files) => files.reduce((n, f) => n + (f.bytes || 0), 0);
const pctOf = (a, b) => (b ? Math.min(100, Math.max(0, Math.round((a / b) * 100))) : 100);
// A failed transport status → which error class. 0 is a thrown fetch (network/abort) — the caller has
// already ruled out abort by the time it asks, so it is a transient drop (the Cardputer went away).
const classifyStatus = (status) => (status === 404 || status === 405) ? 'notfound' : 'transient';
const throwIfAborted = (signal) => { if (signal && signal.aborted) throw new DlError('cancelled', 'cancelled'); };

// id → where to get it. cdnBase/sdBase end with '/'; {file} appended per shard/aux.
// `manifest` is the shard list + SHA-256 EMBEDDED here so the model resolves even when it has not
// been staged on the device SD (the manifest is tiny config, not bulk weight data). A staged SD
// manifest still wins (see filesOf); the embed is the fallback that removes the "manifest not found"
// dead-end and lets the weights stream from the CDN when online. SHAs pin the HF revision below.
export const REGISTRY = [
  { id: 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC', kind: 'webgpu', label: 'Coder 0.5B · GPU (WebLLM)',
    cdnBase: 'https://huggingface.co/mlc-ai/Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC/resolve/main/',
    sdBase: '/apps/anima/forge/models/Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC/',
    manifest: {
      model: 'Qwen2.5-Coder-0.5B-Instruct-q4f16_1-MLC', format: 'mlc', revision: 'b0c035a7873e2f0db0eef83c910c92b34b44af80',
      modelType: 'qwen2', contextWindow: 32768, totalBytes: 289690815,
      shards: [
        { name: 'params_shard_0.bin', bytes: 68067328, sha256: '8ba207c8b5b2502d9d178202d44dfd774050b79e4abd263faaf1c37084f9e781' },
        { name: 'params_shard_1.bin', bytes: 33234176, sha256: '4d65d9746fee0cec58dc28350391f96da2e11c143058237d6149b0229ca5f73b' },
        { name: 'params_shard_2.bin', bytes: 33505280, sha256: 'fdaaecfaf16fe3cb86840a0fe3c70424b4efc087604e4551dfd1d84b93aee20f' },
        { name: 'params_shard_3.bin', bytes: 33053696, sha256: '4b5566be1d7d5a885b92b7d0fa250a6e48db7aa6749a5d8ae32b41e461d34f7e' },
        { name: 'params_shard_4.bin', bytes: 33020928, sha256: 'c269ed538f402a94b2e7a4f883c20b7764b114ed98ad982df6d59380158ed45a' },
        { name: 'params_shard_5.bin', bytes: 29211648, sha256: '4e1fa63b515392f4ca61d44eec16c22fd433862e7e0682f2fc2cc3f0e6c09f0b' },
        { name: 'params_shard_6.bin', bytes: 33297408, sha256: '6fd596e81fc46ee295af587f6304adc4f8007d2c826b657df02d6116bd2aab28' },
        { name: 'params_shard_7.bin', bytes: 14605824, sha256: 'a5c04a0de382276122c49db421768b950783f371b321560602b416b44e0152c4' },
      ],
      aux: [
        { name: 'merges.txt', bytes: 1671839, sha256: '599bab54075088774b1733fde865d5bd747cbcc7a547c5bc12610e874e26f5e3' },
        { name: 'mlc-chat-config.json', bytes: 2043, sha256: 'e1ac4f8e323fb7455629cf31aa3021e76f4399657a18ae8936233bf35b2657a1' },
        { name: 'ndarray-cache.json', bytes: 102431, sha256: '308ae05f2aa75c0fa04a5b822b373af8910548ea9f5c9102750fe010c3845bc6' },
        { name: 'tensor-cache.json', bytes: 102431, sha256: '308ae05f2aa75c0fa04a5b822b373af8910548ea9f5c9102750fe010c3845bc6' },
        { name: 'tokenizer.json', bytes: 7031645, sha256: 'c0382117ea329cdf097041132f6d735924b697924d6f6fc3945713e96ce87539' },
        { name: 'tokenizer_config.json', bytes: 7305, sha256: '5b5d4f65d0acd3b2d56a35b56d374a36cbc1c8fa5cf3b3febbbfabf22f359583' },
        { name: 'vocab.json', bytes: 2776833, sha256: 'ca10d7e9fb3ed18575dd1e277a2579c16d108e32f27439684afa0e10b1440910' },
      ],
    } },
  { id: 'Qwen2.5-Coder-0.5B-Instruct-GGUF', kind: 'wasm', label: 'Coder 0.5B · CPU (WASM, no GPU)',
    cdnBase: 'https://huggingface.co/Qwen/Qwen2.5-Coder-0.5B-Instruct-GGUF/resolve/main/',
    sdBase: '/apps/anima/forge/models/Qwen2.5-Coder-0.5B-Instruct-GGUF/',
    manifest: {
      model: 'Qwen2.5-Coder-0.5B-Instruct-GGUF', format: 'gguf', revision: 'ebb2015119c907b064c512bf053e945850b5875f', totalBytes: 491400064,
      shards: [
        { name: 'qwen2.5-coder-0.5b-instruct-q4_k_m.gguf', bytes: 491400064, sha256: '1d9614638d18024d0fbb36575a15f1302a3adf044df10345688ec4f6e1c4ff32' },
      ],
      aux: [] } },
];

export function modelById(id) { return REGISTRY.find((m) => m.id === id) || null; }

// The manifest embedded in the registry for `id` (null if unknown / not embedded).
export function embeddedManifest(id) { const m = modelById(id); return m && m.manifest ? m.manifest : null; }

// deps: { getManifest(id)->manifest, fetchWhole(url)->{ok,status,bytes:Uint8Array}, fetchRange(url,{start,end})->{ok,status,bytes},
//         sha256(Uint8Array)->hex, cacheHas(id,name)->bool, cachePut(id,name,Uint8Array)->void,
//         isOnline()->bool, telemetry()->{freeHeap,largestBlock,verifyInFlight} }
export function makeModelStore(deps = {}) {
  let active = null;                       // the id currently downloading — the one-at-a-time lock
  const need = (m) => { if (typeof deps[m] !== 'function') throw new Error('model-store: missing dep ' + m); };
  for (const m of ['getManifest', 'sha256', 'cacheHas', 'cachePut']) need(m);

  // Resolve a manifest: prefer one staged on the device SD (deps.getManifest — it can be re-synced
  // to update SHAs), but fall back to the manifest EMBEDDED in the registry when the SD has none.
  // This is what removes the "(manifest non trovato)" dead-end: an un-provisioned model still
  // resolves, and its weights stream from the CDN when online.
  async function resolveManifest(id) {
    let man = null;
    try { man = await deps.getManifest(id); } catch { man = null; }
    if (!man || !Array.isArray(man.shards) || !man.shards.length) man = embeddedManifest(id);
    return man;
  }
  async function filesOf(id) {
    const man = await resolveManifest(id);
    const v = verifyManifest(man ? { model: man.model, revision: man.revision, shards: man.shards } : null);
    if (!v.ok) throw new Error('bad manifest: ' + v.errors.join(','));
    return { man, files: [...man.shards, ...(man.aux || [])] };
  }

  // Is the whole model already cached? (→ status, and lets a re-run skip a finished download.)
  async function isCached(id) {
    const { files } = await filesOf(id);
    for (const f of files) if (!(await deps.cacheHas(id, f.name))) return false;
    return true;
  }
  async function status(id) {
    if (active === id) return 'downloading';
    return (await isCached(id)) ? 'cached' : 'absent';
  }

  // Fetch + verify + cache ONE file (shard or aux). onBytes(curFileBytes) reports cumulative bytes for
  // THIS file (so the caller can paint a smooth bar); onPhase(phase, shard) flags the verify step (the
  // SHA of a 68 MB shard is a multi-second CPU spin worth surfacing). All transport carries the AbortSignal
  // so a cancel aborts in-flight bytes immediately. Throws a typed DlError on every failure.
  async function fetchOneShard(id, base, file, src, signal, onBytes, onPhase) {
    if (await deps.cacheHas(id, file.name)) { onBytes && onBytes(file.bytes); return; }   // idempotent resume: skip cached
    throwIfAborted(signal);
    const url = base + file.name;
    let bytes;
    if (src === 'cdn') {
      // CDN: whole-file GET (fast, not the device). Streams partial bytes through onChunk when the UI's
      // fetcher supports it (a 290 MB pull deserves a live bar), else the bar just steps per file.
      const r = await deps.fetchWhole(url, { signal, onChunk: onBytes });
      if (!r || !r.ok) { throwIfAborted(signal); throw new DlError(classifyStatus(r && r.status), 'cdn ' + (r && r.status) + ' on ' + file.name); }
      bytes = r.bytes;
    } else {
      // SD (Cardputer): SAFE bounded-Range windows, scheduler-gated, never a whole-file GET.
      let win = MAX_WINDOW; const chunks = []; let acc = 0;
      const ranges = () => planRanges(file.bytes, win);
      for (let i = 0, rs = ranges(); i < rs.length; i++) {
        throwIfAborted(signal);
        const d = decide(deps.telemetry ? deps.telemetry() : {}, { op: 'model-pull', source: 'sd' });
        if (d.action === 'pause') throw new DlError('transient', 'paused: ' + d.reason);   // back off — caller auto-resumes
        if (d.action === 'throttle') { win = d.window; rs = ranges(); }                    // shrink window, recompute
        const r = await deps.fetchRange(url, rs[i], { signal });
        if (r && r.status === 503) { win = adaptiveWindow(win, true); rs = ranges(); i--; continue; }
        if (!r || !r.ok) { throwIfAborted(signal); throw new DlError(classifyStatus(r && r.status), 'sd ' + (r && r.status) + ' on ' + file.name); }
        chunks.push(r.bytes); acc += r.bytes.length; onBytes && onBytes(acc);
      }
      bytes = concat(chunks);
    }
    onPhase && onPhase('verifying', file.name);
    throwIfAborted(signal);
    if ((await deps.sha256(bytes)) !== file.sha256) throw new DlError('integrity', 'SHA mismatch on ' + file.name);
    try { await deps.cachePut(id, file.name, bytes); }
    catch (e) { throw new DlError('cache', 'browser cache write failed on ' + file.name + ': ' + (e && e.message || e)); }
  }

  async function fromSource(id, base, src, onProgress, signal) {
    const { man, files } = await filesOf(id);
    const bytesTotal = man.totalBytes || sumBytes(files);
    let done = 0, bytesBase = 0;
    for (const f of files) {
      const onBytes = (cur) => { const bd = bytesBase + Math.min(cur || 0, f.bytes); onProgress && onProgress({ phase: 'progress', source: src, done, total: files.length, bytesDone: bd, bytesTotal, pct: pctOf(bd, bytesTotal), shard: f.name }); };
      const onPhase = (phase, shard) => onProgress && onProgress({ phase, source: src, done, total: files.length, bytesDone: bytesBase, bytesTotal, pct: pctOf(bytesBase, bytesTotal), shard });
      await fetchOneShard(id, base, f, src, signal, onBytes, onPhase);
      done++; bytesBase += f.bytes;
      onProgress && onProgress({ phase: 'progress', source: src, done, total: files.length, bytesDone: bytesBase, bytesTotal, pct: pctOf(bytesBase, bytesTotal), shard: f.name });
    }
  }

  // download(id) — the ONLY entry point, called from a user action. Rejects if another is running.
  // source order: forced `opts.source`, else online→[cdn,sd], offline→[sd]. One call already does the
  // CDN→SD fallback; AUTO-RESUME across reconnects is the caller's loop (install-flow): a transient throw
  // is caught and download(id) is simply re-run — every verified shard is skipped, so it picks up where it
  // left off. opts: { source?, onProgress?, signal? }. Returns { ok, source }; throws a typed DlError.
  async function download(id, opts = {}) {
    if (!modelById(id)) throw new DlError('notfound', 'unknown model: ' + id);
    if (active) throw new DlError('busy', 'a download is already running (' + active + ') — one at a time');
    active = id;
    const signal = opts.signal;
    try {
      const m = modelById(id);
      const online = deps.isOnline ? deps.isOnline() : false;
      const order = opts.source ? [opts.source] : (online ? ['cdn', 'sd'] : ['sd']);
      let lastErr = null;
      for (const src of order) {
        throwIfAborted(signal);
        const base = src === 'cdn' ? m.cdnBase : m.sdBase;
        try { await fromSource(id, base, src, opts.onProgress, signal); opts.onProgress && opts.onProgress({ phase: 'done', source: src }); return { ok: true, source: src }; }
        catch (e) {
          if (e && e.kind === 'cancelled') throw e;                       // user aborted → never fall through
          lastErr = e;
          opts.onProgress && opts.onProgress({ phase: 'fallback', from: src, error: String(e.message || e), kind: e && e.kind });
        }
      }
      throw lastErr || new DlError('transient', 'no source');
    } finally { active = null; }
  }

  return { REGISTRY, status, isCached, download, get active() { return active; } };
}

function concat(chunks) {
  let n = 0; for (const c of chunks) n += c.length;
  const out = new Uint8Array(n); let o = 0; for (const c of chunks) { out.set(c, o); o += c.length; }
  return out;
}

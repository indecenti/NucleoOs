// lib-verify.js — opt-in DEEP integrity check for vendored runtime libs. The air-gap readiness probe is
// size-level (fast, cheap: catches truncation). This is the "checksum-verified" upgrade: it reads the
// WHOLE file from the device SD via BOUNDED, scheduler-gated Range windows (never a whole-file GET, which
// would tip the no-PSRAM single-task webserver) and SHA-256s it against vendor/lib-manifest.json — so a
// same-SIZE-but-wrong-revision or bit-rotted file is caught too, not just a short one.
//
// Pure: all I/O injected (fetchRange / sha256 / telemetry) → host-testable; the UI wires the real fetch.
import { MAX_WINDOW, adaptiveWindow } from './download.js';
import { decide } from './scheduler.js';

// deepVerifyLib(file, deps) — read file.url fully (offset-based, so a mid-read window shrink can't
// misalign) and checksum it.
//   file: { url, bytes, sha256 }
//   deps: { fetchRange(url,{start,end})->{ok,status,bytes:Uint8Array}, sha256(Uint8Array)->hex, telemetry?() }
//   -> { ok, sha?, reason }   reason ∈ null | 'no-manifest' | 'sha-mismatch' | 'read-error' | 'paused'
export async function deepVerifyLib(file, deps) {
  if (!file || !file.bytes || !file.sha256) return { ok: false, reason: 'no-manifest' };
  const fetchRange = deps.fetchRange, sha256 = deps.sha256;
  let win = MAX_WINDOW, off = 0; const chunks = [];
  while (off < file.bytes) {
    const d = decide(deps.telemetry ? deps.telemetry() : {}, { op: 'model-pull', source: 'sd' });
    if (d.action === 'pause') return { ok: false, reason: 'paused' };          // back off — caller retries later
    if (d.action === 'throttle' && d.window) win = Math.max(d.window, 1);       // shrink the window under heap pressure
    const want = Math.min(win, file.bytes - off);
    const end = off + want - 1;
    const r = await fetchRange(file.url, { start: off, end });
    if (r && r.status === 503) { win = adaptiveWindow(win, true); continue; }   // device busy → smaller window, SAME offset
    if (!r || !r.ok || !r.bytes) return { ok: false, reason: 'read-error' };
    // A server that IGNORES Range (HTTP 200, e.g. python http.server in preview) returns the WHOLE file
    // for a ranged request. Detect it (got more than we asked) and checksum it once, rather than
    // concatenating duplicate full copies. The device webfs serves 206 with exactly `want` bytes.
    if (r.bytes.length > want) {
      const full = off === 0 ? r.bytes : concat([...chunks, r.bytes]);
      const body = full.length > file.bytes ? full.slice(0, file.bytes) : full;
      const sha2 = await sha256(body);
      return { ok: sha2 === file.sha256, sha: sha2, reason: sha2 === file.sha256 ? null : 'sha-mismatch' };
    }
    chunks.push(r.bytes);
    off = end + 1;
  }
  const all = concat(chunks);
  const sha = await sha256(all);
  return { ok: sha === file.sha256, sha, reason: sha === file.sha256 ? null : 'sha-mismatch' };
}

// deepVerifyAll(manifest, base, deps, onEach) — verify every lib in the manifest, in series (one Range
// stream at a time, so it never floods the device). Returns [{ name, path, ok, sha?, reason }].
export async function deepVerifyAll(manifest, base, deps, onEach) {
  const out = [];
  for (const l of (manifest && manifest.libs) || []) {
    const res = await deepVerifyLib({ url: base + l.path, bytes: l.bytes, sha256: l.sha256 }, deps);
    const row = { name: l.name, path: l.path, ...res };
    out.push(row); if (onEach) onEach(row);
  }
  return out;
}

function concat(chunks) { let n = 0; for (const c of chunks) n += c.length; const o = new Uint8Array(n); let p = 0; for (const c of chunks) { o.set(c, p); p += c.length; } return o; }

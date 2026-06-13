// micgate.js — OS-wide microphone gate for NucleoOS (the Cardputer's single PDM mic).
//
// The Cardputer has exactly ONE physical microphone (PDM on I2S). The firmware already makes a
// double-open impossible at the hardware level — record-to-SD (s_recording) and the live PCM stream
// (s_streaming) are mutually exclusive atomics, and a loser gets HTTP 409. But that backstop says
// nothing ABOUT WHO holds the mic, and every web app used to talk to /api/rec/* on its own: ANIMA,
// the Dettatura app and the Recorder each re-implemented fetch + 409 + reconnect, drifted apart, and
// could only discover a conflict by blindly trying and catching a 409 (sometimes after ~9s of retries).
//
// This module is THE single way web code touches the Cardputer mic. Like the download gate
// (dlgate.js) it is built on Web Locks, which coordinate across every same-origin context — so the
// 'nucleo-mic' lock is genuinely OS-wide: no two apps, tabs or iframes the device serves can hold the
// mic at once, and the holder's human label is broadcast so anyone can show "🎤 in uso da …".
//
// Two backstops stay untouched beneath this: the firmware atomics (hardware truth) and the 409
// (covers the ONE owner Web Locks can't see — the on-device native Recorder, which holds no web lock).
//
// A new mic app needs zero conflict logic: call streamMic()/recordToSd() and the gate guarantees
// exclusivity, names the current holder on a refusal, and (for streams) owns the whole fetch +
// odd-byte-tail + reconnect-across-the-5-min-cap dance for the life of the held lock.

const LOCK = 'nucleo-mic';
const GAIN = 2.5;                  // firmware PDM is quiet; the apps historically lifted samples ×2.5
const MIC_RATE = 16000;           // matches REC_RATE_HZ in nucleo_recorder.c

const _subs = new Set();
const _bc = (typeof BroadcastChannel !== 'undefined') ? new BroadcastChannel('nucleo-mic') : null;
let _local = null;                 // label holding the mic in THIS tab
let _remote = null;                // label holding it in ANOTHER tab (heard over BroadcastChannel)
let _chain = Promise.resolve();    // same-tab fallback when Web Locks is unavailable
let _busy = false;                 // fallback-only busy flag

function _emit() { const a = _local || _remote; for (const cb of _subs) { try { cb(a); } catch {} } }
function _notify(label) {
  _local = label || null;
  try { _bc && _bc.postMessage({ active: _local }); } catch {}
  _emit();
}
if (_bc) _bc.onmessage = (e) => {
  const d = (e && e.data) || {};
  if (d.query) { if (_local) { try { _bc.postMessage({ active: _local }); } catch {} } return; }  // a tab just opened → re-announce if we hold it
  _remote = d.active || null; _emit();
};

function _hasLocks() { return typeof navigator !== 'undefined' && navigator.locks && navigator.locks.request; }

// Run `fn` while holding the single OS-wide mic lock. `label` is the human string shown to other apps
// ("ANIMA", "Dettatura", "Registrazione"). DEFAULT IS FAIL-FAST (ifAvailable): a mic button must react
// now, not silently queue and fire minutes later. It resolves to opts.skipValue (default null) without
// running `fn` when the mic is already held. Pass {wait:true} to queue FIFO instead (rarely wanted).
export async function withMicLock(label, fn, opts = {}) {
  const skip = ('skipValue' in opts) ? opts.skipValue : null;
  const run = async () => { _notify(label); try { return await fn(); } finally { _notify(null); } };

  if (_hasLocks()) {
    if (opts.wait) return navigator.locks.request(LOCK, { mode: 'exclusive' }, () => run());
    return navigator.locks.request(LOCK, { mode: 'exclusive', ifAvailable: true }, (lock) => (lock ? run() : skip));
  }

  // Fallback (no Web Locks): serialise within this tab via a promise chain.
  if (!opts.wait && _busy) return skip;
  const prev = _chain;
  let release; _chain = new Promise((r) => { release = r; });
  await prev;
  _busy = true;
  try { return await run(); } finally { _busy = false; release(); }
}

// The label of whoever holds the mic right now across the OS, or null. Sees web holders (any tab) via
// the lock-label broadcast; the on-device native Recorder holds no web lock, so use micBusy() for that.
export function activeMicLabel() { return _local || _remote || null; }

// Best-effort OS-wide busy check INCLUDING the native on-device recorder (which the Web Lock can't see):
// combines the web lock label with the firmware's /api/rec/status. Returns {busy, label}.
export async function micBusy() {
  if (_local || _remote) return { busy: true, label: _local || _remote };
  if (_hasLocks() && navigator.locks.query) {
    try { const q = await navigator.locks.query(); if (((q && q.held) || []).some((l) => l.name === LOCK)) return { busy: true, label: null }; } catch {}
  }
  try { const s = await (await fetch('/api/rec/status', { cache: 'no-store' })).json();
    if (s && (s.recording || s.streaming || s.busy)) return { busy: true, label: s.recording ? 'Registrazione' : null }; } catch {}
  return { busy: false, label: null };
}

// Subscribe to holder changes (label string while held, null when free). Fires IMMEDIATELY with what we
// know now and asks any holder in another tab to re-announce — so an app that opens WHILE the mic is
// already taken shows "in uso da …" right away, not only after the next state change. Returns an
// unsubscribe. (Web↔web is covered here; the on-device native Recorder, which holds no web lock, is
// surfaced honestly at click-time via the 'busy' error so we never background-poll the tight httpd.)
export function onMicStatus(cb) {
  _subs.add(cb);
  try { cb(activeMicLabel()); } catch {}
  try { _bc && _bc.postMessage({ query: true }); } catch {}
  return () => _subs.delete(cb);
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// streamMic — the ONE path for live-PCM consumers (ANIMA chat, Dettatura). Holds the mic lock for the
// WHOLE session, so the brief gap while reconnecting across the firmware's 5-min cap can't be stolen by
// another web app. Delivers whole 16-bit samples (odd-byte tails reassembled); the caller applies its
// own transform in onframe (feed Vosk, buffer for Groq, …). Stop by aborting the AbortSignal you pass.
//
//   streamMic({ label, signal, onstart, onframe(Int16Array, info), onlevel(0..1), onend, onerror(kind,who) })
//     info = { sampleRate, bytes:Uint8Array, gainedBytes() }   ·   kind = 'busy' | 'auth' | 'audio'
export async function streamMic(opts = {}) {
  const { label = 'Microfono', signal, onstart, onend, onerror } = opts;
  if (signal && signal.aborted) { onend && onend(); return; }
  let acquired = false;
  await withMicLock(label, async () => { acquired = true; await _runStream(opts); }, { skipValue: null });
  if (!acquired) {                                   // mic already held by another web app → fail fast, name it
    onerror && onerror('busy', activeMicLabel());
    onend && onend();
  }
}

async function _runStream(opts) {
  const { signal, onstart, onframe, onlevel, onend, onerror, onstatus } = opts;
  const delay = (ms) => new Promise((r) => setTimeout(r, ms));

  // Fast-fail if the on-device native Recorder owns the mic: it holds NO web lock, so we'd otherwise
  // burn ~9s of retries on 409 before giving up. /api/rec/status.recording is the only one who knows.
  if (!(signal && signal.aborted)) {
    try { const s = await (await fetch('/api/rec/status', { cache: 'no-store' })).json();
      if (s && s.recording) { onerror && onerror('busy', 'Registrazione'); onend && onend(); return; } } catch {}
  }

  // A reconnect (the 5-min cap, or a socket dropped under load) is normally <200ms and invisible. Only
  // if it drags past 700ms do we tell the app 'reconnecting' ("Riconnetto…") — honest status without a
  // flicker every 5 minutes; the next successful connect clears it back to 'live'.
  let reTimer = null;
  const clearRe = () => { if (reTimer) { clearTimeout(reTimer); reTimer = null; } };
  const armRe = () => { if (!reTimer && started) reTimer = setTimeout(() => { reTimer = null; onstatus && onstatus('reconnecting'); }, 700); };

  let started = false, fails = 0, tail = null, hardErr = null;
  while (!(signal && signal.aborted)) {
    let resp;
    try { resp = await fetch('/api/rec/stream', { signal, cache: 'no-store' }); }
    catch (e) { if (signal && signal.aborted) break; if (++fails > 12) { hardErr = 'audio'; break; } armRe(); await delay(600); continue; }
    if (!resp.ok || !resp.body) {
      if (resp.status === 401 || resp.status === 403) { hardErr = 'auth'; break; }   // auth won't fix on retry
      if (signal && signal.aborted) break;
      if (++fails > 12) { hardErr = resp.status === 409 ? 'busy' : 'audio'; break; }
      armRe(); await delay(resp.status === 409 ? 800 : 600); continue;   // 409 = mic briefly held by a releasing stream
    }
    fails = 0; clearRe();
    if (!started) { started = true; onstart && onstart(); }
    onstatus && onstatus('live');

    const reader = resp.body.getReader();
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        let bytes = value;
        if (tail) { const m = new Uint8Array(tail.length + bytes.length); m.set(tail); m.set(bytes, tail.length); bytes = m; tail = null; }
        const whole = bytes.length & ~1, samples = whole >> 1;   // a half sample waits for the next chunk
        if (samples) {
          const dv = new DataView(bytes.buffer, bytes.byteOffset, whole);
          const s16 = new Int16Array(samples);
          let sum = 0;
          for (let i = 0; i < samples; i++) { const s = dv.getInt16(i * 2, true); s16[i] = s; const f = s / 0x8000; sum += f * f; }
          if (onframe) {
            const view = new Uint8Array(bytes.buffer, bytes.byteOffset, whole);
            onframe(s16, { sampleRate: MIC_RATE, bytes: view, gain: GAIN, gainedBytes: () => _gainBytes(s16) });
          }
          if (onlevel) onlevel(Math.min(1, Math.sqrt(sum / samples) * 4 * GAIN));
        }
        if (bytes.length > whole) tail = bytes.slice(whole);
      }
    } catch (e) { if (signal && signal.aborted) break; /* dropped socket → reconnect under our held lock */ }
    if (signal && signal.aborted) break;
    armRe(); await delay(180);   // let the firmware release s_streaming before we re-claim it (lock still ours)
  }

  clearRe();
  if (hardErr && !(signal && signal.aborted)) onerror && onerror(hardErr, activeMicLabel());
  onend && onend();
}

// Apply the historical ×GAIN lift and return clamped 16-bit LITTLE-ENDIAN bytes (for the Groq WAV path).
function _gainBytes(s16) {
  const out = new Uint8Array(s16.length * 2); const dv = new DataView(out.buffer);
  for (let i = 0; i < s16.length; i++) { let s = Math.max(-32768, Math.min(32767, s16[i] * GAIN)); dv.setInt16(i * 2, s, true); }
  return out;
}

// ─────────────────────────────────────────────────────────────────────────────────────────────────
// recordToSd — the ONE path for record-to-SD (the Voice Recorder). Holds the same mic lock as
// streamMic, so a recording and any live stream are mutually exclusive at the WEB layer too, not only
// via the firmware 409. Resolves with the final firmware status ({path, …}) once recording has stopped;
// transcoding the WAV happens AFTER, outside the lock (it's just file I/O).
//
//   const rec = recordToSd({ label, onstart(s), onstatus(s), onerror(kind,who) });
//   …later… rec.stop();   ·   const final = await rec.done;   // {path} or null
export function recordToSd(opts = {}) {
  const { label = 'Registrazione', onstart, onstatus, onerror } = opts;
  const ac = new AbortController();
  const done = (async () => {
    let acquired = false, result = null;
    await withMicLock(label, async () => { acquired = true; result = await _runRecord(ac.signal, opts); }, { skipValue: null });
    if (!acquired) { onerror && onerror('busy', activeMicLabel()); return null; }
    return result;
  })();
  return { stop: () => { try { ac.abort(); } catch {} }, done };
}

async function _runRecord(signal, opts) {
  const { onstart, onstatus, onerror } = opts;
  let res;
  try { res = await fetch('/api/rec/start', { method: 'POST' }); }
  catch { onerror && onerror('audio'); return null; }
  if (!res.ok) { onerror && onerror(res.status === 409 ? 'busy' : (res.status === 401 || res.status === 403) ? 'auth' : 'audio', activeMicLabel()); return null; }
  const begun = await res.json().catch(() => ({}));
  onstart && onstart(begun);

  const stop = () => { fetch('/api/rec/stop', { method: 'POST' }).catch(() => {}); };
  if (signal.aborted) stop(); else signal.addEventListener('abort', stop, { once: true });

  // Poll until the firmware reports the take finished (user stop, the native app, or a fault).
  // 600 ms, not 300: the handler reads only static vars (~150 B JSON, no SD), but a take can last
  // minutes, so halving the rate halves the steady load on the single httpd task for an imperceptible
  // VU-meter difference (the consumer interpolates between samples).
  return await new Promise((resolve) => {
    const poll = setInterval(async () => {
      try {
        const s = await (await fetch('/api/rec/status', { cache: 'no-store' })).json();
        onstatus && onstatus(s);
        if (!s.recording) { clearInterval(poll); resolve(s); }
      } catch { /* transient; keep polling */ }
    }, 600);
  });
}

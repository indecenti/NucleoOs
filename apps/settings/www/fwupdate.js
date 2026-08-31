// fwupdate.js — the Settings "Updates" tab: check → download → verify → flash → verify reboot.
//
// Trust chain, in order:
//   1. api.github.com tells us the latest release TAG (conditional GET, shared cache with the
//      shell notifier via localStorage — same origin, one coherent state).
//   2. The Pages site (indecenti.github.io) hosts the browser-fetchable copies — GitHub Release
//      assets send no CORS headers, Pages does. Its version.json must match the API tag, or the
//      deploy is still catching up and we refuse to flash yesterday's image under a new tag.
//   3. SHA256SUMS (same Pages deploy) must list the OTA image; the download is hashed in JS
//      (crypto.subtle needs a secure context — http://LAN is not one) and must match.
//   4. POST /api/ota — the app-only image, NEVER the merged 0x0 flasher image. The firmware
//      handles its own RAM posture (arbiter single-flight, canvas free, voice suspend) and
//      reboots; bootloader rollback (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) covers a bad image.
//   5. We poll /api/status until the reported version matches the target.
//
// The device only ever sees step 4 — everything else is browser↔GitHub.

import {
  LS_CACHE, LS_ENABLED, UPDATE_TTL_MS, UPDATE_PAGES_BASE, UPDATE_OTA_BIN, UPDATE_SUMS,
  UPDATE_VERSION_JSON, UPDATE_RELEASES_URL, UPDATE_SD_ZIP_URL,
  checkDue, cmpSemver, parseSemver, fetchLatestRelease, parseSha256Sums,
} from '/update-core.js';
import { sha256Hex } from '/sha256.js';

const readJSON = (k) => { try { return JSON.parse(localStorage.getItem(k)); } catch { return null; } };
const writeJSON = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} };

export function initUpdatesTab({ $, T, getStatus, pausePoller, resumePoller }) {
  const TT = (k, params) => { let s = T(k); for (const [p, v] of Object.entries(params || {})) s = s.replace('{' + p + '}', v); return s; };
  let cache = readJSON(LS_CACHE);
  let busy = false;          // a flash is in progress — ignore re-entrant clicks
  let confirmArmed = false;  // two-step install button

  const els = () => ({
    cur: $('up-cur'), latest: $('up-latest'), checked: $('up-checked'), state: $('up-state'),
    check: $('up-check'), flash: $('up-flash'), flashLbl: $('up-flash-lbl'), sd: $('up-sd'),
    notesLink: $('up-notes-link'), notes: $('up-notes'), prog: $('up-prog'),
    progBar: $('up-prog-bar'), progLbl: $('up-prog-lbl'), auto: $('up-auto'),
  });

  function fmtWhen(ts) {
    if (!ts) return T('upNever');
    const d = new Date(ts);
    return `${String(d.getDate()).padStart(2, '0')}/${String(d.getMonth() + 1).padStart(2, '0')} ${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
  }
  function setState(txt) { const e = els(); if (e.state) e.state.textContent = txt; }
  function setProgress(on, label, pct) {
    const e = els(); if (!e.prog) return;
    e.prog.hidden = !on;
    if (label != null && e.progLbl) e.progLbl.textContent = label;
    if (e.progBar) {
      e.progBar.classList.toggle('pulse', on && pct == null);
      e.progBar.style.width = pct == null ? '100%' : Math.max(2, Math.min(100, pct)) + '%';
    }
  }
  function disarmConfirm() { confirmArmed = false; const e = els(); if (e.flashLbl) e.flashLbl.textContent = T('upInstall'); }

  function render() {
    const e = els();
    const st = getStatus();
    const cur = (st && st.version) || '';
    if (e.cur) e.cur.textContent = cur || '—';
    if (e.latest) e.latest.textContent = (cache && cache.tag) || '—';
    if (e.checked) e.checked.textContent = fmtWhen(cache && cache.checkedAt);
    if (e.sd) { e.sd.href = UPDATE_SD_ZIP_URL; e.sd.hidden = !(cache && cache.tag); }
    if (e.notesLink) { e.notesLink.href = (cache && cache.url) || UPDATE_RELEASES_URL; e.notesLink.hidden = !(cache && cache.tag); }
    if (e.notes) e.notes.textContent = (cache && cache.notes) || '—';   // textContent: remote markdown stays inert
    let newer = false;
    if (cache && cache.tag && parseSemver(cur)) {
      const c = cmpSemver(cur, cache.tag);
      newer = c < 0;
      setState(newer ? TT('upNewAvail', { tag: cache.tag }) : (c > 0 ? T('upDevBuild') : T('upUpToDate')));
    } else if (cache && cache.tag) setState('—');
    if (e.flash && !busy) { e.flash.hidden = !newer; disarmConfirm(); }
    if (e.auto) e.auto.checked = localStorage.getItem(LS_ENABLED) !== '0';
  }

  async function check(force) {
    if (busy) return;
    cache = readJSON(LS_CACHE) || cache;
    if (force || checkDue({ cache, nowMs: Date.now(), ttlMs: UPDATE_TTL_MS })) {
      setState(T('upChecking'));
      const res = await fetchLatestRelease(fetch, cache, Date.now());
      if (res.ok) { cache = res.cache; writeJSON(LS_CACHE, cache); }
      else if (force) { setState(T('upCheckFail')); render(); return; }
    }
    render();
  }

  // A firmware image is a few MB; anything past this is a misconfigured host or a hostile body, and
  // we refuse it rather than grow the browser heap unbounded (the device's app partition is ~3.6 MB).
  const MAX_IMAGE_BYTES = 8 * 1024 * 1024;

  // Read a Response body to completion with a byte-progress callback (Content-Length can be the
  // COMPRESSED transfer size on Pages, so only received-decoded bytes are honest to display).
  // Returns null if the body exceeds MAX_IMAGE_BYTES (caller treats it as a failed download).
  async function readAll(resp, onBytes) {
    if (!resp.body || !resp.body.getReader) {
      const b = new Uint8Array(await resp.arrayBuffer());
      return b.length > MAX_IMAGE_BYTES ? null : b;
    }
    const reader = resp.body.getReader();
    const chunks = []; let total = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.length;
      if (total > MAX_IMAGE_BYTES) { try { await reader.cancel(); } catch {} return null; }
      chunks.push(value);
      onBytes(total);
    }
    const out = new Uint8Array(total); let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.length; }
    return out;
  }

  // Pages fetch with an explicit timeout: the small JSON/checksum reads get a short ceiling, the
  // multi-MB image a generous one, so a stalled connection fails the step instead of hanging the UI.
  const pagesFetch = (name, timeoutMs = 15000) => {
    let signal;
    try { signal = AbortSignal.timeout(timeoutMs); } catch {}
    return fetch(UPDATE_PAGES_BASE + name, signal ? { cache: 'no-store', signal } : { cache: 'no-store' });
  };

  async function doFlash() {
    if (busy || !cache || !cache.tag) return;
    busy = true;
    const e = els();
    if (e.check) e.check.disabled = true;
    if (e.flash) e.flash.disabled = true;
    const fail = (msg) => { setState(msg); setProgress(false); };
    try {
      if (typeof pausePoller === 'function') pausePoller();

      // 1. Pages ↔ API alignment: refuse to ship yesterday's image under a fresh tag.
      setProgress(true, T('upChecking'));
      let vj = null;
      try { const r = await pagesFetch(UPDATE_VERSION_JSON); if (r.ok) vj = await r.json(); } catch {}
      if (!vj || !vj.tag || !parseSemver(vj.tag) || cmpSemver(vj.tag, cache.tag) !== 0) return fail(T('upPagesLag'));

      // 2. Checksums — the release must ship a browser-OTA image (releases before it: web flasher).
      let sums = null;
      try { const r = await pagesFetch(UPDATE_SUMS); if (r.ok) sums = parseSha256Sums(await r.text()); } catch {}
      const want = sums && sums.get(UPDATE_OTA_BIN);
      if (!want) return fail(T('upNoOtaAsset'));

      // 3. Download with live byte count (generous timeout — a few MB over the LAN/WAN).
      let bin = null, gotBody = false;
      try {
        const r = await pagesFetch(UPDATE_OTA_BIN, 120000);
        if (r.ok) { gotBody = true; bin = await readAll(r, (n) => setProgress(true, TT('upDownloading', { mb: (n / 1048576).toFixed(1) }))); }
      } catch {}
      if (gotBody && bin === null) return fail(T('upSumsFail'));   // over-size / aborted mid-stream
      if (!bin || !bin.length) return fail(T('upNoOtaAsset'));     // asset absent (older release)

      // 4. Integrity: SHA-256 + the ESP image magic (a cached HTML error page must never reach the flash).
      setProgress(true, T('upVerifying'));
      await new Promise((res) => setTimeout(res, 30));   // let the label paint before the sync hash
      if (bin[0] !== 0xe9 || sha256Hex(bin) !== want) return fail(T('upSumsFail'));

      // 5. POST to the device. Own AbortSignal: the app's serial fetch gate adds a 20 s timeout to
      // /api/* requests without one, and a real flash takes far longer. 503 = the arbiter is busy
      // with another heavy job — honour Retry-After a few times, then give up honestly.
      setProgress(true, T('upFlashing'));
      let resp = null;
      for (let attempt = 0; attempt < 3; attempt++) {
        try { resp = await fetch('/api/ota', { method: 'POST', body: bin, signal: AbortSignal.timeout(300000) }); }
        catch { return fail(T('upFailTimeout')); }
        if (resp.status !== 503) break;
        setState(T('upBusy'));
        const wait = Math.max(2, parseInt(resp.headers.get('Retry-After') || '3', 10) || 3);
        await new Promise((res) => setTimeout(res, wait * 1000));
      }
      if (!resp) return fail(T('upFailTimeout'));
      if (resp.status === 401) return fail(T('upAuthFail'));
      if (resp.status === 503) return fail(T('upBusy'));
      if (!resp.ok) return fail(T('upFailTimeout'));

      // 6. The device reboots into the new slot. Poll until the reported version matches the tag.
      // The first seconds can still answer with the OLD image (response sent before esp_restart),
      // so early samples are only trusted for success, never for a rollback verdict.
      setProgress(true, T('upWaitReboot'));
      const t0 = Date.now();
      for (;;) {
        if (Date.now() - t0 > 180000) return fail(T('upFailTimeout'));
        await new Promise((res) => setTimeout(res, 5000));
        let s = null;
        try { const r = await fetch('/api/status', { cache: 'no-store', signal: AbortSignal.timeout(4000) }); if (r.ok) s = await r.json(); } catch {}
        if (!s || !s.version) continue;
        const c = cmpSemver(s.version, cache.tag);
        if (c === 0) { setProgress(false); setState(TT('upDone', { tag: cache.tag })); render(); return; }
        // Old version, well past the reboot window → the bootloader rolled the image back.
        if (c < 0 && Date.now() - t0 > 45000) return fail(T('upFailTimeout'));
      }
    } finally {
      busy = false;
      if (e.check) e.check.disabled = false;
      if (e.flash) e.flash.disabled = false;
      disarmConfirm();
      if (typeof resumePoller === 'function') resumePoller();
    }
  }

  // ---- wiring ------------------------------------------------------------
  const e = els();
  if (e.check) e.check.addEventListener('click', () => check(true));
  if (e.flash) e.flash.addEventListener('click', () => {
    if (busy) return;
    if (!confirmArmed) { confirmArmed = true; if (e.flashLbl) e.flashLbl.textContent = T('upConfirm'); return; }
    disarmConfirm();
    doFlash();
  });
  if (e.auto) e.auto.addEventListener('change', () => {
    try { localStorage.setItem(LS_ENABLED, e.auto.checked ? '1' : '0'); } catch {}
  });

  return { check, render };
}

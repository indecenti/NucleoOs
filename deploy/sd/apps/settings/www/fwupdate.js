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
  LS_CACHE, LS_ENABLED, LS_AUTO, LS_AUTOSTART, UPDATE_TTL_MS, UPDATE_PAGES_BASE, UPDATE_OTA_BIN, UPDATE_SUMS,
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
    progBar: $('up-prog-bar'), progLbl: $('up-prog-lbl'), auto: $('up-auto'), autoInstall: $('up-auto-install'),
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
    if (e.autoInstall) e.autoInstall.checked = localStorage.getItem(LS_AUTO) === '1';
    renderSdc();
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

      // 0. Installed by M5Launcher (or on a layout that isn't ours): the device refuses self-OTA — say so up
      // front instead of downloading a few MB into a guaranteed 409. docs/m5launcher.md. And a device still
      // confirming a PREVIOUS update (deferred confirm, ~20 s after a restart) can't take another yet: wait.
      setProgress(true, T('upChecking'));
      for (let i = 0; i < 20; i++) {
        let st = null;
        try { const r = await fetch('/api/status', { cache: 'no-store', signal: AbortSignal.timeout(4000) }); st = r.ok ? await r.json() : null; } catch {}
        if (st && st.ota && st.ota.self_update === false) return fail(T(st.ota.host === 'other' ? 'upOtherLayout' : 'upHosted'));
        if (!(st && st.ota && st.ota.state === 'pending')) break;
        setProgress(true, T('upPrevPending'));
        await new Promise((res) => setTimeout(res, 3000));
      }

      // 1. Pages ↔ API alignment: refuse to ship yesterday's image under a fresh tag.
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
      let refusal = null;   // the device's 409 reason: hosted | layout | pending
      for (let attempt = 0; attempt < 4; attempt++) {
        try { resp = await fetch('/api/ota', { method: 'POST', body: bin, signal: AbortSignal.timeout(300000) }); }
        catch { return fail(T('upFailTimeout')); }
        refusal = null;
        if (resp.status === 409) { try { refusal = (await resp.clone().json()).error || null; } catch {} }
        if (resp.status !== 503 && refusal !== 'pending') break;
        setState(T(refusal === 'pending' ? 'upPrevPending' : 'upBusy'));
        const wait = Math.max(2, parseInt(resp.headers.get('Retry-After') || '3', 10) || 3);
        await new Promise((res) => setTimeout(res, wait * 1000));
      }
      if (!resp) return fail(T('upFailTimeout'));
      if (resp.status === 401) return fail(T('upAuthFail'));
      if (resp.status === 503) return fail(T('upBusy'));
      if (resp.status === 409) return fail(T(refusal === 'layout' ? 'upOtherLayout' : refusal === 'pending' ? 'upPrevPending' : 'upHosted'));
      if (!resp.ok) return fail(T('upFailTimeout'));

      // 6. The device reboots into the new slot. Poll until the reported version matches the tag AND the
      // device has CONFIRMED the image: it does so only after the new OS has run for ~20 s (deferred confirm,
      // main.c) — before that a crash, or a power cut, still rolls it back, so "updated" would be premature.
      // The first seconds can still answer with the OLD image (response sent before esp_restart), so early
      // samples are only trusted for success, never for a rollback verdict.
      setProgress(true, T('upWaitReboot'));
      const t0 = Date.now();
      let sawNew = false;
      for (;;) {
        if (Date.now() - t0 > 240000) return fail(T('upFailTimeout'));
        await new Promise((res) => setTimeout(res, 5000));
        let s = null;
        try { const r = await fetch('/api/status', { cache: 'no-store', signal: AbortSignal.timeout(4000) }); if (r.ok) s = await r.json(); } catch {}
        if (!s || !s.version) continue;
        const c = cmpSemver(s.version, cache.tag);
        if (c === 0 && s.ota && s.ota.state === 'pending') { sawNew = true; setProgress(true, T('upConfirming')); continue; }
        // The new image ran, then the device came back on the old one: the bootloader rolled it back.
        if (c < 0 && sawNew) return fail(T('upRolledBack'));
        if (c === 0) {
          setProgress(false); setState(TT('upDone', { tag: cache.tag })); render();
          // The new firmware expects the SD content of its own release: if the card is behind, say so where the
          // fix is, instead of leaving an old web OS under a new firmware (the device can only serve what's there).
          if (s.sdc && s.sdc.needed) {
            sdcAfterFlash = true; renderSdc();
            try { const sect = $('sdc-sect'); if (sect) sect.scrollIntoView({ behavior: 'smooth', block: 'center' }); } catch {}
          }
          return;
        }
        // Old version, well past the reboot window → the bootloader rolled the image back.
        if (c < 0 && Date.now() - t0 > 45000) return fail(T('upRolledBack'));
      }
    } finally {
      busy = false;
      if (e.check) e.check.disabled = false;
      if (e.flash) e.flash.disabled = false;
      disarmConfirm();
      if (typeof resumePoller === 'function') resumePoller();
    }
  }

  // ---- SD content: the card's web OS, apps and ANIMA knowledge ----------------------------------------
  // /api/status .sdc = { status, card, fw, needed, armed } (nucleo_sdc_status_t codes) and .sdc_diag = why the
  // last install failed. POST /api/sdcontent/arm reboots the device into its lean install boot, which fetches
  // ONLY the changed files from the official site, verifies each against the release manifest (SHA-256) and
  // reboots back; the user's data, keys and settings are never part of the payload. The browser plays no part
  // in the transfer — it only arms it, then waits for the device to come back and reports the verdict.
  const SDC = { NO_SD: 0, MISSING: 1, PARTIAL: 2, OUTDATED: 3, SKIPPED: 4, COMPLETE: 5, MANUAL: 6, UNKNOWN: 7 };
  const SDC_WAIT_MS = 45 * 60 * 1000;   // a first install is ~70 MB over the device's own Wi-Fi: be patient
  let sdcBusy = false, sdcConfirm = false, sdcAfterFlash = false, sdcConfirmTimer = 0;
  let sdcResult = null;   // { msg, until }: the verdict of the last run stays readable over the routine state line
  const SDC_RESULT_MS = 2 * 60 * 1000;
  const sdcEls = () => ({ card: $('sdc-card'), fw: $('sdc-fw'), state: $('sdc-state'), go: $('sdc-go'),
    goLbl: $('sdc-go-lbl'), prog: $('sdc-prog'), progLbl: $('sdc-prog-lbl'), sect: $('sdc-sect') });
  const vtag = (v) => (v ? 'v' + String(v).replace(/^v/, '') : '—');
  let sdcLabel = 'sdcUpdate';

  function renderSdc() {
    if (sdcBusy) return;
    const e = sdcEls(); if (!e.state) return;
    const st = getStatus(), s = st && st.sdc;
    if (!s || typeof s.status !== 'number') {   // a firmware older than this tab: say nothing rather than guess
      if (e.sect) e.sect.hidden = !!st;
      return;
    }
    if (e.sect) e.sect.hidden = false;
    e.card.textContent = s.card ? vtag(s.card) : '—';
    e.fw.textContent = vtag(s.fw);
    const apOnly = !!(st.network && st.network.mode === 'ap');
    let msg, canGo = true, primary = false;
    sdcLabel = 'sdcUpdate';
    switch (s.status) {
      case SDC.COMPLETE:
      case SDC.MANUAL:   msg = TT(s.status === SDC.MANUAL ? 'sdcManual' : 'sdcOk', { tag: vtag(s.card || s.fw) }); sdcLabel = 'sdcRepair'; break;
      case SDC.OUTDATED: msg = TT('sdcOutdated', { card: vtag(s.card), fw: vtag(s.fw) }); primary = true; break;
      case SDC.PARTIAL:  msg = T('sdcPartial'); primary = true; sdcLabel = 'sdcResume'; break;
      case SDC.MISSING:  msg = T('sdcMissing'); primary = true; sdcLabel = 'sdcInstall'; break;
      case SDC.SKIPPED:  msg = T('sdcSkipped'); sdcLabel = 'sdcInstall'; break;
      case SDC.UNKNOWN:  msg = T('sdcUnknown'); sdcLabel = 'sdcRepair'; break;
      default:           msg = T('sdcNoSd'); canGo = false;
    }
    if (st.sdc_diag) msg += ' ' + TT('sdcLastFail', { why: st.sdc_diag });
    if (canGo && apOnly) msg += ' ' + T('sdcNeedNet');
    if (sdcAfterFlash && primary) msg = T('sdcAfterFlash') + ' ' + msg;
    if (sdcResult && Date.now() < sdcResult.until) msg = sdcResult.msg;
    e.state.textContent = sdcConfirm ? T('sdcConfirmHint') : msg;
    if (e.go) {
      e.go.hidden = !canGo;
      e.go.disabled = apOnly;
      e.go.classList.toggle('primary', primary);
    }
    if (e.goLbl) e.goLbl.textContent = T(sdcConfirm ? 'sdcConfirm' : sdcLabel);
  }

  function sdcProgress(on, label) {
    const e = sdcEls();
    if (e.prog) e.prog.hidden = !on;
    if (label != null && e.progLbl) e.progLbl.textContent = label;
  }

  async function doSdcUpdate() {
    if (sdcBusy || busy) return;
    sdcBusy = true;
    const e = sdcEls();
    if (e.go) e.go.disabled = true;
    const done = (msg) => {
      sdcProgress(false);
      sdcResult = { msg, until: Date.now() + SDC_RESULT_MS };
      if (e.state) e.state.textContent = msg;
    };
    try {
      if (typeof pausePoller === 'function') pausePoller();
      sdcProgress(true, T('sdcArming'));
      // 409 "starting" = the device is a few seconds into a restart after a firmware update and must settle
      // first (restarting it now would undo that update): it says when to retry.
      let r = null;
      for (let attempt = 0; attempt < 6; attempt++) {
        r = null;
        try { r = await fetch('/api/sdcontent/arm', { method: 'POST', headers: { 'content-type': 'application/json' },
          body: '{"on":true}', signal: AbortSignal.timeout(15000) }); } catch {}
        if (!r || r.status !== 409) break;
        const wait = Math.max(2, parseInt(r.headers.get('Retry-After') || '4', 10) || 4);
        await new Promise((res) => setTimeout(res, wait * 1000));
      }
      if (!r) return done(T('sdcArmFail'));
      if (r.status === 401) return done(T('upAuthFail'));
      if (r.status === 404) return done(T('sdcOldFw'));
      if (!r.ok) return done(T('sdcArmFail'));
      if (e.state) e.state.textContent = '';   // the progress line below tells the story from here

      // The device reboots into the install boot (no web server there) and comes back when the download is
      // over. "Back" = it answered after we lost it, or its uptime says it restarted after we armed.
      const t0 = Date.now();
      let lost = false;
      for (;;) {
        const el = Date.now() - t0;
        if (el > SDC_WAIT_MS) return done(T('sdcTimeout'));
        await new Promise((res) => setTimeout(res, 5000));
        const secs = Math.floor(el / 1000), time = `${Math.floor(secs / 60)}:${String(secs % 60).padStart(2, '0')}`;
        sdcProgress(true, lost ? TT('sdcDownloading', { time }) : T('sdcRebooting'));
        let s = null;
        try { const q = await fetch('/api/status', { cache: 'no-store', signal: AbortSignal.timeout(4000) }); if (q.ok) s = await q.json(); } catch {}
        if (!s) { lost = true; continue; }
        const restarted = typeof s.uptime_s === 'number' && s.uptime_s * 1000 < Date.now() - t0 - 3000;
        if (!(lost || restarted) || !s.sdc || s.sdc.armed) continue;
        if (s.sdc.status === SDC.COMPLETE) { sdcAfterFlash = false; return done(TT('sdcDone', { tag: vtag(s.sdc.card || s.sdc.fw) })); }
        return done(s.sdc_diag ? TT('sdcFailed', { why: s.sdc_diag }) : T('sdcNotFinished'));
      }
    } finally {
      sdcBusy = false;
      sdcConfirm = false;
      if (e.go) e.go.disabled = false;
      if (typeof resumePoller === 'function') resumePoller();
    }
  }

  // ---- wiring ------------------------------------------------------------
  const e = els();
  const se = sdcEls();
  if (se.go) se.go.addEventListener('click', () => {
    if (sdcBusy || busy) return;
    if (!sdcConfirm) {   // two steps: the device restarts and is away for minutes — say so before it happens
      sdcConfirm = true;
      sdcResult = null;
      renderSdc();
      clearTimeout(sdcConfirmTimer);
      sdcConfirmTimer = setTimeout(() => { if (!sdcBusy && sdcConfirm) { sdcConfirm = false; renderSdc(); } }, 15000);
      return;
    }
    clearTimeout(sdcConfirmTimer);
    doSdcUpdate();
  });
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
  if (e.autoInstall) e.autoInstall.addEventListener('change', () => {
    try { localStorage.setItem(LS_AUTO, e.autoInstall.checked ? '1' : '0'); } catch {}
  });

  // Auto-update hand-off: the shell's cancellable countdown set this tag; run the normal, rollback-aware
  // install now (check first so we only flash a genuinely newer release). One-shot — cleared immediately.
  try {
    const want = localStorage.getItem(LS_AUTOSTART);
    if (want) {
      localStorage.removeItem(LS_AUTOSTART);
      check(true).then(() => { const x = els(); if (x.flash && !x.flash.hidden && !busy) { disarmConfirm(); doFlash(); } }).catch(() => {});
    }
  } catch {}

  return { check, render, renderSdc };
}

/* SPDX-License-Identifier: AGPL-3.0-or-later */
// update-check.js — passive "new NucleoOS release is out" notifier (shell side).
// The BROWSER asks GitHub for the latest release — at most one conditional request per day per
// browser (ETag → a 304 is free against the unauthenticated rate limit) — and compares it with the
// firmware version the shell already holds from its /api/status poll. Zero extra device traffic.
// Every failure path is silent by design: offline, rate-limited or GitHub-down must never surface
// an error toast. The active surface (check now / install) is the Settings app's Updates tab.

import I18N from './nucleo-i18n.js';
import {
  LS_CACHE, LS_NOTIFIED, LS_ENABLED, LS_BRIDGED, LS_SKIP, UPDATE_TTL_MS,
  checkDue, decideNotify, fetchLatestRelease, parseReleaseType, updateLevel, cmpSemver,
} from './update-core.js';

const t = I18N.scope('shell');
const START_DELAY = 15000;        // never compete with the session restore + fsindex SD crawl
const RETRY_DELAY = 60000;        // status snapshot / Notify module not ready yet → try again
const RECHECK_EVERY = 6 * 3600 * 1000;   // long-lived tabs re-evaluate the 24h TTL a few times a day

const readJSON = (k) => { try { return JSON.parse(localStorage.getItem(k)); } catch { return null; } };
const writeJSON = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} };

export function initUpdateCheck({ getSnapVersion, getNotify, onState }) {
  let timer = null;
  const schedule = (ms) => { clearTimeout(timer); timer = setTimeout(run, ms); };

  // Persistent "update available" state the shell paints as a taskbar pill + a Start badge (and a
  // What's-new sheet), so a new release is always in view — not only a notification that can be missed.
  // Recomputed from the cache + the live firmware version on every check AND whenever the status version
  // changes (refresh), so the pill clears the moment an install lands. A skipped tag hides it until a
  // newer one appears. Pure read of what fetchLatestRelease already cached — no extra network.
  let lastState = null;
  function computeState(version) {
    const cache = readJSON(LS_CACHE);
    const skip = (() => { try { return localStorage.getItem(LS_SKIP); } catch { return null; } })();
    const available = !!(version && cache && cache.tag && cmpSemver(cache.tag, version) > 0 && skip !== cache.tag);
    const lvl = available ? updateLevel(parseReleaseType(cache.notes)) : 'info';
    return { available, tag: available ? cache.tag : '', level: lvl, notes: available ? (cache.notes || '') : '', url: available ? (cache.url || '') : '' };
  }
  function emitState(version) {
    if (!onState) return;
    const st = computeState(version);
    const key = st.available + '|' + st.tag + '|' + st.level;
    if (key === lastState) return;   // only on an actual change
    lastState = key; try { onState(st); } catch {}
  }

  async function run() {
    try {
      if (localStorage.getItem(LS_ENABLED) === '0') { emitState(getSnapVersion()); schedule(RECHECK_EVERY); return; }
      const version = getSnapVersion();
      if (!version) { schedule(RETRY_DELAY); return; }               // no /api/status version yet — nothing to compare

      let cache = readJSON(LS_CACHE);
      if (checkDue({ cache, nowMs: Date.now(), ttlMs: UPDATE_TTL_MS })) {
        const res = await fetchLatestRelease(fetch, cache, Date.now());
        if (res.ok) { cache = res.cache; writeJSON(LS_CACHE, cache); }
        // !ok → keep whatever we knew; stay silent.
      }
      emitState(version);   // the taskbar pill / Start badge only need the version + cache — NOT Notify
      const Notify = getNotify();
      if (!Notify) { schedule(RETRY_DELAY); return; }                // notifications need notify.js; the pill is already up
      if (cache && cache.tag) {
        const notified = readJSON(LS_NOTIFIED) || [];
        const d = decideNotify({ currentVer: version, latestTag: cache.tag, notifiedTags: notified });
        const type = parseReleaseType(cache.notes);   // "security" rings louder (see updateLevel)
        const lvl = updateLevel(type);
        // Bridge to the NATIVE boot dialog: the device can't do HTTPS to GitHub, so write what the
        // browser learned to SD. The firmware reads /system/config/update.json at boot (zero TLS)
        // and shows the update dialog (the newer of this file and its own NVS check wins). Best-effort —
        // a write failure just means no native dialog. Written once per learned tag, not on every 6 h
        // re-check: the card is shared with the web OS and an unchanged file is pure flash wear.
        if (d.newer && readJSON(LS_BRIDGED) !== cache.tag) {
          try {
            import('./nucleo-fs.js').then(({ default: fs }) => fs.writeJSON('/system/config/update.json',
              { tag: cache.tag, type, notes: String(cache.notes || '').slice(0, 200) }))
              .then(() => writeJSON(LS_BRIDGED, cache.tag))
              .catch(() => {});
          } catch {}
        }
        if (d.notify) {
          Notify.emit({
            id: 'update-' + cache.tag, src: 'ota', lvl: lvl, icon: lvl === 'warn' ? '⚠️' : '⬆️',
            title: lvl === 'warn' ? t('up_title_sec') : t('up_title'),
            body: t('up_body', { tag: cache.tag }),
            action: 'app:settings@updates',
          });
          writeJSON(LS_NOTIFIED, notified.concat(cache.tag).slice(-8));
        }
      }
      emitState(version);   // keep the taskbar pill / Start badge in step with what we just learned
    } catch (e) { console.warn('[update] check failed', e); }
    schedule(RECHECK_EVERY);
  }

  schedule(START_DELAY);
  // refresh(version): recompute the pill state from the cached release + a (possibly new) firmware
  // version, with no network — the shell calls it when /api/status reports a different version, so an
  // install clears the pill at once. skip(tag): user chose to skip; hide the pill until a newer release.
  return {
    runNow: run,
    refresh: (version) => emitState(version),
    skip: (tag, version) => { try { localStorage.setItem(LS_SKIP, tag); } catch {} lastState = null; emitState(version); },
  };
}

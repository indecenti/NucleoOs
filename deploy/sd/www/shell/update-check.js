// update-check.js — passive "new NucleoOS release is out" notifier (shell side).
// The BROWSER asks GitHub for the latest release — at most one conditional request per day per
// browser (ETag → a 304 is free against the unauthenticated rate limit) — and compares it with the
// firmware version the shell already holds from its /api/status poll. Zero extra device traffic.
// Every failure path is silent by design: offline, rate-limited or GitHub-down must never surface
// an error toast. The active surface (check now / install) is the Settings app's Updates tab.

import I18N from './nucleo-i18n.js';
import {
  LS_CACHE, LS_NOTIFIED, LS_ENABLED, UPDATE_TTL_MS,
  checkDue, decideNotify, fetchLatestRelease,
} from './update-core.js';

const t = I18N.scope('shell');
const START_DELAY = 15000;        // never compete with the session restore + fsindex SD crawl
const RETRY_DELAY = 60000;        // status snapshot / Notify module not ready yet → try again
const RECHECK_EVERY = 6 * 3600 * 1000;   // long-lived tabs re-evaluate the 24h TTL a few times a day

const readJSON = (k) => { try { return JSON.parse(localStorage.getItem(k)); } catch { return null; } };
const writeJSON = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} };

export function initUpdateCheck({ getSnapVersion, getNotify }) {
  let timer = null;
  const schedule = (ms) => { clearTimeout(timer); timer = setTimeout(run, ms); };

  async function run() {
    try {
      if (localStorage.getItem(LS_ENABLED) === '0') { schedule(RECHECK_EVERY); return; }
      const version = getSnapVersion();
      const Notify = getNotify();
      if (!version || !Notify) { schedule(RETRY_DELAY); return; }   // first /api/status or notify.js still loading

      let cache = readJSON(LS_CACHE);
      if (checkDue({ cache, nowMs: Date.now(), ttlMs: UPDATE_TTL_MS })) {
        const res = await fetchLatestRelease(fetch, cache, Date.now());
        if (res.ok) { cache = res.cache; writeJSON(LS_CACHE, cache); }
        // !ok → keep whatever we knew; stay silent.
      }
      if (cache && cache.tag) {
        const notified = readJSON(LS_NOTIFIED) || [];
        const d = decideNotify({ currentVer: version, latestTag: cache.tag, notifiedTags: notified });
        // Bridge to the NATIVE boot dialog: the device can't do HTTPS to GitHub, so write what the
        // browser learned to SD. The firmware reads /system/config/update.json at boot (zero TLS)
        // and shows the update dialog. Best-effort — a write failure just means no native dialog.
        if (d.newer) {
          try {
            fetch('/api/fs/write?path=' + encodeURIComponent('/system/config/update.json'),
              { method: 'POST', body: JSON.stringify({ tag: cache.tag, notes: String(cache.notes || '').slice(0, 200) }) })
              .catch(() => {});
          } catch {}
        }
        if (d.notify) {
          Notify.emit({
            id: 'update-' + cache.tag, src: 'ota', lvl: 'info', icon: '⬆️',
            title: t('up_title'),
            body: t('up_body', { tag: cache.tag }),
            action: 'app:settings@updates',
          });
          writeJSON(LS_NOTIFIED, notified.concat(cache.tag).slice(-8));
        }
      }
    } catch (e) { console.warn('[update] check failed', e); }
    schedule(RECHECK_EVERY);
  }

  schedule(START_DELAY);
  return { runNow: run };
}

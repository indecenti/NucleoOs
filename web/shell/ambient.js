// ambient.js — Proactive ANIMA: the OS speaks first, sparingly.
//
// The notification backbone has carried a src:'anima' tag, a sparkle icon and an 'anima:<query>'
// click-through since day one (notify.js) — and nothing has ever emitted on that channel. This
// module lights it up, with deliberately FEW rules, because the failure mode is social, not
// technical: one dumb or repeated notification and the user reaches for DND, and the feature is
// dead. Hence: two high-signal rules, a hard per-rule cooldown, stable ids (notify.js coalesces
// repeats by id), and one master switch (the ✨ button in the Notification Center).
//
// Everything runs on data the browser already holds: the calendar JSON (ONE paced read per
// session, re-read only when its fs.changed says so) and the shell's existing 15 s /api/status
// snapshot. Rule evaluation itself costs the device nothing.
//
// The rules are PURE exported functions returning notification DESCRIPTORS — strings are i18n
// keys + params, resolved only at emit time — so the anti-regression tests run them in node
// with no DOM, no i18n catalogues and no device (tools/shell-ambient.test.mjs).

import I18N from './nucleo-i18n.js';

export const LS_ON = 'nucleo.ambient';            // '0' = off, anything else = on (default on)
export const LS_FIRED = 'nucleo.ambient.fired';   // { id: epochMs } — the cooldown ledger
export const CAL_PATH = '/system/config/calendar.json';
export const LEAD_MIN = 30;                       // calendar lead-in window (minutes)
export const SD_MIN_FRACTION = 0.05;              // below this free/total the SD counts as full
export const SD_COOLDOWN_MS = 24 * 3600 * 1000;   // nag at most once a day
export const CHECK_MS = 5 * 60 * 1000;            // evaluation is in-browser and cheap
const LEDGER_TTL_MS = 7 * 24 * 3600 * 1000;       // fired ids older than this are pruned (ids
                                                  // embed the event date, so they cannot recur)

// ── the rules (pure) ──────────────────────────────────────────────────────────────────────────

// Calendar lead-in: an event starting within the next LEAD_MIN minutes gets ONE advance notice.
// The device already fires calendar.reminder AT event time — this is the heads-up before it.
// All-day events (no time) get none: there is no "about to start" for them.
export function ruleCalendarLeadIn(events, now = new Date()) {
  const out = [];
  if (!events || typeof events !== 'object') return out;
  const pad = (x) => String(x).padStart(2, '0');
  const key = now.getFullYear() + '-' + pad(now.getMonth() + 1) + '-' + pad(now.getDate());
  for (const ev of (Array.isArray(events[key]) ? events[key] : [])) {
    const m = /^(\d{2}):(\d{2})$/.exec((ev && ev.time) || ''); if (!m) continue;
    const at = new Date(now); at.setHours(+m[1], +m[2], 0, 0);
    const lead = (at - now) / 60000;
    if (lead <= 0 || lead > LEAD_MIN) continue;
    const text = String(ev.text || '').trim(); if (!text) continue;
    out.push({
      id: 'ambient.cal.' + key + '.' + ev.time + '.' + text.slice(0, 24),
      once: true,                                   // one lead-in per event, ever
      n: { src: 'anima', lvl: 'info', title: text,
           bodyKey: 'amb_cal_body', bodyParams: { time: ev.time },
           action: 'app:calendar', sound: 'info' },
    });
  }
  return out;
}

// SD nearly full: the one system condition worth speaking up about, because the user can act on
// it and everything on this OS (recordings, photos, apps) degrades when the SD is full.
export function ruleStorageLow(snap) {
  const st = snap && snap.storage;
  if (!st || !st.mounted || !(st.total_bytes > 0)) return null;
  const frac = st.free_bytes / st.total_bytes;
  if (!(frac < SD_MIN_FRACTION)) return null;
  return {
    id: 'ambient.storage', cooldownMs: SD_COOLDOWN_MS,
    n: { src: 'anima', lvl: 'warn', titleKey: 'amb_sd_title',
         bodyKey: 'amb_sd_body', bodyParams: { pct: Math.max(0, Math.round(frac * 100)) },
         action: 'app:system-monitor', sound: 'warn' },
  };
}

// ── the runner (deps injected, so the whole thing stays testable) ─────────────────────────────
export function createAmbient({ emit, readJSON, getStatusSnap, store, translate, now } = {}) {
  const mem = store || ((typeof localStorage !== 'undefined') ? localStorage : new Map());
  const get = (k) => (mem.getItem ? mem.getItem(k) : mem.get(k));
  const set = (k, v) => (mem.setItem ? mem.setItem(k, v) : mem.set(k, v));
  const t = translate || I18N.scope('shell');
  const clock = now || (() => new Date());
  let cal = null, calFetched = false;               // session cache; dropped on its fs.changed
  let timer = null;

  const enabled = () => (get(LS_ON) || '1') !== '0';
  const setEnabled = (on) => { set(LS_ON, on ? '1' : '0'); if (on) start(); else stop(); };

  const ledger = () => { try { return JSON.parse(get(LS_FIRED)) || {}; } catch { return {}; } };
  const saveLedger = (led, nowMs) => {
    for (const k of Object.keys(led)) if (nowMs - led[k] > LEDGER_TTL_MS) delete led[k];
    try { set(LS_FIRED, JSON.stringify(led)); } catch {}
  };

  // Descriptor → concrete notification: i18n resolved HERE, at emit time, so it follows the
  // live OS language; the rules stay data-only.
  const localize = (d) => {
    const n = { id: d.id, ...d.n };
    if (n.titleKey) { n.title = t(n.titleKey, n.titleParams); delete n.titleKey; delete n.titleParams; }
    if (n.bodyKey) { n.body = t(n.bodyKey, n.bodyParams); delete n.bodyKey; delete n.bodyParams; }
    return n;
  };

  const fire = (d) => {
    const led = ledger();
    const nowMs = clock().getTime();
    const last = led[d.id] || 0;
    if (d.once && last) return false;
    if (d.cooldownMs && nowMs - last < d.cooldownMs) return false;
    emit(localize(d));
    led[d.id] = nowMs;
    saveLedger(led, nowMs);
    return true;
  };

  async function check() {
    if (!enabled()) return 0;
    let fired = 0;
    if (!calFetched && readJSON) {                  // one paced read per session, not per check
      try { cal = await readJSON(CAL_PATH); } catch { cal = null; }
      calFetched = true;
    }
    for (const d of ruleCalendarLeadIn(cal && cal.events, clock())) if (fire(d)) fired++;
    const d = ruleStorageLow(getStatusSnap && getStatusSnap());
    if (d && fire(d)) fired++;
    return fired;
  }

  // The calendar changed on the SD (any client) → next check re-reads it. Everything else is
  // none of our business: this module must never become a second fs listener.
  const onFsChanged = (path) => { if (typeof path === 'string' && path.endsWith('calendar.json')) { cal = null; calFetched = false; } };

  function start() {
    if (timer || !enabled()) return;
    timer = setInterval(() => { check().catch(() => {}); }, CHECK_MS);
  }
  function stop() { if (timer) { clearInterval(timer); timer = null; } }

  return { check, onFsChanged, start, stop, enabled, setEnabled };
}

// ── shell entry point ─────────────────────────────────────────────────────────────────────────
export function initAmbient(api) {
  const amb = createAmbient(api);
  // First check ~20 s after boot (let the status poll and the SD settle), then every CHECK_MS.
  setTimeout(() => { amb.check().catch(() => {}); amb.start(); }, 20000);
  // The ✨ switch in the Notification Center flips the LS key and announces it here.
  document.addEventListener('nucleo:ambient-toggle', (e) => amb.setEnabled(!!(e.detail && e.detail.on)));
  return amb;
}

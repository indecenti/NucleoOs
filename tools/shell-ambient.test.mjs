// Proactive ANIMA (web/shell/ambient.js) — the first producer on the src:'anima' channel.
//
// The risk this module carries is SOCIAL: one dumb or repeated notification and the user turns
// the feature off forever. These tests fence exactly that — the rules fire only in their narrow
// window, every id fires once (or waits out its cooldown), the master switch really kills it,
// and the device is asked for the calendar once per session, not once per check.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  ruleCalendarLeadIn, ruleStorageLow, createAmbient,
  LS_ON, LS_FIRED, CAL_PATH, LEAD_MIN, SD_COOLDOWN_MS,
} from '../web/shell/ambient.js';

// A fixed "now": 2026-08-18 10:00 local.
const NOW = new Date(2026, 7, 18, 10, 0, 0);
const TODAY = '2026-08-18';

function memStore(init = {}) {
  const m = new Map(Object.entries(init));
  return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)), m };
}

// ── the rules, pure ───────────────────────────────────────────────────────────────────────────

test('calendar lead-in fires ONLY inside the window: not yet, not passed, not all-day', () => {
  const events = { [TODAY]: [
    { time: '10:20', text: 'Dentist' },        // 20 min ahead → fires
    { time: '11:30', text: 'Too far' },        // 90 min ahead → not yet
    { time: '09:50', text: 'Started' },        // already started → never (the device reminder had it)
    { text: 'All-day thing' },                 // no time → no "about to start" exists
    { time: '10:25', text: '   ' },            // blank text → nothing to say
  ] };
  const out = ruleCalendarLeadIn(events, NOW);
  assert.equal(out.length, 1);
  assert.equal(out[0].n.title, 'Dentist');
  assert.equal(out[0].n.src, 'anima');
  assert.equal(out[0].n.action, 'app:calendar');
  assert.equal(out[0].once, true, 'an event must get ONE lead-in, ever');
  assert.ok(out[0].id.includes(TODAY) && out[0].id.includes('10:20'), 'id must be stable per event: ' + out[0].id);
});

test('calendar lead-in survives garbage data without firing', () => {
  for (const events of [null, undefined, 'x', 42, {}, { [TODAY]: 'not-an-array' }, { [TODAY]: [null, {}, { time: '99:99', text: 'x' }] }]) {
    assert.deepEqual(ruleCalendarLeadIn(events, NOW), [], 'must stay silent on: ' + JSON.stringify(events));
  }
});

test('a different day never leaks into today', () => {
  const out = ruleCalendarLeadIn({ '2026-08-19': [{ time: '10:20', text: 'Tomorrow' }] }, NOW);
  assert.deepEqual(out, []);
});

test('storage rule: only a mounted, genuinely full SD speaks — as a warn, once per day', () => {
  const full = { storage: { mounted: true, total_bytes: 1000, free_bytes: 30 } };   // 3% free
  const d = ruleStorageLow(full);
  assert.equal(d.n.lvl, 'warn');
  assert.equal(d.n.src, 'anima');
  assert.equal(d.n.bodyParams.pct, 3);
  assert.equal(d.cooldownMs, SD_COOLDOWN_MS);
  assert.equal(ruleStorageLow({ storage: { mounted: true, total_bytes: 1000, free_bytes: 100 } }), null, '10% free is not full');
  assert.equal(ruleStorageLow({ storage: { mounted: false, total_bytes: 1000, free_bytes: 0 } }), null, 'unmounted SD must stay silent');
  assert.equal(ruleStorageLow({ storage: { mounted: true, total_bytes: 0, free_bytes: 0 } }), null, 'zero total is sensor garbage, not fullness');
  assert.equal(ruleStorageLow(null), null);
});

// ── the runner ────────────────────────────────────────────────────────────────────────────────

function harness({ events, snap, store, nowRef } = {}) {
  const emitted = [];
  const reads = [];
  const amb = createAmbient({
    emit: (n) => emitted.push(n),
    readJSON: async (p) => { reads.push(p); return events === undefined ? null : { schema: 1, events }; },
    getStatusSnap: () => snap || null,
    store: store || memStore(),
    translate: (k, v) => k + (v ? ':' + JSON.stringify(v) : ''),   // deterministic, no i18n engine
    now: () => (nowRef ? nowRef.d : NOW),
  });
  return { amb, emitted, reads };
}

test('an event fires ONCE — the second check inside the window stays silent', async () => {
  const events = { [TODAY]: [{ time: '10:20', text: 'Dentist' }] };
  const h = harness({ events });
  assert.equal(await h.amb.check(), 1);
  assert.equal(await h.amb.check(), 0, 'the 5-minute timer must not re-nag the same event');
  assert.equal(h.emitted.length, 1);
  const n = h.emitted[0];
  assert.equal(n.title, 'Dentist');
  assert.equal(n.body, 'amb_cal_body:{"time":"10:20"}', 'body must be localized AT EMIT TIME from the key');
  assert.ok(!('bodyKey' in n) && !('bodyParams' in n), 'descriptor internals must not leak into notify');
});

test('the SD nag respects its 24 h cooldown, then may speak again', async () => {
  const snap = { storage: { mounted: true, total_bytes: 1000, free_bytes: 30 } };
  const nowRef = { d: NOW };
  const store = memStore();
  const h = harness({ events: {}, snap, store, nowRef });
  assert.equal(await h.amb.check(), 1);
  nowRef.d = new Date(NOW.getTime() + 3600 * 1000);                    // +1 h → silent
  assert.equal(await h.amb.check(), 0);
  nowRef.d = new Date(NOW.getTime() + 25 * 3600 * 1000);               // +25 h → may nag again
  assert.equal(await h.amb.check(), 1);
});

test('the master switch really kills it: no reads, no emits', async () => {
  const store = memStore({ [LS_ON]: '0' });
  const h = harness({ events: { [TODAY]: [{ time: '10:20', text: 'X' }] }, snap: { storage: { mounted: true, total_bytes: 1000, free_bytes: 1 } }, store });
  assert.equal(await h.amb.check(), 0);
  assert.equal(h.emitted.length, 0);
  assert.equal(h.reads.length, 0, 'off means off — not even the calendar read');
  assert.equal(h.amb.enabled(), false);
});

test('the calendar is read ONCE per session, and re-read only after its fs.changed', async () => {
  const h = harness({ events: {} });
  await h.amb.check();
  await h.amb.check();
  assert.equal(h.reads.length, 1, 'one paced read per session, not one per check');
  h.amb.onFsChanged('/system/config/other.json');
  await h.amb.check();
  assert.equal(h.reads.length, 1, 'a foreign path must not trigger a re-read');
  h.amb.onFsChanged(CAL_PATH);
  await h.amb.check();
  assert.equal(h.reads.length, 2, 'the calendar changed on the SD → next check re-reads it');
});

test('the fired ledger is pruned — it cannot grow for ever', async () => {
  const store = memStore();
  const old = {};                                       // 200 stale ids, all older than the TTL
  for (let i = 0; i < 200; i++) old['ambient.cal.2020-01-01.09:00.' + i] = new Date(2020, 0, 1).getTime();
  store.setItem(LS_FIRED, JSON.stringify(old));
  const h = harness({ events: { [TODAY]: [{ time: '10:20', text: 'Fresh' }] }, store });
  await h.amb.check();
  const led = JSON.parse(store.getItem(LS_FIRED));
  assert.equal(Object.keys(led).length, 1, 'stale entries must be gone, only the fresh id kept: ' + Object.keys(led).length);
});

test('setEnabled(false) parks the timer; setEnabled(true) restarts it', () => {
  const store = memStore();
  const h = harness({ events: {}, store });
  h.amb.setEnabled(false);
  assert.equal(store.getItem(LS_ON), '0');
  h.amb.setEnabled(true);
  assert.equal(store.getItem(LS_ON), '1');
  h.amb.stop();                                         // leave no interval behind in the test runner
});

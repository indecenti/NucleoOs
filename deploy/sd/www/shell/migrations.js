/* SPDX-License-Identifier: AGPL-3.0-or-later */
// migrations.js — one-time moves of stored state, so an update from ANY older web OS (Cardputer, Handheld,
// P4, Web) keeps the user's settings and data. The rule (docs/migrations.md): a change to a stored format
// ships with a migration here and a test in tests/migrations.test.mjs; old data is never deleted.
//
// A migration is { id, scope, description, run(ctx) }:
//   • scope 'device'  — about the device's files; done once per DEVICE, recorded in /system/config/migrations.json
//   • scope 'browser' — about this browser's storage; done once per BROWSER, recorded in localStorage
//                        'nucleo.migrations.v1' (each browser has its own localStorage to lift)
//   • run(ctx) is idempotent and safe to re-run: it checks before it writes, copies and never deletes the source.
//     Returning (anything) means done and records the id; throwing means "try again next boot".
//
// DEVICE DISCIPLINE (the Cardputer has 4 sockets). The shell runs this once per boot, after pairing, with
// the desktop painted. The first device request waits for ctx.beforeDevice() (the shell gives it a few
// seconds), then: ONE read of migrations.json; nothing else if nothing is pending; at most one write of it
// at the end. Browser-only migrations need no request at all.

import Caps from './nucleo-caps.js';
import { kvPath } from './appbroker.js';

export const DEVICE_LOG = '/system/config/migrations.json';
export const BROWSER_KEY = 'nucleo.migrations.v1';
const LANGS = ['it', 'en', 'es', 'fr', 'de'];
const isObj = (x) => x !== null && typeof x === 'object' && !Array.isArray(x);
const parseObj = (text) => { try { const j = JSON.parse(text); return isObj(j) ? j : null; } catch { return null; } };

// ── the registry, in order ────────────────────────────────────────────────────────────────────────────
export const MIGRATIONS = [
  {
    id: 'browser-settings-hh-key',
    scope: 'browser',
    description: "Browser app: the Handheld kept its settings under 'nucleo_browser_settings' (it renamed the key " +
      "and deleted 'aegis_browser_settings'); the shared app reads 'aegis_browser_settings'. Copy when missing.",
    run(ctx) {
      const old = ctx.storage.get('nucleo_browser_settings');
      if (old == null) return 'nothing';
      if (ctx.storage.get('aegis_browser_settings') != null) return 'kept';   // the shared app already saved its own
      if (!parseObj(old)) return 'unreadable';                                  // the old app would have ignored it too
      if (!ctx.storage.set('aegis_browser_settings', old)) throw new Error('localStorage write refused');
      return 'copied';
    },
  },
  {
    id: 'scoreboard-kv',
    scope: 'browser',
    description: "Scoreboard before 1.1.0 kept its state in this browser's localStorage 'scoreboard.v1' (same " +
      "origin as the shell); 1.1.0 runs sandboxed and reads NucleoApp.kv. Copy it into /data/apps/scoreboard/kv.json " +
      'under the same key when that key is missing; the localStorage copy stays.',
    async run(ctx) {
      const raw = ctx.storage.get('scoreboard.v1');
      if (raw == null) return 'nothing';
      const value = parseObj(raw);
      if (!value) return 'unreadable';                                          // the old app started over too
      return ctx.kvSeed('scoreboard', 'scoreboard.v1', value);                 // 'written' | 'exists' | 'too-large'
    },
  },
  {
    id: 'settings-language',
    scope: 'device',
    description: 'Handheld, P4 and Web: their Settings app read a missing ui.language as Italian (unless the locale ' +
      'was en-*), the shared one reads it as English and rewrites the browser language on open. Store the language ' +
      'the user had in settings.json, so nothing flips.',
    async run(ctx) {
      const info = await ctx.caps.info();
      if (!info || info.source === 'none') throw new Error('edition unknown yet');   // retry when the device answers
      if (!['handheld', 'p4', 'web'].includes(info.edition)) return 'not-needed';    // Cardputer: same rule as now
      const path = '/system/config/settings.json';
      const r = await ctx.fs.read(path);
      if (r.status !== 200 && r.status !== 404) throw new Error('settings.json: http-' + r.status);
      const had = ctx.bootLang && LANGS.includes(ctx.bootLang) ? ctx.bootLang : '';
      let s;
      if (r.status === 404) {
        if (!had) return 'nothing';                      // nothing chosen anywhere: the shared default is fine
        s = {};                                          // none of these backends reads settings.json natively
      } else {
        s = parseObj(r.text);
        if (!s) return 'unreadable';                     // never write over a file we cannot read
      }
      if (isObj(s.ui) && LANGS.includes(s.ui.language)) return 'kept';
      const locale = isObj(s.device) && typeof s.device.locale === 'string' ? s.device.locale : '';
      const lang = had || (/^en/i.test(locale) ? 'en' : 'it');   // what that Settings app showed and wrote
      const next = { ...s, ui: { ...(isObj(s.ui) ? s.ui : {}), language: lang } };
      await ctx.fs.write(path, JSON.stringify(next, null, 2));
      ctx.changed(path);
      return 'set ' + lang;
    },
  },
];

// ── the context a migration gets ──────────────────────────────────────────────────────────────────────
// Every dependency is injectable so the whole thing runs under node --test with a fake device.
export function createContext({
  fetchFn = (...a) => fetch(...a),
  storage = (typeof localStorage !== 'undefined' ? localStorage : null),
  caps = Caps,
  logger = console,
  kvSeed = null,            // the broker's (shell/appbroker.js): same chain and cache as the app's own kv
  beforeDevice = null,      // awaited once, before the first device request
  onChange = null,          // (path) after a migration rewrote a device file
  bootLang,                 // 'anima.lang' as it was before any app window opened
} = {}) {
  let gate = null;
  const ready = () => (gate || (gate = Promise.resolve(beforeDevice ? beforeDevice() : undefined).catch(() => {})));
  const opts = (extra) => {
    const o = { cache: 'no-store', credentials: 'same-origin', ...extra };
    if (typeof AbortSignal !== 'undefined' && AbortSignal.timeout) o.signal = AbortSignal.timeout(8000);
    return o;
  };
  const q = (p) => '?path=' + encodeURIComponent(p);
  const fs = {
    // { status, text }: 404 is an answer (the file is absent), not an error.
    async read(path) {
      await ready();
      const r = await fetchFn('/api/fs/read' + q(path), opts());
      return { status: r.status, text: r.ok ? await r.text() : '' };
    },
    async write(path, body) {
      await ready();
      const r = await fetchFn('/api/fs/write' + q(path), opts({ method: 'POST', body }));
      if (!r.ok) throw new Error('write ' + path + ': http-' + r.status);
    },
    async mkdir(path) {      // not "-p" on every backend, and "exists" is fine: errors are ignored
      await ready();
      try { await fetchFn('/api/fs/mkdir' + q(path), opts({ method: 'POST' })); } catch {}
    },
  };
  const store = {
    get(k) { try { return storage ? storage.getItem(k) : null; } catch { return null; } },
    set(k, v) { try { if (!storage) return false; storage.setItem(k, v); return true; } catch { return false; } },
  };
  // Without the broker (tests, tools): the same rules as kv.set, straight on the file.
  async function seedFile(appId, key, value) {
    const path = kvPath(appId);
    const r = await fs.read(path);
    if (r.status !== 200 && r.status !== 404) throw new Error('kv ' + appId + ': http-' + r.status);
    const data = (r.status === 200 && parseObj(r.text)) || {};   // a damaged file starts over, as in the broker
    if (Object.prototype.hasOwnProperty.call(data, key)) return 'exists';
    const body = JSON.stringify({ ...data, [key]: value });
    if (body.length > 64 * 1024) return 'too-large';
    await fs.mkdir('/data/apps');
    await fs.mkdir('/data/apps/' + appId);
    await fs.write(path, body);
    return 'written';
  }
  const log = {
    info: (...a) => { try { logger.info('[migrations]', ...a); } catch {} },
    warn: (...a) => { try { logger.warn('[migrations]', ...a); } catch {} },
  };
  return {
    fs, storage: store, caps, log, kvPath,
    kvSeed: async (appId, key, value) => { if (kvSeed) { await ready(); return kvSeed(appId, key, value); } return seedFile(appId, key, value); },
    bootLang: bootLang !== undefined ? bootLang : store.get('anima.lang'),
    changed: (path) => { try { if (onChange) onChange(path); } catch {} },
  };
}

// ── the runner ────────────────────────────────────────────────────────────────────────────────────────
function readBrowserLog(ctx) {
  const j = parseObj(ctx.storage.get(BROWSER_KEY) || '');
  return j && isObj(j.done) ? j : { version: 1, done: {} };
}

let started = false;
// Runs every pending migration once. Never throws: a failure is logged, left unrecorded and retried next boot.
// Returns { state, ran: [{id, result}], failed: [{id, error}], skipped: [ids] } for logs and tests.
export async function runMigrations({ ctx, migrations = MIGRATIONS, isPaired, once = true } = {}) {
  const out = { state: 'done', ran: [], failed: [], skipped: [] };
  if (!ctx || !isPaired || !isPaired()) { out.state = 'unpaired'; return out; }   // nothing before pairing, not even a read
  if (once) { if (started) { out.state = 'already-ran'; return out; } started = true; }
  const now = () => new Date().toISOString();
  const browserLog = readBrowserLog(ctx);
  let deviceLog = null;          // { version, done } once read; false when it could not be read
  const deviceNew = {};
  for (const m of migrations) {
    if (m.scope === 'browser') {
      if (browserLog.done[m.id]) continue;
    } else if (m.scope === 'device') {
      if (deviceLog === null) {
        try {
          const r = await ctx.fs.read(DEVICE_LOG);
          if (r.status === 404) deviceLog = { version: 1, done: {} };
          else if (r.status === 200) {
            const j = parseObj(r.text);
            deviceLog = j && isObj(j.done) ? j : { version: 1, done: {} };   // a damaged log: re-run (idempotent), rewrite it
          } else throw new Error('http-' + r.status);
        } catch (e) {
          ctx.log.warn('migrations.json unreadable, device migrations wait for the next boot:', (e && e.message) || e);
          deviceLog = false;
        }
      }
      if (deviceLog === false) { out.skipped.push(m.id); continue; }
      if (deviceLog.done[m.id]) continue;
    } else { out.skipped.push(m.id); continue; }
    try {
      const result = await m.run(ctx);
      out.ran.push({ id: m.id, result: result == null ? 'done' : String(result) });
      ctx.log.info(m.id + ':', result == null ? 'done' : result);
      if (m.scope === 'browser') {
        browserLog.done[m.id] = now();
        if (!ctx.storage.set(BROWSER_KEY, JSON.stringify(browserLog))) ctx.log.warn('could not record', m.id, '(will re-run, harmless)');
      } else deviceNew[m.id] = now();
    } catch (e) {
      out.failed.push({ id: m.id, error: String((e && e.message) || e) });
      ctx.log.warn(m.id + ' failed, retried next boot:', (e && e.message) || e);
    }
  }
  if (Object.keys(deviceNew).length) {
    const next = { ...deviceLog, version: 1, done: { ...deviceLog.done, ...deviceNew } };
    try { await ctx.fs.write(DEVICE_LOG, JSON.stringify(next, null, 2)); }
    catch (e) { ctx.log.warn('could not record', Object.keys(deviceNew).join(', '), '(will re-run, harmless):', (e && e.message) || e); }
  }
  if (out.failed.length) out.state = 'partial';
  return out;
}

// For tests: forget that this page already ran.
export function _resetForTests() { started = false; }

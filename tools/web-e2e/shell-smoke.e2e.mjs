// Browser E2E smoke: boot the real shell in headless Chrome against the device simulator, in every
// OS language, and open EVERY app through the Start menu. An app that throws on load, a shell fault,
// a missing asset or a device route the simulator does not know fails the run — with a screenshot
// under build/web-e2e/. Also enforces the device-load budget of a cold boot (the Cardputer has one
// single-task httpd with 4-6 sockets and ~18 KB of free heap: every request counts).
//   npm run web:e2e                       all 5 languages in parallel
//   E2E_LANGS=it,en npm run web:e2e       a subset
import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim, REPO } from './sim.mjs';
import { LANGS, bootShell, appRows, openAppAt, closeAllWindows, defects } from './shell.mjs';

const langs = (process.env.E2E_LANGS || LANGS.join(',')).split(',').map((s) => s.trim()).filter(Boolean);
const SHOTS = join(REPO, 'build', 'web-e2e');
const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';

// Cold-boot budget, measured on the simulator (server side, so it is what the DEVICE would see).
// Ratchet these DOWN as the boot gets leaner; never raise them to make a regression pass.
// The shell must really BE in the requested language — not flip back to whatever settings.json says.
const catalog = (lang) => JSON.parse(readFileSync(join(REPO, 'web', 'shell', 'i18n', `shell.${lang}.json`), 'utf8'));
const core = (lang) => JSON.parse(readFileSync(join(REPO, 'web', 'shell', 'i18n', `core.${lang}.json`), 'utf8'));
const plain = (s) => String(s).replace(/­/g, '');
const LANG_PROBE = `({ html: document.documentElement.lang, ph: (document.getElementById('sm-search-input') || {}).placeholder })`;
async function assertLang(page, lang, when) {
  const got = await page.eval(LANG_PROBE);
  assert.equal(got.html, lang, `[${lang}] <html lang> is "${got.html}" ${when}`);
  assert.equal(got.ph, catalog(lang).start_search_ph, `[${lang}] Start search placeholder not in ${lang} ${when}`);
  // App names follow the OS language too (the manifest carries one name in one language).
  const names = await page.eval(`({ start: [...document.querySelectorAll('#sm-all .sm-row')].map((r) => r.title.replace(/\u00ad/g, '')),
    desk: [...document.querySelectorAll('#desktop .icon')].map((e) => e.getAttribute('aria-label').replace(/\u00ad/g, '')) })`);
  for (const id of ['settings', 'weather', 'calculator', 'contacts']) {
    const want = plain(catalog(lang)['app_' + id]);
    assert.ok(names.start.includes(want), `[${lang}] Start lacks "${want}" (${id}) ${when}`);
    assert.ok(names.desk.includes(want), `[${lang}] desktop lacks "${want}" (${id}) ${when}`);
  }
}

// 2026-09-30, shell only (no saved windows): 38–42 requests in all five languages, one folder listing (the Desktop).
const BOOT_BUDGET = { requests: 60, peakInFlight: 6, fsLists: 2 };

test('shell smoke — every app opens cleanly in every language', { skip: !findChrome() && 'no Chrome/Edge installed', concurrency: true }, async (t) => {
  // One browser per language: pages sharing a headless browser get background-throttled and run
  // slower in parallel than in series; separate instances really do run side by side.
  await Promise.all(langs.map((lang) => t.test(`[${lang}]`, async (st) => {
    // No saved windows: "cold boot" is the SHELL's cost. With the simulator's dev session the restored apps
    // (Settings, Paint, Mail, Contacts) landed inside or outside the measuring window depending on timing — the
    // same build read 39 or 83. Session restore has its own suite (session-restore.e2e.mjs).
    const sim = await startSim({ seed: { '/system/config/session.json': { windows: [], geom: {} } } });
    const browser = await launchBrowser({ args: [HOST_RULES] });
    try {
      const page = await browser.newPage();
      const bootMark = page.mark();
      assert.ok(await bootShell(page, sim, { lang }), `[${lang}] desktop never came up`);
      const boot = await sim.control('/api/_sim/stats');
      st.diagnostic(`cold boot: ${boot.total} device requests, peak ${boot.peak} in flight`);
      const bootDefects = defects(page.since(bootMark), sim.origin);
      if (bootDefects.length) await page.screenshot(join(SHOTS, `${lang}-boot.png`));
      assert.deepEqual(bootDefects, [], `[${lang}] boot produced defects`);
      assert.ok(boot.total <= BOOT_BUDGET.requests, `[${lang}] cold boot sent ${boot.total} requests to the device (budget ${BOOT_BUDGET.requests})`);
      assert.ok(boot.peak <= BOOT_BUDGET.peakInFlight, `[${lang}] cold boot kept ${boot.peak} requests in flight (budget ${BOOT_BUDGET.peakInFlight})`);
      // The search index is built on the first SEARCH, never at boot: a boot lists the Desktop folder, not /data.
      const lists = (boot.byPath && boot.byPath['/api/fs/list']) || 0;
      assert.ok(lists <= BOOT_BUDGET.fsLists, `[${lang}] cold boot listed ${lists} folders on the device (budget ${BOOT_BUDGET.fsLists}: the Desktop, not a crawl of /data)`);

      await assertLang(page, lang, 'after boot');
      const names = await appRows(page);
      assert.ok(names.length >= 40, `[${lang}] Start menu lists only ${names.length} apps`);
      const failures = [];
      for (let i = 0; i < names.length; i++) {
        const r = await openAppAt(page, i);
        const bad = defects(r.events, sim.origin);
        if (!r.info.opened) bad.push('window did not open');
        else if (r.info.bodyLen === 0) bad.push('window is blank');
        if (r.info.offscreen) bad.push('window opened off-screen');
        if (bad.length) {
          failures.push(`${r.name}: ${[...new Set(bad)].join(' ; ')}`);
          await page.screenshot(join(SHOTS, `${lang}-${String(r.name).replace(/[^\w-]+/g, '_')}.png`));
        }
        await closeAllWindows(page);
      }
      assert.deepEqual(failures, [], `[${lang}] ${failures.length} app(s) misbehaved`);
      await assertLang(page, lang, 'after opening every app');
    } finally {
      await browser.close();
      await sim.stop();
    }
  })));
});

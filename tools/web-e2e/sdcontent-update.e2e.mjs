// Browser E2E — Settings ▸ Updates ▸ SD card content, against the simulator (POST /api/sdcontent/arm and
// /api/status .sdc mirror nucleo_httpd.c). The card's web OS / apps / ANIMA knowledge used to reach a user only
// at first boot: after any firmware update nothing told them the card was behind, and the web console could not
// see the card at all. Guards: the card state is shown in the OS language; the update needs a deliberate second
// click (the device restarts and is away for minutes); it waits through the device's install boot and reports
// the verdict — the new version, or the reason the device persisted — and keeps that verdict readable; a
// hotspot-only device (no internet) cannot start it; a firmware without the route is never offered it.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const { STR } = await import('../../apps/settings/www/i18n.js');

async function openUpdates(browser, sim, lang) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/settings/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  const label = JSON.stringify(STR[lang].tab_updates);
  await page.waitFor(`[...document.querySelectorAll('.nav-item .lbl')].some((l) => l.textContent === ${label})`, { timeout: 45000 });
  await page.eval(`[...document.querySelectorAll('.nav-item')].find((n) => n.querySelector('.lbl').textContent === ${label}).click(), true`);
  return page;
}
const state = (page) => page.eval(`document.getElementById('sdc-state').textContent`);
const fill = (s, vars) => Object.entries(vars).reduce((a, [k, v]) => a.replace(`{${k}}`, v), s);

test('SD content: an outdated card is shown in the OS language and updated after a deliberate second click', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/sdc', { status: 'outdated', card: '0.6.0', install_ms: 6000, fail: false });
  const page = await openUpdates(browser, sim, 'it');
  const it = STR.it;
  const fw = (await sim.control('/api/_sim/sdc', {})).sdc.fw;
  const outdated = fill(it.sdcOutdated, { card: 'v0.6.0', fw: `v${fw}` });
  assert.ok(await page.waitFor(`document.getElementById('sdc-state').textContent === ${JSON.stringify(outdated)}`, { timeout: 15000 }),
    `outdated state in Italian, got: ${await state(page)}`);
  assert.equal(await page.eval(`document.getElementById('sdc-card').textContent`), 'v0.6.0');
  assert.equal(await page.eval(`document.getElementById('sdc-go').classList.contains('primary') && !document.getElementById('sdc-go').hidden`), true);

  // First click only explains; nothing is armed yet.
  await page.eval(`document.getElementById('sdc-go').click(), true`);
  assert.equal(await page.eval(`document.getElementById('sdc-go-lbl').textContent`), it.sdcConfirm);
  assert.equal(await state(page), it.sdcConfirmHint);
  assert.equal((await sim.control('/api/_sim/sdc', {})).sdc.armed, false, 'one click must not arm the device');

  // Second click arms it; the device goes away (install boot) and comes back with the card at this firmware.
  await page.eval(`document.getElementById('sdc-go').click(), true`);
  const done = fill(it.sdcDone, { tag: `v${fw}` });
  assert.ok(await page.waitFor(`document.getElementById('sdc-state').textContent === ${JSON.stringify(done)}`, { timeout: 40000 }),
    `success verdict, got: ${await state(page)}`);
  assert.equal(await page.eval(`document.getElementById('sdc-prog').hidden`), true, 'progress hidden after the verdict');
  // The verdict outlives the next status samples (the poller keeps rendering the routine line underneath).
  await new Promise((r) => setTimeout(r, 7000));
  assert.equal(await state(page), done, 'the verdict stays readable');
  assert.equal(await page.eval(`document.getElementById('sdc-card').textContent`), `v${fw}`);
});

test('SD content: a failed install reports the reason the device persisted, and the card is left as it was', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/sdc', { status: 'outdated', card: '0.6.0', install_ms: 6000, fail: true });
  const page = await openUpdates(browser, sim, 'en');
  await page.waitFor(`!document.getElementById('sdc-go').hidden`, { timeout: 15000 });
  await page.eval(`document.getElementById('sdc-go').click(), document.getElementById('sdc-go').click(), true`);
  const failed = fill(STR.en.sdcFailed, { why: 'Content unreachable (HTTP 404)' });
  assert.ok(await page.waitFor(`document.getElementById('sdc-state').textContent === ${JSON.stringify(failed)}`, { timeout: 40000 }),
    `failure verdict, got: ${await state(page)}`);
  const now = await sim.control('/api/_sim/sdc', {});
  assert.equal(now.sdc.status, 3, 'still outdated');
  assert.equal(now.sdc.card, '0.6.0');
});

test('SD content: a device still settling after a firmware update (409 starting) is waited for, then armed', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/sdc', { status: 'outdated', card: '0.6.0', install_ms: 3000, fail: false });
  const page = await openUpdates(browser, sim, 'en');
  await page.waitFor(`!document.getElementById('sdc-go').hidden`, { timeout: 15000 });
  await sim.control('/api/_sim/sdc', { starting_ms: 5000 });   // restarting now would roll the new firmware back
  await page.eval(`document.getElementById('sdc-go').click(), document.getElementById('sdc-go').click(), true`);
  const fw = (await sim.control('/api/_sim/sdc', {})).sdc.fw;
  const done = fill(STR.en.sdcDone, { tag: `v${fw}` });
  assert.ok(await page.waitFor(`document.getElementById('sdc-state').textContent === ${JSON.stringify(done)}`, { timeout: 45000 }),
    `armed once the device settled, then installed; got: ${await state(page)}`);
});

test('SD content: a hotspot-only device (no internet) cannot start it, and says why', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/sdc', { status: 'missing' });
  await sim.control('/api/_sim/net', { mode: 'ap' });
  const page = await openUpdates(browser, sim, 'en');
  const want = `${STR.en.sdcMissing} ${STR.en.sdcNeedNet}`;
  assert.ok(await page.waitFor(`document.getElementById('sdc-state').textContent === ${JSON.stringify(want)}`, { timeout: 15000 }),
    `hotspot explanation, got: ${await state(page)}`);
  assert.equal(await page.eval(`document.getElementById('sdc-go').disabled`), true, 'the button is disabled');
  assert.equal(await page.eval(`document.getElementById('sdc-go-lbl').textContent`), STR.en.sdcInstall);
  await page.eval(`document.getElementById('sdc-go').click(), document.getElementById('sdc-go').click(), true`);
  await new Promise((r) => setTimeout(r, 1500));
  assert.equal((await sim.control('/api/_sim/sdc', {})).sdc.armed, false, 'a disabled button never arms the device');
});

test('SD content: a firmware without the SD-content API is never offered the update', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/sdc', { status: 'outdated', absent: true });
  const page = await openUpdates(browser, sim, 'en');
  await page.waitFor(`!!document.getElementById('up-cur') && document.getElementById('up-cur').textContent !== '—'`, { timeout: 15000 });
  await new Promise((r) => setTimeout(r, 1500));
  assert.equal(await page.eval(`document.getElementById('sdc-sect').hidden`), true, 'section hidden on an older firmware');
});

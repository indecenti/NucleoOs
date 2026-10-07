// Browser E2E — Voice Commands (voice-manager) against the simulator: the trained commands are the *.tpl
// files in /system/voice (curriculum + custom), deleting one asks first and removes exactly that file, a
// command name is TEXT everywhere (including the inline action buttons), training a custom phrase runs the
// whole device round trip, and failures (unreadable folder, busy engine) never delete or crash anything.
import test from 'node:test';
import assert from 'node:assert/strict';
import { stat } from 'node:fs/promises';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const EN = JSON.parse((await import('node:fs')).readFileSync(new URL('../../apps/voice-manager/www/i18n.en.json', import.meta.url), 'utf8'));
const TPL = '\u0000'.repeat(64);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const exists = (sim, p) => stat(join(sim.sd, p)).then(() => true, () => false);

async function open(browser, sim) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/voice-manager/`);
  assert.ok(await page.waitFor(`typeof window.__t === 'function' && document.querySelectorAll('#curriculum-list .trigger-item').length > 0`, { timeout: 45000 }), 'app started');
  await page.networkIdle({ quiet: 400, timeout: 10000 });
  return page;
}
const tab = (name) => `document.querySelector('.tab-btn[data-tab="${name}"]').click(), true`;
const CUSTOM = `[...document.querySelectorAll('#custom-trigger-list .trigger-item .trigger-name')].map((e) => e.textContent)`;
const customRow = (name) => `[...document.querySelectorAll('#custom-trigger-list .trigger-item')].find((r) => r.querySelector('.trigger-name').textContent === ${JSON.stringify(name)})`;
const requests = (sim) => sim.control('/api/_sim/stats').then((s) => s.byPath);

test('voice-manager: lists the trained commands from /system/voice and deletes only after confirmation', { skip }, async (t) => {
  const sim = await startSim({ seed: {
    '/system/voice/apri musica.tpl': TPL, '/system/voice/accendi luce.tpl': TPL, '/system/voice/notes.txt': 'not a template',
  } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  assert.equal(await page.eval(`document.getElementById('prog-label').textContent`), EN.prog_label.other.replace('{count}', '2'), 'two *.tpl = two trained commands (the .txt is not one)');
  assert.equal(await page.eval(`document.getElementById('prog-fill').style.width`), '10%', '2 of 20 slots');
  const trained = await page.eval(`[...document.querySelectorAll('#curriculum-list .trigger-item.saved .trigger-name')].map((e) => e.textContent)`);
  assert.deepEqual(trained, ['apri musica'], 'the curriculum marks the trained phrase');
  await page.eval(tab('custom'));
  assert.deepEqual(await page.eval(CUSTOM), ['accendi luce'], 'a phrase outside the curriculum is a custom command');

  await t.test('declining the confirmation deletes nothing', async () => {
    await page.eval(`window.__asked = []; window.confirm = (m) => { window.__asked.push(m); return false; }; true`);
    await sim.control('/api/_sim/stats', { reset: true });
    await page.eval(`${customRow('accendi luce')}.querySelector('.act.del').click(), true`);
    await sleep(500);
    assert.deepEqual(await page.eval(`window.__asked`), [EN.confirm_delete.replace('{word}', 'accendi luce')], 'the user is asked, naming the command');
    assert.equal((await requests(sim))['/api/fs/delete'], undefined, 'no delete request was sent');
    assert.ok(await exists(sim, '/system/voice/accendi luce.tpl'), 'the template is still on the SD');
  });

  await t.test('confirming deletes exactly that template and refreshes the lists', async () => {
    await page.eval(`window.confirm = () => true; true`);
    await page.eval(`${customRow('accendi luce')}.querySelector('.act.del').click(), true`);
    assert.ok(await page.waitFor(`document.getElementById('prog-label').textContent === ${JSON.stringify(EN.prog_label.one)}`, { timeout: 10000 }), 'the counter dropped to 1');
    assert.equal(await exists(sim, '/system/voice/accendi luce.tpl'), false, 'the template is gone from the SD');
    assert.ok(await exists(sim, '/system/voice/apri musica.tpl'), 'the other one is untouched');
    assert.ok(await page.waitFor(`document.querySelector('#custom-trigger-list .empty') && document.querySelector('#custom-trigger-list .empty').textContent === ${JSON.stringify(EN.custom_empty)}`, { timeout: 5000 }), 'the custom list is empty again');
  });
});

test('voice-manager: a command name is text everywhere — also inside the Retrain/Delete buttons', { skip }, async (t) => {
  // FAT allows & # ; ' ( ) in names. The inline onclick="startRecord('…')" decoded &#39; back into a quote,
  // breaking the handler — or, with a crafted name, running code.
  const NAMES = ['rock&#39;n roll', "it's mine", 'a&amp;b', 'x&#39;);window.__xss=1;(&#39;'];
  const seed = Object.fromEntries(NAMES.map((n) => [`/system/voice/${n}.tpl`, TPL]));
  const sim = await startSim({ seed });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  await page.eval(tab('custom'));
  assert.deepEqual([...await page.eval(CUSTOM)].sort(), [...NAMES].sort(), 'every name is listed literally');
  const m = page.mark();

  await t.test('Retrain opens the recording wizard for THAT exact phrase', async () => {
    // The failing arm request closes the wizard again within milliseconds — on a fast runner before a
    // 100 ms poll could see it open. Record every moment it IS open instead (a MutationObserver sees them all).
    await page.eval(`(() => {
      window.__shown = [];
      const seen = () => { const p = document.getElementById('panel-record'); if (p && p.classList.contains('active')) window.__shown.push(document.getElementById('display-word').textContent); };
      new MutationObserver(seen).observe(document.body, { subtree: true, attributes: true, childList: true, characterData: true });
      return true;
    })()`);
    for (const n of NAMES) {
      // keep the device out of it: this ONE arm request fails (one-shot fault) → the wizard closes again
      await sim.control('/api/_sim/fault', { route: '/api/voice/learn', status: 503, times: 1 });
      await page.eval(`window.__shown.length = 0, ${customRow(n)}.querySelector('.act:not(.del)').click(), true`);
      const shown = await page.waitFor(`window.__shown.includes(${JSON.stringify(n)}) ? ${JSON.stringify(n)} : (window.__shown.length && window.__shown[window.__shown.length - 1])`, { timeout: 8000 });
      assert.equal(shown, n, 'the wizard asks for the exact phrase');
      assert.ok(await page.waitFor(`!document.getElementById('panel-record').classList.contains('active')`, { timeout: 8000 }), 'the wizard closed');
      await sim.control('/api/_sim/fault', { clear: true });
      await page.eval(tab('custom'));
    }
  });

  await t.test('Delete removes THAT template, and a crafted name never runs code', async () => {
    await page.eval(tab('custom'));
    await page.eval(`window.confirm = () => true; true`);
    for (const n of NAMES) {
      if (!await page.eval(`!!${customRow(n)}`)) continue;
      await page.eval(`${customRow(n)}.querySelector('.act.del').click(), true`);
      await page.waitFor(`!${customRow(n)}`, { timeout: 5000 });
    }
    await sleep(300);
    assert.equal(await page.eval(`window.__xss`), undefined, 'a crafted command name ran code');
    const left = [];
    for (const n of NAMES) if (await exists(sim, `/system/voice/${n}.tpl`)) left.push(n);
    assert.deepEqual(left, [], 'every template could be deleted');
    const thrown = page.since(m).filter((e) => e.kind === 'exception').map((e) => String(e.text).split('\n')[0]);
    assert.deepEqual(thrown, [], 'no handler failed to compile or threw');
  });
});

test('voice-manager: training a custom phrase runs the device round trip and lands in the list', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`document.getElementById('ws-dot').classList.contains('on')`, { timeout: 15000 }), 'connected to the device bus');
  await page.eval(tab('custom'));
  await page.eval(`(() => { const i = document.getElementById('trigger-word'); i.value = '  Accendi Luce  '; i.dispatchEvent(new Event('input', { bubbles: true })); return true; })()`);
  assert.equal(await page.eval(`document.getElementById('btn-start-custom').disabled`), false, 'a typed phrase enables the button');
  await page.eval(`document.getElementById('btn-start-custom').click(), true`);
  assert.equal(await page.eval(`document.getElementById('display-word').textContent`), 'accendi luce', 'the phrase is trimmed and lower-cased');
  // The simulator answers like the device: voice/state listening, then voice/learned ok after ~1 s.
  const ok = await page.waitFor(`document.getElementById('ok-step').style.display === 'block' && document.getElementById('ok-msg').textContent`, { timeout: 15000 });
  assert.equal(ok, EN.ok_msg.replace('{word}', 'accendi luce'));
  assert.ok(await exists(sim, '/system/voice/accendi luce.tpl'), 'the device stored the template');
  await page.eval(`document.getElementById('ok-next').click(), true`);
  assert.ok(await page.waitFor(`${CUSTOM}.includes('accendi luce')`, { timeout: 10000 }), 'the new command is listed');
  assert.equal(await page.eval(`document.getElementById('prog-label').textContent`), EN.prog_label.one);
});

test('voice-manager: an unreadable folder or a busy engine never deletes anything and never throws', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/system/voice/apri musica.tpl': TPL } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await sim.control('/api/_sim/fault', { route: '/api/fs/list', status: 503 });
  const page = await open(browser, sim);
  const m = page.mark();
  await page.eval(tab('commands'));
  await page.eval(`document.querySelector('#panel-commands button.act').click(), true`);   // "Refresh" while unreadable
  await sleep(800);
  await sim.control('/api/_sim/fault', { clear: true });
  assert.equal((await requests(sim))['/api/fs/delete'], undefined, 'a failed read must not lead to a delete');
  assert.ok(await exists(sim, '/system/voice/apri musica.tpl'));

  // The engine cannot be armed (device busy): the user is told, and the wizard closes.
  await sim.control('/api/_sim/fault', { route: '/api/voice/learn', status: 503 });
  await page.eval(`[...document.querySelectorAll('#curriculum-list .trigger-item')].find((r) => r.querySelector('.trigger-name').textContent === 'apri radio').querySelector('.act').click(), true`);
  assert.ok(await page.waitFor(`!document.getElementById('panel-record').classList.contains('active')`, { timeout: 8000 }), 'the wizard closed');
  await sim.control('/api/_sim/fault', { clear: true });
  const dialogs = page.since(m).filter((e) => e.kind === 'dialog').map((e) => e.text);
  assert.deepEqual(dialogs, [EN.err_arm], 'the user is told why');
  // Refresh now works and shows the real state.
  await page.eval(tab('commands'));
  await page.eval(`document.querySelector('#panel-commands button.act').click(), true`);
  assert.ok(await page.waitFor(`document.getElementById('prog-label').textContent === ${JSON.stringify(EN.prog_label.one)}`, { timeout: 10000 }), 'the list recovers once the SD answers');
  assert.deepEqual(page.since(m).filter((e) => e.kind === 'exception').map((e) => String(e.text).split('\n')[0]), []);
});

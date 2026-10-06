// Browser E2E — the Recycle Bin must never lose the index of trashed files, and a per-item permanent
// delete must ask first (like "Empty bin" always did). A failed read of trash.json used to come back as
// an empty bin, and the next purge/restore saved that — wiping the record of every other trashed file.
import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = 45000;
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const it = { ...rd('../../web/shell/i18n/core.it.json'), ...rd('../../apps/recycle-bin/www/i18n.it.json') };

const ITEMS = [
  { orig: '/data/one.txt', trash: '/system/.trash/1_1_one.txt', name: 'one.txt', ts: 1 },
  { orig: '/data/two.txt', trash: '/system/.trash/2_2_two.txt', name: 'two.txt', ts: 2 },
];
const SEED = { '/system/.trash/1_1_one.txt': 'one', '/system/.trash/2_2_two.txt': 'two', '/system/config/trash.json': { items: ITEMS } };

async function openBin(browser, sim) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'it', wait: false });
  await page.goto(`${sim.origin}/apps/recycle-bin/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  assert.ok(await page.waitFor(`document.querySelectorAll('#list .row').length === 2`, { timeout: T }), 'both trashed files are listed');
  return page;
}
// Click the per-item "Delete" button of the row for `name`, with window.confirm answering `answer`.
const purge = (page, name, answer) => page.eval(`(() => {
  window.__asked = [];
  window.confirm = (msg) => { window.__asked.push(msg); return ${answer}; };
  const row = [...document.querySelectorAll('#list .row')].find((r) => r.querySelector('.name').textContent === ${JSON.stringify(name)});
  row.querySelector('button.danger').click();
  return true;
})()`);
const toastText = (page) => page.eval(`document.getElementById('toast').textContent`);

test('recycle-bin: per-item delete asks first; a failed index read never wipes the index', { skip }, async (t) => {
  const sim = await startSim({ seed: SEED });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const onSd = (p) => existsSync(join(sim.sd, p.replace(/^\//, '')));
  // Every case starts from the same two trashed files (a case that fails must not cascade into the next).
  const reset = () => { for (const [p, b] of Object.entries(SEED)) writeFileSync(join(sim.sd, p.replace(/^\//, '')), typeof b === 'string' ? b : JSON.stringify(b)); };
  t.beforeEach(reset);
  const index = async () => JSON.parse(await sim.readSd('/system/config/trash.json')).items.map((i) => i.name);

  await t.test('declining the confirmation deletes nothing', async () => {
    const page = await openBin(browser, sim);
    await purge(page, 'one.txt', false);
    await page.networkIdle({ quiet: 600, timeout: T }); await sleep(300);
    assert.ok(onSd(ITEMS[0].trash), 'one.txt was permanently deleted without asking');
    assert.deepEqual(await index(), ['one.txt', 'two.txt']);
    const asked = await page.eval(`window.__asked`);
    assert.deepEqual(asked, [it.confirm_purge.replace('{name}', 'one.txt')], 'the confirmation names the file, in Italian');
  });

  await t.test('a failed read of the index blocks the purge and keeps every entry', async () => {
    const page = await openBin(browser, sim);
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
    try {
      await purge(page, 'one.txt', true);
      // Settle on the outcome, not on network idle (under a 503 the page keeps retrying its reads).
      await page.waitFor(`document.getElementById('toast').textContent === ${JSON.stringify(it.err_index_unreadable)}`, { timeout: T });
      await sleep(800);
    } finally { await sim.control('/api/_sim/fault', { clear: true }); }
    assert.deepEqual(await index(), ['one.txt', 'two.txt'], 'the index of the other trashed files was wiped');
    assert.ok(onSd(ITEMS[0].trash) && onSd(ITEMS[1].trash), 'no trashed file was deleted while the index was unreadable');
    assert.equal(await toastText(page), it.err_index_unreadable);
  });

  await t.test('a failed read of the index says so instead of showing an empty bin', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
    try {
      const page = await browser.newPage();
      await bootShell(page, sim, { lang: 'it', wait: false });
      await page.goto(`${sim.origin}/apps/recycle-bin/`);
      await page.networkIdle({ quiet: 500, timeout: 20000 });
      const shown = await page.waitFor(`document.getElementById('list').textContent.trim()`, { timeout: T });
      assert.ok(shown.includes(it.load_failed), 'an unreadable index was shown as an empty bin: ' + shown);
      assert.ok(await page.eval(`document.getElementById('empty').disabled && document.getElementById('restore-all').disabled`), 'bulk actions stay off');
    } finally { await sim.control('/api/_sim/fault', { clear: true }); }
  });

  await t.test('confirming deletes that one item and keeps the other', async () => {
    const page = await openBin(browser, sim);
    await purge(page, 'one.txt', true);
    assert.ok(await page.waitFor(`document.querySelectorAll('#list .row').length === 1`, { timeout: T }));
    for (let end = Date.now() + T; Date.now() < end && (await index()).length !== 1;) await sleep(100);
    assert.ok(!onSd(ITEMS[0].trash), 'one.txt is gone for good');
    assert.deepEqual(await index(), ['two.txt']);
  });
});

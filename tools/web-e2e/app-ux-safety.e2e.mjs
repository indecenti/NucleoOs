// Browser E2E — destructive actions must ask, files the shell opens must actually open, and names that
// come from the SD are text, never markup. Covers: Mail (delete account), Archive Manager (open via
// ?path=, overwrite prompt, nested folders, live re-translation), DJ (library names), Calendar (event ids).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const cat = (app, lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/${app}/www/i18n.${lang}.json`) });
const today = () => { const d = new Date(); return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`; };

async function openApp(browser, sim, id, lang = 'en', query = '') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/${id}/${query}`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  return page;
}
// Replace window.confirm with a recorder that answers `answer` (the CDP harness would auto-accept).
const stubConfirm = (answer) => `(() => { window.__asked = []; window.confirm = (m) => { window.__asked.push(String(m)); return ${answer}; }; return true; })()`;

test('mail: deleting a stored SMTP account asks first (localized)', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const de = cat('mail', 'de');
  const page = await openApp(browser, sim, 'mail', 'de');
  await page.eval(`fetch('/api/mail/accounts', { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: 'Work', host: 'smtp.example.org', port: 465, user: 'me@example.org', pass: 'x' }) }).then((r) => r.status)`);
  await page.goto(`${sim.origin}/apps/mail/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  await page.eval(`document.getElementById('tabA').click(), true`);
  assert.ok(await page.waitFor(`!!document.querySelector('[data-del]')`, { timeout: 10000 }), 'the account is listed');
  const count = `(async () => (await (await fetch('/api/mail/accounts', { cache: 'no-store' })).json()).accounts.length)()`;

  await t.test('cancel keeps the account', async () => {
    await page.eval(stubConfirm(false));
    await page.eval(`document.querySelector('[data-del]').click(), true`);
    await sleep(800);
    assert.equal(await page.eval(count), 1, 'the account was deleted without asking');
    const asked = await page.eval(`window.__asked`);
    assert.equal(asked.length, 1, 'one confirmation');
    assert.equal(asked[0], de.confirm_delete_account.replace('{name}', 'Work'), 'the question is in German and names the account');
  });

  await t.test('confirm deletes it', async () => {
    await page.eval(stubConfirm(true));
    await page.eval(`document.querySelector('[data-del]').click(), true`);
    assert.ok(await page.waitFor(`${count}.then((n) => n === 0)`, { timeout: 10000 }), 'deleted after a yes');
  });
});

test('archive-manager: opens the file the shell passes, asks before overwriting, creates nested folders', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/arc/a.txt': 'OLD A' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const it = cat('archive-manager', 'it');

  // Build the .zip in the page with the app's own JSZip: no folder entries, so the extractor itself has
  // to create sub/deep/ (the firmware's /api/fs/write never creates parent folders).
  const maker = await openApp(browser, sim, 'archive-manager', 'en');
  const st = await maker.eval(`(async () => {
    const z = new JSZip();
    z.file('a.txt', 'NEW A', { createFolders: false });
    z.file('sub/deep/b.txt', 'B', { createFolders: false });
    const blob = await z.generateAsync({ type: 'blob' });
    return (await fetch('/api/fs/write?path=' + encodeURIComponent('/data/arc/test.zip'), { method: 'POST', body: blob })).status;
  })()`);
  assert.equal(st, 200, 'test archive written');

  const page = await openApp(browser, sim, 'archive-manager', 'it', '?path=' + encodeURIComponent('/data/arc/test.zip'));

  await t.test('?path= from the shell loads the archive (no "No archive loaded" forever)', async () => {
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li').length === 2`, { timeout: 15000 }), 'entries listed: ' + await page.eval(`document.getElementById('status').textContent`));
    assert.equal(await page.eval(`document.getElementById('btn-action').disabled`), false, 'Extract is enabled');
  });

  await t.test('an existing file is not overwritten without a yes', async () => {
    await page.eval(stubConfirm(false));
    await page.eval(`document.getElementById('btn-action').click(), true`);
    await sleep(1500);
    assert.equal(await sim.readSd('/data/arc/a.txt'), 'OLD A', 'a.txt was overwritten without asking');
    const asked = await page.eval(`window.__asked`);
    assert.equal(asked.length, 1, 'asked once');
    assert.ok(asked[0].includes('a.txt') && !asked[0].includes('b.txt'), 'names exactly the clashing file: ' + asked[0]);
    const want = it.confirm_overwrite.one.replace('{count}', '1').split('{')[0];   // plural form for 1 clash
    assert.ok(asked[0].startsWith(want), 'in Italian: ' + asked[0]);
  });

  await t.test('after a yes: overwritten, and the nested entry lands in a folder it created', async () => {
    await page.eval(stubConfirm(true));
    await page.eval(`document.getElementById('btn-action').click(), true`);
    assert.ok(await page.waitFor(`document.getElementById('prog-title').textContent === ${JSON.stringify(it.extract_complete)}`, { timeout: 15000 }),
      'extraction finished: ' + await page.eval(`document.getElementById('prog-text').textContent`));
    assert.equal(await sim.readSd('/data/arc/a.txt'), 'NEW A');
    assert.equal(await sim.readSd('/data/arc/sub/deep/b.txt'), 'B');
  });

  await t.test('a failed read reports in the user\'s language, not hard-coded English', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
    const p2 = await openApp(browser, sim, 'archive-manager', 'it', '?path=' + encodeURIComponent('/data/arc/test.zip'));
    await sim.control('/api/_sim/fault', { clear: true });
    const txt = await p2.waitFor(`(() => { const s = document.getElementById('list').textContent; return s.includes(${JSON.stringify(it.open_failed)}) && s; })()`, { timeout: 15000 });
    assert.ok(txt, 'the open failure is shown');
    assert.ok(!/Failed to read file/.test(txt), 'hard-coded English: ' + txt);
    assert.ok(txt.includes(it.err_read_file.replace('{status}', '503')), 'the localized reason is shown: ' + txt);
  });

  await t.test('compress mode is re-translated once the language engine is up', async () => {
    const p3 = await openApp(browser, sim, 'archive-manager', 'it', '?compress=' + encodeURIComponent(JSON.stringify(['/data/arc/a.txt'])));
    const want = it.create_in.replace('{dir}', '/data/arc');
    assert.ok(await p3.waitFor(`document.getElementById('status').textContent.includes(${JSON.stringify(want)})`, { timeout: 15000 }),
      'still English: ' + await p3.eval(`document.getElementById('status').textContent`));
  });
});

test('dj: a library file name is text, never markup', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/Music/b&amp;w.mp3': 'x' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openApp(browser, sim, 'dj', 'en');
  const name = await page.waitFor(`(() => { const n = document.querySelector('#list .name'); return n && n.textContent; })()`, { timeout: 15000 });
  assert.equal(name, 'b&amp;w.mp3', 'the name was parsed as HTML');
});

test('calendar: an event id is escaped in its attribute, so it can still be deleted', { skip }, async (t) => {
  const key = today();
  const cal = { schema: 1, events: { [key]: [{ id: 'x" data-pwn="1', time: '09:00', text: 'Quoted id' }, { id: 'u1', time: '10:00', text: 'Plain' }] } };
  const sim = await startSim({ seed: { '/system/config/calendar.json': cal } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openApp(browser, sim, 'calendar', 'en');
  assert.ok(await page.waitFor(`document.querySelectorAll('#evlist .evrow').length === 2`, { timeout: 10000 }));
  assert.equal(await page.eval(`document.querySelectorAll('#evlist [data-pwn]').length`), 0, 'the id broke out of its attribute');
  await page.eval(`[...document.querySelectorAll('#evlist .evrow')].find((r) => r.textContent.includes('Quoted id')).querySelector('.del').click(), true`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#evlist .evrow').length === 1`, { timeout: 10000 }), 'the event was not deleted');
  const saved = JSON.parse(await sim.readSd('/system/config/calendar.json'));
  assert.deepEqual(saved.events[key].map((e) => e.text), ['Plain']);
});

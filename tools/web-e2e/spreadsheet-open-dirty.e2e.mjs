// Browser E2E — opening another file must not silently throw away unsaved edits. Found by review:
// New and Exit asked first, but Open (Ctrl+O / the file dialog) and an 'open-file' message from the
// shell (double-click in File Commander) replaced the sheet without a word.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const en = JSON.parse(readFileSync(new URL('../../apps/spreadsheet/www/i18n.en.json', import.meta.url), 'utf8'));

// The sheet's state lives in the classic script's global scope (cellData, filePath, dirty, dlgId…).
const A1 = `(cellData.get('0,0') || {}).raw`;
const edit = `setCellRaw(0, 0, 'unsaved edit'); markDirty(); renderVirtual();`;
const answer = (yes) => `window.__confirms = []; window.confirm = (m) => { window.__confirms.push(m); return ${yes}; }`;

test('spreadsheet: opening another file asks before discarding unsaved edits', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/a.csv': 'from a\r\n', '/data/b.csv': 'from b\r\n' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });

  // A dirty sheet arms beforeunload; disarm it so the next navigation is not held by a dialog.
  const disarm = () => page.eval(`if (typeof markDirty === 'function') dirty = false`);
  async function freshDirtySheet() {
    await disarm();
    await page.goto(`${sim.origin}/apps/spreadsheet/?path=${encodeURIComponent('/data/a.csv')}`);
    assert.ok(await page.waitFor(`typeof cellData !== 'undefined' && ${A1} === 'from a'`, { timeout: 20000 }), 'a.csv loaded');
    await page.eval(edit);
    assert.equal(await page.eval(`dirty`), true);
  }
  const viaShellMessage = `window.postMessage({ type: 'open-file', path: '/data/b.csv' }, '*')`;
  const viaOpenDialog = `(async () => {
    openFile();
    await new Promise((r) => setTimeout(r, 50));
    window.postMessage({ type: 'os-file-dialog-result', id: dlgId, action: 'open', path: '/data/b.csv' }, '*');
  })()`;

  for (const [how, trigger] of [['an open-file message from the shell', viaShellMessage], ['Open (file dialog)', viaOpenDialog]]) {
    await t.test(`${how}: declining keeps the edits and the file binding`, async () => {
      await freshDirtySheet();
      await page.eval(answer(false));
      await page.eval(trigger);
      await sleep(1000);
      assert.equal(await page.eval(A1), 'unsaved edit', 'unsaved edits were replaced by the other file');
      assert.equal(await page.eval(`filePath`), '/data/a.csv');
      assert.deepEqual(await page.eval(`window.__confirms`), [en.confirm_open || '(missing key confirm_open)'], 'the user is asked, in their language');
    });

    await t.test(`${how}: accepting opens the other file`, async () => {
      await freshDirtySheet();
      await page.eval(answer(true));
      await page.eval(trigger);
      assert.ok(await page.waitFor(`${A1} === 'from b'`, { timeout: 15000 }), 'b.csv opened');
      assert.equal(await page.eval(`filePath`), '/data/b.csv');
      assert.equal(await page.eval(`window.__confirms.length`), 1);
    });
  }

  await t.test('a clean sheet opens another file without asking', async () => {
    await disarm();
    await page.goto(`${sim.origin}/apps/spreadsheet/?path=${encodeURIComponent('/data/a.csv')}`);
    assert.ok(await page.waitFor(`typeof cellData !== 'undefined' && ${A1} === 'from a'`, { timeout: 20000 }));
    await page.eval(answer(false));
    await page.eval(viaShellMessage);
    assert.ok(await page.waitFor(`${A1} === 'from b'`, { timeout: 15000 }));
    assert.equal(await page.eval(`window.__confirms.length`), 0);
  });
});

// Browser E2E — Notepad must never overwrite a file it could not read. Found by review: opening
// ?path=<file> while the device answered 503 (or was unreachable) left the editor EMPTY but still bound
// to that path, so the next Ctrl+S wrote the empty buffer over the real file. Only a definite 404 (a
// new file at a chosen path) may keep the binding.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const en = JSON.parse(readFileSync(new URL('../../apps/notepad/www/i18n.en.json', import.meta.url), 'utf8'));

const KEEP = '/data/Documents/keep.txt';
const ctrlS = `document.dispatchEvent(new KeyboardEvent('keydown', { key: 's', ctrlKey: true, bubbles: true }))`;
// Standalone (no shell parent) Save As falls back to prompt(): record it and cancel, so nothing is written.
const stubPrompt = `window.__prompts = []; window.prompt = (m, d) => { window.__prompts.push(d); return null; }`;

test('notepad: a file that could not be read is never overwritten by Save', { skip }, async (t) => {
  const sim = await startSim({ seed: { [KEEP]: 'precious content' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });

  async function openNotepad() {
    const page = await browser.newPage();
    await bootShell(page, sim, { lang: 'en', wait: false });
    return page;
  }
  const unbound = (en.open_failed_unbound || '(missing key open_failed_unbound)').replace('{name}', 'keep.txt');

  await t.test('503 on open: Save does not write the empty editor over the file', async () => {
    const page = await openNotepad();
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
    await page.goto(`${sim.origin}/apps/notepad/?path=${encodeURIComponent(KEEP)}`);
    await page.networkIdle({ quiet: 800, timeout: 15000 });
    await sim.control('/api/_sim/fault', { clear: true });
    const status = await page.eval(`document.getElementById('status').textContent`);
    await page.eval(stubPrompt);
    await page.eval(ctrlS);
    await sleep(1200);
    assert.equal(await sim.readSd(KEEP), 'precious content', 'Ctrl+S overwrote a file that was never read');
    assert.equal(await page.eval(`window.__prompts.length`), 1, 'Save asks where to save instead');
    assert.equal(status, unbound, 'the user is told why Save will ask for a location');
  });

  await t.test('device unreachable on open: same — the file survives a Save', async () => {
    const page = await openNotepad();
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', drop: true });
    await page.goto(`${sim.origin}/apps/notepad/?path=${encodeURIComponent(KEEP)}`);
    await page.networkIdle({ quiet: 800, timeout: 15000 });
    await sim.control('/api/_sim/fault', { clear: true });
    const status = await page.eval(`document.getElementById('status').textContent`);
    await page.eval(stubPrompt);
    await page.eval(`document.getElementById('ed').value = 'typed after a failed open'; document.getElementById('ed').dispatchEvent(new Event('input'))`);
    await page.eval(ctrlS);
    await sleep(1200);
    assert.equal(await sim.readSd(KEEP), 'precious content', 'Ctrl+S overwrote a file that was never read');
    assert.equal(status, unbound, 'the failure is explained');
  });

  await t.test('404 on open (a new file at a chosen path): Save creates it there', async () => {
    const page = await openNotepad();
    const NEW = '/data/Documents/brand-new.txt';
    await page.goto(`${sim.origin}/apps/notepad/?path=${encodeURIComponent(NEW)}`);
    await page.networkIdle({ quiet: 500, timeout: 15000 });
    await page.eval(stubPrompt);
    await page.eval(`document.getElementById('ed').value = 'hello'; document.getElementById('ed').dispatchEvent(new Event('input'))`);
    await page.eval(ctrlS);
    assert.ok(await page.waitFor(`document.getElementById('status').textContent === ${JSON.stringify(en.saved_file.replace('{name}', 'brand-new.txt'))}`, { timeout: 15000 }), 'saved');
    assert.equal(await sim.readSd(NEW), 'hello');
    assert.equal(await page.eval(`window.__prompts.length`), 0, 'no location prompt for a path that simply did not exist yet');
  });

  await t.test('a normal open still saves in place', async () => {
    const page = await openNotepad();
    await page.goto(`${sim.origin}/apps/notepad/?path=${encodeURIComponent(KEEP)}`);
    assert.ok(await page.waitFor(`document.getElementById('ed').value === 'precious content'`, { timeout: 15000 }));
    await page.eval(`document.getElementById('ed').value += ' + edit'; document.getElementById('ed').dispatchEvent(new Event('input'))`);
    await page.eval(ctrlS);
    assert.ok(await page.waitFor(`document.getElementById('status').textContent === ${JSON.stringify(en.saved_file.replace('{name}', 'keep.txt'))}`, { timeout: 15000 }));
    assert.equal(await sim.readSd(KEEP), 'precious content + edit');
  });
});

// Browser E2E — a new task always sorts after the older ones of the same priority. Found by review:
// add() stamped `ts: tasks.length`, so once tasks had been deleted a new task got a SMALLER (or
// duplicate) timestamp than surviving older ones and jumped above them. Lists saved with the old
// small-integer stamps must keep loading and ordering as before.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';

const titles = `[...document.querySelectorAll('#list li .txt')].map((x) => x.textContent)`;
const addTask = (text) => `document.getElementById('text').value = ${JSON.stringify(text)};
  document.getElementById('text').dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))`;

test('tasks: a task added after deletions sorts below the older ones', { skip }, async (t) => {
  // What a list looks like after "A, B, C, D" were added (ts 0..3) and A and B were deleted.
  const list = [
    { id: 't3', text: 'Older C', done: false, pri: 0, ts: 2 },
    { id: 't4', text: 'Older D', done: false, pri: 0, ts: 3 },
  ];
  const sim = await startSim({ seed: { '/data/tasks.todo': list } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/tasks/`);
  await page.networkIdle({ quiet: 500, timeout: 15000 });

  await t.test('legacy small-integer stamps still load in their order', async () => {
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li .txt').length === 2`, { timeout: 15000 }));
    assert.deepEqual(await page.eval(titles), ['Older C', 'Older D']);
  });

  await t.test('new tasks go to the bottom, in the order they were added, and stay there after a reload', async () => {
    await page.eval(addTask('Newer E'));
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li .txt').length === 3`, { timeout: 15000 }));
    await page.eval(addTask('Newer F'));
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li .txt').length === 4`, { timeout: 15000 }));
    assert.deepEqual(await page.eval(titles), ['Older C', 'Older D', 'Newer E', 'Newer F']);
    assert.ok(await page.waitFor(`document.getElementById('save').textContent === ''`, { timeout: 15000 }), 'autosave settled');
    const saved = JSON.parse(await sim.readSd('/data/tasks.todo'));
    const ts = Object.fromEntries(saved.map((x) => [x.text, x.ts]));
    assert.ok(ts['Newer E'] > ts['Older D'] && ts['Newer F'] > ts['Newer E'], 'stamps are strictly increasing: ' + JSON.stringify(ts));
    await page.goto(`${sim.origin}/apps/tasks/`);       // reload: the order comes from the saved stamps
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li .txt').length === 4`, { timeout: 15000 }));
    assert.deepEqual(await page.eval(titles), ['Older C', 'Older D', 'Newer E', 'Newer F']);
  });
});

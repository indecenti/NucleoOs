// Browser E2E — Voice Recorder against the simulator (the sim implements the firmware's /api/rec/*):
// the library lists only takes, names as TEXT, and each player points at its own file; delete asks first,
// takes the whole sidecar set with it, and a FAILED delete keeps the transcript and says so; a failed
// listing is "unreachable", not "no recordings"; a busy Cardputer mic refuses the start without blaming the
// recorder itself; a free mic records, stops and leaves an MP3 (the device WAV removed).
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync, statSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const EN = { ...rd('../../web/shell/i18n/core.en.json'), ...rd('../../apps/recorder/www/i18n.en.json') };
const DIR = '/data/Recordings';
const MP3 = 'ID3\u0003\u0000\u0000\u0000\u0000\u0000\u0000fake-mp3-bytes';

async function open(browser, sim) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/recorder/`);
  return page;
}
const onSd = (sim, p) => existsSync(join(sim.sd, p.replace(/^\//, '')));
// Library rows as { name (the real file name, from the title), shown (the visible label), path the player reads }.
const ROWS = `[...document.querySelectorAll('#list li.rec')].map((li) => ({
  name: li.querySelector('.nm').getAttribute('title'), shown: li.querySelector('.nm').textContent,
  play: new URL(li.querySelector('audio').getAttribute('src'), location.href).searchParams.get('path') }))`;
const rowBtn = (name, sel) => `(() => { const li = [...document.querySelectorAll('#list li.rec')].find((l) => l.querySelector('.nm').getAttribute('title') === ${JSON.stringify(name)});
  li.querySelector(${JSON.stringify(sel)}).click(); return true; })()`;
const STATUS = `document.getElementById('status').textContent`;

test('recorder: the library lists only takes, names as text, and each player reads its own file', { skip }, async (t) => {
  const sim = await startSim({ seed: {
    [`${DIR}/rec-20261001-101500.mp3`]: MP3, [`${DIR}/rec-20261001-101500.txt`]: 'hello transcript',
    [`${DIR}/rec-20261001-101500.sum.txt`]: 'a summary', [`${DIR}/Tom &amp; Jerry.mp3`]: MP3,
    [`${DIR}/rec-20261002-090000.wav`]: 'RIFF....WAVE', [`${DIR}/notes.txt`]: 'not a take',
  } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list li.rec').length === 3`, { timeout: 45000 }), 'three takes listed');
  const rows = await page.eval(ROWS);
  const names = rows.map((r) => r.name);
  assert.deepEqual([...names].sort(), ['Tom &amp; Jerry.mp3', 'rec-20261001-101500.mp3', 'rec-20261002-090000.wav'], 'MP3 and WAV takes only: transcripts, summaries and other files are not takes');
  assert.ok(names.indexOf('rec-20261002-090000.wav') < names.indexOf('rec-20261001-101500.mp3'), 'newest first');
  for (const r of rows) assert.equal(r.play, `${DIR}/${r.name}`, 'the player of "' + r.name + '" reads that file');
  const tom = rows.find((r) => r.name === 'Tom &amp; Jerry.mp3');
  assert.equal(tom.shown, 'Tom &amp; Jerry', 'a file name is shown literally, never decoded as HTML');
  assert.equal(await page.eval(`document.querySelectorAll('#list .nm *').length`), 0, 'no markup inside a name');
  assert.equal(rows.find((r) => r.name.startsWith('rec-20261001')).shown, '01/10 10:15', 'timestamped takes get a readable date');

  await page.eval(`(() => { const s = document.getElementById('search'); s.value = 'jerry'; s.dispatchEvent(new Event('input')); return true; })()`);
  assert.deepEqual((await page.eval(ROWS)).map((r) => r.name), ['Tom &amp; Jerry.mp3'], 'search filters by name');
  await page.eval(`(() => { const s = document.getElementById('search'); s.value = 'zzz'; s.dispatchEvent(new Event('input')); return true; })()`);
  assert.equal(await page.eval(`document.querySelector('#list .empty').textContent`), EN.noResults);
});

test('recorder: delete asks first; confirm removes the take with its sidecars; a failed delete keeps the transcript', { skip }, async (t) => {
  const A = `${DIR}/rec-20261001-101500`, B = `${DIR}/Tom &amp; Jerry`, C = `${DIR}/rec-20261003-080000`;
  const sim = await startSim({ seed: {
    [A + '.mp3']: MP3, [A + '.txt']: 'transcript A', [A + '.sum.txt']: 'summary A',
    [B + '.mp3']: MP3, [B + '.txt']: 'transcript B', [C + '.mp3']: MP3, [C + '.txt']: 'transcript C',
  } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list li.rec').length === 3`, { timeout: 45000 }));
  await page.networkIdle({ quiet: 400, timeout: 10000 });

  await t.test('cancel: the question is asked and nothing is touched', async () => {
    await page.eval(`window.__asked = []; window.confirm = (m) => { window.__asked.push(m); return false; }; true`);
    await page.eval(rowBtn('rec-20261001-101500.mp3', 'button.del'));
    await sleep(600);
    assert.deepEqual(await page.eval(`window.__asked`), [EN.confirmDelete], 'delete asks first');
    for (const x of ['.mp3', '.txt', '.sum.txt']) assert.ok(onSd(sim, A + x), A + x + ' kept');
  });

  await t.test('confirm: the take AND its transcript/summary go, nothing else', async () => {
    await page.eval(`window.confirm = (m) => { window.__asked.push(m); return true; }; true`);
    await page.eval(rowBtn('rec-20261001-101500.mp3', 'button.del'));
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li.rec').length === 2`, { timeout: 10000 }), 'the row is gone');
    await page.networkIdle({ quiet: 400, timeout: 10000 });
    for (const x of ['.mp3', '.txt', '.sum.txt']) assert.ok(!onSd(sim, A + x), A + x + ' deleted');
    for (const p of [B + '.mp3', B + '.txt', C + '.mp3', C + '.txt']) assert.ok(onSd(sim, p), p + ' untouched');
  });

  await t.test('the device refuses the delete: the take AND its transcript stay, and the user is told', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fs/delete', status: 503, times: 1 });   // only the audio delete fails
    await page.eval(rowBtn('Tom &amp; Jerry.mp3', 'button.del'));
    assert.ok(await page.waitFor(`document.getElementById('toast').textContent === ${JSON.stringify(EN.deleteFailed)}`, { timeout: 10000 }), 'the failure is reported, not "Deleted"');
    await page.networkIdle({ quiet: 400, timeout: 10000 });
    await sim.control('/api/_sim/fault', { clear: true });
    assert.ok(onSd(sim, B + '.mp3'), 'the audio is still there');
    assert.ok(onSd(sim, B + '.txt'), 'so its transcript must be too (it was deleted while the take stayed)');
    assert.equal(await page.eval(`document.querySelectorAll('#list li.rec').length`), 2, 'the take is still listed');
  });
});

test('recorder: a failed listing says the device is unreachable, an absent folder says "no recordings"', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const EMPTY = `(() => { const e = document.querySelector('#list .empty'); return e && e.textContent; })()`;
  const page = await open(browser, sim);
  assert.equal(await page.waitFor(EMPTY, { timeout: 45000 }), EN.noRecordings, 'no folder yet = empty library');

  await sim.control('/api/_sim/fault', { route: '/api/fs/list', status: 503 });
  const p2 = await open(browser, sim);
  const shown = await p2.waitFor(EMPTY, { timeout: 45000 });
  await sim.control('/api/_sim/fault', { clear: true });
  assert.equal(shown, EN.listFailed, 'a failed listing must not claim the recordings are gone');
});

test('recorder: a busy mic refuses the start (without blaming itself); a free mic records and saves an MP3', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`document.querySelector('#list .empty')`, { timeout: 45000 }));
  await page.networkIdle({ quiet: 400, timeout: 10000 });

  await t.test('another app holds the mic: REC is dimmed and the holder is named', async () => {
    await page.eval(`(window.__bc = new BroadcastChannel('nucleo-mic')).postMessage({ active: 'Dettatura' }), true`);
    assert.ok(await page.waitFor(`document.getElementById('recBtn').classList.contains('held')`, { timeout: 5000 }), 'REC dimmed');
    assert.equal(await page.eval(STATUS), EN.micInUseByIcon + 'Dettatura' + EN.freeToRecord);
    await page.eval(`window.__bc.postMessage({ active: null }), true`);
    assert.ok(await page.waitFor(`!document.getElementById('recBtn').classList.contains('held')`, { timeout: 5000 }), 'released');
  });

  await t.test('the firmware answers 409 (the on-device recorder holds the mic): the start is refused', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/rec/start', status: 409, times: 1 });
    await page.eval(`document.getElementById('recBtn').click(), true`);
    const msg = await page.waitFor(`(() => { const s = document.getElementById('status'); return s.classList.contains('err') && s.textContent; })()`, { timeout: 10000 });
    assert.equal(msg, EN.startFailed + EN.micBusy, 'says the mic is busy — not "busy · Recording", which names this very app');
    assert.equal(await page.eval(`document.getElementById('recWrap').classList.contains('on')`), false, 'not left in the recording state');
  });

  await t.test('free mic: record, stop, and the take is saved as MP3 with the WAV removed', async () => {
    await page.eval(`document.getElementById('recBtn').click(), true`);
    assert.ok(await page.waitFor(`document.getElementById('recWrap').classList.contains('on') && ${STATUS}.startsWith(${JSON.stringify(EN.recordingPrefix)})`, { timeout: 10000 }), 'recording');
    await sleep(1800);
    await page.eval(`document.getElementById('recBtn').click(), true`);
    assert.ok(await page.waitFor(`${STATUS} === ${JSON.stringify(EN.savedAsMp3)}`, { timeout: 30000 }), 'saved as MP3');
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li.rec').length === 1`, { timeout: 10000 }));
    const [row] = await page.eval(ROWS);
    assert.match(row.name, /^rec-\d+\.mp3$/);
    assert.ok(onSd(sim, `${DIR}/${row.name}`) && statSync(join(sim.sd, 'data', 'Recordings', row.name)).size > 200, 'a real MP3 on the SD');
    assert.ok(!onSd(sim, `${DIR}/${row.name.replace(/\.mp3$/, '.wav')}`), 'the device WAV was removed');
  });
});

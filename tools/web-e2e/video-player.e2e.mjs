// Browser E2E — Video Player against the simulator: a file opened from Files (?path=) loads its folder as
// the playlist (videos only, names as text), a finished video advances, loop wraps, a missing /data/Videos
// is an EMPTY playlist (not "SD not mounted"), and subtitle names are text while SRT timing survives.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const EN = JSON.parse((await import('node:fs')).readFileSync(new URL('../../apps/video-player/www/i18n.en.json', import.meta.url), 'utf8'));
const VID = '\u0000\u0000\u0000\u0018ftypmp42';                  // the bytes do not matter: playback is not what is tested

async function open(browser, sim, query = '') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/video-player/${query}`);
  return page;
}
const PLAYING = `(() => { const s = document.getElementById('v').getAttribute('src'); return s ? new URL(s, location.href).searchParams.get('path') : ''; })()`;
const CHIPS = `[...document.querySelectorAll('#playlist-items .playlist-chip')].map((c) => c.textContent)`;
const ACTIVE = `[...document.querySelectorAll('#playlist-items .playlist-chip.active')].map((c) => c.textContent)`;
const ENDED = `document.getElementById('v').dispatchEvent(new Event('ended')), true`;
const key = (k, extra = {}) => `document.dispatchEvent(new KeyboardEvent('keydown', Object.assign({ key: ${JSON.stringify(k)}, bubbles: true }, ${JSON.stringify(extra)}))), true`;
const PL_NOTE = `(() => { const s = document.querySelector('#playlist-items .playlist-empty'); return s && s.textContent; })()`;

test('video-player: ?path= loads its folder as the playlist; ended → next, loop wraps, names as text', { skip }, async (t) => {
  const sim = await startSim({ seed: {
    '/data/Videos/a.mp4': VID, '/data/Videos/b&amp;c.webm': VID, '/data/Videos/C.MKV': VID,
    '/data/Videos/a.srt': '1\n00:00:01,000 --> 00:00:02,000\nhi\n', '/data/Videos/notes.txt': 'x', '/data/Videos/clips.mp4/x.mp4': VID,
  } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim, '?path=' + encodeURIComponent('/data/Videos/a.mp4'));
  assert.ok(await page.waitFor(`document.querySelectorAll('#playlist-items .playlist-chip').length === 3`, { timeout: 45000 }), 'the playlist loaded with the 3 videos');
  await page.networkIdle({ quiet: 400, timeout: 10000 });
  const order = await page.eval(CHIPS);
  assert.deepEqual([...order].sort(), ['C.MKV', 'a.mp4', 'b&amp;c.webm'], 'videos only (any case), no subtitles/text, no folder; &amp; stays literal');
  assert.equal(await page.eval(`document.querySelectorAll('#playlist-items .playlist-chip span *').length`), 0, 'a name is never parsed as markup');
  assert.equal(await page.eval(PLAYING), '/data/Videos/a.mp4', 'the opened file plays');
  assert.deepEqual(await page.eval(ACTIVE), ['a.mp4'], 'and is highlighted');
  assert.equal(await page.eval(`document.getElementById('title-sub').textContent`), '— a.mp4', 'the header names the file');

  // Walk from the first to the last entry with "ended".
  await page.eval(`[...document.querySelectorAll('#playlist-items .playlist-chip')][0].click(), true`);
  assert.equal(await page.eval(PLAYING), '/data/Videos/' + order[0], 'clicking a chip plays it');
  await page.eval(ENDED);
  assert.equal(await page.eval(PLAYING), '/data/Videos/' + order[1], 'ended → next video');
  assert.deepEqual(await page.eval(ACTIVE), [order[1]], 'the highlight follows');
  await page.eval(ENDED);
  assert.equal(await page.eval(PLAYING), '/data/Videos/' + order[2]);
  await page.eval(ENDED);
  assert.equal(await page.eval(PLAYING), '/data/Videos/' + order[2], 'the last video ending does not wrap without loop');
  await page.eval(key('l'));
  await page.eval(ENDED);
  assert.equal(await page.eval(PLAYING), '/data/Videos/' + order[0], 'with loop on, the last video ending wraps to the first');
  await page.eval(key('ArrowLeft', { ctrlKey: true }));
  assert.equal(await page.eval(PLAYING), '/data/Videos/' + order[2], 'Ctrl+← on the first video with loop → the last');
  await page.eval(key('s'));
  assert.equal(await page.eval(PLAYING), '', '"s" stops');
  assert.deepEqual(await page.eval(ACTIVE), [], 'and nothing is highlighted any more');
});

test('video-player: no /data/Videos = "SD ready" + an EMPTY playlist; a failed listing = SD error', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.equal(await page.waitFor(`(() => { const s = document.getElementById('sd-status-text').textContent; return s !== ${JSON.stringify(EN.sdDash)} && !s.includes('—') && s; })()`, { timeout: 45000 }), EN.sdReady,
    'the card answers 404 for the folder: it is reachable');
  await page.eval(key('p'));                                       // open the playlist bar → loads /data/Videos
  assert.ok(await page.waitFor(PL_NOTE, { timeout: 15000 }), 'the playlist settled');
  const empty = await page.waitFor(`${PL_NOTE} === ${JSON.stringify(EN.plEmpty)}`, { timeout: 5000 });
  assert.ok(empty, 'a missing videos folder is an empty playlist, not "' + await page.eval(PL_NOTE) + '"');

  await sim.control('/api/_sim/fault', { route: '/api/fs/list', status: 503 });
  const p2 = await open(browser, sim);
  await p2.waitFor(`document.getElementById('sd-status-text').textContent === ${JSON.stringify(EN.sdOffline)}`, { timeout: 45000 });
  await p2.eval(key('p'));
  const err = await p2.waitFor(`${PL_NOTE} === ${JSON.stringify(EN.plSdError)}`, { timeout: 15000 });
  await sim.control('/api/_sim/fault', { clear: true });
  assert.ok(err, 'a failed listing IS an SD error');
});

test('video-player: a subtitle file name is text, and SRT timing is converted exactly', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`!!document.getElementById('input-sub') && document.getElementById('sd-status-text').textContent !== ${JSON.stringify(EN.sdDash)}`, { timeout: 45000 }), 'the app started');
  const SRT = '1\r\n00:00:01,500 --> 00:00:03,000\r\nCiao <i>mondo</i>\r\n\r\n2\r\n00:00:04,000 --> 00:00:05,250\r\n1984\r\n\r\n3\r\n00:01:00,000 --> 00:01:02,000\r\nLast line\r\n';
  // A file from the user's PC can be named anything (Linux/macOS allow < > " in names).
  const NAME = '<img src=x onerror="window.__xss=1">.it.srt';
  await page.eval(`(() => {
    const dt = new DataTransfer();
    dt.items.add(new File([${JSON.stringify(SRT)}], ${JSON.stringify(NAME)}, { type: 'application/x-subrip' }));
    const inp = document.getElementById('input-sub'); inp.files = dt.files;
    inp.dispatchEvent(new Event('change', { bubbles: true })); return true;
  })()`);
  await t.test('SRT timing and text survive the conversion', async () => {
    const cues = await page.waitFor(`(() => { const tt = document.getElementById('v').textTracks[0]; return tt && tt.cues && tt.cues.length && [...tt.cues].map((c) => [c.startTime, c.endTime, c.text]); })()`, { timeout: 15000 });
    assert.deepEqual(cues, [[1.5, 3, 'Ciao <i>mondo</i>'], [4, 5.25, '1984'], [60, 62, 'Last line']],
      'every cue keeps its timing (comma → dot) and its text — a line that is only a number is dialogue, not a cue index');
    assert.equal(await page.eval(`document.getElementById('v').textTracks[0].language`), 'it', 'the .it. in the name is the track language');
  });

  await t.test('the subtitle menu lists the track by its name, as text', async () => {
    await page.waitFor(`document.querySelectorAll('#sub-pop .pop-opt[data-sub]').length === 2`, { timeout: 5000 });
    await new Promise((r) => setTimeout(r, 500));                  // give an injected <img onerror> time to fire
    assert.equal(await page.eval(`document.querySelectorAll('#sub-pop img').length`), 0, 'a file name must never become markup');
    assert.equal(await page.eval(`window.__xss`), undefined, 'markup in a file name ran script');
    const labels = await page.eval(`[...document.querySelectorAll('#sub-pop .pop-opt[data-sub] .opt-name')].map((e) => e.textContent)`);
    assert.ok(labels.includes('<img src=x onerror="window.__xss=1">.it'), 'the label is the literal name: ' + JSON.stringify(labels));
  });
});

test('video-player: the SD browser shows folder names as text and opens the picked video with its folder as playlist', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/R&amp;D/demo.mp4': VID, '/data/R&amp;D/demo2.webm': VID, '/data/R&amp;D/readme.txt': 'x' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`document.getElementById('sd-status-text').textContent !== ${JSON.stringify(EN.sdDash)}`, { timeout: 45000 }), 'the app started');
  await page.eval(key('o', { ctrlKey: true, shiftKey: true }));     // Ctrl+Shift+O = open from SD
  const dir = `[...document.querySelectorAll('#explorer-list .explorer-item')].find((li) => li.querySelector('.explorer-name').textContent === 'R&amp;D')`;
  assert.ok(await page.waitFor(`!!${dir}`, { timeout: 15000 }), 'the folder is listed under its literal name');
  await page.eval(`${dir}.click(), true`);
  const crumbs = await page.waitFor(`(() => { const s = [...document.querySelectorAll('#modal-crumbs .crumbs-seg')]; return s.length === 3 && s.map((e) => e.textContent); })()`, { timeout: 15000 });
  assert.deepEqual(crumbs, ['SD', 'data', 'R&amp;D'], 'the breadcrumb shows the folder name as text, not decoded markup');
  const items = await page.waitFor(`(() => { const n = [...document.querySelectorAll('#explorer-list .explorer-name')].map((e) => e.textContent).filter((x) => !x.startsWith('..')); return n.length === 2 && n; })()`, { timeout: 15000 });
  assert.deepEqual([...items].sort(), ['demo.mp4', 'demo2.webm'], 'videos only');
  await page.eval(`[...document.querySelectorAll('#explorer-list .explorer-item')].find((li) => li.querySelector('.explorer-name').textContent === 'demo.mp4').click(), true`);
  assert.equal(await page.waitFor(PLAYING, { timeout: 15000 }), '/data/R&amp;D/demo.mp4', 'the picked file plays from the right path');
  assert.equal(await page.eval(`document.querySelectorAll('#playlist-items .playlist-chip').length`), 2, 'its folder became the playlist');
  // The breadcrumb's data-path must round-trip too: clicking "R&amp;D" lists that folder again (not "R&D").
  await page.eval(key('o', { ctrlKey: true, shiftKey: true }));
  await page.waitFor(`!!${dir}`, { timeout: 15000 });
  await page.eval(`${dir}.click(), true`);
  await page.waitFor(`document.querySelectorAll('#modal-crumbs .crumbs-seg').length === 3`, { timeout: 15000 });
  await page.eval(`[...document.querySelectorAll('#modal-crumbs .crumbs-seg')][2].click(), true`);
  assert.ok(await page.waitFor(`[...document.querySelectorAll('#explorer-list .explorer-name')].some((e) => e.textContent === 'demo.mp4')`, { timeout: 15000 }),
    'the breadcrumb navigates to the real folder');
});

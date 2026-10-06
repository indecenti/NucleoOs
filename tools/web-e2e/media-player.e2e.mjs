// Browser E2E — Media Player against the simulator: the library is /data/Music filtered to audio (names as
// text), a missing folder is an EMPTY library (not "device offline"), a finished track advances to the next
// one, and a track opened from Files (?path=) is part of that playlist — "next" continues from it.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const EN = JSON.parse((await import('node:fs')).readFileSync(new URL('../../apps/media-player/www/i18n.en.json', import.meta.url), 'utf8'));
const AUDIO = 'ID3\u0003\u0000\u0000\u0000\u0000\u0000\u0000';     // the bytes do not matter: playback is not what is tested

async function open(browser, sim, query = '') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/media-player/${query}`);
  return page;
}
// The SD path the <audio> element is playing ('' = nothing).
const PLAYING = `(() => { const s = document.getElementById('player').getAttribute('src'); return s ? new URL(s, location.href).searchParams.get('path') : ''; })()`;
const ROWS = `[...document.querySelectorAll('#list li:not(.muted)')].map((li) => li.lastElementChild.textContent)`;
const ACTIVE = `[...document.querySelectorAll('#list li.active')].map((li) => li.lastElementChild.textContent)`;
const ENDED = `document.getElementById('player').dispatchEvent(new Event('ended')), true`;
const key = (k) => `document.dispatchEvent(new KeyboardEvent('keydown', { key: ${JSON.stringify(k)}, bubbles: true })), true`;
const NOTE = `(() => { const li = document.querySelector('#list li.muted'); return li && li.textContent; })()`;

test('media-player: the library is the audio in /data/Music, names as text; a finished track plays the next', { skip }, async (t) => {
  const sim = await startSim({ seed: {
    '/data/Music/01 intro.mp3': AUDIO, '/data/Music/02 R&amp;B.MP3': AUDIO, '/data/Music/03 outro.wav': AUDIO,
    '/data/Music/cover.jpg': 'x', '/data/Music/lyrics.txt': 'x', '/data/Music/live.mp3/inner.mp3': AUDIO,   // a FOLDER named like a track
  } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list li:not(.muted)').length >= 3`, { timeout: 45000 }), 'the library loaded');
  await page.networkIdle({ quiet: 400, timeout: 10000 });
  const rows = await page.eval(ROWS);
  assert.deepEqual([...rows].sort(), ['01 intro.mp3', '02 R&amp;B.MP3', '03 outro.wav'], 'mp3/wav only (any case), no folder, no cover/lyrics; &amp; stays literal');
  assert.equal(await page.eval(`document.querySelectorAll('#list li span *').length`), 0, 'a name is never parsed as markup');
  assert.equal(await page.eval(PLAYING), '', 'nothing autoplays');

  await page.eval(`document.querySelectorAll('#list li:not(.muted)')[0].click(), true`);
  assert.equal(await page.eval(PLAYING), '/data/Music/' + rows[0], 'clicking a track plays it from the SD');
  assert.deepEqual(await page.eval(ACTIVE), [rows[0]]);
  await page.eval(ENDED);
  assert.equal(await page.eval(PLAYING), '/data/Music/' + rows[1], 'track ended → the next one plays');
  assert.deepEqual(await page.eval(ACTIVE), [rows[1]], 'and the highlight follows');
  await page.eval(ENDED);
  assert.equal(await page.eval(PLAYING), '/data/Music/' + rows[2]);
  await page.eval(ENDED);
  assert.equal(await page.eval(PLAYING), '/data/Music/' + rows[2], 'the last track ending does not wrap around to the first');
  await page.eval(key('p'));
  assert.equal(await page.eval(PLAYING), '/data/Music/' + rows[1], '"p" = previous track');
  await page.eval(key('n'));
  assert.equal(await page.eval(PLAYING), '/data/Music/' + rows[2], '"n" = next track');
});

test('media-player: a track opened from Files (?path=) continues into the rest of the library', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/Music/a.mp3': AUDIO, '/data/Music/b.mp3': AUDIO, '/data/Music/c.mp3': AUDIO } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const openB = async () => {
    const page = await open(browser, sim, '?path=' + encodeURIComponent('/data/Music/b.mp3'));
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li:not(.muted)').length === 3`, { timeout: 45000 }), 'the library loaded');
    await page.networkIdle({ quiet: 400, timeout: 10000 });
    const rows = await page.eval(ROWS);
    return { page, rows, at: rows.indexOf('b.mp3') };
  };

  await t.test('the opened track plays and is highlighted in the library', async () => {
    const { page } = await openB();
    assert.equal(await page.eval(PLAYING), '/data/Music/b.mp3', 'the opened file plays');
    assert.deepEqual(await page.eval(ACTIVE), ['b.mp3'], 'the opened track is highlighted in the library');
  });

  await t.test('"n" plays the track after the opened one, not the first of the library', async () => {
    const { page, rows, at } = await openB();
    await page.eval(key('n'));
    assert.equal(await page.eval(PLAYING), '/data/Music/' + rows[(at + 1) % rows.length]);
  });

  await t.test('when the opened track ends, the next one plays (not silence)', async () => {
    const { page, rows, at } = await openB();
    await page.eval(ENDED);
    // The last track of the library ending stops (no wrap) — same rule as for a track picked in the list.
    assert.equal(await page.eval(PLAYING), '/data/Music/' + rows[Math.min(at + 1, rows.length - 1)]);
  });
});

test('media-player: no /data/Music = an empty library; a failed listing = "cannot reach"', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  assert.equal(await page.waitFor(NOTE, { timeout: 45000 }), EN.empty_list, 'a 404 folder is an empty library, not an offline device');
  await page.eval(key('n'));
  assert.equal(await page.eval(PLAYING), '', '"next" in an empty library does nothing');

  await sim.control('/api/_sim/fault', { route: '/api/fs/list', status: 503 });
  const p2 = await open(browser, sim);
  const note = await p2.waitFor(NOTE, { timeout: 45000 });
  await sim.control('/api/_sim/fault', { clear: true });
  assert.equal(note, EN.load_failed, 'a failed listing IS reported as unreachable');
});

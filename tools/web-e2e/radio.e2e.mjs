// Browser E2E — Radio Index against the simulator. The station list in /system/config/radio.json is what
// the Cardputer's Radio app tunes, so every edit must land in that file exactly:
//   - add / edit / star (device default) / reorder / delete persist, ids stay stable, names are TEXT;
//   - the editor refuses a stream address that is not a real http(s) URL (the prefilled "http://" alone
//     used to be accepted and saved as a station the device can never play) and says why;
//   - two quick edits reach the card in order — the last edit is what the device keeps, even when the
//     first write is slow;
//   - playback: a dead stream says so, and now-playing metadata (remote JSON) is shown as text.
// The read-failure guard (a failed read never clobbers the list) lives in app-data-safety.e2e.mjs.
// No external services: every stream / metadata URL points back at the simulator.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { writeFile, mkdir } from 'node:fs/promises';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const EN = { ...rd('../../web/shell/i18n/core.en.json'), ...rd('../../apps/radio/www/i18n.en.json') };
const CFG = '/system/config/radio.json';
const HOSTILE = '<img src=x onerror="window.__pwned=1">Rock & <b>Roll</b>';

const STATIONS = () => ({
  schema: 2, default: 'a',
  stations: [
    { id: 'a', name: 'Alpha', genre: 'Jazz', stream: 'http://radio.test/a.mp3', nowplaying: '' },
    { id: 'b', name: HOSTILE, genre: '<i>Genre</i>', stream: 'http://radio.test/b.mp3', nowplaying: '' },
    { id: 'c', name: 'Charlie', genre: '', stream: 'https://radio.test/c.mp3', nowplaying: '' },
  ],
  credits: 'test credits',
});

async function open(browser, sim) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/radio/`);
  await page.waitFor(`document.querySelectorAll('#list li.st').length > 0`, { timeout: 45000 });
  await page.networkIdle({ quiet: 400, timeout: 15000 });
  return page;
}
// Poll the SD file until pred(json) holds (the app saves asynchronously); returns the last parsed value.
async function waitSd(sim, pred, ms = 10000) {
  const end = Date.now() + ms;
  let last = null;
  for (;;) {
    try { last = JSON.parse(await sim.readSd(CFG)); if (pred(last)) return last; } catch {}
    if (Date.now() > end) return last;
    await sleep(100);
  }
}
const NAMES = `[...document.querySelectorAll('#list li.st .name')].map((n) => n.textContent)`;
const rowBtn = (i, n) => `document.querySelectorAll('#list li.st')[${i}].querySelectorAll('.iconbtn')[${n}].click(), true`;   // 0 play 1 star 2 edit 3 up 4 down 5 del
const fillEditor = (f) => `(() => {
  const set = (id, v) => { if (v !== undefined) { const e = document.getElementById(id); e.value = v; e.dispatchEvent(new Event('input')); } };
  set('fName', ${JSON.stringify(f.name)}); set('fGenre', ${JSON.stringify(f.genre)}); set('fStream', ${JSON.stringify(f.stream)}); set('fNp', ${JSON.stringify(f.np)});
  document.getElementById('dlgSave').click(); return true; })()`;

test('radio: add / edit / star / reorder / delete land in radio.json exactly, and station names are text', { skip }, async (t) => {
  const sim = await startSim({ seed: { [CFG]: STATIONS() } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  assert.deepEqual(await page.eval(NAMES), ['Alpha', HOSTILE, 'Charlie'], 'names shown verbatim');
  assert.equal(await page.eval(`document.querySelectorAll('#list .name *, #list .genre *').length`), 0, 'a station name / genre is never parsed as markup');
  assert.equal(await page.eval(`document.querySelectorAll('#list li.st')[1].querySelector('.genre').textContent`), '<i>Genre</i>');
  assert.equal(await page.eval(`window.__pwned === undefined`), true);
  assert.equal(await page.eval(`document.querySelectorAll('#list li.st')[2].querySelector('.badge.warn') !== null`), true, 'the https station is tagged browser-only');

  await t.test('star -> the device default', async () => {
    await page.eval(rowBtn(1, 1));
    const f = await waitSd(sim, (j) => j.default === 'b');
    assert.equal(f.default, 'b');
    assert.deepEqual(f.stations.map((s) => s.id), ['a', 'b', 'c'], 'starring does not reorder');
    assert.equal(await page.eval(`document.querySelectorAll('#list li.st')[1].querySelector('.badge.def') !== null`), true);
  });

  await t.test('reorder', async () => {
    await page.eval(rowBtn(2, 3));          // Charlie up
    let f = await waitSd(sim, (j) => j.stations.map((s) => s.id).join() === 'a,c,b');
    assert.deepEqual(f.stations.map((s) => s.id), ['a', 'c', 'b']);
    await page.eval(rowBtn(0, 4));          // Alpha down
    f = await waitSd(sim, (j) => j.stations.map((s) => s.id).join() === 'c,a,b');
    assert.deepEqual(f.stations.map((s) => s.id), ['c', 'a', 'b']);
    assert.equal(f.default, 'b', 'the default follows the station, not the position');
    assert.deepEqual(await page.eval(NAMES), ['Charlie', 'Alpha', HOSTILE]);
    assert.equal(await page.eval(`document.querySelectorAll('#list li.st')[0].querySelectorAll('.iconbtn')[3].disabled`), true, 'the first row cannot move up');
  });

  await t.test('edit keeps the id', async () => {
    await page.eval(rowBtn(1, 2));          // edit Alpha
    assert.equal(await page.eval(`document.getElementById('fName').value`), 'Alpha', 'the editor opens on that station');
    await page.eval(fillEditor({ name: 'Alpha <2>', genre: 'Bebop', stream: 'http://radio.test:8000/live' }));
    const f = await waitSd(sim, (j) => j.stations[1].name === 'Alpha <2>');
    assert.deepEqual(f.stations[1], { id: 'a', name: 'Alpha <2>', genre: 'Bebop', stream: 'http://radio.test:8000/live', nowplaying: '' });
  });

  await t.test('add appends with a fresh id; the default is untouched', async () => {
    await page.eval(`document.getElementById('add').click(), true`);
    assert.equal(await page.eval(`document.getElementById('fStream').value`), 'http://', 'the stream field is prefilled with the scheme');
    await page.eval(fillEditor({ name: 'Charlie', genre: 'Dup name', stream: 'http://radio.test/c2.mp3' }));
    const f = await waitSd(sim, (j) => j.stations.length === 4);
    assert.equal(f.stations.length, 4);
    const added = f.stations[3];
    assert.equal(added.name, 'Charlie'); assert.equal(added.stream, 'http://radio.test/c2.mp3');
    assert.equal(new Set(f.stations.map((s) => s.id)).size, 4, 'ids stay unique even with a duplicate name');
    assert.equal(f.default, 'b');
  });

  await t.test('delete (after the confirm) removes exactly that station', async () => {
    await page.eval(rowBtn(2, 5));          // the hostile-named station (the default)
    const f = await waitSd(sim, (j) => j.stations.length === 3);
    assert.deepEqual(f.stations.map((s) => s.name), ['Charlie', 'Alpha <2>', 'Charlie']);
    assert.equal(f.default, f.stations[0].id, 'deleting the default hands the star to the first station');
    assert.equal(await page.eval(`document.querySelector('#list li.st .badge.def').closest('li') === document.querySelector('#list li.st')`), true);
  });
});

test('radio: the editor refuses a stream address that is not a real http(s) URL, and says why', { skip }, async (t) => {
  const sim = await startSim({ seed: { [CFG]: STATIONS() } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  const before = await sim.readSd(CFG);
  const note = `document.getElementById('streamNote').textContent`;

  for (const bad of ['http://', 'https://', 'http:///stream.mp3', 'ftp://radio.test/a.mp3', 'javascript:alert(1)', 'radio.test/stream.mp3', 'http://radio .test/a.mp3', 'http://radio.test/a b.mp3']) {
    await page.eval(`document.getElementById('add').click(), true`);
    await page.eval(fillEditor({ name: 'Bad', stream: bad }));
    await sleep(400);
    assert.equal(await page.eval(`document.getElementById('scrim').classList.contains('on')`), true, `"${bad}": the editor stays open`);
    assert.equal(await page.eval(note), EN.note_stream_invalid, `"${bad}": the note says what is wrong`);
    assert.equal(await page.eval(`document.querySelectorAll('#list li.st').length`), 3, `"${bad}": no station added`);
    await page.eval(`document.getElementById('dlgCancel').click(), true`);
  }
  assert.equal(await sim.readSd(CFG), before, 'radio.json untouched');

  // Valid ones still go through: an IP:port stream, and https (with the browser-only warning).
  await page.eval(`document.getElementById('add').click(), true`);
  await page.eval(`(() => { const e = document.getElementById('fStream'); e.value = 'https://radio.test/s'; e.dispatchEvent(new Event('input')); return true; })()`);
  assert.equal(await page.eval(note), EN.note_stream_warn, 'typing https warns it will not play on the device');
  await page.eval(fillEditor({ name: 'Secure', stream: 'https://radio.test/s' }));
  await page.eval(`document.getElementById('add').click(), true`);
  await page.eval(fillEditor({ name: 'LAN', stream: 'http://192.168.1.50:8000/live' }));
  const f = await waitSd(sim, (j) => j.stations.length === 5);
  assert.deepEqual(f.stations.slice(3).map((s) => s.stream), ['https://radio.test/s', 'http://192.168.1.50:8000/live']);
});

test('radio: two quick edits reach the card in order — the last one is what the device keeps', { skip }, async (t) => {
  const sim = await startSim({ seed: { [CFG]: STATIONS() } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);

  // The first write is slow (busy SD / WiFi retry); the second one is not.
  await sim.control('/api/_sim/fault', { route: '/api/fs/write', delay_ms: 1500, times: 1 });
  await page.eval(rowBtn(1, 1));            // star B …
  await sleep(150);
  await page.eval(rowBtn(2, 1));            // … then, changing my mind, star C
  await sleep(3000);
  await sim.control('/api/_sim/fault', { clear: true });
  const f = JSON.parse(await sim.readSd(CFG));
  assert.equal(f.default, 'c', 'the device must end on the LAST choice, not on the slow first write');
  assert.equal(await page.eval(`document.querySelectorAll('#list li.st')[2].querySelector('.badge.def') !== null`), true, 'and the screen agrees');
  assert.equal(await page.eval(`document.getElementById('save').textContent`), EN.saved_ok);
});

// 0.5 s of a 440 Hz tone as 8 kHz / 16-bit mono WAV — a stream the browser can actually decode.
function wav() {
  const sr = 8000, n = sr / 2, b = Buffer.alloc(44 + n * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n * 2, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(sr, 24); b.writeUInt32LE(sr * 2, 28);
  b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) b.writeInt16LE(Math.round(8000 * Math.sin(2 * Math.PI * 440 * i / sr)), 44 + i * 2);
  return b;
}

test('radio: a dead stream says so; now-playing metadata is shown as text; next/prev walk the list', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const origin = sim.origin;
  await mkdir(join(sim.sd, 'data'), { recursive: true });
  await writeFile(join(sim.sd, 'data', 'tone.wav'), wav());
  await writeFile(join(sim.sd, 'data', 'np.json'), JSON.stringify({ track: 'x', track_obj: { artist: '<b>Evil</b> & Co', title: '<img src=x onerror="window.__pwned=1">' }, elapsed_sec: 10, duration_sec: 100 }));
  await mkdir(join(sim.sd, 'system', 'config'), { recursive: true });
  await writeFile(join(sim.sd, 'system', 'config', 'radio.json'), JSON.stringify({ schema: 2, default: 'live', stations: [
    { id: 'dead', name: 'Dead', genre: '', stream: `${origin}/api/fs/read?path=${encodeURIComponent('/data/missing.mp3')}`, nowplaying: '' },
    { id: 'live', name: 'Live', genre: 'Test', stream: `${origin}/api/fs/read?path=${encodeURIComponent('/data/tone.wav')}`, nowplaying: `${origin}/api/fs/read?path=${encodeURIComponent('/data/np.json')}` },
  ] }));
  const page = await open(browser, sim);
  const track = `document.getElementById('npTrack').textContent`;

  await page.eval(rowBtn(0, 0));
  assert.equal(await page.eval(`document.getElementById('npStation').textContent`), 'Dead');
  assert.ok(await page.waitFor(`${track} === ${JSON.stringify(EN.err_unreachable)}`, { timeout: 15000 }), 'a stream that fails says so: ' + await page.eval(track));

  await page.eval(`document.getElementById('next').click(), true`);
  assert.equal(await page.eval(`document.getElementById('npStation').textContent`), 'Live', 'next station');
  const label = '● LIVE · <b>Evil</b> & Co — <img src=x onerror="window.__pwned=1">';
  assert.ok(await page.waitFor(`${track} === ${JSON.stringify(label)}`, { timeout: 15000 }), 'now-playing label: ' + await page.eval(track));
  assert.equal(await page.eval(`document.querySelectorAll('#npTrack b, #npTrack img').length`), 0, 'remote metadata is never parsed as markup');
  assert.equal(await page.eval(`window.__pwned === undefined`), true);
  assert.equal(await page.eval(`document.getElementById('barWrap').classList.contains('hidden')`), false, 'a track with a duration shows the progress bar');
  assert.equal(await page.eval(`document.getElementById('tDur').textContent`), '1:40');
  assert.equal(await page.eval(`document.querySelector('#list li.st.playing .name').textContent`), 'Live', 'the playing row is marked');

  await page.eval(`document.getElementById('next').click(), true`);
  assert.equal(await page.eval(`document.getElementById('npStation').textContent`), 'Dead', 'next wraps around');
  await page.eval(`document.getElementById('prev').click(), true`);
  assert.equal(await page.eval(`document.getElementById('npStation').textContent`), 'Live', 'prev wraps back');
});

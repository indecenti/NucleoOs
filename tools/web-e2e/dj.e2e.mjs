// Browser E2E — DJ Mix against the simulator, with real Web Audio decoding of small WAV tracks.
//   - the library lists only the audio in /data/Music, file names as TEXT; a listing that FAILS says so
//     (it used to read "No tracks", indistinguishable from an empty folder);
//   - cueing is "last click wins": a slow track clicked first must not land on deck B after a quicker
//     one clicked second (deck label and the audio that plays must be the second track);
//   - MIX pressed again, or a track cued, while a crossfade is running must not scramble the decks:
//     when the fade ends deck A is the track now playing and deck B is empty.
// The engine is observed through Web Audio itself: every AudioBufferSourceNode.start() is recorded with
// its buffer duration (each test track has a distinct length).
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
const EN = { ...rd('../../web/shell/i18n/core.en.json'), ...rd('../../apps/dj/www/i18n.en.json') };

function wav(seconds, freq) {                     // 8 kHz / 16-bit mono tone
  const sr = 8000, n = Math.round(sr * seconds), b = Buffer.alloc(44 + n * 2);
  b.write('RIFF', 0); b.writeUInt32LE(36 + n * 2, 4); b.write('WAVEfmt ', 8); b.writeUInt32LE(16, 16);
  b.writeUInt16LE(1, 20); b.writeUInt16LE(1, 22); b.writeUInt32LE(sr, 24); b.writeUInt32LE(sr * 2, 28);
  b.writeUInt16LE(2, 32); b.writeUInt16LE(16, 34); b.write('data', 36); b.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) b.writeInt16LE(Math.round(6000 * Math.sin(2 * Math.PI * freq * i / sr)), 44 + i * 2);
  return b;
}
const DUR = { 'one.wav': 1.0, 'two.wav': 0.5, 'three.wav': 0.75 };
async function music(sim, extra = {}) {
  const dir = join(sim.sd, 'data', 'Music');
  await mkdir(join(dir, 'Sub.mp3'), { recursive: true });                  // a FOLDER named like a track
  for (const [f, s] of Object.entries(DUR)) await writeFile(join(dir, f), wav(s, 330));
  for (const [f, body] of Object.entries(extra)) await writeFile(join(dir, f), body);
}
// Record every buffer source that starts (its duration identifies the track); optionally slow down the
// read of one track so two cue clicks finish in the opposite order.
const PROBE = (slow) => `(() => {
  if (!location.pathname.startsWith('/apps/dj/')) return;
  window.__starts = [];
  const st = AudioBufferSourceNode.prototype.start;
  AudioBufferSourceNode.prototype.start = function (...a) { window.__starts.push(this.buffer ? +this.buffer.duration.toFixed(2) : null); return st.apply(this, a); };
  const SLOW = ${JSON.stringify(slow || '')};
  if (SLOW) {
    const real = window.fetch.bind(window);
    window.fetch = async (u, o) => {
      const s = String(u);
      if (s.includes('/api/fs/read') && decodeURIComponent(s).endsWith('/' + SLOW)) await new Promise((r) => setTimeout(r, 1500));
      return real(u, o);
    };
  }
})();`;

async function open(browser, sim, slow) {
  const page = await browser.newPage();
  await page.initScript(PROBE(slow));
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/dj/`);
  await page.waitFor(`document.querySelector('#list li') && !document.querySelector('#list li[data-i18n="loading"]')`, { timeout: 45000 });
  return page;
}
const LIB = `[...document.querySelectorAll('#list li .name')].map((n) => n.textContent)`;
const cue = (name) => `[...document.querySelectorAll('#list li')].find((li) => li.querySelector('.name') && li.querySelector('.name').textContent === ${JSON.stringify(name)}).click(), true`;
const TXT = (id) => `document.getElementById(${JSON.stringify(id)}).textContent`;

test('dj: the library lists only the audio files in /data/Music, names as text; a failed listing says so', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const evil = '&lt;img src=x onerror=window.__pwned=1&gt; & co.mp3';      // valid on every file system
  await music(sim, { [evil]: Buffer.from('not really mp3'), 'one.npx': Buffer.from('x'), 'notes.txt': Buffer.from('x'), 'LOUD.WAV': wav(0.2, 200) });

  const page = await open(browser, sim);
  const names = await page.eval(LIB);
  assert.deepEqual([...names].sort(), [evil, 'LOUD.WAV', 'one.wav', 'three.wav', 'two.wav'].sort(), 'mp3/wav only (any case); no .npx sidecar, no .txt, no folder');
  assert.equal(await page.eval(`document.querySelectorAll('#list li .name *').length`), 0, 'a file name is never markup');
  assert.equal(await page.eval(`window.__pwned === undefined`), true);

  await sim.control('/api/_sim/fault', { route: '/api/fs/list', status: 503 });
  const p2 = await open(browser, sim);
  await sim.control('/api/_sim/fault', { clear: true });
  assert.equal(await p2.eval(`document.querySelector('#list li').textContent`), EN.library_failed, 'a failed listing is not "no tracks"');

  const sim2 = await startSim();
  t.after(() => sim2.stop());
  const p3 = await open(browser, sim2);
  assert.equal(await p3.eval(`document.querySelector('#list li').textContent`), EN.empty_library, 'a card without /data/Music: "no tracks"');
});

test('dj: cueing is "last click wins" — a slow first track never lands on deck B after the second', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await music(sim);
  const page = await open(browser, sim, 'one.wav');      // one.wav takes 1.5 s longer to read

  await page.eval(cue('one.wav'));
  await sleep(100);
  await page.eval(cue('two.wav'));
  assert.ok(await page.waitFor(`${TXT('bName')} === 'two.wav'`, { timeout: 10000 }), 'two.wav cued');
  await sleep(2500);                                    // let the slow one.wav read finish
  assert.equal(await page.eval(TXT('bName')), 'two.wav', 'deck B still shows the LAST click');
  assert.equal(await page.eval(TXT('status')), EN.st_b_ready);
  assert.deepEqual(await page.eval(`[...document.querySelectorAll('#list li.cued .name')].map((n) => n.textContent)`), ['two.wav'], 'only the last click is marked cued');

  await page.eval(`document.getElementById('mix').click(), true`);
  assert.ok(await page.waitFor(`window.__starts.length > 0`, { timeout: 5000 }), 'something plays');
  assert.deepEqual(await page.eval(`window.__starts`), [DUR['two.wav']], 'and it is two.wav that plays, not the late one.wav');
  assert.equal(await page.eval(TXT('aName')), 'two.wav');
});

test('dj: MIX again, or a cue, during a running crossfade never scrambles the decks', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  await music(sim);
  const page = await open(browser, sim);

  await page.eval(cue('one.wav'));
  assert.ok(await page.waitFor(`${TXT('bName')} === 'one.wav'`, { timeout: 10000 }));
  await page.eval(`document.getElementById('mix').click(), true`);          // first play: B goes on air
  assert.ok(await page.waitFor(`${TXT('aName')} === 'one.wav'`, { timeout: 5000 }), 'one.wav on air');
  assert.equal(await page.eval(TXT('bName')), EN.empty_deck);

  await page.eval(cue('two.wav'));
  assert.ok(await page.waitFor(`${TXT('bName')} === 'two.wav'`, { timeout: 10000 }));
  await page.eval(`document.getElementById('mix').click(), true`);          // crossfade one -> two
  assert.ok(await page.waitFor(`${TXT('status')} === ${JSON.stringify(EN.st_mixing)}`, { timeout: 3000 }));
  const xfadeSec = await page.eval(`parseFloat(document.getElementById('plan').textContent.split('·')[1])`);
  assert.ok(xfadeSec > 0 && xfadeSec < 30, 'plan shows the crossfade length: ' + xfadeSec);
  await sleep(300);
  await page.eval(`document.getElementById('mix').click(), true`);          // impatient second press
  await sleep(200);
  await page.eval(cue('three.wav'));                                       // and a cue while the fade runs
  await sleep(1500);
  const startsMidFade = await page.eval(`window.__starts.slice()`);

  assert.ok(await page.waitFor(`${TXT('aName')} === 'two.wav' && ${TXT('bName')} === ${JSON.stringify(EN.empty_deck)}`, { timeout: (xfadeSec + 8) * 1000 }),
    `after the fade: A = two.wav, B empty (got A="${await page.eval(TXT('aName'))}", B="${await page.eval(TXT('bName'))}")`);
  await sleep(1500);                                                       // nothing else may fire later
  assert.equal(await page.eval(TXT('aName')), 'two.wav', 'deck A stays on the track that is playing');
  assert.equal(await page.eval(TXT('bName')), EN.empty_deck);
  assert.deepEqual(startsMidFade, [DUR['one.wav'], DUR['two.wav']], 'exactly one crossfade was started: no second start of two.wav, nothing from the mid-fade cue');

  // After the fade the decks work normally again.
  await page.eval(cue('three.wav'));
  assert.ok(await page.waitFor(`${TXT('bName')} === 'three.wav'`, { timeout: 10000 }), 'cueing works again once the fade is over');
});

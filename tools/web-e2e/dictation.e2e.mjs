// Browser E2E — Dictation against the simulator (its /api/rec/stream serves a PCM tone). Vosk is stubbed
// in the page (window.Vosk is what asr.js uses when present), so no model is ever downloaded and the test
// drives recognised text by hand. Covers: the remembered IT/EN recognition language is the one shown AND the
// model loaded; recognised text lands in /data/Transcripts with its header; a busy Cardputer mic is reported
// as such; "New" never throws away a transcript the device failed to save.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

// Every host but the simulator resolves to nothing: a CDN model or a cloud call can never leave the box.
const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1, MAP * ~NOTFOUND, EXCLUDE localhost, EXCLUDE 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const EN = { ...rd('../../web/shell/i18n/core.en.json'), ...rd('../../apps/dictation/www/i18n.en.json') };
const TDIR = '/data/Transcripts';

// A Vosk stand-in: records which model URL was asked for and exposes the recogniser so the test can "speak".
const FAKE_VOSK = `window.__models = []; window.Vosk = { createModel: async (url) => { window.__models.push(String(url)); return {
  registerPort() {},
  KaldiRecognizer: function () { const h = {}; this.id = 1; this.setWords = () => {}; this.on = (ev, fn) => { h[ev] = fn; };
    this.say = (text) => h.result && h.result({ result: { text } }); window.__rec = this; },
}; } };`;

async function open(browser, sim, { local = {}, before } = {}) {
  const page = await browser.newPage();
  await page.initScript(FAKE_VOSK);
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.eval(`(() => { for (const [k, v] of Object.entries(${JSON.stringify(local)})) localStorage.setItem(k, v); return true; })()`);
  if (before) await page.eval(before);
  await page.goto(`${sim.origin}/apps/dictation/`);
  return page;
}
const STATUS = `document.getElementById('statusTxt').textContent`;
const LANG_ON = `[...document.querySelectorAll('#lang button.on')].map((b) => b.dataset.l)`;
const listening = (page) => page.waitFor(`${STATUS} === ${JSON.stringify(EN.st_listening)}`, { timeout: 30000 });

test('dictation: the remembered recognition language is the one shown and loaded; text is saved with its header', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim, { local: { 'dictation.lang': 'it', 'dictation.file': 'trascrizione-e2e.txt', 'dictation.text': '' } });
  assert.ok(await listening(page), 'listening to the Cardputer mic');

  assert.deepEqual(await page.eval(LANG_ON), ['it'], 'IT is highlighted — the language it will actually recognise');
  assert.ok((await page.eval(`window.__models`)).every((u) => /small-it/.test(u)) && (await page.eval(`window.__models.length`)) >= 1, 'the Italian model was loaded');

  await page.eval(`window.__rec.say('ciao mondo'), true`);
  assert.ok(await page.waitFor(`document.getElementById('transcript').textContent === 'ciao mondo'`, { timeout: 5000 }), 'the text is on screen');
  assert.ok(await page.waitFor(`document.getElementById('saved').textContent === ${JSON.stringify(EN.saved_as.replace('{file}', 'trascrizione-e2e.txt'))}`, { timeout: 10000 }), 'and saved');
  const body = await sim.readSd(`${TDIR}/trascrizione-e2e.txt`);
  assert.match(body, /\nLanguage: IT\n/, 'the header names the recognition language');
  assert.ok(body.trimEnd().endsWith('\nciao mondo'), 'the transcript follows the header');

  await page.eval(`[...document.querySelectorAll('#lang button')].find((b) => b.dataset.l === 'en').click(), true`);
  assert.deepEqual(await page.eval(LANG_ON), ['en'], 'switching shows EN');
  assert.equal(await page.eval(`localStorage.getItem('dictation.lang')`), 'en', 'and remembers it');
  assert.ok(await page.waitFor(`window.__models.some((u) => /small-en-us/.test(u))`, { timeout: 15000 }), 'the English model is loaded for the running session');
  assert.ok(await listening(page), 'still listening after the switch');
});

test('dictation: a busy Cardputer mic is reported, and the app works once it is free', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  // The on-device recorder holds the mic (it holds no web lock — only /api/rec/status knows).
  const page = await open(browser, sim, { before: `fetch('/api/rec/start', { method: 'POST' }).then((r) => r.status)` });
  assert.ok(await page.waitFor(`${STATUS} === ${JSON.stringify(EN.st_mic_busy)}`, { timeout: 30000 }), 'status: mic busy');
  assert.ok(await page.eval(`document.getElementById('notice').classList.contains('show') && document.getElementById('notice').textContent.startsWith('Microphone busy.')`), 'the notice explains it');
  assert.equal(await page.eval(`document.getElementById('loadingModal').style.display`), 'none', 'no spinner left behind');

  await page.eval(`fetch('/api/rec/stop', { method: 'POST' }).then((r) => r.status)`);
  await page.eval(`document.getElementById('btnRec').click(), true`);
  assert.ok(await listening(page), 'once the mic is free a click starts listening');
  assert.equal(await page.eval(`document.getElementById('notice').classList.contains('show')`), false, 'the busy notice is gone');
});

test('dictation: "New" never throws away a transcript the device failed to save', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const words = 'the only copy of an important meeting';
  const page = await open(browser, sim, { local: { 'dictation.lang': 'en', 'dictation.file': 'trascrizione-keep.txt', 'dictation.text': words } });
  assert.ok(await listening(page));
  await page.networkIdle({ quiet: 400, timeout: 10000 });
  await page.eval(`window.confirm = () => true; true`);

  await t.test('device refuses the write: the text stays (screen + local copy) and the user is told', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 503 });
    await page.eval(`document.getElementById('btnNew').click(), true`);
    assert.ok(await page.waitFor(`document.getElementById('notice').classList.contains('show') && document.getElementById('notice').textContent === ${JSON.stringify(EN.new_unsaved)}`, { timeout: 10000 }), 'the user learns the transcript was not saved');
    await sleep(800);
    assert.equal(await page.eval(`document.getElementById('transcript').textContent`), words, 'the transcript is still on screen');
    assert.equal(await page.eval(`localStorage.getItem('dictation.text')`), words, 'and in the local copy');
    assert.equal(await page.eval(`localStorage.getItem('dictation.file')`), 'trascrizione-keep.txt', 'still the same session');
    assert.ok(!existsSync(join(sim.sd, 'data', 'Transcripts', 'trascrizione-keep.txt')), 'nothing reached the SD');
  });

  await t.test('device back: "New" saves the old transcript first, then starts an empty one', async () => {
    await sim.control('/api/_sim/fault', { clear: true });
    await page.eval(`document.getElementById('btnNew').click(), true`);
    assert.ok(await page.waitFor(`localStorage.getItem('dictation.file') !== 'trascrizione-keep.txt'`, { timeout: 10000 }), 'a new session started');
    await page.networkIdle({ quiet: 400, timeout: 10000 });
    assert.ok((await sim.readSd(`${TDIR}/trascrizione-keep.txt`)).trimEnd().endsWith(words), 'the old transcript is on the SD');
    assert.equal(await page.eval(`localStorage.getItem('dictation.text')`), '');
    assert.equal(await page.eval(`document.getElementById('words').textContent`), EN.wordCount.other.replace('{count}', '0'));
  });
});

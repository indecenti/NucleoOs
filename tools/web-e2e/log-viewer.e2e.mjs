// Browser E2E — Log Viewer against the simulator: live events (forwarded by the shell as postMessage) render
// topic and payload as TEXT, the filter narrows them and the status bar counts both; Pause stops the tail and
// Clear empties it; History reads only the tail of a large SD journal, skips malformed lines without losing
// the good ones, and tells "no journal yet" from "journal unreadable".
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const EN = JSON.parse(readFileSync(new URL('../../apps/log-viewer/www/i18n.en.json', import.meta.url), 'utf8'));
const JOURNAL = '/journal/events.ndjson';
const CAP = 600;

async function open(browser, sim) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/log-viewer/`);
  assert.ok(await page.waitFor(`typeof window.__t === 'function' && document.getElementById('stat').textContent !== ''`, { timeout: 45000 }), 'log viewer up');
  // Health probe off (it synthesises rows on its own schedule), then a clean slate.
  await page.eval(`document.getElementById('health').click(), true`);
  await page.networkIdle({ quiet: 500, timeout: 10000 });
  await sleep(300);
  await page.eval(`document.getElementById('clear').click(), true`);
  return page;
}
const post = (ev) => `window.postMessage(${JSON.stringify(ev)}, '*'), true`;
const ROWS = `[...document.querySelectorAll('#log .ev')].map((r) => ({ seq: r.querySelector('.seq').textContent, t: r.querySelector('.t').textContent, d: r.querySelector('.d').textContent }))`;
const STAT = `document.getElementById('stat').textContent`;
const SRC = `document.getElementById('src').textContent`;
const filter = (q) => `(() => { const f = document.getElementById('filter'); f.value = ${JSON.stringify(q)}; f.dispatchEvent(new Event('input')); return true; })()`;
const nEvents = (n) => (n === 1 ? EN.events.one : EN.events.other).replace('{count}', n);

test('log-viewer: live events render as text; filter, pause and clear do what they say', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  const ts = Math.floor(Date.now() / 1000);
  const evil = '<img src=x onerror="window.__pwn=1">&amp;';
  await page.eval(post({ t: 'fs.changed', seq: 11, ts, d: { op: 'write', path: '/data/' + evil } }));
  await page.eval(post({ t: 'wifi.<b>bold</b>', seq: 12, ts, d: 'plain string payload' }));
  await page.eval(post({ t: 'system.error', seq: 13, ts, d: { lvl: 'critical', msg: 'brownout' } }));
  await page.eval(post({ seq: 14, d: 'no topic: not an event' }));
  assert.ok(await page.waitFor(`document.querySelectorAll('#log .ev').length === 3`, { timeout: 5000 }), 'three events (one without a topic ignored)');
  const rows = await page.eval(ROWS);
  assert.deepEqual(rows.map((r) => r.seq), ['#11', '#12', '#13']);
  assert.equal(rows[0].d, JSON.stringify({ op: 'write', path: '/data/' + evil }), 'a payload is shown literally');
  assert.equal(rows[1].t, 'wifi.<b>bold</b>', 'a topic is shown literally');
  assert.equal(await page.eval(`document.querySelectorAll('#log .ev img, #log .ev b').length`), 0, 'nothing in an event becomes markup');
  assert.equal(await page.eval(`window.__pwn`), undefined, 'no handler ran');
  assert.equal(await page.eval(`document.querySelector('#log .ev:nth-child(3)').classList.contains('lvl-crit')`), true, 'a critical event is flagged');
  assert.equal(await page.eval(STAT), nEvents(3));

  await page.eval(filter('BROWNOUT'));
  assert.deepEqual((await page.eval(ROWS)).map((r) => r.seq), ['#13'], 'the filter matches the payload, case-insensitively');
  assert.equal(await page.eval(STAT), nEvents(1) + EN.events_of.replace('{total}', 3));
  await page.eval(filter('fs.'));
  assert.deepEqual((await page.eval(ROWS)).map((r) => r.seq), ['#11'], 'and the topic');
  await page.eval(filter(''));

  await page.eval(`document.getElementById('pause').click(), true`);
  await page.eval(post({ t: 'rec.started', seq: 15, ts, d: {} }));
  await sleep(300);
  assert.equal(await page.eval(`document.querySelectorAll('#log .ev').length`), 3, 'paused: the tail does not move');
  assert.ok((await page.eval(STAT)).endsWith(EN.status_paused), 'and the status bar says so');
  await page.eval(`document.getElementById('pause').click(), true`);
  await page.eval(post({ t: 'rec.stopped', seq: 16, ts, d: {} }));
  assert.ok(await page.waitFor(`document.querySelectorAll('#log .ev').length === 4`, { timeout: 5000 }), 'resumed');

  await page.eval(`document.getElementById('clear').click(), true`);
  assert.equal(await page.eval(`document.querySelectorAll('#log .ev').length`), 0, 'clear empties the view');
  assert.equal(await page.eval(STAT), nEvents(0));
});

test('log-viewer: History shows the tail of a large journal and keeps the good lines around malformed ones', { skip }, async (t) => {
  const N = 2000, ts = 1790000000;
  const lines = [];
  for (let i = 1; i <= N; i++) lines.push(JSON.stringify({ seq: i, ts: ts + i, t: i % 50 ? 'fs.changed' : 'health.mem', d: { path: `/data/f${i}.txt`, note: i === N ? '<script>x</script>' : '' } }));
  // A torn/garbled tail (power loss mid-append, a truncated payload): none of it may cost the good lines.
  lines.splice(N - 5, 0, '{"seq":', 'null', '42', '"just a string"', '[1,2]', '{"seq":9999}');
  const sim = await startSim({ seed: { [JOURNAL]: lines.join('\n') + '\n' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  await page.eval(post({ t: 'live.only', seq: 5000, ts, d: 'arrived before History' }));
  await page.eval(post({ t: 'fs.changed', seq: N, ts, d: 'a live copy of the last journal line' }));
  await page.eval(`document.getElementById('history').click(), true`);
  assert.ok(await page.waitFor(`${SRC} === ${JSON.stringify(EN.src_journal.replace('{lines}', lines.length))}`, { timeout: 20000 }), 'the journal was read (' + lines.length + ' lines)');
  const rows = await page.eval(ROWS);
  assert.equal(rows.length, CAP, 'the view stays bounded');
  assert.equal(await page.eval(STAT), nEvents(CAP));
  assert.equal(rows.at(-1).seq, '#5000', 'live events stay after the history');
  assert.equal(rows.filter((r) => r.seq === '#' + N).length, 1, 'an event both live and in the journal is shown once');
  assert.ok(rows.some((r) => r.d.includes('<script>x</script>')), 'the last journal line is shown, as text');
  assert.ok(!rows.some((r) => r.t === 'undefined' || r.t === ''), 'lines without a topic are not shown as "undefined" rows');
  assert.equal(await page.eval(`document.querySelectorAll('#log script').length`), 0);
  assert.equal(await page.eval(`document.getElementById('history').disabled`), false, 'the button is usable again');
});

test('log-viewer: History tells "no journal yet" from "journal unreadable"', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await open(browser, sim);
  await page.eval(`document.getElementById('history').click(), true`);
  assert.equal(await page.waitFor(`${SRC}`, { timeout: 10000 }), EN.src_none, 'a 404 = no journal yet');

  await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
  await page.eval(`document.getElementById('src').textContent = ''; document.getElementById('history').click(), true`);
  const shown = await page.waitFor(`${SRC}`, { timeout: 10000 });
  await sim.control('/api/_sim/fault', { clear: true });
  assert.equal(shown, EN.src_error, 'a busy/failed device is "journal unavailable", not "no journal"');
});

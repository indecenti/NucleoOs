// Browser E2E — more apps that must never lose the user's data on the SD. Same failure class as
// data-safety.e2e.mjs: a FAILED read (offline, 401, 503, corrupt JSON) was treated as "empty", and the
// next edit wrote that empty (or built-in default) state over the real file. Only a 404 means "none yet".
// Covers: SSH saved hosts, Settings' secondary AI keys (teacher.json), Radio stations, Miei fatti,
// IR Remote user data.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { writeFile, rm } from 'node:fs/promises';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
// The app catalog over the shared core one, exactly like the I18N engine resolves a key.
const cat = (app, lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/${app}/www/i18n.${lang}.json`) });
const FAULT_READ = { route: '/api/fs/read', status: 503 };

async function openApp(browser, sim, id, lang = 'en', query = '') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/${id}/${query}`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  return page;
}

test('ssh: a failed read of the saved hosts never overwrites them when a host is added', { skip }, async (t) => {
  const hosts = { hosts: [{ id: 'h1', name: 'nas', host: '192.168.0.2', port: 22, user: 'root', auth: 'password' },
    { id: 'h2', name: 'pi', host: '192.168.0.3', port: 22, user: 'pi', auth: 'key' }] };
  const sim = await startSim({ seed: { '/data/ssh/hosts.json': hosts } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('ssh', 'en');
  const before = await sim.readSd('/data/ssh/hosts.json');
  const fill = `(() => { document.getElementById('new-btn').click();
    document.getElementById('f-host').value = '10.0.0.9'; document.getElementById('f-user').value = 'me';
    document.getElementById('f-save').click(); return true; })()`;

  await sim.control('/api/_sim/fault', FAULT_READ);
  const page = await openApp(browser, sim, 'ssh', 'en');

  await t.test('read fails: adding a host does not touch hosts.json, and the form says why', async () => {
    await page.eval(fill);
    await sleep(1500);
    assert.equal(await sim.readSd('/data/ssh/hosts.json'), before, 'the saved hosts were overwritten by a one-entry list');
    assert.equal(await page.eval(`document.getElementById('f-stat').textContent`), en.read_failed, 'the failure is explained');
    assert.equal(await page.eval(`document.getElementById('f-host').value`), '10.0.0.9', 'what was typed is kept for a retry');
  });

  await t.test('device back: the save re-reads and merges the new host into the real list', async () => {
    await sim.control('/api/_sim/fault', { clear: true });
    await page.eval(`document.getElementById('f-save').click(), true`);
    assert.ok(await page.waitFor(`document.getElementById('f-stat').textContent === ${JSON.stringify(en.saved)}`, { timeout: 10000 }), 'saved');
    const names = JSON.parse(await sim.readSd('/data/ssh/hosts.json')).hosts.map((h) => h.host).sort();
    assert.deepEqual(names, ['10.0.0.9', '192.168.0.2', '192.168.0.3']);
  });
});

test('settings: adding a Groq/xAI key never erases teacher.json when it cannot be read', { skip }, async (t) => {
  const teacher = { provider: 'anthropic', model: 'claude-test', key: 'sk-ant-test', keys: { anthropic: { key: 'sk-ant-test', model: 'claude-test' } } };
  const sim = await startSim({ seed: { '/data/anima/teacher.json': teacher } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const { STR } = await import('../../apps/settings/www/i18n.js');
  const TP = '/data/anima/teacher.json';
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/settings/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  await page.waitFor(`document.querySelectorAll('.nav-item').length > 4`, { timeout: 45000 });
  await page.eval(`document.querySelectorAll('.nav-item')[4].click(), true`);          // TABS: …, network, ai
  assert.ok(await page.waitFor(`!!document.querySelector('#caps-row button[data-fix="xai"]')`, { timeout: 45000 }), 'the "+ xAI" fix button is offered');
  await page.eval(`window.prompt = () => 'xai-test-key', true`);
  const click = `document.querySelector('#caps-row button[data-fix="xai"]').click(), true`;
  const status = `document.getElementById('status-text').textContent`;

  await t.test('503 on the read: refused, the Anthropic key survives, the user is told', async () => {
    const before = await sim.readSd(TP);
    await sim.control('/api/_sim/fault', FAULT_READ);
    await page.eval(click);
    assert.ok(await page.waitFor(`${status} === ${JSON.stringify(STR.en.teacherReadFailed)}`, { timeout: 10000 }), 'explained: ' + await page.eval(status));
    await sim.control('/api/_sim/fault', { clear: true });
    assert.equal(await sim.readSd(TP), before, 'teacher.json was overwritten with only the new key');
  });

  await t.test('corrupt teacher.json: refused and left untouched', async () => {
    await writeFile(`${sim.sd}${TP}`, '{"provider":"anthropic","key":"sk-ant-te');
    await page.eval(click);
    assert.ok(await page.waitFor(`${status} === ${JSON.stringify(STR.en.teacherReadCorrupt)}`, { timeout: 10000 }), 'explained: ' + await page.eval(status));
    assert.equal(await sim.readSd(TP), '{"provider":"anthropic","key":"sk-ant-te', 'a corrupt file was replaced');
  });

  await t.test('readable file: the key is ADDED next to the existing config', async () => {
    await writeFile(`${sim.sd}${TP}`, JSON.stringify(teacher));
    await page.eval(click);
    assert.ok(await page.waitFor(`(async () => { const j = await (await fetch('/api/fs/read?path=${encodeURIComponent(TP)}', { cache: 'no-store' })).json(); return !!(j.keys && j.keys.xai); })()`, { timeout: 10000 }), 'written');
    const saved = JSON.parse(await sim.readSd(TP));
    assert.equal(saved.key, 'sk-ant-test', 'the active key is kept');
    assert.equal(saved.keys.anthropic.key, 'sk-ant-test');
    assert.equal(saved.keys.xai.key, 'xai-test-key');
  });

  await t.test('no teacher.json yet (404): a fresh file is created', async () => {
    await rm(`${sim.sd}${TP}`);
    await page.eval(click);
    assert.ok(await page.waitFor(`(async () => (await fetch('/api/fs/read?path=${encodeURIComponent(TP)}', { cache: 'no-store' })).ok)()`, { timeout: 10000 }), 'written');
    assert.equal(JSON.parse(await sim.readSd(TP)).keys.xai.key, 'xai-test-key');
  });
});

test('radio: a failed read never replaces the station list with the built-in one', { skip }, async (t) => {
  const mine = { schema: 2, default: 'mine', stations: [{ id: 'mine', name: 'My Station', genre: 'Talk', stream: 'http://example.org/s.mp3', nowplaying: '' }] };
  const sim = await startSim({ seed: { '/system/config/radio.json': mine } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const it = cat('radio', 'it');
  const before = await sim.readSd('/system/config/radio.json');

  await sim.control('/api/_sim/fault', FAULT_READ);
  const page = await openApp(browser, sim, 'radio', 'it');

  await t.test('read fails: says so, and neither an add nor a star touches radio.json', async () => {
    assert.ok(await page.waitFor(`document.getElementById('save').textContent === ${JSON.stringify(it.read_failed)}`, { timeout: 10000 }), 'the failure is shown in Italian');
    await page.eval(`(() => { const b = document.querySelector('#list .star:not(.on)'); if (b) b.click(); return true; })()`);
    await page.eval(`(() => { document.getElementById('add').click();
      document.getElementById('fName').value = 'Nuova'; document.getElementById('fStream').value = 'http://example.org/n.mp3';
      document.getElementById('dlgSave').click(); return true; })()`);
    await sleep(1500);
    assert.equal(await sim.readSd('/system/config/radio.json'), before, 'radio.json was overwritten with the built-in list');
    assert.ok(await page.eval(`document.getElementById('scrim').classList.contains('on')`), 'the editor stays open: what was typed is kept');
  });

  await t.test('device back: the save re-reads and adds to the REAL list', async () => {
    await sim.control('/api/_sim/fault', { clear: true });
    await page.eval(`document.getElementById('dlgSave').click(), true`);
    assert.ok(await page.waitFor(`!document.getElementById('scrim').classList.contains('on')`, { timeout: 10000 }), 'saved and closed');
    await sleep(800);
    assert.deepEqual(JSON.parse(await sim.readSd('/system/config/radio.json')).stations.map((s) => s.name), ['My Station', 'Nuova']);
  });
});

test('miei-fatti: a failed read never overwrites the facts file', { skip }, async (t) => {
  const facts = { schema: 2, profile: { name: 'Nik' }, facts: [{ id: 1, kind: 'flat', cat: 'generic', subject: 'gatto', value: 'Micio', cop: 'è', status: 'ok', ts: 1 }], rels: [] };
  const sim = await startSim({ seed: { '/data/miei-fatti.json': facts } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('miei-fatti', 'en');
  const before = await sim.readSd('/data/miei-fatti.json');
  const teach = `(() => { document.getElementById('subj').value = 'cane'; document.getElementById('val').value = 'Fido'; document.getElementById('teach').click(); return true; })()`;

  await sim.control('/api/_sim/fault', FAULT_READ);
  const page = await openApp(browser, sim, 'miei-fatti', 'en');

  await t.test('read fails: teaching a fact does not overwrite the file, and says why', async () => {
    await page.eval(teach);
    assert.ok(await page.waitFor(`document.getElementById('addmsg').textContent === ${JSON.stringify(en.readFailed)}`, { timeout: 15000 }), 'explained: ' + await page.eval(`document.getElementById('addmsg').textContent`));
    await sleep(1200);
    assert.equal(await sim.readSd('/data/miei-fatti.json'), before, 'the facts file was overwritten');
    assert.equal(await page.eval(`document.getElementById('val').value`), 'Fido', 'what was typed is kept');
  });

  await t.test('device back: the fact is added next to the real ones', async () => {
    await sim.control('/api/_sim/fault', { clear: true });
    await page.eval(`document.getElementById('teach').click(), true`);
    assert.ok(await page.waitFor(`(async () => { const r = await fetch('/api/fs/read?path=%2Fdata%2Fmiei-fatti.json', { cache: 'no-store' }); const j = await r.json(); return j.facts.length === 2; })()`, { timeout: 20000 }), 'saved');
    const saved = JSON.parse(await sim.readSd('/data/miei-fatti.json'));
    assert.deepEqual(saved.facts.map((f) => f.subject).sort(), ['cane', 'gatto']);
    assert.equal(saved.profile.name, 'Nik', 'the profile survived');
  });
});

test('ir-remote: a failed read never overwrites /data/ir/userdata.json; the next good read merges', { skip }, async (t) => {
  const ud = { favs: [{ code: { protocol: 'nec', address: 1, command: 2 }, label: 'TV power' }], custom: [{ name: 'Lamp', protocol: 'nec', address: 3, command: 4 }], macros: [], recents: [] };
  const sim = await startSim({ seed: { '/data/ir/userdata.json': ud } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const before = await sim.readSd('/data/ir/userdata.json');
  const press = (cmd) => `(() => { document.querySelector('[data-tab="builder"]').click();
    document.getElementById('b_addr').value = '7'; document.getElementById('b_cmd').value = '${cmd}';
    document.getElementById('b_test').click(); return true; })()`;

  await sim.control('/api/_sim/fault', FAULT_READ);
  const page = await openApp(browser, sim, 'ir-remote', 'en');

  await t.test('read fails: pressing a button does not overwrite the SD copy', async () => {
    await page.eval(press(9));
    await sleep(2000);
    assert.equal(await sim.readSd('/data/ir/userdata.json'), before, 'userdata.json was overwritten with an empty set');
  });

  await t.test('device back: the next change re-reads the SD and keeps its favourites', async () => {
    await sim.control('/api/_sim/fault', { clear: true });
    await page.eval(press(10));
    assert.ok(await page.waitFor(`(async () => { const j = await (await fetch('/api/fs/read?path=%2Fdata%2Fir%2Fuserdata.json', { cache: 'no-store' })).json(); return (j.recents || []).length > 0; })()`, { timeout: 10000 }), 'written once readable');
    const saved = JSON.parse(await sim.readSd('/data/ir/userdata.json'));
    assert.deepEqual(saved.favs.map((f) => f.label), ['TV power'], 'favourites survived');
    assert.deepEqual(saved.custom.map((c) => c.name), ['Lamp'], 'custom buttons survived');
  });
});

// Browser E2E — apps must never lose the user's data on the SD. The pattern this guards against was
// found in several apps: a FAILED read (offline, 401, 503) was treated as "empty", and the next edit
// wrote that empty state over the real file. Also: records the device writes (ANIMA reminders) must be
// editable like any other.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const today = () => { const d = new Date(); return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`; };

// Open an app in its own page (standalone, same origin as the shell) — the app code path is the same.
async function openApp(browser, sim, id, lang = 'en') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/${id}/`);
  await page.networkIdle({ quiet: 500, timeout: 10000 });
  return page;
}

test('calendar: device-written reminders can be deleted, and a failed read never overwrites the SD', { skip }, async (t) => {
  const key = today();
  const cal = { schema: 1, events: { [key]: [{ time: '09:00', text: 'Call Marco (by ANIMA)' }, { id: 'u1', time: '10:00', text: 'Mine' }] } };
  const sim = await startSim({ seed: { '/system/config/calendar.json': cal } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openApp(browser, sim, 'calendar');

  await t.test('an id-less reminder written by the device is deletable', async () => {
    assert.ok(await page.waitFor(`document.querySelectorAll('#evlist .evrow').length === 2`, { timeout: 8000 }), 'both events listed');
    await page.eval(`[...document.querySelectorAll('#evlist .evrow')].find((r) => r.textContent.includes('by ANIMA')).querySelector('.del').click()`);
    assert.ok(await page.waitFor(`document.querySelectorAll('#evlist .evrow').length === 1`, { timeout: 8000 }), 'the ANIMA reminder was removed from the view');
    const saved = JSON.parse(await sim.readSd('/system/config/calendar.json'));
    assert.deepEqual(saved.events[key].map((e) => e.text), ['Mine'], 'and from the SD');
  });

  await t.test('a failed read pauses editing instead of overwriting the file', async () => {
    const before = await sim.readSd('/system/config/calendar.json');
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
    await page.eval(`(() => { document.getElementById('ev-text').value = 'Should not clobber'; document.getElementById('ev-text').dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true })); })()`);
    await sleep(1500);
    await sim.control('/api/_sim/fault', { clear: true });
    assert.equal(await sim.readSd('/system/config/calendar.json'), before, 'the calendar file was rewritten while it could not be read');
    assert.equal(await page.eval(`document.getElementById('ev-text').value`), 'Should not clobber', 'the typed text is kept for a retry');
  });

  await t.test('an event ANIMA adds on the device shows up live and is not erased by the next edit', async () => {
    // What the firmware does for "ricordami …": append {time,text} with no id + publish calendar.changed.
    const raw = JSON.parse(await sim.readSd('/system/config/calendar.json'));
    raw.events[key].push({ time: '18:00', text: 'Gym (by ANIMA)' });
    await (await import('node:fs/promises')).writeFile(`${sim.sd}/system/config/calendar.json`, JSON.stringify(raw));
    await page.eval(`document.getElementById('ev-text').value = 'Mine too'; document.getElementById('ev-text').dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))`);
    assert.ok(await page.waitFor(`document.getElementById('ev-text').value === ''`, { timeout: 8000 }), 'the add went through');
    const saved = JSON.parse(await sim.readSd('/system/config/calendar.json'));
    const texts = saved.events[key].map((e) => e.text);
    assert.ok(texts.includes('Gym (by ANIMA)'), 'the device-side reminder was erased by a stale write: ' + texts.join(', '));
    assert.ok(texts.includes('Mine too'));
  });
});

test('authenticator: a failed read never overwrites the 2FA vault; deletions elsewhere stay deleted', { skip }, async (t) => {
  const vault = { schema: 1, tokens: [{ id: 'tok-a', type: 'totp', issuer: 'GitHub', account: 'me', secret: 'JBSWY3DPEHPK3PXP', digits: 6, period: 30, algorithm: 'SHA1' }] };
  const sim = await startSim({ seed: { '/system/config/authenticator.json': vault } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });

  await t.test('a NEW browser whose first read fails cannot wipe the vault by adding an account', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
    const page = await openApp(browser, sim, 'authenticator', 'it');
    await page.eval(`(async () => {
      document.getElementById('add').click();
      document.getElementById('f-issuer').value = 'Nuovo'; document.getElementById('f-secret').value = 'KRSXG5CTMVRXEZLU';
      document.getElementById('save').click();
      await new Promise((r) => setTimeout(r, 1200));
    })()`);
    await sim.control('/api/_sim/fault', { clear: true });
    const saved = JSON.parse(await sim.readSd('/system/config/authenticator.json'));
    assert.deepEqual(saved.tokens.map((x) => x.issuer), ['GitHub'], 'the vault was overwritten while it could not be read');
    assert.ok(await page.eval(`document.getElementById('scrim').classList.contains('show')`), 'the add sheet stays open so nothing typed is lost');
  });

  await t.test('an account deleted on another browser does not come back from this browser\'s cache', async () => {
    const page = await openApp(browser, sim, 'authenticator', 'en');
    assert.ok(await page.waitFor(`document.querySelectorAll('#list > *').length === 1`, { timeout: 8000 }), 'GitHub listed');
    await (await import('node:fs/promises')).writeFile(`${sim.sd}/system/config/authenticator.json`, JSON.stringify({ schema: 1, tokens: [] }));
    const page2 = await openApp(browser, sim, 'authenticator', 'en');
    await page2.networkIdle({ quiet: 400, timeout: 8000 });
    assert.equal(await page2.eval(`document.querySelectorAll('#list > *').length`), 0, 'a deleted account resurrected from localStorage');
  });

  await t.test('HOTP and SHA-512 links are refused instead of producing wrong codes', async () => {
    const page = await openApp(browser, sim, 'authenticator', 'en');
    const msg = await page.eval(`(async () => {
      document.getElementById('add').click();
      document.getElementById('uri').value = 'otpauth://hotp/X:me?secret=JBSWY3DPEHPK3PXP&counter=3';
      document.getElementById('save').click();
      await new Promise((r) => setTimeout(r, 300));
      const a = document.getElementById('msg').textContent;
      document.getElementById('uri').value = 'otpauth://totp/Y:me?secret=JBSWY3DPEHPK3PXP&algorithm=SHA512';
      document.getElementById('save').click();
      await new Promise((r) => setTimeout(r, 300));
      return [a, document.getElementById('msg').textContent];
    })()`);
    assert.match(msg[0], /HOTP/);
    assert.match(msg[1], /SHA-512/);
    const saved = JSON.parse(await sim.readSd('/system/config/authenticator.json'));
    assert.equal(saved.tokens.length, 0, 'nothing unsupported was stored');
  });
});

test('tasks: a failed read never wipes the list; a refused write is retried, never reported as saved', { skip }, async (t) => {
  const list = [{ id: 't1', text: 'Buy milk', done: false, pri: 0, ts: 0 }, { id: 't2', text: 'Call Anna', done: false, pri: 1, ts: 1 }];
  const sim = await startSim({ seed: { '/data/tasks.todo': list } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const { readFileSync } = await import('node:fs');
  const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
  const it = { ...rd('../../web/shell/i18n/core.it.json'), ...rd('../../apps/tasks/www/i18n.it.json') };   // app catalog over the core one, like I18N
  const before = await sim.readSd('/data/tasks.todo');
  const enter = `document.getElementById('text').dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', bubbles: true }))`;

  await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
  const page = await openApp(browser, sim, 'tasks', 'it');

  await t.test('first read fails: says so (never "no tasks yet"), and an add does not touch the SD', async () => {
    assert.ok(await page.waitFor(`document.getElementById('save').textContent === ${JSON.stringify(it.read_failed)}`, { timeout: 8000 }), 'the read failure is explained in Italian');
    assert.ok(await page.eval(`document.getElementById('list').textContent.includes(${JSON.stringify(it.not_loaded)})`), 'the list says "not loaded", not "empty"');
    await page.eval(`document.getElementById('text').value = 'Nuova'; ${enter}`);
    await sleep(1200);
    assert.equal(await sim.readSd('/data/tasks.todo'), before, 'the task list on the SD was overwritten');
    assert.equal(await page.eval(`document.getElementById('text').value`), 'Nuova', 'the typed task is kept for a retry');
  });

  await t.test('device back: the real list loads and the add goes through, merged', async () => {
    await sim.control('/api/_sim/fault', { clear: true });
    await page.eval(enter);                                   // blocked once more, but it re-reads
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li .box').length === 2`, { timeout: 8000 }), 'the two real tasks are shown');
    await page.eval(enter);
    assert.ok(await page.waitFor(`document.getElementById('text').value === ''`, { timeout: 8000 }));
    await sleep(900);
    assert.deepEqual(JSON.parse(await sim.readSd('/data/tasks.todo')).map((x) => x.text), ['Buy milk', 'Call Anna', 'Nuova']);
  });

  await t.test('a refused write keeps the change, says "not saved", and lands once the device answers', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 503 });
    await page.eval(`document.querySelector('#list li .box').click()`);
    assert.ok(await page.waitFor(`document.getElementById('save').textContent === ${JSON.stringify(it.save_failed)}`, { timeout: 8000 }), 'no false "saved"');
    assert.ok(!JSON.parse(await sim.readSd('/data/tasks.todo')).some((x) => x.done), 'nothing written while refused');
    await sim.control('/api/_sim/fault', { clear: true });
    assert.ok(await page.waitFor(`document.getElementById('save').textContent === ${JSON.stringify(it.saved)} || document.getElementById('save').textContent === ''`, { timeout: 15000 }), 'the retry saved it');
    assert.equal(JSON.parse(await sim.readSd('/data/tasks.todo')).filter((x) => x.done).length, 1, 'the toggle reached the SD');
  });
});

test('contacts: a failed read never overwrites the address book; edits merge with changes made elsewhere', { skip }, async (t) => {
  const book = { schema: 1, contacts: [{ id: 'c1', name: 'Anna Rossi', email: 'anna@example.org', fav: false }, { id: 'c2', name: 'Marco Bianchi', fav: false }] };
  const sim = await startSim({ seed: { '/data/contacts.json': book } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const { readFileSync } = await import('node:fs');
  const de = JSON.parse(readFileSync(new URL('../../apps/contacts/www/i18n.de.json', import.meta.url), 'utf8'));
  const addContact = (name) => `(async () => { document.getElementById('add').click(); await new Promise((r) => setTimeout(r, 80));
    document.getElementById('f-name').value = ${JSON.stringify(name)}; document.getElementById('save').click(); return true; })()`;
  const before = await sim.readSd('/data/contacts.json');

  await t.test('first read fails: a note says so (in German) and a new contact does not overwrite the file', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
    const page = await openApp(browser, sim, 'contacts', 'de');
    assert.ok(await page.waitFor(`(document.getElementById('ro-note') || {}).textContent`, { timeout: 8000 }), 'a persistent note while the list is unknown');
    assert.ok((await page.eval(`document.getElementById('ro-note').textContent`)).includes(de.read_failed));
    await page.eval(addContact('Nicht speichern'));
    await sleep(1200);
    assert.equal(await sim.readSd('/data/contacts.json'), before, 'the address book on the SD was overwritten');
    assert.equal(await page.eval(`document.getElementById('scrim').classList.contains('show')`), true, 'the sheet stays open: what was typed is not lost');
    await sim.control('/api/_sim/fault', { clear: true });
  });

  await t.test('another browser adds a contact meanwhile: our add keeps it (read-modify-write)', async () => {
    const page = await openApp(browser, sim, 'contacts', 'en');
    assert.ok(await page.waitFor(`document.querySelectorAll('.row').length === 2`, { timeout: 8000 }));
    const fresh = JSON.parse(await sim.readSd('/data/contacts.json'));
    fresh.contacts.push({ id: 'c3', name: 'Added elsewhere', fav: false });
    await (await import('node:fs/promises')).writeFile(`${sim.sd}/data/contacts.json`, JSON.stringify(fresh));
    await page.eval(addContact('Mine'));
    assert.ok(await page.waitFor(`!document.getElementById('scrim').classList.contains('show')`, { timeout: 8000 }), 'saved and closed');
    const names = JSON.parse(await sim.readSd('/data/contacts.json')).contacts.map((c) => c.name);
    assert.deepEqual(names.sort(), ['Added elsewhere', 'Anna Rossi', 'Marco Bianchi', 'Mine'], 'nothing lost: ' + names.join(', '));
  });
});

test('qr: a failed read never overwrites the saved QR library; delete removes THAT item even if the list moved', { skip }, async (t) => {
  const lib = { schema: 1, items: [{ id: 'qa', label: 'Wi-Fi casa', type: 'text', data: 'WIFI:S:casa;;', ts: 1 }, { id: 'qb', label: 'Sito', type: 'link', data: 'https://example.org', ts: 2 }] };
  const sim = await startSim({ seed: { '/data/QR/qrcodes.json': lib } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const before = await sim.readSd('/data/QR/qrcodes.json');

  await t.test('first read fails: the list says so and "save to device" writes nothing', async () => {
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
    const page = await openApp(browser, sim, 'qr', 'es');
    const es = JSON.parse((await import('node:fs')).readFileSync(new URL('../../apps/qr/www/i18n.es.json', import.meta.url), 'utf8'));
    assert.ok(await page.waitFor(`document.getElementById('savedList').textContent.includes(${JSON.stringify(es.read_failed)})`, { timeout: 8000 }), 'explained in Spanish, not "no QR saved"');
    await page.eval(`(() => { const i = document.querySelector('#form [data-k]'); i.value = 'hola'; i.dispatchEvent(new Event('input', { bubbles: true })); return true; })()`);
    await sleep(400);
    await page.eval(`document.getElementById('save').click()`);
    await sleep(1200);
    assert.equal(await sim.readSd('/data/QR/qrcodes.json'), before, 'the QR library on the SD was overwritten');
    await sim.control('/api/_sim/fault', { clear: true });
  });

  await t.test('the native app added a QR meanwhile: deleting "Sito" removes Sito, not the item now at its position', async () => {
    const page = await openApp(browser, sim, 'qr', 'en');
    assert.ok(await page.waitFor(`document.querySelectorAll('#savedList [data-del]').length === 2`, { timeout: 8000 }));
    const fresh = JSON.parse(await sim.readSd('/data/QR/qrcodes.json'));
    fresh.items.unshift({ id: 'qn', label: 'From the Cardputer', type: 'text', data: 'native', ts: 3 });
    await (await import('node:fs/promises')).writeFile(`${sim.sd}/data/QR/qrcodes.json`, JSON.stringify(fresh));
    await page.eval(`[...document.querySelectorAll('#savedList .item')].find((r) => r.textContent.includes('Sito')).querySelector('[data-del]').click()`);
    await sleep(1200);
    const labels = JSON.parse(await sim.readSd('/data/QR/qrcodes.json')).items.map((i) => i.label);
    assert.deepEqual(labels, ['From the Cardputer', 'Wi-Fi casa'], 'wrong item deleted or the native one lost: ' + labels.join(', '));
  });
});

test('settings: a failed read never rewrites settings.json with the defaults', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  // a real user file: a custom device name the defaults do not have
  const real = JSON.parse(await sim.readSd('/system/config/settings.json'));
  real.device = { ...(real.device || {}), name: 'cardputer-of-anna' };
  await (await import('node:fs/promises')).writeFile(`${sim.sd}/system/config/settings.json`, JSON.stringify(real));
  const { STR } = await import('../../apps/settings/www/i18n.js');

  await t.test('read fails at open: the page says so and "Save" writes nothing', async () => {
    const page = await browser.newPage();
    await bootShell(page, sim, { lang: 'en', wait: false });     // (its /api/lang persists ui.language, like the firmware)
    const before = await sim.readSd('/system/config/settings.json');
    await sim.control('/api/_sim/fault', { route: '/api/fs/read', status: 503 });
    try {
      await page.goto(`${sim.origin}/apps/settings/`);
      assert.ok(await page.waitFor(`document.getElementById('status-text').textContent === ${JSON.stringify(STR.en.settingsReadFailed)}`, { timeout: 10000 }), 'the read failure is shown');
      await page.eval(`(() => { const b = document.getElementById('pw-vol'); b.value = '13'; b.dispatchEvent(new Event('input', { bubbles: true })); document.getElementById('btn-save').click(); return true; })()`);
      await sleep(1500);
      assert.equal(await sim.readSd('/system/config/settings.json'), before, 'settings.json was rewritten while it could not be read');
    } finally { await sim.control('/api/_sim/fault', { clear: true }); }
  });

  await t.test('device back: the real values load, and a save keeps the fields it did not touch', async () => {
    const page = await openApp(browser, sim, 'settings', 'en');
    assert.ok(await page.waitFor(`document.getElementById('dev-name').value === 'cardputer-of-anna'`, { timeout: 10000 }), 'the real device name is shown');
    await page.eval(`(() => { const b = document.getElementById('pw-vol'); b.value = '60'; b.dispatchEvent(new Event('input', { bubbles: true })); document.getElementById('btn-save').click(); return true; })()`);
    assert.ok(await page.waitFor(`document.getElementById('status-text').textContent === ${JSON.stringify(STR.en.saved)}`, { timeout: 10000 }), 'saved');
    const saved = JSON.parse(await sim.readSd('/system/config/settings.json'));
    assert.equal(saved.power.volume, 60);
    assert.equal(saved.device.name, 'cardputer-of-anna', 'untouched fields survive');
  });
});

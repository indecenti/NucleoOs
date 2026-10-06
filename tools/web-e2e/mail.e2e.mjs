// Browser E2E — Mail against the simulator's /api/mail/{presets,accounts,send} (mirror of nucleo_httpd.c +
// nucleo_mailcfg.c). What a user must be able to trust:
//   • Compose refuses an invalid recipient / empty message before anything is sent;
//   • a send POSTs exactly {account, to, subject, body} and says "sent" ONLY on {ok:true} — a refusal, a
//     busy device, a dropped connection or an unpaired browser keep the draft and say why;
//   • the account form validates before saving, the SMTP password is write-only (never echoed back into the
//     page) and an edit with a blank password keeps the stored one;
//   • a save/delete the device does not accept never makes the accounts vanish from the screen;
//   • the Sent log (device-written, carries recipient/subject text) renders as text, newest first.
// Delete-asks-first is covered by app-ux-safety.e2e.mjs.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const cat = (app, lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/${app}/www/i18n.${lang}.json`) });
const fmt = (s, a) => String(s).replace(/\{(\w+)\}/g, (m, k) => (k in a ? String(a[k]) : m));

// Record every POST body the app sends to /api/mail/* (requests still reach the simulator).
const RECORD = `(() => {
  if (!location.pathname.startsWith('/apps/mail')) return;
  window.__posts = [];
  const real = window.fetch.bind(window);
  window.fetch = (input, opts = {}) => {
    const p = new URL(typeof input === 'string' ? input : input.url, location.href).pathname;
    if (p.startsWith('/api/mail/') && (opts.method || 'GET').toUpperCase() === 'POST') { try { window.__posts.push({ path: p, body: JSON.parse(opts.body) }); } catch {} }
    return real(input, opts);
  };
})()`;
const ACCOUNTS = [
  { name: 'Work', host: 'smtp.work.example', port: 465, user: 'me@work.example', pass: 'work-secret-1' },
  { name: 'Home', host: 'smtp.home.example', port: 587, user: 'me@home.example', pass: 'home-secret-2' },
];

async function openApp(browser, sim, { lang = 'en', accounts = [] } = {}) {
  const page = await browser.newPage();
  await page.initScript(RECORD);
  await bootShell(page, sim, { lang, wait: false });
  for (const a of accounts) {
    await page.eval(`fetch('/api/mail/accounts', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: ${JSON.stringify(JSON.stringify(a))} }).then((r) => r.status)`);
  }
  await page.goto(`${sim.origin}/apps/mail/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  return page;
}
const status = (page) => page.eval(`(() => { const s = document.getElementById('st'); return s ? { cls: s.className, text: s.textContent } : null; })()`);
const posts = (page, path) => page.eval(`window.__posts.filter((p) => p.path === ${JSON.stringify(path)}).map((p) => p.body)`);
const compose = (page, { to, subject = '', body = '' }) => page.eval(`(() => {
  const set = (id, v) => { const el = document.getElementById(id); el.value = v; el.dispatchEvent(new Event('input')); };
  set('to', ${JSON.stringify(to)}); set('subj', ${JSON.stringify(subject)}); set('body', ${JSON.stringify(body)});
  document.getElementById('send').click(); return true; })()`);
const deviceAccounts = (page) => page.eval(`fetch('/api/mail/accounts', { cache: 'no-store' }).then((r) => r.json())`);

test('mail: Compose refuses an invalid recipient or an empty message before sending anything', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('mail', 'en');
  const page = await openApp(browser, sim, { accounts: ACCOUNTS.slice(0, 1) });
  assert.ok(await page.waitFor(`!!document.getElementById('send')`, { timeout: 30000 }), 'Compose is ready');
  for (const to of ['', 'bob', 'bob@', 'bob@host', '@host.it', 'a b@x.it', 'bob@@x.it']) {
    await compose(page, { to, body: 'hello' });
    await sleep(150);
    assert.deepEqual(await status(page), { cls: 'status err', text: en.invalid_email }, JSON.stringify(to));
    assert.ok(await page.eval(`document.getElementById('to').classList.contains('bad')`), 'the field is flagged: ' + to);
  }
  for (const body of ['', '   \n  ']) {
    await compose(page, { to: 'anna@example.org', body });
    await sleep(150);
    assert.deepEqual(await status(page), { cls: 'status err', text: en.missing_fields }, 'empty body ' + JSON.stringify(body));
  }
  assert.deepEqual(await posts(page, '/api/mail/send'), [], 'nothing was sent');
});

test('mail: a send POSTs {account, to, subject, body}; "sent" only on ok:true, then the draft is cleared', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const it = cat('mail', 'it');
  const page = await openApp(browser, sim, { lang: 'it', accounts: ACCOUNTS });
  assert.ok(await page.waitFor(`document.querySelectorAll('#acc option').length === 2`, { timeout: 30000 }), 'both accounts offered as sender');
  await page.eval(`(() => { const s = document.getElementById('acc'); s.value = '1'; s.dispatchEvent(new Event('change')); return true; })()`);
  await compose(page, { to: '  anna@example.org ', subject: 'Ciao', body: 'Riga 1\nRiga 2' });
  assert.ok(await page.waitFor(`document.getElementById('st') && document.getElementById('st').className === 'status ok'`, { timeout: 15000 }));
  assert.equal((await status(page)).text, it.sent_ok);
  assert.deepEqual(await posts(page, '/api/mail/send'), [{ account: 1, to: 'anna@example.org', subject: 'Ciao', body: 'Riga 1\nRiga 2' }],
    'exactly one request: the chosen account, the trimmed recipient, subject and body verbatim');
  assert.ok(await page.waitFor(`document.getElementById('to') && document.getElementById('to').value === '' && document.getElementById('body').value === '' && document.getElementById('subj').value === ''`, { timeout: 5000 }),
    'the sent draft is cleared');
});

test('mail: a send the device does not confirm is never shown as sent; the draft is kept', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('mail', 'en');
  const page = await openApp(browser, sim, { accounts: ACCOUNTS.slice(0, 1) });
  assert.ok(await page.waitFor(`!!document.getElementById('send')`, { timeout: 30000 }));
  const cases = [
    // [what, fault or null, recipient, expected status text (or RegExp)]
    ['the device refuses it (ok:false + reason)', null, 'anna<b>@example.org', fmt(en.send_fail, { err: 'invalid recipient address' })],
    ['the device is busy (503)', { route: '/api/mail/send', status: 503 }, 'anna@example.org', fmt(en.send_fail, { err: 'sim fault' })],
    ['the connection drops', { route: '/api/mail/send', drop: true }, 'anna@example.org', /^Send failed: .+/],
    ['the browser is not paired (401)', { route: '/api/mail/send', status: 401 }, 'anna@example.org', en.need_pair],
  ];
  for (const [what, fault, to, want] of cases) {
    await t.test(what, async () => {
      await sim.control('/api/_sim/fault', { clear: true });
      if (fault) await sim.control('/api/_sim/fault', fault);
      await compose(page, { to, subject: 'Report', body: 'Numbers attached' });
      assert.ok(await page.waitFor(`document.getElementById('st').className === 'status err'`, { timeout: 15000 }), 'an error is shown');
      await sleep(1300);                                    // past the success path's draft-clearing re-render
      const s = await status(page);
      assert.equal(s.cls, 'status err');
      if (want instanceof RegExp) assert.match(s.text, want); else assert.equal(s.text, want);
      assert.notEqual(s.text, en.sent_ok);
      assert.deepEqual(await page.eval(`[document.getElementById('to').value, document.getElementById('subj').value, document.getElementById('body').value, document.getElementById('send').disabled]`),
        [to, 'Report', 'Numbers attached', false], 'the draft is intact and Send is usable again');
    });
  }
  await sim.control('/api/_sim/fault', { clear: true });
});

test('mail: the account form validates first; the password is write-only; a blank password keeps the stored one', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('mail', 'en');
  const page = await openApp(browser, sim);
  await page.eval(`document.getElementById('tabA').click(), true`);
  assert.ok(await page.waitFor(`!!document.getElementById('add')`, { timeout: 30000 }));
  await page.eval(`document.getElementById('add').click(), true`);
  assert.ok(await page.waitFor(`!!document.getElementById('save')`, { timeout: 5000 }));
  const PASS = 'abcd efgh ijkl mnop';
  const fill = (user, pass) => page.eval(`(() => { document.getElementById('user').value = ${JSON.stringify(user)};
    document.getElementById('pass').value = ${JSON.stringify(pass)}; document.getElementById('save').click(); return true; })()`);

  await fill('not-an-email', PASS); await sleep(200);
  assert.deepEqual(await status(page), { cls: 'status err', text: en.invalid_email });
  await fill('me@gmail.com', ''); await sleep(200);
  assert.deepEqual(await status(page), { cls: 'status err', text: en.err_password_required }, 'a NEW account needs its password');
  assert.deepEqual(await posts(page, '/api/mail/accounts'), [], 'nothing invalid was saved');

  await fill('me@gmail.com', PASS);
  assert.ok(await page.waitFor(`document.querySelectorAll('.acct').length === 1`, { timeout: 15000 }), 'the account is listed');
  const [sent] = await posts(page, '/api/mail/accounts');
  assert.deepEqual(sent, { idx: -1, name: 'Gmail', host: 'smtp.gmail.com', port: 465, tls: 0, user: 'me@gmail.com', from_name: '', default: true, pass: PASS },
    'the first preset (Gmail), its host locked, the password sent once, the first account becomes default');
  assert.equal(await page.eval(`document.documentElement.outerHTML.includes(${JSON.stringify(PASS)})`), false, 'the password is not in the page');
  assert.ok(await page.eval(`document.querySelector('.acct .sub').textContent.includes(${JSON.stringify(en.secured)})`), 'shown as "stored"');
  const dev = await deviceAccounts(page);
  assert.equal(JSON.stringify(dev).includes(PASS), false, 'the device never returns the password');

  // Edit: rename, leave the password blank -> no "pass" key, the stored password stays.
  await page.eval(`document.querySelector('[data-edit]').click(), true`);
  assert.ok(await page.waitFor(`!!document.getElementById('nm') && document.getElementById('pass').value === ''`, { timeout: 5000 }), 'the edit form never pre-fills the password');
  await page.eval(`(() => { document.getElementById('nm').value = 'Personal'; document.getElementById('save').click(); return true; })()`);
  assert.ok(await page.waitFor(`document.querySelector('.acct .nm') && document.querySelector('.acct .nm').textContent === 'Personal'`, { timeout: 15000 }));
  const edit = (await posts(page, '/api/mail/accounts'))[1];
  assert.equal(edit.idx, 0); assert.equal(edit.name, 'Personal');
  assert.equal('pass' in edit, false, 'a blank password is not sent (it would erase the stored one)');
  assert.equal((await deviceAccounts(page)).accounts[0].has_pass, true, 'the stored password survived the edit');
});

test('mail: a save or delete the device does not accept never makes the accounts vanish', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('mail', 'en');
  const page = await openApp(browser, sim, { accounts: ACCOUNTS });
  await page.eval(`document.getElementById('tabA').click(), true`);
  assert.ok(await page.waitFor(`document.querySelectorAll('.acct').length === 2`, { timeout: 30000 }));

  await t.test('save refused (503): the form stays open with what was typed, the reason is shown', async () => {
    await page.eval(`document.querySelector('[data-edit="1"]').click(), true`);
    assert.ok(await page.waitFor(`!!document.getElementById('nm')`, { timeout: 5000 }));
    await sim.control('/api/_sim/fault', { route: '/api/mail/accounts', status: 503, times: 1 });
    await page.eval(`(() => { document.getElementById('nm').value = 'Home (new)'; document.getElementById('save').click(); return true; })()`);
    assert.ok(await page.waitFor(`document.getElementById('st') && document.getElementById('st').className === 'status err'`, { timeout: 15000 }), 'an error is shown');
    assert.equal(await page.eval(`document.getElementById('nm') && document.getElementById('nm').value`), 'Home (new)', 'still editing, nothing typed was lost');
    assert.equal((await status(page)).text, fmt(en.device_failed || '(no device_failed string)', { err: 'sim fault' }));
    await page.eval(`document.getElementById('cancel').click(), true`);
    assert.equal(await page.eval(`document.querySelectorAll('.acct').length`), 2, 'both accounts are still listed');
    assert.deepEqual((await deviceAccounts(page)).accounts.map((a) => a.name), ['Work', 'Home'], 'and unchanged on the device');
  });

  await t.test('delete refused (503): both accounts stay listed, the reason is shown', async () => {
    await page.goto(`${sim.origin}/apps/mail/`);                      // a fresh Accounts view
    // (click until the module has wired the tabs and loaded the accounts)
    assert.ok(await page.waitFor(`(document.getElementById('tabA').click(), document.querySelectorAll('.acct').length === 2)`, { timeout: 30000, interval: 300 }));
    await sim.control('/api/_sim/fault', { route: '/api/mail/accounts', status: 503, times: 1 });
    await page.eval(`(() => { window.confirm = () => true; document.querySelector('[data-del="0"]').click(); return true; })()`);
    assert.ok(await page.waitFor(`document.getElementById('st') && document.getElementById('st').className === 'status err'`, { timeout: 15000 }), 'an error is shown');
    assert.equal(await page.eval(`document.querySelectorAll('.acct').length`), 2, 'both accounts are still listed');
    assert.equal((await status(page)).text, fmt(en.device_failed || '(no device_failed string)', { err: 'sim fault' }));
    assert.equal((await deviceAccounts(page)).accounts.length, 2, 'nothing was deleted on the device');
  });
});

test('mail: the Sent log renders newest first, OK/failed marked, recipient and subject as text', { skip }, async (t) => {
  const evil = '<img src=x onerror="window.__pwned=1">';
  const log = '2026-10-01 09:00 | OK | anna@example.org | Hello\n' +
    `2026-10-02 10:30 | ERR | ${evil} | <b>Invoice</b> &amp; more\n` +
    '2026-10-03 11:45 | OK | marco@example.org | (no subj)\n';
  const sim = await startSim({ seed: { '/system/mail/sent.log': log } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openApp(browser, sim);
  assert.ok(await page.waitFor(`!!document.querySelector('#view .bar, #view #send')`, { timeout: 30000 }), 'the app has loaded');
  await page.eval(`document.getElementById('tabS').click(), true`);
  assert.ok(await page.waitFor(`document.querySelectorAll('.sent li').length === 3`, { timeout: 15000 }));
  const items = await page.eval(`[...document.querySelectorAll('.sent li')].map((li) => ({ to: li.querySelector('.to').textContent,
    subj: li.querySelector('.subj').textContent, when: li.querySelector('.when').textContent, ok: li.querySelector('.dot').classList.contains('ok') }))`);
  assert.deepEqual(items, [
    { to: 'marco@example.org', subj: '(no subj)', when: '2026-10-03 11:45', ok: true },
    { to: evil, subj: '<b>Invoice</b> &amp; more', when: '2026-10-02 10:30', ok: false },
    { to: 'anna@example.org', subj: 'Hello', when: '2026-10-01 09:00', ok: true },
  ]);
  assert.deepEqual(await page.eval(`({ injected: document.querySelectorAll('.sent img, .sent .subj b').length, pwned: !!window.__pwned })`), { injected: 0, pwned: false });
});

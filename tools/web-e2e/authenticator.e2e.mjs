// Browser E2E — Authenticator behaviour against the simulator: the codes ON SCREEN are the right codes
// (RFC 6238 vectors through the real UI wiring of digits / period / algorithm, with a frozen clock), what
// the add sheet stores on the SD is exactly what the user chose, invalid input is refused without touching
// the vault, delete asks first, copy hands over the exact code, and names are text. The pure OTP core is
// pinned by tools/authenticator-*.test.mjs; failed-read vault safety + HOTP/SHA-512 refusal by
// data-safety.e2e.mjs — not repeated here.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createHmac } from 'node:crypto';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const cat = (app, lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/${app}/www/i18n.${lang}.json`) });
const fmt = (s, a) => s.replace(/\{(\w+)\}/g, (m, k) => (k in a ? String(a[k]) : m));
const VAULT = '/system/config/authenticator.json';

function b32(buf) {
  const A = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'; let bits = 0, val = 0, out = '';
  for (const b of buf) { val = ((val << 8) | b) & 0xffff; bits += 8; while (bits >= 5) { out += A[(val >>> (bits - 5)) & 31]; bits -= 5; } }
  if (bits) out += A[(val << (5 - bits)) & 31];
  return out;
}
function refTotp(secretBuf, tSec, { digits = 6, period = 30, algorithm = 'sha1' } = {}) {
  const c = Buffer.alloc(8); c.writeBigUInt64BE(BigInt(Math.floor(tSec / period)));
  const h = createHmac(algorithm, secretBuf).update(c).digest();
  const o = h[h.length - 1] & 0x0f;
  const bin = ((h[o] & 0x7f) << 24) | (h[o + 1] << 16) | (h[o + 2] << 8) | h[o + 3];
  return String(bin % 10 ** digits).padStart(digits, '0');
}
const grouped = (c) => c.length === 6 ? c.slice(0, 3) + ' ' + c.slice(3) : c.slice(0, 4) + ' ' + c.slice(4);

const SEED20 = Buffer.from('12345678901234567890');
const SEED32 = Buffer.from('12345678901234567890123456789012');
const T = 1111111109;                                    // an RFC 6238 App. B instant
const tokens = () => [
  { id: 'tok-1', type: 'totp', issuer: 'RFC-SHA1', account: 'eight', secret: b32(SEED20), digits: 8, period: 30, algorithm: 'SHA1' },
  { id: 'tok-2', type: 'totp', issuer: 'RFC-SHA256', account: 'six', secret: b32(SEED32), digits: 6, period: 30, algorithm: 'SHA256' },
  { id: 'tok-3', type: 'totp', issuer: 'Minute', account: 'p60', secret: b32(SEED20), digits: 6, period: 60, algorithm: 'SHA1' },
  { id: 'tok-4', type: 'totp', issuer: 'Legacy', account: 'no-fields', secret: b32(SEED20).toLowerCase() },   // an old entry: defaults
  { id: 'tok-5', type: 'totp', issuer: 'Strong', account: 'sha512', secret: b32(SEED20), digits: 6, period: 30, algorithm: 'SHA512' },
];

async function openApp(browser, sim, lang = 'en') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/authenticator/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  return page;
}
// Freeze the page clock at `sec` and repaint (search input re-renders every row; codes are computed then).
const freeze = (page, sec) => page.eval(`(() => { const T = ${sec * 1000}; Date.now = () => T;
  const s = document.getElementById('search'); s.value = ''; s.dispatchEvent(new Event('input')); return true; })()`);
const codes = (page) => page.eval(`Object.fromEntries([...document.querySelectorAll('#list .tok')].map((el) =>
  [el.querySelector('.iss').textContent, el.querySelector('.code').textContent]))`);
const writes = async (sim) => (await sim.control('/api/_sim/stats')).byPath['/api/fs/write'] || 0;

test('authenticator: the codes on screen are the RFC 6238 codes for each account\'s digits, period and algorithm', { skip }, async (t) => {
  const sim = await startSim({ seed: { [VAULT]: { schema: 1, tokens: tokens() } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openApp(browser, sim);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list .tok').length === 5`, { timeout: 30000 }), 'every vault account is listed');
  await freeze(page, T);
  const c = await codes(page);
  assert.equal(c['RFC-SHA1'], '0708 1804', 'RFC 6238 SHA-1 8 digits (leading zero kept)');
  assert.equal(c['RFC-SHA256'], '084 774', 'RFC 6238 SHA-256, 6-digit truncation of 68084774');
  assert.equal(c.Minute, grouped(refTotp(SEED20, T, { period: 60 })), 'a 60 s account uses its own window, not 30 s');
  assert.equal(c.Legacy, grouped(refTotp(SEED20, T)), 'an entry without digits/period/algorithm = 6 digits, 30 s, SHA-1');
  assert.doesNotMatch(c.Strong, /\d/, 'an algorithm the app cannot compute shows dashes, never a wrong (SHA-1) code: ' + c.Strong);

  await t.test('the codes roll over exactly at the window boundary, per account period', async () => {
    const w30 = Math.ceil(T / 30) * 30;                                   // next 30 s boundary after T
    await freeze(page, w30 - 1);
    const before = await codes(page);
    await freeze(page, w30);
    const after = await codes(page);
    assert.equal(after['RFC-SHA1'], grouped(refTotp(SEED20, w30, { digits: 8 })));
    assert.notEqual(after['RFC-SHA1'], before['RFC-SHA1'], 'the 30 s code changed at the boundary');
    if (w30 % 60 !== 0) assert.equal(after.Minute, before.Minute, 'the 60 s code did not change at a mid-minute 30 s boundary');
  });

  await t.test('clicking an account copies exactly the code shown (no grouping space)', async () => {
    await freeze(page, T);
    const sent = await page.eval(`(async () => {
      const got = []; addEventListener('message', (e) => { if (e.data && e.data.type === 'clipboard-write') got.push(e.data.data); });
      [...document.querySelectorAll('#list .tok')].find((el) => el.querySelector('.iss').textContent === 'RFC-SHA1').click();
      await new Promise((r) => setTimeout(r, 300));
      return got;
    })()`);
    assert.deepEqual(sent, ['07081804']);
  });
});

test('authenticator: the add sheet stores exactly what the user chose; invalid input never touches the vault', { skip }, async (t) => {
  const seed = { schema: 1, tokens: tokens().slice(0, 1) };
  const sim = await startSim({ seed: { [VAULT]: seed } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const en = cat('authenticator', 'en');
  const page = await openApp(browser, sim);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list .tok').length === 1`, { timeout: 30000 }));

  await t.test('an invalid secret is refused with the reason and nothing is written', async () => {
    await sim.control('/api/_sim/stats', { reset: true });
    const msg = await page.eval(`(async () => {
      document.getElementById('add').click();
      document.getElementById('f-issuer').value = 'Bad'; document.getElementById('f-secret').value = 'JBSWY3DP0189';
      document.getElementById('save').click();
      await new Promise((r) => setTimeout(r, 600));
      return document.getElementById('msg').textContent;
    })()`);
    assert.equal(msg, en.err_secret);
    assert.equal(await writes(sim), 0, 'no write for an invalid secret');
    assert.deepEqual(JSON.parse(await sim.readSd(VAULT)), seed);
    assert.ok(await page.eval(`document.getElementById('scrim').classList.contains('show')`), 'the sheet stays open for a fix');
  });

  await t.test('a grouped lowercase secret with advanced options is saved normalized, with digits/period/algorithm', async () => {
    await page.eval(`(() => {
      document.getElementById('cancel').click(); document.getElementById('add').click();
      document.getElementById('f-issuer').value = '  Bank  '; document.getElementById('f-account').value = 'me@bank.example';
      document.getElementById('f-secret').value = 'gezd gnbv gy3t qojq gezd gnbv gy3t qojq';
      document.getElementById('adv-toggle').click();
      document.getElementById('f-digits').value = '8'; document.getElementById('f-period').value = '60'; document.getElementById('f-algo').value = 'SHA256';
      document.getElementById('save').click(); return true; })()`);
    assert.ok(await page.waitFor(`!document.getElementById('scrim').classList.contains('show')`, { timeout: 15000 }), 'the sheet closes on success');
    const saved = JSON.parse(await sim.readSd(VAULT)).tokens;
    assert.equal(saved.length, 2, 'the existing account is kept');
    assert.deepEqual(saved[0], seed.tokens[0]);
    const n = saved[1];
    assert.deepEqual({ type: n.type, issuer: n.issuer, account: n.account, secret: n.secret, digits: n.digits, period: n.period, algorithm: n.algorithm },
      { type: 'totp', issuer: 'Bank', account: 'me@bank.example', secret: 'gezdgnbvgy3tqojqgezdgnbvgy3tqojq', digits: 8, period: 60, algorithm: 'SHA256' });
    await freeze(page, T);
    assert.equal((await codes(page)).Bank, grouped(refTotp(SEED20, T, { digits: 8, period: 60, algorithm: 'sha256' })), 'and the code shown uses those options');
  });

  await t.test('pasting an otpauth:// link fills every field (issuer, account, secret, digits, period, algorithm)', async () => {
    const f = await page.eval(`(() => {
      document.getElementById('add').click();
      const u = document.getElementById('uri');
      u.value = 'otpauth://totp/ACME%20Co:alice%40acme.example?secret=JBSWY3DPEHPK3PXP&digits=8&period=45&algorithm=SHA256';
      u.dispatchEvent(new Event('input'));
      const v = (id) => document.getElementById(id).value;
      return [v('f-issuer'), v('f-account'), v('f-secret'), v('f-digits'), v('f-period'), v('f-algo'), document.getElementById('msg').textContent];
    })()`);
    assert.deepEqual(f, ['ACME Co', 'alice@acme.example', 'JBSWY3DPEHPK3PXP', '8', '45', 'SHA256', '']);
    await page.eval(`document.getElementById('cancel').click(), true`);
  });
});

test('authenticator: deleting an account asks first (localized, named); declining keeps the vault untouched', { skip }, async (t) => {
  const seed = { schema: 1, tokens: tokens().slice(0, 3) };
  const sim = await startSim({ seed: { [VAULT]: seed } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const it = cat('authenticator', 'it');
  const page = await openApp(browser, sim, 'it');
  assert.ok(await page.waitFor(`document.querySelectorAll('#list .tok').length === 3`, { timeout: 30000 }));
  const del = (answer) => page.eval(`(async () => {
    window.__asked = []; window.confirm = (m) => { window.__asked.push(String(m)); return ${answer}; };
    document.querySelector('#list .tok[data-id="tok-2"] .del').click();
    await new Promise((r) => setTimeout(r, 1200));
    return window.__asked;
  })()`);
  await sim.control('/api/_sim/stats', { reset: true });
  assert.deepEqual(await del(false), [fmt(it.confirm_del, { name: 'RFC-SHA256' })], 'asked once, in Italian, naming the account');
  assert.equal(await writes(sim), 0, 'declined: no write');
  assert.deepEqual(JSON.parse(await sim.readSd(VAULT)), seed);
  assert.equal(await page.eval(`document.querySelectorAll('#list .tok').length`), 3);

  await del(true);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list .tok').length === 2`, { timeout: 10000 }));
  assert.deepEqual(JSON.parse(await sim.readSd(VAULT)).tokens.map((x) => x.id), ['tok-1', 'tok-3'], 'only the confirmed account is gone');
});

test('authenticator: issuer/account names are text, never markup; search filters by them', { skip }, async (t) => {
  const evil = '<img src=x onerror="window.__pwned=1">';
  const seed = { schema: 1, tokens: [
    { id: 'tok-x', type: 'totp', issuer: evil, account: '<b>bold</b>', secret: 'JBSWY3DPEHPK3PXP', digits: 6, period: 30, algorithm: 'SHA1' },
    { id: 'tok-y', type: 'totp', issuer: 'GitHub', account: 'octo', secret: 'JBSWY3DPEHPK3PXP', digits: 6, period: 30, algorithm: 'SHA1' }] };
  const sim = await startSim({ seed: { [VAULT]: seed } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await openApp(browser, sim);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list .tok').length === 2`, { timeout: 30000 }));
  await sleep(300);
  const r = await page.eval(`(() => { const el = document.querySelector('#list .tok[data-id="tok-x"]');
    return { iss: el.querySelector('.iss').textContent, acc: el.querySelector('.acc').textContent,
      imgs: document.querySelectorAll('#list img, #list b').length, pwned: !!window.__pwned }; })()`);
  assert.deepEqual(r, { iss: evil, acc: '<b>bold</b>', imgs: 0, pwned: false });
  const shown = await page.eval(`(() => { const s = document.getElementById('search'); s.value = 'octo'; s.dispatchEvent(new Event('input'));
    return [...document.querySelectorAll('#list .tok')].map((el) => el.dataset.id); })()`);
  assert.deepEqual(shown, ['tok-y'], 'search matches the account name');
});

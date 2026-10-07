// Browser E2E — Help (System Manual) against the simulator's real manual registry: every topic opens and
// shows exactly what its .info record says, in all five languages — placeholders such as "cat <file>" are
// TEXT, not swallowed as unknown HTML tags; search finds topics by their title in each language and
// highlights every match; "Run in Terminal" asks the shell for the Terminal with that command; ?path= opens a
// topic; the API "Send Request" shows the device's answer as text.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell, LANGS } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const cat = (lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/help/www/i18n.${lang}.json`) });
// The manuals the simulator serves (its SD is a copy of tools/sd-sim).
const MAN_DIR = new URL('../../tools/sd-sim/system/registry/manual/', import.meta.url);
const MANUALS = readdirSync(MAN_DIR).filter((f) => f.endsWith('.info')).map((f) => JSON.parse(readFileSync(new URL(f, MAN_DIR), 'utf8')));
const SPEC = rd('../../tools/sd-sim/system/registry/web-api-spec.json');
const byId = Object.fromEntries(MANUALS.map((m) => [m.id, m]));

async function open(browser, sim, lang, query = '') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/help/${query}`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list-terminal .sidebar-item').length >= 50 && document.querySelectorAll('#list-api .sidebar-item').length >= 50`, { timeout: 45000 }), `help (${lang}) loaded its topics`);
  return page;
}
// Open every topic in the sidebar and read back what the card shows.
const READ_ALL = `(() => [...document.querySelectorAll('.sidebar-item')].map((li) => {
  li.click();
  const c = document.getElementById('content'), q = (s) => { const e = c.querySelector(s); return e ? e.textContent : null; };
  return { key: li.textContent.trim(), h2: q('h2'), syn: q('.synopsis-box code'), desc: q('.description-text'), det: q('.details-box') };
}))()`;
const search = (q) => `(() => { const s = document.getElementById('search'); s.value = ${JSON.stringify(q)}; s.dispatchEvent(new Event('input')); return true; })()`;
const IDS_SHOWN = `[...document.querySelectorAll('#list-guides .sidebar-item, #list-terminal .sidebar-item')].map((li) => li.textContent)`;
const clickTopic = (key) => `(() => { const li = [...document.querySelectorAll('.sidebar-item')].find((l) => l.textContent.trim() === ${JSON.stringify(key)}); li.click(); return true; })()`;

test('help: every topic shows exactly its manual record — placeholders as text — in all 5 languages', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  for (const lang of LANGS) {
    await t.test(lang, async () => {
      const page = await open(browser, sim, lang);
      const cards = await page.eval(READ_ALL);
      assert.equal(cards.length, MANUALS.length + SPEC.length, 'one sidebar entry per manual and API endpoint');
      const bad = [];
      for (const c of cards) {
        const api = SPEC.find((s) => `${s.method} ${s.path}` === c.key);
        if (api) {
          const d = api[lang] || api.en;
          if (c.h2 !== api.path) bad.push(`${c.key}: title "${c.h2}"`);
          if (c.desc !== d.description) bad.push(`${c.key}: description`);
          if (c.det !== (d.details || null)) bad.push(`${c.key}: details`);
          continue;
        }
        const m = byId[c.key];
        if (!m) { bad.push(`${c.key}: no such manual`); continue; }
        const d = m[lang] || m.en;
        if (c.h2 !== d.title) bad.push(`${c.key}: title "${c.h2}" ≠ "${d.title}"`);
        if (c.syn !== d.synopsis) bad.push(`${c.key}: synopsis "${c.syn}" ≠ "${d.synopsis}"`);
        if (c.desc !== d.description) bad.push(`${c.key}: description`);
        if (c.det !== (d.details || null)) bad.push(`${c.key}: details`);
      }
      assert.deepEqual(bad.slice(0, 8), [], `${bad.length} topic(s) do not show their manual text`);
      assert.equal(await page.eval(`document.querySelectorAll('#content .synopsis-box code *, #content h2 *').length`), 0, 'no markup created from manual text');
    });
  }
});

test('help: search finds topics by their title in every language and highlights every match', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  for (const lang of LANGS) {
    await t.test(lang, async () => {
      const L = cat(lang);
      const page = await open(browser, sim, lang);
      for (const id of ['cat', 'grep', 'storage_layout', 'wifi']) {
        // The longest word of the localized title that is not the command name itself.
        const word = (byId[id][lang].title.match(/\p{L}{4,}/gu) || []).filter((w) => w.toLowerCase() !== id).sort((a, b) => b.length - a.length)[0];
        await page.eval(search(word));
        assert.ok((await page.eval(IDS_SHOWN)).includes(id), `"${word}" (${lang}) finds ${id}`);
      }
      await page.eval(search('zzqxv'));
      assert.equal(await page.eval(`(() => { const e = document.getElementById('no-results-msg'); return e && e.textContent.trim(); })()`), L.no_results);
      assert.equal(await page.eval(IDS_SHOWN + '.length'), 0);
    });
  }

  await t.test('every occurrence of the query in the open topic is highlighted', async () => {
    const page = await open(browser, sim, 'en');
    await page.eval(search('file'));
    await page.eval(clickTopic('cat'));
    // Text nodes that still contain the query outside a highlight (span-wrapped ones were processed).
    const left = await page.eval(`(() => { const w = document.createTreeWalker(document.getElementById('content'), NodeFilter.SHOW_TEXT); const out = []; let n;
      while ((n = w.nextNode())) { const p = n.parentNode.nodeName; if (!['MARK', 'SPAN', 'SCRIPT', 'STYLE'].includes(p) && /file/i.test(n.nodeValue)) out.push(p + ': ' + n.nodeValue.slice(0, 40)); }
      return out; })()`);
    assert.deepEqual(left, [], 'parts of the card where "file" was not highlighted');
    assert.ok(await page.eval(`document.querySelectorAll('#content mark.search-highlight').length`) >= 3);
  });
});

test('help: "Run in Terminal", ?path= and the API "Send Request" do what they say', { skip }, async (t) => {
  const sim = await startSim({ seed: { '/data/evil.txt': '<img src=x onerror="window.__pwn=1"> & done' } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });

  const page = await open(browser, sim, 'en');
  await page.eval(`window.__msgs = []; addEventListener('message', (e) => { if (e.data && e.data.type === 'open-app') window.__msgs.push(e.data); }); true`);
  await page.eval(clickTopic('grep'));
  await page.eval(`document.getElementById('exec-cmd-btn').click(), true`);
  assert.deepEqual(await page.waitFor(`window.__msgs.length && window.__msgs`, { timeout: 5000 }), [{ type: 'open-app', id: 'terminal', query: 'cmd=grep' }], 'the shell is asked for the Terminal with that command');
  await page.eval(clickTopic('storage_layout'));
  assert.equal(await page.eval(`document.getElementById('exec-cmd-btn')`), null, 'a guide has nothing to run');

  await page.eval(clickTopic('GET /api/fs/read'));
  await page.eval(`(() => { document.getElementById('param-path').value = '/data/evil.txt'; document.getElementById('try-api-btn').click(); return true; })()`);
  const body = await page.waitFor(`(() => { const b = document.querySelector('#api-response-container .response-body'); return b && b.textContent; })()`, { timeout: 10000 });
  assert.equal(body, '<img src=x onerror="window.__pwn=1"> & done', 'the device answer is shown verbatim');
  assert.equal(await page.eval(`document.querySelectorAll('#api-response-container img').length`), 0, 'as text, never markup');
  assert.match(await page.eval(`document.querySelector('#api-response-container .status-badge').textContent`), /200/);

  const p2 = await open(browser, sim, 'de', '?path=' + encodeURIComponent('/system/registry/manual/tail.info'));
  assert.ok(await p2.waitFor(`(() => { const h = document.querySelector('#content h2'); return h && h.textContent === ${JSON.stringify(byId.tail.de.title)}; })()`, { timeout: 10000 }), '?path= opens that topic, in the OS language');
  assert.equal(await p2.eval(`document.querySelector('#content .synopsis-box code').textContent`), byId.tail.de.synopsis);
});

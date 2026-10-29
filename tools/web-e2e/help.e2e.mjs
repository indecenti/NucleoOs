// Browser E2E — Help (System Manual) against the simulator. Since web OS 1.0 Help shows the manual that ships with
// the web OS (web/shell/help/<lang>.json: guide topics and the Terminal's command pages, five languages), only what
// applies to this device, plus the device's API reference. Every topic opens and shows exactly what the manual
// says — placeholders such as "cat <file>" are TEXT, not swallowed as unknown HTML tags; search finds commands and
// guides by their title in each language and highlights every match; "Run in Terminal" asks the shell for the
// Terminal with that command; ?path= still opens a device page; the API "Send Request" shows the answer as text.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell, LANGS } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const cat = (lang) => ({ ...rd(`../../web/shell/i18n/core.${lang}.json`), ...rd(`../../apps/help/www/i18n.${lang}.json`) });
const MANUAL = Object.fromEntries(LANGS.map((l) => [l, rd(`../../web/shell/help/${l}.json`)]));
const SPEC = rd('../../tools/sd-sim/system/registry/web-api-spec.json');
const DEVICE_PAGE = rd('../../tools/sd-sim/system/registry/manual/tail.info');   // a page of the device's own registry
// The simulator is a Cardputer: every capability a command page names is there, no system shell (pty).
const COMMANDS = (lang) => MANUAL[lang].commands;
const command = (lang, id) => COMMANDS(lang).find((c) => c.id === id);
const topic = (lang, id) => MANUAL[lang].topics.find((t) => t.id === id);

async function open(browser, sim, lang, query = '') {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang, wait: false });
  await page.goto(`${sim.origin}/apps/help/${query}`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list-terminal .sidebar-item').length >= ${COMMANDS(lang).length} && document.querySelectorAll('#list-api .sidebar-item').length >= 50 && document.querySelectorAll('#list-guides .sidebar-item').length >= 5`, { timeout: 45000 }), `help (${lang}) loaded its topics`);
  return page;
}
// Open every topic in the sidebar and read back what the card shows.
const READ_ALL = `(() => [...document.querySelectorAll('.sidebar-item')].map((li) => {
  li.click();
  const c = document.getElementById('content'), q = (s) => { const e = c.querySelector(s); return e ? e.textContent : null; };
  return { key: li.textContent.trim(), list: li.parentElement.id, h2: q('h2'), syn: q('.synopsis-box code'), desc: q('.description-text'), det: q('.details-box'), doc: q('.doc-body') };
}))()`;
const search = (q) => `(() => { const s = document.getElementById('search'); s.value = ${JSON.stringify(q)}; s.dispatchEvent(new Event('input')); return true; })()`;
const SHOWN = `[...document.querySelectorAll('#list-guides .sidebar-item, #list-terminal .sidebar-item')].map((li) => li.textContent.trim())`;
const clickTopic = (key) => `(() => { const li = [...document.querySelectorAll('.sidebar-item')].find((l) => l.textContent.trim() === ${JSON.stringify(key)}); li.click(); return true; })()`;
// The words of a guide as the card shows them: Markdown marks and list dashes gone, spacing not compared
const plain = (md) => md.replace(/\*\*|`/g, '').replace(/^#+ |^- /gm, '').replace(/\s+/g, '');

test('help: every topic shows exactly its manual text — placeholders as text — in all 5 languages', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  for (const lang of LANGS) {
    await t.test(lang, async () => {
      const page = await open(browser, sim, lang);
      const cards = await page.eval(READ_ALL);
      const bad = [];
      let guides = 0, commands = 0, apis = 0;
      for (const c of cards) {
        if (c.list === 'list-api') {
          const api = SPEC.find((s) => `${s.method} ${s.path}` === c.key);
          if (!api) { bad.push(`${c.key}: no such endpoint`); continue; }
          apis++;
          const d = api[lang] || api.en;
          if (c.h2 !== api.path) bad.push(`${c.key}: title "${c.h2}"`);
          if (c.desc !== d.description) bad.push(`${c.key}: description`);
          if (c.det !== (d.details || null)) bad.push(`${c.key}: details`);
        } else if (c.list === 'list-terminal') {
          const m = command(lang, c.key);
          if (!m) { bad.push(`${c.key}: no such command page`); continue; }
          commands++;
          if (c.h2 !== m.title) bad.push(`${c.key}: title "${c.h2}" ≠ "${m.title}"`);
          if (c.syn !== m.synopsis) bad.push(`${c.key}: synopsis "${c.syn}" ≠ "${m.synopsis}"`);
          if (c.desc !== m.description) bad.push(`${c.key}: description`);
          if (c.det !== (m.details || null)) bad.push(`${c.key}: details`);
        } else {
          const g = MANUAL[lang].topics.find((tp) => c.key.endsWith(tp.title));
          if (!g) { bad.push(`${c.key}: no such guide`); continue; }
          guides++;
          if (c.h2 !== g.title) bad.push(`${c.key}: title "${c.h2}"`);
          if (plain(c.doc || '') !== plain(g.md)) bad.push(`${c.key}: text`);
        }
      }
      assert.deepEqual(bad.slice(0, 8), [], `${bad.length} topic(s) do not show their manual text`);
      assert.equal(commands, COMMANDS(lang).length, 'every command page of this device, once');
      assert.equal(apis, SPEC.length, 'every API endpoint, once');
      assert.ok(guides >= 5 && !cards.some((c) => c.key.endsWith(topic(lang, 'accounts').title)), 'the guides of this device, not the accounts guide (no users.multi)');
      assert.equal(await page.eval(`document.querySelectorAll('#content .synopsis-box code *, #content h2 *').length`), 0, 'no markup created from manual text');
    });
  }
});

test('help: search finds commands and guides by their title in every language and highlights every match', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  for (const lang of LANGS) {
    await t.test(lang, async () => {
      const L = cat(lang);
      const page = await open(browser, sim, lang);
      for (const id of ['cat', 'grep', 'wifi']) {
        // The longest word of the localized title that is not the command name itself.
        const word = (command(lang, id).title.match(/\p{L}{4,}/gu) || []).filter((w) => w.toLowerCase() !== id).sort((a, b) => b.length - a.length)[0];
        await page.eval(search(word));
        assert.ok((await page.eval(SHOWN)).includes(id), `"${word}" (${lang}) finds ${id}`);
      }
      const files = topic(lang, 'files').title;
      await page.eval(search(files.match(/\p{L}{5,}/gu).pop()));
      assert.ok((await page.eval(SHOWN)).some((s) => s.endsWith(files)), `(${lang}) finds the guide "${files}"`);
      await page.eval(search('zzqxv'));
      assert.equal(await page.eval(`(() => { const e = document.getElementById('no-results-msg'); return e && e.textContent.trim(); })()`), L.no_results);
      assert.equal(await page.eval(SHOWN + '.length'), 0);
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

  // Opened on its own (no shell around it), "Run in Terminal" goes to the Terminal with that command
  // (nucleo-shell.js openApp); inside the shell it asks the shell for that window instead.
  const run = await open(browser, sim, 'en');
  await run.eval(clickTopic('grep'));
  await run.eval(`document.getElementById('exec-cmd-btn').click(), true`);
  assert.ok(await run.waitFor(`location.pathname === '/apps/terminal/' && new URLSearchParams(location.search).get('cmd') === 'grep'`, { timeout: 10000 }), 'the Terminal opens with that command');

  const page = await open(browser, sim, 'en');
  const guide = topic('en', 'keyboard');
  await page.eval(`(() => { const li = [...document.querySelectorAll('#list-guides .sidebar-item')].find((l) => l.textContent.trim().endsWith(${JSON.stringify(guide.title)})); li.click(); return true; })()`);
  assert.equal(await page.eval(`document.querySelector('#content h2').textContent`), guide.title);
  assert.equal(await page.eval(`document.getElementById('exec-cmd-btn')`), null, 'a guide has nothing to run');

  await page.eval(clickTopic('GET /api/fs/read'));
  await page.eval(`(() => { document.getElementById('param-path').value = '/data/evil.txt'; document.getElementById('try-api-btn').click(); return true; })()`);
  const body = await page.waitFor(`(() => { const b = document.querySelector('#api-response-container .response-body'); return b && b.textContent; })()`, { timeout: 10000 });
  assert.equal(body, '<img src=x onerror="window.__pwn=1"> & done', 'the device answer is shown verbatim');
  assert.equal(await page.eval(`document.querySelectorAll('#api-response-container img').length`), 0, 'as text, never markup');
  assert.match(await page.eval(`document.querySelector('#api-response-container .status-badge').textContent`), /200/);

  const p2 = await open(browser, sim, 'de', '?path=' + encodeURIComponent('/system/registry/manual/tail.info'));
  assert.ok(await p2.waitFor(`(() => { const h = document.querySelector('#content h2'); return h && h.textContent === ${JSON.stringify(DEVICE_PAGE.de.title)}; })()`, { timeout: 10000 }), '?path= opens that page, in the OS language');
  assert.equal(await p2.eval(`document.querySelector('#content .synopsis-box code').textContent`), DEVICE_PAGE.de.synopsis);
});

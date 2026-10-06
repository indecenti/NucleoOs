// Browser E2E — ANIMA's local-server path against a REAL Ollama on this PC (skipped when none runs).
//  • from http://localhost (what NucleoOS Link / a flagged origin gives): detection with real per-model
//    capabilities, task-based model choice, an Italian TOOL CALL that books the right year because the
//    prompt carries the clock — all browser → PC, zero requests to the Cardputer;
//  • from the device-like origin: Ollama refuses it (CORS) and the probe says exactly that ('cors'), which
//    is what the OLLAMA_ORIGINS wizard keys on.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
// Chrome's Local Network Access: a page served from the network (the Cardputer) may reach localhost only once the
// user allows "Local network access" for the site. Headless Chrome reports it 'denied' and cannot grant it (CDP:
// "can't be granted in current context"), so the device-like cases ran into 'blocked' and never reached Ollama's
// CORS answer they exist to check. GRANTED = the user allowed it: the check is off and the permission reads granted.
const LNA_OFF = '--disable-features=LocalNetworkAccessChecks';
const LNA_GRANTED = `(() => { const q = navigator.permissions && navigator.permissions.query && navigator.permissions.query.bind(navigator.permissions);
  if (!q) return; const lna = new Set(['loopback-network', 'local-network-access', 'local-network']);
  navigator.permissions.query = (d) => (d && lna.has(d.name)) ? Promise.resolve({ state: 'granted', name: d.name, onchange: null }) : q(d); })();`;
let ollamaUp = false;
try { ollamaUp = (await fetch('http://localhost:11434/api/version', { signal: AbortSignal.timeout(1500) })).ok; } catch {}
const skip = (!findChrome() && 'no Chrome/Edge installed') || (!ollamaUp && 'no Ollama on localhost:11434');

test('local AI server (real Ollama) from the browser', { skip, timeout: 10 * 60 * 1000 }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  const granted = await launchBrowser({ args: [HOST_RULES, LNA_OFF] });
  t.after(async () => { await browser.close(); await granted.close(); await sim.stop(); });
  const grantedPage = async () => { const p = await granted.newPage(); await p.initScript(LNA_GRANTED); return p; };

  await t.test('secure origin: detect, pick, and call a tool in Italian with the right date', async () => {
    const page = await browser.newPage();
    await bootShell(page, sim, { lang: 'it', wait: false, origin: sim.local });
    await page.goto(sim.local + '/api/status');          // a quiet same-origin page: no shell polling in the count
    await sim.control('/api/_sim/stats', { reset: true });
    const r = await page.eval(`(async () => {
      const E = await import('/ai-engines.js');
      const AI = await import('/ai.js');
      const [ol] = await E.detectServers({ servers: [E.DEFAULT_SERVERS[0]] });
      const model = E.pickModel(ol.models, 'agent');
      const tools = [{ type: 'function', function: { name: 'add_event', description: 'Add an event to the OS calendar',
        parameters: { type: 'object', properties: { title: { type: 'string' }, when: { type: 'string', description: 'ISO 8601 date-time' } }, required: ['title', 'when'] } } }];
      const sys = 'Sei ANIMA, l\\'assistente di NucleoOS. Usa gli strumenti quando servono. Adesso: ' + AI.nowText('it') + '.';
      const t0 = performance.now();
      // the PRODUCT path: the user's/auto choice, falling through lighter models if one doesn't fit in VRAM
      const out = await E.localComplete('agent', { tools, servers: [ol], messages: [{ role: 'system', content: sys }, { role: 'user', content: 'Crea un promemoria per domani alle 9: chiamare Marco.' }] });
      return { status: ol.status, n: ol.models.length, caps: ol.models.map((m) => m.id + ':' + (m.caps && m.caps.tools ? 'T' : '-')), model, out, ms: Math.round(performance.now() - t0) };
    })()`, 5 * 60 * 1000);
    assert.ok(r.out, 'a local model answered (null = every model ran out of GPU memory — is something else holding the GPU?)');
    t.diagnostic(`Ollama ${r.status}: ${r.n} models [${r.caps.join(' ')}] → first choice ${r.model}, answered by ${r.out.engine.model}${r.out.engine.skipped ? ' (skipped: ' + r.out.engine.skipped.join(', ') + ')' : ''}; ${r.ms} ms, ${r.out.usage.tokPerSec} tok/s`);
    t.diagnostic('tool call: ' + JSON.stringify(r.out.toolCalls));
    assert.equal(r.status, 'ok');
    assert.ok(r.n > 0 && r.model, 'a tools-capable model was chosen');
    const call = r.out.toolCalls.find((c) => c.name === 'add_event');
    assert.ok(call, 'the model called add_event');
    const tomorrow = new Date(Date.now() + 86400000);
    const ymd = `${tomorrow.getFullYear()}-${String(tomorrow.getMonth() + 1).padStart(2, '0')}-${String(tomorrow.getDate()).padStart(2, '0')}`;
    assert.ok(String(call.arguments.when || '').startsWith(ymd), `booked for tomorrow (${ymd}), not the model's training year: ${call.arguments.when}`);
    assert.match(String(call.arguments.title || ''), /marco/i);
    const st = await sim.control('/api/_sim/stats');
    const viaDevice = Object.keys(st.byPath).filter((p) => /^\/api\/(llm|anima|proxy|online)/.test(p));
    assert.deepEqual(viaDevice, [], 'inference never goes through the Cardputer');
    assert.ok(!Object.keys(st.byPath).some((p) => p.startsWith('/api/')), 'no device API call at all: ' + JSON.stringify(st.byPath));
  });

  // The PRODUCT path: the ANIMA app itself, Private mode (no network, no cloud key), a question no offline
  // rung can answer. The PC's Ollama must answer it, in the OS language, and say so in the engine map —
  // while the Cardputer sees no inference traffic at all.
  for (const [lang, ask, re] of [
    ['it', 'Scrivi una frase di benvenuto per un piccolo computer tascabile.', /\b(benvenut|ciao)/i],
    ['de', 'Schreib einen Willkommenssatz für einen kleinen Taschencomputer.', /\b(willkommen|hallo)/i],
  ]) {
    await t.test(`ANIMA app, Private (${lang}): answered by the PC's Ollama, named in the engine map`, async () => {
      const page = await browser.newPage();
      await bootShell(page, sim, { lang, wait: false, origin: sim.local });
      await page.eval(`localStorage.setItem('anima.mode','private'); localStorage.setItem('anima.modeSet','1'); true`);
      await page.goto(sim.local + '/apps/anima/');
      assert.ok(await page.waitFor(`!!document.getElementById('q') && !!document.getElementById('send')`, { timeout: 20000 }));
      await sim.control('/api/_sim/stats', { reset: true });
      await page.eval(`(() => { const q = document.getElementById('q'); q.value = ${JSON.stringify(ask)}; q.dispatchEvent(new Event('input', { bubbles: true })); document.getElementById('send').click(); return true; })()`);
      const got = await page.waitFor(`(() => { const e = [...document.querySelectorAll('details.emap')].pop(); if (!e || !e.classList.contains('k-server')) return null;
        const bots = document.querySelectorAll('.msg.bot, .bot'); const b = bots[bots.length - 1];
        return { line: e.querySelector('summary').textContent, text: (e.closest('.turn, .msg, .bot') || b || document.body).textContent }; })()`, { timeout: 4 * 60 * 1000 });
      assert.ok(got, 'the turn was answered by the local server (engine map k-server)');
      t.diagnostic(`${lang}: ${got.line}`);
      assert.match(got.line, /Ollama/);
      assert.match(got.text, re, `${lang} reply: ${got.text.slice(0, 200)}`);
      const st = await sim.control('/api/_sim/stats');
      // Inference routes only: /api/anima?q= (the device cascade) and the relays. /api/anima/l1 is the one-off
      // L1-policy POST ANIMA sends at launch (config, a few bytes) — not a turn.
      const viaDevice = Object.keys(st.byPath).filter((p) => p === '/api/anima' || /^\/api\/(llm|proxy|online|transcribe)(\/|$)/.test(p));
      assert.deepEqual(viaDevice, [], 'inference never goes through the Cardputer');
    });
  }

  // From the Cardputer's origin Ollama refuses the page. ANIMA must SAY so once, and the wizard must carry the
  // exact command for this OS with this page's origin in it — in the OS language.
  for (const [lang, word] of [['it', 'PowerShell'], ['fr', 'PowerShell']]) {
    await t.test(`device-like origin (${lang}): ANIMA points to the OLLAMA_ORIGINS fix with the exact command`, async () => {
      const page = await grantedPage();
      await bootShell(page, sim, { lang, wait: false });
      await page.eval(`localStorage.setItem('anima.mode','private'); localStorage.setItem('anima.modeSet','1'); sessionStorage.clear(); true`);
      await page.goto(sim.origin + '/apps/anima/');
      assert.ok(await page.waitFor(`!!document.getElementById('q') && !!document.getElementById('send')`, { timeout: 20000 }));
      await page.eval(`(() => { const q = document.getElementById('q'); q.value = 'Scrivi una poesia sul mare.'; q.dispatchEvent(new Event('input', { bubbles: true })); document.getElementById('send').click(); return true; })()`);
      const hint = await page.waitFor(`(() => { const b = document.querySelector('.lai-hint .lai-how'); return b ? b.parentElement.textContent : null; })()`, { timeout: 60000 });
      assert.ok(hint && /Ollama/.test(hint), 'the one-time hint names the refusing server: ' + hint);
      await page.eval(`document.querySelector('.lai-hint .lai-how').click()`);
      const html = await page.waitFor(`(() => { const b = document.getElementById('dlg-b'); return b && b.querySelector('.lai-help') ? b.innerHTML : null; })()`, { timeout: 10000 });
      assert.ok(html, "the wizard opens in ANIMA's dialog");
      if (process.platform === 'win32') {
        assert.ok(html.includes(`setx OLLAMA_ORIGINS "${sim.origin}"`), "the Windows command carries this page's exact origin");
        assert.ok(html.includes(word));
      }
      const c = JSON.parse((await import('node:fs')).readFileSync(new URL(`../../web/shell/i18n/core.${lang}.json`, import.meta.url), 'utf8'));
      const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
      assert.ok(html.includes(esc(c.cap_verify)), `${lang}: "check again" in ${lang}`);
      assert.ok(html.includes(esc(c.lai_why.replace('{server}', 'Ollama').replace('{origin}', sim.origin))), `${lang}: the explanation in ${lang}, with the origin`);
    });
  }

  await t.test('device-like origin: the probe reports CORS (the OLLAMA_ORIGINS fix), not "down"', async () => {
    const page = await grantedPage();
    await bootShell(page, sim, { lang: 'it', wait: false });
    const status = await page.eval(`(async () => { const E = await import('/ai-engines.js'); const [ol] = await E.detectServers({ servers: [E.DEFAULT_SERVERS[0]] }); return ol.status; })()`);
    assert.equal(status, 'cors');
  });

  // The other real case: the browser does NOT allow local network access (Chrome's default on a site served from
  // the Cardputer). Nothing reaches Ollama, so ANIMA must say what to allow, in the OS language — never "down".
  for (const lang of ['it', 'en', 'es', 'fr', 'de']) {
    await t.test(`device-like origin, local network access denied (${lang}): ANIMA says what to allow`, async () => {
      const page = await browser.newPage();
      await bootShell(page, sim, { lang, wait: false });
      const status = await page.eval(`(async () => { const E = await import('/ai-engines.js'); const [ol] = await E.detectServers({ servers: [E.DEFAULT_SERVERS[0]] }); return ol.status; })()`);
      assert.equal(status, 'blocked');
      await page.eval(`localStorage.setItem('anima.mode','private'); localStorage.setItem('anima.modeSet','1'); sessionStorage.clear(); true`);
      await page.goto(sim.origin + '/apps/anima/');
      assert.ok(await page.waitFor(`!!document.getElementById('q') && !!document.getElementById('send')`, { timeout: 20000 }));
      await page.eval(`(() => { const q = document.getElementById('q'); q.value = 'Ciao'; q.dispatchEvent(new Event('input', { bubbles: true })); document.getElementById('send').click(); return true; })()`);
      const hint = await page.waitFor(`(() => { const h = document.querySelector('.lai-hint'); return h ? h.textContent : null; })()`, { timeout: 60000 });
      const c = JSON.parse((await import('node:fs')).readFileSync(new URL(`../../apps/anima/www/i18n.${lang}.json`, import.meta.url), 'utf8'));
      assert.ok(hint && hint.includes(c.engPcBlocked), `${lang}: the hint explains the browser block: ${hint}`);
    });
  }
});

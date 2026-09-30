// Browser E2E — ANIMA's in-browser model path, end to end.
//  A. (always) On a plain-http origin — exactly the Cardputer's — the "How to turn it on" button opens the
//     OS-wide capability panel with the RIGHT fix for this browser (flag page + exact origin + copy + verify),
//     in all five languages. The old text told Android users to "use a computer" and never said the same
//     switch unlocks the model cache.
//  B. (E2E_GPU=1, real GPU, downloads ~1 GB once into build/web-e2e/chrome-gpu-profile) On a secure origin:
//     probe → Qwen3-1.7B installs through ANIMA's own loader → it answers in Italian AND German, and knows
//     today's date (every prompt carries the real clock now). Reports tokens/s.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim, REPO } from './sim.mjs';
import { LANGS, bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const core = (lang) => JSON.parse(readFileSync(join(REPO, 'web', 'shell', 'i18n', `core.${lang}.json`), 'utf8'));
const skip = !findChrome() && 'no Chrome/Edge installed';

test('A. insecure origin: ANIMA explains how to unlock WebGPU with the shared, correct panel (5 languages)', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  for (const lang of LANGS) {
    await t.test(lang, async () => {
      const page = await browser.newPage();
      await bootShell(page, sim, { lang, wait: false });
      await page.goto(`${sim.origin}/apps/anima/`);
      assert.ok(await page.waitFor(`document.querySelectorAll('#set-localmodel option').length > 0 && !!document.getElementById('localmodel-get').dataset.why`, { timeout: 20000 }),
        'the local-model row shows the "how to turn it on" state on an http page');
      await page.eval(`document.getElementById('localmodel-get').click()`);
      const html = await page.waitFor(`(() => { const b = document.getElementById('dlg-b'); return b && b.querySelector('.cap-help') ? b.innerHTML : null; })()`, { timeout: 10000 });
      assert.ok(html, 'the dialog hosts the shared capability panel');
      const c = core(lang);
      assert.ok(html.includes('chrome://flags/#unsafely-treat-insecure-origin-as-secure'), 'Chrome flag page');
      assert.ok(html.includes(sim.origin), 'the exact origin to add');
      for (const k of ['cap_secure', 'cap_webgpu', 'cap_fix_type', 'cap_tip_unlocks', 'cap_verify']) {
        assert.ok(html.includes(c[k].replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;')), `${lang}: ${k} shown in ${lang}`);
      }
      assert.ok(/class="cap-row no"/.test(html), 'the checklist marks what is missing');
    });
  }
});

test('B. real GPU: Qwen3-1.7B installs and answers in Italian and German, knowing today\'s date', {
  skip: skip || (process.env.E2E_GPU !== '1' && 'set E2E_GPU=1 (needs a GPU; downloads ~1 GB once)'), timeout: 30 * 60 * 1000,
}, async (t) => {
  const sim = await startSim({ seed: { '/data/proj/app.js': 'export function somma(a, b) {\n  return a - b;\n}\n', '/data/proj/notes.md': '# Spesa\n\n- latte\n- pane\n- mele\n' } });
  const browser = await launchBrowser({ gpu: true, profileDir: join(REPO, 'build', 'web-e2e', 'chrome-gpu-profile') });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'it', wait: false, origin: sim.local });
  await page.goto(`${sim.local}/apps/anima/`);
  await page.waitFor(`document.querySelectorAll('#set-localmodel option').length > 0`, { timeout: 30000 });

  const probe = await page.eval(`(async () => { const m = await import('/apps/anima/local-llm.js'); localStorage.setItem('anima.localModel', 'Qwen3-1.7B-q4f16_1-MLC');
    const c = await m.probeLocal({ fresh: true }); return { c, build: m.chosenLocalBuild() }; })()`);
  t.diagnostic(`GPU: ${JSON.stringify(probe.c.adapter)} class=${probe.c.adapterClass} f16=${probe.c.f16} → ${probe.build}`);
  assert.equal(probe.c.webgpu, true);
  assert.equal(probe.build, probe.c.f16 ? 'Qwen3-1.7B-q4f16_1-MLC' : 'Qwen3-1.7B-q4f32_1-MLC');

  const t0 = Date.now();
  const load = await page.eval(`(async () => { const m = await import('/apps/anima/local-llm.js'); let last = '';
    await m.loadLocal((p) => { last = p && p.text || last; }, { allowDownload: true }); return { ok: m.localReady(), last }; })()`, 25 * 60 * 1000);
  t.diagnostic(`install/load: ${Math.round((Date.now() - t0) / 1000)} s — ${load.last}`);
  assert.equal(load.ok, true, 'the model is loaded on the GPU');

  const ask = (q, lang) => page.eval(`(async () => { const m = await import('/apps/anima/local-llm.js'); const s = performance.now();
    const r = await m.queryLocal(${JSON.stringify(q)}, ${JSON.stringify(lang)}, [], null); return { reply: r && r.reply, ms: Math.round(performance.now() - s) }; })()`, 180000);
  const year = String(new Date().getFullYear());
  const it = await ask('Che giorno è oggi? Rispondi con giorno, mese e anno.', 'it');
  t.diagnostic(`it (${it.ms} ms): ${it.reply}`);
  assert.ok(it.reply && it.reply.includes(year), 'Italian answer knows the current year: ' + it.reply);
  assert.ok(!/<think>/.test(it.reply), 'no thinking residue');
  const de = await ask('Welches Datum haben wir heute? Antworte kurz auf Deutsch, mit Tag, Monat und Jahr.', 'de');
  t.diagnostic(`de (${de.ms} ms): ${de.reply}`);
  assert.ok(de.reply && de.reply.includes(year), 'German answer knows the current year: ' + de.reply);
  assert.ok(/\b(heute|ist|der|den|am|Januar|Februar|März|April|Mai|Juni|Juli|August|September|Oktober|November|Dezember)\b/i.test(de.reply) && !/\b(oggi|è|today)\b/i.test(de.reply),
    'the German question is answered in German (the prompt used to say "Rispondi in italiano"): ' + de.reply);
  // NB measured 2026-09-29: Qwen3-1.7B copied the date but said "Montag" on a Tuesday. A small model must
  // never be the source of truth for the clock — date/time questions are answered deterministically by
  // ANIMA's own cascade (see the plan, phase 4); the LLM only needs the context to reason with.

  // C. The same GPU model as a TOOL-USING AGENT in the ANIMA app: Private, no PC server (disabled), a workspace
  // on the SD. The Agenti runtime's grammar-constrained loop (local-worker.js) drives the real file tools; the
  // result is checked ON THE SD. Same page context as above, so the ~1 GB model is not fetched twice.
  await page.eval(`localStorage.setItem('anima.mode','private'); localStorage.setItem('anima.modeSet','1'); localStorage.setItem('anima.agentauto','1');
    localStorage.setItem('ai.local.engines', JSON.stringify({ enabled: false, servers: [], models: {} }));
    localStorage.setItem('anima.ws', JSON.stringify({ root: '/data/proj', recents: [] })); true`);
  await page.goto(`${sim.local}/apps/anima/`);
  assert.ok(await page.waitFor(`!!document.getElementById('q') && !!document.getElementById('send')`, { timeout: 30000 }));
  const agentTurn = async (ask) => {
    const before = await page.eval(`document.querySelectorAll('details.emap').length`);
    const s0 = Date.now();
    await page.eval(`(() => { const q = document.getElementById('q'); q.value = ${JSON.stringify(ask)}; q.dispatchEvent(new Event('input', { bubbles: true })); document.getElementById('send').click(); return true; })()`);
    const got = await page.waitFor(`(() => { const all = document.querySelectorAll('details.emap'); if (all.length <= ${before}) return null; const e = all[all.length - 1];
      return { line: e.querySelector('summary').textContent, kind: [...e.classList].find((c) => c.startsWith('k-')) || '', text: (e.parentElement || document.body).textContent.slice(0, 800) }; })()`, { timeout: 8 * 60 * 1000 });
    return got && { ...got, s: Math.round((Date.now() - s0) / 1000) };
  };
  const fix = await agentTurn('Correggi la funzione somma in app.js: deve sommare i due numeri, non sottrarli.');
  t.diagnostic(`GPU agent, fix (${fix && fix.s} s): ${fix && fix.line}`);
  assert.ok(fix, 'the agent turn finished');
  assert.equal(fix.kind, 'k-gpu', 'answered on the browser GPU: ' + fix.line);
  assert.match(fix.line, /Qwen3-1\.7B/, 'the engine map names the model that really ran: ' + fix.line);
  const src = await sim.readSd('/data/proj/app.js');
  assert.match(src, /return\s+a\s*\+\s*b/, 'the GPU agent fixed the file on the SD:\n' + src);
  const count = await agentTurn('Leggi notes.md e dimmi quanti elementi ci sono nella lista della spesa.');
  t.diagnostic(`GPU agent, read (${count && count.s} s): ${count && count.text.slice(0, 200)}`);
  assert.ok(count && /\b(3|tre)\b/i.test(count.text), 'the right count from the file: ' + (count && count.text));
});

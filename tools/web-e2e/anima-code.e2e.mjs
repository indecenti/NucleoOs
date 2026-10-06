// Browser E2E — ANIMA Code on the user's OWN PC model: the real ANIMA app, in Private (no cloud key, no
// network), with a workspace on the simulated SD, driving a REAL Ollama through the Agenti runtime's native
// tool loop. This is the "OpenCode on a Cardputer" claim, executed: the model reads, searches, edits and
// creates files; the Cardputer only stores them and never runs a single inference request.
// Skipped when Chrome or Ollama is missing. Each task is checked on the RESULT (the file on the SD), not on
// what the model says it did. Tasks cover the five OS languages.
import test from 'node:test';
import assert from 'node:assert/strict';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

let ollamaUp = false;
try { ollamaUp = (await fetch('http://localhost:11434/api/version', { signal: AbortSignal.timeout(1500) })).ok; } catch {}
const skip = (!findChrome() && 'no Chrome/Edge installed') || (!ollamaUp && 'no Ollama on localhost:11434');

const WS = '/data/proj';
const SEED = {
  [WS + '/app.js']: 'export function somma(a, b) {\n  return a - b;\n}\n\nexport function moltiplica(a, b) {\n  return a * b;\n}\n',
  [WS + '/notes.md']: '# Shopping\n\n- milk\n- bread\n- apples\n- coffee\n',
  [WS + '/lib/format.js']: 'export const euro = (n) => n.toFixed(2) + " EUR";\n',
};

// One turn in the ANIMA app; resolves when the reply is on screen. → { line, text, kind, ms }
async function turn(page, ask, { timeout = 6 * 60 * 1000 } = {}) {
  const before = await page.eval(`document.querySelectorAll('details.emap').length`);
  const t0 = Date.now();
  await page.eval(`(() => { const q = document.getElementById('q'); q.value = ${JSON.stringify(ask)}; q.dispatchEvent(new Event('input', { bubbles: true })); document.getElementById('send').click(); return true; })()`);
  const got = await page.waitFor(`(() => { const all = document.querySelectorAll('details.emap'); if (all.length <= ${before}) return null;
    const e = all[all.length - 1]; const turn = e.closest('.turn, .msg, .bot') || e.parentElement;
    return { line: e.querySelector('summary').textContent, kind: [...e.classList].find((c) => c.startsWith('k-')) || '', text: (turn && turn.textContent || '').slice(0, 1500) }; })()`, { timeout });
  return got && { ...got, ms: Date.now() - t0 };
}

// Start from an empty GPU. A model left resident by anything else (measured: the 35B, 22.5 GB on an 8 GB card)
// made Ollama swap models inside the first task, which then ran past its 6-minute window — the suite failed
// 1–4 cases depending on what had run before, with the same code. Unloading costs nothing; the first task
// then pays one plain load of the model it picks.
async function unloadOllamaModels() {
  try {
    const ps = await (await fetch('http://localhost:11434/api/ps', { signal: AbortSignal.timeout(5000) })).json();
    for (const m of (ps.models || [])) {
      await fetch('http://localhost:11434/api/generate', { method: 'POST', body: JSON.stringify({ model: m.name, keep_alive: 0 }), signal: AbortSignal.timeout(60000) }).catch(() => {});
    }
  } catch {}
}

test('ANIMA Code on the PC model (real Ollama), Private, workspace on the SD', { skip, timeout: 40 * 60 * 1000 }, async (t) => {
  await unloadOllamaModels();
  const sim = await startSim({ seed: SEED });
  const browser = await launchBrowser();
  t.after(async () => { await browser.close(); await sim.stop(); });

  async function openAnima(lang) {
    const page = await browser.newPage();
    await bootShell(page, sim, { lang, wait: false, origin: sim.local });
    await page.eval(`localStorage.setItem('anima.mode','private'); localStorage.setItem('anima.modeSet','1');
      localStorage.setItem('anima.agentauto','1'); localStorage.setItem('anima.agents','1');
      localStorage.setItem('anima.ws', JSON.stringify({ root: ${JSON.stringify(WS)}, recents: [] })); true`);
    await page.goto(sim.local + '/apps/anima/');
    assert.ok(await page.waitFor(`!!document.getElementById('q') && !!document.getElementById('send')`, { timeout: 20000 }));
    await sim.control('/api/_sim/stats', { reset: true });
    return page;
  }
  async function noDeviceInference() {
    const st = await sim.control('/api/_sim/stats');
    const via = Object.keys(st.byPath).filter((p) => p === '/api/anima' || /^\/api\/(llm|proxy|online|transcribe)(\/|$)/.test(p));
    assert.deepEqual(via, [], 'no inference through the Cardputer: ' + JSON.stringify(st.byPath));
  }
  const report = (lang, r) => t.diagnostic(`${lang}: ${Math.round(r.ms / 1000)} s · ${r.line.trim()}`);

  await t.test('it: fix a bug in an existing file (read → edit), checked on the SD', async () => {
    const page = await openAnima('it');
    const r = await turn(page, 'Correggi la funzione somma in app.js: deve sommare i due numeri, non sottrarli. Non toccare moltiplica.');
    assert.ok(r, 'the turn finished'); report('it', r);
    assert.equal(r.kind, 'k-server', 'answered by the local server: ' + r.line);
    assert.match(r.line, /Ollama/);
    const src = await sim.readSd(WS + '/app.js');
    assert.match(src, /return\s+a\s*\+\s*b/, 'somma now adds:\n' + src);
    assert.match(src, /return\s+a\s*\*\s*b/, 'moltiplica untouched:\n' + src);
    await noDeviceInference();
  });

  await t.test('it: create a working HTML page from a description', async () => {
    const page = await openAnima('it');
    const r = await turn(page, 'Crea il file orologio.html: una pagina con un orologio digitale che mostra ore:minuti:secondi e si aggiorna ogni secondo.');
    assert.ok(r); report('it', r);
    assert.equal(r.kind, 'k-server', r.line);
    const html = await sim.readSd(WS + '/orologio.html');
    assert.match(html, /<html|<!doctype/i, 'an HTML page');
    assert.match(html, /setInterval|setTimeout|requestAnimationFrame/, 'it updates itself');
    assert.match(html, /getHours|toLocaleTimeString|Date\(/, 'it reads the clock');
    await noDeviceInference();
  });

  await t.test('en: read a file and answer from its content', async () => {
    const page = await openAnima('en');
    const r = await turn(page, 'Read notes.md and tell me how many items are on the shopping list.');
    assert.ok(r); report('en', r);
    assert.equal(r.kind, 'k-server', r.line);
    assert.match(r.text, /\b(4|four)\b/i, 'the right count from the file: ' + r.text.slice(0, 300));
  });

  await t.test('es: search the workspace for where something is defined', async () => {
    const page = await openAnima('es');
    const r = await turn(page, 'Busca en el proyecto en qué archivo está definida la función euro y dime el nombre del archivo.');
    assert.ok(r); report('es', r);
    assert.equal(r.kind, 'k-server', r.line);
    assert.match(r.text, /format\.js/, 'names lib/format.js: ' + r.text.slice(0, 300));
  });

  for (const [lang, ask, file, re] of [
    ['fr', 'Crée le fichier bonjour.txt avec le texte : Bonjour le monde', 'bonjour.txt', /bonjour le monde/i],
    ['de', 'Erstelle die Datei todo.md mit einer Liste von drei Aufgaben für heute.', 'todo.md', /^\s*(?:[-*]|\d+\.)\s+\S+[\s\S]*^\s*(?:[-*]|\d+\.)\s+\S+[\s\S]*^\s*(?:[-*]|\d+\.)\s+\S+/m],
  ]) {
    await t.test(`${lang}: create a file`, async () => {
      const page = await openAnima(lang);
      const r = await turn(page, ask);
      assert.ok(r); report(lang, r);
      assert.equal(r.kind, 'k-server', r.line);
      const body = await sim.readSd(WS + '/' + file);
      assert.match(body, re, file + ':\n' + body);
      await noDeviceInference();
    });
  }

  await t.test('it: a plain chat turn stays chat (no tools, ANIMA grounding)', async () => {
    const page = await openAnima('it');
    const r = await turn(page, 'Scrivi una frase di benvenuto per un piccolo computer tascabile.');
    assert.ok(r); report('it', r);
    assert.equal(r.kind, 'k-server');
    assert.doesNotMatch(r.line, /agent/i, 'not routed to the agent: ' + r.line);
  });
});

// Help (System Manual) data integrity — pure Node, no browser/device. The Help app renders whatever the
// registry holds, so a broken record is a broken topic for the user:
//   · every registry/manual/*.info parses, its id matches the file name, its category is one the sidebar
//     shows, and it has a title/synopsis/description in all 5 OS languages (the app picks manual[lang]);
//   · every "see also" reference — name(N) — points at a manual or at a real Terminal command;
//   · the Help app's offline fallback list and its guide list name only manuals that exist;
//   · every Web API spec entry the "Mini Swagger" lists has what its card renders.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';

const root = new URL('../', import.meta.url);
const read = (p) => readFileSync(new URL(p, root), 'utf8');
const LANGS = ['it', 'en', 'es', 'fr', 'de'];
const MAN = 'registry/manual/';
const files = readdirSync(new URL(MAN, root)).filter((f) => f.endsWith('.info')).sort();
const manuals = files.map((f) => ({ f, j: JSON.parse(read(MAN + f)) }));
const ids = new Set(manuals.map((m) => m.j.id));
const help = read('apps/help/www/index.html');

// Terminal commands = the method names of the COMMANDS table in the Terminal app.
function terminalCommands() {
  const src = read('apps/terminal/www/index.html');
  const start = src.indexOf('const COMMANDS = {');
  assert.ok(start > 0, 'the Terminal COMMANDS table was found');
  const names = new Set();
  for (const m of src.slice(start).matchAll(/^ {6}(?:async\s+)?([a-z][a-z0-9_]*)\s*\(/gm)) {
    if (['if', 'for', 'while', 'switch', 'catch', 'function', 'return'].includes(m[1])) continue;
    names.add(m[1]);
    if (m[1] === 'clear') break;                       // the last entry of the table
  }
  return names;
}

test('help manuals: every record parses, is named after its file and has every language the app can show', () => {
  assert.ok(manuals.length >= 50, 'the manual registry is populated');
  const bad = [];
  for (const { f, j } of manuals) {
    if (j.id + '.info' !== f) bad.push(`${f}: id "${j.id}"`);
    if (!['guide', 'terminal', 'system'].includes(j.category)) bad.push(`${f}: category "${j.category}" has no sidebar section`);
    for (const l of LANGS) {
      const d = j[l];
      if (!d || typeof d !== 'object') { bad.push(`${f}: no "${l}"`); continue; }
      for (const k of ['title', 'synopsis', 'description']) if (typeof d[k] !== 'string' || !d[k].trim()) bad.push(`${f}: ${l}.${k} missing`);
      if (d.details != null && typeof d.details !== 'string') bad.push(`${f}: ${l}.details is not text`);
    }
  }
  assert.deepEqual(bad, []);
});

test('help manuals: every "see also" reference points at a manual or a Terminal command', () => {
  const cmds = terminalCommands();
  assert.ok(cmds.has('ls') && cmds.has('help') && cmds.has('man'), 'command table parsed');
  const dead = [];
  for (const { f, j } of manuals) for (const l of LANGS) for (const v of Object.values(j[l] || {})) {
    if (typeof v !== 'string') continue;
    for (const m of v.matchAll(/\b([a-z][a-z0-9_-]*)\((\d)\)/g)) if (!ids.has(m[1]) && !cmds.has(m[1])) dead.push(`${f} (${l}): ${m[0]}`);
  }
  assert.deepEqual([...new Set(dead)], []);
});

// Since web OS 1.0 Help shows the manual that ships with the web OS (nucleo-webos docs/user -> /help/<lang>.json,
// topics and Terminal pages filtered by what the device can do). The made-up offline list of commands it used to
// show when the device had no pages is gone, and must not come back.
test('help app: reads the manual that ships with the web OS, no hard-coded list of commands', () => {
  assert.ok(!/const fallbackIds = \[/.test(help), 'a hard-coded offline list of commands is back');
  assert.match(help, /'\/help\/' \+ l \+ '\.json'/, 'Help loads /help/<lang>.json');
  for (const l of LANGS) {
    const man = JSON.parse(read(`web/shell/help/${l}.json`));
    assert.ok(man.topics.length >= 5 && man.commands.length >= 40, `the ${l} manual is complete`);
  }
});

test('help API spec: every endpoint card has a method, an /api path and en text (it/en, en is the fallback)', () => {
  const spec = JSON.parse(read('registry/web-api-spec.json'));
  assert.ok(Array.isArray(spec) && spec.length >= 50);
  const bad = [];
  const seen = new Set();
  for (const s of spec) {
    if (seen.has(s.id)) bad.push(`duplicate id ${s.id}`); seen.add(s.id);
    if (ids.has(s.id)) bad.push(`${s.id} collides with a manual id`);
    if (s.category !== 'api') bad.push(`${s.id}: category ${s.category}`);
    if (!/^(GET|POST|PUT|DELETE|PATCH)$/.test(s.method || '')) bad.push(`${s.id}: method ${s.method}`);
    if (!/^\/api\//.test(s.path || '')) bad.push(`${s.id}: path ${s.path}`);
    if (!s.en || !s.en.title || !s.en.description) bad.push(`${s.id}: en text missing`);
    for (const p of s.params || []) if (!p.name || !/^[\w-]+$/.test(p.name) || !(p.en && p.en.description)) bad.push(`${s.id}: param ${JSON.stringify(p.name)}`);
  }
  assert.deepEqual(bad, []);
});

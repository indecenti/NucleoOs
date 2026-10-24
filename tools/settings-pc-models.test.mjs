// Settings ▸ AI ▸ "Models on your PC" (apps/settings/www/pc-models.js): the pure config edits behind the panel.
// They write ai.local.engines, which web/shell/ai-engines.js reads on every turn — so the shapes must stay exactly
// what loadLocalConfig / liveServers / localComplete expect. Run: node --test tools/settings-pc-models.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { chatModels, serverRows, addServer, removeServer, setServerEnabled, setTaskModel, setPrefer, TASKS } from '../apps/settings/www/pc-models.js';
import { loadLocalConfig, DEFAULT_SERVERS } from '../web/shell/ai-engines.js';
import { readFileSync as _rf } from 'node:fs';
// Settings is on the central engine now: its strings live in apps/settings/www/i18n.<lang>.json.
const STR = Object.fromEntries(['it', 'en', 'es', 'fr', 'de'].map((l) => {
  let s = _rf(new URL(`../apps/settings/www/i18n.${l}.json`, import.meta.url), 'utf8');
  if (s.charCodeAt(0) === 0xfeff) s = s.slice(1);
  return [l, JSON.parse(s)];
}));

const mem = () => { const m = new Map(); return { getItem: (k) => (m.has(k) ? m.get(k) : null), setItem: (k, v) => m.set(k, String(v)) }; };
const base = () => loadLocalConfig(mem());          // the engine layer's own default config

test('the panel starts from the engine layer default: four servers, enabled, no picks', () => {
  const c = base();
  assert.equal(c.enabled, true);
  assert.deepEqual(c.servers.map((s) => s.id), DEFAULT_SERVERS.map((s) => s.id));
  assert.deepEqual(serverRows(c, null).map((r) => r.status), ['unknown', 'unknown', 'unknown', 'unknown']);
});

test('serverRows joins the probe and keeps embedding-only models out of the pickers', () => {
  const c = setServerEnabled(base(), 'jan', false);
  const live = [
    { id: 'ollama', status: 'ok', models: [{ id: 'qwen3.5:9b', caps: { chat: true } }, { id: 'nomic-embed-text', caps: { embedding: true } }, { id: 'bge-m3' }] },
    { id: 'lmstudio', status: 'cors', models: [] },
    { id: 'llamacpp', status: 'down', models: [] },
  ];
  const rows = serverRows(c, live);
  assert.deepEqual(rows.map((r) => r.status), ['ok', 'cors', 'down', 'off']);
  assert.deepEqual(rows[0].models.map((m) => m.id), ['qwen3.5:9b']);
  assert.deepEqual(chatModels([{ id: 'x', caps: { embedding: true, chat: true } }]).map((m) => m.id), ['x']);   // a chat model that also embeds stays
});

test('addServer: Ollama at the root, OpenAI-compatible at /v1, refuses junk and duplicates, never mutates', () => {
  const c = base(), snap = JSON.stringify(c);
  const a = addServer(c, { kind: 'ollama', url: 'http://192.168.1.20:11434/api/' });
  assert.equal(a.error, ''); assert.equal(a.server.base, 'http://192.168.1.20:11434'); assert.equal(a.server.custom, true);
  const b = addServer(a.cfg, { kind: 'openai', url: 'http://gpu-box:8000' });
  assert.equal(b.server.base, 'http://gpu-box:8000/v1'); assert.equal(b.server.kind, 'openai');
  assert.equal(addServer(b.cfg, { kind: 'openai', url: 'http://gpu-box:8000/v1/' }).error, 'dup');
  assert.equal(addServer(c, { kind: 'ollama', url: 'http://localhost:11434' }).error, 'dup');           // the default Ollama
  for (const bad of ['', 'localhost:11434', 'ftp://x:21', 'javascript:alert(1)', 'http://user:pw@host:1']) assert.equal(addServer(c, { url: bad }).error, 'bad', bad);
  assert.equal(JSON.stringify(c), snap);
  const ids = new Set(b.cfg.servers.map((s) => s.id)); assert.equal(ids.size, b.cfg.servers.length);    // unique ids
});

test('removeServer drops only a user-added server, with its model picks; defaults are switched off instead', () => {
  const a = addServer(base(), { kind: 'ollama', url: 'http://10.0.0.5:11434' });
  let c = setTaskModel(a.cfg, a.server.id, 'chat', 'llama3.2:3b');
  c = removeServer(c, a.server.id);
  assert.ok(!c.servers.some((s) => s.id === a.server.id)); assert.ok(!(a.server.id in c.models));
  const d = removeServer(c, 'ollama');
  assert.ok(d.servers.some((s) => s.id === 'ollama'));
  assert.equal(setServerEnabled(d, 'ollama', false).servers.find((s) => s.id === 'ollama').enabled, false);
});

test('setTaskModel: one pick per task, all automatic = no entry (what localComplete reads as "rank the models")', () => {
  let c = setTaskModel(base(), 'ollama', 'code', 'qwen2.5-coder:7b');
  assert.deepEqual(c.models.ollama, { code: 'qwen2.5-coder:7b' });
  c = setTaskModel(c, 'ollama', 'agent', 'qwen3.5:9b');
  assert.deepEqual(c.models.ollama, { code: 'qwen2.5-coder:7b', agent: 'qwen3.5:9b' });
  c = setTaskModel(setTaskModel(c, 'ollama', 'code', ''), 'ollama', 'agent', '');
  assert.ok(!('ollama' in c.models));
  assert.deepEqual(TASKS, ['chat', 'code', 'agent']);
});

test('setPrefer: only "local" is stored; the default order leaves no key (ANIMA and the copilot read prefer === "local")', () => {
  assert.equal(setPrefer(base(), 'local').prefer, 'local');
  assert.ok(!('prefer' in setPrefer(setPrefer(base(), 'local'), 'cloud')));
});

test('every string the panel uses exists in all five languages', async () => {
  const { readFileSync } = await import('node:fs');
  const src = readFileSync(new URL('../apps/settings/www/pc-models.js', import.meta.url), 'utf8');
  const keys = new Set([...src.matchAll(/T\('([A-Za-z_]+)'\)/g)].map((m) => m[1]));
  for (const tk of TASKS) keys.add('pcTask_' + tk);
  keys.add('pcAddDup'); keys.add('pcAddBad');
  assert.ok(keys.size > 20);
  for (const l of ['it', 'en', 'es', 'fr', 'de']) for (const k of keys) assert.ok(typeof STR[l][k] === 'string' && STR[l][k], `${l}.${k}`);
});

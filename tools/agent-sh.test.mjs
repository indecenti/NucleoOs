// apps/agent/www/agent-sh.js — the agent's POSIX-like shell over the Cardputer's files, run through the REAL
// fsclient (makeFS) against an in-memory device that behaves like nucleo_fsapi (write/mkdir need a parent).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeFS } from '../apps/anima/www/fsclient.js';
import { createAgentShell, parse, plannedWrites } from '../apps/agent/www/agent-sh.js';

function fakeDevice(files = {}) {
  const dirs = new Set(['/', '/data']), store = new Map();
  const parent = (p) => p.slice(0, p.lastIndexOf('/')) || '/';
  const addDirs = (p) => { for (let d = parent(p); d && !dirs.has(d); d = parent(d)) { dirs.add(d); if (d === '/') break; } };
  for (const [p, c] of Object.entries(files)) { addDirs(p); store.set(p, c); }
  const res = (status, body = '') => ({ ok: status < 300, status, text: async () => String(body), json: async () => JSON.parse(body), arrayBuffer: async () => new TextEncoder().encode(String(body)).buffer });
  globalThis.fetch = async (url, { method = 'GET', body } = {}) => {
    const u = new URL(url, 'http://dev'); const op = u.pathname.split('/').pop(), p = u.searchParams.get('path');
    if (op === 'mkdir') { if (!dirs.has(parent(p))) return res(500, 'mkdir'); dirs.add(p); return res(200, '{"ok":true}'); }
    if (op === 'write') { if (!dirs.has(parent(p))) return res(500, 'open'); store.set(p, String(body)); return res(200, '{"ok":true}'); }
    if (op === 'read') return store.has(p) ? res(200, store.get(p)) : res(404, 'no file');
    if (op === 'delete') { if (store.delete(p)) return res(200, '{"ok":true}'); if (dirs.has(p) && ![...store.keys(), ...dirs].some((x) => x !== p && x.startsWith(p + '/'))) { dirs.delete(p); return res(200, '{"ok":true}'); } return res(404, 'no entry'); }
    if (op === 'move') { const f = u.searchParams.get('from'), t = u.searchParams.get('to'); if (!store.has(f)) return res(404, 'x'); store.set(t, store.get(f)); store.delete(f); return res(200, '{"ok":true}'); }
    if (op === 'list') {
      if (!dirs.has(p)) return res(404, 'no dir');
      const kids = [...store.keys()].filter((f) => parent(f) === p).map((f) => ({ name: f.slice(p.length + 1), type: 'file', size: store.get(f).length }))
        .concat([...dirs].filter((x) => x !== p && parent(x) === p).map((x) => ({ name: x.slice(p.length + 1), type: 'dir', size: 0 })));
      return res(200, JSON.stringify({ entries: kids }));
    }
    return res(404);
  };
  return { store, dirs };
}

const STATUS = { version: '0.4.0', uptime_s: 3720, free_heap: 26820, min_free_heap: 4344, largest_free_block: 8192, profile: 'web',
  storage: { mounted: true, total_bytes: 31998345216, free_bytes: 18714492928 }, battery: { pct: 100, mv: 4152 }, network: { time: 1790808000 } };

function shell(files, { confirm = async () => true } = {}) {
  const dev = fakeDevice(files);
  const fs = makeFS('/data/agent');
  const asked = [];
  const sh = createAgentShell({ fs, confirm: async (x) => { asked.push(x); return confirm(x); },
    device: { status: async () => STATUS, apps: async () => [{ id: 'calculator', name: 'Calcolatrice' }], open: async (x) => 'opened ' + (x.app || x.path) } });
  return { sh, dev, asked };
}

const FILES = {
  '/data/agent/notes/todo.md': '- latte\n- uova\n- pane\n',
  '/data/agent/src/app.js': 'function add(a, b) {\n  return a + b;\n}\n// TODO: tests\nexport { add };\n',
  '/data/agent/src/util.js': 'export const TODO = 1;\n',
  '/data/agent/readme.txt': 'hello\nworld\n',
};

test('the everyday read commands models already know', async () => {
  const { sh } = shell(FILES);
  assert.equal((await sh.run('ls')).out, 'notes/\nsrc/\nreadme.txt', 'directories first (fsclient order), marked with /');
  assert.equal((await sh.run('cat notes/todo.md')).out, '- latte\n- uova\n- pane\n');
  assert.equal((await sh.run('head -n 2 src/app.js')).out, 'function add(a, b) {\n  return a + b;');
  assert.equal((await sh.run('tail -1 src/app.js')).out, 'export { add };');
  assert.equal((await sh.run("sed -n '2,3p' src/app.js")).out, '  return a + b;\n}');
  assert.equal((await sh.run('wc -l notes/todo.md')).out, '3 notes/todo.md');
  assert.equal((await sh.run('grep -rn TODO src')).out, 'src/app.js:4:// TODO: tests\nsrc/util.js:1:export const TODO = 1;');
  assert.equal((await sh.run("find . -name '*.js'")).out, 'src/app.js\nsrc/util.js');
  assert.match((await sh.run('tree')).out, /├─ notes\/[\s\S]*├─ todo\.md/);
});

test('pipes, redirection and && / || behave like a shell', async () => {
  const { sh, dev } = shell(FILES);
  assert.equal((await sh.run('cat notes/todo.md | grep -v pane | wc -l')).out, '2');
  assert.equal((await sh.run('grep -c e notes/todo.md')).out, '2');
  await sh.run('echo caffè >> notes/todo.md');
  assert.equal(dev.store.get('/data/agent/notes/todo.md'), '- latte\n- uova\n- pane\ncaffè\n');
  await sh.run('ls src > listing.txt');
  assert.equal(dev.store.get('/data/agent/listing.txt'), 'app.js\nutil.js\n');
  assert.equal((await sh.run('grep nothing readme.txt && echo yes || echo no')).out, 'no');
  assert.equal((await sh.run('sort < notes/todo.md | head -1')).out, '- latte');
});

test('cd / pwd move inside the workspace, and nothing escapes it', async () => {
  const { sh } = shell({ ...FILES, '/system/config/settings.json': '{"secret":1}' });
  await sh.run('cd src');
  assert.equal((await sh.run('pwd')).out, '/src');
  assert.equal((await sh.run('cat app.js | head -1')).out, 'function add(a, b) {');
  const esc = await sh.run('cat ../../../system/config/settings.json');
  assert.equal(esc.code, 1); assert.doesNotMatch(esc.out, /secret/, 'the workspace boundary holds');
  await sh.run('cd /'); assert.equal((await sh.run('pwd')).out, '/');
});

test('changes are confirmed ONCE per line, as typed; declined means nothing ran; rm is flagged destructive', async () => {
  const { sh, dev, asked } = shell(FILES);
  const r = await sh.run('mkdir -p proj/docs && touch proj/docs/a.md && cp readme.txt proj/ && mv proj/readme.txt proj/r.txt');
  assert.equal(r.code, 0, r.out);
  assert.equal(asked.length, 1, 'one confirmation for the whole line');
  assert.deepEqual(asked[0].writes.map((w) => w.op), ['mkdir', 'touch', 'cp', 'mv']);
  assert.equal(dev.store.get('/data/agent/proj/r.txt'), 'hello\nworld\n');
  assert.equal(dev.store.has('/data/agent/proj/docs/a.md'), true);
  await sh.run('rm -r proj');
  assert.equal(asked[1].destructive, true);
  assert.equal([...dev.store.keys()].some((k) => k.startsWith('/data/agent/proj')), false, 'rm -r removed the tree');
  const no = shell(FILES, { confirm: async () => false });
  const d = await no.sh.run('rm readme.txt');
  assert.match(d.out, /declined/); assert.equal(no.dev.store.has('/data/agent/readme.txt'), true);
  assert.equal(no.asked.length, 1);
  assert.equal((await no.sh.run('cat readme.txt')).code, 0, 'read-only lines never ask');
  assert.equal(no.asked.length, 1);
});

test('the Cardputer itself: df, free, uptime, uname, apps, open', async () => {
  const { sh } = shell(FILES);
  assert.match((await sh.run('df -h')).out, /sd\s+32\.0G\s+13\.3G\s+18\.7G\s+42%/);
  assert.match((await sh.run('free')).out, /heap free: 26 KB/);
  assert.equal((await sh.run('uptime')).out, 'up 1h 2m, battery 100%');
  assert.match((await sh.run('uname -a')).out, /NucleoOS 0\.4\.0 esp32s3 \(web profile\)/);
  assert.equal((await sh.run('apps | grep calc')).out, 'calculator\tCalcolatrice');
  assert.equal((await sh.run('open calculator')).out, 'opened calculator');
});

test('helpful errors and bounded output', async () => {
  const big = { '/data/agent/big.txt': Array.from({ length: 4000 }, (_, i) => 'line ' + i).join('\n') };
  const { sh } = shell(big);
  const r = await sh.run('cat big.txt');
  assert.ok(r.out.length < 13 * 1024); assert.match(r.out, /output truncated .*sed -n/);
  assert.match((await sh.run('vim big.txt')).out, /command not found\. Available: .*grep/);
  assert.equal((await sh.run('cat "unterminated')).code, 2);
  assert.match((await sh.run("sed -i 's/a/b/' big.txt")).out, /edit_file/);
  assert.match((await sh.run('cat nope.txt')).out, /No such file/);
});

test('echo -e / -n and printf write real multi-line files', async () => {
  // what qwen3.5:9b actually ran on the ADV (2026-10-01) — before, the file got "-e …\n…" literally
  const { sh, dev } = shell(FILES);
  await sh.run('mkdir -p demo && echo -e "riga uno\\nriga due\\nriga tre" > demo/README.md');
  assert.equal(dev.store.get('/data/agent/demo/README.md'), 'riga uno\nriga due\nriga tre\n');
  await sh.run('echo -n senza-a-capo > a.txt');
  assert.equal(dev.store.get('/data/agent/a.txt'), 'senza-a-capo');
  await sh.run('printf "%s: %d\\n" totale 42 > b.txt');
  assert.equal(dev.store.get('/data/agent/b.txt'), 'totale: 42\n');
  assert.equal((await sh.run('echo "a\\nb"')).out, 'a\\nb', 'plain echo leaves backslashes alone');
});

test('plannedWrites sees through pipes and redirections', () => {
  assert.deepEqual(plannedWrites(parse('cat a | grep x > out.txt')), [{ op: 'write', path: 'out.txt' }]);
  assert.deepEqual(plannedWrites(parse('ls; cat a')), []);
});

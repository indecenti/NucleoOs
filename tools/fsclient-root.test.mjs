// fsclient on a card where the workspace root does not exist yet (apps/anima/www/fsclient.js).
// Regression: the agent's default sandbox /data/agent is created by nobody, and the device's /api/fs/write
// does not create parent folders, so every FIRST agent write ("crea un file spesa.md …") failed with
// "500 open" — mkdirp only created folders BELOW the root, and write skipped it for a file in the root.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { makeFS } from '../apps/anima/www/fsclient.js';

// In-memory SD that behaves like nucleo_fsapi: write/mkdir need an existing parent; list 404s a missing dir.
function fakeDevice(dirs = ['/', '/data']) {
  const d = new Set(dirs), files = new Map(), calls = [];
  const parent = (p) => p.slice(0, p.lastIndexOf('/')) || '/';
  const res = (status, body = '') => ({ ok: status < 300, status, text: async () => String(body), json: async () => JSON.parse(body), arrayBuffer: async () => new TextEncoder().encode(String(body)).buffer });
  globalThis.fetch = async (url, { method = 'GET', body } = {}) => {
    const u = new URL(url, 'http://dev');
    const op = u.pathname.split('/').pop(), p = u.searchParams.get('path');
    calls.push(op + ' ' + p);
    if (op === 'mkdir') { if (!d.has(parent(p))) return res(500, 'mkdir'); d.add(p); return res(200, '{"ok":true}'); }
    if (op === 'write') { if (!d.has(parent(p))) return res(500, 'open'); files.set(p, String(body)); return res(200, '{"ok":true}'); }
    if (op === 'read') return files.has(p) ? res(200, files.get(p)) : res(404, 'no file');
    if (op === 'list') {
      if (!d.has(p)) return res(404, 'no dir');
      const kids = [...files.keys()].filter((f) => parent(f) === p).map((f) => ({ name: f.slice(p.length + 1), type: 'file', size: files.get(f).length }))
        .concat([...d].filter((x) => x !== p && parent(x) === p).map((x) => ({ name: x.slice(p.length + 1), type: 'dir', size: 0 })));
      return res(200, JSON.stringify({ entries: kids }));
    }
    if (op === 'move') { const f = u.searchParams.get('from'), t = u.searchParams.get('to'); if (!d.has(parent(t))) return res(500, 'rename'); files.set(t, files.get(f)); files.delete(f); return res(200, '{"ok":true}'); }
    return res(404);
  };
  return { dirs: d, files, calls };
}

test('first write into a root that does not exist yet creates the root, then the file', async () => {
  const dev = fakeDevice();
  const fs = makeFS('/data/agent');
  const r = await fs.write('spesa.md', '- latte\n- uova\n');
  assert.equal(r.ok, true, JSON.stringify(r));
  assert.ok(dev.dirs.has('/data/agent'));
  assert.equal(dev.files.get('/data/agent/spesa.md'), '- latte\n- uova\n');
});

test('a nested write on a fresh card creates the whole chain', async () => {
  const dev = fakeDevice();
  const fs = makeFS('/data/agent');
  const r = await fs.write('progetti/app/index.html', '<!doctype html>');
  assert.equal(r.ok, true, JSON.stringify(r));
  for (const x of ['/data/agent', '/data/agent/progetti', '/data/agent/progetti/app']) assert.ok(dev.dirs.has(x), x);
});

test('the root chain is created once per root, and again after setRoot', async () => {
  const dev = fakeDevice();
  const fs = makeFS('/data/agent');
  await fs.write('a.txt', 'a'); await fs.write('b.txt', 'b');
  assert.equal(dev.calls.filter((c) => c === 'mkdir /data/agent').length, 1);
  fs.setRoot('/data/other');
  const r = await fs.write('c.txt', 'c');
  assert.equal(r.ok, true, JSON.stringify(r));
  assert.ok(dev.dirs.has('/data/other'));
});

test('mkdir and move into a missing root work too', async () => {
  const dev = fakeDevice();
  const fs = makeFS('/data/agent');
  assert.equal((await fs.mkdir('docs')).ok, true);
  assert.ok(dev.dirs.has('/data/agent/docs'));
  await fs.write('docs/x.md', 'x');
  const mv = await fs.move('docs/x.md', 'x.md');
  assert.equal(mv.ok, true, JSON.stringify(mv));
  assert.equal(dev.files.get('/data/agent/x.md'), 'x');
});

// app-ops-check.mjs — INTEGRATION test of the app-lifecycle orchestration (apps/agent/www/app-ops.js)
// against an IN-MEMORY device. Unlike app-publish-check (pure units), this drives the WHOLE publish/scaffold/
// manage flow and locks the anti-destructive INVARIANTS end-to-end: a null registry read never wipes other
// apps, the lint gate blocks BEFORE any write, traversal/size are refused before writing, and the registry
// is always written LAST. The device is mocked, so it runs on the host with no board.
//   node tools/anima-host/app-ops-check.mjs
import { orchestrateScaffold, orchestratePublish, orchestrateManage } from '../../apps/agent/www/app-ops.js';
import { buildManifest, MAX_APP_BYTES } from '../../apps/agent/www/app-publish.js';

let pass = 0, fail = 0; const fails = [];
const ok = (name, cond, detail) => { if (cond) { pass++; } else { fail++; fails.push(name + (detail ? ' — ' + detail : '')); } };

// In-memory io: a workspace store (ws) + a privileged store (sys). Every write (ws or sys) is logged in
// order, so we can prove "registry written LAST" and "zero writes on refusal".
function makeMemIo({ ws = {}, sys = {}, regUnreadable = false } = {}) {
  const writes = [];
  const io = {
    _ws: ws, _sys: sys, _writes: writes,
    async readWs(rel) { return (rel in ws) ? { ok: true, content: ws[rel] } : { ok: false, error: 'not-found' }; },
    async readWsPlain(rel) { return io.readWs(rel); },
    async treeWs(dir) {
      const pre = dir.endsWith('/') ? dir : dir + '/';
      const entries = Object.keys(ws).filter((p) => p.startsWith(pre)).map((p) => ({ path: p, type: 'file', size: Buffer.byteLength(ws[p] || '', 'utf8') }));
      return { ok: true, entries };
    },
    async writeWs(rel, content) { writes.push({ kind: 'ws', path: rel }); ws[rel] = content; return { ok: true }; },
    async sysReadJson(abs) { if (regUnreadable && /registry/.test(abs)) return null; return (abs in sys) ? JSON.parse(sys[abs]) : null; },
    async sysMkdir() {},
    async sysWrite(abs, content) { writes.push({ kind: 'sys', path: abs }); sys[abs] = content; return { ok: true }; },
    checkSyntax: (code) => (/BROKEN/.test(code) ? { ok: false, line: 1, error: 'parse' } : { ok: true }),
    wait: async () => {},
  };
  return io;
}
const REG = (apps) => JSON.stringify({ schema: 1, installed: apps });
const validManifest = (id, name) => JSON.stringify(buildManifest({ id, name: name || id }));
const stagedApp = (id) => ({ [id + '/manifest.json']: validManifest(id), [id + '/www/index.html']: '<h1>ok</h1>' });

/* ---- (a) registry illeggibile → NESSUNA scrittura + errore (no-wipe) ---- */
await (async () => {
  const io = makeMemIo({ ws: stagedApp('todo'), sys: { '/system/registry/apps.json': REG([{ id: 'other', created_by: 'agent', enabled: true }]) }, regUnreadable: true });
  const r = await orchestratePublish(io, { id: 'todo' });
  ok('unreadable registry → refuse', r.ok === false && r.error === 'registry-unreadable');
  ok('unreadable registry → ZERO writes (no wipe)', io._writes.length === 0);
  ok('unreadable registry → other app intact', JSON.parse(io._sys['/system/registry/apps.json']).installed.some((a) => a.id === 'other'));
})();

/* ---- (b) JS/JSON rotto → nessuna scrittura (lint blocca prima) ---- */
await (async () => {
  const io = makeMemIo({ ws: { 'bad/manifest.json': validManifest('bad'), 'bad/www/index.html': '<h1>x</h1><script>BROKEN</script>' }, sys: { '/system/registry/apps.json': REG([]) } });
  const r = await orchestratePublish(io, { id: 'bad' });
  ok('broken inline JS → refuse (lint)', r.ok === false && r.error === 'lint');
  ok('lint refusal → ZERO writes', io._writes.length === 0);
  const io2 = makeMemIo({ ws: { 'b2/manifest.json': validManifest('b2'), 'b2/www/index.html': '<h1>x</h1>', 'b2/www/i18n.it.json': '{bad json' }, sys: { '/system/registry/apps.json': REG([]) } });
  const r2 = await orchestratePublish(io2, { id: 'b2' });
  ok('broken JSON file → refuse (lint)', r2.ok === false && r2.error === 'lint' && io2._writes.length === 0);
})();

/* ---- (c) happy path → manifest+files written, registry LAST, others preserved ---- */
await (async () => {
  const io = makeMemIo({ ws: { 'todo/manifest.json': validManifest('todo', 'Todo'), 'todo/www/index.html': '<h1>x</h1>', 'todo/www/js/app.js': 'const x = 1;' },
    sys: { '/system/registry/apps.json': REG([{ id: 'other', created_by: 'agent', enabled: true }]) } });
  const r = await orchestratePublish(io, { id: 'todo' });
  ok('happy path ok + copied=2', r.ok === true && r.copied === 2);
  const order = io._writes.map((w) => w.path);
  ok('registry written LAST', order[order.length - 1] === '/system/registry/apps.json');
  ok('manifest+files written before registry', order.indexOf('/apps/todo/manifest.json') >= 0 && order.indexOf('/apps/todo/manifest.json') < order.length - 1 && order.includes('/apps/todo/www/js/app.js'));
  const reg = JSON.parse(io._sys['/system/registry/apps.json']);
  ok('registry preserves other + adds todo', reg.installed.some((a) => a.id === 'other') && reg.installed.some((a) => a.id === 'todo' && a.created_by === 'agent'));
  ok('app files materialized under /apps/todo/www', io._sys['/apps/todo/www/js/app.js'] === 'const x = 1;');
})();

/* ---- (d) traversal staged path → refuse, no writes ---- */
await (async () => {
  const io = makeMemIo({ ws: { 'trav/manifest.json': validManifest('trav'), 'trav/www/index.html': '<h1>x</h1>' }, sys: { '/system/registry/apps.json': REG([]) } });
  io.treeWs = async () => ({ ok: true, entries: [{ path: 'trav/www/../evil.js', type: 'file', size: 10 }] });
  const r = await orchestratePublish(io, { id: 'trav' });
  ok('traversal path → refuse', r.ok === false && r.error === 'unsafe-path');
  ok('traversal → ZERO writes', io._writes.length === 0);
})();

/* ---- (e) size cap exceeded → refuse, no writes ---- */
await (async () => {
  const io = makeMemIo({ ws: { 'big/manifest.json': validManifest('big'), 'big/www/index.html': '<h1>x</h1>' }, sys: { '/system/registry/apps.json': REG([]) } });
  io.treeWs = async () => ({ ok: true, entries: [{ path: 'big/www/index.html', type: 'file', size: MAX_APP_BYTES + 1 }] });
  const r = await orchestratePublish(io, { id: 'big' });
  ok('over size cap → refuse', r.ok === false && r.error === 'too-large');
  ok('over size cap → ZERO writes', io._writes.length === 0);
})();

/* ---- (f) manage: only agent apps, never system; not-found; already; unreadable ---- */
await (async () => {
  const sys = { '/system/registry/apps.json': REG([{ id: 'sysapp', enabled: true }, { id: 'mine', created_by: 'agent', enabled: true }]) };
  const io = makeMemIo({ sys });
  const r1 = await orchestrateManage(io, { id: 'sysapp', action: 'disable' });
  ok('manage refuses a SYSTEM app', r1.ok === false && r1.error === 'system-id' && io._writes.length === 0);
  const r2 = await orchestrateManage(io, { id: 'ghost', action: 'disable' });
  ok('manage refuses unknown id', r2.ok === false && r2.error === 'not-found');
  const r3 = await orchestrateManage(io, { id: 'mine', action: 'disable' });
  ok('manage disables an agent app', r3.ok === true && JSON.parse(io._sys['/system/registry/apps.json']).installed.find((a) => a.id === 'mine').enabled === false);
  const r4 = await orchestrateManage(io, { id: 'mine', action: 'disable' });
  ok('manage no-op when already in state', r4.ok === true && r4.noop === true);
  const io2 = makeMemIo({ sys: { '/system/registry/apps.json': REG([{ id: 'mine', created_by: 'agent', enabled: true }]) }, regUnreadable: true });
  ok('manage refuses unreadable registry', (await orchestrateManage(io2, { id: 'mine', action: 'disable' })).error === 'registry-unreadable' && io2._writes.length === 0);
  ok('manage bad action', (await orchestrateManage(io, { id: 'mine', action: 'frob' })).error === 'bad-action');
})();

/* ---- (g) scaffold creates the expected files; refuses a system id ---- */
await (async () => {
  const io = makeMemIo({ sys: {} });
  const r = await orchestrateScaffold(io, { input: { name: 'My Notes', kind: 'list', category: 'productivity' } });
  ok('scaffold ok', r.ok === true && r.id === 'my-notes');
  ok('scaffold writes 5 files', ['my-notes/manifest.json', 'my-notes/www/index.html', 'my-notes/www/icon.svg', 'my-notes/www/i18n.en.json', 'my-notes/www/i18n.it.json'].every((p) => p in io._ws));
  const mf = JSON.parse(io._ws['my-notes/manifest.json']);
  ok('scaffold manifest sane', mf.id === 'my-notes' && mf.category === 'productivity' && mf.created_by === 'agent');
  ok('scaffold i18n valid + kind body', (() => { try { JSON.parse(io._ws['my-notes/www/i18n.it.json']); return io._ws['my-notes/www/index.html'].includes('id="list"'); } catch { return false; } })());
  const io2 = makeMemIo({ sys: { '/system/registry/apps.json': REG([{ id: 'calculator', enabled: true }]) } });
  const r2 = await orchestrateScaffold(io2, { input: { name: 'Calculator' } });
  ok('scaffold refuses a system id', r2.ok === false && r2.error === 'system-id' && io2._writes.length === 0);
})();

/* ---- (h) advisory review: note appended; failure swallowed; Stop aborts (rethrow, zero writes) ---- */
await (async () => {
  const base = () => makeMemIo({ ws: { 'rv/manifest.json': validManifest('rv'), 'rv/www/index.html': '<h1>x</h1>' }, sys: { '/system/registry/apps.json': REG([]) } });
  const io1 = base(); io1.reviewApp = async () => '\n⚠ Revisore (Grok): manca #go';
  const r1 = await orchestratePublish(io1, { id: 'rv' });
  ok('review note appended to publish message', r1.ok === true && r1.message.includes('⚠ Revisore'));
  const io2 = base(); io2.reviewApp = async () => { throw new Error('boom'); };
  const r2 = await orchestratePublish(io2, { id: 'rv' });
  ok('review failure (non-stopped) → publish still ok, no note', r2.ok === true && !r2.message.includes('⚠ Revisore'));
  const io3 = base(); io3.reviewApp = async () => { throw new Error('stopped'); };
  let stopped = false; try { await orchestratePublish(io3, { id: 'rv' }); } catch (e) { stopped = String(e && e.message) === 'stopped'; }
  ok('review Stop → publish aborts (rethrow) with ZERO writes', stopped && io3._writes.length === 0);
})();

/* ---- (i) privileged write failures: manifest fail aborts (no registry); a www file fail → partial ---- */
await (async () => {
  const io = makeMemIo({ ws: { 'pw/manifest.json': validManifest('pw'), 'pw/www/index.html': '<h1>x</h1>' }, sys: { '/system/registry/apps.json': REG([]) } });
  const real = io.sysWrite.bind(io);
  io.sysWrite = async (abs, c) => (abs.endsWith('/manifest.json') ? { ok: false, error: 'http-500' } : real(abs, c));
  const r = await orchestratePublish(io, { id: 'pw' });
  ok('manifest write fail → sys-write-manifest, registry untouched', r.ok === false && r.error === 'sys-write-manifest' && !io._sys['/system/registry/apps.json'].includes('"pw"'));
  const io2 = makeMemIo({ ws: { 'pw2/manifest.json': validManifest('pw2'), 'pw2/www/index.html': '<h1>x</h1>', 'pw2/www/a.js': 'const a = 1;' }, sys: { '/system/registry/apps.json': REG([]) } });
  const real2 = io2.sysWrite.bind(io2);
  io2.sysWrite = async (abs, c) => (abs.endsWith('/a.js') ? { ok: false, error: 'http-500' } : real2(abs, c));
  const r2 = await orchestratePublish(io2, { id: 'pw2' });
  ok('partial file fail → still ok, copied counts only successes', r2.ok === true && r2.copied === 1);
  ok('partial file fail → app still registered (documented limitation)', JSON.parse(io2._sys['/system/registry/apps.json']).installed.some((a) => a.id === 'pw2'));
})();

/* ---- (j) registry retry: null on the first read, valid on the second → publish succeeds ---- */
await (async () => {
  const io = makeMemIo({ ws: { 'rt/manifest.json': validManifest('rt'), 'rt/www/index.html': '<h1>x</h1>' }, sys: { '/system/registry/apps.json': REG([]) } });
  let n = 0; const real = io.sysReadJson.bind(io);
  io.sysReadJson = async (abs) => { if (/registry/.test(abs)) { n++; if (n === 1) return null; } return real(abs); };
  const r = await orchestratePublish(io, { id: 'rt' });
  ok('registry retry (null→valid) recovers and publishes', r.ok === true && n >= 2);
})();

/* ---- (k) lifecycle extras: scaffold over an agent id; manage enable; explicit bad-id ---- */
await (async () => {
  const io = makeMemIo({ sys: { '/system/registry/apps.json': REG([{ id: 'my-notes', created_by: 'agent', enabled: true }]) } });
  ok('scaffold over an agent-owned id proceeds', (await orchestrateScaffold(io, { input: { name: 'My Notes', kind: 'blank' } })).ok === true && 'my-notes/manifest.json' in io._ws);
  const io2 = makeMemIo({ sys: { '/system/registry/apps.json': REG([{ id: 'mine', created_by: 'agent', enabled: false }]) } });
  const r2 = await orchestrateManage(io2, { id: 'mine', action: 'enable' });
  ok('manage enable of a disabled agent app', r2.ok === true && !r2.noop && JSON.parse(io2._sys['/system/registry/apps.json']).installed[0].enabled === true);
  const io3 = makeMemIo({ sys: {} });
  ok('publish bad-id explicit', (await orchestratePublish(io3, { id: 'x' })).error === 'bad-id');
  ok('scaffold bad-id explicit', (await orchestrateScaffold(io3, { input: { name: '!' } })).error === 'bad-id');
})();

console.log(`\napp-ops-check: ${pass} passed, ${fail} failed`);
if (fail) { console.log('FAILED:\n - ' + fails.join('\n - ')); process.exit(1); }
console.log('all green ✓');

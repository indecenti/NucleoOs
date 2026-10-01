// web/shell/seq-import.js — an app's module graph fetched ONE FILE AT A TIME and linked in the browser (no
// service worker on http://, and a parallel import() burst lost modules on the Cardputer's 4-socket httpd).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { seqImport, staticSpecifiers, rewriteModule, planGraph } from '../web/shell/seq-import.js';

function tree(files) {
  const dir = mkdtempSync(join(tmpdir(), 'seqimp-'));
  for (const [p, c] of Object.entries(files)) { mkdirSync(join(dir, p, '..'), { recursive: true }); writeFileSync(join(dir, p), c); }
  return pathToFileURL(dir + '/').href;
}

test('specifiers and rewriting touch import lines only', () => {
  const src = "import { a } from './a.js';\nimport {\n  b,\n  c,\n} from '../b.js';\nexport { d } from \"/d.js\";\nimport './side.js';\nconst s = 'import x from \"no\"';\nconst m = await import('./lazy.mjs');\n";
  assert.deepEqual(staticSpecifiers(src), ['./a.js', '../b.js', '/d.js', './side.js']);
  const out = rewriteModule(src, 'http://dev/apps/x/m.js', (u) => 'blob:' + u);
  assert.match(out, /from 'blob:http:\/\/dev\/apps\/x\/a\.js'/);
  assert.match(out, /from 'blob:http:\/\/dev\/apps\/b\.js'/);
  assert.match(out, /const s = 'import x from "no"'/, 'a string that looks like an import is left alone');
  assert.match(out, /import\('http:\/\/dev\/apps\/x\/lazy\.mjs'\)/, 'relative dynamic import made absolute');
});

test('the graph is fetched sequentially, linked deps-first, and shared modules keep their real URL', async () => {
  globalThis.__seqCount = 0;
  const base = tree({
    'shared.js': 'globalThis.__seqCount++; export const shared = {};',
    'apps/agent/runtime.js': "import { tool } from './tools.js';\nimport { shared } from '../../shared.js';\nexport const run = () => tool() + 1;\nexport { shared };\nexport const lazy = () => import('./lazy.js');",
    'apps/agent/tools.js': "import { util } from '../code/util.js';\nexport const tool = () => util() * 2;",
    'apps/code/util.js': 'export const util = () => 20;',
    'apps/agent/lazy.js': 'export const ok = true;',
  });
  const order = []; let inflight = 0, peak = 0;
  const fetchText = async (u) => { inflight++; peak = Math.max(peak, inflight); order.push(u.slice(base.length)); await new Promise((r) => setTimeout(r, 5)); inflight--; const { readFileSync } = await import('node:fs'); return readFileSync(new URL(u), 'utf8'); };
  const m = await seqImport('apps/agent/runtime.js', { base, own: ['apps/agent/', 'apps/code/'], fetchText });
  assert.equal(m.run(), 41);
  assert.equal(peak, 1, 'never two device fetches at once');
  assert.deepEqual(order, ['apps/agent/runtime.js', 'apps/agent/tools.js', 'apps/code/util.js']);
  const real = await import(base + 'shared.js');
  assert.equal(m.shared, real.shared, 'the shared module is the SAME instance as the rest of the page');
  assert.equal(globalThis.__seqCount, 1);
  assert.equal((await m.lazy()).ok, true, 'a relative dynamic import still resolves');
  const plan = await planGraph(base + 'apps/agent/runtime.js', { own: [base + 'apps/'], fetchText });
  assert.deepEqual(plan.order.map((u) => u.slice(base.length)), ['apps/code/util.js', 'apps/agent/tools.js', 'apps/agent/runtime.js']);
});

test('a cycle or a fetch error falls back to the plain import()', async () => {
  const base = tree({ 'c/a.js': "import { b } from './b.js'; export const a = 1;", 'c/b.js': "import { a } from './a.js'; export const b = 2;" });
  let plain = null;
  const m = await seqImport('c/a.js', { base, own: ['c/'], fetchText: async (u) => (await import('node:fs')).readFileSync(new URL(u), 'utf8'), importer: (u) => { plain = u; return import(u); } });
  assert.equal(plain, base + 'c/a.js'); assert.equal(m.a, 1);
  let p2 = null;
  await seqImport('c/b.js', { base, own: ['c/'], fetchText: async () => { throw new Error('reset'); }, importer: (u) => { p2 = u; return { ok: 1 }; } });
  assert.equal(p2, base + 'c/b.js');
});

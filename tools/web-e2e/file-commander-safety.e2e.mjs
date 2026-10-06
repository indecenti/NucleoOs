// Browser E2E — File Commander must never lose a file when a step of a delete / move / copy / restore
// fails. Every case here was a real data-loss path: the app read or wrote without checking the
// response and then deleted the original anyway (or wrote an error body into a destination, truncated
// an existing file, or wiped the Recycle Bin index after a failed read).
//
// Faults: the simulator's /api/_sim/fault matches a whole route (every /api/fs/write), so a fault on
// ONE path is injected in the page instead: window.fetch is wrapped and answers a chosen status for an
// exact (op, path) pair, exactly as the device would (e.g. 404 "no entry" for a non-empty folder).
import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = 45000;
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const en = { ...rd('../../web/shell/i18n/core.en.json'), ...rd('../../apps/file-commander/www/i18n.en.json') };
const plural = (v, n) => (v == null ? String(v) : typeof v === 'string' ? v : (n === 1 ? v.one : v.other)).replace('{count}', String(n));

const TRASH_ITEM = { orig: '/data/old.txt', trash: '/system/.trash/1_1_old.txt', name: 'old.txt', ts: 1 };
const SEED = {
  '/system/.trash/1_1_old.txt': 'old body',
  '/data/t1/a.txt': 'alpha',                       // trash: write of the trash copy fails
  '/data/t2/b.txt': 'bravo',                       // trash: read of the original fails
  '/data/t3/c.txt': 'charlie',                     // undo: restore write fails
  '/data/px/src/d.txt': 'delta',                   // cut → paste: upload fails
  '/data/px/dst/.keep': '',
  '/data/pm/src/m1.txt': 'mike one',              // cut → paste of two: only the landed one moves
  '/data/pm/src/m2.txt': 'mike two',
  '/data/pm/dst/.keep': '',
  '/data/pf/src/e.txt': 'echo',                    // OS-clipboard cut → paste: copy fails
  '/data/pf/dst/.keep': '',
  '/data/nf/untitled.txt': 'keep me',              // new file must not truncate
  '/data/fd/full/f.txt': 'foxtrot',                // non-empty folder delete
  '/data/fd2/full/f.txt': 'foxtrot',
  '/data/dc/g.txt': 'golf',                        // drag-copy with a failed read
  '/data/dc/into/.keep': '',
  '/data/ti/h.txt': 'hotel',                       // trash index unreadable
  '/data/tz/i.txt': 'india',                       // trash index answered "{}" (firmware: missing file)
  '/data/te/j.txt': 'juliett',                     // trash index is a 0-byte file
  '/data/tc/k.txt': 'kilo',                        // trash index is corrupt
};

// Wrap window.fetch so an exact /api/fs/<op>?path=<path> answers `status` (optionally `times` times).
const FAULTS = `(() => {
  if (window.__ff) return true;
  window.__ff = [];
  const real = window.fetch.bind(window);
  window.fetch = (u, o) => {
    const s = String(u);
    const i = window.__ff.findIndex((r) => s === '/api/fs/' + r.op + '?path=' + encodeURIComponent(r.path));
    if (i >= 0) {
      const r = window.__ff[i];
      if (r.times && --r.times === 0) window.__ff.splice(i, 1);
      return Promise.resolve(new Response(r.body || 'sim fault', { status: r.status }));
    }
    return real(u, o);
  };
  return true;
})()`;
const fault = (page, op, path, status, extra = {}) =>
  page.eval(`${FAULTS}, window.__ff.push(${JSON.stringify({ op, path, status, ...extra })}), true`);

async function openFc(browser, sim, path) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/file-commander/?path=${encodeURIComponent(path)}`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  assert.ok(await page.waitFor(`window._fc && document.querySelectorAll('#list li .name').length > 0`, { timeout: T }), 'File Commander listed ' + path);
  await page.eval(FAULTS);
  return page;
}
const rowJs = (name) => `[...document.querySelectorAll('#list li')].find((li) => li.querySelector('.name') && li.querySelector('.name').textContent === ${JSON.stringify(name)})`;
const select = (page, name) => page.eval(`${rowJs(name)}.click(), true`);
const shortcut = (page, action) => page.eval(`window.postMessage({ type: 'os-shortcut', action: ${JSON.stringify(action)} }, '*'), true`);
const toastText = (page) => page.eval(`(document.getElementById('fc-toast') || {}).textContent || ''`);
const waitToast = (page, text) => page.waitFor(`(document.getElementById('fc-toast') || {}).textContent === ${JSON.stringify(text)}`, { timeout: T });
const settle = async (page) => { await page.networkIdle({ quiet: 800, timeout: T }); await sleep(300); };

test('file-commander: a failed step never costs the user a file', { skip }, async (t) => {
  const sim = await startSim({ seed: { ...SEED, '/system/config/trash.json': { items: [TRASH_ITEM] } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const onSd = (p) => existsSync(join(sim.sd, p.replace(/^\//, '')));
  const trashDb = async () => JSON.parse(await sim.readSd('/system/config/trash.json'));

  await t.test('trash: a refused write of the Recycle Bin copy keeps the original', async () => {
    const page = await openFc(browser, sim, '/data/t1');
    await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 500 });
    try {
      await select(page, 'a.txt');
      await shortcut(page, 'delete');
      await settle(page);
    } finally { await sim.control('/api/_sim/fault', { clear: true }); }
    assert.ok(onSd('/data/t1/a.txt'), 'a.txt was deleted although its Recycle Bin copy was never written');
    assert.equal(await sim.readSd('/data/t1/a.txt'), 'alpha');
    assert.equal(await toastText(page), en.err_trash_failed.replace('{name}', 'a.txt'), 'the user is told the delete did not happen');
  });

  await t.test('trash: a failed read of the original keeps it (and never trashes an error body)', async () => {
    const page = await openFc(browser, sim, '/data/t2');
    await fault(page, 'read', '/data/t2/b.txt', 500);
    await select(page, 'b.txt');
    await shortcut(page, 'delete');
    await settle(page);
    assert.ok(onSd('/data/t2/b.txt'), 'b.txt was deleted although it could not be read into the Recycle Bin');
    assert.ok(!(await trashDb()).items.some((i) => i.orig === '/data/t2/b.txt'), 'no Recycle Bin entry for a copy that was never made');
    assert.equal(await toastText(page), en.err_trash_failed.replace('{name}', 'b.txt'));
  });

  await t.test('undo: a refused restore keeps the Recycle Bin copy and its index entry', async () => {
    const page = await openFc(browser, sim, '/data/t3');
    await select(page, 'c.txt');
    await shortcut(page, 'delete');
    assert.ok(await page.waitFor(`!${rowJs('c.txt')}`, { timeout: T }), 'c.txt went to the Recycle Bin');
    await settle(page);
    const entry = (await trashDb()).items.find((i) => i.orig === '/data/t3/c.txt');
    assert.ok(entry && onSd(entry.trash), 'the trash copy exists');
    await fault(page, 'write', '/data/t3/c.txt', 500);
    await shortcut(page, 'undo');
    await settle(page);
    assert.ok(onSd(entry.trash), 'the trash copy was deleted although the restore write failed — the file is gone');
    assert.ok((await trashDb()).items.some((i) => i.trash === entry.trash), 'the Recycle Bin entry was dropped');
    assert.equal(await toastText(page), en.err_restore_failed.replace('{name}', 'c.txt'));
  });

  await t.test('cut → paste: a failed upload keeps the source', async () => {
    const page = await openFc(browser, sim, '/data/px/src');
    await select(page, 'd.txt');
    await shortcut(page, 'cut');
    await page.eval(`document.getElementById('up').click(), true`);
    assert.ok(await page.waitFor(`!!${rowJs('dst')}`, { timeout: T }));
    await page.eval(`${rowJs('dst')}.ondblclick(), true`);
    assert.ok(await page.waitFor(`window._fc.state().cwd === '/data/px/dst'`, { timeout: T }));
    await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 500, times: 1 });   // only the upload
    try {
      await shortcut(page, 'paste');
      assert.ok(await page.waitFor(`/1/.test(document.getElementById('up-title').textContent) && document.querySelector('#up-body .uitem.err')`, { timeout: T }), 'the upload failed');
      await settle(page);
    } finally { await sim.control('/api/_sim/fault', { clear: true }); }
    assert.ok(!onSd('/data/px/dst/d.txt'), 'nothing landed in the destination');
    assert.ok(onSd('/data/px/src/d.txt'), 'the source of a failed move was deleted');
    assert.equal(await toastText(page), plural(en.err_move_kept, 1));
  });

  await t.test('cut → paste of two, one upload fails: only the landed one is moved', async () => {
    const page = await openFc(browser, sim, '/data/pm/src');
    await page.eval(`window.postMessage({ type: 'os-shortcut', action: 'selectAll' }, '*'), true`);
    assert.ok(await page.waitFor(`document.querySelectorAll('#list li.sel').length === 2`, { timeout: T }));
    await shortcut(page, 'cut');
    await page.eval(`document.getElementById('up').click(), true`);
    assert.ok(await page.waitFor(`!!${rowJs('dst')}`, { timeout: T }));
    await page.eval(`${rowJs('dst')}.ondblclick(), true`);
    assert.ok(await page.waitFor(`window._fc.state().cwd === '/data/pm/dst'`, { timeout: T }));
    await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 500, times: 1 });   // the first upload only
    try {
      await shortcut(page, 'paste');
      assert.ok(await page.waitFor(`document.querySelectorAll('#up-body .uitem.err').length === 1 && document.querySelectorAll('#up-body .uitem.done').length === 1`, { timeout: T }));
      await settle(page);
    } finally { await sim.control('/api/_sim/fault', { clear: true }); }
    const moved = ['m1.txt', 'm2.txt'].filter((n) => onSd('/data/pm/dst/' + n));
    assert.equal(moved.length, 1, 'exactly one copy landed');
    const kept = moved[0] === 'm1.txt' ? 'm2.txt' : 'm1.txt';
    assert.ok(!onSd('/data/pm/src/' + moved[0]), 'the landed one was moved (its source is gone)');
    assert.ok(onSd('/data/pm/src/' + kept), 'the failed one keeps its source');
    assert.equal(await toastText(page), plural(en.err_move_kept, 1));
  });

  await t.test('OS-clipboard cut → paste: a refused copy keeps the source', async () => {
    const page = await openFc(browser, sim, '/data/pf/dst');
    await sim.control('/api/_sim/fault', { route: '/api/fs/write', status: 500 });
    try {
      await page.eval(`window.postMessage({ type: 'clipboard-data', item: { kind: 'files', data: { op: 'cut', paths: ['/data/pf/src/e.txt'] } } }, '*'), true`);
      await settle(page);
    } finally { await sim.control('/api/_sim/fault', { clear: true }); }
    assert.ok(onSd('/data/pf/src/e.txt'), 'the source of a failed clipboard move was deleted');
    assert.equal(await sim.readSd('/data/pf/src/e.txt'), 'echo');
    assert.equal(await toastText(page), plural(en.err_move_kept, 1));
  });

  await t.test('new text file never truncates an existing one with the same name', async () => {
    const page = await openFc(browser, sim, '/data/nf');
    await page.eval(`document.getElementById('list').dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 40, clientY: 40 })), true`);
    await page.eval(`[...document.querySelectorAll('#ctx .ci')].find((d) => d.textContent.includes(${JSON.stringify(en.ctx_new_text)})).click(), true`);
    assert.ok(await page.waitFor(`document.getElementById('m-in') && !document.getElementById('scrim').classList.contains('hidden')`, { timeout: T }));
    await page.eval(`document.getElementById('m-in').value = 'untitled.txt', document.getElementById('m-ok').click(), true`);
    assert.ok(await page.waitFor(`!!${rowJs('untitled (1).txt')}`, { timeout: T }), 'the new file got a unique name');
    assert.equal(await sim.readSd('/data/nf/untitled.txt'), 'keep me', 'the existing untitled.txt was overwritten with an empty file');
    assert.equal(await sim.readSd('/data/nf/untitled (1).txt'), '');
  });

  for (const via of ['shortcut', 'menu']) {
    await t.test(`deleting a non-empty folder (${via}) says it was not deleted`, async () => {
      const dir = via === 'shortcut' ? '/data/fd' : '/data/fd2';
      const page = await openFc(browser, sim, dir);
      await fault(page, 'delete', dir + '/full', 404, { body: 'no entry' });   // the firmware's rmdir on a non-empty folder
      await select(page, 'full');
      if (via === 'shortcut') await shortcut(page, 'delete');
      else {
        await page.eval(`${rowJs('full')}.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, clientX: 40, clientY: 40 })), true`);
        await page.eval(`[...document.querySelectorAll('#ctx .ci')].find((d) => d.textContent.trim().endsWith(${JSON.stringify(en.delete)})).click(), true`);
      }
      assert.ok(await page.waitFor(`!document.getElementById('scrim').classList.contains('hidden') && document.getElementById('m-ok')`, { timeout: T }));
      await page.eval(`document.getElementById('m-ok').click(), true`);
      await settle(page);
      assert.ok(onSd(dir + '/full/f.txt'));
      assert.ok(await page.waitFor(`!!(document.getElementById('fc-toast') || {}).textContent`, { timeout: 10000 }), 'a silent no-op: the folder was not deleted and nobody said so');
      assert.equal(await toastText(page), (en.err_folder_not_empty || '').replace('{name}', 'full'));
    });
  }

  await t.test('drag-copy with a failed read never writes the error body into the destination', async () => {
    const page = await openFc(browser, sim, '/data/dc');
    await fault(page, 'read', '/data/dc/g.txt', 500);
    await page.eval(`(() => {
      const items = [{ path: '/data/dc/g.txt', name: 'g.txt', isDir: false }];
      const dt = { types: ['application/x-nucleo-sd'], getData: () => JSON.stringify({ items, srcDir: '/data/dc' }) };
      ${rowJs('into')}.ondrop({ dataTransfer: dt, preventDefault() {}, ctrlKey: true, altKey: false });
      return true;
    })()`);
    await settle(page);
    assert.ok(!onSd('/data/dc/into/g.txt'), 'the read error body was written as /data/dc/into/g.txt');
    assert.ok(await waitToast(page, plural(en.err_copy_failed, 1)), 'the failed copy is reported: ' + JSON.stringify(await toastText(page)));
  });

  await t.test('an unreadable Recycle Bin index blocks the delete instead of wiping the index', async () => {
    const page = await openFc(browser, sim, '/data/ti');
    await fault(page, 'read', '/system/config/trash.json', 503);
    await select(page, 'h.txt');
    await shortcut(page, 'delete');
    await settle(page);
    assert.ok(onSd('/data/ti/h.txt'), 'h.txt was deleted while the Recycle Bin index could not be read');
    assert.ok((await trashDb()).items.some((i) => i.trash === TRASH_ITEM.trash), 'the index of the other trashed files was wiped');
    assert.equal(await toastText(page), en.err_trash_index);
  });

  await t.test('a "{}" index (the firmware answer for a missing trash.json) is an empty bin, and the file is indexed', async () => {
    const page = await openFc(browser, sim, '/data/tz');
    const fs = await import('node:fs/promises');
    const before = await sim.readSd('/system/config/trash.json');
    await fs.writeFile(join(sim.sd, 'system/config/trash.json'), '{}');
    try {
      await select(page, 'i.txt');
      await shortcut(page, 'delete');
      assert.ok(await page.waitFor(`!${rowJs('i.txt')}`, { timeout: T }));
      await settle(page);
      const db = await trashDb();
      assert.ok(Array.isArray(db.items) && db.items.some((i) => i.orig === '/data/tz/i.txt'), 'the trashed file is missing from the index: ' + JSON.stringify(db));
    } finally { await fs.writeFile(join(sim.sd, 'system/config/trash.json'), before); }
  });

  // Review finding: a 0-byte or corrupt index used to block EVERY delete for good (no way out in the UI).
  await t.test('a 0-byte index is an empty bin: the delete works and the file is indexed', async () => {
    const page = await openFc(browser, sim, '/data/te');
    const fs = await import('node:fs/promises');
    const before = await sim.readSd('/system/config/trash.json');
    await fs.writeFile(join(sim.sd, 'system/config/trash.json'), '');
    try {
      await select(page, 'j.txt');
      await shortcut(page, 'delete');
      assert.ok(await page.waitFor(`!${rowJs('j.txt')}`, { timeout: T }), 'the delete went through');
      await settle(page);
      assert.ok((await trashDb()).items.some((i) => i.orig === '/data/te/j.txt'), 'the trashed file is indexed');
    } finally { await fs.writeFile(join(sim.sd, 'system/config/trash.json'), before); }
  });

  await t.test('a corrupt index is kept aside (never lost) and the bin starts a fresh one', async () => {
    const page = await openFc(browser, sim, '/data/tc');
    const fs = await import('node:fs/promises');
    const before = await sim.readSd('/system/config/trash.json');
    await fs.writeFile(join(sim.sd, 'system/config/trash.json'), '{"items":[{"orig":"/x"');
    try {
      await select(page, 'k.txt');
      await shortcut(page, 'delete');
      assert.ok(await page.waitFor(`!${rowJs('k.txt')}`, { timeout: T }), 'the delete went through');
      await settle(page);
      assert.ok((await trashDb()).items.some((i) => i.orig === '/data/tc/k.txt'), 'the trashed file is indexed');
      assert.equal(await sim.readSd('/system/config/trash.corrupt.json'), '{"items":[{"orig":"/x"', 'the unreadable index is kept, byte for byte');
    } finally { await fs.writeFile(join(sim.sd, 'system/config/trash.json'), before); }
  });
});

// The local → local paste (a PC folder via the File System Access API). Needs a secure context for OPFS,
// so it runs on the simulator's localhost origin.
test('file-commander: local cut → paste keeps the source when the write fails', { skip }, async (t) => {
  const sim = await startSim();
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false, origin: sim.local });
  await page.goto(`${sim.local}/apps/file-commander/`);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  assert.ok(await page.waitFor(`!!window._fc`, { timeout: T }));
  const ok = await page.eval(`(async () => {
    const root = await navigator.storage.getDirectory();
    for await (const [n] of root.entries()) await root.removeEntry(n, { recursive: true });
    const w = await (await root.getFileHandle('k.txt', { create: true })).createWritable(); await w.write('kilo'); await w.close();
    await root.getDirectoryHandle('sub', { create: true });
    window._fc.mountFsa(root, 'opfs', true);
    return true;
  })()`);
  assert.ok(ok);
  assert.ok(await page.waitFor(`window._fc.state().kind === 'local' && !!${rowJs('k.txt')}`, { timeout: T }));
  await select(page, 'k.txt');
  await shortcut(page, 'cut');
  await page.eval(`${rowJs('sub')}.ondblclick(), true`);
  assert.ok(await page.waitFor(`document.querySelectorAll('#list li .name').length === 0`, { timeout: T }), 'entered sub/');
  await page.eval(`FileSystemFileHandle.prototype.createWritable = () => Promise.reject(new Error('disk full')), true`);
  await shortcut(page, 'paste');
  await sleep(1500);
  const left = await page.eval(`(async () => { const root = await navigator.storage.getDirectory(); try { await root.getFileHandle('k.txt'); return true; } catch { return false; } })()`);
  assert.ok(left, 'the source k.txt was deleted although its copy was never written');
  assert.equal(await toastText(page), plural(en.err_move_kept, 1));
});

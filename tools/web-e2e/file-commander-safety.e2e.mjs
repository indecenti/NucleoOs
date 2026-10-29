// Browser E2E — File Commander must never lose a file when a step of a delete / move / copy / restore
// fails. Every case here was a real data-loss path: the app read or wrote without checking the
// response and then deleted the original anyway (or wrote an error body into a destination, truncated
// an existing file, or wiped the Recycle Bin index after a failed read).
//
// The File Commander is the modular one of the web OS (apps/file-commander/www/js/fm-*.js) on the core layers: the
// disk only through nucleo-fs.js (its fs.trash is THE way into the Recycle Bin), the OS services through
// nucleo-shell.js. Opened on its own, as here, its toasts go to the console and its questions are the browser's own
// confirm/prompt: the test records the first and answers the second. The OS clipboard is the shell's, so that one
// case runs in a window of the real shell.
//
// Faults: the simulator's /api/_sim/fault matches a whole route (every /api/fs/write), so a fault on
// ONE path is injected in the page instead: window.fetch is wrapped and answers a chosen status for an
// exact (op, path) pair — or every path under a folder (prefix) — exactly as the device would (e.g. 404
// "no entry" for a non-empty folder).
import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { launchBrowser, findChrome } from './cdp.mjs';
import { startSim } from './sim.mjs';
import { bootShell, closeAllWindows } from './shell.mjs';

const HOST_RULES = '--host-resolver-rules=MAP nucleo.test 127.0.0.1';
const skip = !findChrome() && 'no Chrome/Edge installed';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = 45000;
const J = JSON.stringify;
const rd = (p) => JSON.parse(readFileSync(new URL(p, import.meta.url), 'utf8'));
const en = { ...rd('../../web/shell/i18n/core.en.json'), ...rd('../../apps/file-commander/www/i18n.en.json') };
const plural = (v, n) => (v == null ? String(v) : typeof v === 'string' ? v : (n === 1 ? v.one : v.other)).replace('{count}', String(n));

const TRASH_ITEM = { orig: '/data/old.txt', trash: '/system/.trash/1_1_old.txt', name: 'old.txt', ts: 1 };
const SEED = {
  '/system/.trash/1_1_old.txt': 'old body',
  '/data/t1/a.txt': 'alpha',                       // trash: write of the Recycle Bin index fails
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

// Wrap window.fetch so /api/fs/<op>?path=<path> answers `status` (optionally `times` times; `prefix`: every path
// that starts with it, the core's temporary files included). move and copy have no ?path=: they match their source.
const FAULTS = `(() => {
  if (window.__ff) return true;
  window.__ff = [];
  const real = window.fetch.bind(window);
  window.fetch = (u, o) => {
    const s = String(u);
    const i = window.__ff.findIndex((r) => {
      const head = '/api/fs/' + r.op + (r.op === 'move' || r.op === 'copy' ? '?from=' : '?path=') + encodeURIComponent(r.path);
      return r.prefix ? s.startsWith(head) : (s === head || ((r.op === 'move' || r.op === 'copy') && s.startsWith(head + '&')));
    });
    if (i >= 0) {
      const r = window.__ff[i];
      if (r.times && --r.times === 0) window.__ff.splice(i, 1);
      return Promise.resolve(new Response(r.body || 'sim fault', { status: r.status }));
    }
    return real(u, o);
  };
  return true;
})()`;
// Outside the shell: toasts are console lines ("[toast]", text), questions are confirm()/prompt().
const STANDALONE = `(() => {
  if (window.__toasts) return true;
  window.__toasts = []; window.__asked = []; window.__answer = { confirm: true, prompt: null };
  const info = console.info.bind(console);
  console.info = (...a) => { if (a[0] === '[toast]') window.__toasts.push(String(a[1])); return info(...a); };
  window.confirm = (m) => { window.__asked.push(String(m)); return window.__answer.confirm; };
  window.prompt = (m, v) => { window.__asked.push(String(m)); return window.__answer.prompt != null ? window.__answer.prompt : v; };
  return true;
})()`;
const fault = (page, op, path, status, extra = {}) =>
  page.eval(`${FAULTS}, window.__ff.push(${J({ op, path, status, ...extra })}), true`);

async function openFc(browser, sim, path) {
  const page = await browser.newPage();
  await bootShell(page, sim, { lang: 'en', wait: false });
  await page.goto(`${sim.origin}/apps/file-commander/?path=${encodeURIComponent(path)}`);
  await page.eval(STANDALONE);
  await page.networkIdle({ quiet: 500, timeout: 20000 });
  assert.ok(await page.waitFor(`window._fc && document.querySelectorAll('#list .row .nm').length > 0`, { timeout: T }), 'File Commander listed ' + path);
  await page.eval(FAULTS);
  return page;
}
const rowJs = (name) => `[...document.querySelectorAll('#list .row')].find((r) => r.querySelector('.nm') && r.querySelector('.nm').textContent === ${J(name)})`;
const select = (page, name) => page.eval(`${rowJs(name)}.click(), true`);
const enter = (page, name) => page.eval(`${rowJs(name)}.dispatchEvent(new MouseEvent('dblclick', { bubbles: true })), true`);
const shortcut = (page, action) => page.eval(`window.postMessage({ type: 'os-shortcut', action: ${J(action)} }, location.origin), true`);
const toasts = (page) => page.eval(`window.__toasts.slice()`);
const waitToast = (page, text) => page.waitFor(`window.__toasts.includes(${J(text)})`, { timeout: T });
const settle = async (page) => { await page.networkIdle({ quiet: 800, timeout: T }); await sleep(300); };

test('file-commander: a failed step never costs the user a file', { skip }, async (t) => {
  const sim = await startSim({ seed: { ...SEED, '/system/config/trash.json': { items: [TRASH_ITEM] } } });
  const browser = await launchBrowser({ args: [HOST_RULES] });
  t.after(async () => { await browser.close(); await sim.stop(); });
  const onSd = (p) => existsSync(join(sim.sd, p.replace(/^\//, '')));
  const trashDb = async () => JSON.parse(await sim.readSd('/system/config/trash.json'));

  await t.test('trash: a refused write of the Recycle Bin index keeps the original', async () => {
    const page = await openFc(browser, sim, '/data/t1');
    await fault(page, 'write', '/system/config/', 500, { prefix: true });   // the index (and its temporary file)
    await select(page, 'a.txt');
    await shortcut(page, 'delete');
    await settle(page);
    assert.equal((await page.eval(`window.__asked`)).length, 1, 'the delete asked first');
    assert.ok(onSd('/data/t1/a.txt'), 'a.txt was deleted although the Recycle Bin never recorded it');
    assert.equal(await sim.readSd('/data/t1/a.txt'), 'alpha');
    assert.ok(await waitToast(page, en.err_trash_failed.replace('{name}', 'a.txt')), 'the user is told the delete did not happen: ' + J(await toasts(page)));
  });

  await t.test('trash: a failed read of the original keeps it (and never trashes an error body)', async () => {
    const page = await openFc(browser, sim, '/data/t2');
    // The device's one-step move into the bin is refused, its own copy is not there: the bytes go through the browser.
    await fault(page, 'move', '/data/t2/b.txt', 500);
    await fault(page, 'copy', '/data/t2/b.txt', 404);
    await fault(page, 'read', '/data/t2/b.txt', 500);
    await select(page, 'b.txt');
    await shortcut(page, 'delete');
    await settle(page);
    assert.ok(onSd('/data/t2/b.txt'), 'b.txt was deleted although it could not be read into the Recycle Bin');
    assert.equal(await sim.readSd('/data/t2/b.txt'), 'bravo');
    assert.ok(!(await trashDb()).items.some((i) => i.orig === '/data/t2/b.txt'), 'no Recycle Bin entry for a copy that was never made');
    assert.ok(await waitToast(page, en.err_trash_failed.replace('{name}', 'b.txt')), J(await toasts(page)));
  });

  await t.test('undo: a refused restore keeps the Recycle Bin copy and its index entry', async () => {
    const page = await openFc(browser, sim, '/data/t3');
    await select(page, 'c.txt');
    await shortcut(page, 'delete');
    assert.ok(await page.waitFor(`!${rowJs('c.txt')}`, { timeout: T }), 'c.txt went to the Recycle Bin');
    await settle(page);
    const entry = (await trashDb()).items.find((i) => i.orig === '/data/t3/c.txt');
    assert.ok(entry && onSd(entry.trash), 'the trash copy exists');
    await fault(page, 'move', entry.trash, 500);       // the restore renames first...
    await fault(page, 'copy', entry.trash, 404);       // ...then copies (the device's own copy is not there)...
    await fault(page, 'write', '/data/t3/', 500, { prefix: true });   // ...through the browser, and the write is refused
    await shortcut(page, 'undo');
    await settle(page);
    assert.ok(onSd(entry.trash), 'the trash copy was deleted although the restore write failed — the file is gone');
    assert.ok((await trashDb()).items.some((i) => i.trash === entry.trash), 'the Recycle Bin entry was dropped');
    assert.ok(await waitToast(page, en.err_restore_failed.replace('{name}', 'c.txt')), J(await toasts(page)));
  });

  await t.test('cut → paste: a failed upload keeps the source', async () => {
    const page = await openFc(browser, sim, '/data/px/src');
    await select(page, 'd.txt');
    await shortcut(page, 'cut');
    await page.eval(`document.getElementById('b-up').click(), true`);
    assert.ok(await page.waitFor(`!!${rowJs('dst')}`, { timeout: T }));
    await enter(page, 'dst');
    assert.ok(await page.waitFor(`window._fc.state().cwd === '/data/px/dst'`, { timeout: T }));
    await fault(page, 'write', '/data/px/dst/', 500, { prefix: true, times: 1 });   // only the upload
    await shortcut(page, 'paste');
    assert.ok(await page.waitFor(`document.querySelector('#up-body .uitem.err')`, { timeout: T }), 'the upload failed');
    await settle(page);
    assert.ok(!onSd('/data/px/dst/d.txt'), 'nothing landed in the destination');
    assert.ok(onSd('/data/px/src/d.txt'), 'the source of a failed move was deleted');
    assert.ok(await waitToast(page, plural(en.err_move_kept, 1)), J(await toasts(page)));
  });

  await t.test('cut → paste of two, one upload fails: only the landed one is moved', async () => {
    const page = await openFc(browser, sim, '/data/pm/src');
    await shortcut(page, 'selectAll');
    assert.ok(await page.waitFor(`document.querySelectorAll('#list .row.sel').length === 2`, { timeout: T }));
    await shortcut(page, 'cut');
    await page.eval(`document.getElementById('b-up').click(), true`);
    assert.ok(await page.waitFor(`!!${rowJs('dst')}`, { timeout: T }));
    await enter(page, 'dst');
    assert.ok(await page.waitFor(`window._fc.state().cwd === '/data/pm/dst'`, { timeout: T }));
    await fault(page, 'write', '/data/pm/dst/', 500, { prefix: true, times: 1 });   // the first upload only
    await shortcut(page, 'paste');
    assert.ok(await page.waitFor(`document.querySelectorAll('#up-body .uitem.err').length === 1 && document.querySelectorAll('#up-body .uitem.done').length === 1`, { timeout: T }));
    await settle(page);
    const moved = ['m1.txt', 'm2.txt'].filter((n) => onSd('/data/pm/dst/' + n));
    assert.equal(moved.length, 1, 'exactly one copy landed');
    const kept = moved[0] === 'm1.txt' ? 'm2.txt' : 'm1.txt';
    assert.ok(!onSd('/data/pm/src/' + moved[0]), 'the landed one was moved (its source is gone)');
    assert.ok(onSd('/data/pm/src/' + kept), 'the failed one keeps its source');
    assert.ok(await waitToast(page, plural(en.err_move_kept, 1)), J(await toasts(page)));
  });

  await t.test('OS-clipboard cut → paste: a refused copy keeps the source', async () => {
    // The OS clipboard is the shell's: File Commander runs in a window of the real shell, as for the user.
    const shell = await browser.newPage();
    await bootShell(shell, sim, { lang: 'en' });
    await closeAllWindows(shell).catch(() => {});
    await shell.eval(`(() => { window.__toasts = [];
      new MutationObserver(() => { for (const n of document.querySelectorAll('#toast-container .toast-title')) if (!n.__seen) { n.__seen = 1; window.__toasts.push(n.textContent); } })
        .observe(document.getElementById('toast-container'), { childList: true, subtree: true });
      window.postMessage({ type: 'clipboard-write', kind: 'files', data: { op: 'cut', paths: ['/data/pf/src/e.txt'] } }, location.origin);
      window.postMessage({ type: 'open-app', id: 'file-commander', query: 'path=' + encodeURIComponent('/data/pf/dst') }, location.origin);
      return true; })()`);
    const FRAME = `[...document.querySelectorAll('.win iframe')].find((f) => (f.getAttribute('src') || '').includes('/apps/file-commander/'))`;
    assert.ok(await shell.waitFor(`(() => { const f = ${FRAME}; const w = f && f.contentWindow;
      return !!(w && w._fc && w._fc.state().cwd === '/data/pf/dst' && w._fc.state().names.includes('.keep')); })()`, { timeout: T }), 'File Commander opened in the shell');
    const inFc = (expr) => shell.eval(`${FRAME}.contentWindow.eval(${J(expr)})`);
    await inFc(FAULTS);
    for (const [op, status] of [['move', 500], ['copy', 404], ['read', 500]]) await inFc(`window.__ff.push(${J({ op, path: '/data/pf/src/e.txt', status })}), true`);
    await inFc(`window.__ff.push(${J({ op: 'write', path: '/data/pf/dst/', status: 500, prefix: true })}), true`);
    // Ctrl+V the way the shell sends it: a message from the window that holds File Commander.
    await shell.eval(`${FRAME}.contentWindow.postMessage({ type: 'os-shortcut', action: 'paste' }, location.origin), true`);
    assert.ok(await shell.waitFor(`window.__toasts.includes(${J(plural(en.err_move_kept, 1))})`, { timeout: T }), 'the failed move is reported: ' + J(await shell.eval(`window.__toasts`)));
    await settle(shell);
    assert.ok(onSd('/data/pf/src/e.txt'), 'the source of a failed clipboard move was deleted');
    assert.equal(await sim.readSd('/data/pf/src/e.txt'), 'echo');
    assert.ok(!onSd('/data/pf/dst/e.txt'), 'nothing landed in the destination');
    await shell.send('Page.close').catch(() => {});      // a whole shell polling the device would slow the cases after it
  });

  await t.test('new text file never truncates an existing one with the same name', async () => {
    const page = await openFc(browser, sim, '/data/nf');
    await page.eval(`window.__answer.prompt = 'untitled.txt', true`);
    await page.eval(`document.getElementById('c-new').click(), true`);
    assert.ok(await page.waitFor(`!!document.querySelector('.nm-item')`, { timeout: T }), 'the New menu opened');
    await page.eval(`[...document.querySelectorAll('.nm-item')].find((d) => d.querySelector('.nm-lb').textContent === ${J(en.new_text)}).click(), true`);
    assert.ok(await page.waitFor(`!!${rowJs('untitled (2).txt')} || !!${rowJs('untitled (1).txt')}`, { timeout: T }), 'the new file got a unique name: ' + J(await page.eval(`window._fc.state().names`)));
    assert.equal(await sim.readSd('/data/nf/untitled.txt'), 'keep me', 'the existing untitled.txt was overwritten with an empty file');
    const made = (await page.eval(`window._fc.state().names`)).find((n) => /^untitled \(\d\)\.txt$/.test(n));
    assert.equal(await sim.readSd('/data/nf/' + made), '');
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
        assert.ok(await page.waitFor(`!!document.querySelector('.nm-item')`, { timeout: T }), 'the menu opened');
        await page.eval(`[...document.querySelectorAll('.nm-item')].find((d) => d.querySelector('.nm-lb').textContent.trim() === ${J(en.delete)}).click(), true`);
      }
      assert.ok(await page.waitFor(`window.__asked.length === 1`, { timeout: T }), 'the delete asked first');
      await settle(page);
      assert.ok(onSd(dir + '/full/f.txt'));
      assert.ok(await waitToast(page, (en.err_folder_not_empty || '').replace('{name}', 'full')), 'a silent no-op: the folder was not deleted and nobody said so: ' + J(await toasts(page)));
    });
  }

  await t.test('drag-copy with a failed read never writes the error body into the destination', async () => {
    const page = await openFc(browser, sim, '/data/dc');
    await fault(page, 'copy', '/data/dc/g.txt', 404);   // no device-side copy: the bytes go through the browser
    await fault(page, 'read', '/data/dc/g.txt', 500);
    await page.eval(`(() => {
      const items = [{ path: '/data/dc/g.txt', name: 'g.txt', isDir: false }];
      const dt = new DataTransfer(); dt.setData('application/x-nucleo-sd', JSON.stringify({ items, srcDir: '/data/dc' }));
      ${rowJs('into')}.dispatchEvent(new DragEvent('drop', { dataTransfer: dt, bubbles: true, cancelable: true, ctrlKey: true }));
      return true;
    })()`);
    await settle(page);
    assert.ok(!onSd('/data/dc/into/g.txt'), 'the read error body was written as /data/dc/into/g.txt');
    assert.ok(await waitToast(page, plural(en.err_copy_failed, 1)), 'the failed copy is reported: ' + J(await toasts(page)));
  });

  await t.test('an unreadable Recycle Bin index blocks the delete instead of wiping the index', async () => {
    const page = await openFc(browser, sim, '/data/ti');
    await fault(page, 'read', '/system/config/trash.json', 503);
    await select(page, 'h.txt');
    await shortcut(page, 'delete');
    await settle(page);
    assert.ok(onSd('/data/ti/h.txt'), 'h.txt was deleted while the Recycle Bin index could not be read');
    assert.ok((await trashDb()).items.some((i) => i.trash === TRASH_ITEM.trash), 'the index of the other trashed files was wiped');
    assert.ok(await waitToast(page, en.err_trash_failed.replace('{name}', 'h.txt')), 'the user is told nothing was deleted: ' + J(await toasts(page)));
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
      assert.ok(Array.isArray(db.items) && db.items.some((i) => i.orig === '/data/tz/i.txt'), 'the trashed file is missing from the index: ' + J(db));
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
  await page.eval(STANDALONE);
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
  await enter(page, 'sub');
  assert.ok(await page.waitFor(`document.querySelectorAll('#list .row .nm').length === 0`, { timeout: T }), 'entered sub/');
  await page.eval(`FileSystemFileHandle.prototype.createWritable = () => Promise.reject(new Error('disk full')), true`);
  await shortcut(page, 'paste');
  await sleep(1500);
  const left = await page.eval(`(async () => { const root = await navigator.storage.getDirectory(); try { await root.getFileHandle('k.txt'); return true; } catch { return false; } })()`);
  assert.ok(left, 'the source k.txt was deleted although its copy was never written');
  assert.ok(await waitToast(page, plural(en.err_move_kept, 1)), J(await toasts(page)));
});
